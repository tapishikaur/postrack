#include "postrack/position.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace postrack {

void Position::apply(Side side, Qty qty, PriceRaw price) {
  if (qty <= 0) throw std::invalid_argument("fill quantity must be positive");
  if (price <= 0) throw std::invalid_argument("fill price must be positive");
  const Int128 notional = Int128{price} * qty;
  const Qty sign = side == Side::Buy ? 1 : -1;
  cash_ += side == Side::Buy ? -notional : notional;

  // Flat, or adding to the position: a new lot at the back.
  if (net_ == 0 || (net_ > 0) == (sign > 0)) {
    lots_.push_back({qty, price});
    net_ += sign * qty;
    return;
  }

  // Closing, oldest lot first.
  Qty remaining = qty;
  while (remaining > 0 && !lots_.empty()) {
    Lot& lot = lots_.front();
    const Qty closed = std::min(remaining, lot.qty);
    const bool longLot = net_ > 0;
    const Int128 perShare = longLot ? Int128{price} - lot.price : Int128{lot.price} - price;
    realized_ += perShare * closed;
    lot.qty -= closed;
    remaining -= closed;
    net_ += sign * closed;
    if (lot.qty == 0) lots_.pop_front();
  }

  // Closed everything and there's more: the position flips.
  if (remaining > 0) {
    lots_.push_back({remaining, price});
    net_ += sign * remaining;
  }
}

Int128 Position::unrealized(PriceRaw mark) const {
  Int128 total = 0;
  const bool longLots = net_ > 0;
  for (const Lot& lot : lots_) {
    const Int128 perShare = longLots ? Int128{mark} - lot.price : Int128{lot.price} - mark;
    total += perShare * lot.qty;
  }
  return total;
}

std::optional<PriceRaw> Position::averageCost() const {
  if (lots_.empty()) return std::nullopt;
  Int128 notional = 0;
  Int128 shares = 0;
  for (const Lot& lot : lots_) {
    notional += Int128{lot.price} * lot.qty;
    shares += lot.qty;
  }
  Int128 avg = notional / shares;
  if ((notional % shares) * 2 >= shares) ++avg; // halves round up (all values positive)
  return static_cast<PriceRaw>(avg);
}

Position Position::restore(Qty net, Int128 realized, Int128 cash, std::deque<Lot> lots) {
  Qty sum = 0;
  for (const Lot& lot : lots) {
    if (lot.qty <= 0 || lot.price <= 0) throw std::invalid_argument("snapshot lot must be positive");
    sum += lot.qty;
  }
  if (sum != (net < 0 ? -net : net)) throw std::invalid_argument("snapshot lots don't add up to the net quantity");
  Position p;
  p.lots_ = std::move(lots);
  p.net_ = net;
  p.realized_ = realized;
  p.cash_ = cash;
  return p;
}

void PositionBook::apply(const Fill& fill) {
  positions_[PositionKey{fill.strategy, fill.symbol}].apply(fill.side, fill.qty, fill.price);
  ++fillsApplied_;
}

void PositionBook::setMark(const std::string& symbol, PriceRaw price) { marks_[symbol] = price; }

std::optional<PriceRaw> PositionBook::mark(const std::string& symbol) const {
  const auto found = marks_.find(symbol);
  if (found == marks_.end()) return std::nullopt;
  return found->second;
}

Int128 PositionBook::unrealized(const PositionKey& key) const {
  const auto position = positions_.find(key);
  const std::optional<PriceRaw> m = mark(key.symbol);
  if (position == positions_.end() || !m) return 0;
  return position->second.unrealized(*m);
}

} // namespace postrack
