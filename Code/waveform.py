"""Eight raw-code lanes, bounded ring and pixel min/max envelopes."""
import numpy as np
from PySide6.QtCore import QPointF, QRectF
from PySide6.QtGui import QColor, QPainter, QPen, QPolygonF
from PySide6.QtWidgets import QWidget


class Waveform(QWidget):
    def __init__(self):
        super().__init__()
        self.setMinimumSize(650, 400)
        self.capacity = 320000
        self.data = np.empty((self.capacity, 9), dtype=np.float64)
        self.position = self.count = 0
        self.seconds, self.amplitude = 5., 1000000.
        self.previous = None
        self.discontinuities = 0

    def clear(self):
        self.position = self.count = 0
        self.previous = None
        self.update()

    def append(self, packet):
        prev = self.previous
        if prev and (packet.stream_id != prev.stream_id or
                     (packet.nominal_rate, packet.mclk_hz, packet.gain, packet.mode) !=
                     (prev.nominal_rate, prev.mclk_hz, prev.gain, prev.mode) or
                     packet.first_sequence != (prev.first_sequence + prev.count) & 0xffffffff or
                     packet.packet_sequence != (prev.packet_sequence + 1) & 0xffffffff or
                     packet.first_timestamp_us <= prev.first_timestamp_us):
            self.clear()  # Never join across gaps, restarts, or configuration changes.
            self.discontinuities += 1
        self.previous = packet
        rows = np.array([(ts / 1e6, *channels) for _, ts, _, _, channels in packet.samples()])
        size = len(rows)
        first = min(size, self.capacity - self.position)
        self.data[self.position:self.position + first] = rows[:first]
        if first < size:
            self.data[:size - first] = rows[first:]
        self.position = (self.position + size) % self.capacity
        self.count = min(self.capacity, self.count + size)

    def visible(self):
        if not self.count:
            return self.data[:0]
        # Only copy the requested time window, at most 10 seconds at ADC rates.
        rate = self.previous.sample_rate
        size = min(self.count, int(self.seconds * rate) + 2)
        start = (self.position - size) % self.capacity
        rows = (self.data[start:start + size] if start + size <= self.capacity
                else np.concatenate((self.data[start:], self.data[:self.position])))
        return rows[rows[:, 0] >= rows[-1, 0] - self.seconds]

    def paintEvent(self, event):
        p = QPainter(self)
        p.fillRect(self.rect(), QColor('#071b29'))
        rect = QRectF(66, 20, max(1, self.width() - 90), max(1, self.height() - 54))
        lane = rect.height() / 8
        for tick in range(11):
            x = rect.left() + rect.width() * tick / 10
            p.setPen(QColor('#163647'))
            p.drawLine(QPointF(x, rect.top()), QPointF(x, rect.bottom()))
            p.setPen(QColor('#8aaebe'))
            p.drawText(QRectF(x - 23, rect.bottom() + 8, 50, 20), f'{-self.seconds + self.seconds*tick/10:g}s')
        rows = self.visible()
        for channel in range(8):
            y = rect.top() + lane * (channel + .5)
            p.setPen(QColor('#214457'))
            p.drawLine(QPointF(rect.left(), y), QPointF(rect.right(), y))
            p.setPen(QColor('#a7c8d8'))
            p.drawText(QRectF(8, y - 10, 54, 20), f'CH{channel + 1:02}')
            if len(rows) < 2:
                continue
            p.save()
            p.setClipRect(QRectF(rect.left(), y - lane/2 + 1, rect.width(), lane - 2))
            p.setPen(QPen(QColor('#4de0c0' if channel % 2 == 0 else '#60baff'), 1))
            values = rows[:, channel + 1]
            x = rect.right() + (rows[:, 0] - rows[-1, 0]) / self.seconds * rect.width()
            # Min/max retains narrow peaks at high sample rates; no stride-only sampling.
            bins = max(1, int(rect.width()))
            if len(rows) > bins * 2:
                step = int(np.ceil(len(rows) / bins))
                starts = np.arange(0, len(rows), step)
                low = np.minimum.reduceat(values, starts)
                high = np.maximum.reduceat(values, starts)
                for xx, lo, hi in zip(x[starts], low, high):
                    p.drawLine(QPointF(float(xx), y - float(lo) / self.amplitude * lane * .45),
                               QPointF(float(xx), y - float(hi) / self.amplitude * lane * .45))
            else:
                yy = y - values / self.amplitude * lane * .45
                p.drawPolyline(QPolygonF([QPointF(float(a), float(b)) for a, b in zip(x, yy)]))
            p.restore()
        if not len(rows):
            p.setPen(QColor('#7ca3b6'))
            p.drawText(rect, 0x84, '等待有效的八通道数据')
        p.end()
