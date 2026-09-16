"""Keep serial I/O and recording outside the Python GUI's GIL.

Only the bounded display queue may drop snapshots. Commands use a separate pipe.
"""
import multiprocessing as mp
import queue
import threading
from usb_receiver import Receiver


def _worker(port, raw_path, control, updates, quit_event):
    receiver = None
    dropped = 0
    updates.cancel_join_thread()  # Never block shutdown on unconsumed display data.
    try:
        receiver = Receiver(port, raw_path=raw_path, display=True)
        control.send(('ready', None))
        # A Windows pipe may deliver one serialized batch per GUI tick.
        # Publish at 20 Hz, below the GUI's 30 Hz consumption cadence.
        while not quit_event.wait(.05):
            if control.poll():
                op, value, timeout = control.recv()
                try:
                    result = receiver.local(op, value) if isinstance(op, str) else receiver.request(op, value, timeout)
                    control.send(('reply', result))
                except Exception as exc:
                    control.send(('error', str(exc)))
            stats, packets = receiver.snapshot(take_display=True)
            stats['display_dropped_samples'] += dropped
            try:
                updates.put_nowait((stats, packets))
            except queue.Full:
                dropped += sum(p.count for p in packets)
            if receiver.done.is_set():
                break
    except Exception as exc:
        try: control.send(('error', str(exc)))
        except (OSError, EOFError): pass
    finally:
        if receiver:
            receiver.close()
        control.close()


class ProcessReceiver:
    def __init__(self, port, *, raw_path=None, display=True):
        context = mp.get_context('spawn')
        self.control, child = context.Pipe()
        self.updates = context.Queue(maxsize=4)
        self.quit = context.Event()
        self.done = threading.Event()
        self.command_lock = threading.Lock()
        self.error = ''
        self.stats = dict(bytes=0, samples=0, packets=0, sample_gaps=0, packet_gaps=0,
                          reorders=0, generations=0, display_dropped_samples=0,
                          malformed=0, discarded_bytes=0, error='')
        self.process = context.Process(target=_worker,
            args=(port, raw_path, child, self.updates, self.quit), daemon=True)
        self.process.start()
        child.close()
        try:
            if not self.control.poll(10):
                raise TimeoutError('USB receive process did not start')
            kind, value = self.control.recv()
            if kind != 'ready':
                raise ConnectionError(value)
        except Exception:
            self.close()
            raise

    def request(self, op, value=0, timeout=4):
        with self.command_lock:
            if self.done.is_set() or not self.process.is_alive():
                raise ConnectionError(self.error or 'USB receive process closed')
            self.control.send((op, value, timeout))
            if not self.control.poll(timeout+2):
                # A late response cannot be mistaken for the next command.
                self.error = 'USB receive process timed out; reconnect before issuing commands'
                self.quit.set()
                self.done.set()
                raise TimeoutError(self.error)
            kind, result = self.control.recv()
            if kind != 'reply':
                raise ConnectionError(result)
            return result

    def snapshot(self, take_display=False):
        packets = []
        # Bounded GUI work; serial reception never waits for this queue.
        for _ in range(4):
            try: self.stats, batch = self.updates.get_nowait()
            except queue.Empty: break
            if take_display: packets.extend(batch)
        if not self.process.is_alive():
            self.done.set()
            self.error = self.stats.get('error') or 'USB receive process stopped'
            self.stats['error'] = self.error
        return self.stats.copy(), packets

    def close(self):
        self.quit.set()
        self.process.join(7)
        if self.process.is_alive():
            self.process.terminate()
            self.process.join(2)
            self.error = 'Receive process forced closed; recording may be incomplete'
        self.done.set()
        self.control.close()
        self.updates.close()
