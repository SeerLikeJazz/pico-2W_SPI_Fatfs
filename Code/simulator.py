"""LOCAL SIMULATION ONLY; does not validate ADS1299 hardware or Wi-Fi throughput."""
import argparse
import math
import socket
import select
import struct
import threading
import time
import zlib

from network import REQUEST, REPLY
from protocol import HEADER, TAIL, RATES, GAINS


def make_packet(sequence=0, first=0, timestamp=0, rate=250, gain=1, stream=1, count=36, mode=0):
    raw = bytearray(1024)
    HEADER.pack_into(raw, 0, b'EEG1', 1, 1, 8 | (count < 36) | (2 if sequence == 0 else 0),
                     1024, 44, count, 27, sequence & 0xffffffff, first & 0xffffffff,
                     timestamp, 2048000, rate, gain, mode, stream)
    for sample in range(count):
        offset = 44 + sample * 27
        raw[offset:offset + 3] = b'\xc0\x00\x00'
        for ch in range(8):
            value = int(300000 * math.sin(2 * math.pi * (ch + 1) * (first + sample) / rate))
            if mode == 1: value = 0
            elif mode == 3: value = 120000 if math.sin(2*math.pi*31.25*(first+sample)/rate)>=0 else -120000
            raw[offset + 3 + ch*3:offset + 6 + ch*3] = value.to_bytes(3, 'big', signed=True)
    struct.pack_into('<I', raw, 1016, zlib.crc32(raw[:1016]))
    raw[1020:] = TAIL
    return bytes(raw)


class Simulator:
    def __init__(self, host='127.0.0.1', data_port=5000, control_port=5001,
                 rate=250, disconnect_after=0, corrupt_every=0):
        self.stop_event = threading.Event()
        self.lock = threading.Lock()
        self.rate, self.gain, self.stream = rate, 1, 1
        self.mode, self.session = 0, 0
        self.running, self.connected, self.configured = False, False, True
        self.fail_next = None
        self.reply_delay = 0
        self.heartbeat_timeout = 5
        self.paused = False
        self.disconnect_after, self.corrupt_every = disconnect_after, corrupt_every
        self.sockets = []
        self.threads = []
        for port in (data_port, control_port):
            sock = socket.socket()
            sock.bind((host, port)); sock.listen(1); sock.settimeout(.1)
            self.sockets.append(sock)
        self.data_port, self.control_port = (s.getsockname()[1] for s in self.sockets)

    def start(self):
        for sock, handler in zip(self.sockets, (self.data_client, self.control_client)):
            thread = threading.Thread(target=self.serve, args=(sock, handler), daemon=True)
            self.threads.append(thread); thread.start()
        return self

    def stop(self):
        self.stop_event.set()
        for t in self.threads: t.join(2)
        for s in self.sockets: s.close()

    def serve(self, listener, handler):
        while not self.stop_event.is_set():
            try:
                client, _ = listener.accept()
            except socket.timeout:
                continue
            with client:
                client.settimeout(.5)
                try: handler(client)
                except OSError: pass

    def data_client(self, client):
        with self.lock:
            self.connected = True; self.running = False; self.session += 1
        try:
            self.stream_client(client)
        finally:
            with self.lock:
                self.connected = self.running = False; self.session += 1

    def stream_client(self, client):
        first = sequence = 0
        timestamp = 1000000.
        old_stream = None
        sent = 0
        deadline = time.monotonic()
        heartbeat = time.monotonic()
        while not self.stop_event.is_set():
            if select.select([client], [], [], 0)[0]:
                if not client.recv(128): return
                heartbeat = time.monotonic()
            if time.monotonic() - heartbeat > self.heartbeat_timeout: return
            if self.paused or not self.running:
                self.stop_event.wait(.02)
                deadline = time.monotonic()
                continue
            with self.lock:
                rate, gain, stream, mode = self.rate, self.gain, self.stream, self.mode
            if stream != old_stream:
                first = sequence = 0; old_stream = stream
            raw = make_packet(sequence, first, int(timestamp), rate, gain, stream, mode=mode)
            sent += 1
            if self.corrupt_every and sent % self.corrupt_every == 0:
                raw = bytearray(raw); raw[100] ^= 1
            # Intentionally split both magic and payload across send calls.
            client.sendall(raw[:2]); client.sendall(raw[2:91]); client.sendall(raw[91:])
            if self.disconnect_after and sent >= self.disconnect_after:
                return
            sequence = (sequence + 1) & 0xffffffff
            first = (first + 36) & 0xffffffff
            timestamp += 36e6 / rate
            deadline += 36 / rate
            self.stop_event.wait(max(0, deadline - time.monotonic()))

    def control_client(self, client):
        data = bytearray()
        while len(data) < REQUEST.size:
            part = client.recv(REQUEST.size - len(data))
            if not part: return
            data.extend(part)
        magic, request_id, operation, value, session = REQUEST.unpack(data)
        if magic != b'WFC2' or operation not in range(6) or (operation in (0,4,5) and value): return
        with self.lock:
            result, error = 1, 0
            if not self.connected or (operation and session != self.session):
                result, error = 0, 2
            elif self.fail_next and operation == self.fail_next[0]:
                result, error = 0, self.fail_next[1]; self.fail_next = None
            elif operation in (1, 2, 3):
                if self.running:
                    result, error = 0, 2
                elif value not in {1: RATES, 2: GAINS, 3: range(4)}[operation]:
                    result, error = 0, 1
                else:
                    field = {1: 'rate', 2: 'gain', 3: 'mode'}[operation]
                    if getattr(self, field) != value:
                        setattr(self, field, value); result = 2
            elif operation == 4:
                if not self.configured: result, error = 0, 2
                elif not self.running:
                    self.running = True; self.stream += 1; result = 2
            elif operation == 5 and self.running:
                self.running = False; result = 2
            reply = REPLY.pack(b'WFR2', request_id, result, error, self.rate,
                               self.gain, int(self.running), int(self.configured), 2048000,
                               self.mode, self.stream, self.session)
        if self.reply_delay: self.stop_event.wait(self.reply_delay)
        client.sendall(reply[:7]); client.sendall(reply[7:])


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--data-port', type=int, default=5000)
    parser.add_argument('--control-port', type=int, default=5001)
    parser.add_argument('--rate', type=int, choices=sorted(RATES), default=250)
    parser.add_argument('--disconnect-after', type=int, default=0, help='close each data session after N packets')
    parser.add_argument('--corrupt-every', type=int, default=0)
    args = parser.parse_args()
    sim = Simulator(data_port=args.data_port, control_port=args.control_port, rate=args.rate,
                    disconnect_after=args.disconnect_after, corrupt_every=args.corrupt_every).start()
    print(f'SIMULATION ONLY · 127.0.0.1 data={sim.data_port}, control={sim.control_port}', flush=True)
    try:
        while True: time.sleep(.5)
    except KeyboardInterrupt:
        sim.stop()
