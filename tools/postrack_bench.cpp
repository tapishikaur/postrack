// postrack_bench: how fast the pieces are, one at a time.
//
//   postrack_bench [--fills 1000000] [--dir bench-tmp] [--db CONN --schema bench]
//
//   1. apply only: fills applied to the position book, in memory
//   2. log + apply, for each fsync policy: batch (one fsync per 500 fills, like one
//      socket read), every (one fsync per fill; fewer fills, it's slow), none
//   3. recovery: open the 1M-record log, check every record, replay it
//   4. snapshots (with --db): save and load the final state in PostgreSQL

#include "postrack/cli.hpp"
#include "postrack/fill_log.hpp"
#include "postrack/position.hpp"
#include "postrack/sim.hpp"
#include "postrack/snapshot.hpp"

#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>

namespace {

using namespace postrack;
using Clock = std::chrono::steady_clock;

double seconds(Clock::time_point t0) { return std::chrono::duration<double>(Clock::now() - t0).count(); }

void line(const std::string& what, std::uint64_t n, double s) {
  std::cout << "  " << std::left << std::setw(38) << what << std::right << std::setw(10) << n << " fills  "
            << std::setw(9) << std::fixed << std::setprecision(1) << s * 1000 << " ms  " << std::setw(12)
            << std::setprecision(0) << static_cast<double>(n) / s << " fills/s\n";
}

double logAndApply(const std::vector<Fill>& fills, std::uint64_t n, SyncMode mode, const std::filesystem::path& path,
                   std::uint64_t* syncs) {
  std::filesystem::remove(path);
  FillLog log = FillLog::open(path.string(), mode, [](const Fill&) {});
  PositionBook book;
  const auto t0 = Clock::now();
  for (std::uint64_t i = 0; i < n; ++i) {
    log.append(fills[i]);
    book.apply(fills[i]);
    if (mode == SyncMode::Batch && (i + 1) % 500 == 0) log.sync();
  }
  log.sync();
  const double s = seconds(t0);
  if (syncs) *syncs = log.syncs();
  return s;
}

} // namespace

int main(int argc, char** argv) {
  try {
    const Args args(argc, argv, {"fills", "dir", "db", "schema"});
    SimConfig config;
    config.fills = args.u64("fills", 1'000'000);
    const SimData data = generate(config);
    const std::uint64_t n = data.fills.size();
    const std::filesystem::path dir = args.str("dir", "bench-tmp");
    std::filesystem::create_directories(dir);
    std::cout << "postrack_bench: " << n << " synthetic fills, " << config.strategies << " strategies x "
              << config.symbols << " symbols\n\n";

    std::cout << "1. Apply only (in memory)\n";
    {
      PositionBook book;
      const auto t0 = Clock::now();
      for (const Fill& f : data.fills) book.apply(f);
      line("apply", n, seconds(t0));
    }

    std::cout << "\n2. Fill log + apply\n";
    std::uint64_t syncs = 0;
    const double batch = logAndApply(data.fills, n, SyncMode::Batch, dir / "batch.log", &syncs);
    line("fsync per 500 fills (" + std::to_string(syncs) + " fsyncs)", n, batch);
    const std::uint64_t everyN = std::min<std::uint64_t>(n, 20'000);
    const double every = logAndApply(data.fills, everyN, SyncMode::Every, dir / "every.log", &syncs);
    line("fsync every fill", everyN, every);
    const double none = logAndApply(data.fills, n, SyncMode::None, dir / "none.log", nullptr);
    line("no fsync (unsafe; for comparison)", n, none);

    std::cout << "\n3. Recovery: check and replay the whole log\n";
    PositionBook recovered;
    {
      const auto t0 = Clock::now();
      LogOpenStats stats;
      FillLog log =
          FillLog::open((dir / "batch.log").string(), SyncMode::Batch, [&](const Fill& f) { recovered.apply(f); }, &stats);
      line("open + checksum + replay", stats.records, seconds(t0));
    }

    if (args.has("db")) {
      std::cout << "\n4. Snapshots in PostgreSQL (final state of the run above)\n";
      const std::string db = args.str("db", ""), schema = args.str("schema", "postrack_bench");
      PgSnapshotStore::dropSchema(db, schema);
      PgSnapshotStore store(db, schema, 3);
      const Snapshot snapshot = takeSnapshot(recovered, n);
      std::size_t lots = 0;
      for (const auto& p : snapshot.positions) lots += p.lots.size();
      constexpr int kRuns = 5;
      auto t0 = Clock::now();
      for (int i = 0; i < kRuns; ++i) store.save(snapshot);
      const double save = seconds(t0) / kRuns;
      t0 = Clock::now();
      for (int i = 0; i < kRuns; ++i) (void)store.loadLatest();
      const double load = seconds(t0) / kRuns;
      std::cout << "  " << snapshot.positions.size() << " positions, " << lots << " open lots\n"
                << "  save (one transaction, COPY)   " << std::setprecision(1) << save * 1000 << " ms\n"
                << "  load latest                    " << load * 1000 << " ms\n";
      PgSnapshotStore::dropSchema(db, schema);
    }

    for (const char* f : {"batch.log", "every.log", "none.log"}) std::filesystem::remove(dir / f);
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "postrack_bench: " << e.what() << '\n';
    return 1;
  }
}
