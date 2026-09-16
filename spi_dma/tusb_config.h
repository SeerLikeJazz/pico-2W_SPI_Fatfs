#ifndef EEG_TUSB_CONFIG_H
#define EEG_TUSB_CONFIG_H
#define CFG_TUSB_RHPORT0_MODE (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)
/* Pico SDK supplies OPT_OS_PICO (IRQ-safe event queue, no RTOS task). */
#define CFG_TUSB_DEBUG 0
#define CFG_TUD_ENDPOINT0_SIZE 64
#define CFG_TUD_CDC 1
#define CFG_TUD_CDC_RX_BUFSIZE 1024
#define CFG_TUD_CDC_TX_BUFSIZE 4096
/* Larger transfer than a single 64-byte USB packet; RP2350 DCD splits it. */
#define CFG_TUD_CDC_EP_BUFSIZE 512
/* TinyUSB only arms OUT when the RX FIFO has one full transfer free. */
#if CFG_TUD_CDC_RX_BUFSIZE < CFG_TUD_CDC_EP_BUFSIZE
#error "CDC RX FIFO must hold at least one endpoint transfer"
#endif
#endif
