#pragma once
// Snapshots: the whole position state at one moment, tagged with the last fill seq it
// includes. Recovery = latest snapshot + replay of the fill log after that seq.

#include "postrack/position.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace postrack {

struct PositionRow {
  std::string strategy;
  std::string symbol;
  Qty netQty = 0;
  Int128 realized = 0;
  Int128 cash = 0;
  std::vector<Lot> lots; // oldest first
  bool operator==(const PositionRow&) const = default;
};

struct Snapshot {
  std::uint64_t lastSeq = 0;
  std::vector<PositionRow> positions; // sorted by (strategy, symbol)
  bool operator==(const Snapshot&) const = default;
};

Snapshot takeSnapshot(const PositionBook& book, std::uint64_t lastSeq);
// Replaces the book's positions with the snapshot's (marks are kept).
void restoreSnapshot(const Snapshot& snapshot, PositionBook& book);

class SnapshotStore {
public:
  virtual ~SnapshotStore() = default;
  // All or nothing: a failed save leaves the store exactly as it was, and throws.
  virtual void save(const Snapshot& snapshot) = 0;
  virtual std::optional<Snapshot> loadLatest() = 0;
  virtual std::vector<Snapshot> loadAll() = 0; // oldest first
};

// Keeps snapshots in memory: for tests and for running without a database.
class MemorySnapshotStore final : public SnapshotStore {
public:
  void save(const Snapshot& snapshot) override { snapshots_.push_back(snapshot); }
  std::optional<Snapshot> loadLatest() override;
  std::vector<Snapshot> loadAll() override { return snapshots_; }

private:
  std::vector<Snapshot> snapshots_;
};

// PostgreSQL through libpqxx. Tables live in their own schema (created if missing), so
// separate runs and tests don't see each other. keep = how many snapshots to keep after
// each save (0 keeps all).
class PgSnapshotStore final : public SnapshotStore {
public:
  PgSnapshotStore(const std::string& connection, const std::string& schema, std::size_t keep);
  ~PgSnapshotStore() override;

  void save(const Snapshot& snapshot) override;
  std::optional<Snapshot> loadLatest() override;
  std::vector<Snapshot> loadAll() override;

  // Drops the schema and everything in it (tests and fresh runs).
  static void dropSchema(const std::string& connection, const std::string& schema);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace postrack
