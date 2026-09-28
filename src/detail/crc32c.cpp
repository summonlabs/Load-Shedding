#include "detail/crc32c.hpp"

namespace load_shedding::detail {
namespace {

constexpr std::uint32_t kPolynomial = 0x82F63B78u;  // reflected 0x1EDC6F41

struct Table {
  std::uint32_t entries[256];

  constexpr Table() : entries() {
    for (std::uint32_t index = 0; index < 256; ++index) {
      std::uint32_t value = index;
      for (int bit = 0; bit < 8; ++bit) {
        value = (value & 1u) != 0u ? (value >> 1) ^ kPolynomial : value >> 1;
      }
      entries[index] = value;
    }
  }
};

constexpr Table kTable{};

}  // namespace

std::uint32_t crc32c(const void* data, std::size_t size) noexcept {
  const auto* bytes = static_cast<const unsigned char*>(data);
  std::uint32_t crc = 0xFFFFFFFFu;
  for (std::size_t index = 0; index < size; ++index) {
    crc = kTable.entries[(crc ^ bytes[index]) & 0xFFu] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFFu;
}

std::uint32_t crc32c(std::string_view data) noexcept {
  return crc32c(data.data(), data.size());
}

}  // namespace load_shedding::detail
