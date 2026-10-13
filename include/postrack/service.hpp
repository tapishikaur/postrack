#pragma once
// The PosTrack service: connects to the upstream, logs every fill before applying it,
// keeps positions, snapshots them every N fills, and recovers exactly after a crash.
// Everything runs on the io_context's one thread: no locks, deterministic.

#include "postrack/fill_log.hpp"
#include "postrack/position.hpp"
#include "postrack/snapshot.hpp"

#include <boost/asio/io_context.hpp>

#include <cstdint>
#include <memory>
#include <string>

namespace postrack {

struct ServiceConfig {
  std::string upstreamHost = "127.0.0.1";
  std::uint16_t upstreamPort = 9101;
  std::string pricesHost = "127.0.0.1";
  std::uint16_t pricesPort = 0;         // 0: no price feed
  std::string logPath = "postrack.log";
  SyncMode sync = SyncMode::Batch;
  std::uint64_t snapshotEvery = 10'000; // fills between snapshots; 0: only at stop
  std::uint64_t stopAfterSeq = 0;       // stop by itself once this seq is applied (and marks have ended)
  std::string reportDir;                // write positions.csv, lots.csv, summary.txt at stop; "" = none
  bool quiet = false;
};

struct RecoveryStats {
  bool fromSnapshot = false;
  std::uint64_t snapshotSeq = 0;     // last seq in the snapshot (0 if none)
  std::uint64_t logRecords = 0;      // good records in the fill log
  std::uint64_t replayed = 0;        // log records applied on top of the snapshot
  std::uint64_t truncatedBytes = 0;  // torn tail cut from the log
  double snapshotMs = 0, replayMs = 0;
};

struct ServiceStats {
  std::uint64_t fillsApplied = 0;    // this run, not counting recovery
  std::uint64_t duplicates = 0;      // fills ignored because their seq was already logged
  std::uint64_t gaps = 0;            // reconnects because a seq was skipped
  std::uint64_t connects = 0;        // successful upstream connections
  std::uint64_t snapshots = 0;
  std::uint64_t snapshotFailures = 0;
  std::uint64_t protocolErrors = 0;
  std::uint64_t marks = 0;
};

class Service {
public:
  Service(boost::asio::io_context& io, ServiceConfig config, SnapshotStore& store);
  ~Service();

  // Loads the latest snapshot and replays the fill log after it. Call once, before start().
  RecoveryStats recover();
  // Connects to the upstream (and price feed), sending Resume. Returns at once; run the
  // io_context to process.
  void start();
  // Graceful stop: forces the log to disk, takes a final snapshot, writes the report, and
  // closes every socket and timer so the io_context runs out of work.
  void stop();

  bool stopped() const;
  const PositionBook& book() const;
  std::uint64_t appliedSeq() const; // last fill applied to the positions
  std::uint64_t loggedSeq() const;  // last fill in the log
  const ServiceStats& stats() const;

  void writeReport(const std::string& dir) const;

private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};

} // namespace postrack
