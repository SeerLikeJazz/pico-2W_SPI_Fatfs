import sys
from pathlib import Path
import multiprocessing as mp
import queue
import threading
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'host'))
import usb_process


class DisplayQueue(queue.Queue):
    def cancel_join_thread(self): pass


class FakeReceiver:
    instance = None
    def __init__(self, *args, **kwargs):
        self.done = threading.Event()
        self.snapshots = 0
        self.closed = False
        FakeReceiver.instance = self
    def snapshot(self, **kwargs):
        self.snapshots += 1
        return {'display_dropped_samples': 0}, []
    def request(self, op, value, timeout):
        return {'op': op, 'result': 0}
    def close(self): self.closed = True


class ProcessTests(unittest.TestCase):
    def test_full_display_queue_does_not_block_command_or_shutdown(self):
        parent, child = mp.Pipe()
        updates = DisplayQueue(maxsize=1)
        updates.put(('full', []))
        quit_event = threading.Event()
        with patch.object(usb_process, 'Receiver', FakeReceiver):
            worker = threading.Thread(target=usb_process._worker,
                args=('fake', None, child, updates, quit_event))
            worker.start()
            try:
                self.assertTrue(parent.poll(2))
                self.assertEqual(parent.recv()[0], 'ready')
                parent.send((0, 0, 1))
                self.assertTrue(parent.poll(2))
                self.assertEqual(parent.recv(), ('reply', {'op': 0, 'result': 0}))
            finally:
                quit_event.set()
                worker.join(2)
                parent.close()
            self.assertFalse(worker.is_alive())
            self.assertTrue(FakeReceiver.instance.closed)


if __name__ == '__main__': unittest.main()
