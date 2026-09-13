"""Bounded, Qt-independent TCP receiver and explicit control transactions."""
from collections import deque
from dataclasses import dataclass
import ipaddress
import socket
import struct
import threading
import time

from protocol import StreamDecoder, Continuity, RATES, GAINS

REQUEST = struct.Struct('<4s4I')
REPLY = struct.Struct('<4s11I')
MODES = {2: '正常采集模式', 3: '阻抗采集模式', 1: '内部短路模式', 0: '内部测试模式'}
ERRORS = ('OK', 'ARGUMENT', 'STATE', 'SPI_TIMEOUT', 'ID', 'READBACK',
          'DMA_RESOURCE', 'SPI_BUDGET', 'DRDY_TIMEOUT', 'DMA_TIMEOUT',
          'DMA_HW', 'FRAME', 'ABORT')


def endpoint(host, port):
    # Numeric IPv4 avoids an unbounded platform DNS lookup during shutdown.
    host = str(ipaddress.IPv4Address(host.strip()))
    port = int(port)
    if not 1 <= port <= 65535:
        raise ValueError('端口须在 1～65535 之间')
    return host, port


@dataclass(frozen=True)
class ControlReply:
    request_id: int
    result: int
    error: int
    rate: int
    gain: int
    running: int
    configured: int
    mclk: int
    mode: int
    stream_id: int
    session: int

    @property
    def description(self):
        error = ERRORS[self.error] if self.error < len(ERRORS) else str(self.error)
        return f"{('FAILED', 'UNCHANGED', 'APPLIED')[self.result]} / {error}"


def control(host, port, request_id, operation=0, value=0, timeout=4, session=0):
    if operation not in range(6) or (operation in (0, 4, 5) and value != 0):
        raise ValueError('非法操作')
    if operation in (1, 2, 3) and value not in {1: RATES, 2: GAINS, 3: MODES}[operation]:
        raise ValueError('设备不支持此参数')
    deadline = time.monotonic() + timeout
    with socket.create_connection(endpoint(host, port), timeout=timeout) as sock:
        sock.sendall(REQUEST.pack(b'WFC2', request_id, operation, value, session))
        data = bytearray()
        while len(data) < REPLY.size:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError('控制回执超时；结果未知，请查询设备')
            sock.settimeout(remaining)
            chunk = sock.recv(REPLY.size - len(data))
            if not chunk:
                raise ConnectionError('未收到 WFR2 完整回执；请确认固件已升级')
            data.extend(chunk)
    magic, *fields = REPLY.unpack(data)
    if magic != b'WFR2' or fields[0] != request_id or fields[1] not in (0, 1, 2):
        raise ValueError('控制回执标识或请求编号错误')
    reply = ControlReply(*fields)
    if (reply.running not in (0, 1) or reply.configured not in (0, 1)
            or reply.mode not in MODES or reply.mclk == 0
            or (reply.configured and (reply.rate not in RATES or reply.gain not in GAINS))):
        raise ValueError('控制回执参数非法')
    if operation in (1, 2, 3) and reply.result and (not reply.configured or reply.error or reply.running or
                       {1: reply.rate, 2: reply.gain, 3: reply.mode}[operation] != value):
        raise ValueError('设备成功回执与请求参数不一致，结果未确认')
    if reply.result and operation in (4, 5) and (reply.error or reply.running != (operation == 4)
                                                or (operation == 4 and not reply.configured)):
        raise ValueError('设备启停回执与请求不一致')
    if operation and reply.result and reply.session != session:
        raise ValueError('设备会话已改变，结果未确认')
    return reply


class Receiver(threading.Thread):
    def __init__(self, host, port, idle_timeout=5, capacity=256):
        super().__init__(name='pico-wifi-receiver', daemon=True)
        self.address = endpoint(host, port)
        self.idle_timeout = idle_timeout
        self.stop_event = threading.Event()
        self.expecting_data = threading.Event()
        self.lock = threading.Lock()
        self.pending = deque(maxlen=capacity)
        self.status = '正在连接…'
        self.connected = False
        self.display_drops = 0
        self.decoder = StreamDecoder()
        self.tracking = Continuity()
        self.stats = ''

    def stop(self):
        self.stop_event.set()

    def snapshot(self):
        with self.lock:
            packets = list(self.pending)
            self.pending.clear()
            return packets, self.status, self.connected, self.stats, self.display_drops

    def run(self):
        try:
            with socket.create_connection(self.address, timeout=3) as sock:
                sock.settimeout(.25)
                sock.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
                last_valid = time.monotonic()
                last_heartbeat = 0
                with self.lock:
                    self.connected = True
                    self.status = '已连接，等待有效数据'
                while not self.stop_event.is_set():
                    if time.monotonic() - last_heartbeat >= 1:
                        sock.sendall(b'K')  # Liveness only; never starts acquisition.
                        last_heartbeat = time.monotonic()
                    try:
                        data = sock.recv(16384)
                        if not data:
                            raise ConnectionError('设备关闭了数据连接')
                        packets = self.decoder.feed(data)
                        for p in packets:
                            self.tracking.accept(p)
                        if packets:
                            last_valid = time.monotonic()
                        with self.lock:
                            for p in packets:
                                if len(self.pending) == self.pending.maxlen:
                                    self.display_drops += self.pending[0].count
                                self.pending.append(p)
                            t, d = self.tracking, self.decoder
                            self.stats = (f'接收 {t.samples} 帧 | 缺包 {t.packet_gaps} / 缺帧 {t.sample_gaps}'
                                          f' | 乱序 {t.reorders} | 状态异常 {t.bad_status}'
                                          f' | CRC/头/尾/填充 {d.crc_errors}/{d.header_errors}/'
                                          f'{d.tail_errors}/{d.padding_errors}')
                    except socket.timeout:
                        pass
                    with self.lock:
                        self.status = ('数据超时：连接保留，设备可能已停采；可查询设备或重新连接'
                                       if self.expecting_data.is_set() and time.monotonic() - last_valid > self.idle_timeout else '已连接')
        except (OSError, ValueError) as exc:
            with self.lock:
                self.status = f'连接失败或断开：{exc}'
        finally:
            with self.lock:
                self.connected = False
                if self.stop_event.is_set():
                    self.status = '已断开'
