#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "ads1299_format.h"

int main(void) {
    /* Exhaust the 24-bit domain, including both rails and the sign boundary. */
    for (uint32_t v = 0; v <= 0xffffffu; ++v) {
        uint8_t b[3] = {(uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
        int64_t expected = (v < 0x800000u) ? (int64_t)v : (int64_t)v - 0x1000000;
        assert(ads1299_signed24(b) == expected);
    }
    ads1299_frame_t f = {.sequence = 17, .timestamp_us = 123456789};
    const uint32_t values[8] = {0, 1, 0x7fffff, 0x800000, 0xffffff, 0xfffffe, 0x123456, 0xfedcba};
    f.raw[0] = 0xca; f.raw[1] = 0xbc; f.raw[2] = 0xde;
    for (unsigned i = 0; i < 8; ++i) {
        f.raw[3 + i * 3] = (uint8_t)(values[i] >> 16);
        f.raw[4 + i * 3] = (uint8_t)(values[i] >> 8);
        f.raw[5 + i * 3] = (uint8_t)values[i];
    }
    uint8_t original[27]; memcpy(original, f.raw, sizeof original);
    assert(ads1299_decode(&f));
    assert(f.status == 0xcabcde && f.sequence == 17 && f.timestamp_us == 123456789);
    assert(!memcmp(original, f.raw, sizeof original));
    for (unsigned i = 0; i < 8; ++i) {
        int64_t expected = values[i] < 0x800000u ? (int64_t)values[i] : (int64_t)values[i] - 0x1000000;
        assert(f.channel[i] == expected);
    }
    for (unsigned header = 0; header < 16; ++header) {
        f.raw[0] = (uint8_t)(header << 4);
        assert(ads1299_decode(&f) == (header == 12));
    }
    assert(!ads1299_decode(NULL));
    const unsigned rates[7] = {16000, 8000, 4000, 2000, 1000, 500, 250};
    const unsigned gains[7] = {1, 2, 4, 6, 8, 12, 24};
    for (unsigned i = 0; i < 7; ++i) {
        uint8_t code = 255;
        assert(ads1299_rate_code(rates[i], &code) && code == i);
        assert(ads1299_gain_code(gains[i], &code) && code == i);
    }
    uint8_t untouched = 99;
    assert(!ads1299_rate_code(123, &untouched) && untouched == 99);
    assert(!ads1299_rate_code(250, NULL));
    assert(!ads1299_gain_code(3, &untouched) && untouched == 99);
    assert(!ads1299_gain_code(24, NULL));
    assert(ads1299_period_us(2048000, 6) == 4000);
    assert(ads1299_period_us(2048000, 0) == 63); /* 62.5 us rounded upward. */
    assert(ads1299_period_us(2000000, 6) == 4096);
    assert(ads1299_period_us(0, 6) == 0 && ads1299_period_us(2048000, 7) == 0);
    assert(ads1299_spi_budget_ok(2048000, 6, 1000000));
    assert(ads1299_spi_budget_ok(2048000, 3, 1000000));
    assert(!ads1299_spi_budget_ok(2048000, 2, 1000000));
    assert(!ads1299_spi_budget_ok(2048000, 0, 1000000));
    assert(ads1299_spi_budget_ok(2048000, 0, 8000000));
    assert(!ads1299_spi_budget_ok(0, 0, 8000000));
    assert(!ads1299_spi_budget_ok(2048000, 7, 8000000));
    assert(!ads1299_spi_budget_ok(2048000, 0, 0));
    puts("PASS: all 16777216 signed24 values, frame layout/status, rate/gain mapping, timing/bandwidth boundaries");
    return 0;
}
