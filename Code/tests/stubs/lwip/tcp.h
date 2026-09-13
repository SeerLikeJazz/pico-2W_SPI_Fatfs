#pragma once
#include <stdint.h>
#include <string.h>
typedef int err_t;
typedef uint16_t u16_t;
struct tcp_pcb { uint32_t remote_ip; };
struct pbuf { u16_t tot_len; const void *payload; };
#define ERR_OK 0
#define ERR_MEM -1
#define ERR_ABRT -2
#define ERR_VAL -3
#define IPADDR_TYPE_V4 0
#define IP_ADDR_ANY 0
#define TCP_WRITE_FLAG_COPY 1
#define ip_2_ip4(p) (p)
#define ip4_addr_get_u32(p) (*(p))
#define lwip_ntohl(v) (v)
struct tcp_pcb *tcp_new_ip_type(int type);
struct tcp_pcb *tcp_listen_with_backlog_and_err(struct tcp_pcb *p, int n, err_t *e);
err_t tcp_bind(struct tcp_pcb *p, int ip, int port);
err_t tcp_close(struct tcp_pcb *p);
void tcp_abort(struct tcp_pcb *p);
err_t tcp_write(struct tcp_pcb *p, const void *v, unsigned n, int flags);
err_t tcp_output(struct tcp_pcb *p);
void tcp_recv(struct tcp_pcb *p, err_t (*cb)(void *,struct tcp_pcb *,struct pbuf *,err_t));
void tcp_err(struct tcp_pcb *p, void (*cb)(void *,err_t));
void tcp_sent(struct tcp_pcb *p, void *cb);
void tcp_poll(struct tcp_pcb *p, void *cb, int period);
void tcp_accept(struct tcp_pcb *p, err_t (*cb)(void *,struct tcp_pcb *,err_t));
void tcp_nagle_disable(struct tcp_pcb *p);
void tcp_recved(struct tcp_pcb *p, unsigned n);
void pbuf_free(struct pbuf *p);
static inline unsigned pbuf_copy_partial(struct pbuf *p, void *dest, unsigned n, unsigned offset) {
    memcpy(dest,(const uint8_t *)p->payload+offset,n); return n;
}
