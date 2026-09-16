import os
os.environ.setdefault('QT_QPA_PLATFORM','offscreen')
from pathlib import Path
import sys
import unittest
import numpy as np
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'host'))
from PySide6.QtWidgets import QApplication
from usb_gui import Plot,Window
from sweep_plot import peak_path
from usb_protocol import Decoder
from test_usb_host import data_packet
app=QApplication.instance() or QApplication([])

class GuiTests(unittest.TestCase):
    def test_filter_resets_at_real_gap_and_marker_is_bounded(self):
        from signal_tools import DisplayFilter, digital_samples
        p=Plot();p.set_filter(1,30,50);d=Decoder()
        first=d.feed(data_packet())[0];last=d.feed(data_packet(100,2))[0]
        p.append(first);p.append(last);p.flush()
        expected=DisplayFilter(16000).apply(digital_samples(last)*4500000/8388608)
        np.testing.assert_allclose(p.data[:,100:136].T,expected,rtol=1e-6,atol=1e-6)
        self.assertTrue(p.add_marker(dict(generation=1,sequence=110),'E1'))
        self.assertEqual(p.markers[0][0],110)
        rgba=np.asarray(p.buffer_rgba()).astype(int)
        self.assertTrue(((rgba[:,:,0]>rgba[:,:,1]+60)&(rgba[:,:,0]>rgba[:,:,2]+60)).any())
        self.assertFalse(p.add_marker(dict(generation=2,sequence=110),'future'))
        p.feed(np.zeros((p.display_length,8)));p.flush()
        self.assertFalse(p.markers);p.close()

    def test_incremental_batch_seams_are_visible_lines(self):
        # Inspect the incremental raster, not draw(): a full redraw hides this bug.
        for batch in (1, 2, 13, 36):
            with self.subTest(batch=batch):
                p=Plot(fs=250);p.resize(1200,650);p.show();app.processEvents()
                p.set_time_scale(1);p.set_voltage_scale(2)
                for _ in range(3):
                    p.feed(np.zeros((batch,8)));p.flush()
                rgba=np.asarray(p.buffer_rgba())
                x,y=p.axes.transData.transform((batch-.5,2))
                row=rgba.shape[0]-1-round(y);col=round(x)
                self.assertLess(rgba[row-2:row+3,col,:3].min(),220,
                                'Missing line across two contiguous display batches')
                p.close()

    def test_incremental_overlap_preserves_real_boundaries(self):
        p=Plot(fs=250);p.set_time_scale(1)
        p.feed(np.zeros((13,8)));p.flush()
        p.feed(np.ones((13,8)),boundary=True);p.flush()
        y=p.curves[0].get_ydata()
        self.assertTrue(np.isnan(y).any())
        p.feed(np.full((2,8),np.nan));p.flush()
        p.feed(np.zeros((13,8)));p.flush()
        self.assertTrue(np.isnan(p.curves[0].get_ydata()[0]))
        p.feed(np.zeros((250,8)));p.flush()
        self.assertTrue(np.all(np.diff(p.curves[0].get_xdata())>=0))
        p.close()

    def test_wrap_retains_previous_page_and_stop_freezes(self):
        p=Plot(fs=250);p.set_time_scale(1)
        p.feed(np.ones((250,8))*10)
        p.feed(np.ones((50,8))*20);p.flush()
        self.assertEqual(p.position,50)
        np.testing.assert_array_equal(p.data[:,:50],20)
        np.testing.assert_array_equal(p.data[:,50:],10)
        old=p.data.copy();p.draw();p.flush()
        np.testing.assert_array_equal(p.data,old)
        p.close()

    def test_gaps_and_generation_keep_history(self):
        p=Plot();d=Decoder()
        p.append(d.feed(data_packet())[0])
        p.append(d.feed(data_packet(100,2))[0])
        self.assertEqual(p.position,136);self.assertEqual(p.breaks,1)
        self.assertTrue(np.isnan(p.data[:,36:100]).all())
        self.assertTrue(np.isfinite(p.data[:,:36]).all())
        p.append(d.feed(data_packet(0,0,2))[0])
        self.assertEqual(p.generation_changes,1);self.assertEqual(p.position,172)
        self.assertTrue(p.boundary[136]);p.close()

    def test_packet_splits_have_identical_sampling_positions(self):
        wire=data_packet()+data_packet(36,1)
        p,q=Plot(),Plot();d,e=Decoder(),Decoder()
        for m in d.feed(wire):p.append(m)
        for byte in wire:
            for m in e.feed(bytes([byte])):q.append(m)
        np.testing.assert_array_equal(p.data,q.data)
        self.assertEqual(p.position,q.position);p.close();q.close()

    def test_peak_reduction_keeps_spikes_and_gaps(self):
        x=np.arange(10000,dtype=float);y=np.zeros(10000)
        y[18]=999;y[19]=-888;y[5000:5010]=np.nan
        rx,ry=peak_path(x,y,.01)
        self.assertIn(999,ry);self.assertIn(-888,ry)
        self.assertTrue(np.isnan(ry).any());self.assertLess(len(rx),500)
        self.assertTrue(np.all(np.diff(rx)>=0))

    def test_scale_history_and_cross_channel_clipping(self):
        p=Plot(fs=250);p.resize(1000,600);p.show();app.processEvents()
        p.feed(np.full((100,8),250.0));p.flush();p.draw()
        before=p.data.copy()
        # CH1 baseline=100, data=250 -> 350, beyond CH1's 0..200 lane.
        self.assertEqual(p.curves[0].get_ydata()[0],350)
        self.assertTrue(p.curves[0].get_clip_on())
        self.assertEqual(p.axes.get_ylim()[0],0)
        p.set_voltage_scale(300)
        np.testing.assert_array_equal(p.data,before)
        self.assertEqual(p.curves[0].get_ydata()[0],550)
        p.set_time_scale(2);self.assertEqual(p.position,0)
        self.assertTrue(np.isnan(p.data).all());p.close()

    def test_ui_renders_reference_controls(self):
        w=Window();w.show();app.processEvents()
        self.assertEqual(w.time_page.currentData(),5)
        self.assertEqual(w.amplitude.currentData(),100)
        self.assertTrue(w.grab().save(str(ROOT/'build/usb-ui-test.png')))
        w.close();app.processEvents()

if __name__=='__main__':unittest.main()
