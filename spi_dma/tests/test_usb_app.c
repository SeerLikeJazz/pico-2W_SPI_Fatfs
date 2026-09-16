#define USB_APP_TEST 1
#define main firmware_main
#include "../spi_dma.c"
#undef main
#include <assert.h>
#include <stdio.h>
static ads1299_info_t device;
static ads1299_state_callback_t state_callback;
static unsigned start_calls, configure_calls;
static bool fail_init;
void ads1299_power_on(void) { }
void ads1299_set_state_callback(ads1299_state_callback_t cb) { state_callback=cb; }
bool ads1299_init(const ads1299_settings_t *s) {
    if(fail_init){device.last_error=ADS_ERR_ID;return false;}
    device.initialized=device.configured=true;device.settings=*s;return true;
}
bool ads1299_start(void) {
    if(!device.configured)return false;
    if(!device.running){++start_calls;device.running=true;state_callback(true,&device.settings);}
    return true;
}
bool ads1299_stop(void) {device.running=false;state_callback(false,&device.settings);return true;}
bool ads1299_preflight(const ads1299_settings_t *s,uint32_t *target) {
    uint8_t code;*target=15000000;
    return ads1299_rate_code(s->nominal_sps,&code) && ads1299_gain_code(s->gain,&code) && (unsigned)s->mode<=3;
}
bool ads1299_configure(const ads1299_settings_t *s) {++configure_calls;device.settings=*s;return true;}
void ads1299_get_info(ads1299_info_t *out) {*out=device;}
void ads1299_get_stats(ads1299_stats_t *out) {memset(out,0,sizeof *out);}
void ads1299_get_perf(ads1299_perf_t *out) {memset(out,0,sizeof *out);}
void ads1299_poll(void) { }
const ads1299_raw_frame_t *ads1299_peek_raw(void) {return NULL;}
void ads1299_consume_raw(void) { }
static uint32_t send(unsigned op,unsigned value) {
    usb_request_t r={.id=op+10,.op=op,.value=value};assert(usb_link_reply_room(&link_state));execute(&r);
    usb_slot_t *slot=&link_state.slots[(link_state.head-1)&(USB_TX_SLOTS-1)];
    assert(slot->bytes[5]==(op==USB_QUERY?USB_STATUS:USB_REPLY));
    return usb_u32(slot->bytes+20); /* result */
}
int main(void) {
    usb_link_init(&link_state);ads1299_set_state_callback(acquisition_state);assert(ads1299_init(&settings));
    assert(send(USB_QUERY,0)==USB_OK && !running);
    assert(send(USB_START,0)==USB_OK && running && generation==1);
    assert(send(USB_START,0)==USB_OK && start_calls==1 && generation==1);
    unsigned old_rate=device.settings.nominal_sps;
    assert(send(USB_RATE,16000)==USB_BAD_STATE && configure_calls==0 && device.settings.nominal_sps==old_rate);
    assert(send(USB_STOP,0)==USB_OK && !running && stop_reason==1);
    assert(send(USB_RATE,16000)==USB_OK && device.settings.nominal_sps==16000);
    assert(send(USB_START,0)==USB_OK && generation==2);
    assert(send(USB_START,1)==USB_BAD_COMMAND);assert(send(99,0)==USB_BAD_COMMAND);
    assert(send(USB_STOP,0)==USB_OK);device.configured=false;
    assert(send(USB_STOP,0)==USB_OK); /* Already stopped, even after a config fault. */
    fail_init=true;assert(send(USB_START,0)==USB_ADC_ERROR && !running);
    fail_init=false;assert(send(USB_START,0)==USB_OK && running && generation==3);
    connected=true;lost_connection=false;tud_mount_cb();assert(lost_connection);
    lost_connection=false;tud_cdc_line_state_cb(0,false,false);assert(lost_connection);
    puts("PASS production USB commands: query, idempotent START, STOP, running config rejection, explicit retry, reconnect latch");
}
