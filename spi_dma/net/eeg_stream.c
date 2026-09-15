#include "eeg_stream.h"
#include <string.h>
#include <assert.h>

_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "32-bit atomics must be lock-free");
_Static_assert((EEG_QUEUE_CAPACITY & (EEG_QUEUE_CAPACITY - 1u)) == 0, "power-of-two queue");

void eeg_queue_init(eeg_queue_t *q) {
    memset(q, 0, sizeof *q);
    atomic_init(&q->head, 0); atomic_init(&q->tail, 0);
    atomic_init(&q->drops, 0); atomic_init(&q->peak, 0);
    atomic_init(&q->stop_cursor, 0); atomic_init(&q->stop_serial, 0);
}
bool eeg_queue_push(eeg_queue_t *q, const eeg_sample_t *s) {
    uint32_t h = atomic_load_explicit(&q->head, memory_order_relaxed);
    uint32_t t = atomic_load_explicit(&q->tail, memory_order_acquire);
    uint32_t depth = h - t;
    if (depth >= EEG_QUEUE_CAPACITY) {
        /* Single writer, so avoid an unnecessary atomic read-modify-write. */
        atomic_store(&q->drops, atomic_load(&q->drops) + 1u);
        return false;
    }
    q->items[h & (EEG_QUEUE_CAPACITY - 1u)] = *s;
    atomic_store_explicit(&q->head, h + 1u, memory_order_release);
    if (depth + 1u > atomic_load(&q->peak)) atomic_store(&q->peak, depth + 1u);
    return true;
}
bool eeg_queue_peek(eeg_queue_t *q, eeg_sample_t *s) {
    uint32_t t = atomic_load_explicit(&q->tail, memory_order_relaxed);
    if (t == atomic_load_explicit(&q->head, memory_order_acquire)) return false;
    *s = q->items[t & (EEG_QUEUE_CAPACITY - 1u)];
    return true;
}
void eeg_queue_pop(eeg_queue_t *q) {
    uint32_t t = atomic_load_explicit(&q->tail, memory_order_relaxed);
    atomic_store_explicit(&q->tail, t + 1u, memory_order_release);
}
uint32_t eeg_queue_discard(eeg_queue_t *q) {
    uint32_t t = atomic_load_explicit(&q->tail, memory_order_relaxed);
    uint32_t h = atomic_load_explicit(&q->head, memory_order_acquire);
    atomic_store_explicit(&q->tail, h, memory_order_release);
    return h - t;
}
void eeg_queue_stop(eeg_queue_t *q) {
    atomic_store_explicit(&q->stop_cursor, atomic_load(&q->head), memory_order_release);
    atomic_store_explicit(&q->stop_serial, atomic_load(&q->stop_serial) + 1u, memory_order_release);
}
bool eeg_queue_stop_due(eeg_queue_t *q, uint32_t *seen) {
    uint32_t serial = atomic_load_explicit(&q->stop_serial, memory_order_acquire);
    if (serial == *seen) return false;
    uint32_t cursor = atomic_load_explicit(&q->stop_cursor, memory_order_acquire);
    uint32_t tail = atomic_load_explicit(&q->tail, memory_order_relaxed);
    if ((uint32_t)(tail - cursor) >= 0x80000000u) return false;
    *seen = serial;
    return true;
}

eeg_sample_t *eeg_queue_reserve(eeg_queue_t *q) {
    uint32_t h = atomic_load_explicit(&q->head, memory_order_relaxed);
    uint32_t depth = h - atomic_load_explicit(&q->tail, memory_order_acquire);
    if (depth == EEG_QUEUE_CAPACITY) {
        atomic_store_explicit(&q->drops, atomic_load_explicit(&q->drops, memory_order_relaxed) + 1u, memory_order_relaxed);
        return NULL;
    }
    if (depth + 1u > atomic_load_explicit(&q->peak, memory_order_relaxed))
        atomic_store_explicit(&q->peak, depth + 1u, memory_order_relaxed);
    return &q->items[h & (EEG_QUEUE_CAPACITY - 1u)];
}
void eeg_queue_commit(eeg_queue_t *q) {
    uint32_t h = atomic_load_explicit(&q->head, memory_order_relaxed);
    atomic_store_explicit(&q->head, h + 1u, memory_order_release);
}
unsigned eeg_queue_read_batch(eeg_queue_t *q, const eeg_sample_t **items,
                             unsigned maximum, uint32_t stop_seen) {
    uint32_t t = atomic_load_explicit(&q->tail, memory_order_relaxed);
    uint32_t h = atomic_load_explicit(&q->head, memory_order_acquire);
    uint32_t count = h - t;
    uint32_t contiguous = EEG_QUEUE_CAPACITY - (t & (EEG_QUEUE_CAPACITY - 1u));
    if (count > contiguous) count = contiguous;
    if (count > maximum) count = maximum;
    if (atomic_load_explicit(&q->stop_serial, memory_order_acquire) != stop_seen) {
        uint32_t distance = atomic_load_explicit(&q->stop_cursor, memory_order_acquire) - t;
        if (distance < count) count = distance;
    }
    *items = &q->items[t & (EEG_QUEUE_CAPACITY - 1u)];
    return count;
}
void eeg_queue_consume(eeg_queue_t *q, unsigned count) {
    uint32_t t = atomic_load_explicit(&q->tail, memory_order_relaxed);
    atomic_store_explicit(&q->tail, t + count, memory_order_release);
}

static void le16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void le32(uint8_t *p, uint32_t v) { for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8u*i)); }
static void le64(uint8_t *p, uint64_t v) { for (unsigned i = 0; i < 8; ++i) p[i] = (uint8_t)(v >> (8u*i)); }
void eeg_stream_init(eeg_stream_t *s) {
    memset(s, 0, sizeof *s);
    s->building = s->buffers[0]; s->pending = s->buffers[1];
}
static bool same_config(const eeg_sample_t *a, const eeg_sample_t *b) {
    return a->stream_id == b->stream_id && a->mclk_hz == b->mclk_hz &&
        a->nominal_rate == b->nominal_rate && a->gain == b->gain &&
        a->mode == b->mode && a->mclk_assumed == b->mclk_assumed;
}
bool eeg_stream_flush(eeg_stream_t *s) {
    if (!s->count) return true;
    if (s->ready) return false;
    uint16_t flags = s->count < EEG_SAMPLES_PER_PACKET ? 1u : 0u;
    if (!s->have_stream || s->stream_id != s->first.stream_id) {
        s->stream_id = s->first.stream_id;
        s->next_packet = 0;
        s->have_stream = true;
        s->have_previous = false;
        flags |= 2u;
    }
    if (s->have_previous && s->first.sequence != s->expected_sample) flags |= 4u;
    if (s->first.mclk_assumed) flags |= 8u;
    uint8_t *p = s->building;
    memcpy(p, "EEG1", 4); p[4] = 2; p[5] = 1;
    le16(p+6, flags); le16(p+8, EEG_PACKET_SIZE); le16(p+10, EEG_HEADER_SIZE);
    le16(p+12, s->count); le16(p+14, EEG_SAMPLE_SIZE);
    le32(p+16, s->next_packet++); le32(p+20, s->first.sequence);
    le64(p+24, s->first.timestamp_us); le32(p+32, s->first.mclk_hz);
    le16(p+36, s->first.nominal_rate); p[38] = s->first.gain; p[39] = s->first.mode;
    le32(p+40, s->first.stream_id);
    size_t used = EEG_HEADER_SIZE + s->count * EEG_SAMPLE_SIZE;
    memset(p + used, 0, EEG_TAIL_OFFSET - used);
    memcpy(p+1020, "\x0d\x0a\xa5\x5a", 4);
    s->building = s->pending;
    s->pending = p;
    s->pending_samples = s->count;
    s->expected_sample = s->first.sequence + s->count;
    s->have_previous = true;
    ++s->packets;
    if (s->count < EEG_SAMPLES_PER_PACKET) ++s->partial_packets;
    s->count = 0; s->offset = 0; s->ready = true;
    return true;
}
bool eeg_stream_offer(eeg_stream_t *s, const eeg_sample_t *v, uint64_t now) {
    if (s->count && (s->count == EEG_SAMPLES_PER_PACKET || !same_config(&s->first, v) ||
                    v->sequence != s->first.sequence + s->count)) {
        if (!eeg_stream_flush(s)) return false;
    }
    if (!s->count) { s->first = *v; s->began_us = now; }
    memcpy(s->building + EEG_HEADER_SIZE + s->count * EEG_SAMPLE_SIZE, v->raw, EEG_SAMPLE_SIZE);
    ++s->count;
    if (s->count == EEG_SAMPLES_PER_PACKET) (void)eeg_stream_flush(s);
    return true;
}
void eeg_stream_tick(eeg_stream_t *s, uint64_t now) {
    if (s->count && (s->count == EEG_SAMPLES_PER_PACKET || now - s->began_us >= EEG_FLUSH_US))
        (void)eeg_stream_flush(s);
}
void eeg_stream_disconnect(eeg_stream_t *s) {
    s->dropped_samples += s->count + (s->ready ? s->pending_samples : 0u);
    if (s->ready) ++s->dropped_packets;
    /* Account an unsent partial assembly as a discarded packet too. Consume
     * its packet number so a later connection can observe a packet gap. */
    if (s->count) {
        if (!s->have_stream || s->stream_id != s->first.stream_id) {
            s->stream_id = s->first.stream_id; s->next_packet = 0;
            s->have_stream = true; s->have_previous = false;
        }
        ++s->next_packet; ++s->dropped_packets;
    }
    s->count = 0; s->ready = false; s->offset = 0; s->pending_samples = 0;
}
int eeg_stream_pump(eeg_stream_t *s, eeg_write_fn fn, void *ctx) {
    if (!s->ready) return 0;
    size_t left = EEG_PACKET_SIZE - s->offset;
    int n = fn(ctx, s->pending + s->offset, left);
    if (n < 0) return -1;
    if ((size_t)n > left) return -1;
    s->offset += (uint16_t)n;
    if (s->offset == EEG_PACKET_SIZE) { s->ready = false; s->offset = 0; s->pending_samples = 0; }
    return n;
}
