#pragma once
// CRC-32C (Castagnoli), as used by the fill log. crc32c("123456789") == 0xE3069283.

#include <cstddef>
#include <cstdint>

namespace postrack {

std::uint32_t crc32c(const void* data, std::size_t size);

} // namespace postrack
