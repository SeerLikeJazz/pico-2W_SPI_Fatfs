#include "wifi_stream.h"
#include "wifi_config.h"
#include "eeg_stream.h"
#include "dhcpserver.h"
#include "ads1299_config.h"
#include "debug_console.h"
#include <inttypes.h>
#include <string.h>
#include "pico/multicore.h"
#include "pico/rand.h"
#include "pico/cyw43_arch.h"
#include "lwip/tcp.h"

static eeg_queue_t samples;
static eeg_sample_t source; /* Core 0 only, changed by ADS lifecycle hook. */
static uint32_t stream_seed;
static bool source_running;
static uint32_t wifi_core1_stack[EEG_CORE1_STACK_BYTES / sizeof(uint32_t)] __attribute__((aligned(8)));
static eeg_stream_t stream; /* Everything below until diagnostics: Core 1 only. */
static struct tcp_pcb *listener, *client;
static dhcp_server_t dhcp;
static uint32_t stop_seen, no_client_drop, disconnect_queue_drop;
static uint32_t connections, disconnects, rejected, enqueued, acked, backpressure, unacked_discard;
static uint32_t conn_enqueued, conn_acked;
static uint64_t last_progress;
static bool stop_pending;

typedef struct {
    _Atomic uint32_t ap_state, attempt, connected, client_ip, error;
    _Atomic uint32_t no_client, disconnect_queue, packets, partial, discarded_packets, discarded_samples;
    _Atomic uint32_t connections, disconnects, rejected, enqueued, acked, backpressure, unacked_discard;
} diagnostics_t;
static diagnostics_t diag;
/* Approximate per-field snapshot; diagnostic counters wrap modulo 2^32. */
static void publish(void) {
    atomic_store(&diag.connected, client != NULL);
    atomic_store(&diag.no_client, no_client_drop);
    atomic_store(&diag.disconnect_queue, disconnect_queue_drop);
    atomic_store(&diag.packets, stream.packets); atomic_store(&diag.partial, stream.partial_packets);
    atomic_store(&diag.discarded_packets, stream.dropped_packets);
    atomic_store(&diag.discarded_samples, stream.dropped_samples);
    atomic_store(&diag.connections, connections); atomic_store(&diag.disconnects, disconnects);
    atomic_store(&diag.rejected, rejected); atomic_store(&diag.enqueued, enqueued);
    atomic_store(&diag.acked, acked); atomic_store(&diag.backpressure, backpressure);
    atomic_store(&diag.unacked_discard, unacked_discard);
}
static void forget_connection(void) {
    ++disconnects;
    unacked_discard += conn_enqueued - conn_acked;
    eeg_stream_disconnect(&stream);
    disconnect_queue_drop += eeg_queue_discard(&samples);
    stop_pending = false;
    client = NULL;
    atomic_store(&diag.client_ip, 0);
}
/* Detach callbacks BEFORE close/abort: an old connection must never mutate a
 * newer connection. close(ERR_MEM) uses bounded immediate abort fallback. */
static err_t close_client(bool abort_now) {
    if (!client) return ERR_OK;
    struct tcp_pcb *pcb = client;
    tcp_arg(pcb, NULL); tcp_recv(pcb, NULL); tcp_sent(pcb, NULL);
    tcp_err(pcb, NULL); tcp_poll(pcb, NULL, 0);
    forget_connection();
    if (!abort_now && tcp_close(pcb) == ERR_OK) return ERR_OK;
    tcp_abort(pcb);
    return ERR_ABRT;
}
static void on_error(void *arg, err_t err) {
    (void)arg; /* lwIP already freed this PCB. Never dereference/close it. */
    atomic_store(&diag.error, (uint32_t)(int32_t)err);
    forget_connection();
}
static err_t on_receive(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    (void)arg;
    if (p) {
        tcp_recved(pcb, p->tot_len);
        pbuf_free(p); /* No network control commands in V1. */
    }
    if (err != ERR_OK) return close_client(true);
    if (!p) return close_client(false); /* FIN */
    return ERR_OK;
}
static err_t on_sent(void *arg, struct tcp_pcb *pcb, u16_t length) {
    (void)arg; (void)pcb;
    acked += length; conn_acked += length;
    last_progress = time_us_64();
    return ERR_OK;
}
static err_t on_poll(void *arg, struct tcp_pcb *pcb) {
    (void)arg; (void)pcb; /* Main-loop service retries without waiting for ACK. */
    return ERR_OK;
}
static err_t on_accept(void *arg, struct tcp_pcb *pcb, err_t err) {
    (void)arg;
    if (err != ERR_OK || !pcb) return ERR_VAL;
    if (client) { ++rejected; tcp_abort(pcb); return ERR_ABRT; }
    /* Discard all pre-connection samples; only the consumer advances tail. */
    no_client_drop += eeg_queue_discard(&samples);
    eeg_stream_disconnect(&stream);
    stop_pending = false;
    client = pcb; conn_enqueued = conn_acked = 0;
    ++connections; last_progress = time_us_64();
    atomic_store(&diag.client_ip, lwip_ntohl(ip4_addr_get_u32(ip_2_ip4(&pcb->remote_ip))));
    tcp_nagle_disable(pcb);
    tcp_arg(pcb, NULL); tcp_recv(pcb, on_receive); tcp_err(pcb, on_error);
    tcp_sent(pcb, on_sent); tcp_poll(pcb, on_poll, 2);
    return ERR_OK;
}
static int write_copy(void *ctx, const uint8_t *data, size_t length) {
    (void)ctx;
    if (!client) return -1;
    u16_t space = tcp_sndbuf(client);
    if (length > space) length = space;
    if (!length) { ++backpressure; return 0; }
    err_t err = tcp_write(client, data, (u16_t)length, TCP_WRITE_FLAG_COPY);
    if (err == ERR_MEM) { ++backpressure; return 0; }
    if (err != ERR_OK) { atomic_store(&diag.error, (uint32_t)(int32_t)err); return -1; }
    enqueued += (uint32_t)length; conn_enqueued += (uint32_t)length;
    last_progress = time_us_64();
    return (int)length;
}
static void service_samples(void) {
    if (!client) {
        no_client_drop += eeg_queue_discard(&samples);
        (void)eeg_queue_stop_due(&samples, &stop_seen);
        stop_pending = false;
        return;
    }
    if (eeg_stream_pump(&stream, write_copy, NULL) < 0) { (void)close_client(true); return; }
    for (unsigned i = 0; i < 64u; ++i) {
        if (eeg_queue_stop_due(&samples, &stop_seen)) stop_pending = true;
        if (stop_pending) {
            if (!eeg_stream_flush(&stream)) break;
            stop_pending = false;
        }
        eeg_sample_t v;
        if (!eeg_queue_peek(&samples, &v)) break;
        if (!eeg_stream_offer(&stream, &v, time_us_64())) break;
        eeg_queue_pop(&samples);
    }
    eeg_stream_tick(&stream, time_us_64());
    /* tcp_output failure does NOT roll back the copy offset. */
    err_t err = tcp_output(client);
    if (err != ERR_OK && err != ERR_MEM && err != ERR_BUF) {
        atomic_store(&diag.error, (uint32_t)(int32_t)err);
        (void)close_client(true); return;
    }
    bool work = stream.ready || stream.count || conn_enqueued != conn_acked;
    if (!work) last_progress = time_us_64();
    else if (time_us_64() - last_progress > EEG_NO_PROGRESS_US) {
        atomic_store(&diag.error, 1004u);
        (void)close_client(true);
    }
}
static bool start_ap(void) {
    size_t password_len = strlen(EEG_AP_PASSWORD);
    if (password_len && (password_len < 8 || password_len > 63)) {
        atomic_store(&diag.error, 1001u); return false;
    }
    if (cyw43_arch_init()) { atomic_store(&diag.error, 1002u); return false; }
    cyw43_arch_enable_ap_mode(EEG_AP_SSID, password_len ? EEG_AP_PASSWORD : NULL,
                             password_len ? CYW43_AUTH_WPA2_AES_PSK : CYW43_AUTH_OPEN);
    /* AP radio link_status always reports DOWN in this driver: use netif
     * administration state, not STA association status, to avoid restart loops. */
    struct netif *ap = &cyw43_state.netif[CYW43_ITF_AP];
    ip_addr_t ip, mask;
    IP4_ADDR(&ip, 192,168,4,1); IP4_ADDR(&mask, 255,255,255,0);
    netif_set_addr(ap, ip_2_ip4(&ip), ip_2_ip4(&mask), ip_2_ip4(&ip));
    netif_set_default(ap);
    dhcp_server_init(&dhcp, ap, &ip, &mask);
    struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (!netif_is_up(ap) || !dhcp.udp || !pcb) {
        if (pcb) tcp_abort(pcb);
        goto failed;
    }
    if (tcp_bind(pcb, &ip, EEG_TCP_PORT) != ERR_OK) { tcp_abort(pcb); goto failed; }
    err_t err;
    listener = tcp_listen_with_backlog_and_err(pcb, 1, &err);
    if (!listener) { tcp_abort(pcb); goto failed; }
    tcp_accept(listener, on_accept);
    atomic_store(&diag.error, 0);
    return true;
failed:
    atomic_store(&diag.error, 1003u);
    dhcp_server_deinit(&dhcp);
    cyw43_arch_disable_ap_mode(); cyw43_arch_deinit();
    return false;
}
static void core1_main(void) {
    eeg_stream_init(&stream);
    bool up = false;
    uint32_t attempts = 0;
    uint64_t retry_at = 0, next_report = 0;
    while (true) {
        uint64_t now = time_us_64();
        if (!up && attempts < EEG_AP_RETRY_LIMIT && now >= retry_at) {
            atomic_store(&diag.ap_state, 1); atomic_store(&diag.attempt, ++attempts);
            up = start_ap();
            atomic_store(&diag.ap_state, up ? 2u : attempts == EEG_AP_RETRY_LIMIT ? 3u : 1u);
            retry_at = time_us_64() + EEG_AP_RETRY_US;
        }
        if (up) {
            cyw43_arch_poll();
            if (!netif_is_up(&cyw43_state.netif[CYW43_ITF_AP])) {
                (void)close_client(true);
                tcp_accept(listener, NULL); (void)tcp_close(listener); listener = NULL;
                dhcp_server_deinit(&dhcp);
                cyw43_arch_disable_ap_mode(); cyw43_arch_deinit();
                up = false; atomic_store(&diag.error, 1005u);
                atomic_store(&diag.ap_state, attempts == EEG_AP_RETRY_LIMIT ? 3u : 1u);
                retry_at = time_us_64() + EEG_AP_RETRY_US;
            }
        }
        service_samples();
        if (now >= next_report) { publish(); next_report = now + 100000u; }
        /* Core-local wait; does not use Core 0's default alarm pool. */
        busy_wait_us_32(250);
    }
}
static void acquisition_state(bool running, const ads1299_settings_t *settings) {
    if (!running) { source_running = false; eeg_queue_stop(&samples); return; }
    source.stream_id = ++stream_seed;
    source.mclk_hz = ADS_MCLK_HZ; source.nominal_rate = (uint16_t)settings->nominal_sps;
    source.gain = (uint8_t)settings->gain; source.mode = (uint8_t)settings->mode;
    source.mclk_assumed = EEG_MCLK_ASSUMED;
    source_running = true;
}
void wifi_stream_init(void) {
    eeg_queue_init(&samples);
    stream_seed = get_rand_32(); /* Random boot seed + per-start increment, not a globally unique ID. */
    ads1299_set_state_callback(acquisition_state);
}
void wifi_stream_launch(void) { multicore_launch_core1_with_stack(core1_main, wifi_core1_stack, sizeof wifi_core1_stack); }
void wifi_stream_submit(const ads1299_frame_t *frame) {
    if (!source_running) return;
    source.sequence = frame->sequence; source.timestamp_us = frame->timestamp_us;
    memcpy(source.raw, frame->raw, EEG_SAMPLE_SIZE);
    (void)eeg_queue_push(&samples, &source);
}
void wifi_stream_report(void) {
    uint32_t ip = atomic_load(&diag.client_ip);
    uint32_t tail = atomic_load(&samples.tail), head = atomic_load(&samples.head);
    uint32_t depth = head - tail; if (depth > EEG_QUEUE_CAPACITY) depth = EEG_QUEUE_CAPACITY;
    debug_log("WiFi SSID=%s AP=192.168.4.1:%u auth=%s state=%" PRIu32 " attempt=%" PRIu32
              " client=%" PRIu32 " ip=%u.%u.%u.%u error=%" PRId32 "\r\n", EEG_AP_SSID, EEG_TCP_PORT,
              strlen(EEG_AP_PASSWORD) ? "WPA2" : "OPEN-dev", atomic_load(&diag.ap_state),
              atomic_load(&diag.attempt), atomic_load(&diag.connected), (unsigned)(ip>>24),
              (unsigned)((ip>>16)&255), (unsigned)((ip>>8)&255), (unsigned)(ip&255), (int32_t)atomic_load(&diag.error));
    debug_log("net q=%" PRIu32 "/%u peak=%" PRIu32 " sample_drop=%" PRIu32 " no_client=%" PRIu32
              " packets=%" PRIu32 " partial=%" PRIu32 " disconnect_drop(q/sample/packet)=%" PRIu32 "/%" PRIu32 "/%" PRIu32 "\r\n",
              depth, EEG_QUEUE_CAPACITY, atomic_load(&samples.peak), atomic_load(&samples.drops),
              atomic_load(&diag.no_client), atomic_load(&diag.packets), atomic_load(&diag.partial),
              atomic_load(&diag.disconnect_queue), atomic_load(&diag.discarded_samples), atomic_load(&diag.discarded_packets));
    debug_log("tcp enqueued=%" PRIu32 " acked=%" PRIu32 " backpressure=%" PRIu32
              " connect/disconnect/reject=%" PRIu32 "/%" PRIu32 "/%" PRIu32 " unacked_on_disconnect=%" PRIu32
              " queue_bytes=%u core1_stack=%u\r\n", atomic_load(&diag.enqueued), atomic_load(&diag.acked),
              atomic_load(&diag.backpressure), atomic_load(&diag.connections), atomic_load(&diag.disconnects),
              atomic_load(&diag.rejected), atomic_load(&diag.unacked_discard), (unsigned)sizeof samples, EEG_CORE1_STACK_BYTES);
}
