#include "ads1299_control.h"
ads1299_change_result_t ads1299_change(ads1299_settings_t *ram, ads1299_change_t field,
                                     unsigned value, ads1299_error_t *error) {
    ads1299_info_t before, after;
    ads1299_get_info(&before);
    *error = ADS_ERR_STATE;
    if (!before.initialized || !before.configured || before.fatal) return ADS_CHANGE_FAILED;
    ads1299_settings_t requested = before.settings;
    if (field == ADS_CHANGE_RATE) requested.nominal_sps = value;
    else if (field == ADS_CHANGE_GAIN) requested.gain = value;
    else { *error = ADS_ERR_ARGUMENT; return ADS_CHANGE_FAILED; }
    uint32_t target;
    if (!ads1299_preflight(&requested, &target)) goto failed;
    *ram = before.settings;
    if (requested.nominal_sps == before.settings.nominal_sps && requested.gain == before.settings.gain) {
        *error = ADS_OK; return ADS_CHANGE_UNCHANGED;
    }
    /* configure performs preflight again and safely stops before register/SPI changes. */
    if (!ads1299_configure(&requested)) goto failed;
    *ram = requested; /* Only commit after successful register readback. */
    if (before.running && !ads1299_start()) goto failed;
    *error = ADS_OK; return ADS_CHANGE_APPLIED;
failed:
    ads1299_get_info(&after);
    *error = after.last_error;
    return ADS_CHANGE_FAILED;
}
