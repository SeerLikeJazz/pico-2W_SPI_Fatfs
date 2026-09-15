#ifndef EEG_STREAM_H
#define EEG_STREAM_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdatomic.h>

#define EEG_PACKET_SIZE 1024u
#define EEG_HEADER_SIZE 44u
#define EEG_SAMPLE_SIZE 27u
#define EEG_SAMPLES_PER_PACKET 36u
#define EEG_RESERVED_OFFSET 1016u
#define EEG_TAIL_OFFSET 1020u
#ifndef EEG_QUEUE_CAPACITY
#define EEG_QUEUE_CAPACITY 512u
#endif
#define EEG_FLUSH_US 200000u

typedef struct {
    uint64_t timestamp_us;
    uint32_t sequence, stream_id, mclk_hz;
    uint16_t nominal_rate;
    uint8_t gain, mode, mclk_assumed;
    uint8_t raw[EEG_SAMPLE_SIZE];
} eeg_sample_t;

typedef struct {
    eeg_sample_t items[EEG_QUEUE_CAPACITY];
    _Atomic uint32_t head, tail, drops, peak;
    /* Producer publishes the cursor AFTER the last pre-stop sample. Multiple
     * stops may coalesce: intervening generations are still visible in samples. */
    _Atomic uint32_t stop_cursor, stop_serial;
} eeg_queue_t;
void eeg_queue_init(eeg_queue_t *q);
bool eeg_queue_push(eeg_queue_t *q, const eeg_sample_t *s);
bool eeg_queue_peek(eeg_queue_t *q, eeg_sample_t *s);
void eeg_queue_pop(eeg_queue_t *q);
uint32_t eeg_queue_discard(eeg_queue_t *q); /* Consumer only; bounded snapshot. */
void eeg_queue_stop(eeg_queue_t *q); /* Producer only, cannot fail when full. */
bool eeg_queue_stop_due(eeg_queue_t *q, uint32_t *observed_serial);

/* Consumer borrows contiguous slots until consume; cap at an unseen STOP. */
unsigned eeg_queue_read_batch(eeg_queue_t *q, const eeg_sample_t **items,
                             unsigned maximum, uint32_t stop_seen);
void eeg_queue_consume(eeg_queue_t *q, unsigned count);
eeg_sample_t *eeg_queue_reserve(eeg_queue_t *q);
void eeg_queue_commit(eeg_queue_t *q);
typedef struct {
    uint8_t buffers[2][EEG_PACKET_SIZE];
    uint8_t *building, *pending; /* Non-copyable after init: pointers own buffers. */
    eeg_sample_t first;
    uint64_t began_us;
    uint32_t stream_id, next_packet, expected_sample;
    uint32_t packets, partial_packets, dropped_packets, dropped_samples;
    uint16_t count, pending_samples, offset;
    bool have_stream, have_previous, ready;
} eeg_stream_t;
void eeg_stream_init(eeg_stream_t *s);
/* False means backpressure: retain the offered sample and try again. */
bool eeg_stream_offer(eeg_stream_t *s, const eeg_sample_t *sample, uint64_t now);
bool eeg_stream_flush(eeg_stream_t *s);
void eeg_stream_tick(eeg_stream_t *s, uint64_t now);
void eeg_stream_disconnect(eeg_stream_t *s);
/* Sink returns bytes COPIED, 0 for temporary backpressure, -1 for fatal error.
 * One call per pump. Bytes accepted are never offered twice. */
typedef int (*eeg_write_fn)(void *ctx, const uint8_t *data, size_t length);
int eeg_stream_pump(eeg_stream_t *s, eeg_write_fn write_fn, void *ctx);
#endif
