/* Execute production control mailbox/callbacks and mode register image on host.
 * lwIP transport, time, and ADC hardware calls are the only test doubles. */
#include <assert.h>
#include <stdio.h>
#include "../firmware/wifi_control.c"
#include "../firmware/ads_mode_registers.h"

static ads1299_info_t device;
static uint32_t generation;
static bool readback_fail, disconnect_during_start;
static unsigned changes, starts, stops;
static uint64_t now;
static uint8_t captured[48];
static struct tcp_pcb socket_stub={.remote_ip=123};
uint64_t time_us_64(void) { return now; }
uint32_t wifi_stream_id(void) { return generation; }
void ads1299_get_info(ads1299_info_t *out) { *out=device; }
bool ads1299_preflight(const ads1299_settings_t *s, uint32_t *spi) {
    uint8_t code;
    *spi=1000000;
    bool ok=s->mode<=ADS_MODE_IMPEDANCE && ads1299_rate_code(s->nominal_sps,&code) && ads1299_gain_code(s->gain,&code);
    if (!ok) device.last_error=ADS_ERR_ARGUMENT;
    return ok;
}
bool ads1299_configure(const ads1299_settings_t *s) {
    ++changes; device.running=false;
    if (readback_fail) { device.configured=false; device.last_error=ADS_ERR_READBACK; return false; }
    device.settings=*s; device.configured=true; return true;
}
bool ads1299_start(void) {
    ++starts;
    if (!device.configured) { device.last_error=ADS_ERR_STATE; return false; }
    if (!device.running) ++generation;
    device.running=true;
    if (disconnect_during_start) wifi_control_data_session(false,0);
    return true;
}
bool ads1299_stop(void) { ++stops; device.running=false; return true; }
struct tcp_pcb *tcp_new_ip_type(int type) { (void)type; return &socket_stub; }
struct tcp_pcb *tcp_listen_with_backlog_and_err(struct tcp_pcb *p,int n,err_t *e) { (void)n; *e=0; return p; }
err_t tcp_bind(struct tcp_pcb *p,int ip,int port) { (void)p;(void)ip;(void)port;return 0; }
err_t tcp_close(struct tcp_pcb *p) { (void)p;return 0; }
void tcp_abort(struct tcp_pcb *p) { (void)p; }
err_t tcp_write(struct tcp_pcb *p,const void *v,unsigned n,int flags) { (void)p;(void)flags;assert(n==48);memcpy(captured,v,n);return 0; }
err_t tcp_output(struct tcp_pcb *p) { (void)p;return 0; }
void tcp_recv(struct tcp_pcb *p,err_t (*cb)(void *,struct tcp_pcb *,struct pbuf *,err_t)) { (void)p;(void)cb; }
void tcp_err(struct tcp_pcb *p,void (*cb)(void *,err_t)) { (void)p;(void)cb; }
void tcp_sent(struct tcp_pcb *p,void *cb) { (void)p;(void)cb; }
void tcp_poll(struct tcp_pcb *p,void *cb,int period) { (void)p;(void)cb;(void)period; }
void tcp_accept(struct tcp_pcb *p,err_t (*cb)(void *,struct tcp_pcb *,err_t)) { (void)p;(void)cb; }
void tcp_nagle_disable(struct tcp_pcb *p) { (void)p; }
void tcp_recved(struct tcp_pcb *p,unsigned n) { (void)p;(void)n; }
void pbuf_free(struct pbuf *p) { (void)p; }

static void submit(unsigned op,unsigned value,uint32_t token) {
    uint8_t bytes[20];memcpy(bytes,"WFC2",4);
    control_put(bytes+4,99);control_put(bytes+8,op);control_put(bytes+12,value);control_put(bytes+16,token);
    assert(accept_cb(NULL,&socket_stub,0)==0);
    struct pbuf a={.tot_len=3,.payload=bytes},b={.tot_len=17,.payload=bytes+3};
    assert(receive_cb(NULL,peer,&a,0)==0);assert(atomic_load(&mailbox)==0);
    assert(receive_cb(NULL,peer,&b,0)==0);assert(atomic_load(&mailbox)==1);
}
static void execute(ads1299_settings_t *s) {
    wifi_control_apply(s);assert(atomic_load(&mailbox)==2);
    wifi_control_poll();assert(atomic_load(&mailbox)==0);
    assert(!memcmp(captured,"WFR2",4));assert(control_u32(captured+4)==99);
}
int main(void) {
    device.initialized=device.configured=true;
    device.settings=(ads1299_settings_t){.nominal_sps=250,.gain=24,.mode=ADS_MODE_TEST};
    ads1299_settings_t ram=device.settings;
    wifi_control_apply(&ram);assert(starts==0 && !device.running);
    wifi_control_data_session(true,123);
    uint32_t token=atomic_load(&data_session);
    submit(0,0,0);execute(&ram);assert(control_u32(captured+24)==0 && control_u32(captured+44)==token);
    for (unsigned mode=0;mode<4;++mode) {
        submit(3,mode,token);execute(&ram);
        assert(control_u32(captured+8)!=0 && !device.running && device.settings.mode==mode);
        uint8_t regs[ADS_REG_COUNT]={0}; ads_mode_registers(regs,&ram,6);
        assert((regs[ADS_REG_CH1SET]>>4)==6);
        assert((regs[ADS_REG_CH1SET]&7)==(mode==0?5:mode==1?1:0));
        assert(regs[ADS_REG_LOFF]==(mode==3?2:0));
        assert(regs[ADS_REG_LOFF_SENSP]==(mode==3?255:0));
        assert(regs[ADS_REG_LOFF_SENSN]==(mode==3?255:0));
        assert(regs[ADS_REG_MISC1]==0 && regs[ADS_REG_CONFIG4]==0);
    }
    submit(4,0,token);execute(&ram);assert(device.running && generation==1);
    unsigned old_changes=changes;
    for (unsigned op=1;op<=3;++op) {
        submit(op,op==1?1000:op==2?1:0,token);execute(&ram);
        assert(control_u32(captured+8)==0 && control_u32(captured+12)==ADS_ERR_STATE);
    }
    assert(changes==old_changes && device.running);
    submit(5,0,token);execute(&ram);assert(!device.running);
    submit(4,0,token);execute(&ram);assert(generation==2);
    wifi_control_data_session(false,0);wifi_control_data_session(true,123);
    wifi_control_apply(&ram);assert(!device.running);
    submit(4,0,token);execute(&ram);assert(!device.running && control_u32(captured+8)==0);
    token=atomic_load(&data_session);
    /* A lost control reply does not undo START. A subsequent query reconciles. */
    submit(4,0,token);(void)close_peer();wifi_control_apply(&ram);wifi_control_poll();
    assert(device.running && atomic_load(&mailbox)==0);
    submit(0,0,0);execute(&ram);assert(control_u32(captured+24)==1);
    submit(5,0,token);execute(&ram);
    submit(4,0,token);disconnect_during_start=true;execute(&ram);
    assert(!device.running && control_u32(captured+8)==0);
    disconnect_during_start=false;wifi_control_data_session(true,123);token=atomic_load(&data_session);
    submit(3,99,token);execute(&ram);assert(control_u32(captured+12)==ADS_ERR_ARGUMENT);
    readback_fail=true;submit(3,0,token);execute(&ram);
    assert(control_u32(captured+8)==0 && control_u32(captured+12)==ADS_ERR_READBACK);
    assert(!device.configured && !device.running);
    uint8_t legacy[20]={0};memcpy(legacy,"WFC1",4);assert(!control_valid(legacy));
    /* Real normal routing vs impedance: shared reference must not leak across modes. */
    uint8_t image[ADS_REG_COUNT]={0};
    ram.mode=ADS_MODE_NORMAL;ram.srb1=true;ram.bias=true;
    ads_mode_registers(image,&ram,6);
    assert(image[ADS_REG_MISC1]==0x20 && (image[ADS_REG_CONFIG3]&4));
    memset(image,0,sizeof image);ram.mode=ADS_MODE_IMPEDANCE;
    ads_mode_registers(image,&ram,6);
    assert(image[ADS_REG_MISC1]==0 && !(image[ADS_REG_CONFIG3]&4));
    assert(image[ADS_REG_LOFF_FLIP]==0 && image[ADS_REG_CONFIG2]==0xc0);
    puts("firmware mode/mailbox/session/failure tests passed");
}
