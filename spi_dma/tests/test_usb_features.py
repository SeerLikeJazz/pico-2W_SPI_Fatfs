import json
from pathlib import Path
import sys
import tempfile
import threading
import queue
import unittest
import numpy as np
import pyedflib
from scipy.signal import butter, iirnotch, lfilter, lfilter_zi
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'host'))
from signal_tools import DisplayFilter, digital_samples
from bdf_recording import BdfRecorder
from usb_protocol import Message, DATA_META
from usb_receiver import Receiver
from test_usb_host import data_packet, FakeSerial, until


def packet(seq=0, count=36, generation=1, rate=250, gain=24):
    raw=bytearray(data_packet(seq,seq//36,generation,count))
    DATA_META.pack_into(raw,16,seq,round(seq*1e6/rate),2048000,rate,gain,0,count,0)
    values=np.tile(np.array([-8388608,-3000,-1,0,1,3000,8388606,8388607]),(count,1))
    for i in range(count):
        for ch in range(8):raw[43+i*27+ch*3:46+i*27+ch*3]=int(values[i,ch]&0xffffff).to_bytes(3,'big')
    return Message(1,seq//36,generation,bytes(raw))


class FeatureTests(unittest.TestCase):
    def test_bdf_writer_failure_is_not_silent(self):
        class Broken(BdfRecorder):
            def consume(self,msg):raise OSError('simulated disk failure')
        with tempfile.TemporaryDirectory() as tmp:
            r=Broken(Path(tmp)/'failed.bdf');r.offer(packet())
            status=r.close()
            self.assertIn('disk failure',status['bdf_error'])
            self.assertEqual(status['bdf_dropped_samples'],36)

    def test_bdf_backpressure_is_bounded_and_reported(self):
        class Stalled(BdfRecorder):
            def consume(self,msg):
                entered.set();release.wait(2);super().consume(msg)
        entered,release=threading.Event(),threading.Event()
        with tempfile.TemporaryDirectory() as tmp:
            r=Stalled(Path(tmp)/'slow.bdf')
            r.offer(packet());self.assertTrue(entered.wait(2))
            r.queue=queue.Queue(1)
            try:
                r.offer(packet(36));r.offer(packet(72))
                self.assertEqual(r.dropped,36);self.assertTrue(r.error)
            finally:release.set();status=r.close()
            self.assertEqual(status['bdf_samples'],72)
            self.assertFalse(status['bdf_active'])

    def test_filter_matches_reference_and_packetization(self):
        x=np.random.default_rng(2).normal(size=(2000,8))
        expected=x.copy()
        for b,a in (iirnotch(50,30,fs=250),butter(2,1,btype='high',fs=250),butter(2,30,btype='low',fs=250)):
            expected,_=lfilter(b,a,expected,axis=0,zi=np.repeat(lfilter_zi(b,a)[:,None],8,axis=1))
        f=DisplayFilter(250)
        actual=np.concatenate([f.apply(x[i:i+13]) for i in range(0,len(x),13)])
        np.testing.assert_allclose(actual,expected,atol=1e-12)
        f.reset();np.testing.assert_allclose(f.apply(x),expected,atol=1e-12)
        np.testing.assert_array_equal(DisplayFilter(250,None,None,None).apply(x),x)
        with self.assertRaises(ValueError):DisplayFilter(250,1,200,50)

    def test_bdf_digital_exact_tail_and_annotation(self):
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/'test.bdf';r=BdfRecorder(path)
            for i in range(8):r.offer(packet(i*36))
            r.marker(dict(generation=1,sequence=287,adc_timestamp_us=1148000),'E1 测试')
            status=r.close();self.assertFalse(status['bdf_error'],status)
            with pyedflib.EdfReader(str(path)) as f:
                self.assertEqual(f.getSampleFrequency(0),250)
                for ch in range(8):
                    samples=f.readSignal(ch,digital=True)
                    np.testing.assert_array_equal(samples[:288],digital_samples(packet())[0,ch])
                    np.testing.assert_array_equal(samples[288:],0)
                starts,durations,labels=f.readAnnotations()
                self.assertIn('E1 测试',labels);self.assertIn('INVALID_PADDING',labels)
                self.assertAlmostEqual(starts[list(labels).index('E1 测试')],287/250,places=4)
            events=[json.loads(line) for line in Path(str(path)+'.jsonl').read_text(encoding='utf-8').splitlines()]
            end=next(e for e in events if e['type']=='segment_end')
            self.assertEqual((end['valid_samples'],end['padding_samples']),(288,212))
            with self.assertRaises(FileExistsError):BdfRecorder(path)

    def test_bdf_gap_generation_and_configuration_split(self):
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/'gaps.bdf';r=BdfRecorder(path)
            for msg in (packet(),packet(40),packet(0,generation=2),packet(36,generation=2,rate=500)):
                r.offer(msg)
            status=r.close();self.assertEqual(status['bdf_parts'],4);self.assertFalse(status['bdf_error'])
            events=[json.loads(line) for line in Path(str(path)+'.jsonl').read_text(encoding='utf-8').splitlines()]
            self.assertEqual([e['reason'] for e in events if e['type']=='segment_start'],
                             ['record_start','sequence_gap_4','generation_change','configuration_change'])

    def test_receiver_records_despite_display_queue_overflow(self):
        fake=FakeSerial()
        with tempfile.TemporaryDirectory() as tmp:
            r=Receiver(serial_object=fake,display=True)
            try:
                r.local('bdf_start',str(Path(tmp)/'record.bdf'))
                wire=b''.join(packet(i*36,rate=16000).raw for i in range(300))
                for i in range(0,len(wire),65536):fake.offer(wire[i:i+65536])
                until(lambda:r.snapshot()[0]['samples']==10800)
                event=r.local('marker','E1')
                self.assertEqual(event['anchor']['sequence'],10799)
                status=r.local('bdf_stop',None)
                self.assertEqual(status['bdf_samples'],10800);self.assertEqual(status['bdf_dropped_samples'],0)
                self.assertFalse(status['bdf_error']);self.assertEqual(status['bdf_markers'],1)
                self.assertGreater(r.snapshot()[0]['display_dropped_samples'],0)
            finally:r.close()


if __name__=='__main__':unittest.main()
