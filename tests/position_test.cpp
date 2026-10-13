#include "postrack/position.hpp"

#include <gtest/gtest.h>

#include <limits>
#include <random>

namespace postrack {
namespace {

constexpr PriceRaw px(std::int64_t units) { return units * kPriceScale; }
constexpr Int128 money(std::int64_t units) { return Int128{units} * kPriceScale; }

// realized + unrealized(m) == cash + net × m, the identity from docs/accounting.md.
void expectIdentity(const Position& p, PriceRaw mark) {
  EXPECT_EQ(p.realized() + p.unrealized(mark), p.cash() + Int128{p.netQty()} * mark);
}

TEST(Position, BuysAddLotsAtTheBack) {
  Position p;
  p.apply(Side::Buy, 100, px(10));
  p.apply(Side::Buy, 50, px(12));
  EXPECT_EQ(p.netQty(), 150);
  EXPECT_EQ(p.lots(), (std::deque<Lot>{{100, px(10)}, {50, px(12)}}));
  EXPECT_EQ(p.realized(), 0);
  EXPECT_EQ(p.cash(), -money(1000 + 600));
  EXPECT_EQ(p.unrealized(px(11)), money(100 - 50));
}

TEST(Position, SellsCloseTheOldestLotFirst) {
  Position p;
  p.apply(Side::Buy, 100, px(10));
  p.apply(Side::Buy, 100, px(20));
  p.apply(Side::Sell, 150, px(25)); // closes 100 @ 10 and 50 @ 20
  EXPECT_EQ(p.netQty(), 50);
  EXPECT_EQ(p.realized(), money(100 * 15 + 50 * 5));
  EXPECT_EQ(p.lots(), (std::deque<Lot>{{50, px(20)}}));
}

TEST(Position, ShortsMirrorLongs) {
  Position p;
  p.apply(Side::Sell, 100, px(50));
  EXPECT_EQ(p.netQty(), -100);
  EXPECT_EQ(p.unrealized(px(45)), money(500)); // price fell: a short gains
  p.apply(Side::Buy, 40, px(42));
  EXPECT_EQ(p.realized(), money(40 * 8));
  EXPECT_EQ(p.netQty(), -60);
  expectIdentity(p, px(47));
}

TEST(Position, FlipsFromLongToShort) {
  Position p;
  p.apply(Side::Buy, 100, px(10));
  p.apply(Side::Sell, 130, px(12)); // closes 100, then short 30 @ 12
  EXPECT_EQ(p.realized(), money(200));
  EXPECT_EQ(p.netQty(), -30);
  EXPECT_EQ(p.lots(), (std::deque<Lot>{{30, px(12)}}));
  p.apply(Side::Buy, 50, px(11)); // closes the short 30 (+30), then long 20 @ 11
  EXPECT_EQ(p.realized(), money(230));
  EXPECT_EQ(p.netQty(), 20);
  EXPECT_EQ(p.lots(), (std::deque<Lot>{{20, px(11)}}));
}

TEST(Position, ClosingExactlyLeavesItFlat) {
  Position p;
  p.apply(Side::Buy, 10, px(5));
  p.apply(Side::Sell, 10, px(4));
  EXPECT_EQ(p.netQty(), 0);
  EXPECT_TRUE(p.lots().empty());
  EXPECT_EQ(p.realized(), -money(10));
  EXPECT_EQ(p.unrealized(px(100)), 0);
  EXPECT_EQ(p.averageCost(), std::nullopt);
}

TEST(Position, AverageCostIsForDisplayAndRounds) {
  Position p;
  p.apply(Side::Buy, 1, 10);
  p.apply(Side::Buy, 2, 11); // (10 + 22) / 3 = 10.67, rounds to 11
  EXPECT_EQ(p.averageCost(), 11);
  p.apply(Side::Buy, 1, 9); // (32 + 9) / 4 = 10.25, rounds to 10
  EXPECT_EQ(p.averageCost(), 10);
}

TEST(Position, RefusesNonsenseFills) {
  Position p;
  EXPECT_THROW(p.apply(Side::Buy, 0, px(1)), std::invalid_argument);
  EXPECT_THROW(p.apply(Side::Buy, -5, px(1)), std::invalid_argument);
  EXPECT_THROW(p.apply(Side::Buy, 5, 0), std::invalid_argument);
  EXPECT_EQ(p, Position{}); // unchanged
}

TEST(Position, HugeValuesDontOverflow) {
  Position p;
  const PriceRaw big = std::numeric_limits<std::int64_t>::max() / 2;
  p.apply(Side::Buy, 1'000'000'000, big); // price × qty is far beyond 64 bits
  p.apply(Side::Sell, 1'000'000'000, big - 1);
  EXPECT_EQ(p.realized(), -Int128{1'000'000'000});
  EXPECT_EQ(p.netQty(), 0);
}

TEST(Position, RestoreRebuildsExactlyAndChecks) {
  Position p;
  p.apply(Side::Sell, 70, px(9));
  p.apply(Side::Sell, 30, px(10));
  p.apply(Side::Buy, 20, px(8));
  const Position copy = Position::restore(p.netQty(), p.realized(), p.cash(), p.lots());
  EXPECT_EQ(copy, p);
  EXPECT_THROW(Position::restore(10, 0, 0, {{5, px(1)}}), std::invalid_argument);  // lots don't add up
  EXPECT_THROW(Position::restore(5, 0, 0, {{5, 0}}), std::invalid_argument);         // bad lot
}

// A random walk of buys and sells (fixed seed): after every fill the identity must hold,
// the lots must add up to the net quantity, and all lots must be on the position's side.
TEST(PositionModel, RandomFillsKeepTheInvariants) {
  std::mt19937_64 rng(2024);
  for (int run = 0; run < 50; ++run) {
    Position p;
    for (int i = 0; i < 2'000; ++i) {
      const Side side = rng() % 2 ? Side::Buy : Side::Sell;
      const Qty qty = 1 + static_cast<Qty>(rng() % 300);
      const PriceRaw price = 1 + static_cast<PriceRaw>(rng() % 2'000'000);
      p.apply(side, qty, price);
      Qty sum = 0;
      for (const Lot& lot : p.lots()) {
        ASSERT_GT(lot.qty, 0);
        sum += lot.qty;
      }
      ASSERT_EQ(sum, p.netQty() < 0 ? -p.netQty() : p.netQty());
      const PriceRaw mark = 1 + static_cast<PriceRaw>(rng() % 2'000'000);
      ASSERT_EQ(p.realized() + p.unrealized(mark), p.cash() + Int128{p.netQty()} * mark);
    }
  }
}

TEST(PositionBook, KeepsStrategiesAndSymbolsApart) {
  PositionBook book;
  book.apply(Fill{1, 1, 0, "A", "X", Side::Buy, 10, px(5)});
  book.apply(Fill{2, 2, 0, "B", "X", Side::Sell, 10, px(5)});
  book.apply(Fill{3, 3, 0, "A", "Y", Side::Buy, 1, px(7)});
  ASSERT_EQ(book.positions().size(), 3u);
  EXPECT_EQ(book.positions().at({"A", "X"}).netQty(), 10);
  EXPECT_EQ(book.positions().at({"B", "X"}).netQty(), -10);
  EXPECT_EQ(book.unrealized({"A", "X"}), 0); // no mark yet
  book.setMark("X", px(6));
  EXPECT_EQ(book.unrealized({"A", "X"}), money(10));
  EXPECT_EQ(book.unrealized({"B", "X"}), -money(10));
  EXPECT_EQ(book.fillsApplied(), 3u);
}

} // namespace
} // namespace postrack
