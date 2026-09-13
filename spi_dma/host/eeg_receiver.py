#!/usr/bin/env python3
"""Pico2W_EEG protocol V1, Python standard library only. No GUI required."""
import argparse
import csv
from pathlib import Path
from dataclasses import dataclass
import socket
import struct
import time
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="192.168.4.1")
    parser.add_argument("--port", type=int, default=5000)
    parser.add_argument("--retries", type=int, default=5, help="total reconnects after first attempt; not reset on success")
    parser.add_argument("--retry-delay", type=float, default=2)
    parser.add_argument("--seconds", type=float, default=0, help="0: run until Ctrl-C, otherwise maximum total duration")
    parser.add_argument("--raw", help="append received TCP bytes, including malformed packets")
    parser.add_argument("--csv", help="write decoded sample CSV (can be slow at high rates)")
    parser.add_argument("--offline", help="decode a previously saved binary stream instead of networking")
    args = parser.parse_args()
    if args.retries < 0 or args.retry_delay < 0 or args.seconds < 0:
        parser.error("retries, retry-delay and seconds must be non-negative")
    paths = [Path(p).resolve() for p in (args.offline, args.raw, args.csv) if p]
    if len(set(paths)) != len(paths):
        parser.error("offline input, raw output and CSV output must be different files")
    raw_file = open(args.raw, "ab") if args.raw else None
    csv_file = open(args.csv, "w", newline="", encoding="utf-8") if args.csv else None
    writer = csv.writer(csv_file) if csv_file else None
    if writer:
        writer.writerow(["connection", "stream_id", "packet_sequence", "sample_sequence",
                         "timestamp_us", "timestamp_kind", "mclk_hz", "nominal_rate", "gain", "mode",
                         "status"] + [f"ch{i}" for i in range(1, 9)])
    tracking = Continuity()
    decoder = StreamDecoder()
    start = last_report = time.monotonic()
    latest = None
    connection = 0

    def consume(data):
        nonlocal latest
        if raw_file:
            raw_file.write(data)
        for packet in decoder.feed(data):
            tracking.accept(packet)
            latest = packet
            if writer:
                for seq, timestamp, kind, status, channels in packet.samples():
                    writer.writerow([connection, packet.stream_id, packet.packet_sequence, seq,
                                     f"{timestamp:.3f}", kind, packet.mclk_hz, packet.nominal_rate,
                                     packet.gain, packet.mode, f"{status:06x}", *channels])

    def report():
        print(f"packets={tracking.packets} samples={tracking.samples} packet_gap={tracking.packet_gaps} "
              f"sample_gap={tracking.sample_gaps} reorder={tracking.reorders} bad_status={tracking.bad_status} "
              f"CRC/header/tail/padding={decoder.crc_errors}/{decoder.header_errors}/"
              f"{decoder.tail_errors}/{decoder.padding_errors}")
        if latest:
            print(f"  stream={latest.stream_id:08x} packet={latest.packet_sequence} "
                  f"first_seq={latest.first_sequence} count={latest.count} rate={latest.sample_rate:.6f} "
                  f"gain={latest.gain} mode={latest.mode} flags=0x{latest.flags:04x}")

    try:
        if args.offline:
            with open(args.offline, "rb") as capture:
                while chunk := capture.read(8192):
                    consume(chunk)
        else:
            deadline = start + args.seconds if args.seconds else float("inf")
            for attempt in range(args.retries+1):
                if time.monotonic() >= deadline:
                    break
                try:
                    with socket.create_connection((args.host, args.port), timeout=5) as sock:
                        connection += 1
                        # New TCP connection never joins an old incomplete packet.
                        decoder.discarded_bytes += len(decoder.buffer)
                        decoder.buffer.clear()
                        tracking.previous = None  # Cannot infer loss across reconnect/reboot.
                        print(f"Connected #{connection}: {args.host}:{args.port}")
                        sock.settimeout(1)
                        heartbeat = 0
                        while time.monotonic() < deadline:
                            if time.monotonic() - heartbeat >= 1:
                                sock.sendall(b'K'); heartbeat = time.monotonic()
                            try:
                                data = sock.recv(8192)
                                if not data:
                                    raise ConnectionError("server closed")
                                consume(data)
                            except socket.timeout:
                                pass  # ADS may deliberately be stopped; not a connection failure.
                            if time.monotonic()-last_report >= 1:
                                report(); last_report = time.monotonic()
                except OSError as exc:
                    print(f"Connection ended/failed: {exc}")
                if attempt < args.retries and time.monotonic() < deadline:
                    time.sleep(min(args.retry_delay, max(0, deadline-time.monotonic())))
    except KeyboardInterrupt:
        pass
    finally:
        report()
        if raw_file:
            raw_file.close()
        if csv_file:
            csv_file.close()


if __name__ == "__main__":
    main()
