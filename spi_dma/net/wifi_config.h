#ifndef WIFI_CONFIG_H
#define WIFI_CONFIG_H
#define EEG_AP_SSID "Pico2W_EEG"
#define EEG_AP_PASSWORD "" /* Empty = OPEN development AP; set 8..63 chars for WPA2. */
#define EEG_TCP_PORT 5000u
#define EEG_CORE1_STACK_BYTES 8192u
#define EEG_NO_PROGRESS_US 10000000ull
#define EEG_AP_RETRY_LIMIT 3u
#define EEG_AP_RETRY_US 2000000ull
#define EEG_MCLK_ASSUMED 1
#endif
