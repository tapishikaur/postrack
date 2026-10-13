# Accounting rules

Both implementations (C++ `postrack` and Python `reference/postrack_ref.py`) follow this
document. If they disagree, one of them doesn't follow it: that's a break.

## Units

- **Price**: a whole number of 1/10,000ths of a currency unit. `150.25` is `1502500`.
  Prices are always positive.
- **Quantity**: whole shares, always positive in a fill. The side says which way.
- **Money** (realized, unrealized, cash): price units × shares, so `1/10,000` of a currency unit.
  Kept as exact integers (128-bit in C++, Python `int`). Never floating point.

## Positions

A position is kept per **(strategy, symbol)**. Strategy and symbol are case-sensitive strings.

A position is a queue of **lots**. A lot is `(qty, price)` with `qty > 0`. Every lot in a
position is on the same side: all long, or all short. The net quantity is the sum of the lot
quantities: positive when long, negative when short, 0 when the queue is empty.

## Applying a fill

`side` is buy or sell. Let `s = +1` for a buy and `-1` for a sell.

1. **Cash**: a buy subtracts `price × qty`, a sell adds `price × qty`.
2. If the position is flat, or the fill is on the same side as the position (a buy while long,
   a sell while short), append a new lot `(qty, price)` at the back. Done.
3. Otherwise the fill **closes** lots, oldest first (FIFO). While fill quantity remains and
   lots remain:
   - `closed = min(remaining, front lot qty)`
   - long lot (we're selling): `realized += (fill price - lot price) × closed`
   - short lot (we're buying): `realized += (lot price - fill price) × closed`
   - subtract `closed` from the front lot and from `remaining`; remove the lot if it's now 0.
4. If quantity remains after every lot is closed, the position **flips**: the rest becomes one
   new lot `(remaining, fill price)` on the other side.

## Marks and unrealized P&L

The **mark** for a symbol is the price in the latest `Mark` message for it. Before any mark
has arrived for a symbol, that symbol has no mark and its unrealized P&L is 0.

Unrealized P&L at mark `m`:

- long lot: `(m - lot price) × qty`
- short lot: `(lot price - m) × qty`

summed over the position's lots.

## The identity

For every position, at every moment and any mark `m`:

```
realized + unrealized(m) == cash + net_qty × m
```

If this ever fails, money appeared or vanished. Both test suites check it.

## Average cost

For display only: `sum(lot price × lot qty) / sum(lot qty)`, rounded to the nearest price unit,
halves away from zero. It's never used in any P&L number, so it can't cause a break.

## Duplicates and order

Fills are numbered `seq = 1, 2, 3, ...` by the upstream. A fill is applied exactly once, in seq
order. A fill with a seq at or below the last applied one is a duplicate and is ignored.
