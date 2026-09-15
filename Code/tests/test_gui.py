"""Offscreen GUI integration, with explicit simulation and screenshot output."""
import os
os.environ.setdefault('QT_QPA_PLATFORM', 'offscreen')
import sys
import time
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from PySide6.QtWidgets import QApplication, QLabel
from PySide6.QtGui import QFontDatabase
from main import Window
from simulator import Simulator, make_packet
from protocol import StreamDecoder

app = QApplication.instance() or QApplication([])
for font in ('msyh.ttc', 'segoeui.ttf'):
    path = Path(os.environ.get('WINDIR', 'C:/Windows')) / 'Fonts' / font
    if path.exists(): QFontDatabase.addApplicationFont(str(path))


def pump(predicate, timeout=4):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        app.processEvents()
        if predicate(): return
        time.sleep(.01)
    raise AssertionError('GUI condition timed out')


class GuiTests(unittest.TestCase):
    def create_window(self, sim):
        w = Window()
        w.host.setText('127.0.0.1'); w.data_port.setValue(sim.data_port); w.control_port.setValue(sim.control_port)
        w.show(); w.connect_device()
        pump(lambda: w.state == '待机' and not w.busy())
        return w

    def finish(self, w, sim):
        w.close()
        pump(lambda: not w.receiver.is_alive() and not w.busy(), timeout=6)
        app.processEvents(); sim.stop()

    def test_dashboard_with_simulator(self):
        sim = Simulator(data_port=0, control_port=0).start()
        w = self.create_window(sim)
        try:
            w.findChildren(QLabel)[0].setText('PICO 2 W   /   本地模拟验证')
            self.assertEqual(w.plot.count, 0); self.assertFalse(sim.running)
            self.assertEqual(w.display_button.text(), '开始显示')
            w.rate.setCurrentText('1000'); w.send_control(1, 1000)
            pump(lambda: not w.busy() and sim.rate == 1000)
            w.gain.setCurrentText('24'); w.send_control(2, 24)
            pump(lambda: not w.busy() and sim.gain == 24)
            for mode in (2, 3, 1, 0):
                w.mode.setCurrentIndex(w.mode.findData(mode)); w.send_control(3, mode)
                pump(lambda: not w.busy() and sim.mode == mode)
                self.assertEqual(w.state, '待机'); self.assertFalse(sim.running)
                w.toggle_display()
                pump(lambda: w.state == '采集中' and w.plot.count > 50 and not w.busy())
                self.assertEqual(w.latest.mode, mode)
                w.refresh(); self.assertFalse(w.rate.isEnabled()); self.assertFalse(w.mode.isEnabled())
                w.send_control(1, 250); self.assertEqual(sim.rate, 1000)
                w.toggle_display(); pump(lambda: w.state == '待机' and not w.busy())
                self.assertFalse(sim.running)
                count = w.plot.count
                time.sleep(.08); app.processEvents(); self.assertEqual(w.plot.count, count)
            w.toggle_display(); pump(lambda: w.plot.count > 3000, timeout=5)
            w.amplitude.setValue(500000)
            w.resize(1200, 800); app.processEvents()
            self.assertTrue(w.grab().save(str(Path(os.environ.get('EEG_TEST_SCREENSHOT', str(Path(__file__).resolve().parents[1] / 'ui-simulation.png'))))))
            w.disconnect_device(); pump(lambda: not w.receiver.is_alive())
            self.assertFalse(sim.running)
            w.connect_device(); pump(lambda: w.state == '待机' and not w.busy())
            self.assertFalse(sim.running); self.assertEqual(w.plot.count, 0)
            self.assertEqual(w.receiver.tracking.packet_gaps, 0)
        finally:
            self.finish(w, sim)

    def test_failure_and_unknown_start_reconciliation(self):
        from unittest.mock import patch
        from network import control as real_control
        sim = Simulator(data_port=0, control_port=0).start()
        w = self.create_window(sim)
        try:
            sim.fail_next = (3, 5); w.send_control(3, 3)
            pump(lambda: w.state == '故障' and not w.busy())
            self.assertFalse(sim.running); self.assertFalse(w.displaying)
            w.send_control(0, 0); pump(lambda: w.state == '待机' and not w.busy())
            def lose_start_reply(*args, **kwargs):
                reply = real_control(*args, **kwargs)
                if args[3] == 4: raise TimeoutError('injected lost START reply')
                return reply
            with patch('main.control', side_effect=lose_start_reply):
                w.toggle_display(); pump(lambda: w.state == '采集中' and w.plot.count > 36)
            self.assertTrue(sim.running)
            w.toggle_display(); pump(lambda: w.state == '待机' and not w.busy())
        finally:
            self.finish(w, sim)

    def test_unexpected_disconnect_and_close(self):
        sim = Simulator(data_port=0, control_port=0, disconnect_after=2).start()
        w = self.create_window(sim)
        try:
            w.toggle_display(); pump(lambda: w.state == '故障' and not w.receiver.is_alive())
            self.assertFalse(w.displaying); self.assertFalse(sim.running)
            sim.disconnect_after = 0
            w.connect_device(); pump(lambda: w.state == '待机' and not w.busy())
            self.assertEqual(w.plot.count, 0)
            w.toggle_display(); pump(lambda: w.state == '采集中' and not w.busy())
        finally:
            self.finish(w, sim)
            self.assertFalse(sim.running)

    def test_ring_and_configuration_gap(self):
        w = Window()
        try:
            w.plot.capacity = 72
            w.plot.data = w.plot.data[:72]
            decoder = StreamDecoder()
            for seq in range(4):
                w.plot.append(decoder.feed(make_packet(sequence=seq, first=seq*36, timestamp=seq*144000))[0])
            self.assertEqual(w.plot.count, 72)
            self.assertEqual(len(w.plot.visible()), 72)
            w.plot.append(decoder.feed(make_packet(sequence=6, first=216, timestamp=864000))[0])
            self.assertEqual(w.plot.count, 36)
            self.assertEqual(w.plot.discontinuities, 1)
        finally:
            w.close()

    def test_late_start_reply_after_data_disconnect(self):
        sim = Simulator(data_port=0, control_port=0, disconnect_after=1).start()
        w = self.create_window(sim)
        try:
            sim.reply_delay = .3
            w.toggle_display()
            pump(lambda: not w.receiver.is_alive() and not w.busy())
            self.assertEqual(w.state, '故障')
            self.assertFalse(w.displaying)
            self.assertEqual(w.plot.count, 0)
            self.assertFalse(sim.running)
        finally:
            self.finish(w, sim)

    def test_no_periodic_queries_and_manual_query_works(self):
        from unittest.mock import patch
        from network import control as real_control
        sim = Simulator(data_port=0, control_port=0).start()
        w = self.create_window(sim)
        try:
            with patch('main.control', wraps=real_control) as requests:
                w.next_query = 0
                until = time.monotonic() + 2.3
                pump(lambda: time.monotonic() >= until)
                self.assertEqual(requests.call_count, 0)
                w.query_button.click()
                pump(lambda: requests.call_count == 1 and not w.busy())
                self.assertEqual(requests.call_args.args[3], 0)
                w.toggle_display(); pump(lambda: w.state == '采集中' and not w.busy())
                calls = requests.call_count
                until = time.monotonic() + 2.3
                pump(lambda: time.monotonic() >= until)
                self.assertEqual(requests.call_count, calls)
        finally:
            self.finish(w, sim)

    def test_failed_background_poll_cancels_queued_configuration(self):
        from unittest.mock import patch
        sim = Simulator(data_port=0, control_port=0).start()
        w = self.create_window(sim)
        try:
            def unavailable(*args, **kwargs):
                time.sleep(.15)
                raise TimeoutError('injected background timeout')
            with patch('main.control', side_effect=unavailable):
                w.send_control(0, 0, background=True)
                pump(lambda: w.busy() and w.background_request)
                w.send_control(1, 1000)
                pump(lambda: w.state == '故障' and not w.busy())
            self.assertIsNone(w.pending_action)
            self.assertEqual(sim.rate, 250)
            self.assertIn('已取消', w.control_label.text())
        finally:
            self.finish(w, sim)


if __name__ == '__main__': unittest.main()
