#ifndef ADS_MODE_REGISTERS_H
#define ADS_MODE_REGISTERS_H
#include "ads1299.h"
#include "ads1299_config.h"
/* Caller zeroes the full image before each configuration. Shared by driver/tests. */
static inline void ads_mode_registers(uint8_t *registers, const ads1299_settings_t *settings, uint8_t gain) {
    registers[ADS_REG_CONFIG2] = ADS_CONFIG2_RESERVED |
        (settings->mode == ADS_MODE_TEST ? ADS_CONFIG2_INT_CAL : 0);
    registers[ADS_REG_CONFIG3] = ADS_CONFIG3_RESERVED | ADS_CONFIG3_REF_ON | ADS_CONFIG3_BIASREF_INT;
    uint8_t mux = settings->mode == ADS_MODE_TEST ? ADS_MUX_TEST :
                  settings->mode == ADS_MODE_SHORT ? ADS_MUX_SHORT : ADS_MUX_NORMAL;
    for (unsigned i = ADS_REG_CH1SET; i <= ADS_REG_CH8SET; ++i)
        registers[i] = (uint8_t)((gain << 4) | mux |
            (settings->mode == ADS_MODE_NORMAL && settings->srb2 ? ADS_CH_SRB2 : 0));
    if (settings->mode == ADS_MODE_NORMAL) {
        if (settings->srb1) registers[ADS_REG_MISC1] = ADS_MISC1_SRB1;
        if (settings->bias) {
            registers[ADS_REG_CONFIG3] |= ADS_CONFIG3_BIAS_ON;
            registers[ADS_REG_BIAS_SENSP] = ADS_BIAS_SENSP_MASK;
            registers[ADS_REG_BIAS_SENSN] = ADS_BIAS_SENSN_MASK;
        }
    }
    if (settings->mode == ADS_MODE_IMPEDANCE) {
        /* TI SBAS499C 9.3.2.4.3.2 / 9.6.1.5: 6 nA, fCLK/65536.
         * Differential P/N excitation; no SRB or BIAS routing. Raw codes only. */
        registers[ADS_REG_LOFF] = 0x02;
        registers[ADS_REG_LOFF_SENSP] = 0xff;
        registers[ADS_REG_LOFF_SENSN] = 0xff;
    }
    /* Other modes leave LOFF disabled. CONFIG4=0: comparators off, continuous. */
}
#endif
