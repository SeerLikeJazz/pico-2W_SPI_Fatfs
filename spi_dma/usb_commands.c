#include "usb_commands.h"
#include "ads1299_format.h"
#include <string.h>
static bool space(char c) { return c == ' ' || c == '\t'; }
static usb_command_t parse(char *line) {
    usb_command_t result = {.kind=USB_CMD_ERROR, .error="Use :rate <SPS> or :gain <multiple>"};
    char *p = line;
    while (space(*p)) ++p;
    if (!*p) { result.kind = USB_CMD_NONE; return result; }
    usb_command_kind_t kind;
    if (!strncmp(p, "rate", 4) && space(p[4])) kind = USB_CMD_RATE;
    else if (!strncmp(p, "gain", 4) && space(p[4])) kind = USB_CMD_GAIN;
    else return result;
    p += 4;
    while (space(*p)) ++p;
    if (*p < '0' || *p > '9') return result;
    uint32_t value = 0;
    while (*p >= '0' && *p <= '9') {
        unsigned digit = (unsigned)(*p++ - '0');
        if (value > (UINT32_MAX - digit) / 10u) { result.error = "Integer overflow"; return result; }
        value = value * 10u + digit;
    }
    while (space(*p)) ++p;
    if (*p) return result;
    uint8_t code;
    if (!(kind == USB_CMD_RATE ? ads1299_rate_code(value, &code) : ads1299_gain_code(value, &code))) {
        result.error = "Unsupported value"; return result;
    }
    result.kind = kind; result.value = value; result.error = NULL;
    return result;
}
usb_command_t usb_command_feed(usb_command_parser_t *p, int c) {
    usb_command_t result = {0};
    if (c < 0) return result;
    if (!p->active) {
        if (c == ':') { p->active = true; p->used = 0; p->overflow = false; }
        else if (c != '\r' && c != '\n' && c != ' ' && c != '\t' && c != 8 && c != 127)
            result = (usb_command_t){.kind=USB_CMD_LEGACY, .value=(uint32_t)c};
        return result;
    }
    if (c == '\r' || c == '\n') {
        p->active = false;
        if (p->overflow) return (usb_command_t){.kind=USB_CMD_ERROR, .error="Line too long; discarded"};
        p->line[p->used] = 0;
        return parse(p->line);
    }
    if (p->overflow) return result;
    if (c == 8 || c == 127) { if (p->used) --p->used; return result; }
    if (p->used == sizeof p->line - 1u) { p->overflow = true; return result; }
    /* Embedded NUL/control bytes must not hide suffixes from the parser. */
    p->line[p->used++] = (c < 32 && c != '\t') || c > 126 ? '\x01' : (char)c;
    return result;
}
