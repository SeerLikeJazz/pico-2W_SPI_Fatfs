#include "../Code/firmware/ads_mode_registers.h"
#include "ads1299.h"
#include "ads1299_config.h"

#include <string.h>
#include "pico/stdlib.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/spi.h"
#include "hardware/sync.h"
#include "hardware/clocks.h"
#include "perf.h"

_Static_assert(ADS_MCLK_HZ >= 100000u && ADS_MCLK_HZ <= 2500000u, "Check ADS external MCLK");
_Static_assert(ADS_SPI_HZ == 15000000u, "Fixed 15 MHz SPI requires DVDD=3.3V");
_Static_assert(ADS_SPI_MAX_HZ == ADS_SPI_HZ, "SPI request range");
_Static_assert(ADS_QUEUE_CAPACITY >= 2u, "Queue too small");
_Static_assert((ADS_QUEUE_CAPACITY & (ADS_QUEUE_CAPACITY - 1u)) == 0u, "Queue must be power of two for counter wrap");

typedef struct {
    uint8_t raw[ADS_FRAME_BYTES];
    uint32_t sequence;
    uint64_t timestamp_us;
} raw_frame_t;

static ads1299_info_t info;
static volatile ads1299_stats_t stats;
static raw_frame_t queue[ADS_QUEUE_CAPACITY], active;
static volatile uint32_t head, tail;
static ads1299_perf_t timing;
static uint32_t next_watchdog_us, previous_poll_us;
static volatile bool dma_active, rx_done, active_tainted;
static volatile ads1299_error_t pending_fault;
static volatile uint64_t last_drdy_us, dma_started_us;
static uint64_t power_enabled_us;
static bool powered;
static int rx_channel = -1, tx_channel = -1;
static uint32_t dma_mask, transfer_us;
static uint32_t spi_request_hz = ADS_SPI_HZ;
static unsigned bad_streak, recovery_attempts;
static const uint8_t nop = 0x00;
static ads1299_state_callback_t state_callback;
void ads1299_set_state_callback(ads1299_state_callback_t callback) { state_callback = callback; }
static bool dma_has_error(void);

static uint32_t clock_us(uint32_t cycles) {
    return (uint32_t)(((uint64_t)cycles * 1000000u + ADS_MCLK_HZ - 1u) / ADS_MCLK_HZ);
}

static bool fail(ads1299_error_t error) {
    info.last_error = error;
    return false;
}

static void cs_release(void) {
    /* 4 tCLK from last SCLK falling edge, then at least 2 tCLK high. */
    busy_wait_us_32(clock_us(4));
    gpio_put(ADS_PIN_CS, 1);
    busy_wait_us_32(clock_us(2));
}

static void spi_setup(void) {
    info.spi_hz = spi_init(spi0, spi_request_hz);
    spi_set_format(spi0, 8, SPI_CPOL_0, SPI_CPHA_1, SPI_MSB_FIRST);
    spi_get_hw(spi0)->icr = SPI_SSPICR_RORIC_BITS | SPI_SSPICR_RTIC_BITS;
}

static void spi_clean(void) {
    /* Called only after DMA quiescence. Peripheral reset empties both FIFOs. */
    spi_deinit(spi0);
    spi_setup();
}

static bool spi_idle_until(uint64_t deadline) {
    while (spi_is_busy(spi0)) {
        if (time_us_64() >= deadline) {
            ++stats.spi_timeouts;
            return fail(ADS_ERR_SPI_TIMEOUT);
        }
        tight_loop_contents();
    }
    return true;
}

static bool byte_xfer(uint8_t tx, uint8_t *rx) {
    uint64_t deadline = time_us_64() + ADS_CONTROL_TIMEOUT_US;
    while (!spi_is_writable(spi0)) {
        if (time_us_64() >= deadline) goto timeout;
    }
    spi_get_hw(spi0)->dr = tx;
    while (!spi_is_readable(spi0)) {
        if (time_us_64() >= deadline) goto timeout;
    }
    *rx = (uint8_t)spi_get_hw(spi0)->dr;
    if (!spi_idle_until(deadline)) return false;
    /* Explicit conservative inter-byte decode delay, including at high SCLK. */
    busy_wait_us_32(clock_us(4));
    return true;
timeout:
    ++stats.spi_timeouts;
    return fail(ADS_ERR_SPI_TIMEOUT);
}

static bool end_control(bool ok) {
    if (!ok) {
        spi_clean();
        info.configured = false;
    }
    cs_release();
    return ok;
}

static bool command(uint8_t cmd) {
    uint8_t discard;
    gpio_put(ADS_PIN_CS, 0);
    busy_wait_us_32(1); /* tCSSC >= 2 tCLK. MCLK assumption validated above. */
    busy_wait_us_32(clock_us(2));
    bool ok = byte_xfer(cmd, &discard);
    if (cmd == ADS_CMD_RESET) busy_wait_us_32(clock_us(18));
    return end_control(ok);
}

static bool range_ok(uint8_t address, size_t count) {
    return count && address < ADS_REG_COUNT && count <= (size_t)(ADS_REG_COUNT - address);
}

static bool read_internal(uint8_t address, uint8_t *values, size_t count) {
    uint8_t discard;
    gpio_put(ADS_PIN_CS, 0);
    busy_wait_us_32(clock_us(2));
    bool ok = byte_xfer((uint8_t)(ADS_CMD_RREG | address), &discard) &&
              byte_xfer((uint8_t)(count - 1u), &discard);
    for (size_t i = 0; ok && i < count; ++i) ok = byte_xfer(nop, &values[i]);
    return end_control(ok);
}

static bool write_internal(uint8_t address, const uint8_t *values, size_t count) {
    uint8_t discard;
    gpio_put(ADS_PIN_CS, 0);
    busy_wait_us_32(clock_us(2));
    bool ok = byte_xfer((uint8_t)(ADS_CMD_WREG | address), &discard) &&
              byte_xfer((uint8_t)(count - 1u), &discard);
    for (size_t i = 0; ok && i < count; ++i) ok = byte_xfer(values[i], &discard);
    return end_control(ok);
}

static bool writable_value(uint8_t address, uint8_t value) {
    switch (address) {
    case ADS_REG_CONFIG1: return (value & 0x98u) == 0x90u && (value & 7u) != 7u;
    case ADS_REG_CONFIG2: return (value & 0xe8u) == 0xc0u && (value & 3u) != 2u;
    case ADS_REG_CONFIG3: return (value & 0x61u) == 0x60u;
    case ADS_REG_LOFF: return !(value & 0x10u);
    case ADS_REG_MISC1: return !(value & ~0x20u);
    case ADS_REG_MISC2: return value == 0;
    case ADS_REG_CONFIG4: return !(value & ~0x0au);
    default:
        if (address >= ADS_REG_CH1SET && address <= ADS_REG_CH8SET)
            return ((value >> 4) & 7u) != 7u;
        return (address >= ADS_REG_BIAS_SENSP && address <= ADS_REG_LOFF_FLIP) ||
               address == ADS_REG_GPIO;
    }
}

bool ads1299_read_regs(uint8_t address, uint8_t *values, size_t count) {
    if (!values || !range_ok(address, count)) return fail(ADS_ERR_ARGUMENT);
    if (!info.initialized || info.running || info.fatal) return fail(ADS_ERR_STATE);
    return command(ADS_CMD_SDATAC) && read_internal(address, values, count);
}

bool ads1299_write_regs(uint8_t address, const uint8_t *values, size_t count) {
    if (!values || !range_ok(address, count)) return fail(ADS_ERR_ARGUMENT);
    for (size_t i = 0; i < count; ++i)
        if (!writable_value((uint8_t)(address + i), values[i])) return fail(ADS_ERR_ARGUMENT);
    if (!info.initialized || info.running || info.fatal) return fail(ADS_ERR_STATE);
    info.configured = false;
    return command(ADS_CMD_SDATAC) && write_internal(address, values, count);
}

bool ads1299_read_reg(uint8_t address, uint8_t *value) {
    return ads1299_read_regs(address, value, 1);
}

bool ads1299_write_reg(uint8_t address, uint8_t value) {
    return ads1299_write_regs(address, &value, 1);
}

/* Caller serializes with GPIO/DMA handlers. Never waits for hardware in IRQ. */
static void finish_frame(void) {
    if (!dma_active || (!rx_done && dma_channel_hw_addr(rx_channel)->transfer_count != 0) ||
        dma_channel_is_busy(rx_channel) ||
        dma_channel_is_busy(tx_channel) || spi_is_busy(spi0)) return;
    if (dma_has_error()) { pending_fault = ADS_ERR_DMA_HW; return; }
    if (!active_tainted) {
        if ((active.raw[0] & 0xf0u) != 0xc0u) {
            ++stats.bad_frames;
            if (++bad_streak >= 3u) pending_fault = ADS_ERR_FRAME;
        } else {
            bad_streak = 0;
            ++stats.frames;
            uint32_t used = head - tail;
            if (used == ADS_QUEUE_CAPACITY) ++stats.queue_drops;
            else {
                queue[head % ADS_QUEUE_CAPACITY] = active;
                __dmb();
                ++head;
                if (used + 1u > stats.queue_peak) stats.queue_peak = used + 1u;
            }
        }
    }
    dma_active = false;
    rx_done = false;
}

static bool dma_has_error(void) {
    return ((dma_channel_hw_addr(rx_channel)->ctrl_trig |
             dma_channel_hw_addr(tx_channel)->ctrl_trig) & DMA_CH0_CTRL_TRIG_AHB_ERROR_BITS) ||
           (spi_get_hw(spi0)->ris & SPI_SSPRIS_RORRIS_BITS);
}

static void dma_irq_work(void) {
    uint32_t flags = dma_hw->ints0 & dma_mask;
    if (!flags) return;
    dma_hw->ints0 = flags; /* W1C only our channels. Shared IRQ. */
    if (!info.running || !dma_active) return;
    if (dma_has_error()) {
        pending_fault = ADS_ERR_DMA_HW;
        return;
    }
    if (flags & (1u << rx_channel)) rx_done = true;
    finish_frame(); /* May defer SPI tail completion to poll/next DRDY. */
}

static void drdy_work(uint gpio, uint32_t events) {
    if (gpio != ADS_PIN_DRDY || !(events & GPIO_IRQ_EDGE_FALL) || !info.running) return;
    uint64_t now = time_us_64();
    last_drdy_us = now;
    ++stats.drdy;
    finish_frame();
    if (pending_fault != ADS_OK) return;
    if (dma_active) {
        ++stats.busy_drdy;
        active_tainted = true; /* A new conversion can overwrite the output shift data. */
        return;
    }
    if (spi_is_readable(spi0) || dma_has_error()) {
        pending_fault = ADS_ERR_DMA_HW;
        return;
    }
    /* A DRDY IRQ can observe completion before the pending DMA IRQ runs. */
    dma_hw->ints0 = dma_mask;
    active.timestamp_us = now;
    active.sequence = stats.drdy;
    dma_started_us = now;
    active_tainted = false;
    rx_done = false;
    dma_active = true;
    dma_channel_set_write_addr(rx_channel, active.raw, false);
    dma_channel_set_trans_count(rx_channel, ADS_FRAME_BYTES, false);
    dma_channel_set_read_addr(tx_channel, &nop, false);
    dma_channel_set_trans_count(tx_channel, ADS_FRAME_BYTES, false);
    /* RX is armed before the TX channel can create any clocks. CS stays low
     * throughout RDATAC, including SPI's final-bit tail and between frames. */
    dma_start_channel_mask(1u << rx_channel);
    dma_start_channel_mask(1u << tx_channel);
}

/* Each timing domain has a single owner. IRQs have identical priority. */
static void dma_irq_handler(void) {
    uint32_t began = time_us_32();
    dma_irq_work();
    perf_add(&timing.dma_irq, time_us_32() - began);
}
static void drdy_handler(uint gpio, uint32_t events) {
    uint32_t began = time_us_32();
    drdy_work(gpio, events);
    perf_add(&timing.drdy_irq, time_us_32() - began);
}

void ads1299_power_on(void) {
    if (powered) return;
    for (uint pin = ADS_PIN_DOUT; pin <= ADS_PIN_DRDY; ++pin) {
        gpio_init(pin);
        gpio_disable_pulls(pin);
        gpio_set_dir(pin, GPIO_IN);
    }
    gpio_init(ADS_PIN_5V_EN);
    gpio_put(ADS_PIN_5V_EN, 1);
    gpio_set_dir(ADS_PIN_5V_EN, GPIO_OUT);
    power_enabled_us = time_us_64();
    powered = true;
}

static bool dma_setup(void) {
    rx_channel = dma_claim_unused_channel(false);
    if (rx_channel < 0) return fail(ADS_ERR_DMA_RESOURCE);
    tx_channel = dma_claim_unused_channel(false);
    if (tx_channel < 0) {
        dma_channel_unclaim(rx_channel);
        rx_channel = -1;
        return fail(ADS_ERR_DMA_RESOURCE);
    }
    dma_mask = (1u << rx_channel) | (1u << tx_channel);
    dma_channel_config rx = dma_channel_get_default_config(rx_channel);
    channel_config_set_transfer_data_size(&rx, DMA_SIZE_8);
    channel_config_set_read_increment(&rx, false);
    channel_config_set_write_increment(&rx, true);
    channel_config_set_dreq(&rx, spi_get_dreq(spi0, false));
    dma_channel_configure(rx_channel, &rx, active.raw, &spi_get_hw(spi0)->dr, ADS_FRAME_BYTES, false);
    dma_channel_config tx = dma_channel_get_default_config(tx_channel);
    channel_config_set_transfer_data_size(&tx, DMA_SIZE_8);
    channel_config_set_read_increment(&tx, false);
    channel_config_set_write_increment(&tx, false);
    channel_config_set_dreq(&tx, spi_get_dreq(spi0, true));
    dma_channel_configure(tx_channel, &tx, &spi_get_hw(spi0)->dr, &nop, ADS_FRAME_BYTES, false);
    irq_add_shared_handler(DMA_IRQ_0, dma_irq_handler, PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);
    /* Same priority: handlers cannot preempt each other. Both outrank USB. */
    irq_set_priority(DMA_IRQ_0, 0x40);
    irq_set_priority(IO_IRQ_BANK0, 0x40);
    irq_set_enabled(DMA_IRQ_0, true);
    gpio_set_irq_enabled_with_callback(ADS_PIN_DRDY, GPIO_IRQ_EDGE_FALL, false, drdy_handler);
    /* SDK only enables NVIC in the callback helper when enabled=true. */
    irq_set_enabled(IO_IRQ_BANK0, true);
    return true;
}

/* Explicit bounded abort, unlike SDK dma_channel_abort()'s unbounded loop.
 * Clear EN on BOTH channels first: RP2350-E5. No channel chaining is used. */
static bool abort_dma(void) {
    hw_clear_bits(&dma_channel_hw_addr(rx_channel)->al1_ctrl, DMA_CH0_CTRL_TRIG_EN_BITS);
    hw_clear_bits(&dma_channel_hw_addr(tx_channel)->al1_ctrl, DMA_CH0_CTRL_TRIG_EN_BITS);
    dma_hw->abort = dma_mask;
    uint64_t deadline = time_us_64() + ADS_CONTROL_TIMEOUT_US;
    while (dma_channel_is_busy(rx_channel) || dma_channel_is_busy(tx_channel) ||
           (dma_hw->abort & dma_mask)) {
        if (time_us_64() >= deadline) {
            info.fatal = true; /* Never reuse a buffer an unquiesced DMA may still write. */
            info.configured = false;
            hw_clear_bits(&spi_get_hw(spi0)->dmacr,
                          SPI_SSPDMACR_TXDMAE_BITS | SPI_SSPDMACR_RXDMAE_BITS);
            return fail(ADS_ERR_ABORT);
        }
    }
    dma_hw->ints0 = dma_mask;
    hw_set_bits(&dma_channel_hw_addr(rx_channel)->al1_ctrl,
                DMA_CH0_CTRL_TRIG_READ_ERROR_BITS | DMA_CH0_CTRL_TRIG_WRITE_ERROR_BITS);
    hw_set_bits(&dma_channel_hw_addr(tx_channel)->al1_ctrl,
                DMA_CH0_CTRL_TRIG_READ_ERROR_BITS | DMA_CH0_CTRL_TRIG_WRITE_ERROR_BITS);
    /* AL1_CTRL is non-triggering. Writing CTRL_TRIG with EN=1 would start
     * the old transfer count immediately and collide with control commands. */
    hw_set_bits(&dma_channel_hw_addr(rx_channel)->al1_ctrl, DMA_CH0_CTRL_TRIG_EN_BITS);
    hw_set_bits(&dma_channel_hw_addr(tx_channel)->al1_ctrl, DMA_CH0_CTRL_TRIG_EN_BITS);
    return true;
}

bool ads1299_stop(void) {
    if (!info.initialized || info.fatal) return fail(ADS_ERR_STATE);
    bool was_running = info.running;
    uint32_t saved = save_and_disable_interrupts();
    gpio_set_irq_enabled(ADS_PIN_DRDY, GPIO_IRQ_EDGE_FALL, false);
    dma_set_irq0_channel_mask_enabled(dma_mask, false);
    info.running = false;
    restore_interrupts(saved);
    if (was_running && state_callback) state_callback(false, &info.settings);
    uint64_t deadline = time_us_64() + transfer_us + ADS_CONTROL_TIMEOUT_US;
    bool graceful = true;
    while (dma_channel_is_busy(rx_channel) || dma_channel_is_busy(tx_channel) || spi_is_busy(spi0)) {
        if (time_us_64() >= deadline || dma_has_error()) { graceful = false; break; }
    }
    if (!graceful && pending_fault == ADS_OK) {
        if (dma_has_error()) { ++stats.dma_errors; fail(ADS_ERR_DMA_HW); }
        else { ++stats.dma_timeouts; fail(ADS_ERR_DMA_TIMEOUT); }
    }
    if (!abort_dma()) return false;
    if (!graceful) spi_clean();
    cs_release();
    stats.discarded_on_stop += (head - tail) + (dma_active ? 1u : 0u);
    head = tail = 0;
    dma_active = rx_done = false;
    pending_fault = ADS_OK;
    bad_streak = 0;
    gpio_acknowledge_irq(ADS_PIN_DRDY, GPIO_IRQ_EDGE_FALL);
    spi_clean();
    /* Clocking commands only after DMA and SPI are fully quiescent. */
    bool ok = command(ADS_CMD_STOP) && command(ADS_CMD_SDATAC);
    if (!graceful || !ok) info.configured = false;
    return graceful && ok;
}

bool ads1299_reset(void) {
    if (!info.initialized || info.fatal) return fail(ADS_ERR_STATE);
    if (info.running || dma_active) {
        (void)ads1299_stop();
        if (info.fatal) return false;
    }
    info.configured = false;
    info.checked_mask = info.mismatch_mask = 0;
    info.id = 0;
    spi_clean();
    if (!command(ADS_CMD_RESET) || !command(ADS_CMD_SDATAC)) return false;
    if (!read_internal(ADS_REG_ID, &info.id, 1)) return false;
    /* Ignore revision bits [7:5], validate fixed/device/channel fields [4:0]. */
    if ((info.id & 0x1fu) != 0x1eu) return fail(ADS_ERR_ID);
    return true;
}

bool ads1299_preflight(const ads1299_settings_t *settings, uint32_t *spi_request) {
    uint8_t rate, gain;
    if (!settings || !ads1299_rate_code(settings->nominal_sps, &rate) ||
        !ads1299_gain_code(settings->gain, &gain) ||
        (unsigned)settings->mode > ADS_MODE_IMPEDANCE || (settings->srb1 && settings->srb2))
        return fail(ADS_ERR_ARGUMENT);
    if (!info.initialized || info.fatal) return fail(ADS_ERR_STATE);
    if (!ads1299_spi_plan(ADS_MCLK_HZ, rate, clock_get_hz(clk_peri), ADS_SPI_HZ,
                         ADS_SPI_MAX_HZ, spi_request)) return fail(ADS_ERR_SPI_BUDGET);
    if (ads1299_spi_actual(clock_get_hz(clk_peri), *spi_request) != ADS_SPI_HZ)
        return fail(ADS_ERR_SPI_BUDGET);
    return true;
}

bool ads1299_configure(const ads1299_settings_t *settings) {
    uint32_t request;
    if (!ads1299_preflight(settings, &request)) return false;
    uint8_t rate = 0, gain = 0;
    (void)ads1299_rate_code(settings->nominal_sps, &rate);
    (void)ads1299_gain_code(settings->gain, &gain);
    if (info.running && !ads1299_stop()) return false;
    info.configured = false;
    /* Fully stopped, DMA disabled, CS high. Preserve chosen rate through FIFO resets. */
    spi_request_hz = request;
    info.spi_hz = spi_set_baudrate(spi0, request);
    if (info.spi_hz != ADS_SPI_HZ || !ads1299_spi_budget_ok(ADS_MCLK_HZ, rate, info.spi_hz))
        return fail(ADS_ERR_SPI_BUDGET);
    if (!command(ADS_CMD_SDATAC)) return false;
    memset(info.expected, 0, sizeof info.expected);
    info.checked_mask = info.mismatch_mask = 0;
    /* Reserved bits preserved, DAISY_EN=1 for a standalone chip, CLK_EN=0. */
    info.expected[ADS_REG_CONFIG1] = ADS_CONFIG1_RESERVED | ADS_CONFIG1_MULTIPLE_READBACK | rate;
    ads_mode_registers(info.expected, settings, gain);
    info.expected[ADS_REG_GPIO] = 0x0f;
    for (uint8_t a = ADS_REG_CONFIG1; a < ADS_REG_COUNT; ++a) {
        if (a == ADS_REG_LOFF_STATP || a == ADS_REG_LOFF_STATN) continue;
        if (!write_internal(a, &info.expected[a], 1)) return false;
    }
    sleep_ms(ADS_REFERENCE_SETTLE_MS);
    if (!read_internal(0, info.readback, ADS_REG_COUNT)) return false;
    info.id = info.readback[ADS_REG_ID];
    if ((info.id & 0x1fu) != 0x1eu) return fail(ADS_ERR_ID);
    for (uint8_t a = ADS_REG_CONFIG1; a < ADS_REG_COUNT; ++a) {
        if (a == ADS_REG_LOFF_STATP || a == ADS_REG_LOFF_STATN) continue;
        uint8_t mask = a == ADS_REG_CONFIG3 ? 0xfe : a == ADS_REG_GPIO ? 0x0f : 0xff;
        info.checked_mask |= 1u << a;
        if ((info.expected[a] & mask) != (info.readback[a] & mask)) info.mismatch_mask |= 1u << a;
    }
    if (info.mismatch_mask) return fail(ADS_ERR_READBACK);
    info.settings = *settings;
    info.period_us = ads1299_period_us(ADS_MCLK_HZ, rate);
    transfer_us = (uint32_t)((216000000ull + info.spi_hz - 1u) / info.spi_hz);
    info.configured = true;
    info.last_error = ADS_OK;
    return true;
}

bool ads1299_start(void) {
    if (!info.configured || info.fatal) return fail(ADS_ERR_STATE);
    if (info.running) return true;
    spi_clean();
    /* RDATAC first while stopped; arm immediately after START. The first
     * DRDY includes digital-filter settling (several output periods). */
    if (!command(ADS_CMD_RDATAC)) return false;
    gpio_put(ADS_PIN_CS, 0);
    busy_wait_us_32(clock_us(2));
    uint8_t discard;
    if (!byte_xfer(ADS_CMD_START, &discard)) return end_control(false);
    dma_hw->ints0 = dma_mask;
    pending_fault = ADS_OK;
    gpio_acknowledge_irq(ADS_PIN_DRDY, GPIO_IRQ_EDGE_FALL);
    last_drdy_us = time_us_64();
    previous_poll_us = 0; next_watchdog_us = time_us_32();
    if (state_callback) state_callback(true, &info.settings);
    info.running = true;
    dma_set_irq0_channel_mask_enabled(1u << rx_channel, true);
    gpio_set_irq_enabled(ADS_PIN_DRDY, GPIO_IRQ_EDGE_FALL, true);
    /* Leave CS low until stop(), not until TX DMA completion. */
    return true;
}

bool ads1299_init(const ads1299_settings_t *settings) {
    ads1299_power_on();
    if (!info.initialized) {
        /* tPOR begins only once rails AND MCLK are valid; board margin first. */
        uint64_t wait_us = (uint64_t)ADS_SUPPLY_SETTLE_MS * 1000u + clock_us(1u << 18);
        uint64_t analog_us = (uint64_t)ADS_ANALOG_STARTUP_MS * 1000u;
        if (wait_us < analog_us) wait_us = analog_us;
        sleep_until(from_us_since_boot(power_enabled_us + wait_us));
        gpio_put(ADS_PIN_CS, 1);
        gpio_set_dir(ADS_PIN_CS, GPIO_OUT);
        spi_setup();
        gpio_set_function(ADS_PIN_DOUT, GPIO_FUNC_SPI);
        gpio_set_function(ADS_PIN_SCLK, GPIO_FUNC_SPI);
        gpio_set_function(ADS_PIN_DIN, GPIO_FUNC_SPI);
        if (!dma_setup()) return false;
        info.initialized = true;
    }
    recovery_attempts = 0; /* Explicit user init/retry opens a new bounded budget. */
    return ads1299_reset() && ads1299_configure(settings);
}

void ads1299_poll(void) {
    if (!info.running) return;
    uint32_t now32 = time_us_32();
    if (previous_poll_us && now32 - previous_poll_us > timing.poll_gap_max_us)
        timing.poll_gap_max_us = now32 - previous_poll_us;
    previous_poll_us = now32;
    /* Normally RX IRQ completes the frame. Poll only its possible SPI tail;
     * full watchdog work is capped at 10 kHz, not every empty main-loop pass. */
    if (dma_active && rx_done) {
        uint32_t began = time_us_32();
        uint32_t saved = save_and_disable_interrupts();
        finish_frame();
        restore_interrupts(saved);
        uint32_t elapsed = time_us_32() - began;
        if (elapsed > timing.critical_max_us) timing.critical_max_us = elapsed;
    }
    if (pending_fault == ADS_OK && (int32_t)(now32 - next_watchdog_us) < 0) return;
    next_watchdog_us = now32 + 100u;
    uint32_t began = time_us_32();
    uint32_t saved = save_and_disable_interrupts();
    ads1299_error_t fault = pending_fault;
    bool active_snapshot = dma_active;
    uint64_t dma_start = dma_started_us, drdy_time = last_drdy_us;
    bool hardware_error = active_snapshot && dma_has_error();
    restore_interrupts(saved);
    uint32_t elapsed = time_us_32() - began;
    if (elapsed > timing.critical_max_us) timing.critical_max_us = elapsed;
    uint64_t now = time_us_64();
    uint32_t dma_deadline = transfer_us * 2u + 100u;
    if (dma_deadline < info.period_us) dma_deadline = info.period_us;
    if (fault == ADS_OK && hardware_error) fault = ADS_ERR_DMA_HW;
    if (fault == ADS_OK && active_snapshot && now - dma_start > dma_deadline) fault = ADS_ERR_DMA_TIMEOUT;
    if (fault == ADS_OK && now - drdy_time > (uint64_t)info.period_us * 10u + 10000u)
        fault = ADS_ERR_DRDY_TIMEOUT;
    if (fault == ADS_OK) return;
    /* An IRQ may have completed/started a frame after the snapshot. Recheck
     * timeout candidates under the lock before stopping a healthy stream. */
    saved = save_and_disable_interrupts();
    if (pending_fault != ADS_OK) fault = pending_fault;
    else if (fault == ADS_ERR_DMA_TIMEOUT && (!dma_active || dma_started_us != dma_start)) fault = ADS_OK;
    else if (fault == ADS_ERR_DRDY_TIMEOUT && last_drdy_us != drdy_time) fault = ADS_OK;
    if (fault != ADS_OK) {
        pending_fault = fault;
        gpio_set_irq_enabled(ADS_PIN_DRDY, GPIO_IRQ_EDGE_FALL, false);
        if (fault == ADS_ERR_DMA_TIMEOUT) ++stats.dma_timeouts;
        if (fault == ADS_ERR_DRDY_TIMEOUT) ++stats.drdy_timeouts;
        if (fault == ADS_ERR_DMA_HW) ++stats.dma_errors;
    }
    restore_interrupts(saved);
    if (fault == ADS_OK) return;
    (void)ads1299_stop();
    if (info.fatal) return;
    info.configured = false;
    info.last_error = fault;
    if (recovery_attempts >= ADS_MAX_AUTO_RECOVERIES) return;
    ++recovery_attempts;
    ++stats.recoveries;
    ads1299_settings_t settings = info.settings;
    if (ads1299_reset() && ads1299_configure(&settings) && ads1299_start())
        info.last_error = fault; /* Keep the recovered incident visible. */
}

bool ads1299_get_frame(ads1299_frame_t *frame) {
    if (!frame) return false;
    /* Same-core SPSC: ISR cannot reuse this slot until tail is published.
     * Stop/reset only run on this main context, never concurrently. */
    uint32_t t = tail;
    if (t == head) return false;
    __dmb();
    raw_frame_t raw = queue[t % ADS_QUEUE_CAPACITY];
    __dmb();
    tail = t + 1u;
    uint32_t age = (uint32_t)(time_us_64() - raw.timestamp_us);
    if (age > timing.queue_age_max_us) timing.queue_age_max_us = age;
    memcpy(frame->raw, raw.raw, ADS_FRAME_BYTES);
    frame->sequence = raw.sequence;
    frame->timestamp_us = raw.timestamp_us;
    /* Raw transport only: decoded fields are intentionally untouched. */
    return true;
}

void ads1299_get_stats(ads1299_stats_t *out) {
    if (!out) return;
    uint32_t saved = save_and_disable_interrupts();
    *out = stats;
    restore_interrupts(saved);
}

void ads1299_get_perf(ads1299_perf_t *out) {
    if (!out) return;
    uint32_t saved = save_and_disable_interrupts();
    *out = timing;
    restore_interrupts(saved);
}

void ads1299_get_info(ads1299_info_t *out) { if (out) *out = info; }

const char *ads1299_mode_name(ads1299_mode_t mode) {
    switch (mode) {
    case ADS_MODE_TEST: return "internal-test";
    case ADS_MODE_SHORT: return "internal-short";
    case ADS_MODE_NORMAL: return "normal-differential";
    case ADS_MODE_IMPEDANCE: return "impedance-ac-raw";
    default: return "invalid";
    }
}

const char *ads1299_error_name(ads1299_error_t error) {
    static const char *const names[] = {"ok", "invalid-argument", "invalid-state", "SPI-timeout",
        "ID-mismatch", "register-readback", "DMA-resource", "SPI-too-slow-for-rate",
        "DRDY-timeout", "DMA-timeout", "DMA/SPI-hardware-error", "frame-status", "DMA-abort-fatal-reboot"};
    return (unsigned)error < sizeof names / sizeof names[0] ? names[error] : "unknown";
}
