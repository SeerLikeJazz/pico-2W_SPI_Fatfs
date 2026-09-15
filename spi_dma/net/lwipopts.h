#ifndef EEG_LWIPOPTS_H
#define EEG_LWIPOPTS_H
#define NO_SYS 1
#define LWIP_SOCKET 0
#define LWIP_NETCONN 0
#define LWIP_IPV4 1
#define LWIP_IPV6 0
#define LWIP_TCP 1
#define LWIP_UDP 1
#define LWIP_ARP 1
#define LWIP_ICMP 1
#define LWIP_DHCP 1
#define LWIP_DNS 0
#define LWIP_NETIF_STATUS_CALLBACK 1
#define LWIP_NETIF_LINK_CALLBACK 1
#define MEM_ALIGNMENT 4
#define MEM_SIZE (32 * 1024)
#define MEMP_NUM_TCP_PCB 4
#define MEMP_NUM_TCP_PCB_LISTEN 2
#define MEMP_NUM_TCP_SEG 64
#define MEMP_NUM_UDP_PCB 4
#define PBUF_POOL_SIZE 16
#define PBUF_POOL_BUFSIZE 1536
#define TCP_MSS 1460
/* Tune only from measured ACK delay and queue age; override at build time. */
#ifndef EEG_TCP_SND_MSS
#define EEG_TCP_SND_MSS 8
#endif
#define TCP_SND_BUF (EEG_TCP_SND_MSS * TCP_MSS)
#define TCP_WND (4 * TCP_MSS)
#define TCP_SND_QUEUELEN 64
#define LWIP_TCP_KEEPALIVE 1
#define LWIP_NETIF_HOSTNAME 1
#define LWIP_STATS 0
#define LWIP_PROVIDE_ERRNO 1
#define LWIP_CHKSUM_ALGORITHM 3
#endif
