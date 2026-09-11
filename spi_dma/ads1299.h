#ifndef ADS1299_H
#define ADS1299_H
#include "ads1299_format.h"
#include "ads1299_registers.h"

typedef enum { ADS_MODE_TEST, ADS_MODE_SHORT, ADS_MODE_NORMAL } ads1299_mode_t;
typedef enum {
    ADS_OK, ADS_ERR_ARGUMENT, ADS_ERR_STATE, ADS_ERR_SPI_TIMEOUT,
    ADS_ERR_ID, ADS_ERR_READBACK, ADS_ERR_DMA_RESOURCE, ADS_ERR_SPI_BUDGET,
    ADS_ERR_DRDY_TIMEOUT, ADS_ERR_DMA_TIMEOUT, ADS_ERR_DMA_HW,
    ADS_ERR_FRAME, ADS_ERR_ABORT
} ads1299_error_t;
typedef struct {
    unsigned nominal_sps, gain;
    ads1299_mode_t mode;
    bool srb1, srb2, bias;
} ads1299_settings_t;
typedef struct {
    uint32_t drdy, frames, busy_drdy, queue_drops, discarded_on_stop;
    uint32_t drdy_timeouts, dma_timeouts, dma_errors, bad_frames;
    uint32_t spi_timeouts, recoveries, queue_peak;
} ads1299_stats_t;
typedef struct {
    uint8_t id, expected[ADS_REG_COUNT], readback[ADS_REG_COUNT];
    uint32_t checked_mask, mismatch_mask;
    uint32_t spi_hz, period_us;
    ads1299_error_t last_error;
    bool initialized, configured;
    volatile bool running; /* Read by DRDY/DMA IRQs; written by main only. */
    bool fatal;
    ads1299_settings_t settings;
} ads1299_info_t;

/* Single instance; all public APIs are core-0 main-loop only. No multicore use. */
typedef void (*ads1299_state_callback_t)(bool running, const ads1299_settings_t *settings);
void ads1299_set_state_callback(ads1299_state_callback_t callback);
void ads1299_power_on(void); /* Call before USB init. Inputs until rail settling. */
bool ads1299_init(const ads1299_settings_t *settings);
bool ads1299_reset(void); /* Leaves stopped and unconfigured. */
bool ads1299_configure(const ads1299_settings_t *settings); /* Leaves stopped. */
bool ads1299_preflight(const ads1299_settings_t *settings, uint32_t *spi_request);
bool ads1299_start(void);
bool ads1299_stop(void);
void ads1299_poll(void); /* Watchdog + max two automatic recovery attempts. */
bool ads1299_get_frame(ads1299_frame_t *frame);
void ads1299_get_stats(ads1299_stats_t *stats);
void ads1299_get_info(ads1299_info_t *info);
/* Control access requires stopped state. Write invalidates configured state;
 * call configure() before starting again. Reserved/read-only writes rejected. */
bool ads1299_read_regs(uint8_t address, uint8_t *values, size_t count);
bool ads1299_write_regs(uint8_t address, const uint8_t *values, size_t count);
bool ads1299_read_reg(uint8_t address, uint8_t *value);
bool ads1299_write_reg(uint8_t address, uint8_t value);
const char *ads1299_error_name(ads1299_error_t error);
const char *ads1299_mode_name(ads1299_mode_t mode);
#endif
