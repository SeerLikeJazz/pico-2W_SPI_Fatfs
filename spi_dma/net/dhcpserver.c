/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2018-2019 Damien P. George
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

// Adapted from Raspberry Pi pico-examples access_point/dhcpserver.
// Local changes: bounded TLVs, validated BOOTP, renewing/releasing leases,
// explicit AP-bound broadcasts, allocation/bind failure reporting, no stdio.
#include <string.h>
#include "dhcpserver.h"
#include "dhcp_options.h"
#include "pico/time.h"
#include "lwip/udp.h"

#define LEASE_MS 86400000u
static uint32_t ticks_ms(void) { return (uint32_t)(time_us_64() / 1000u); }
static bool empty_mac(const uint8_t *p) {
    static const uint8_t zero[6] = {0};
    return memcmp(p, zero, 6) == 0;
}
static void option(uint8_t *reply, size_t *n, uint8_t code, const void *data, uint8_t len) {
    reply[(*n)++] = code; reply[(*n)++] = len;
    memcpy(reply + *n, data, len); *n += len;
}
static void receive(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                    const ip_addr_t *address, u16_t port) {
    (void)address;
    dhcp_server_t *d = arg;
    if (!p) return;
    uint8_t request[576] = {0}, reply[320] = {0};
    size_t length = pbuf_copy_partial(p, request, sizeof request, 0);
    pbuf_free(p);
    if (port != 68 || length < 240 || request[0] != 1 || request[1] != 1 ||
        request[2] != 6 || memcmp(request+236, "\x63\x82\x53\x63", 4) ||
        empty_mac(request+28)) return;
    /* No relays on this isolated AP. */
    if (request[24] || request[25] || request[26] || request[27]) return;
    const uint8_t *type = dhcp_find_option(request+240, length-240, 53);
    const uint8_t *server = dhcp_find_option(request+240, length-240, 54);
    const uint8_t *requested = dhcp_find_option(request+240, length-240, 50);
    const uint8_t *server_ip = (const uint8_t *)&ip4_addr_get_u32(ip_2_ip4(&d->ip));
    if (!type || type[1] != 1) return;
    if (server && (server[1] != 4 || memcmp(server+2, server_ip, 4))) return;
    if (requested && requested[1] != 4) return;
    uint32_t now = ticks_ms();
    int slot = -1, free_slot = -1;
    for (int i = 0; i < DHCPS_MAX_IP; ++i) {
        if (!empty_mac(d->lease[i].mac) && (int32_t)(now-d->lease[i].expiry) >= 0)
            memset(d->lease[i].mac, 0, 6);
        if (!memcmp(d->lease[i].mac, request+28, 6)) slot = i;
        if (empty_mac(d->lease[i].mac) && free_slot < 0) free_slot = i;
    }
    if (type[2] == 7 || type[2] == 4) {
        if (slot >= 0) {
            if (type[2] == 7) memset(d->lease[slot].mac, 0, 6);
            else { memset(d->lease[slot].mac, 0xff, 6); d->lease[slot].expiry = now + 60000u; }
        }
        return;
    }
    uint8_t message;
    if (type[2] == 1) {
        if (slot < 0) slot = free_slot;
        if (slot < 0) return;
        message = 2; /* OFFER; reserve MAC immediately against parallel DISCOVER. */
    } else if (type[2] == 3) {
        const uint8_t *ip = requested ? requested+2 : request+12; /* RENEW uses ciaddr. */
        int wanted = (int)ip[3] - DHCPS_BASE_IP;
        if (memcmp(ip, server_ip, 3) || wanted < 0 || wanted >= DHCPS_MAX_IP ||
            (!empty_mac(d->lease[wanted].mac) && memcmp(d->lease[wanted].mac, request+28, 6))) {
            message = 6; /* NAK */ slot = -1;
        } else { slot = wanted; message = 5; }
    } else return;
    reply[0] = 2; reply[1] = 1; reply[2] = 6;
    memcpy(reply+4, request+4, 4); /* xid */
    reply[10] = 0x80; /* Broadcast, works before a client owns an IP. */
    memcpy(reply+28, request+28, 16);
    memcpy(reply+236, "\x63\x82\x53\x63", 4);
    if (slot >= 0) {
        memcpy(d->lease[slot].mac, request+28, 6);
        d->lease[slot].expiry = now + (message == 2 ? 60000u : LEASE_MS);
        memcpy(reply+16, server_ip, 4); reply[19] = (uint8_t)(DHCPS_BASE_IP+slot);
    }
    size_t n = 240;
    option(reply, &n, 53, &message, 1);
    option(reply, &n, 54, server_ip, 4);
    if (message != 6) {
        option(reply, &n, 1, &ip4_addr_get_u32(ip_2_ip4(&d->nm)), 4);
        option(reply, &n, 3, server_ip, 4);
        const uint8_t lease[4] = {0, 1, 0x51, 0x80}; /* 86400 seconds */
        option(reply, &n, 51, lease, 4);
        /* No DNS option: this firmware does not implement a DNS server. */
    }
    reply[n++] = 255;
    if (n < 300) n = 300; /* BOOTP minimum reply including padding. */
    struct pbuf *out = pbuf_alloc(PBUF_TRANSPORT, (u16_t)n, PBUF_RAM);
    if (!out) return;
    if (pbuf_take(out, reply, (u16_t)n) == ERR_OK) {
        ip_addr_t broadcast; IP4_ADDR(&broadcast, 255,255,255,255);
        (void)udp_sendto_if(pcb, out, &broadcast, 68, ip_current_input_netif());
    }
    pbuf_free(out);
}
void dhcp_server_init(dhcp_server_t *d, struct netif *nif, ip_addr_t *ip, ip_addr_t *nm) {
    memset(d, 0, sizeof *d);
    ip_addr_copy(d->ip, *ip); ip_addr_copy(d->nm, *nm);
    d->udp = udp_new_ip_type(IPADDR_TYPE_V4);
    if (!d->udp) return;
    ip_set_option(d->udp, SOF_BROADCAST);
    if (udp_bind(d->udp, IP_ANY_TYPE, 67) != ERR_OK) { udp_remove(d->udp); d->udp = NULL; return; }
    udp_bind_netif(d->udp, nif);
    udp_recv(d->udp, receive, d);
}
void dhcp_server_deinit(dhcp_server_t *d) {
    if (d->udp) { udp_remove(d->udp); d->udp = NULL; }
}
