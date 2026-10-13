#include "postrack/fill_log.hpp"

#include "test_dir.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <vector>

namespace postrack {
namespace {

using testing::TestDir;

Fill fill(std::uint64_t seq) {
  return Fill{seq, seq * 10, static_cast<std::int64_t>(seq), "S", "X", seq % 3 ? Side::Buy : Side::Sell,
              static_cast<Qty>(seq), 10'000 + static_cast<PriceRaw>(seq)};
}

std::vector<Fill> readBack(const std::string& path, LogOpenStats* stats = nullptr) {
  std::vector<Fill> got;
  FillLog log = FillLog::open(path, SyncMode::Batch, [&](const Fill& f) { got.push_back(f); }, stats);
  return got;
}

void writeFills(const std::string& path, std::uint64_t n, SyncMode mode = SyncMode::Batch) {
  FillLog log = FillLog::open(path, mode, [](const Fill&) {});
  for (std::uint64_t s = log.lastSeq() + 1; s <= n; ++s) log.append(fill(s));
  log.sync();
}

std::uintmax_t size(const std::string& path) { return std::filesystem::file_size(path); }

void cutTo(const std::string& path, std::uintmax_t bytes) { std::filesystem::resize_file(path, bytes); }

void overwriteByte(const std::string& path, std::uintmax_t at, char value) {
  std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
  f.seekp(static_cast<std::streamoff>(at));
  f.put(value);
}

TEST(FillLog, WritesAndReadsBack) {
  TestDir dir;
  const std::string path = dir.file("fills.log");
  writeFills(path, 1'000);
  LogOpenStats stats;
  const std::vector<Fill> got = readBack(path, &stats);
  ASSERT_EQ(got.size(), 1'000u);
  for (std::uint64_t i = 0; i < got.size(); ++i) EXPECT_EQ(got[i], fill(i + 1));
  EXPECT_EQ(stats.lastSeq, 1'000u);
  EXPECT_EQ(stats.truncatedBytes, 0u);
}

TEST(FillLog, ReopeningAppendsAfterTheLastRecord) {
  TestDir dir;
  const std::string path = dir.file("fills.log");
  writeFills(path, 10);
  writeFills(path, 25); // opens, finds seq 10, appends 11..25
  EXPECT_EQ(readBack(path).size(), 25u);
}

TEST(FillLog, AppendOutOfOrderIsABug) {
  TestDir dir;
  FillLog log = FillLog::open(dir.file("fills.log"), SyncMode::Batch, [](const Fill&) {});
  log.append(fill(1));
  EXPECT_THROW(log.append(fill(3)), std::logic_error);
  EXPECT_THROW(log.append(fill(1)), std::logic_error);
}

// A crash can stop the last write after any byte: every such tail must be cut off cleanly,
// keeping every complete record before it.
TEST(FillLog, TornLastRecordIsCutAtEveryPossibleByte) {
  TestDir dir;
  const std::string full = dir.file("full.log");
  writeFills(full, 20);
  writeFills(dir.file("nineteen.log"), 19);
  const std::uintmax_t goodSize = size(dir.file("nineteen.log"));
  const std::uintmax_t fullSize = size(full);

  for (std::uintmax_t cut = goodSize; cut < fullSize; ++cut) {
    const std::string path = dir.file("torn.log");
    std::filesystem::copy_file(full, path, std::filesystem::copy_options::overwrite_existing);
    cutTo(path, cut);
    LogOpenStats stats;
    const std::vector<Fill> got = readBack(path, &stats);
    ASSERT_EQ(got.size(), 19u) << "cut at " << cut;
    EXPECT_EQ(stats.truncatedBytes, cut - goodSize);
    EXPECT_EQ(size(path), goodSize); // the torn bytes are gone from the file
    writeFills(path, 20);            // and appending carries on cleanly
    EXPECT_EQ(readBack(path).size(), 20u);
  }
}

TEST(FillLog, BadChecksumOnTheLastRecordIsATornWrite) {
  TestDir dir;
  const std::string path = dir.file("fills.log");
  writeFills(path, 5);
  overwriteByte(path, size(path) - 1, '\x7F');
  EXPECT_EQ(readBack(path).size(), 4u);
}

TEST(FillLog, ZeroFilledTailIsATornWrite) {
  TestDir dir;
  const std::string path = dir.file("fills.log");
  writeFills(path, 5);
  const std::uintmax_t good = size(path);
  {
    std::ofstream f(path, std::ios::app | std::ios::binary);
    const std::string zeros(300, '\0');
    f.write(zeros.data(), static_cast<std::streamsize>(zeros.size()));
  }
  EXPECT_EQ(readBack(path).size(), 5u);
  EXPECT_EQ(size(path), good);
}

TEST(FillLog, DamageBeforeTheEndIsRefused) {
  TestDir dir;
  const std::string path = dir.file("fills.log");
  writeFills(path, 50);
  const std::uintmax_t before = size(path);
  overwriteByte(path, 100, '\x55'); // inside an early record
  EXPECT_THROW(readBack(path), LogCorrupted);
  EXPECT_EQ(size(path), before); // nothing was cut: a person needs to look
}

TEST(FillLog, SeqNumbersMustRunOneTwoThree) {
  TestDir dir;
  // Two good logs glued together: 1..3 then 1..3 again.
  writeFills(dir.file("a.log"), 3);
  const std::string path = dir.file("glued.log");
  {
    std::ifstream a(dir.file("a.log"), std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(a)), std::istreambuf_iterator<char>());
    std::ofstream out(path, std::ios::binary);
    out << bytes << bytes;
  }
  EXPECT_THROW(readBack(path), LogCorrupted);
}

TEST(FillLog, EverySyncModeKeepsTheRecords) {
  for (const SyncMode mode : {SyncMode::Batch, SyncMode::Every, SyncMode::None}) {
    TestDir dir;
    writeFills(dir.file("fills.log"), 300, mode);
    EXPECT_EQ(readBack(dir.file("fills.log")).size(), 300u);
  }
  EXPECT_EQ(parseSyncMode("every"), SyncMode::Every);
  EXPECT_THROW(parseSyncMode("sometimes"), std::invalid_argument);
}

TEST(FillLog, ReaderDoesNotChangeTheFile) {
  TestDir dir;
  const std::string path = dir.file("fills.log");
  writeFills(path, 5);
  cutTo(path, size(path) - 3);
  const std::uintmax_t before = size(path);
  std::uint64_t n = 0;
  const LogOpenStats stats = readFillLog(path, [&](const Fill&) { ++n; });
  EXPECT_EQ(n, 4u);
  EXPECT_GT(stats.truncatedBytes, 0u);
  EXPECT_EQ(size(path), before);
  EXPECT_THROW(readFillLog(dir.file("missing.log"), [](const Fill&) {}), std::runtime_error);
}

} // namespace
} // namespace postrack
