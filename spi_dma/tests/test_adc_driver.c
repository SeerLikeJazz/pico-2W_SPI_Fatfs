#define ADS_DRIVER_TEST 1
#include "../ads1299.c"
#include <assert.h>
#include <stdio.h>
static void reset_fixture(void) {
    memset(&info,0,sizeof info); memset((void *)&stats,0,sizeof stats);
    memset(channels,0,sizeof channels); memset(&spi_hw,0,sizeof spi_hw);
    memset(&dma_regs,0,sizeof dma_regs); memset(&timing,0,sizeof timing);
    rx_channel=0;tx_channel=1;dma_mask=3;head=tail=0;
    dma_active=rx_done=active_tainted=false;pending_fault=ADS_OK;
    channel_busy[0]=channel_busy[1]=spi_busy=spi_readable=false;
    info.running=info.initialized=info.configured=true;info.period_us=63;
    fake_now=1000;last_drdy_us=1000;transfer_us=15;next_watchdog_us=0;
    after_unlock=0;irq_locked=false;start_order=0;
}
static void complete(bool tail_busy) {
    active.raw[0]=0xc0;channels[0].transfer_count=0;channels[1].transfer_count=0;
    channel_busy[0]=channel_busy[1]=false;spi_busy=tail_busy;
    dma_regs.ints0=1;dma_irq_work();dma_regs.ints0=0;
}
static void late_completion(void) { complete(false); }
int main(void) {
    reset_fixture();drdy_work(ADS_PIN_DRDY,GPIO_IRQ_EDGE_FALL);
    assert(dma_active && start_order==6); /* RX mask1 then TX mask2. */
    complete(true);assert(head==0 && dma_active && rx_done);
    spi_busy=false;ads1299_poll();assert(head==1 && !dma_active);
    finish_frame();assert(head==1); /* No duplicate publish from IRQ/poll. */
    const ads1299_raw_frame_t *borrow=ads1299_peek_raw();assert(borrow && borrow->sequence==1);
    assert(!irq_locked && tail==0);ads1299_consume_raw();assert(tail==1);
    reset_fixture();drdy_work(ADS_PIN_DRDY,1);fake_now+=63;drdy_work(ADS_PIN_DRDY,1);
    assert(stats.busy_drdy==1 && active_tainted);complete(false);
    assert(stats.frames==0 && stats.tainted_frames==1 && head==0);
    reset_fixture();head=ADS_QUEUE_CAPACITY;tail=0;queue[0].sequence=77;
    drdy_work(ADS_PIN_DRDY,1);complete(false);
    assert(stats.frames==1 && stats.queue_drops==1 && queue[0].sequence==77);
    reset_fixture();head=tail=UINT32_MAX;drdy_work(ADS_PIN_DRDY,1);complete(false);
    assert(head==0 && ads1299_peek_raw()->sequence==1);ads1299_consume_raw();assert(tail==0);
    reset_fixture();drdy_work(ADS_PIN_DRDY,1);fake_now+=200;
    after_unlock=late_completion;ads1299_poll();
    assert(info.running && stats.dma_timeouts==0 && pending_fault==ADS_OK);
    reset_fixture();drdy_work(ADS_PIN_DRDY,1);channels[0].ctrl_trig=DMA_CH0_CTRL_TRIG_AHB_ERROR_BITS;
    complete(false);assert(pending_fault==ADS_ERR_DMA_HW && head==0);
    puts("PASS production ADC RX-before-TX, SPI tail, no duplicate, taint, queue ownership/wrap, timeout race, DMA error");
}
