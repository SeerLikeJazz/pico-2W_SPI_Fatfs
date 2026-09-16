/* Hardware boundary only. Tests execute the production driver state machine. */
#ifndef ADC_HW_FAKE_H
#define ADC_HW_FAKE_H
#include <stdint.h>
#include <stdbool.h>
typedef unsigned uint;
typedef struct { uint32_t ctrl_trig, transfer_count, al1_ctrl; } fake_channel_t;
typedef struct { uint32_t dr, icr, ris, dmacr; } fake_spi_t;
typedef struct { uint32_t ints0, abort; } fake_dma_t;
typedef unsigned dma_channel_config;
static fake_channel_t channels[2];
static fake_spi_t spi_hw;
static fake_dma_t dma_regs;
#define dma_hw (&dma_regs)
#define spi0 0
static uint64_t fake_now;
static bool channel_busy[2], spi_busy, spi_readable, irq_locked;
static void (*after_unlock)(void);
static uint32_t irq_enabled_mask, start_order;
#define DMA_CH0_CTRL_TRIG_AHB_ERROR_BITS 0x80000000u
#define DMA_CH0_CTRL_TRIG_EN_BITS 1u
#define DMA_CH0_CTRL_TRIG_READ_ERROR_BITS 0x40000000u
#define DMA_CH0_CTRL_TRIG_WRITE_ERROR_BITS 0x20000000u
#define DMA_IRQ_0 0
#define DMA_SIZE_8 0
#define GPIO_FUNC_SPI 0
#define GPIO_IN 0
#define GPIO_OUT 1
#define GPIO_IRQ_EDGE_FALL 1
#define IO_IRQ_BANK0 1
#define PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY 0
#define SPI_CPHA_1 1
#define SPI_CPOL_0 0
#define SPI_MSB_FIRST 0
#define SPI_SSPDMACR_RXDMAE_BITS 1u
#define SPI_SSPDMACR_TXDMAE_BITS 2u
#define SPI_SSPICR_RORIC_BITS 1u
#define SPI_SSPICR_RTIC_BITS 2u
#define SPI_SSPRIS_RORRIS_BITS 1u
#define clk_peri 0
static inline uint64_t time_us_64(void) { return fake_now; }
static inline uint32_t time_us_32(void) { return (uint32_t)fake_now; }
static inline uint32_t save_and_disable_interrupts(void) { uint32_t old=irq_locked; irq_locked=true; return old; }
static inline void restore_interrupts(uint32_t old) {
    irq_locked=old;
    if (!old && after_unlock) { void (*fn)(void)=after_unlock; after_unlock=0; fn(); }
}
static inline void __dmb(void) { }
static inline void busy_wait_us_32(uint32_t us) { fake_now+=us; }
static inline void sleep_ms(uint32_t ms) { fake_now+=(uint64_t)ms*1000; }
static inline uint64_t from_us_since_boot(uint64_t us) { return us; }
static inline void sleep_until(uint64_t us) { fake_now=us; }
static inline void tight_loop_contents(void) { ++fake_now; }
static inline uint32_t clock_get_hz(int clk) { (void)clk; return 150000000; }
static inline fake_spi_t *spi_get_hw(int spi) { (void)spi; return &spi_hw; }
static inline bool spi_is_busy(int spi) { (void)spi; return spi_busy; }
static inline bool spi_is_readable(int spi) { (void)spi; return spi_readable; }
static inline bool spi_is_writable(int spi) { (void)spi; return true; }
static inline uint32_t spi_init(int spi,uint32_t hz) { (void)spi; return hz; }
static inline uint32_t spi_set_baudrate(int spi,uint32_t hz) { (void)spi; return hz; }
static inline void spi_set_format(int spi,int bits,int pol,int phase,int order) { (void)spi;(void)bits;(void)pol;(void)phase;(void)order; }
static inline void spi_deinit(int spi) { (void)spi; }
static inline unsigned spi_get_dreq(int spi,bool tx) { (void)spi; return tx; }
static inline fake_channel_t *dma_channel_hw_addr(int channel) { return &channels[channel]; }
static inline bool dma_channel_is_busy(int channel) { return channel_busy[channel]; }
static inline void dma_channel_set_write_addr(int ch,void *addr,bool trigger) { (void)ch;(void)addr;(void)trigger; }
static inline void dma_channel_set_read_addr(int ch,const void *addr,bool trigger) { (void)ch;(void)addr;(void)trigger; }
static inline void dma_channel_set_trans_count(int ch,unsigned count,bool trigger) { (void)trigger;channels[ch].transfer_count=count; }
static inline void dma_start_channel_mask(uint32_t mask) { start_order=start_order*4+mask; for(int i=0;i<2;i++) if(mask&(1u<<i)) channel_busy[i]=true; }
static inline void dma_set_irq0_channel_mask_enabled(uint32_t mask,bool enable) { if(enable)irq_enabled_mask|=mask;else irq_enabled_mask&=~mask; }
static inline int dma_claim_unused_channel(bool required) { static int next; (void)required;return next++%2; }
static inline void dma_channel_unclaim(int ch) { (void)ch; }
static inline dma_channel_config dma_channel_get_default_config(int ch) { return (unsigned)ch; }
static inline void channel_config_set_transfer_data_size(dma_channel_config *c,unsigned v) { (void)c;(void)v; }
static inline void channel_config_set_read_increment(dma_channel_config *c,bool v) { (void)c;(void)v; }
static inline void channel_config_set_write_increment(dma_channel_config *c,bool v) { (void)c;(void)v; }
static inline void channel_config_set_dreq(dma_channel_config *c,unsigned v) { (void)c;(void)v; }
static inline void dma_channel_configure(int ch,const dma_channel_config *c,void *w,const void *r,unsigned n,bool start) { (void)ch;(void)c;(void)w;(void)r;(void)n;(void)start; }
static inline void irq_add_shared_handler(int irq,void(*fn)(void),int p) { (void)irq;(void)fn;(void)p; }
static inline void irq_set_priority(int irq,unsigned p) { (void)irq;(void)p; }
static inline void irq_set_enabled(int irq,bool en) { (void)irq;(void)en; }
static inline void gpio_set_irq_enabled_with_callback(unsigned pin,unsigned ev,bool en,void(*fn)(uint,uint32_t)) { (void)pin;(void)ev;(void)en;(void)fn; }
static inline void gpio_set_irq_enabled(unsigned pin,unsigned ev,bool en) { (void)pin;(void)ev;(void)en; }
static inline void gpio_acknowledge_irq(unsigned pin,unsigned ev) { (void)pin;(void)ev; }
static inline void gpio_init(unsigned pin) { (void)pin; }
static inline void gpio_disable_pulls(unsigned pin) { (void)pin; }
static inline void gpio_set_dir(unsigned pin,bool dir) { (void)pin;(void)dir; }
static inline void gpio_put(unsigned pin,bool value) { (void)pin;(void)value; }
static inline void gpio_set_function(unsigned pin,unsigned fn) { (void)pin;(void)fn; }
static inline void hw_clear_bits(uint32_t *p,uint32_t bits) { *p &= ~bits; }
static inline void hw_set_bits(uint32_t *p,uint32_t bits) { *p |= bits; }
#endif
