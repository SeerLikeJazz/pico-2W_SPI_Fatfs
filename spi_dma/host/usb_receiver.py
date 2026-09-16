"""USB receiver: I/O, disk writer and optional display have independent queues."""
import argparse
from collections import deque
import json
import queue
import threading
import time
from pathlib import Path
from usb_protocol import Decoder, Continuity, command, QUERY, START, STOP, RATE, GAIN, MODE


class Recorder(threading.Thread):
    def __init__(self, path):
        super().__init__(name='usb-disk', daemon=True)
        self.file = open(path, 'xb')  # Never overwrite a previous measurement.
        self.queue = queue.Queue(128)  # <=8 MiB for 64 KiB serial reads.
        self.dropped = 0
        self.error = ''
        self.done = threading.Event()
        self.start()

    def offer(self, data):
        try:
            self.queue.put_nowait(data)
        except queue.Full:
            self.dropped += len(data)

    def run(self):
        try:
            while not self.done.is_set() or not self.queue.empty():
                try:
                    data = self.queue.get(timeout=.05)
                except queue.Empty:
                    continue
                if self.error:
                    self.dropped += len(data)
                    continue
                try:
                    self.file.write(data)
                except OSError as exc:
                    self.error = str(exc)
                    self.dropped += len(data)
        finally:
            try:
                self.file.close()
            except OSError as exc:
                self.error = str(exc)

    def close(self):
        self.done.set()
        self.join(5)
        if self.is_alive():
            self.error = 'Disk writer did not drain within 5 s; recording incomplete'


class Receiver(threading.Thread):
    def __init__(self, port=None, *, serial_object=None, raw_path=None, display=False):
        super().__init__(name='usb-receive', daemon=True)
        if serial_object is None:
            import serial
            serial_object = serial.Serial(port, baudrate=115200, timeout=.02, write_timeout=1,
                                          rtscts=False, dsrdtr=False)
        self.serial = serial_object
        try:
            # Windows defaults to only 4096 bytes (~9 ms at 16 kSPS).
            # Request scheduling headroom while the GUI or OS is busy.
            if hasattr(self.serial, 'set_buffer_size'):
                self.serial.set_buffer_size(rx_size=1024*1024, tx_size=4096)
            self.serial.dtr = True  # Mandatory: TinyUSB otherwise permits FIFO overwrite.
            self.recorder = Recorder(raw_path) if raw_path else None
        except Exception:
            self.serial.close()
            raise
        self.decoder, self.tracking = Decoder(), Continuity()
        self.bdf = None
        self.bdf_last = {}
        self.last_sample = None
        self.display_enabled = display
        self.display = deque(maxlen=128)
        self.display_drops = self.bytes = 0
        self.lock = threading.Lock()
        self.command_lock = threading.Lock()
        self.reply_event = threading.Event()
        self.waiting_id = None
        self.response = None
        self.request_id = 0
        self.error = ''
        self.done = threading.Event()
        self.start()

    def run(self):
        try:
            while not self.done.is_set():
                data = self.serial.read(min(max(self.serial.in_waiting, 1), 65536))
                if not data:
                    continue
                if self.recorder:
                    self.recorder.offer(data)
                messages = self.decoder.feed(data)
                with self.lock:
                    self.bytes += len(data)
                    for msg in messages:
                        if msg.kind == 1:
                            self.tracking.accept(msg)  # No channel decoding in receiver thread.
                            self.last_sample = msg
                            if self.bdf: self.bdf.offer(msg)
                            if self.display_enabled:
                                if len(self.display) == self.display.maxlen:
                                    self.display_drops += self.display[0].count
                                self.display.append(msg)
                        elif msg.ident == self.waiting_id:
                            self.response = msg.status
                            self.reply_event.set()
        except Exception as exc:
            if not self.done.is_set():
                self.error = str(exc)
        finally:
            self.done.set()
            self.reply_event.set()

    def request(self, op, value=0, timeout=4):
        # Caller must use a GUI worker, not the paint/event thread.
        with self.command_lock:
            if self.done.is_set():
                raise ConnectionError(self.error or 'USB receiver closed')
            self.request_id = (self.request_id+1) & 0xffffffff
            with self.lock:
                self.waiting_id = self.request_id
                self.response = None
                self.reply_event.clear()
            packet = command(self.request_id, op, value)
            if self.serial.write(packet) != len(packet):
                raise ConnectionError('Incomplete USB command write')
            if not self.reply_event.wait(timeout):
                raise TimeoutError('No USB command reply; state unknown. Query before retrying START.')
            with self.lock:
                response = self.response
                self.waiting_id = None
            if response is None:
                raise ConnectionError(self.error or 'USB disconnected')
            return response

    def snapshot(self, take_display=False):
        with self.lock:
            t = self.tracking
            stats = dict(bytes=self.bytes, samples=t.samples, packets=t.packets,
                         sample_gaps=t.sample_gaps, packet_gaps=t.packet_gaps, reorders=t.reorders,
                         generations=t.generations, display_dropped_samples=self.display_drops,
                         malformed=self.decoder.malformed, discarded_bytes=self.decoder.discarded,
                         error=self.error)
            packets = list(self.display) if take_display else []
            if take_display:
                self.display.clear()
        if self.recorder:
            stats.update(storage_dropped_bytes=self.recorder.dropped, storage_error=self.recorder.error)
        stats.update(self.bdf.status() if self.bdf else self.bdf_last)
        return stats, packets

    def local(self, action, value):
        if action == 'bdf_start':
            from bdf_recording import BdfRecorder
            if self.bdf: raise RuntimeError('请先结束当前 BDF 录制')
            recorder = BdfRecorder(value)
            with self.lock: self.bdf = recorder
            return recorder.status()
        if action == 'bdf_stop':
            with self.lock:
                recorder, self.bdf = self.bdf, None
            if recorder: self.bdf_last = recorder.close()
            return self.bdf_last
        if action == 'marker':
            text = str(value).strip()
            if not text or len(text.encode('utf-8')) > 40:
                raise ValueError('事件必须为 1～40 个 UTF-8 字节')
            with self.lock:
                msg = self.last_sample
                if msg is None: raise RuntimeError('尚未接收到采样数据')
                seq, stamp, mclk, rate, *_ = msg.meta
                anchor = dict(generation=msg.generation,
                    sequence=(seq+msg.count-1)&0xffffffff,
                    adc_timestamp_us=round(stamp+(msg.count-1)*1e6/(rate*mclk/2048000)))
                if self.bdf: self.bdf.marker(anchor, text)
            return dict(anchor=anchor, text=text)
        raise ValueError(action)

    def close(self):
        self.done.set()
        try:
            self.serial.dtr = False  # Device stops even if STOP reply was lost.
            self.serial.close()
        finally:
            self.join(2)
            if self.recorder:
                self.recorder.close()
            self.local('bdf_stop', None)


def checked(receiver, op, value=0):
    result = receiver.request(op, value)
    if result['result']:
        raise RuntimeError(f'Command rejected: {result}')
    return result


def main():
    parser = argparse.ArgumentParser(description='USB raw capture / continuity test, no plotting, no periodic device queries')
    parser.add_argument('--port', help='e.g. COM7')
    parser.add_argument('--list', action='store_true', help='list serial ports')
    parser.add_argument('--seconds', type=float, default=60)
    parser.add_argument('--rate', type=int, choices=[250,500,1000,2000,4000,8000,16000], default=16000)
    parser.add_argument('--gain', type=int, choices=[1,2,4,6,8,12,24], default=1)
    parser.add_argument('--mode', type=int, choices=range(4), default=0)
    parser.add_argument('--raw', help='new raw binary recording; existing files rejected')
    parser.add_argument('--offline', help='parse a saved recording without a device')
    args = parser.parse_args()
    if args.list:
        from serial.tools import list_ports
        for port in list_ports.comports(): print(port.device, port.description)
        return
    if args.offline:
        decoder, tracker = Decoder(), Continuity()
        with open(args.offline, 'rb') as f:
            while data := f.read(65536):
                for message in decoder.feed(data):
                    if message.kind == 1: tracker.accept(message)
        print(json.dumps({k:v for k,v in vars(tracker).items() if k != 'last'}, indent=2))
        print(f'malformed={decoder.malformed} discarded={decoder.discarded} trailing={len(decoder.buffer)}')
        return
    if not args.port or args.seconds <= 0:
        parser.error('--port and positive --seconds are required')
    receiver = Receiver(args.port, raw_path=args.raw)
    try:
        checked(receiver, QUERY)
        checked(receiver, STOP)
        for op, value in ((RATE,args.rate),(GAIN,args.gain),(MODE,args.mode)):
            checked(receiver, op, value)
        baseline = checked(receiver, QUERY)
        print('BASELINE', json.dumps(baseline))
        checked(receiver, START)
        started = time.monotonic()
        while time.monotonic()-started < args.seconds and not receiver.done.wait(1):
            stats, _ = receiver.snapshot()
            stats['elapsed_seconds'] = round(time.monotonic()-started, 3)
            print(json.dumps(stats))  # Host console only; no USB status request.
        if receiver.done.is_set():
            raise ConnectionError(receiver.error or 'Receiver stopped')
        final = checked(receiver, STOP)
        print('DEVICE_FINAL', json.dumps(final))
        print('DEVICE_DELTAS', json.dumps({k:(final[k]-baseline[k]) & 0xffffffff for k in
            ('drdy','frames','busy_drdy','adc_queue_drops','tx_sample_drops','tainted_frames','stop_discard')}))
    except KeyboardInterrupt:
        try: print('STOP', checked(receiver, STOP))
        except Exception as exc: print('STOP unconfirmed:', exc)
    finally:
        receiver.close()
        print('HOST_FINAL', json.dumps(receiver.snapshot()[0]))


if __name__ == '__main__':
    main()
