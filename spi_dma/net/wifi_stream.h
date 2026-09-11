#ifndef WIFI_STREAM_H
#define WIFI_STREAM_H
#include "ads1299.h"

#ifndef ENABLE_WIFI_STREAM
#define ENABLE_WIFI_STREAM 0
#endif
#if ENABLE_WIFI_STREAM
void wifi_stream_init(void); /* Core 0, before ADS init: queue and state hook. */
void wifi_stream_launch(void); /* Core 0, after ADS resources have been claimed. */
void wifi_stream_submit(const ads1299_frame_t *frame);
void wifi_stream_report(void); /* Core 0 only; logs atomic diagnostic snapshots. */
#else
static inline void wifi_stream_init(void) {}
static inline void wifi_stream_launch(void) {}
static inline void wifi_stream_submit(const ads1299_frame_t *frame) { (void)frame; }
static inline void wifi_stream_report(void) {}
#endif
#endif
