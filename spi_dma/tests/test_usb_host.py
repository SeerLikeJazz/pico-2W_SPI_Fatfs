from pathlib import Path
import sys
import struct
import tempfile
import threading
import time
import unittest
from collections import deque
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'host'))
from usb_protocol import Decoder, HEADER, DATA_META, Continuity, command, Message, STATUS_FIELDS, START, STOP
from usb_receiver import Receiver, Recorder


def data_packet(seq=0, packet_id=0, generation=1, count=36):
    raw=bytearray(40+27*count)
    HEADER.pack_into(raw,0,b'AUSB',1,1,len(raw),packet_id,generation)
    DATA_META.pack_into(raw,16,seq,123456789,2048000,16000,1,0,count,0)
    for i in range(count):raw[40+27*i]=0xc0
    return bytes(raw)


def status_packet(ident=1,op=0):
    words=[0]*40;words[0]=op;words[3]=1;words[4]=int(op==START);words[5]=16000;words[9]=15000000
    return HEADER.pack(b'AUSB',1,3 if op==0 else 2,176,ident,1)+struct.pack('<40I',*words)


class FakeSerial:
    def __init__(self):self.chunks=deque();self.lock=threading.Lock();self.closed=False;self.dtr=False;self.writes=[]
    def offer(self,data):
        with self.lock:self.chunks.append(data)
    @property
    def in_waiting(self):
        with self.lock:return sum(map(len,self.chunks))
    def read(self,n):
        with self.lock:
            if self.chunks:
                block=self.chunks.popleft();result=block[:n]
                if block[n:]:self.chunks.appendleft(block[n:])
                return result
        time.sleep(.001);return b''
    def write(self,data):
        self.writes.append(data);self.offer(status_packet(struct.unpack_from('<I',data,8)[0],struct.unpack_from('<I',data,16)[0]));return len(data)
    def close(self):self.closed=True


def until(fn):
    end=time.monotonic()+3
    while time.monotonic()<end:
        if fn():return
        time.sleep(.005)
    raise AssertionError('timeout')


class ProtocolTests(unittest.TestCase):
    def test_c_reference_every_split_and_mixed_reply(self):
        raw=(ROOT/'build/usb_reference.bin').read_bytes()
        for split in range(len(raw)+1):
            d=Decoder();messages=d.feed(raw[:split])+d.feed(raw[split:])
            self.assertEqual(len(messages),2)
            self.assertEqual(messages[0].count,36)
            self.assertEqual(messages[1].status['spi'],15000000)
        self.assertEqual(messages[0].generation,7)
        self.assertEqual(messages[0].meta[0],0)

    def test_invalid_headers_bounded_buffer_and_no_checksum(self):
        raw=data_packet()
        for offset in (4,5,6,7,36,38):
            corrupt=bytearray(raw);corrupt[offset]^=0x80
            self.assertEqual(len(Decoder().feed(corrupt+raw)),1)
        corrupt=bytearray(raw);corrupt[80]^=1
        self.assertEqual(len(Decoder().feed(corrupt)),1)
        d=Decoder();d.feed(b'x'*1000000);self.assertLessEqual(len(d.buffer),3)

    def test_sequence_wrap_gap_generation(self):
        d=Decoder();t=Continuity()
        for raw in (data_packet(0xffffffdc),data_packet(0,1),data_packet(40,3),data_packet(1,0,2)):
            t.accept(d.feed(raw)[0])
        self.assertEqual((t.sample_gaps,t.packet_gaps,t.generations),(4,1,2))

    def test_commands(self):
        raw=command(42,1)
        self.assertEqual(len(raw),24);self.assertEqual(HEADER.unpack_from(raw),(b'AUSB',1,16,24,42,0))


class ReceiverTests(unittest.TestCase):
    def test_display_queue_does_not_affect_receive_or_recording(self):
        fake=FakeSerial()
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/'raw.bin';r=Receiver(serial_object=fake,raw_path=path,display=True)
            raw=b''.join(data_packet(i*36,i) for i in range(300))
            try:
                for i in range(0,len(raw),65536):fake.offer(raw[i:i+65536])
                until(lambda:r.snapshot()[0]['samples']==10800)
                stats,packets=r.snapshot(take_display=True)
                self.assertEqual(stats['sample_gaps'],0)
                self.assertEqual(stats['display_dropped_samples'],172*36)
                self.assertEqual(len(packets),128)
            finally:r.close()
            self.assertEqual(path.read_bytes(),raw)
            self.assertEqual(r.recorder.dropped,0)

    def test_headless_no_display_and_only_requested_commands(self):
        fake=FakeSerial();r=Receiver(serial_object=fake)
        try:
            self.assertEqual(r.request(START)['running'],1)
            fake.offer(data_packet())
            until(lambda:r.snapshot()[0]['samples']==36)
            self.assertEqual(len(r.display),0)
            time.sleep(.05);self.assertEqual(len(fake.writes),1)
            self.assertEqual(r.request(STOP)['running'],0)
        finally:r.close()
        self.assertFalse(fake.dtr)

    def test_timeout_does_not_accept_late_reply_as_new_command(self):
        fake=FakeSerial();r=Receiver(serial_object=fake)
        real_write=fake.write
        try:
            fake.write=lambda data:len(data)
            with self.assertRaises(TimeoutError):r.request(START,timeout=.03)
            fake.offer(status_packet(1,START));fake.write=real_write
            reply=r.request(STOP)
            self.assertEqual(reply['op'],STOP)
        finally:r.close()

    def test_disk_overflow_is_counted_without_blocking(self):
        import queue
        recorder=Recorder.__new__(Recorder);recorder.queue=queue.Queue(1);recorder.dropped=0
        recorder.offer(b'first');recorder.offer(b'lost')
        self.assertEqual(recorder.dropped,4)


if __name__=='__main__':unittest.main()
