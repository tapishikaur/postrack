#include "postrack/snapshot.hpp"

#include <gtest/gtest.h>

#include <cctype>
#include <cstdlib>
#include <limits>
#include <string>

namespace postrack {
namespace {

PositionBook sampleBook() {
  PositionBook book;
  std::uint64_t seq = 0;
  for (int i = 0; i < 200; ++i) {
    book.apply(Fill{++seq, seq, 0, i % 3 ? "S1" : "S2", "SYM" + std::to_string(i % 7), i % 4 ? Side::Buy : Side::Sell,
                    static_cast<Qty>(10 + i), 10'000 + 37 * static_cast<PriceRaw>(i)});
  }
  return book;
}

TEST(Snapshot, TakeAndRestoreGiveTheSameBook) {
  const PositionBook book = sampleBook();
  const Snapshot s = takeSnapshot(book, 200);
  EXPECT_EQ(s.lastSeq, 200u);
  PositionBook restored;
  restoreSnapshot(s, restored);
  EXPECT_EQ(restored, book);
}

TEST(Snapshot, MemoryStoreKeepsThemInOrder) {
  MemorySnapshotStore store;
  EXPECT_FALSE(store.loadLatest().has_value());
  store.save(takeSnapshot(sampleBook(), 1));
  store.save(takeSnapshot(PositionBook{}, 2));
  EXPECT_EQ(store.loadLatest()->lastSeq, 2u);
  EXPECT_EQ(store.loadAll().size(), 2u);
}

// ── PostgreSQL ───────────────────────────────────────────────────────────────
// These need a database: set POSTRACK_TEST_DB to a libpq connection string
// (scripts/build.sh starts the throwaway local cluster and sets it).

class PgSnapshotTest : public ::testing::Test {
protected:
  void SetUp() override {
    const char* db = std::getenv("POSTRACK_TEST_DB");
    if (!db || !*db) GTEST_SKIP() << "POSTRACK_TEST_DB not set";
    db_ = db;
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    schema_ = "t_" + std::string(info->name());
    for (char& c : schema_) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    PgSnapshotStore::dropSchema(db_, schema_);
  }
  void TearDown() override {
    if (!db_.empty()) PgSnapshotStore::dropSchema(db_, schema_);
  }
  std::string db_, schema_;
};

TEST_F(PgSnapshotTest, EmptyStoreHasNoSnapshot) {
  PgSnapshotStore store(db_, schema_, 3);
  EXPECT_FALSE(store.loadLatest().has_value());
  EXPECT_TRUE(store.loadAll().empty());
}

TEST_F(PgSnapshotTest, SavesAndLoadsExactly) {
  PgSnapshotStore store(db_, schema_, 3);
  const Snapshot s = takeSnapshot(sampleBook(), 200);
  store.save(s);
  const std::optional<Snapshot> loaded = store.loadLatest();
  ASSERT_TRUE(loaded.has_value());
  EXPECT_EQ(*loaded, s);
}

TEST_F(PgSnapshotTest, ExtremeMoneyValuesSurvive) {
  PgSnapshotStore store(db_, schema_, 3);
  const Int128 max = ~(Int128{1} << 127);
  Snapshot s;
  s.lastSeq = 7;
  s.positions.push_back({"S", "X", 0, max, -max - 1, {}});
  s.positions.push_back({"S", "Y", -5, -1, 1, {{5, std::numeric_limits<std::int64_t>::max()}}});
  store.save(s);
  EXPECT_EQ(*store.loadLatest(), s);
}

TEST_F(PgSnapshotTest, KeepsOnlyTheNewestN) {
  PgSnapshotStore store(db_, schema_, 2);
  for (std::uint64_t seq = 1; seq <= 5; ++seq) store.save(takeSnapshot(sampleBook(), seq));
  const std::vector<Snapshot> all = store.loadAll();
  ASSERT_EQ(all.size(), 2u);
  EXPECT_EQ(all[0].lastSeq, 4u);
  EXPECT_EQ(all[1].lastSeq, 5u);
}

TEST_F(PgSnapshotTest, KeepZeroKeepsEverything) {
  PgSnapshotStore store(db_, schema_, 0);
  for (std::uint64_t seq = 1; seq <= 4; ++seq) store.save(takeSnapshot(sampleBook(), seq));
  EXPECT_EQ(store.loadAll().size(), 4u);
}

// A save that fails half way (here the database refuses a duplicate row in the middle of
// the COPY) must leave no trace: the previous snapshot is still the latest, untouched.
TEST_F(PgSnapshotTest, FailedSaveLeavesThePreviousSnapshotIntact) {
  PgSnapshotStore store(db_, schema_, 3);
  const Snapshot good = takeSnapshot(sampleBook(), 100);
  store.save(good);

  Snapshot bad = takeSnapshot(sampleBook(), 200);
  bad.positions.push_back(bad.positions.front()); // same (strategy, symbol) twice
  EXPECT_ANY_THROW(store.save(bad));

  EXPECT_EQ(*store.loadLatest(), good);
  EXPECT_EQ(store.loadAll().size(), 1u);
  store.save(takeSnapshot(sampleBook(), 300)); // and the store still works
  EXPECT_EQ(store.loadLatest()->lastSeq, 300u);
}

TEST_F(PgSnapshotTest, SchemaNamesAreChecked) {
  EXPECT_THROW(PgSnapshotStore(db_, "bad; DROP TABLE x", 3), std::invalid_argument);
  EXPECT_THROW(PgSnapshotStore(db_, "Upper", 3), std::invalid_argument);
}

} // namespace
} // namespace postrack
