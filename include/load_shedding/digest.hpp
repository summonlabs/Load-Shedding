#pragma once

// SHA-256 digests. Digests are the canonical comparison unit for "equal inputs
// produce equal plans" and the integrity unit for durable state.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "load_shedding/status.hpp"

namespace load_shedding {

class Digest {
 public:
  static constexpr std::size_t kBytes = 32;
  static constexpr std::size_t kHexLength = 64;

  constexpr Digest() noexcept = default;

  static Digest from_bytes(const std::array<std::uint8_t, kBytes>& bytes) noexcept;
  static Result<Digest> from_hex(std::string_view hex);

  /// SHA-256 of the exact bytes of `content`.
  static Digest of(std::string_view content) noexcept;

  constexpr const std::array<std::uint8_t, kBytes>& bytes() const noexcept { return bytes_; }
  constexpr bool is_zero() const noexcept {
    for (std::size_t index = 0; index < kBytes; ++index) {
      if (bytes_[index] != 0) {
        return false;
      }
    }
    return true;
  }

  /// Lowercase hexadecimal, always exactly 64 characters.
  std::string hex() const;

  friend constexpr bool operator==(const Digest& a, const Digest& b) noexcept {
    return a.bytes_ == b.bytes_;
  }
  friend constexpr bool operator!=(const Digest& a, const Digest& b) noexcept {
    return !(a.bytes_ == b.bytes_);
  }
  friend constexpr bool operator<(const Digest& a, const Digest& b) noexcept {
    return a.bytes_ < b.bytes_;
  }

 private:
  std::array<std::uint8_t, kBytes> bytes_{};
};

/// Streaming SHA-256. Exposed because the store hashes content as it is written
/// without buffering a second copy.
class Sha256 {
 public:
  static constexpr std::size_t kBlockBytes = 64;

  Sha256() noexcept;
  Sha256(const Sha256&) = delete;
  Sha256& operator=(const Sha256&) = delete;

  void update(const void* data, std::size_t size) noexcept;
  void update(std::string_view data) noexcept { update(data.data(), data.size()); }

  /// Finalizes and returns the digest. The object must not be updated afterwards.
  Digest finish() noexcept;

 private:
  void compress(const std::uint8_t* block) noexcept;

  std::array<std::uint32_t, 8> state_{};
  std::array<std::uint8_t, kBlockBytes> buffer_{};
  std::uint64_t total_bytes_ = 0;
  std::size_t buffered_ = 0;
  bool finalized_ = false;
};

}  // namespace load_shedding
