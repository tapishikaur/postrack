#include "postrack/types.hpp"

#include <gtest/gtest.h>

namespace postrack {
namespace {

TEST(Price, ParsesExactlyOrRefuses) {
  EXPECT_EQ(parsePrice("150.25"), 1'502'500);
  EXPECT_EQ(parsePrice("150"), 1'500'000);
  EXPECT_EQ(parsePrice("0.0001"), 1);
  EXPECT_EQ(parsePrice("1.23450"), 12'345); // trailing zero past the 4th place is fine
  EXPECT_EQ(parsePrice("1.23456"), std::nullopt); // would need rounding
  EXPECT_EQ(parsePrice("0"), std::nullopt);       // prices are positive
  EXPECT_EQ(parsePrice("-1"), std::nullopt);
  EXPECT_EQ(parsePrice("1."), std::nullopt);
  EXPECT_EQ(parsePrice(""), std::nullopt);
  EXPECT_EQ(parsePrice("1e3"), std::nullopt);
  EXPECT_EQ(parsePrice("99999999999999999999"), std::nullopt); // doesn't fit
}

TEST(Price, FormatsWithAsFewDecimalsAsNeeded) {
  EXPECT_EQ(formatPrice(1'502'500), "150.25");
  EXPECT_EQ(formatPrice(1'500'000), "150");
  EXPECT_EQ(formatPrice(1), "0.0001");
  for (const char* text : {"150.25", "0.0001", "7", "123456.7891"}) EXPECT_EQ(formatPrice(*parsePrice(text)), text);
}

TEST(Int128Text, RoundTripsTheWholeRange) {
  const Int128 max = ~(Int128{1} << 127);
  const Int128 min = -max - 1;
  for (const Int128 v : {Int128{0}, Int128{-1}, Int128{42}, Int128{-123456789}, max, min}) {
    EXPECT_EQ(parseInt128(toString(v)), v) << toString(v);
  }
  EXPECT_EQ(toString(max), "170141183460469231731687303715884105727");
  EXPECT_EQ(toString(min), "-170141183460469231731687303715884105728");
  EXPECT_EQ(parseInt128("170141183460469231731687303715884105728"), std::nullopt); // max + 1
  EXPECT_EQ(parseInt128("12x"), std::nullopt);
  EXPECT_EQ(parseInt128(""), std::nullopt);
}

TEST(Money, FormatsExactDecimals) {
  EXPECT_EQ(formatMoney(12'345'678), "1234.5678");
  EXPECT_EQ(formatMoney(-50'000), "-5");
  EXPECT_EQ(formatMoney(5), "0.0005");
  EXPECT_EQ(formatMoney(0), "0");
}

} // namespace
} // namespace postrack
