#include "usb_link.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static usb_link_t link;
static uint8_t output[128*1024];
static unsigned offset, limit=100000;
static unsigned sink(void *ctx,const uint8_t *p,unsigned n) {
    (void)ctx;if(n>limit)n=limit;
    assert(offset+n<=sizeof output);memcpy(output+offset,p,n);offset+=n;return n;
}
static void drain(void) { while(link.head!=link.tail) assert(usb_link_pump(&link,sink,NULL,4096)); }
static ads1299_raw_frame_t frame(uint32_t seq) {
    ads1299_raw_frame_t f={.sequence=seq,.timestamp_us=123456789};f.raw[0]=0xc0;
    for(unsigned i=3;i<27;i++)f.raw[i]=(uint8_t)(seq+i);
    return f;
}
int main(int argc,char **argv) {
    usb_link_init(&link);
    ads1299_settings_t config={.nominal_sps=16000,.gain=1,.mode=ADS_MODE_TEST};
    usb_link_begin(&link,7,&config);
    for(unsigned i=0;i<36;i++){ads1299_raw_frame_t f=frame(i);assert(usb_link_offer(&link,&f,i));}
    assert(link.head==1 && link.count==0 && link.slots[0].length==1012);
    uint8_t expected[1012];memcpy(expected,link.slots[0].bytes,sizeof expected);
    limit=7;assert(usb_link_pump(&link,sink,NULL,100)==100 && link.offset==100);
    limit=0;assert(usb_link_pump(&link,sink,NULL,4096)==0 && link.offset==100);
    limit=10000;drain();assert(offset==1012 && !memcmp(expected,output,1012));
    uint32_t words[USB_STATUS_WORDS]={0};words[5]=16000;words[9]=15000000;
    assert(usb_link_reply(&link,USB_STATUS,42,words));drain();
    if(argc>1){FILE *f=fopen(argv[1],"wb");assert(f);assert(fwrite(output,1,offset,f)==offset);fclose(f);}
    usb_link_init(&link);usb_link_begin(&link,1,&config);offset=0;
    ads1299_raw_frame_t f=frame(UINT32_MAX);assert(usb_link_offer(&link,&f,0));
    f=frame(0);assert(usb_link_offer(&link,&f,1));assert(link.count==2);
    f=frame(2);assert(usb_link_offer(&link,&f,2));assert(link.head==1 && link.count==1);
    usb_link_tick(&link,4001);assert(link.count==1);usb_link_tick(&link,4002);assert(link.count==0);
    drain();assert(usb_u32(output+16)==UINT32_MAX && output[36]==2);
    usb_link_init(&link);usb_link_begin(&link,1,&config);offset=0;
    for(unsigned i=0;i<(USB_TX_SLOTS-2)*36;i++){f=frame(i);assert(usb_link_offer(&link,&f,i));}
    assert(link.head==62);uint8_t first[1012];memcpy(first,link.slots[0].bytes,1012);
    f=frame(9999);assert(!usb_link_offer(&link,&f,9999));assert(link.sample_drops==1);
    assert(!memcmp(first,link.slots[0].bytes,1012));
    assert(usb_link_reply(&link,USB_REPLY,1,words));assert(usb_link_reply(&link,USB_REPLY,2,words));
    assert(!usb_link_reply(&link,USB_REPLY,3,words));assert(link.peak==64);
    limit=13;assert(usb_link_pump(&link,sink,NULL,13)==13);
    usb_link_disconnect(&link);assert(!link.count && link.head==link.tail && link.offset==0);
    assert(link.discarded_samples==62*36 && link.accepted_bytes==13);
    usb_link_begin(&link,2,&config);f=frame(10001);assert(usb_link_offer(&link,&f,0));
    assert(usb_link_reply(&link,USB_REPLY,99,words));assert(link.head==2 && link.count==0);
    assert(usb_u32(link.slots[0].bytes+12)==2 && link.slots[1].bytes[5]==USB_REPLY);
    usb_link_disconnect(&link);link.head=link.tail=UINT32_MAX;
    f=frame(10);assert(usb_link_offer(&link,&f,0));usb_link_flush(&link);assert(link.head==0);
    limit=10000;offset=0;drain();assert(link.tail==0);
    usb_parser_t parser={0};usb_request_t req;
    uint8_t command[]={ 'A','U','S','B',1,16,24,0,42,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0 };
    for(unsigned i=0;i<sizeof command;i++)assert(usb_parse(&parser,command[i],&req)==(i==23));
    assert(req.id==42 && req.op==USB_START);
    command[6]=255;
    for(unsigned i=0;i<sizeof command;i++)assert(!usb_parse(&parser,command[i],&req));
    command[6]=24;
    for(unsigned i=0;i<sizeof command;i++)assert(usb_parse(&parser,command[i],&req)==(i==23));
    assert(parser.malformed>0);
    puts("PASS USB packet format, partial writes, backpressure, wrap, gap flush, STOP ordering, disconnect, parser");
}
