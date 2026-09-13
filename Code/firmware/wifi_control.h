#ifndef WIFI_CONTROL_H
#define WIFI_CONTROL_H
#include "ads1299.h"
/* Core 1 owns lwIP. Core 0 alone may access the ADC. */
bool wifi_control_listen(void);
void wifi_control_shutdown(void);
void wifi_control_poll(void);
void wifi_control_apply(ads1299_settings_t *settings);
void wifi_control_data_session(bool connected, uint32_t ip); /* Core 1 */
#endif
