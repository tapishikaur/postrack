# Planted-bug check: does the reconciliation actually catch mistakes?

A reconciliation that always says "0 breaks" proves nothing unless it can fail. So a real accounting
bug was planted in the C++ on purpose: closing trades took the **newest** lot instead of the oldest
(LIFO instead of FIFO), a two-line change in `src/position.cpp`.

| With the bug planted | Result |
|---|---|
| C++ tests | `Position.SellsCloseTheOldestLotFirst` failed (the property test still passes: LIFO also keeps the accounting identity, which is why the hand-worked tests exist too) |
| `scripts/reconcile.sh 10000` | **FAIL, 431 breaks**: 430 in the final positions (realized P&L, open lots), 1 in the PostgreSQL snapshot; the fill log itself still matched |

The code was then restored, and the tests and the 1,000,000-fill reconciliation pass with 0 breaks
([reconcile-1m-fills.md](reconcile-1m-fills.md)).

How to repeat it: in `Position::apply`, change `lots_.front()` to `lots_.back()` and `pop_front()` to
`pop_back()`, rebuild, run `bash scripts/reconcile.sh 10000 /tmp/check.md`.
