#ifndef ADS1299_CONFIG_H
#define ADS1299_CONFIG_H

/* Board wiring: CLKSEL is LOW, so an external oscillator MUST be fitted. */
#define ADS_PIN_DOUT 16u
#define ADS_PIN_CS   17u
#define ADS_PIN_SCLK 18u
#define ADS_PIN_DIN  19u
#define ADS_PIN_DRDY 20u
#define ADS_PIN_5V_EN 21u

/* U12 frequency is not specified in Schematic.pdf: VERIFY ON HARDWARE. */
#ifndef ADS_MCLK_HZ
#define ADS_MCLK_HZ 2048000u
#endif
#ifndef ADS_SPI_HZ
#define ADS_SPI_HZ 1000000u
#endif
/* Adaptive requests: base, 2x, 4x ... capped here; actual SDK rate rechecked. */
#ifndef ADS_SPI_MAX_HZ
#define ADS_SPI_MAX_HZ 8000000u
#endif
#ifndef ADS_DEFAULT_RATE
#define ADS_DEFAULT_RATE 250u /* Nominal SPS at 2.048 MHz. */
#endif
#define ADS_DEFAULT_MODE ADS_MODE_TEST
#define ADS_DEFAULT_GAIN 1u

/* Normal mode defaults to independent INxP/INxN, no SRB connection.
 * Set only after checking H2/H5 reference jumpers and electrode wiring. */
#define ADS_NORMAL_SRB1 0
#define ADS_NORMAL_SRB2 0
#define ADS_NORMAL_BIAS 0
#define ADS_BIAS_SENSP_MASK 0xffu
#define ADS_BIAS_SENSN_MASK 0xffu

/* Board/analog settling margins, not substitutes for voltage measurements.
 * VCAP1 (100 uF) must exceed 1.1 V before RESET. Increase if measurements require. */
#define ADS_SUPPLY_SETTLE_MS 100u
#define ADS_ANALOG_STARTUP_MS 1000u
#define ADS_REFERENCE_SETTLE_MS 150u
#define ADS_CONTROL_TIMEOUT_US 5000u
#define ADS_QUEUE_CAPACITY 64u
#define ADS_MAX_AUTO_RECOVERIES 2u

#endif
