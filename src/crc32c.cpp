#include "postrack/crc32c.hpp"

#include <array>

namespace postrack {
namespace {

// Table-driven, reflected polynomial 0x82F63B78. Built at compile time.
constexpr std::array<std::uint32_t, 256> makeTable() {
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t i = 0; i < 256; ++i) {
    std::uint32_t c = i;
    for (int k = 0; k < 8; ++k) c = (c & 1) ? (c >> 1) ^ 0x82F63B78u : c >> 1;
    table[i] = c;
  }
  return table;
}
constexpr std::array<std::uint32_t, 256> kTable = makeTable();

} // namespace

std::uint32_t crc32c(const void* data, std::size_t size) {
  const auto* p = static_cast<const std::uint8_t*>(data);
  std::uint32_t crc = 0xFFFFFFFFu;
  for (std::size_t i = 0; i < size; ++i) crc = kTable[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
  return crc ^ 0xFFFFFFFFu;
}

} // namespace postrack
