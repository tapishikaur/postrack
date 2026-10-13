// End to end, in one process: the real Service against the real UpstreamServer and
// PriceServer over TCP on 127.0.0.1, with duplicates and gaps injected on purpose, and
// with crashes (the service destroyed mid-run) followed by recovery.

#include "postrack/service.hpp"
#include "postrack/sim.hpp"

#include "test_dir.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <functional>

namespace postrack {
namespace {

using testing::TestDir;

// Runs the io_context until `done` is true; fails the test after `limit`.
void runUntil(boost::asio::io_context& io, const std::function<bool()>& done,
              std::chrono::seconds limit = std::chrono::seconds(60)) {
  const auto deadline = std::chrono::steady_clock::now() + limit;
  while (!done()) {
    ASSERT_LT(std::chrono::steady_clock::now(), deadline) << "timed out";
    io.run_one_for(std::chrono::milliseconds(50));
  }
}

PositionBook direct(const SimData& data) {
  PositionBook book;
  for (const Fill& f : data.fills) book.apply(f);
  return book;
}

struct Fixture {
  explicit Fixture(std::uint64_t fills, UpstreamOptions options = {}) {
    SimConfig config;
    config.fills = fills;
    config.markEvery = 1'000;
    data = generate(config);
    upstream = std::make_unique<UpstreamServer>(io, data.fills, options);
    prices = std::make_unique<PriceServer>(io, data.marks);
  }

  ServiceConfig config(std::uint64_t stopAfter) const {
    ServiceConfig c;
    c.upstreamPort = upstream->port();
    c.pricesPort = prices->port();
    c.logPath = dir.file("fills.log");
    c.snapshotEvery = 2'500;
    c.stopAfterSeq = stopAfter;
    c.quiet = true;
    return c;
  }

  TestDir dir;
  boost::asio::io_context io;
  SimData data;
  std::unique_ptr<UpstreamServer> upstream;
  std::unique_ptr<PriceServer> prices;
  MemorySnapshotStore store; // outlives every Service, like a database would
};

void expectFinalMarks(const PositionBook& book, const SimData& data) {
  std::map<std::string, PriceRaw> last;
  for (const MarkMsg& m : data.marks) last[m.symbol] = m.price;
  EXPECT_EQ(book.marks(), last);
}

TEST(ServiceEndToEnd, MatchesDirectApplicationDespiteDuplicatesAndGaps) {
  UpstreamOptions chaos;
  chaos.duplicateRate = 0.01;
  chaos.gapRate = 0.001;
  Fixture f(20'000, chaos);
  Service service(f.io, f.config(20'000), f.store);
  service.recover();
  service.start();
  runUntil(f.io, [&] { return service.stopped(); });

  EXPECT_EQ(service.appliedSeq(), 20'000u);
  EXPECT_EQ(service.book(), direct(f.data));
  expectFinalMarks(service.book(), f.data);
  EXPECT_GT(service.stats().duplicates, 0u);
  // The server may skip several fills in one batch; the client reconnects at the first, so
  // later skips in that batch are resent before they're ever seen: gaps seen <= gaps made.
  EXPECT_GT(service.stats().gaps, 0u);
  EXPECT_LE(service.stats().gaps, f.upstream->gapsMade());
  EXPECT_GE(service.stats().connects, 1 + service.stats().gaps);
  EXPECT_EQ(f.store.loadLatest()->lastSeq, 20'000u); // final snapshot at stop
}

TEST(ServiceEndToEnd, CrashMidRunThenRecoveryIsExact) {
  Fixture f(30'000);
  {
    Service first(f.io, f.config(0), f.store);
    first.recover();
    first.start();
    runUntil(f.io, [&] { return first.appliedSeq() >= 12'345; });
  } // "crash": destroyed without stop(), so no final snapshot

  const std::uint64_t snapshotSeq = f.store.loadLatest()->lastSeq;
  EXPECT_GT(snapshotSeq, 0u);
  Service second(f.io, f.config(30'000), f.store);
  const RecoveryStats r = second.recover();
  EXPECT_TRUE(r.fromSnapshot);
  EXPECT_EQ(r.snapshotSeq, snapshotSeq);
  EXPECT_EQ(r.replayed, r.logRecords - snapshotSeq); // only what the snapshot doesn't have
  second.start();
  runUntil(f.io, [&] { return second.stopped(); });

  EXPECT_EQ(second.book(), direct(f.data));
  // Each fill applied exactly once overall: the second run applies only what the log lacked.
  EXPECT_EQ(second.stats().fillsApplied, 30'000 - r.logRecords);
}

TEST(ServiceEndToEnd, TornLogTailAfterACrashIsRecovered) {
  Fixture f(10'000);
  {
    Service first(f.io, f.config(0), f.store);
    first.recover();
    first.start();
    runUntil(f.io, [&] { return first.appliedSeq() >= 6'000; });
  }
  // The crash also tore the last record in half.
  const std::string log = f.dir.file("fills.log");
  std::filesystem::resize_file(log, std::filesystem::file_size(log) - 20);

  Service second(f.io, f.config(10'000), f.store);
  const RecoveryStats r = second.recover();
  EXPECT_GT(r.truncatedBytes, 0u);
  second.start();
  runUntil(f.io, [&] { return second.stopped(); });
  EXPECT_EQ(second.book(), direct(f.data));
}

TEST(ServiceEndToEnd, RecoversFromTheLogAloneWithoutSnapshots) {
  Fixture f(5'000);
  {
    ServiceConfig c = f.config(0);
    c.snapshotEvery = 0; // never snapshot while running
    Service first(f.io, c, f.store);
    first.recover();
    first.start();
    runUntil(f.io, [&] { return first.appliedSeq() >= 3'000; });
  }
  EXPECT_FALSE(f.store.loadLatest().has_value());
  Service second(f.io, f.config(5'000), f.store);
  const RecoveryStats r = second.recover();
  EXPECT_FALSE(r.fromSnapshot);
  EXPECT_EQ(r.replayed, r.logRecords);
  second.start();
  runUntil(f.io, [&] { return second.stopped(); });
  EXPECT_EQ(second.book(), direct(f.data));
}

TEST(ServiceEndToEnd, WritesTheReport) {
  Fixture f(2'000);
  ServiceConfig c = f.config(2'000);
  c.reportDir = f.dir.file("report");
  Service service(f.io, c, f.store);
  service.recover();
  service.start();
  runUntil(f.io, [&] { return service.stopped(); });
  for (const char* name : {"positions.csv", "lots.csv", "summary.txt"}) {
    EXPECT_TRUE(std::filesystem::exists(std::filesystem::path(c.reportDir) / name)) << name;
  }
}

} // namespace
} // namespace postrack
