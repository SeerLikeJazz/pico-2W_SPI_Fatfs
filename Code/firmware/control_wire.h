#ifndef CONTROL_WIRE_H
#define CONTROL_WIRE_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#define CONTROL_PORT 5001u
#define CONTROL_REQUEST_SIZE 20u
#define CONTROL_REPLY_SIZE 48u
static inline uint32_t control_u32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
static inline void control_put(uint8_t *p, uint32_t v) {
    for (unsigned i=0; i<4; ++i) p[i]=(uint8_t)(v>>(8*i));
}
static inline bool control_valid(const uint8_t *p) {
    uint32_t op=control_u32(p+8);
    return !memcmp(p,"WFC2",4) && op<=5 &&
           ((op>=1 && op<=3) || control_u32(p+12)==0);
}
#endif
