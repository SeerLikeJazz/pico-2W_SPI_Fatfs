#ifndef EEG_DHCP_OPTIONS_H
#define EEG_DHCP_OPTIONS_H
#include <stdint.h>
#include <stddef.h>
/* Return the complete TLV only if bounded by the actual received length. */
static inline const uint8_t *dhcp_find_option(const uint8_t *p, size_t n, uint8_t code) {
    for (size_t i = 0; i < n;) {
        if (p[i] == 255) break;
        if (p[i] == 0) { ++i; continue; }
        if (n - i < 2 || p[i+1] > n-i-2) return NULL;
        if (p[i] == code) return p+i;
        i += 2u + p[i+1];
    }
    return NULL;
}
#endif
