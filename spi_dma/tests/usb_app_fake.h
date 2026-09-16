#ifndef USB_APP_FAKE_H
#define USB_APP_FAKE_H
#include <stdint.h>
#include <stdbool.h>
#define CFG_TUD_CDC_TX_BUFSIZE 4096
static inline uint32_t time_us_32(void) { static uint32_t t; return ++t; }
static inline void tight_loop_contents(void) { }
static inline bool tud_init(unsigned port) { (void)port; return true; }
static inline void tud_task_ext(unsigned timeout,bool isr) { (void)timeout;(void)isr; }
static inline bool tud_cdc_connected(void) { return true; }
static inline unsigned tud_cdc_available(void) { return 0; }
static inline unsigned tud_cdc_read(void *p,unsigned n) { (void)p;(void)n;return 0; }
static inline unsigned tud_cdc_write_available(void) { return 4096; }
static inline unsigned tud_cdc_write(const void *p,unsigned n) { (void)p;return n; }
static inline void tud_cdc_write_clear(void) { }
static inline void tud_cdc_read_flush(void) { }
static inline unsigned tud_cdc_write_flush(void) { return 0; }
#endif
