#include "postrack/snapshot.hpp"

#include <deque>

namespace postrack {

Snapshot takeSnapshot(const PositionBook& book, std::uint64_t lastSeq) {
  Snapshot s;
  s.lastSeq = lastSeq;
  s.positions.reserve(book.positions().size());
  for (const auto& [key, position] : book.positions()) {
    PositionRow row;
    row.strategy = key.strategy;
    row.symbol = key.symbol;
    row.netQty = position.netQty();
    row.realized = position.realized();
    row.cash = position.cash();
    row.lots.assign(position.lots().begin(), position.lots().end());
    s.positions.push_back(std::move(row));
  }
  return s;
}

void restoreSnapshot(const Snapshot& snapshot, PositionBook& book) {
  auto& positions = book.positions();
  positions.clear();
  for (const PositionRow& row : snapshot.positions) {
    positions[PositionKey{row.strategy, row.symbol}] = Position::restore(
        row.netQty, row.realized, row.cash, std::deque<Lot>(row.lots.begin(), row.lots.end()));
  }
}

std::optional<Snapshot> MemorySnapshotStore::loadLatest() {
  if (snapshots_.empty()) return std::nullopt;
  return snapshots_.back();
}

} // namespace postrack
