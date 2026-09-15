#include "wifi_stream.h"
#include "wifi_control.h"
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
#include "hardware/timer.h"
#include "hardware/sync.h"
#include "perf.h"

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
static uint64_t last_progress, last_heartbeat;
static bool stop_pending, output_pending;
static perf_counter_t network_time, service_time;
static uint32_t queue_age_max_us, inflight_max, ack_delay_max_us;
static uint32_t probe_end, probe_started;
static bool probe_active;
static int idle_alarm;
static void idle_alarm_callback(uint alarm_num) { (void)alarm_num; __sev(); }

typedef struct {
    _Atomic uint32_t network_avg_us, network_max_us, service_avg_us, service_max_us;
    _Atomic uint32_t queue_age_max_us, inflight_max, ack_delay_max_us;
    _Atomic uint32_t ap_state, attempt, connected, client_ip, error;
    _Atomic uint32_t no_client, disconnect_queue, packets, partial, discarded_packets, discarded_samples;
    _Atomic uint32_t connections, disconnects, rejected, enqueued, acked, backpressure, unacked_discard;
} diagnostics_t;
static diagnostics_t diag;
/* Approximate per-field snapshot; diagnostic counters wrap modulo 2^32. */
static void publish(void) {
    atomic_store(&diag.network_avg_us, network_time.count ? network_time.total_us/network_time.count : 0);
    atomic_store(&diag.network_max_us, network_time.max_us);
    atomic_store(&diag.service_avg_us, service_time.count ? service_time.total_us/service_time.count : 0);
    atomic_store(&diag.service_max_us, service_time.max_us);
    atomic_store(&diag.queue_age_max_us, queue_age_max_us);
    atomic_store(&diag.inflight_max, inflight_max);
    atomic_store(&diag.ack_delay_max_us, ack_delay_max_us);
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
    wifi_control_data_session(false, 0);
    ++disconnects;
    unacked_discard += conn_enqueued - conn_acked;
    eeg_stream_disconnect(&stream);
    disconnect_queue_drop += eeg_queue_discard(&samples);
    stop_pending = output_pending = probe_active = false;
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
        last_heartbeat = time_us_64();
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
    if (probe_active && (uint32_t)(conn_acked - probe_end) < 0x80000000u) {
        uint32_t delay = time_us_32() - probe_started;
        if (delay > ack_delay_max_us) ack_delay_max_us = delay;
        probe_active = false;
    }
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
    last_heartbeat = last_progress;
    wifi_control_data_session(true, lwip_ntohl(ip4_addr_get_u32(ip_2_ip4(&pcb->remote_ip))));
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
    output_pending = true;
    uint32_t inflight = conn_enqueued - conn_acked;
    if (inflight > inflight_max) inflight_max = inflight;
    if (!probe_active) {
        probe_end = conn_enqueued; probe_started = time_us_32(); probe_active = true;
    }
    last_progress = time_us_64();
    return (int)length;
}
/* Bounded work: at most 64 samples / 4 TCP writes / roughly 200 us per pass.
 * A single lwIP call can exceed the time budget; network poll runs next. */
static bool service_samples(void) {
    uint64_t started = time_us_64();
    if (client && started - last_heartbeat > 5000000ull) (void)close_client(true);
    if (!client) {
        no_client_drop += eeg_queue_discard(&samples);
        (void)eeg_queue_stop_due(&samples, &stop_seen);
        stop_pending = false;
        return false;
    }
    unsigned consumed = 0, writes = 0;
    bool progressed = false, blocked = false;
    while (consumed < 64u && time_us_64() - started < 200u) {
        if (stream.ready) {
            if (writes == 4u) break;
            ++writes;
            int n = eeg_stream_pump(&stream, write_copy, NULL);
            if (n < 0) { (void)close_client(true); return false; }
            if (n == 0) { blocked = true; break; }
            progressed = true;
        }
        if (eeg_queue_stop_due(&samples, &stop_seen)) stop_pending = true;
        if (stop_pending) {
            if (!eeg_stream_flush(&stream)) break;
            stop_pending = false;
            if (stream.ready) continue;
        }
        const eeg_sample_t *batch;
        unsigned count = eeg_queue_read_batch(&samples, &batch, 64u - consumed, stop_seen);
        if (!count) break;
        unsigned used = 0;
        for (; used < count; ++used) {
            uint64_t now = time_us_64();
            if (now - started >= 200u) break;
            if (!eeg_stream_offer(&stream, &batch[used], now)) break;
            uint64_t age = now - batch[used].timestamp_us;
            uint32_t age32 = age > UINT32_MAX ? UINT32_MAX : (uint32_t)age;
            if (age32 > queue_age_max_us) queue_age_max_us = age32;
            if (stream.ready) { ++used; break; }
        }
        eeg_queue_consume(&samples, used);
        consumed += used; progressed |= used != 0;
        if (!used) break;
    }
    eeg_stream_tick(&stream, time_us_64());
    /* Send newly completed/timeout-flushed packets in this same pass. */
    if (stream.ready && writes < 4u && !blocked) {
        int n = eeg_stream_pump(&stream, write_copy, NULL);
        if (n < 0) { (void)close_client(true); return false; }
        progressed |= n > 0; blocked = n == 0;
    }
    if (output_pending) {
        err_t err = tcp_output(client);
        if (err == ERR_OK) output_pending = false;
        else if (err != ERR_MEM && err != ERR_BUF) {
            atomic_store(&diag.error, (uint32_t)(int32_t)err);
            (void)close_client(true); return false;
        }
    }
    bool work = stream.ready || stream.count || conn_enqueued != conn_acked;
    if (!work) last_progress = time_us_64();
    else if (time_us_64() - last_progress > EEG_NO_PROGRESS_US) {
        atomic_store(&diag.error, 1004u);
        (void)close_client(true); return false;
    }
    return progressed && !blocked;
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
    if (!wifi_control_listen()) {
        tcp_accept(listener, NULL); tcp_close(listener); listener = NULL;
        goto failed;
    }
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
    idle_alarm = hardware_alarm_claim_unused(true);
    hardware_alarm_set_callback((uint)idle_alarm, idle_alarm_callback);
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
            uint32_t began = time_us_32();
            cyw43_arch_poll();
            perf_add(&network_time, time_us_32() - began);
            wifi_control_poll();
            if (!netif_is_up(&cyw43_state.netif[CYW43_ITF_AP])) {
                (void)close_client(true);
                wifi_control_shutdown();
                tcp_accept(listener, NULL); (void)tcp_close(listener); listener = NULL;
                dhcp_server_deinit(&dhcp);
                cyw43_arch_disable_ap_mode(); cyw43_arch_deinit();
                up = false; atomic_store(&diag.error, 1005u);
                atomic_store(&diag.ap_state, attempts == EEG_AP_RETRY_LIMIT ? 3u : 1u);
                retry_at = time_us_64() + EEG_AP_RETRY_US;
            }
        }
        uint32_t began = time_us_32();
        bool progressed = service_samples();
        perf_add(&service_time, time_us_32() - began);
        if (now >= next_report) { publish(); next_report = now + 100000u; }
        /* No delay while productive. A Core 1 alarm bounds idle/backpressure
         * WFE; producer SEV wakes an empty queue without using Core 0's pool. */
        if (!progressed) {
            if (!hardware_alarm_set_target((uint)idle_alarm, make_timeout_time_us(250))) __wfe();
            hardware_alarm_cancel((uint)idle_alarm);
        }
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
uint32_t wifi_stream_id(void) { return source.stream_id; }
void wifi_stream_submit(const ads1299_frame_t *frame) {
    if (!source_running) return;
    eeg_sample_t *slot = eeg_queue_reserve(&samples);
    if (!slot) return;
    slot->sequence = frame->sequence; slot->timestamp_us = frame->timestamp_us;
    slot->stream_id = source.stream_id; slot->mclk_hz = source.mclk_hz;
    slot->nominal_rate = source.nominal_rate; slot->gain = source.gain;
    slot->mode = source.mode; slot->mclk_assumed = source.mclk_assumed;
    memcpy(slot->raw, frame->raw, EEG_SAMPLE_SIZE);
    eeg_queue_commit(&samples);
    __sev();
}
void wifi_stream_report(void) {
    debug_log("perf core1 us net(avg/max)=%lu/%lu service=%lu/%lu qage_max=%lu ack_delay_max=%lu inflight_max=%lu\r\n",
        (unsigned long)atomic_load(&diag.network_avg_us), (unsigned long)atomic_load(&diag.network_max_us),
        (unsigned long)atomic_load(&diag.service_avg_us), (unsigned long)atomic_load(&diag.service_max_us),
        (unsigned long)atomic_load(&diag.queue_age_max_us), (unsigned long)atomic_load(&diag.ack_delay_max_us),
        (unsigned long)atomic_load(&diag.inflight_max));
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
