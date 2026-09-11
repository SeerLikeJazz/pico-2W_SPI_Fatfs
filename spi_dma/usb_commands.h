#ifndef USB_COMMANDS_H
#define USB_COMMANDS_H
#include <stdbool.h>
#include <stdint.h>
#define USB_COMMAND_LINE_BYTES 48u
#define USB_COMMAND_CHARS_PER_POLL 16u
typedef enum { USB_CMD_NONE, USB_CMD_LEGACY, USB_CMD_RATE, USB_CMD_GAIN, USB_CMD_ERROR } usb_command_kind_t;
typedef struct { usb_command_kind_t kind; uint32_t value; const char *error; } usb_command_t;
typedef struct { char line[USB_COMMAND_LINE_BYTES]; unsigned used; bool active, overflow; } usb_command_parser_t;
/* Zero initialize once. No timing assumptions; ':' enters line mode. */
usb_command_t usb_command_feed(usb_command_parser_t *parser, int character);
#endif
