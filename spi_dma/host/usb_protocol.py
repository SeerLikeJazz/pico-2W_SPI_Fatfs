"""AUSB v1: raw USB CDC acquisition, no application checksum. Standard library parser."""
from dataclasses import dataclass
import struct

HEADER = struct.Struct('<4sBBHII')
DATA_META = struct.Struct('<IQIHBBHH')
MAGIC = b'AUSB'
QUERY, START, STOP, RATE, GAIN, MODE = range(6)
STATUS_FIELDS = '''op result error configured running rate gain mode mclk spi drdy frames busy_drdy
adc_queue_drops bad_frames drdy_timeouts dma_timeouts dma_errors recoveries stop_discard
adc_queue_peak tx_sample_drops tx_queue_peak tx_bytes_low tx_bytes_high disconnects stall_stops
stop_reason profile_enabled drdy_irq_max dma_irq_max poll_gap_max adc_age_max tainted_frames
usb_task_max main_max tx_queued disconnect_discard malformed_commands fatal'''.split()


def command(request_id, op, value=0):
    return HEADER.pack(MAGIC, 1, 16, 24, request_id, 0) + struct.pack('<II', op, value)


@dataclass(frozen=True)
class Message:
    kind: int
    ident: int
    generation: int
    raw: bytes

    @property
    def meta(self):
        return DATA_META.unpack_from(self.raw, 16)

    @property
    def count(self):
        return self.meta[6]

    @property
    def status(self):
        values = struct.unpack_from('<40I', self.raw, 16)
        result = dict(zip(STATUS_FIELDS, values))
        result['tx_bytes'] = result['tx_bytes_low'] | result['tx_bytes_high'] << 32
        result['generation'] = self.generation
        return result


class Decoder:
    def __init__(self):
        self.buffer = bytearray()
        self.malformed = self.discarded = 0

    def feed(self, data):
        self.buffer.extend(data)
        output = []
        while True:
            pos = self.buffer.find(MAGIC)
            if pos < 0:
                n = max(0, len(self.buffer)-3)
                self.discarded += n
                del self.buffer[:n]
                break
            if pos:
                self.discarded += pos
                del self.buffer[:pos]
            if len(self.buffer) < 16:
                break
            _, version, kind, length, ident, generation = HEADER.unpack_from(self.buffer)
            valid = version == 1 and ((kind == 1 and 67 <= length <= 1012 and (length-40) % 27 == 0)
                                      or (kind in (2, 3) and length == 176))
            if valid and kind == 1 and len(self.buffer) >= 40:
                seq, stamp, mclk, rate, gain, mode, count, reserved = DATA_META.unpack_from(self.buffer, 16)
                valid = (count == (length-40)//27 and 1 <= count <= 36 and reserved == 0 and mclk > 0
                         and rate in (250, 500, 1000, 2000, 4000, 8000, 16000)
                         and gain in (1, 2, 4, 6, 8, 12, 24) and mode in range(4))
            if not valid:
                self.malformed += 1
                self.discarded += 1
                del self.buffer[0]
                continue
            if len(self.buffer) < length:
                break
            output.append(Message(kind, ident, generation, bytes(self.buffer[:length])))
            del self.buffer[:length]
        return output


class Continuity:
    def __init__(self):
        self.samples = self.packets = self.sample_gaps = self.packet_gaps = self.reorders = self.generations = 0
        self.last = None

    def accept(self, message):
        seq = message.meta[0]
        if self.last and self.last.generation == message.generation:
            for actual, expected, name in ((seq, self.last.meta[0]+self.last.count, 'sample_gaps'),
                                           (message.ident, self.last.ident+1, 'packet_gaps')):
                delta = (actual-expected) & 0xffffffff
                if delta >= 0x80000000:
                    self.reorders += 1
                else:
                    setattr(self, name, getattr(self, name)+delta)
        else:
            self.generations += 1
        self.last = message
        self.samples += message.count
        self.packets += 1
