#ifndef ADS1299_CONTROL_H
#define ADS1299_CONTROL_H
#include "ads1299.h"
typedef enum { ADS_CHANGE_RATE, ADS_CHANGE_GAIN } ads1299_change_t;
typedef enum { ADS_CHANGE_FAILED, ADS_CHANGE_UNCHANGED, ADS_CHANGE_APPLIED } ads1299_change_result_t;
/* Core 0 only. RAM keeps last verified settings for explicit 'i' retry.
 * Failure after hardware modification leaves acquisition stopped, no blind rollback. */
ads1299_change_result_t ads1299_change(ads1299_settings_t *ram, ads1299_change_t field,
                                     unsigned value, ads1299_error_t *error);
#endif
