#ifndef BDF_WRITER_H
#define BDF_WRITER_H

#include <stdint.h>
#include "ff.h"

typedef struct {
    uint32_t target_bytes;
    uint32_t actual_bytes;
    uint32_t header_bytes;
    uint32_t record_bytes;
    uint32_t records;
    uint32_t data_write_calls;
    uint64_t elapsed_us;
} bdf_write_stats_t;

FRESULT bdf_write_demo_file(const char *path, UINT *bytes_written);
FRESULT bdf_write_speed_test_file(const char *path, bdf_write_stats_t *stats);

#endif