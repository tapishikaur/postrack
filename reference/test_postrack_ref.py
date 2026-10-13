"""Tests for the Python reference itself (python -m unittest, standard library only)."""

import os
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import postrack_ref as ref  # noqa: E402

PX = ref.PRICE_SCALE


def record(fill: ref.Fill) -> bytes:
    body = struct.pack("<BBQQq", 2, 1, fill.seq, fill.exec_id, fill.ts_ns)
    for s in (fill.strategy, fill.symbol):
        b = s.encode()
        body += struct.pack("<B", len(b)) + b
    body += struct.pack("<Bqq", fill.side, fill.qty, fill.price)
    return struct.pack("<II", len(body), ref.crc32c(body)) + body


def fill(seq: int, side: int = 1, qty: int = 10, price: int = 5 * PX) -> ref.Fill:
    return ref.Fill(seq, seq, seq, "S", "X", side, qty, price)


class AccountingTest(unittest.TestCase):
    def test_fifo_close_and_flip(self):
        p = ref.Position()
        p.apply(1, 100, 10 * PX)
        p.apply(1, 100, 20 * PX)
        p.apply(2, 150, 25 * PX)  # closes 100 @ 10 and 50 @ 20
        self.assertEqual(p.realized, (100 * 15 + 50 * 5) * PX)
        self.assertEqual(p.net, 50)
        p.apply(2, 80, 22 * PX)  # closes 50 @ 20, then short 30 @ 22
        self.assertEqual(p.realized, (100 * 15 + 50 * 5 + 50 * 2) * PX)
        self.assertEqual(p.net, -30)
        self.assertEqual(list(map(tuple, p.lots)), [(30, 22 * PX)])

    def test_identity_holds(self):
        p = ref.Position()
        for side, qty, price in [(1, 7, 101), (2, 3, 99), (2, 9, 120), (1, 2, 80), (1, 30, 77)]:
            p.apply(side, qty, price)
            for mark in (1, 50, 1000):
                self.assertEqual(p.realized + p.unrealized(mark), p.cash + p.net * mark)

    def test_no_mark_means_no_unrealized(self):
        p = ref.Position()
        p.apply(1, 5, 10)
        self.assertEqual(p.unrealized(None), 0)


class LogTest(unittest.TestCase):
    def write(self, data: bytes) -> str:
        f = tempfile.NamedTemporaryFile(delete=False)
        f.write(data)
        f.close()
        self.addCleanup(os.unlink, f.name)
        return f.name

    def test_crc32c_check_value(self):
        self.assertEqual(ref.crc32c(b"123456789"), 0xE3069283)

    def test_reads_records_and_cuts_a_torn_tail(self):
        data = b"".join(record(fill(s)) for s in (1, 2, 3))
        for cut in range(len(data) - len(record(fill(3))), len(data)):
            stats = ref.ReadStats()
            got = list(ref.read_fills(self.write(data[:cut]), stats))
            self.assertEqual([f.seq for f in got], [1, 2])
            self.assertEqual(stats.torn_bytes, cut - (len(data) - len(record(fill(3)))))
        self.assertEqual(len(list(ref.read_fills(self.write(data)))), 3)

    def test_damage_before_the_end_is_an_error(self):
        data = bytearray(b"".join(record(fill(s)) for s in (1, 2, 3)))
        data[20] ^= 0xFF
        with self.assertRaises(ref.LogError):
            list(ref.read_fills(self.write(bytes(data))))

    def test_seq_must_run_in_order(self):
        data = record(fill(1)) + record(fill(3))
        with self.assertRaises(ref.LogError):
            list(ref.read_fills(self.write(data)))

    def test_prices_parse_exactly(self):
        self.assertEqual(ref.parse_price("150.25"), 1_502_500)
        self.assertEqual(ref.parse_price("7"), 70_000)
        with self.assertRaises(ValueError):
            ref.parse_price("1.23456")


if __name__ == "__main__":
    unittest.main()
