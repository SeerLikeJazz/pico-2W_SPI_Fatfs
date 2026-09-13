#!/usr/bin/env python3
"""EEG1 decoder and continuity tracking, reused from spi_dma/host/eeg_receiver.py."""
from dataclasses import dataclass
import struct
import zlib

MAGIC = b"EEG1"
TAIL = b"\x0d\x0a\xa5\x5a"
HEADER = struct.Struct("<4sBB5HIIQIHBBI")
PACKET_SIZE = 1024
RATES = {250, 500, 1000, 2000, 4000, 8000, 16000}
GAINS = {1, 2, 4, 6, 8, 12, 24}


@dataclass(frozen=True)
class Packet:
    flags: int
    count: int
    packet_sequence: int
    first_sequence: int
    first_timestamp_us: int
    mclk_hz: int
    nominal_rate: int
    gain: int
    mode: int
    stream_id: int
    raw: bytes

    @property
    def sample_rate(self):
        return self.nominal_rate * self.mclk_hz / 2048000

    def samples(self):
        for i in range(self.count):
            offset = 44 + 27 * i
            status = int.from_bytes(self.raw[offset:offset+3], "big")
            channels = tuple(int.from_bytes(self.raw[offset+3+3*c:offset+6+3*c], "big", signed=True)
                             for c in range(8))
            yield ((self.first_sequence+i) & 0xffffffff,
                   self.first_timestamp_us + i * 1000000 / self.sample_rate,
                   "recorded_irq" if i == 0 else "estimated", status, channels)


class StreamDecoder:
    def __init__(self):
        self.buffer = bytearray()
        self.crc_errors = self.header_errors = self.tail_errors = self.padding_errors = 0
        self.discarded_bytes = 0

    @staticmethod
    def valid_header(h):
        return (h[0] == MAGIC and h[1] == 1 and h[2] == 1 and h[4] == 1024 and h[5] == 44
                and 1 <= h[6] <= 36 and h[7] == 27 and h[11] > 0
                and h[12] in RATES and h[13] in GAINS and h[14] in (0, 1, 2, 3)
                and bool(h[3] & 1) == (h[6] < 36))

    def feed(self, data):
        self.buffer.extend(data)
        packets = []
        while True:
            position = self.buffer.find(MAGIC)
            if position < 0:
                n = max(0, len(self.buffer)-3)  # Retain a possibly split magic.
                self.discarded_bytes += n
                del self.buffer[:n]
                break
            if position:
                self.discarded_bytes += position
                del self.buffer[:position]
            if len(self.buffer) < HEADER.size:
                break
            h = HEADER.unpack_from(self.buffer)
            if not self.valid_header(h):
                self.header_errors += 1
            elif len(self.buffer) < PACKET_SIZE:
                break
            elif self.buffer[1020:1024] != TAIL:
                self.tail_errors += 1
            elif zlib.crc32(self.buffer[:1016]) != struct.unpack_from("<I", self.buffer, 1016)[0]:
                self.crc_errors += 1
            elif any(self.buffer[44+27*h[6]:1016]):
                self.padding_errors += 1
            else:
                raw = bytes(self.buffer[:1024])
                packets.append(Packet(h[3], h[6], h[8], h[9], h[10], h[11], h[12], h[13], h[14], h[15], raw))
                del self.buffer[:1024]
                continue
            # Only skip one byte after invalid candidate, not an entire packet.
            # A valid next packet may begin inside the corrupted candidate.
            del self.buffer[0]
            self.discarded_bytes += 1
        return packets


class Continuity:
    def __init__(self):
        self.previous = None
        self.packet_gaps = self.sample_gaps = self.reorders = 0
        self.packets = self.samples = self.bad_status = 0

    def accept(self, packet):
        prev = self.previous
        if prev and prev.stream_id == packet.stream_id:
            for actual, expected, name in (
                (packet.packet_sequence, (prev.packet_sequence+1) & 0xffffffff, "packet_gaps"),
                (packet.first_sequence, (prev.first_sequence+prev.count) & 0xffffffff, "sample_gaps"),
            ):
                delta = (actual-expected) & 0xffffffff
                if delta >= 0x80000000:
                    self.reorders += 1
                else:
                    setattr(self, name, getattr(self, name)+delta)
        self.previous = packet
        self.packets += 1
        self.samples += packet.count
        self.bad_status += sum(status >> 20 != 12 for _, _, _, status, _ in packet.samples())
