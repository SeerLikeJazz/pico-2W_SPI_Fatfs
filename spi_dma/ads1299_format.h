#ifndef ADS1299_FORMAT_H
#define ADS1299_FORMAT_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define ADS_CHANNELS 8u
#define ADS_FRAME_BYTES 27u
typedef struct {
    uint8_t raw[ADS_FRAME_BYTES];
    uint32_t sequence; /* Observed DRDY count, not a hardware sample counter. */
    uint64_t timestamp_us; /* Time the GPIO ISR observed DRDY. */
    uint32_t status;
    int32_t channel[ADS_CHANNELS];
} ads1299_frame_t;
int32_t ads1299_signed24(const uint8_t bytes[3]);
bool ads1299_decode(ads1299_frame_t *frame);
bool ads1299_rate_code(unsigned nominal_sps, uint8_t *code);
bool ads1299_gain_code(unsigned gain, uint8_t *code);
uint32_t ads1299_period_us(uint32_t mclk_hz, uint8_t rate_code);
/* Conservative budget: 25% of period reserved for software and update window. */
bool ads1299_spi_budget_ok(uint32_t mclk_hz, uint8_t rate_code, uint32_t spi_hz);
#endif
