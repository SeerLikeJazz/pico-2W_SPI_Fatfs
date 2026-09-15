import socket
import struct
import sys
import threading
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from network import Receiver, control, REQUEST, REPLY
from protocol import StreamDecoder, Continuity
from simulator import Simulator, make_packet


def until(predicate, timeout=3):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate(): return
        time.sleep(.01)
    raise AssertionError('timed out')


class ProtocolTests(unittest.TestCase):
    def test_firmware_reference_and_all_splits(self):
        path = Path(__file__).resolve().parents[2] / 'spi_dma/docs/protocol_v1_example.hex'
        raw = bytes.fromhex(' '.join(line.split(':', 1)[1] for line in path.read_text().splitlines() if ':' in line))
        self.assertEqual(len(raw), 1024)
        for split in range(1025):
            decoder = StreamDecoder()
            packets = decoder.feed(raw[:split]) + decoder.feed(raw[split:])
            self.assertEqual(len(packets), 1)
        self.assertEqual(next(packets[0].samples())[4], (0, 1, 8388607, -8388608, -1, -2, 1193046, -74566))

    def test_corruption_sticky_and_bounded_garbage(self):
        raw = make_packet()
        for offset in (4, 8, 10, 12, 14, 1016, 1020):
            corrupt = bytearray(raw); corrupt[offset] ^= 128
            decoder = StreamDecoder()
            self.assertEqual(len(decoder.feed(b'junk' + corrupt + raw + raw)), 2)
        decoder.feed(b'x' * 1000000)
        self.assertLessEqual(len(decoder.buffer), 3)

    def test_padding_and_wrap(self):
        raw = bytearray(make_packet(count=1)); raw[80] = 1
        decoder = StreamDecoder()
        self.assertFalse(decoder.feed(raw)); self.assertEqual(decoder.padding_errors, 1)
        tracking = Continuity()
        for packet in StreamDecoder().feed(make_packet(sequence=0xffffffff, first=0xffffffdc) + make_packet()):
            tracking.accept(packet)
        self.assertEqual((tracking.packet_gaps, tracking.sample_gaps, tracking.reorders), (0, 0, 0))


class NetworkTests(unittest.TestCase):
    def setUp(self):
        self.sim = Simulator(data_port=0, control_port=0).start()
        self.receivers = []
        self.token = 0

    def tearDown(self):
        for r in self.receivers: r.stop(); r.join(4)
        self.sim.stop()

    def receiver(self, **kw):
        r = Receiver('127.0.0.1', self.sim.data_port, **kw)
        self.receivers.append(r); r.start()
        until(lambda: r.connected and self.sim.connected)
        self.token = self.call().session
        return r

    def call(self, op=0, value=0, **kw):
        return control('127.0.0.1', self.sim.control_port, 13, op, value, session=self.token, **kw)

    def test_connect_standby_and_all_modes(self):
        r = self.receiver()
        time.sleep(.2)
        self.assertFalse(self.sim.running)
        self.assertEqual(r.tracking.samples, 0)
        self.assertFalse(r.snapshot()[0])
        for mode in (2, 3, 1, 0):
            configured = self.call(3, mode)
            self.assertFalse(configured.running)
            self.assertEqual(configured.mode, mode)
            start = self.call(4)
            self.assertTrue(start.running)
            until(lambda: any(p.mode == mode and p.stream_id == start.stream_id for p in r.snapshot()[0]))
            stop = self.call(5)
            self.assertFalse(stop.running)

    def test_control_and_data_agree(self):
        r = self.receiver()
        a = self.call(1, 1000); b = self.call(2, 24); c = self.call()
        self.assertEqual((a.result, b.result, c.rate, c.gain, c.running), (2, 2, 1000, 24, 0))
        self.call(4)
        until(lambda: any(p.nominal_rate == 1000 and p.gain == 24 for p in r.snapshot()[0]))
        for op, value in ((1, 250), (2, 1), (3, 2)):
            reply = self.call(op, value)
            self.assertEqual((reply.result, reply.error, reply.running), (0, 2, 1))
        self.call(5)
        self.assertEqual(self.call(2, 24).result, 1)
        with self.assertRaises(ValueError): self.call(1, 123)

    def test_disconnect_and_reconnect_standby(self):
        self.sim.disconnect_after = 2
        r = self.receiver(); old_token = self.token
        self.call(4); r.join(3)
        self.assertFalse(r.is_alive()); self.assertIn('关闭', r.status)
        until(lambda: not self.sim.running)
        second = self.receiver()
        self.assertFalse(self.sim.running)
        self.assertEqual(second.tracking.samples, 0)
        reply = control('127.0.0.1', self.sim.control_port, 99, 4, session=old_token)
        self.assertEqual(reply.result, 0)
        self.assertFalse(self.sim.running)

    def test_heartbeat_loss_stops_device(self):
        self.sim.heartbeat_timeout = .15
        with socket.create_connection(('127.0.0.1', self.sim.data_port)):
            until(lambda: self.sim.connected)
            self.token = self.call().session
            self.call(4)
            until(lambda: not self.sim.running)
            self.assertFalse(self.sim.connected)

    def test_queue_bound_and_cancel_stops_device(self):
        self.sim.rate = 16000
        r = self.receiver(capacity=2); self.call(4)
        until(lambda: r.display_drops > 0)
        with r.lock: self.assertLessEqual(len(r.pending), 2)
        r.stop(); r.join(1)
        self.assertFalse(r.is_alive())
        until(lambda: not self.sim.running)

    def test_idle_timeout_only_when_running(self):
        self.sim.paused = True
        r = self.receiver(idle_timeout=.03)
        time.sleep(.3)
        self.assertNotIn('数据超时', r.snapshot()[1])
        self.call(4); r.expecting_data.set()
        until(lambda: '数据超时' in r.snapshot()[1])
        self.assertTrue(r.connected)

    def test_refused_connection(self):
        with socket.socket() as temp:
            temp.bind(('127.0.0.1', 0)); port = temp.getsockname()[1]
        r = Receiver('127.0.0.1', port); r.start(); r.join(4)
        self.assertFalse(r.connected); self.assertIn('失败', r.status)

    def test_start_timeout_reconciles_by_query(self):
        self.receiver()
        self.sim.reply_delay = .2
        with self.assertRaises(TimeoutError): self.call(4, timeout=.03)
        self.assertTrue(self.sim.running)
        self.sim.reply_delay = 0
        time.sleep(.25)
        self.assertTrue(self.call().running)
        self.call(5)
        self.assertFalse(self.call().running)

    def test_failed_configuration_keeps_standby(self):
        self.receiver(); self.sim.fail_next = (3, 5)
        reply = self.call(3, 3)
        self.assertEqual((reply.result, reply.error, reply.mode, reply.running), (0, 5, 0, 0))

    def test_bad_and_legacy_replies(self):
        responses = [
            REPLY.pack(b'WFR2', 999, 1, 0, 250, 1, 0, 1, 2048000, 0, 1, 1),
            REPLY.pack(b'WFR2', 1, 2, 0, 250, 1, 0, 1, 2048000, 0, 1, 1),
            struct.pack('<4s9I', b'WFR1', 1, 1, 0, 250, 1, 0, 1, 2048000, 0),
        ]
        for response in responses:
            with socket.socket() as server:
                server.bind(('127.0.0.1', 0)); server.listen(1)
                def serve():
                    with server.accept()[0] as client:
                        client.recv(20); client.sendall(response)
                worker = threading.Thread(target=serve); worker.start()
                with self.assertRaises((ValueError, ConnectionError)):
                    control('127.0.0.1', server.getsockname()[1], 1, 1, 1000, session=1)
                worker.join()


if __name__ == '__main__': unittest.main()
