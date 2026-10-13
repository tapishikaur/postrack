#pragma once
// Core value types. Every money amount is an exact integer: prices in 1/10,000ths,
// quantities in whole shares, money (price × quantity) in 128 bits. See docs/accounting.md.

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace postrack {

using Qty = std::int64_t;     // whole shares
using PriceRaw = std::int64_t; // 1/10,000ths: 150.25 is 1'502'500

// 128-bit integer for money: price × quantity summed over a day can overflow 64 bits.
// GCC and Clang provide it; __extension__ keeps -Wpedantic quiet about it.
__extension__ typedef __int128 Int128;

inline constexpr std::int64_t kPriceScale = 10'000;

enum class Side : std::uint8_t { Buy = 1, Sell = 2 };

// One execution, as sent by the upstream. seq numbers every fill 1, 2, 3, ...
struct Fill {
  std::uint64_t seq = 0;
  std::uint64_t execId = 0;
  std::int64_t tsNs = 0;
  std::string strategy;
  std::string symbol;
  Side side = Side::Buy;
  Qty qty = 0;
  PriceRaw price = 0;

  bool operator==(const Fill&) const = default;
};

// Parses a decimal price such as "150.25" or "100". Never rounds: nullopt for malformed
// text, for a non-zero digit past the 4th decimal, for values that don't fit, and for
// prices that aren't positive.
std::optional<PriceRaw> parsePrice(std::string_view text);

// 1'502'500 -> "150.25", 1'500'000 -> "150". Exact.
std::string formatPrice(PriceRaw raw);

// Decimal text of a 128-bit integer, and back. parseInt128 returns nullopt on bad text.
std::string toString(Int128 value);
std::optional<Int128> parseInt128(std::string_view text);

// A money amount in price units, as decimal currency: 12'345'678 -> "1234.5678". Exact.
std::string formatMoney(Int128 raw);

} // namespace postrack
