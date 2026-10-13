#pragma once
// Positions and P&L with FIFO tax lots, exactly as docs/accounting.md describes.

#include "postrack/types.hpp"

#include <compare>
#include <cstddef>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace postrack {

// A chunk bought (or sold short) at one price. qty is always positive; whether it is
// long or short is the position's direction.
struct Lot {
  Qty qty = 0;
  PriceRaw price = 0;
  bool operator==(const Lot&) const = default;
};

// One (strategy, symbol) position.
class Position {
public:
  Position() = default;

  // Applies one fill. Throws std::invalid_argument for qty <= 0 or price <= 0.
  void apply(Side side, Qty qty, PriceRaw price);

  Qty netQty() const { return net_; }       // > 0 long, < 0 short, 0 flat
  Int128 realized() const { return realized_; }
  Int128 cash() const { return cash_; }      // sells minus buys, in price units × shares
  Int128 unrealized(PriceRaw mark) const;    // at a given mark
  const std::deque<Lot>& lots() const { return lots_; }

  // Display only: never used in any P&L. nullopt when flat.
  std::optional<PriceRaw> averageCost() const;

  // Rebuilds a position from a snapshot. Throws std::invalid_argument if the parts don't
  // agree (lots that don't add up to net, a non-positive lot).
  static Position restore(Qty net, Int128 realized, Int128 cash, std::deque<Lot> lots);

  bool operator==(const Position&) const = default;

private:
  std::deque<Lot> lots_; // oldest first
  Qty net_ = 0;
  Int128 realized_ = 0;
  Int128 cash_ = 0;
};

struct PositionKey {
  std::string strategy;
  std::string symbol;
  auto operator<=>(const PositionKey&) const = default;
};

// Every position, plus the latest mark per symbol. Ordered maps, so iterating (reports,
// snapshots) is deterministic.
class PositionBook {
public:
  void apply(const Fill& fill);
  void setMark(const std::string& symbol, PriceRaw price);
  std::optional<PriceRaw> mark(const std::string& symbol) const;
  Int128 unrealized(const PositionKey& key) const; // 0 if no mark yet for the symbol

  const std::map<PositionKey, Position>& positions() const { return positions_; }
  std::map<PositionKey, Position>& positions() { return positions_; } // for restore
  const std::map<std::string, PriceRaw>& marks() const { return marks_; }
  std::size_t fillsApplied() const { return fillsApplied_; }

  bool operator==(const PositionBook& other) const { return positions_ == other.positions_; }

private:
  std::map<PositionKey, Position> positions_;
  std::map<std::string, PriceRaw> marks_;
  std::size_t fillsApplied_ = 0;
};

} // namespace postrack
