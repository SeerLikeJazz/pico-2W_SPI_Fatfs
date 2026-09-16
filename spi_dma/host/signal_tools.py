"""Reference EEG filter topology; raw ADC decoding shared by display/BDF."""
import numpy as np
from scipy.signal import butter, iirnotch, lfilter, lfilter_zi

HIGH = (None, .1, 1, 5, 20)
LOW = (None, 30, 40, 70, 100, 200, 500)
NOTCH = (None, 50, 60)


def digital_samples(msg):
    b = np.frombuffer(msg.raw, dtype=np.uint8, offset=40).reshape(msg.count, 27)[:, 3:]
    b = b.reshape(msg.count, 8, 3).astype(np.int32)
    v = (b[:, :, 0] << 16) | (b[:, :, 1] << 8) | b[:, :, 2]
    return (v ^ 0x800000) - 0x800000


class DisplayFilter:
    def __init__(self, fs, high=1, low=30, notch=50):
        self.fs = fs
        self.settings = (high, low, notch)
        self.stages = []
        for value in self.settings:
            if value is not None and not 0 < value < fs/2:
                raise ValueError(f'滤波频率必须小于奈奎斯特频率 {fs/2:g} Hz')
        if high is not None and low is not None and high >= low:
            raise ValueError('高通频率必须低于低通频率')
        # Exact reference order, second order Butterworth, notch Q=30.
        if notch is not None: self.stages.append(iirnotch(notch, 30, fs=fs))
        if high is not None: self.stages.append(butter(2, high, btype='high', fs=fs))
        if low is not None: self.stages.append(butter(2, low, btype='low', fs=fs))
        self.reset()

    def reset(self):
        self.states = [np.repeat(lfilter_zi(b, a)[:, None], 8, axis=1)
                       for b, a in self.stages]

    def apply(self, values):
        values = np.array(values, dtype=float, copy=True)
        for i, (b, a) in enumerate(self.stages):
            values, self.states[i] = lfilter(b, a, values, axis=0, zi=self.states[i])
        return values
