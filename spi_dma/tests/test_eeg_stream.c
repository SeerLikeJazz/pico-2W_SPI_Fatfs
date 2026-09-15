#include "net/eeg_stream.h"
#include "net/dhcp_options.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

static eeg_queue_t q;
static eeg_stream_t s;
static uint8_t output[4096];
static unsigned output_size, limit = 1024;
static int sink(void *ctx, const uint8_t *p, size_t n) {
    (void)ctx;
    if (n > limit) n = limit;
    assert(output_size + n <= sizeof output);
    memcpy(output + output_size, p, n); output_size += (unsigned)n;
    return (int)n;
}
static uint32_t u32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
static eeg_sample_t sample(uint32_t seq) {
    eeg_sample_t v = {.timestamp_us=123456789, .sequence=seq, .stream_id=0x12345678,
        .mclk_hz=2048000, .nominal_rate=250, .gain=1, .mode=0, .mclk_assumed=1};
    v.raw[0] = 0xc0;
    const uint32_t ch[8] = {0, 1, 0x7fffff, 0x800000, 0xffffff, 0xfffffe, 0x123456, 0xfedcba};
    for (unsigned i=0;i<8;i++) for(unsigned b=0;b<3;b++) v.raw[3+i*3+b]=(uint8_t)(ch[i]>>(16-8*b));
    return v;
}
static void drain(void) { while (s.ready) assert(eeg_stream_pump(&s, sink, NULL) > 0); }
#ifdef _WIN32
static DWORD WINAPI producer(LPVOID arg) {
    (void)arg;
    for(uint32_t i=0;i<200000;i++) {
        eeg_sample_t *slot;
        while(!(slot=eeg_queue_reserve(&q))) SwitchToThread();
        *slot=sample(i); eeg_queue_commit(&q);
    }
    return 0;
}
#endif
int main(int argc, char **argv) {
    eeg_queue_init(&q); eeg_sample_t v=sample(0), got;
    for(unsigned i=0;i<EEG_QUEUE_CAPACITY;i++){v.sequence=i;assert(eeg_queue_push(&q,&v));}
    assert(!eeg_queue_push(&q,&v)); assert(atomic_load(&q.drops)==1);
    eeg_queue_stop(&q); uint32_t seen=0;
    assert(!eeg_queue_stop_due(&q,&seen));
    for(unsigned i=0;i<EEG_QUEUE_CAPACITY;i++){assert(eeg_queue_peek(&q,&got));assert(got.sequence==i);eeg_queue_pop(&q);}
    assert(eeg_queue_stop_due(&q,&seen)); assert(!eeg_queue_stop_due(&q,&seen));
    eeg_queue_init(&q); atomic_store(&q.head,UINT32_MAX);atomic_store(&q.tail,UINT32_MAX);
    assert(eeg_queue_push(&q,&v));assert(eeg_queue_peek(&q,&got));eeg_queue_pop(&q);
    assert(!eeg_queue_peek(&q,&got));assert(atomic_load(&q.tail)==0);
    /* Coalesced stops do not lose the final boundary, even with new generations. */
    eeg_queue_stop(&q); assert(eeg_queue_push(&q,&v)); eeg_queue_stop(&q); seen=0;
    assert(!eeg_queue_stop_due(&q,&seen));assert(eeg_queue_discard(&q)==1);assert(eeg_queue_stop_due(&q,&seen));
    /* Batched borrow cannot cross STOP or ring wrap, and reserve/commit publish once. */
    eeg_queue_init(&q); seen=0;
    for (unsigned i=0;i<5;i++) { eeg_sample_t *slot=eeg_queue_reserve(&q); assert(slot); *slot=sample(i); eeg_queue_commit(&q); }
    eeg_queue_stop(&q);
    v=sample(5); assert(eeg_queue_push(&q,&v));
    const eeg_sample_t *batch;
    assert(eeg_queue_read_batch(&q,&batch,64,seen)==5 && batch[4].sequence==4);
    eeg_queue_consume(&q,3);
    assert(eeg_queue_read_batch(&q,&batch,64,seen)==2 && batch[0].sequence==3);
    eeg_queue_consume(&q,2); assert(eeg_queue_stop_due(&q,&seen));
    assert(eeg_queue_read_batch(&q,&batch,64,seen)==1 && batch[0].sequence==5);
    eeg_queue_consume(&q,1);
    atomic_store(&q.head,UINT32_MAX);atomic_store(&q.tail,UINT32_MAX);
    assert(eeg_queue_push(&q,&v));assert(eeg_queue_push(&q,&v));
    assert(eeg_queue_read_batch(&q,&batch,64,seen)==1);eeg_queue_consume(&q,1);
    assert(eeg_queue_read_batch(&q,&batch,64,seen)==1);eeg_queue_consume(&q,1);
    eeg_stream_init(&s);
    for(unsigned i=0;i<36;i++){v=sample(i);assert(eeg_stream_offer(&s,&v,i*4000u));}
    assert(s.ready && s.count==0 && s.packets==1);
    assert(!memcmp(s.pending,"EEG1",4) && s.pending[6]==10 && s.pending[12]==36);
    assert(u32(s.pending+20)==0 && u32(s.pending+40)==0x12345678);
    assert(s.pending[4]==2 && u32(s.pending+1016)==0);
    if(argc>1){FILE *f=fopen(argv[1],"wb");assert(f);assert(fwrite(s.pending,1,1024,f)==1024);fclose(f);}
    uint8_t expected[1024];memcpy(expected,s.pending,1024);
    uint8_t *pending_before=s.pending;
    v=sample(36); assert(eeg_stream_offer(&s,&v,0));
    assert(s.pending==pending_before && !memcmp(expected,s.pending,1024));
    s.count=0; /* Resume original partial-packet fixture below. */
    limit=7;assert(eeg_stream_pump(&s,sink,NULL)==7);limit=0;
    assert(eeg_stream_pump(&s,sink,NULL)==0 && s.offset==7);
    limit=13;drain();assert(output_size==1024 && !memcmp(output,expected,1024));
    output_size=0;limit=1024;
    v=sample(36);assert(eeg_stream_offer(&s,&v,0));
    eeg_stream_tick(&s,199999);assert(!s.ready);eeg_stream_tick(&s,200000);assert(s.ready);
    assert(s.pending[6]==9 && s.pending[12]==1 && u32(s.pending+16)==1);
    for(unsigned i=71;i<1016;i++)assert(s.pending[i]==0);
    drain();output_size=0;
    v=sample(100);assert(eeg_stream_offer(&s,&v,0));assert(eeg_stream_flush(&s));
    assert((s.pending[6]&4)!=0);drain();output_size=0;
    v=sample(101);assert(eeg_stream_offer(&s,&v,0));v.mode=1;v.sequence=102;
    assert(eeg_stream_offer(&s,&v,1));assert(s.ready && s.count==1 && s.pending[39]==0);
    /* Backpressure preserves the caller's unaccepted new sample. */
    v.sequence=200;assert(!eeg_stream_offer(&s,&v,2));drain();output_size=0;
    assert(eeg_stream_offer(&s,&v,2));drain();output_size=0;
    eeg_stream_disconnect(&s);assert(s.count==0 && !s.ready && s.dropped_samples==1);
    v=sample(UINT32_MAX);v.stream_id++;assert(eeg_stream_offer(&s,&v,0));v.sequence=0;
    assert(eeg_stream_offer(&s,&v,1));assert(eeg_stream_flush(&s));
    assert(s.pending[12]==2 && (s.pending[6]&2) && u32(s.pending+16)==0);
    limit=100;assert(eeg_stream_pump(&s,sink,NULL)==100);eeg_stream_disconnect(&s);
    assert(s.offset==0 && !s.ready && s.dropped_samples==3);
    s.next_packet=UINT32_MAX;v.sequence=1;assert(eeg_stream_offer(&s,&v,3));assert(eeg_stream_flush(&s));
    assert(u32(s.pending+16)==UINT32_MAX && s.next_packet==0);
    /* Repeated ping-pong ownership under partial writes: next assembly must
     * never modify the pending packet even when both buffers are full. */
    eeg_stream_init(&s); output_size=0;
    for(unsigned i=0;i<36;i++) { v=sample(i); assert(eeg_stream_offer(&s,&v,i)); }
    memcpy(expected,s.pending,1024);
    for(unsigned i=36;i<72;i++) { v=sample(i); assert(eeg_stream_offer(&s,&v,i)); }
    assert(s.ready && s.count==36 && !memcmp(expected,s.pending,1024));
    v=sample(72); assert(!eeg_stream_offer(&s,&v,72));
    limit=31; drain(); assert(!memcmp(output,expected,1024)); output_size=0;
    assert(eeg_stream_flush(&s)); assert(u32(s.pending+20)==36);
    memcpy(expected,s.pending,1024); drain(); assert(!memcmp(output,expected,1024)); output_size=0;
    const uint8_t options[]={0,0,53,1,3,50,4,192,168,4,16,255};
    assert(dhcp_find_option(options,sizeof options,53)==options+2);
    assert(!dhcp_find_option(options,9,50));assert(!dhcp_find_option(options,3,53));
#ifdef _WIN32
    eeg_queue_init(&q);
    HANDLE thread=CreateThread(NULL,0,producer,NULL,0,NULL);assert(thread);
    for(uint32_t i=0;i<200000;) {
        const eeg_sample_t *items; unsigned n=eeg_queue_read_batch(&q,&items,37,0);
        if (!n) { SwitchToThread(); continue; }
        for(unsigned j=0;j<n;j++) { assert(items[j].sequence==i+j); assert(items[j].raw[0]==0xc0); }
        eeg_queue_consume(&q,n); i+=n;
    }
    assert(WaitForSingleObject(thread,10000)==WAIT_OBJECT_0);CloseHandle(thread);
#endif
    printf("PASS stream/no-CRC/queue/stop/backpressure/disconnect; sample=%u queue=%u stream=%u bytes\n",
        (unsigned)sizeof(eeg_sample_t),(unsigned)sizeof(eeg_queue_t),(unsigned)sizeof(eeg_stream_t));
    return 0;
}
