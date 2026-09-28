#pragma once

// CRC-32C (Castagnoli). Used for fixed-size store headers, where it is a cheap
// truncation and corruption check; whole payloads are covered by SHA-256.
//
// Internal header: not installed.

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace load_shedding::detail {

std::uint32_t crc32c(const void* data, std::size_t size) noexcept;
std::uint32_t crc32c(std::string_view data) noexcept;

}  // namespace load_shedding::detail
