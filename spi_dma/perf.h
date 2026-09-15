#ifndef EEG_PERF_H
#define EEG_PERF_H
#include <stdint.h>
/* Single-owner counters; totals/counts wrap modulo 2^32. Times are wall time,
 * including preemption, not exclusive CPU cycles. Never print in an IRQ. */
typedef struct { uint32_t count, total_us, max_us; } perf_counter_t;
static inline void perf_add(perf_counter_t *p, uint32_t us) {
    ++p->count; p->total_us += us;
    if (us > p->max_us) p->max_us = us;
}
#endif
