#include "wifi_control.h"
#include "control_wire.h"
#include "ads1299_control.h"
#include "ads1299_config.h"
#include "net/wifi_stream.h"
#include "pico/stdlib.h"
#include "lwip/tcp.h"
#include <stdatomic.h>

/* Single outstanding transaction. release/acquire transfers ownership of
 * request/reply storage. Disconnect never resets an executing transaction. */
static _Atomic unsigned mailbox; /* 0 idle, 1 requested, 2 complete */
static uint8_t request[CONTROL_REQUEST_SIZE], reply[CONTROL_REPLY_SIZE];
static struct tcp_pcb *listener, *peer;
static uint8_t input[CONTROL_REQUEST_SIZE];
static size_t used;
static bool submitted;
static uint64_t deadline;
static _Atomic uint32_t data_session, data_ip;
static uint32_t applied_session; /* Core 0 only. */
static uint32_t request_session; /* Captured by Core 1; mailbox ownership. */

void wifi_control_data_session(bool connected, uint32_t ip) {
    atomic_store(&data_ip, ip);
    uint32_t current=atomic_load(&data_session);
    /* Odd connected / even disconnected. Increment even on fast reconnect. */
    atomic_store(&data_session, (current + 2u) / 2u * 2u + (connected ? 1u : 0u));
}

static err_t close_peer(void) {
    if (!peer) return ERR_OK;
    struct tcp_pcb *p=peer; peer=NULL;
    tcp_recv(p,NULL); tcp_err(p,NULL); tcp_sent(p,NULL); tcp_poll(p,NULL,0);
    if (tcp_close(p)==ERR_OK) return ERR_OK;
    tcp_abort(p); return ERR_ABRT;
}
static void error_cb(void *arg, err_t error) {
    (void)arg; (void)error; peer=NULL;
}
static err_t receive_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    (void)arg;
    if (!p) return close_peer();
    size_t n=p->tot_len;
    bool valid=err==ERR_OK && !submitted && n<=sizeof input-used;
    if (valid) { pbuf_copy_partial(p,input+used,(u16_t)n,0); used+=n; }
    tcp_recved(pcb,p->tot_len); pbuf_free(p);
    if (!valid) return close_peer();
    if (used==sizeof input) {
        if (!control_valid(input)) return close_peer();
        request_session=atomic_load(&data_session);
        if (!(request_session & 1u) ||
            atomic_load(&data_ip)!=lwip_ntohl(ip4_addr_get_u32(ip_2_ip4(&pcb->remote_ip))))
            return close_peer();
        memcpy(request,input,sizeof request);
        submitted=true;
        atomic_store_explicit(&mailbox,1,memory_order_release);
    }
    return ERR_OK;
}
static err_t accept_cb(void *arg, struct tcp_pcb *pcb, err_t err) {
    (void)arg;
    if (!pcb || err!=ERR_OK) return ERR_VAL;
    if (peer || atomic_load_explicit(&mailbox,memory_order_acquire)!=0) {
        tcp_abort(pcb); return ERR_ABRT;
    }
    peer=pcb; used=0; submitted=false; deadline=time_us_64()+5000000ull;
    tcp_recv(peer,receive_cb); tcp_err(peer,error_cb); tcp_nagle_disable(peer);
    return ERR_OK;
}
bool wifi_control_listen(void) {
    struct tcp_pcb *p=tcp_new_ip_type(IPADDR_TYPE_V4);
    if (!p) return false;
    if (tcp_bind(p,IP_ADDR_ANY,CONTROL_PORT)!=ERR_OK) { tcp_abort(p); return false; }
    err_t err;
    listener=tcp_listen_with_backlog_and_err(p,1,&err);
    if (!listener) { tcp_abort(p); return false; }
    tcp_accept(listener,accept_cb); return true;
}
void wifi_control_shutdown(void) {
    (void)close_peer();
    if (listener) { tcp_accept(listener,NULL); tcp_close(listener); listener=NULL; }
}
void wifi_control_poll(void) {
    if (peer && time_us_64()>deadline) (void)close_peer();
    if (atomic_load_explicit(&mailbox,memory_order_acquire)!=2) return;
    if (peer) {
        err_t err=tcp_write(peer,reply,sizeof reply,TCP_WRITE_FLAG_COPY);
        if (err==ERR_MEM) return;
        if (err==ERR_OK) (void)tcp_output(peer);
        (void)close_peer();
    }
    atomic_store_explicit(&mailbox,0,memory_order_release);
}
void wifi_control_apply(ads1299_settings_t *settings) {
    uint32_t session=atomic_load(&data_session);
    if (session!=applied_session) {
        ads1299_info_t current; ads1299_get_info(&current);
        if (current.running) (void)ads1299_stop();
        applied_session=session;
    }
    if (atomic_load_explicit(&mailbox,memory_order_acquire)!=1) return;
    unsigned op=control_u32(request+8);
    ads1299_error_t error=ADS_OK;
    ads1299_change_result_t result=ADS_CHANGE_UNCHANGED;
    ads1299_info_t before; ads1299_get_info(&before);
    bool valid=session==request_session && (session & 1u) &&
               (!op || control_u32(request+16)==session);
    if (!valid || (op>=1 && op<=3 && before.running)) {
        result=ADS_CHANGE_FAILED; error=ADS_ERR_STATE;
    } else if (op>=1 && op<=3) {
        result=ads1299_change(settings,op==1?ADS_CHANGE_RATE:op==2?ADS_CHANGE_GAIN:ADS_CHANGE_MODE,
                             control_u32(request+12),&error);
    } else if (op==4 || op==5) {
        bool was_running=before.running;
        bool ok=op==4 ? ads1299_start() : (!before.running || ads1299_stop());
        ads1299_get_info(&before);
        result=!ok?ADS_CHANGE_FAILED:was_running==before.running?ADS_CHANGE_UNCHANGED:ADS_CHANGE_APPLIED;
        if (!ok) error=before.last_error;
    }
    /* Disconnect during a slow register transaction must defeat START. */
    if (session!=atomic_load(&data_session)) {
        ads1299_get_info(&before);
        if (before.running) (void)ads1299_stop();
        result=ADS_CHANGE_FAILED; error=ADS_ERR_STATE;
    }
    ads1299_info_t info; ads1299_get_info(&info);
    memcpy(reply,"WFR2",4); control_put(reply+4,control_u32(request+4));
    control_put(reply+8,result); control_put(reply+12,error);
    control_put(reply+16,info.settings.nominal_sps); control_put(reply+20,info.settings.gain);
    control_put(reply+24,info.running); control_put(reply+28,info.configured && !info.fatal);
    control_put(reply+32,ADS_MCLK_HZ); control_put(reply+36,info.settings.mode);
    control_put(reply+40,wifi_stream_id()); control_put(reply+44,session);
    atomic_store_explicit(&mailbox,2,memory_order_release);
}
