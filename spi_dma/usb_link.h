#ifndef EEG_USB_LINK_H
#define EEG_USB_LINK_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "ads1299.h"

#define USB_WIRE_HEADER 16u
#define USB_DATA_HEADER 40u
#define USB_BATCH_SAMPLES 36u
#define USB_MESSAGE_MAX 1024u
#define USB_TX_SLOTS 64u
#define USB_BATCH_FLUSH_US 4000u
#define USB_STATUS_WORDS 40u
enum { USB_DATA=1, USB_REPLY=2, USB_STATUS=3, USB_COMMAND=16 };
enum { USB_QUERY=0, USB_START=1, USB_STOP=2, USB_RATE=3, USB_GAIN=4, USB_MODE=5 };
enum { USB_OK=0, USB_BAD_COMMAND=1, USB_BAD_STATE=2, USB_ADC_ERROR=3 };
typedef struct { uint32_t id, op, value; } usb_request_t;
typedef struct { uint8_t bytes[24]; unsigned used; uint32_t malformed; } usb_parser_t;
typedef struct { uint8_t bytes[USB_MESSAGE_MAX]; uint16_t length; } usb_slot_t;
typedef struct {
    usb_slot_t slots[USB_TX_SLOTS];
    uint32_t head, tail, generation, packet_id, first_sequence, began_us;
    uint16_t count, offset;
    ads1299_settings_t settings;
    uint32_t sample_drops, peak, discarded_samples, malformed;
    uint64_t accepted_bytes;
} usb_link_t;
typedef unsigned (*usb_sink_fn)(void *context, const uint8_t *data, unsigned length);
uint32_t usb_u32(const uint8_t *p);
void usb_put32(uint8_t *p, uint32_t v);
bool usb_parse(usb_parser_t *p, uint8_t byte, usb_request_t *out);
void usb_link_init(usb_link_t *s);
/* Reset transport storage on disconnect; lifetime counters remain. */
void usb_link_disconnect(usb_link_t *s);
void usb_link_begin(usb_link_t *s, uint32_t generation, const ads1299_settings_t *settings);
bool usb_link_offer(usb_link_t *s, const ads1299_raw_frame_t *raw, uint32_t now);
void usb_link_flush(usb_link_t *s);
void usb_link_tick(usb_link_t *s, uint32_t now);
bool usb_link_reply_room(const usb_link_t *s);
bool usb_link_reply(usb_link_t *s, uint8_t type, uint32_t id, const uint32_t words[USB_STATUS_WORDS]);
unsigned usb_link_pump(usb_link_t *s, usb_sink_fn sink, void *context, unsigned budget);
#endif
