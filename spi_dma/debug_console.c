#include "debug_console.h"
#include <stdarg.h>
#include <stdio.h>
#include "pico/stdio_usb.h"
#include "tusb.h"
#if ENABLE_UART_LOG
#include "hardware/gpio.h"
#include "hardware/uart.h"
#endif

/* Main-loop-only TinyUSB calls. CMake disables the SDK background task IRQ;
 * the USBCTRL hardware IRQ still processes hardware events. */
#define LOG_CAPACITY 8192u
static char usb_queue[LOG_CAPACITY];
static uint32_t usb_head, usb_tail, dropped, discarded_bytes;
static bool usb_ok;
#if ENABLE_UART_LOG
static char uart_queue[LOG_CAPACITY];
static uint32_t uart_head, uart_tail, uart_dropped;
#endif

bool debug_console_init(void) {
    usb_ok = stdio_usb_init();
#if ENABLE_UART_LOG
    uart_init(uart0, 115200);
    gpio_set_function(0, GPIO_FUNC_UART); /* Opt-in; conflicts with screen MOSI. */
#endif
    return usb_ok;
}

bool debug_console_connected(void) { return usb_ok && tud_cdc_connected(); }

void debug_console_poll(void) {
    if (usb_ok) {
        tud_task_ext(0, false); /* Zero queue wait; never wait for a host event. */
        if (!tud_cdc_connected()) {
            discarded_bytes += usb_head - usb_tail;
            usb_tail = usb_head;
        } else {
            uint32_t count = usb_head - usb_tail;
            uint32_t contiguous = LOG_CAPACITY - usb_tail % LOG_CAPACITY;
            if (count > contiguous) count = contiguous;
            if (count > 64u) count = 64u;
            uint32_t available = tud_cdc_write_available();
            if (count > available) count = available;
            if (count) usb_tail += tud_cdc_write(&usb_queue[usb_tail % LOG_CAPACITY], count);
            (void)tud_cdc_write_flush();
        }
    }
#if ENABLE_UART_LOG
    for (unsigned i = 0; i < 16u && uart_tail != uart_head && uart_is_writable(uart0); ++i)
        uart_get_hw(uart0)->dr = (uint8_t)uart_queue[uart_tail++ % LOG_CAPACITY];
#endif
}

int debug_console_getchar(void) {
    return debug_console_connected() && tud_cdc_available() ? tud_cdc_read_char() : -1;
}

void debug_log(const char *format, ...) {
    char line[768];
    va_list args;
    va_start(args, format);
    int result = vsnprintf(line, sizeof line, format, args);
    va_end(args);
    if (result < 0 || (size_t)result >= sizeof line) { ++dropped; return; }
    uint32_t count = (uint32_t)result;
    if (!debug_console_connected() || count > LOG_CAPACITY - (usb_head - usb_tail)) {
        ++dropped;
        discarded_bytes += count;
    } else {
        for (uint32_t i = 0; i < count; ++i) usb_queue[usb_head++ % LOG_CAPACITY] = line[i];
    }
#if ENABLE_UART_LOG
    if (count > LOG_CAPACITY - (uart_head - uart_tail)) ++uart_dropped;
    else for (uint32_t i = 0; i < count; ++i) uart_queue[uart_head++ % LOG_CAPACITY] = line[i];
#endif
}

uint32_t debug_log_dropped_messages(void) { return dropped; }
uint32_t debug_log_discarded_bytes(void) { return discarded_bytes; }
uint32_t debug_uart_dropped_messages(void) {
#if ENABLE_UART_LOG
    return uart_dropped;
#else
    return 0;
#endif
}
