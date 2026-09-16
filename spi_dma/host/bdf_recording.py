"""Bounded, independent BDF+ writer. Never record from the display queue.

Each continuous/configuration-homogeneous span is its own BDF+ file. A JSONL
manifest records exact raw sample counts, sequence/time anchors and padding.
"""
from datetime import datetime
import json
from pathlib import Path
import queue
import threading
import numpy as np
import pyedflib
from signal_tools import digital_samples


class BdfRecorder(threading.Thread):
    def __init__(self, path):
        super().__init__(name='usb-bdf', daemon=True)
        self.path = Path(path).with_suffix('.bdf')
        if self.path.exists(): raise FileExistsError(self.path)
        self.manifest = open(str(self.path)+'.jsonl', 'x', encoding='utf-8')
        self.queue = queue.Queue(2048)
        self.done = threading.Event()
        self.error = ''
        self.dropped = self.samples = self.parts = self.markers = 0
        self.accepted = 0
        self.writer = None
        self.last = None
        self.buffer = None
        self.used = self.segment_samples = 0
        self.event_second = None
        self.event_count = 0
        self.start()

    def offer(self, msg):
        if self.error or self.done.is_set():
            self.dropped += msg.count
            return
        try:
            self.queue.put_nowait(('data', msg))
            self.accepted += msg.count
        except queue.Full:
            self.dropped += msg.count
            self.error = 'BDF 队列已满，录制停止；USB 接收继续'
            self.done.set()

    def marker(self, anchor, text):
        if self.error or self.done.is_set(): raise RuntimeError(self.error or 'BDF 已停止')
        if not self.accepted: raise RuntimeError('BDF 尚未接收到样本，触发未接受')
        try: self.queue.put_nowait(('marker', (anchor, text)))
        except queue.Full: raise RuntimeError('BDF 事件队列已满，触发未保存')

    def status(self):
        return dict(bdf_active=not self.done.is_set(), bdf_samples=self.samples,
                    bdf_dropped_samples=self.dropped, bdf_error=self.error,
                    bdf_parts=self.parts, bdf_markers=self.markers, bdf_path=str(self.path))

    def log(self, **item):
        self.manifest.write(json.dumps(item, ensure_ascii=False)+'\n')
        self.manifest.flush()

    def begin(self, msg, reason):
        seq, stamp, mclk, rate, gain, mode, count, _ = msg.meta
        fs = rate*mclk/2048000
        if fs != int(fs): raise ValueError('BDF 当前仅支持整数实际采样率')
        self.fs = int(fs)
        self.parts += 1
        path = self.path if self.parts == 1 else self.path.with_name(
            f'{self.path.stem}.part{self.parts:04d}.bdf')
        # Reserve the exact new output; pyedflib itself opens with truncate mode.
        with open(path, 'xb'): pass
        self.writer = pyedflib.EdfWriter(str(path), 8, pyedflib.FILETYPE_BDFPLUS)
        self.writer.set_number_of_annotation_signals(64)
        # BDF physical fields have only eight ASCII characters; digital values
        # are exact, while the JSONL retains the exact nominal conversion ratio.
        ratio = 4500000/(gain*8388608)
        self.writer.setSignalHeaders([dict(label=f'CH{i+1}', dimension='uV',
            sample_frequency=self.fs, physical_min=round(-8388608*ratio),
            physical_max=round(8388607*ratio), digital_min=-8388608,
            digital_max=8388607, prefilter='None', transducer='ADS1299') for i in range(8)])
        self.writer.setEquipment('ADS1299_USB')
        self.writer.setStartdatetime(datetime.now())
        self.writer.writeAnnotation(0, -1, f'START {reason}')
        self.start_seq, self.start_stamp = seq, stamp
        self.segment_samples = self.used = 0
        self.event_second, self.event_count = None, 0
        self.buffer = np.empty((8, self.fs), dtype=np.int32)
        self.log(type='segment_start', part=self.parts, file=str(path), reason=reason,
                 generation=msg.generation, sequence=seq, adc_timestamp_us=stamp,
                 rate=self.fs, gain=gain, mode=mode, nominal_uV_per_code=ratio,
                 start_wall_clock=datetime.now().isoformat())

    def finish(self, reason):
        if self.writer is None: return
        padding = (self.fs-self.used) if self.used else 0
        try:
            if self.used:
                self.buffer[:, self.used:] = 0
                self.writer.writeAnnotation(self.segment_samples/self.fs, padding/self.fs,
                                            'INVALID_PADDING')
                self.writer.writeSamples(self.buffer, digital=True)
            self.writer.writeAnnotation(self.segment_samples/self.fs, -1, f'END {reason}')
        finally:
            self.writer.close()
            self.writer = None
        self.log(type='segment_end', part=self.parts, valid_samples=self.segment_samples,
                 padding_samples=padding, reason=reason)

    def consume(self, msg):
        reason = 'record_start'
        if self.last:
            delta = (msg.meta[0]-self.last.meta[0]-self.last.count) & 0xffffffff
            if msg.generation != self.last.generation: reason = 'generation_change'
            elif msg.meta[2:6] != self.last.meta[2:6]: reason = 'configuration_change'
            elif delta: reason = f'sequence_gap_{delta}' if delta < 0x80000000 else 'reorder'
            else: reason = ''
        if reason:
            self.finish(reason)
            self.begin(msg, reason)
        values = digital_samples(msg).T
        pos = 0
        while pos < msg.count:
            n = min(msg.count-pos, self.fs-self.used)
            self.buffer[:, self.used:self.used+n] = values[:, pos:pos+n]
            self.used += n; pos += n; self.segment_samples += n; self.samples += n
            if self.used == self.fs:
                self.writer.writeSamples(self.buffer, digital=True)
                self.used = 0
        self.last = msg

    def consume_marker(self, anchor, text):
        if self.last is None or self.writer is None: raise RuntimeError('触发前没有录制样本')
        # Queue ordering guarantees this marker follows exactly the anchored packet.
        onset = (self.segment_samples-1)/self.fs
        second = int(onset)
        if second != self.event_second: self.event_second, self.event_count = second, 0
        self.event_count += 1
        if self.event_count > 50: raise RuntimeError('每秒触发超过 50 次，BDF 录制停止')
        if self.writer.writeAnnotation(onset, -1, text) < 0: raise OSError('BDF annotation rejected')
        self.markers += 1
        self.log(type='marker', part=self.parts, onset=onset, text=text, **anchor)

    def run(self):
        pending = None
        before = 0
        try:
            while not self.done.is_set() or not self.queue.empty():
                try: kind, item = self.queue.get(timeout=.05)
                except queue.Empty: continue
                pending, before = (item if kind == 'data' else None), self.samples
                if kind == 'data': self.consume(item)
                else: self.consume_marker(*item)
                pending = None
        except Exception as exc:
            self.error = str(exc)
            if pending is not None: self.dropped += pending.count-(self.samples-before)
            while True:
                try:
                    kind, item = self.queue.get_nowait()
                    if kind == 'data': self.dropped += item.count
                except queue.Empty: break
        finally:
            self.done.set()
            try:
                self.finish('error' if self.error else 'record_stop')
                self.log(type='record_end', **self.status())
            except Exception as exc: self.error = str(exc)
            self.manifest.close()

    def close(self):
        self.done.set()
        self.join(5)
        if self.is_alive(): self.error = 'BDF 关闭超时，文件可能不完整'
        return self.status()
