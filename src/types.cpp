#include "postrack/types.hpp"

#include <algorithm>
#include <limits>

namespace postrack {

std::optional<PriceRaw> parsePrice(std::string_view text) {
  if (text.empty()) return std::nullopt;
  const std::size_t dot = text.find('.');
  const std::string_view whole = text.substr(0, dot);
  const std::string_view frac = dot == std::string_view::npos ? std::string_view{} : text.substr(dot + 1);
  if (whole.empty() && frac.empty()) return std::nullopt;
  if (dot != std::string_view::npos && frac.empty()) return std::nullopt; // "150."

  constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
  std::int64_t value = 0;
  for (const char c : whole) {
    if (c < '0' || c > '9') return std::nullopt;
    if (value > (kMax - (c - '0')) / 10) return std::nullopt;
    value = value * 10 + (c - '0');
  }
  if (value > kMax / kPriceScale) return std::nullopt;
  value *= kPriceScale;

  std::int64_t place = kPriceScale / 10;
  for (std::size_t i = 0; i < frac.size(); ++i) {
    const char c = frac[i];
    if (c < '0' || c > '9') return std::nullopt;
    if (i >= 4) {
      if (c != '0') return std::nullopt; // would need rounding: refuse
      continue;
    }
    const std::int64_t add = (c - '0') * place;
    if (value > kMax - add) return std::nullopt;
    value += add;
    place /= 10;
  }
  if (value <= 0) return std::nullopt;
  return value;
}

std::string formatPrice(PriceRaw raw) { return formatMoney(raw); }

std::string toString(Int128 value) {
  if (value == 0) return "0";
  const bool negative = value < 0;
  // Work with the negative magnitude so the most negative value doesn't overflow.
  Int128 v = negative ? value : -value;
  std::string digits;
  while (v != 0) {
    digits.push_back(static_cast<char>('0' - static_cast<int>(v % 10)));
    v /= 10;
  }
  if (negative) digits.push_back('-');
  std::reverse(digits.begin(), digits.end());
  return digits;
}

std::optional<Int128> parseInt128(std::string_view text) {
  bool negative = false;
  if (!text.empty() && (text.front() == '-' || text.front() == '+')) {
    negative = text.front() == '-';
    text.remove_prefix(1);
  }
  if (text.empty() || text.size() > 39) return std::nullopt;
  constexpr Int128 kMin = -(Int128{1} << 126) * 2; // -2^127
  Int128 value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') return std::nullopt;
    const int d = c - '0';
    // Accumulate as a negative number, which covers the full range. Division truncates
    // toward zero, so this is exactly "value * 10 - d >= kMin".
    if (value < (kMin + d) / 10) return std::nullopt;
    value = value * 10 - d;
  }
  if (!negative) {
    if (value == kMin) return std::nullopt; // -2^127 has no positive twin
    value = -value;
  }
  return value;
}

std::string formatMoney(Int128 raw) {
  const bool negative = raw < 0;
  std::string digits = toString(raw);
  if (negative) digits.erase(0, 1);
  while (digits.size() < 5) digits.insert(0, "0");
  std::string whole = digits.substr(0, digits.size() - 4);
  std::string frac = digits.substr(digits.size() - 4);
  while (!frac.empty() && frac.back() == '0') frac.pop_back();
  std::string out = negative ? "-" : "";
  out += whole;
  if (!frac.empty()) out += "." + frac;
  return out;
}

} // namespace postrack
