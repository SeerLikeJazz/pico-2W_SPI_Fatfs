#include "usb_link.h"
#include "ads1299_config.h"
#include <string.h>

_Static_assert((USB_TX_SLOTS & (USB_TX_SLOTS-1u)) == 0, "power of two queue");
static void put16(uint8_t *p, uint16_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
void usb_put32(uint8_t *p, uint32_t v) { for(unsigned i=0;i<4;i++) p[i]=(uint8_t)(v>>(8*i)); }
uint32_t usb_u32(const uint8_t *p) { return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static void header(uint8_t *p, uint8_t type, unsigned length, uint32_t id, uint32_t generation) {
    memcpy(p,"AUSB",4); p[4]=1; p[5]=type; put16(p+6,(uint16_t)length);
    usb_put32(p+8,id); usb_put32(p+12,generation);
}
bool usb_parse(usb_parser_t *p, uint8_t byte, usb_request_t *out) {
    p->bytes[p->used++]=byte;
    while (p->used >= 4) {
        if (!memcmp(p->bytes,"AUSB",4)) {
            if (p->used < 16) return false;
            if (p->bytes[4]==1 && p->bytes[5]==USB_COMMAND &&
                p->bytes[6]==24 && !p->bytes[7] && usb_u32(p->bytes+12)==0) {
                if (p->used < 24) return false;
                out->id=usb_u32(p->bytes+8); out->op=usb_u32(p->bytes+16);
                out->value=usb_u32(p->bytes+20); p->used=0; return true;
            }
            ++p->malformed;
        }
        memmove(p->bytes,p->bytes+1,--p->used);
    }
    return false;
}
static usb_slot_t *building(usb_link_t *s) { return &s->slots[s->head & (USB_TX_SLOTS-1u)]; }
static void commit(usb_link_t *s) {
    ++s->head;
    if (s->head-s->tail > s->peak) s->peak=s->head-s->tail;
}
void usb_link_init(usb_link_t *s) { memset(s,0,sizeof *s); }
void usb_link_disconnect(usb_link_t *s) {
    s->discarded_samples += s->count;
    for(uint32_t t=s->tail;t!=s->head;++t) {
        const uint8_t *p=s->slots[t & (USB_TX_SLOTS-1u)].bytes;
        if (p[5]==USB_DATA) s->discarded_samples += (uint32_t)p[36]|(uint32_t)p[37]<<8;
    }
    s->head=s->tail=0; s->count=s->offset=0;
}
void usb_link_begin(usb_link_t *s, uint32_t generation, const ads1299_settings_t *settings) {
    usb_link_flush(s); s->generation=generation; s->packet_id=0; s->settings=*settings;
}
void usb_link_flush(usb_link_t *s) {
    if (!s->count) return;
    usb_slot_t *slot=building(s);
    unsigned length=USB_DATA_HEADER+27u*s->count;
    header(slot->bytes,USB_DATA,length,s->packet_id++,s->generation);
    put16(slot->bytes+36,s->count); put16(slot->bytes+38,0);
    slot->length=(uint16_t)length; commit(s); s->count=0;
}
bool usb_link_offer(usb_link_t *s, const ads1299_raw_frame_t *raw, uint32_t now) {
    if (s->count && raw->sequence != s->first_sequence+s->count) usb_link_flush(s);
    /* Leave two slots for a partial data flush + an on-demand control reply. */
    if (!s->count && s->head-s->tail >= USB_TX_SLOTS-2u) { ++s->sample_drops; return false; }
    uint8_t *p=building(s)->bytes;
    if (!s->count) {
        s->first_sequence=raw->sequence; s->began_us=now;
        usb_put32(p+16,raw->sequence);
        usb_put32(p+20,(uint32_t)raw->timestamp_us); usb_put32(p+24,(uint32_t)(raw->timestamp_us>>32));
        usb_put32(p+28,ADS_MCLK_HZ);
        put16(p+32,(uint16_t)s->settings.nominal_sps); p[34]=(uint8_t)s->settings.gain; p[35]=(uint8_t)s->settings.mode;
    }
    memcpy(p+USB_DATA_HEADER+27u*s->count,raw->raw,27);
    if (++s->count==USB_BATCH_SAMPLES) usb_link_flush(s);
    return true;
}
void usb_link_tick(usb_link_t *s, uint32_t now) {
    if (s->count && now-s->began_us>=USB_BATCH_FLUSH_US) usb_link_flush(s);
}
bool usb_link_reply_room(const usb_link_t *s) { return s->head-s->tail+(s->count!=0)<USB_TX_SLOTS; }
bool usb_link_reply(usb_link_t *s, uint8_t type, uint32_t id, const uint32_t words[USB_STATUS_WORDS]) {
    if (!usb_link_reply_room(s)) return false;
    usb_link_flush(s);
    usb_slot_t *slot=building(s);
    slot->length=16+4*USB_STATUS_WORDS;
    header(slot->bytes,type,slot->length,id,s->generation);
    for(unsigned i=0;i<USB_STATUS_WORDS;i++) usb_put32(slot->bytes+16+4*i,words[i]);
    commit(s); return true;
}
unsigned usb_link_pump(usb_link_t *s, usb_sink_fn sink, void *context, unsigned budget) {
    unsigned total=0;
    while(s->tail!=s->head && total<budget) {
        usb_slot_t *slot=&s->slots[s->tail & (USB_TX_SLOTS-1u)];
        unsigned n=slot->length-s->offset;
        if(n>budget-total) n=budget-total;
        unsigned written=sink(context,slot->bytes+s->offset,n);
        if(written>n || !written) break;
        s->offset+=(uint16_t)written; total+=written; s->accepted_bytes+=written;
        if(s->offset==slot->length) { ++s->tail; s->offset=0; }
    }
    return total;
}
