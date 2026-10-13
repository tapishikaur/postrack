"""Independent reference implementation of PosTrack's accounting, in plain Python.

Written from docs/protocol.md and docs/accounting.md, not translated from the C++, so a
bug in one is unlikely to be copied into the other. Standard library only. Python's
integers are exact and unbounded, so no money value can overflow or round.

Used by reconcile.py; can also be run on its own:

    python postrack_ref.py --fills fills.bin [--prices prices.csv]
"""

from __future__ import annotations

import argparse
import struct
from collections import deque
from dataclasses import dataclass, field
from typing import Iterator

PRICE_SCALE = 10_000
MAX_BODY = 4096

# ── CRC-32C (Castagnoli), reflected polynomial 0x82F63B78 ──────────────────────
_CRC_TABLE = []
for _i in range(256):
    _c = _i
    for _ in range(8):
        _c = (_c >> 1) ^ 0x82F63B78 if _c & 1 else _c >> 1
    _CRC_TABLE.append(_c)


def crc32c(data: bytes) -> int:
    crc = 0xFFFFFFFF
    for b in data:
        crc = _CRC_TABLE[(crc ^ b) & 0xFF] ^ (crc >> 8)
    return crc ^ 0xFFFFFFFF


assert crc32c(b"123456789") == 0xE3069283


# ── Records ───────────────────────────────────────────────────────────────────
@dataclass(frozen=True)
class Fill:
    seq: int
    exec_id: int
    ts_ns: int
    strategy: str
    symbol: str
    side: int  # 1 buy, 2 sell
    qty: int
    price: int


class LogError(Exception):
    """Damage that is not a torn tail from a crash."""


def _decode_fill(body: bytes) -> Fill:
    pos = 0

    def take(fmt: str):
        nonlocal pos
        size = struct.calcsize(fmt)
        if pos + size > len(body):
            raise LogError("body ends inside a field")
        values = struct.unpack_from(fmt, body, pos)
        pos += size
        return values

    def text() -> str:
        nonlocal pos
        (n,) = take("<B")
        if not 1 <= n <= 64:
            raise LogError("string length out of range")
        if pos + n > len(body):
            raise LogError("body ends inside a string")
        s = body[pos : pos + n].decode("utf-8")
        pos += n
        return s

    msg_type, version = take("<BB")
    if msg_type != 2 or version != 1:
        raise LogError(f"record is type {msg_type} version {version}, not a version-1 fill")
    seq, exec_id, ts_ns = take("<QQq")
    strategy = text()
    symbol = text()
    (side,) = take("<B")
    qty, price = take("<qq")
    if pos != len(body):
        raise LogError("bytes left over after the last field")
    if seq < 1 or side not in (1, 2) or qty < 1 or price < 1:
        raise LogError(f"fill {seq} breaks the field rules")
    return Fill(seq, exec_id, ts_ns, strategy, symbol, side, qty, price)


@dataclass
class ReadStats:
    records: int = 0
    torn_bytes: int = 0


def read_fills(path: str, stats: ReadStats | None = None) -> Iterator[Fill]:
    """Every fill in a fill log (or simulator dump), checked as docs/protocol.md says."""
    with open(path, "rb") as f:
        data = f.read()
    stats = stats if stats is not None else ReadStats()
    pos, expected = 0, 1
    while pos < len(data):
        rest = len(data) - pos
        if rest < 8:
            stats.torn_bytes = rest
            return
        length, crc = struct.unpack_from("<II", data, pos)
        if not 1 <= length <= MAX_BODY:
            if not any(data[pos:]):
                stats.torn_bytes = rest
                return
            raise LogError(f"{path}: record length {length} at byte {pos}")
        if rest - 8 < length:
            stats.torn_bytes = rest
            return
        body = data[pos + 8 : pos + 8 + length]
        if crc32c(body) != crc:
            if pos + 8 + length == len(data):
                stats.torn_bytes = rest
                return
            raise LogError(f"{path}: checksum mismatch at byte {pos}")
        fill = _decode_fill(body)
        if fill.seq != expected:
            raise LogError(f"{path}: seq {fill.seq} where {expected} was expected")
        yield fill
        stats.records += 1
        expected += 1
        pos += 8 + length


def parse_price(text: str) -> int:
    """Decimal text to 1/10,000ths, exactly (refuses anything that would need rounding)."""
    whole, _, frac = text.strip().partition(".")
    if not whole.isdigit() or (frac and not frac.isdigit()):
        raise ValueError(f"bad price {text!r}")
    if len(frac) > 4 and int(frac[4:]) != 0:
        raise ValueError(f"price {text!r} has more than 4 decimals")
    frac = (frac + "0000")[:4]
    return int(whole) * PRICE_SCALE + int(frac)


def read_final_marks(path: str) -> dict[str, int]:
    """The last mark per symbol in a "ts_ns,symbol,price" file."""
    marks: dict[str, int] = {}
    with open(path, encoding="utf-8") as f:
        next(f)  # header
        for line in f:
            line = line.strip()
            if line:
                _, symbol, price = line.split(",")
                marks[symbol] = parse_price(price)
    return marks


# ── Accounting (docs/accounting.md) ───────────────────────────────────────────
@dataclass
class Position:
    lots: deque = field(default_factory=deque)  # [qty, price], oldest first, all one side
    net: int = 0
    realized: int = 0
    cash: int = 0

    def apply(self, side: int, qty: int, price: int) -> None:
        s = 1 if side == 1 else -1
        self.cash += -price * qty if s == 1 else price * qty
        if self.net == 0 or (self.net > 0) == (s > 0):
            self.lots.append([qty, price])
            self.net += s * qty
            return
        remaining = qty
        while remaining and self.lots:
            lot = self.lots[0]
            closed = min(remaining, lot[0])
            if self.net > 0:  # long lots, selling
                self.realized += (price - lot[1]) * closed
            else:  # short lots, buying
                self.realized += (lot[1] - price) * closed
            lot[0] -= closed
            remaining -= closed
            self.net += s * closed
            if lot[0] == 0:
                self.lots.popleft()
        if remaining:
            self.lots.append([remaining, price])
            self.net += s * remaining

    def unrealized(self, mark: int | None) -> int:
        if mark is None:
            return 0
        if self.net > 0:
            return sum((mark - p) * q for q, p in self.lots)
        return sum((p - mark) * q for q, p in self.lots)

    def frozen(self) -> tuple:
        return (self.net, self.realized, self.cash, tuple((q, p) for q, p in self.lots))


class Book:
    def __init__(self) -> None:
        self.positions: dict[tuple[str, str], Position] = {}

    def apply(self, fill: Fill) -> None:
        key = (fill.strategy, fill.symbol)
        position = self.positions.get(key)
        if position is None:
            position = self.positions[key] = Position()
        position.apply(fill.side, fill.qty, fill.price)

    def frozen(self) -> dict:
        """(net, realized, cash, lots) per key: an immutable copy for comparisons."""
        return {key: p.frozen() for key, p in self.positions.items()}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--fills", required=True, help="fill log or simulator dump")
    parser.add_argument("--prices", help="price file, for unrealized P&L at the final marks")
    args = parser.parse_args()
    book = Book()
    stats = ReadStats()
    for fill in read_fills(args.fills, stats):
        book.apply(fill)
    marks = read_final_marks(args.prices) if args.prices else {}
    realized = sum(p.realized for p in book.positions.values())
    unrealized = sum(p.unrealized(marks.get(sym)) for (_, sym), p in book.positions.items())
    print(f"{stats.records} fills, {len(book.positions)} positions")
    print(f"realized {realized / PRICE_SCALE:.4f}, unrealized {unrealized / PRICE_SCALE:.4f}")


if __name__ == "__main__":
    main()
