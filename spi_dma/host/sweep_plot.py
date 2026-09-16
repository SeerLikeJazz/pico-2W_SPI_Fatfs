"""USB sweep display based on iSensys-X-Client's active signalMat.py.

Same axes, channel offsets, sweep/erase/cursor geometry and EEG colours.
Raw USB samples remain outside this display; gaps are explicit NaNs.
"""
import os
import numpy as np
import matplotlib
from PySide6.QtCore import Signal
from matplotlib.backends.backend_qtagg import FigureCanvasQTAgg
from matplotlib.figure import Figure
from matplotlib.patches import Rectangle

VREF_UV = 4_500_000.0  # Nominal ADS1299 internal reference; not a calibration.
TIME_SCALES = (1, 2, 5, 10, 15)
VOLTAGE_SCALES = (2, 10, 20, 40, 100, 300, 700, 1000, 3000)
if os.name == 'nt':
    matplotlib.rc('font', family='DengXian')  # Active reference renderer's Windows font.


def peak_path(x, y, pixels):
    """First/min/max/last in each x pixel, ordered in time; never bridge NaNs."""
    if len(x) < 4:
        return x, y
    finite = np.isfinite(y)
    bucket = np.floor(x * pixels).astype(np.int64)
    edges = np.flatnonzero(np.r_[True, (bucket[1:] != bucket[:-1]) |
                                    (finite[1:] != finite[:-1]), True])
    if len(edges) >= len(x)//2:
        return x, y
    selected = []
    for a, b in zip(edges[:-1], edges[1:]):
        if not finite[a]:
            selected.append(a)
        else:
            selected.extend(sorted({a, b-1, a+int(np.argmin(y[a:b])),
                                    a+int(np.argmax(y[a:b]))}))
    return x[selected], y[selected]


class Plot(FigureCanvasQTAgg):
    scale_changed = Signal(float)

    def __init__(self, fs=16000, ch_num=8):
        self.fig = Figure(figsize=(2, 2), dpi=55, facecolor='#FDFEF6', edgecolor='#FDFEF6')
        super().__init__(self.fig)
        self.setMinimumHeight(400)
        self.fs, self.ch_num = float(fs), ch_num
        self.time_scale, self.voltage_scale = 5, 100.0
        self.auto_scale = False
        self.focus_channel = None
        self.axes = self.fig.add_subplot()
        self.axes.set_position([.04, .03, .95, .97])
        self.last = None
        self.breaks = self.generation_changes = self.reorders = 0
        self.timestamp_discontinuities = 0
        self._force_break = False
        self.filter = None
        self.filter_settings = (None, None, None)
        self.markers = []
        self._ready = False
        self.reset_page()
        self.mpl_connect('button_press_event', self._focus)

    def reset_page(self):
        self.markers.clear()
        self.display_length = max(1, round(self.fs*self.time_scale))
        self.data = np.full((self.ch_num, self.display_length), np.nan, dtype=np.float32)
        self.boundary = np.zeros(self.display_length, dtype=bool)
        self.position = self.painted = 0
        self._ready = True
        self._init_axes()
        self.draw()

    def _init_axes(self):
        self.axes.clear()
        self.axes.set_facecolor('white')
        self.axes.set_xlim(0, self.display_length)
        self.axes.set_xticks(np.linspace(0, self.display_length, 11))
        self.axes.set_xticklabels(np.round(np.linspace(0, self.time_scale, 11), 1), fontsize=15)
        self.channels = list(range(self.ch_num)) if self.focus_channel is None else [self.focus_channel]
        n, scale = len(self.channels), self.voltage_scale
        self.axes.set_ylim(0, n*2*scale+.1)
        labels = ['']
        for ch in self.channels:
            labels.extend([f'CH{ch+1}', ''])
        self.axes.set_yticks(np.arange(2*n+1)*scale, labels=labels)
        self.axes.xaxis.grid(True, linestyle='-.', alpha=.5)
        self.curves = [self.axes.plot([], [], color='#3D2E03', lw=.7)[0] for _ in self.channels]
        self.eraser = Rectangle((0, 0), 0, 0, facecolor='white', edgecolor='none')
        self.axes.add_patch(self.eraser)
        self.cursor = self.axes.axvline(-10, color='#37B492', lw=1.5, alpha=.5)

    def set_time_scale(self, seconds):
        if seconds not in TIME_SCALES:
            raise ValueError('Unsupported time page')
        if seconds != self.time_scale:
            self.time_scale = seconds
            self.reset_page()  # Reference resets the page when the time window changes.

    def set_voltage_scale(self, value):
        self.auto_scale = value is None
        if value is not None:
            self.voltage_scale = max(.001, float(value))
        self._autoscale()
        self._init_axes()
        self.draw()  # Frozen history also responds to scale changes.

    def _autoscale(self):
        if not self.auto_scale or not np.isfinite(self.data).any():
            return False
        m = int(np.nanmax(np.abs(self.data)))
        p = 10**(len(str(m))-1)
        scale = float((m//p+1)*p)
        if scale == self.voltage_scale:
            return False
        self.voltage_scale = scale
        self.scale_changed.emit(scale)
        return True

    def begin_connection(self):
        self.last = None
        self._force_break = True  # Keep retained history, but don't join distinct sessions.

    def set_filter(self, high, low, notch):
        from signal_tools import DisplayFilter
        replacement = DisplayFilter(self.fs, high, low, notch)
        self.filter_settings = (high, low, notch)
        self.filter = replacement
        self._force_break = True  # Retain old page; do not join old/new filter states.

    def add_marker(self, anchor, label):
        if self.last is None or anchor['generation'] != self.last.generation: return False
        end = (self.last.meta[0]+self.last.count) & 0xffffffff
        age = (end-anchor['sequence']) & 0xffffffff
        if not 0 < age <= self.display_length: return False
        position = (self.position-age) % self.display_length
        self.markers.append((position, label))
        self.markers = self.markers[-256:]
        self.draw()
        return True

    def _draw_markers(self):
        artists = []
        offset = self.display_length/(self.fig.get_size_inches()[0]*72)
        for position, label in self.markers:
            item = self.axes.text(position, len(self.channels)*.01*self.voltage_scale,
                ' \n'.join(label+'|'), color='r', fontsize=16, clip_on=True)
            item.set_x(max(1, position-1-item.get_window_extent(self.get_renderer()).width*offset))
            artists.append(item)
        return artists

    def append(self, msg):
        seq, stamp, mclk, rate, gain, mode, count, _ = msg.meta
        fs = rate*mclk/2048000
        if fs != self.fs:
            self.fs = fs
            if self.filter:
                # Invalid old cutoffs are disabled when lowering the sample rate.
                self.set_filter(*(v if v is None or v < fs/2 else None for v in self.filter_settings))
            self.reset_page()
        boundary = self._force_break
        self._force_break = False
        if self.last is not None:
            if self.last.generation != msg.generation:
                self.flush()
                self.generation_changes += 1
                boundary = True  # START resumes the scan; it does not erase the old page.
            else:
                delta = (seq-self.last.meta[0]-self.last.count) & 0xffffffff
                if delta >= 0x80000000:
                    self.reorders += 1
                    return
                if delta:
                    self.breaks += 1
                    self._advance_gap(delta)
                    boundary = True
                # Sequence provides the sampling grid; timestamps validate it, not USB arrival time.
                elapsed = (stamp-self.last.meta[1])/1e6
                expected = (self.last.count+delta)/fs
                if abs(elapsed-expected) > max(.002, 4/fs):
                    self.timestamp_discontinuities += 1
        b = np.frombuffer(msg.raw, dtype=np.uint8, offset=40).reshape(count, 27)[:, 3:]
        b = b.reshape(count, self.ch_num, 3).astype(np.int32)
        values = (b[:, :, 0]<<16) | (b[:, :, 1]<<8) | b[:, :, 2]
        values = ((values ^ 0x800000)-0x800000) * (VREF_UV/(8388608*gain))
        if self.filter:
            if boundary: self.filter.reset()
            values = self.filter.apply(values)
        self.feed(values, boundary=boundary)
        self.last = msg

    def _advance_gap(self, count):
        # Skip arbitrarily long losses in bounded work and erase only their scan positions.
        if count >= self.display_length:
            self.data.fill(np.nan)
            self.boundary.fill(False)
            self.markers.clear()
            self.position = (self.position+count) % self.display_length
            self.painted = self.position
            self.draw()
        else:
            self.feed(np.full((count, self.ch_num), np.nan, dtype=np.float32))

    def feed(self, values, boundary=False):
        """Display-only µV blocks; also used by deterministic reference comparisons."""
        values = np.asarray(values)
        start = 0
        while start < len(values):
            n = min(len(values)-start, self.display_length-self.position)
            end = self.position+n
            self.markers = [(x, t) for x, t in self.markers if not self.position <= x < end]
            self.data[:, self.position:end] = values[start:start+n].T
            self.boundary[self.position:end] = False
            if boundary:
                self.boundary[self.position] = True
                boundary = False
            self.position = end
            start += n
            if self.position == self.display_length:
                self.flush()
                self.position = self.painted = 0

    def _path(self, ch, a, b):
        x = np.arange(a, b, dtype=np.float64)
        y = self.data[ch, a:b].copy()
        # NaN at a boundary prevents lines between sessions or two different sweeps.
        cut = np.flatnonzero(self.boundary[a:b])
        if len(cut):
            x = np.insert(x, cut, x[cut])
            y = np.insert(y, cut, np.nan)
        return peak_path(x, y, max(1, self.axes.bbox.width)/self.display_length)

    def flush(self):
        a, b = self.painted, self.position
        if a == b:
            return
        if self._autoscale():
            self._init_axes()
            self.draw()
            self.painted = b
            return
        if not hasattr(self, 'renderer'):
            self.draw()
        n, scale = len(self.channels), self.voltage_scale
        # Same reference geometry: one typographic-point worth of clearing ahead,
        # cursor two points ahead; old page remains until its columns are visited.
        offset = self.display_length/(self.fig.get_size_inches()[0]*72)
        self.eraser.set_bounds(a, -n*scale*.1, b-a+offset, n*scale*2.2)
        self.axes.draw_artist(self.eraser)
        for i, (ch, curve) in enumerate(zip(self.channels, self.curves)):
            # Include the preceding sample to draw the segment across flushes.
            # Never wrap to the previous page; _path retains NaN/session breaks.
            x, y = self._path(ch, max(0, a-1), b)
            curve.set_data(x, y+scale*(2*i+1))
            self.axes.draw_artist(curve)
        # Restore vertical page ticks in the overwritten strip, after the curves.
        for item in self._draw_markers():
            self.axes.draw_artist(item)
            item.remove()
        for tick in range(1, 10):
            x = tick*self.display_length/10
            if a <= x <= b:
                line = self.axes.axvline(x, color='#3D2E03', linestyle='-.', lw=.5, alpha=.5)
                self.axes.draw_artist(line)
                line.remove()
        self.cursor.set_xdata([b+2*offset, b+2*offset])
        self.axes.draw_artist(self.cursor)
        self.blit(self.axes.bbox)
        self.painted = b

    def draw(self, *args, **kwargs):
        if not self._ready:
            return super().draw(*args, **kwargs)
        # Reconstruct retained pages on expose/resize/scale changes, unlike a raster-only trace.
        for i, (ch, curve) in enumerate(zip(self.channels, self.curves)):
            x, y = self._path(ch, 0, self.display_length)
            # Cut at the sweep seam without dropping a real sample from stored data.
            if 0 < self.position < self.display_length:
                split = np.searchsorted(x, self.position)
                x = np.insert(x, split, self.position)
                y = np.insert(y, split, np.nan)
            curve.set_data(x, y+self.voltage_scale*(2*i+1))
        self.eraser.set_bounds(0, 0, 0, 0)
        markers = self._draw_markers() if hasattr(self, 'renderer') else []
        try: return super().draw(*args, **kwargs)
        finally:
            for item in markers: item.remove()

    def _focus(self, event):
        if not event.dblclick:
            return
        if self.focus_channel is not None:
            self.focus_channel = None
        else:
            for label, ch in zip(self.axes.get_yticklabels()[1::2], self.channels):
                if label.get_window_extent(self.get_renderer()).contains(event.x, event.y):
                    self.focus_channel = ch
                    break
            else:
                return
        self._init_axes()
        self.draw()
