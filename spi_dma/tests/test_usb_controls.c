#include "usb_commands.h"
#include "ads1299_control.h"
#include "net/eeg_stream.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Hardware boundary doubles: production parser, configuration controller,
 * SPI planner and network encoder are linked unchanged. */
static ads1299_info_t device;
static unsigned configure_calls, start_calls, stops, generation;
static bool preflight_fail, configure_fail, start_fail;
void ads1299_get_info(ads1299_info_t *out) { *out = device; }
bool ads1299_preflight(const ads1299_settings_t *v, uint32_t *request) {
    uint8_t rate, gain;
    if (!ads1299_rate_code(v->nominal_sps, &rate) || !ads1299_gain_code(v->gain, &gain)) {
        device.last_error = ADS_ERR_ARGUMENT; return false;
    }
    if (preflight_fail || !ads1299_spi_plan(2048000, rate, 150000000, 1000000, 8000000, request)) {
        device.last_error = ADS_ERR_SPI_BUDGET; return false;
    }
    return true;
}
bool ads1299_configure(const ads1299_settings_t *v) {
    ++configure_calls;
    if (device.running) ++stops;
    device.running = false; device.configured = false;
    if (configure_fail) { device.last_error = ADS_ERR_READBACK; return false; }
    device.settings = *v; device.configured = true; return true;
}
bool ads1299_start(void) {
    ++start_calls;
    if (start_fail) { device.configured = false; device.last_error = ADS_ERR_SPI_TIMEOUT; return false; }
    device.running = true; ++generation; return true;
}
static void reset(bool running) {
    memset(&device, 0, sizeof device);
    device.initialized = device.configured = true; device.running = running;
    device.settings = (ads1299_settings_t){.nominal_sps=250, .gain=1, .mode=ADS_MODE_NORMAL,
                                          .srb1=true, .bias=true};
    configure_calls = start_calls = stops = generation = 0;
    preflight_fail = configure_fail = start_fail = false;
}
static usb_command_t line(usb_command_parser_t *p, const char *s) {
    usb_command_t event = {0};
    for (; *s; ++s) {
        usb_command_t next = usb_command_feed(p, (unsigned char)*s);
        if (next.kind != USB_CMD_NONE) { assert(event.kind == USB_CMD_NONE); event = next; }
    }
    return event;
}
static void parser_tests(void) {
    const unsigned rates[] = {250,500,1000,2000,4000,8000,16000};
    const unsigned gains[] = {1,2,4,6,8,12,24};
    usb_command_parser_t p = {0}; char text[64];
    for (unsigned i=0; i<7; ++i) {
        snprintf(text,sizeof text,":rate %u\r\n",rates[i]);
        usb_command_t v=line(&p,text); assert(v.kind==USB_CMD_RATE && v.value==rates[i]);
        snprintf(text,sizeof text,":gain %u\n",gains[i]);
        v=line(&p,text); assert(v.kind==USB_CMD_GAIN && v.value==gains[i]);
    }
    const char *invalid[]={":rate\n",":gain \n",":rate -250\n",":gain +1\n",":rate 1k\n",
        ":gain 25\n",":gain 3\n",":rate 249\n",":rate 16001\n",":rate 0\n",":gain 0\n",
        ":rate 250 extra\n",":gain 1 2\n",":rate 4294967296\n",":gain 9999999999999999\n",
        ":gain24\n",":RATE 250\n",":garbage\n"};
    for (unsigned i=0;i<sizeof invalid/sizeof *invalid;++i) assert(line(&p,invalid[i]).kind==USB_CMD_ERROR);
    assert(line(&p,":ra").kind==USB_CMD_NONE);
    assert(line(&p,"te 10").kind==USB_CMD_NONE);
    usb_command_t v=line(&p,"00\r"); assert(v.kind==USB_CMD_RATE && v.value==1000);
    assert(line(&p,":gain 21\b4\n").value==24);
    assert(line(&p,":gain 21\1774\n").value==24);
    assert(line(&p,":\b\177  rate\t500 \r\n").value==500);
    assert(line(&p,":\n\r\n").kind==USB_CMD_NONE);
    assert(usb_command_feed(&p,':').kind==USB_CMD_NONE);
    for (unsigned i=0;i<100;++i) assert(usb_command_feed(&p,'g').kind==USB_CMD_NONE);
    assert(line(&p,"r\b\n").kind==USB_CMD_ERROR); /* discard entire oversized line */
    assert(line(&p,":gain 8\n").value==8);
    assert(line(&p,":gain 1").kind==USB_CMD_NONE);
    assert(usb_command_feed(&p,0).kind==USB_CMD_NONE);
    assert(line(&p,"\n").kind==USB_CMD_ERROR);
    for (const char *s="?srpxgithnb";*s;++s) {
        v=usb_command_feed(&p,*s); assert(v.kind==USB_CMD_LEGACY && v.value==(unsigned)*s);
    }
}
static void spi_tests(void) {
    const uint32_t requests[]={1000000,1000000,1000000,1000000,2000000,4000000,8000000};
    const unsigned rates[]={250,500,1000,2000,4000,8000,16000};
    for (unsigned i=0;i<7;++i) {
        uint8_t code; uint32_t request;
        assert(ads1299_rate_code(rates[i],&code));
        assert(ads1299_spi_plan(2048000,code,150000000,1000000,8000000,&request));
        assert(request==requests[i]);
        uint32_t actual=ads1299_spi_actual(150000000,request);
        assert(actual<=request && ads1299_spi_budget_ok(2048000,code,actual));
    }
    uint32_t request;
    assert(!ads1299_spi_plan(2048000,0,150000000,1000000,1000000,&request));
    assert(!ads1299_spi_plan(2048000,0,1000000,1000000,8000000,&request));
    assert(!ads1299_spi_plan(0,0,150000000,1000000,8000000,&request));
    assert(!ads1299_spi_plan(1000000,0,150000000,1000000,8000000,&request));
    assert(!ads1299_spi_plan(2500000,0,150000000,1000000,8000000,&request));
    assert(!ads1299_spi_plan(2048000,7,150000000,1000000,8000000,&request));
    assert(!ads1299_spi_plan(2048000,0,150000000,0,8000000,&request));
    assert(!ads1299_spi_plan(2048000,0,150000000,1000000,20000001,&request));
    for (uint8_t code=0;code<7;code++) {
        assert(ads1299_spi_plan(2048000,code,150000000,15000000,15000000,&request));
        assert(request==15000000 && ads1299_spi_actual(150000000,request)==15000000);
    }
    assert(ads1299_spi_actual(150000000,8000000)==7500000);
    assert(ads1299_spi_actual(150000000,1)==0);
    assert(!ads1299_spi_budget_ok(2048000,0,4608000)); /* exactly 75% forbidden */
    assert(ads1299_spi_budget_ok(2048000,0,4608001));
}
static eeg_sample_t network_sample(uint32_t seq) {
    eeg_sample_t v={.sequence=seq, .stream_id=generation, .mclk_hz=2048000,
        .nominal_rate=(uint16_t)device.settings.nominal_sps, .gain=(uint8_t)device.settings.gain,
        .mode=(uint8_t)device.settings.mode};
    v.raw[0]=0xc0; return v;
}
static void control_tests(void) {
    ads1299_settings_t ram; ads1299_error_t error;
    const unsigned rates[]={250,500,1000,2000,4000,8000,16000}, gains[]={1,2,4,6,8,12,24};
    for (unsigned i=0;i<7;++i) for (unsigned j=0;j<7;++j) {
        reset(true); ram=device.settings;
        assert(ads1299_change(&ram,ADS_CHANGE_RATE,rates[i],&error)!=ADS_CHANGE_FAILED);
        assert(ads1299_change(&ram,ADS_CHANGE_GAIN,gains[j],&error)!=ADS_CHANGE_FAILED);
        assert(device.running && ram.nominal_sps==rates[i] && ram.gain==gains[j]);
        assert(ram.mode==ADS_MODE_NORMAL && ram.srb1 && ram.bias && !ram.srb2);
    }
    reset(true); ram=device.settings;
    assert(ads1299_change(&ram,ADS_CHANGE_RATE,250,&error)==ADS_CHANGE_UNCHANGED);
    assert(configure_calls==0 && start_calls==0 && stops==0 && generation==0);
    assert(ads1299_change(&ram,ADS_CHANGE_RATE,16000,&error)==ADS_CHANGE_APPLIED);
    assert(ram.nominal_sps==16000 && ram.gain==1 && ram.mode==ADS_MODE_NORMAL && ram.srb1 && ram.bias);
    assert(device.running && configure_calls==1 && start_calls==1 && generation==1);
    eeg_stream_t stream; eeg_stream_init(&stream);
    eeg_sample_t old=network_sample(10); assert(eeg_stream_offer(&stream,&old,0));
    assert(ads1299_change(&ram,ADS_CHANGE_GAIN,24,&error)==ADS_CHANGE_APPLIED);
    eeg_sample_t fresh=network_sample(11); assert(eeg_stream_offer(&stream,&fresh,1));
    assert(stream.ready && stream.pending[38]==1 && stream.pending[36]==0x80 && stream.pending[37]==0x3e);
    assert(stream.first.stream_id==2 && stream.first.gain==24 && stream.count==1);
    assert(ram.nominal_sps==16000 && ram.gain==24 && device.running);
    reset(false); ram=device.settings;
    assert(ads1299_change(&ram,ADS_CHANGE_GAIN,12,&error)==ADS_CHANGE_APPLIED);
    assert(!device.running && start_calls==0 && stops==0 && device.configured);
    assert(ads1299_change(&ram,ADS_CHANGE_GAIN,12,&error)==ADS_CHANGE_UNCHANGED);
    assert(!device.running && configure_calls==1 && start_calls==0);
    reset(true); ram=device.settings;
    assert(ads1299_change(&ram,ADS_CHANGE_GAIN,3,&error)==ADS_CHANGE_FAILED);
    assert(device.running && configure_calls==0 && ram.gain==1);
    preflight_fail=true;
    assert(ads1299_change(&ram,ADS_CHANGE_RATE,16000,&error)==ADS_CHANGE_FAILED);
    assert(error==ADS_ERR_SPI_BUDGET && device.running && configure_calls==0);
    reset(true); ram=device.settings; configure_fail=true;
    assert(ads1299_change(&ram,ADS_CHANGE_GAIN,24,&error)==ADS_CHANGE_FAILED);
    assert(!device.running && !device.configured && ram.gain==1 && start_calls==0 && error==ADS_ERR_READBACK);
    reset(true); ram=device.settings; start_fail=true;
    assert(ads1299_change(&ram,ADS_CHANGE_GAIN,24,&error)==ADS_CHANGE_FAILED);
    assert(!device.running && !device.configured && ram.gain==24 && error==ADS_ERR_SPI_TIMEOUT);
    assert(device.settings.gain==24 && generation==0);
    device.fatal=true;
    assert(ads1299_change(&ram,ADS_CHANGE_GAIN,1,&error)==ADS_CHANGE_FAILED && error==ADS_ERR_STATE);
}
int main(void) {
    parser_tests(); spi_tests(); control_tests();
    puts("PASS USB parser, SPI planning, configuration state and stream metadata");
    return 0;
}
