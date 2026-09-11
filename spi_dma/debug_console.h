#ifndef DEBUG_CONSOLE_H
#define DEBUG_CONSOLE_H
#include <stdbool.h>
#include <stdint.h>
bool debug_console_init(void);
void debug_console_poll(void);
bool debug_console_connected(void);
int debug_console_getchar(void);
void debug_log(const char *format, ...) __attribute__((format(printf, 1, 2)));
uint32_t debug_log_dropped_messages(void);
uint32_t debug_log_discarded_bytes(void);
uint32_t debug_uart_dropped_messages(void);
#endif
