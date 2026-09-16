#include <string.h>
#ifdef USB_APP_TEST
#include "tests/usb_app_fake.h"
#else
#include "pico/stdlib.h"
#include "tusb.h"
#endif
#include "ads1299.h"
#include "ads1299_config.h"
#include "usb_link.h"

static usb_link_t link_state;
static usb_parser_t parser;
static ads1299_settings_t settings = {
    .nominal_sps=ADS_DEFAULT_RATE, .gain=ADS_DEFAULT_GAIN, .mode=ADS_DEFAULT_MODE,
    .srb1=ADS_NORMAL_SRB1, .srb2=ADS_NORMAL_SRB2, .bias=ADS_NORMAL_BIAS
};
static bool running, lost_connection, connected;
static uint32_t generation, disconnects, stalls, stop_reason;
static uint32_t usb_task_max, main_max;
/* Callbacks execute in tud_task on Core 0, never issue blocking ADC commands. */
void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts) {
    (void)itf; (void)rts;
    if (!dtr) lost_connection=true;
}
void tud_umount_cb(void) { lost_connection=true; }
void tud_mount_cb(void) { if(connected) lost_connection=true; }
static void acquisition_state(bool active, const ads1299_settings_t *current) {
    running=active;
    if(active) usb_link_begin(&link_state,++generation,current);
    else usb_link_flush(&link_state);
}
static unsigned usb_write(void *context, const uint8_t *data, unsigned length) {
    (void)context;
    unsigned available=tud_cdc_write_available();
    if(length>available) length=available;
    return length ? tud_cdc_write(data,length) : 0;
}
static bool stop(unsigned reason) {
    bool ok=true;
    if(running) ok=ads1299_stop();
    else { ads1299_info_t info; ads1299_get_info(&info); ok=!info.fatal; }
    stop_reason=reason;
    return ok;
}
static void reply(const usb_request_t *request, unsigned result) {
    ads1299_info_t info; ads1299_stats_t stats; ads1299_perf_t perf;
    ads1299_get_info(&info); ads1299_get_stats(&stats); ads1299_get_perf(&perf);
    uint32_t words[USB_STATUS_WORDS] = {
        request->op,result,info.last_error,info.configured,info.running,
        info.settings.nominal_sps,info.settings.gain,info.settings.mode,ADS_MCLK_HZ,info.spi_hz,
        stats.drdy,stats.frames,stats.busy_drdy,stats.queue_drops,stats.bad_frames,
        stats.drdy_timeouts,stats.dma_timeouts,stats.dma_errors,stats.recoveries,stats.discarded_on_stop,
        stats.queue_peak,link_state.sample_drops,link_state.peak,
        (uint32_t)link_state.accepted_bytes,(uint32_t)(link_state.accepted_bytes>>32),
        disconnects,stalls,stop_reason,ENABLE_ACQ_PROFILE,perf.drdy_irq.max_us,
        perf.dma_irq.max_us,perf.poll_gap_max_us,perf.queue_age_max_us,stats.tainted_frames,
        usb_task_max,main_max,link_state.head-link_state.tail,link_state.discarded_samples,
        parser.malformed,info.fatal
    };
    (void)usb_link_reply(&link_state,request->op==USB_QUERY ? USB_STATUS : USB_REPLY,request->id,words);
}
static void execute(const usb_request_t *request) {
    unsigned result=USB_OK;
    if (request->op>USB_MODE || ((request->op<=USB_STOP) && request->value)) result=USB_BAD_COMMAND;
    else if(request->op==USB_START) {
        if(!running) {
            ads1299_info_t info; ads1299_get_info(&info);
            /* Explicit START retries a latched configuration fault; never automatic. */
            if(!info.configured && !ads1299_init(&settings)) result=USB_ADC_ERROR;
            else if(!ads1299_start()) result=USB_ADC_ERROR;
            else stop_reason=0;
        }
    } else if(request->op==USB_STOP) {
        if(!stop(1)) result=USB_ADC_ERROR;
    } else if(request->op>=USB_RATE) {
        if(running) result=USB_BAD_STATE;
        else {
            ads1299_settings_t requested=settings;
            if(request->op==USB_RATE) requested.nominal_sps=request->value;
            else if(request->op==USB_GAIN) requested.gain=request->value;
            else requested.mode=(ads1299_mode_t)request->value;
            uint32_t target;
            if(!ads1299_preflight(&requested,&target)) result=USB_ADC_ERROR;
            else if(!ads1299_configure(&requested)) result=USB_ADC_ERROR;
            else settings=requested;
        }
    }
    reply(request,result);
}
int main(void) {
    ads1299_power_on();
    usb_link_init(&link_state);
    ads1299_set_state_callback(acquisition_state);
    (void)ads1299_init(&settings);
    (void)tud_init(0); /* No stdio driver: this CDC carries binary messages only. */
    bool have_request=false;
    usb_request_t request={0};
    uint32_t last_progress=time_us_32();
    while(true) {
#if ENABLE_ACQ_PROFILE
        uint32_t loop_began=time_us_32();
#endif
        bool was_running=running;
        ads1299_poll();
        if(was_running && !running) stop_reason=4;
        /* Drain bounded acquisition work before servicing USB. */
        uint32_t now=time_us_32();
        for(unsigned i=0;i<64;i++) {
            const ads1299_raw_frame_t *raw=ads1299_peek_raw();
            if(!raw) break;
            if(connected) (void)usb_link_offer(&link_state,raw,now);
            ads1299_consume_raw();
        }
#if ENABLE_ACQ_PROFILE
        uint32_t usb_began=time_us_32();
#endif
        tud_task_ext(0,false); /* Handles queued events, no timeout wait. */
#if ENABLE_ACQ_PROFILE
        uint32_t usb_elapsed=time_us_32()-usb_began;
        if(usb_elapsed>usb_task_max) usb_task_max=usb_elapsed;
#endif
        bool open=tud_cdc_connected(); /* Requires DTR; disables TinyUSB FIFO overwrite. */
        if(lost_connection || (connected && !open)) {
            stop(2); ++disconnects;
            usb_link_disconnect(&link_state);
            tud_cdc_write_clear(); tud_cdc_read_flush();
            parser.used=0; have_request=false; connected=false; lost_connection=false;
        }
        if(open && !connected) { connected=true; last_progress=time_us_32(); }
        if(connected) {
            if(!have_request) {
                for(unsigned i=0;i<64 && tud_cdc_available();i++) {
                    uint8_t byte; if(tud_cdc_read(&byte,1)!=1) break;
                    if(usb_parse(&parser,byte,&request)) { have_request=true; break; }
                }
            }
            /* Reserve response storage before applying side effects. */
            if(have_request && usb_link_reply_room(&link_state)) {
                execute(&request); have_request=false;
            }
            now=time_us_32(); usb_link_tick(&link_state,now);
            unsigned written=usb_link_pump(&link_state,usb_write,NULL,4096);
            (void)tud_cdc_write_flush();
            bool pending=link_state.head!=link_state.tail || tud_cdc_write_available()<CFG_TUD_CDC_TX_BUFSIZE;
            if(written || !pending) last_progress=now;
            else if(running && now-last_progress>=2000000u) { stop(3); ++stalls; }
        }
#if ENABLE_ACQ_PROFILE
        uint32_t elapsed=time_us_32()-loop_began;
        if(elapsed>main_max) main_max=elapsed;
#endif
        tight_loop_contents();
    }
}
