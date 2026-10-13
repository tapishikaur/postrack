# Wire protocol and fill log format

Version 1. All integers are **little-endian**, written byte by byte (never by copying a struct).

## Frames (TCP)

```
[u32 length][body: length bytes]
```

`length` counts only the body. It must be between 1 and 4,096; anything else is a protocol
error and the connection is dropped. TCP is a byte stream, so a reader must collect bytes until
a whole frame has arrived: one `read()` can hold half a frame or several.

## Body

```
[u8 type][u8 version = 1][fields...]
```

A body with an unknown type, a version other than 1, missing bytes, or bytes left over after
its last field is a protocol error.

`str` is `[u8 n][n bytes]`, with `1 <= n <= 64`.

| type | name | fields | sent by |
|---|---|---|---|
| 1 | Resume | `u64 last_seq` | PosTrack, first thing on every connection |
| 2 | Fill | `u64 seq, u64 exec_id, i64 ts_ns, str strategy, str symbol, u8 side, i64 qty, i64 price` | upstream |
| 3 | Mark | `str symbol, i64 price, i64 ts_ns` | price feed |
| 4 | Heartbeat | (none) | either side |
| 5 | End | `u64 last_seq` | upstream / price feed: nothing more will follow |

Field rules for Fill: `seq >= 1`, `side` is 1 (buy) or 2 (sell), `qty >= 1`, `price >= 1`.
For Mark: `price >= 1`. Prices are in 1/10,000ths (see `accounting.md`).

## Session

1. PosTrack connects and sends `Resume{last_seq}`: the last seq it has durably logged (0 if none).
2. The upstream sends every fill with `seq > last_seq`, in order, then new fills as they happen.
3. PosTrack ignores any fill with `seq <= ` its last seq (a duplicate). If a fill arrives with
   `seq > last + 1` (a gap), PosTrack drops the connection and reconnects with a new Resume.

The price feed works the same way, except that `Resume` is ignored: on every connection it
plays its whole price file from the start, then sends `End`. A mark replaces the previous mark
for its symbol, so replaying from the start after a reconnect gives the same marks.

## Fill log (on disk)

An append-only file of records:

```
[u32 length][u32 crc32c][body: length bytes]
```

`body` is exactly a Fill body as above (type 2, version 1). `crc32c` is CRC-32C (Castagnoli:
polynomial `0x1EDC6F41`, reflected `0x82F63B78`, initial value `0xFFFFFFFF`, final XOR
`0xFFFFFFFF`) of the body. `crc32c("123456789") = 0xE3069283`.

Records hold seq `1, 2, 3, ...` with no gaps or repeats.

Reading the log:

- A **last** record that is incomplete, or whose checksum fails, or a tail of zero bytes, is a
  **torn write** from a crash. It's cut off, and the upstream resends that fill.
- A bad record **before** the end, or seq numbers out of order, is **corruption**. PosTrack
  refuses to start, because that needs a person.

The fill simulator's `--dump` file uses the same record format.
