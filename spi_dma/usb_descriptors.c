#include <string.h>
#include "tusb.h"
#include "pico/unique_id.h"

static const tusb_desc_device_t device = {
    .bLength = sizeof(tusb_desc_device_t), .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200, .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON, .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = 64, .idVendor = 0x2e8a, .idProduct = 0x0009,
    .bcdDevice = 0x0200, .iManufacturer = 1, .iProduct = 2,
    .iSerialNumber = 3, .bNumConfigurations = 1
};
static const uint8_t config[] = {
    TUD_CONFIG_DESCRIPTOR(1, 2, 0, TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN, 0, 250),
    TUD_CDC_DESCRIPTOR(0, 4, 0x81, 8, 0x02, 0x82, 64)
};
const uint8_t *tud_descriptor_device_cb(void) { return (const uint8_t *)&device; }
const uint8_t *tud_descriptor_configuration_cb(uint8_t index) { (void)index; return config; }
const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    static uint16_t descriptor[32];
    static char serial[PICO_UNIQUE_BOARD_ID_SIZE_BYTES * 2 + 1];
    const char *strings[] = {"", "Raspberry Pi", "ADS1299 USB Raw", serial, "Binary acquisition"};
    unsigned count = 0;
    if (index == 0) { descriptor[1] = 0x0409; count = 1; }
    else {
        if (index >= sizeof strings / sizeof *strings) return NULL;
        if (!serial[0]) pico_get_unique_board_id_string(serial, sizeof serial);
        for (; count < 31 && strings[index][count]; ++count)
            descriptor[count + 1] = (uint8_t)strings[index][count];
    }
    descriptor[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * count + 2));
    return descriptor;
}
