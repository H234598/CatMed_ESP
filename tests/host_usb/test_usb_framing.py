"""Transport fragments must preserve the bounded LF-frame contract."""
from pathlib import Path
import argparse
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
import katmed_usb_setup as usb


class Clock:
    now = 0.0
    def __call__(self):
        return self.now


class Port:
    def __init__(self, clock, chunks, step=0.25):
        self.clock, self.chunks, self.step = clock, list(chunks), step
        self.sizes, self.writes, self.closed = [], [], False
    def read_until(self, end, size):
        self.sizes.append(size)
        self.clock.now += self.step
        item = self.chunks.pop(0) if self.chunks else b''
        if isinstance(item, Exception):
            raise item
        return item  # Deliberately permits a broken fake to exceed its size.
    def write(self, data):
        self.writes.append(data)
        return len(data)
    def flush(self):
        pass
    def close(self):
        self.closed = True


class FramingTests(unittest.TestCase):
    def session(self, chunks, cls=usb.ObservationSession):
        clock = Clock()
        port = Port(clock, chunks)
        session = cls(port, clock=clock, emit=lambda _: None, duration_seconds=30)
        return session, port, clock

    def test_fragment_timeout_and_lf_reassembly(self):
        s, p, c = self.session([b'abc', b'', b'de', b'\n'])
        deadline = s.deadline
        for _ in range(3):
            self.assertIsNone(s._line())
        self.assertEqual(b'abcde', s._line())
        self.assertEqual([769, 766, 766, 764], p.sizes)
        self.assertEqual((6, 1, deadline), (s.input_bytes, s.lines, s.deadline))
        s.ensure_complete_line()

    def test_max_body_and_split_crlf(self):
        for chunks in [[b'x' * 767, b'\n'], [b'x' * 767, b'\r', b'\n']]:
            with self.subTest(crlf=len(chunks) == 3):
                s, p, _ = self.session(chunks)
                for _ in chunks[:-1]: self.assertIsNone(s._line())
                self.assertEqual(b'x' * 767, s._line())
                self.assertEqual(1, s.lines)
                self.assertTrue(all(0 < n <= 769 for n in p.sizes))

    def test_overlength_and_filled_incomplete_frames(self):
        for chunks in [[b'x' * 768 + b'\n'], [b'x' * 769],
                       [b'x' * 700, b'x' * 69], [b'x', b'x' * 769]]:
            with self.subTest(lengths=list(map(len, chunks))):
                s, _, _ = self.session(chunks)
                with self.assertRaises(usb.SetupError):
                    for _ in chunks: s._line()
                self.assertLessEqual(len(s._line_buffer), 769)

    def test_embedded_cr_still_rejected_after_reassembly(self):
        s, _, _ = self.session([b'a\r', b'b\n'])
        self.assertIsNone(s._line())
        with self.assertRaises(usb.SetupError): s._line()

    def test_fragmented_reset_marker_still_rejected(self):
        s, _, _ = self.session([b'ESP-', b'ROM: reset\n'])
        self.assertIsNone(s._line())
        with self.assertRaisesRegex(usb.SetupError, 'Geräteneustart'): s._line()

    def test_fragmented_km1_keeps_response_limit(self):
        s, _, _ = self.session([b'KM1 ', b'x' * 252 + b'\n'])
        self.assertIsNone(s._line())
        with self.assertRaisesRegex(usb.SetupError, '256'): s._line()

    def test_bytes_budget_is_charged_for_each_fragment(self):
        s, _, _ = self.session([b'ab', b'cd'])
        s.input_bytes = usb.MAX_SESSION_INPUT_BYTES - 3
        self.assertIsNone(s._line())
        with self.assertRaisesRegex(usb.SetupError, 'Sitzungsgrenze'): s._line()
        self.assertEqual(usb.MAX_SESSION_INPUT_BYTES + 1, s.input_bytes)

    def test_line_budget_counts_started_lines_not_reads(self):
        s, _, _ = self.session([b'a', b'b', b'\n', b'c'])
        s.lines = usb.MAX_SESSION_LINES - 1
        self.assertIsNone(s._line()); self.assertIsNone(s._line())
        self.assertEqual(b'ab', s._line())
        self.assertEqual(usb.MAX_SESSION_LINES, s.lines)
        with self.assertRaisesRegex(usb.SetupError, 'Sitzungsgrenze'): s._line()

    def test_deadline_and_explicit_finish_reject_pending_without_read(self):
        s, p, clock = self.session([b'part'])
        self.assertIsNone(s._line())
        with self.assertRaises(usb.SetupError): s.ensure_complete_line()
        clock.now = s.deadline
        with self.assertRaises(usb.SetupError): s._line()
        self.assertEqual(1, len(p.sizes))

    def test_silence_is_not_reset_by_fragments(self):
        s, _, clock = self.session([b'p'])
        s.last_measure_time = 0.0
        clock.now = 29.9
        with self.assertRaisesRegex(usb.SetupError, '30 Sekunden'): s._line()
        self.assertEqual(0.0, s.last_measure_time)

    def test_status_deadline_is_not_extended_by_pending(self):
        s, p, clock = self.session([b'KM1 O'])
        with self.assertRaisesRegex(usb.SetupError, 'acht Sekunden'): s.status()
        self.assertLessEqual(clock.now, usb.SESSION_REPLY_SECONDS + 0.25)
        self.assertLessEqual(len(p.sizes), 33)

    def test_eof_after_fragment_is_not_success(self):
        s, _, _ = self.session([b'a', EOFError()])
        self.assertIsNone(s._line())
        with self.assertRaisesRegex(usb.SetupError, 'Verbindung'): s._line()

    def test_bound_subclass_inherits_reassembly_and_finish_guard(self):
        class Bound(usb.ObservationSession):
            def _line(self):
                return super()._line()
        s, _, _ = self.session([b'a', b'\n', b'part'], Bound)
        self.assertIsNone(s._line()); self.assertEqual(b'a', s._line())
        self.assertIsNone(s._line())
        with self.assertRaises(usb.SetupError): s.ensure_complete_line()

    def test_fragmented_identity_on_one_closed_port(self):
        status = (b'KM1 OK STATUS configured=0 durable=0 pending=0 clock=0 '
                  b'device=ac276e46bc2c catalog=' + b'a' * 64 +
                  b' firmware=f4575aa build=abc123 error=OK\n')
        clock = Clock(); port = Port(clock, [status[:35], b'', status[35:]])
        opened = []
        args = argparse.Namespace(port='/dev/serial/by-id/fake',
                                  expect_device='ac276e46bc2c',
                                  expect_firmware='f4575aa', duration_seconds=30)
        usb.run_observation(args, opener=lambda path: (opened.append(path) or port),
                            startup=lambda _: None, command=lambda: 'quit',
                            clock=clock, emit=lambda _: None)
        self.assertEqual(['/dev/serial/by-id/fake'], opened)
        self.assertEqual([b'KM1 STATUS\n'], port.writes)
        self.assertTrue(port.closed)


if __name__ == '__main__':
    unittest.main()
