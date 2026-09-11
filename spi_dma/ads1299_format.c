#include "ads1299_format.h"

int32_t ads1299_signed24(const uint8_t bytes[3]) {
    uint32_t v = ((uint32_t)bytes[0] << 16) | ((uint32_t)bytes[1] << 8) | bytes[2];
    /* No signed shift or out-of-range unsigned-to-signed conversion. */
    return (v & 0x800000u) ? (int32_t)v - 16777216 : (int32_t)v;
}

bool ads1299_decode(ads1299_frame_t *frame) {
    if (!frame) return false;
    frame->status = ((uint32_t)frame->raw[0] << 16) |
                    ((uint32_t)frame->raw[1] << 8) | frame->raw[2];
    for (unsigned i = 0; i < ADS_CHANNELS; ++i)
        frame->channel[i] = ads1299_signed24(&frame->raw[3u + 3u * i]);
    return (frame->status & 0xf00000u) == 0xc00000u;
}

bool ads1299_rate_code(unsigned sps, uint8_t *code) {
    static const unsigned rates[] = {16000, 8000, 4000, 2000, 1000, 500, 250};
    if (!code) return false;
    for (uint8_t i = 0; i < sizeof rates / sizeof rates[0]; ++i) {
        if (sps == rates[i]) { *code = i; return true; }
    }
    return false;
}

bool ads1299_gain_code(unsigned gain, uint8_t *code) {
    static const unsigned gains[] = {1, 2, 4, 6, 8, 12, 24};
    if (!code) return false;
    for (uint8_t i = 0; i < sizeof gains / sizeof gains[0]; ++i) {
        if (gain == gains[i]) { *code = i; return true; }
    }
    return false;
}

uint32_t ads1299_period_us(uint32_t mclk, uint8_t code) {
    if (!mclk || code > 6) return 0;
    return (uint32_t)(((uint64_t)(128u << code) * 1000000u + mclk - 1u) / mclk);
}

bool ads1299_spi_budget_ok(uint32_t mclk, uint8_t code, uint32_t spi) {
    if (!mclk || !spi || code > 6) return false;
    return (uint64_t)ADS_FRAME_BYTES * 8u * mclk * 4u <
           (uint64_t)(128u << code) * spi * 3u;
}
