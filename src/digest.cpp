#include "load_shedding/digest.hpp"

#include <cstring>

namespace load_shedding {
namespace {

constexpr std::array<std::uint32_t, 64> kRoundConstants = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u};

constexpr std::array<std::uint32_t, 8> kInitialState = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u,
                                                        0xa54ff53au, 0x510e527fu, 0x9b05688cu,
                                                        0x1f83d9abu, 0x5be0cd19u};

constexpr std::uint32_t rotate_right(std::uint32_t value, unsigned count) noexcept {
  return (value >> count) | (value << (32u - count));
}

constexpr std::uint32_t choose(std::uint32_t x, std::uint32_t y, std::uint32_t z) noexcept {
  return (x & y) ^ (~x & z);
}

constexpr std::uint32_t majority(std::uint32_t x, std::uint32_t y, std::uint32_t z) noexcept {
  return (x & y) ^ (x & z) ^ (y & z);
}

constexpr std::uint32_t big_sigma0(std::uint32_t value) noexcept {
  return rotate_right(value, 2) ^ rotate_right(value, 13) ^ rotate_right(value, 22);
}

constexpr std::uint32_t big_sigma1(std::uint32_t value) noexcept {
  return rotate_right(value, 6) ^ rotate_right(value, 11) ^ rotate_right(value, 25);
}

constexpr std::uint32_t small_sigma0(std::uint32_t value) noexcept {
  return rotate_right(value, 7) ^ rotate_right(value, 18) ^ (value >> 3);
}

constexpr std::uint32_t small_sigma1(std::uint32_t value) noexcept {
  return rotate_right(value, 17) ^ rotate_right(value, 19) ^ (value >> 10);
}

constexpr char kHexDigits[] = "0123456789abcdef";

int hex_value(char character) noexcept {
  if (character >= '0' && character <= '9') {
    return character - '0';
  }
  if (character >= 'a' && character <= 'f') {
    return character - 'a' + 10;
  }
  if (character >= 'A' && character <= 'F') {
    return character - 'A' + 10;
  }
  return -1;
}

}  // namespace

Sha256::Sha256() noexcept : state_(kInitialState) {}

void Sha256::compress(const std::uint8_t* block) noexcept {
  std::uint32_t schedule[64];
  for (std::size_t index = 0; index < 16; ++index) {
    schedule[index] = (static_cast<std::uint32_t>(block[index * 4]) << 24) |
                      (static_cast<std::uint32_t>(block[index * 4 + 1]) << 16) |
                      (static_cast<std::uint32_t>(block[index * 4 + 2]) << 8) |
                      static_cast<std::uint32_t>(block[index * 4 + 3]);
  }
  for (std::size_t index = 16; index < 64; ++index) {
    schedule[index] = small_sigma1(schedule[index - 2]) + schedule[index - 7] +
                      small_sigma0(schedule[index - 15]) + schedule[index - 16];
  }

  std::uint32_t a = state_[0];
  std::uint32_t b = state_[1];
  std::uint32_t c = state_[2];
  std::uint32_t d = state_[3];
  std::uint32_t e = state_[4];
  std::uint32_t f = state_[5];
  std::uint32_t g = state_[6];
  std::uint32_t h = state_[7];

  for (std::size_t index = 0; index < 64; ++index) {
    const std::uint32_t temp1 =
        h + big_sigma1(e) + choose(e, f, g) + kRoundConstants[index] + schedule[index];
    const std::uint32_t temp2 = big_sigma0(a) + majority(a, b, c);
    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }

  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

void Sha256::update(const void* data, std::size_t size) noexcept {
  if (finalized_ || size == 0) {
    return;
  }
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  total_bytes_ += static_cast<std::uint64_t>(size);
  std::size_t offset = 0;
  if (buffered_ != 0) {
    const std::size_t needed = kBlockBytes - buffered_;
    const std::size_t take = size < needed ? size : needed;
    std::memcpy(buffer_.data() + buffered_, bytes, take);
    buffered_ += take;
    offset += take;
    if (buffered_ == kBlockBytes) {
      compress(buffer_.data());
      buffered_ = 0;
    }
  }
  while (size - offset >= kBlockBytes) {
    compress(bytes + offset);
    offset += kBlockBytes;
  }
  if (offset < size) {
    const std::size_t remaining = size - offset;
    std::memcpy(buffer_.data(), bytes + offset, remaining);
    buffered_ = remaining;
  }
}

Digest Sha256::finish() noexcept {
  if (!finalized_) {
    const std::uint64_t bit_length = total_bytes_ * 8u;
    std::uint8_t padding[kBlockBytes * 2] = {};
    padding[0] = 0x80u;
    std::size_t padding_length = buffered_ < 56 ? 56 - buffered_ : 120 - buffered_;
    for (std::size_t index = 0; index < 8; ++index) {
      padding[padding_length + index] =
          static_cast<std::uint8_t>(bit_length >> (56u - 8u * static_cast<unsigned>(index)));
    }
    update(padding, padding_length + 8);
    finalized_ = true;
  }
  std::array<std::uint8_t, Digest::kBytes> bytes{};
  for (std::size_t index = 0; index < 8; ++index) {
    bytes[index * 4] = static_cast<std::uint8_t>(state_[index] >> 24);
    bytes[index * 4 + 1] = static_cast<std::uint8_t>(state_[index] >> 16);
    bytes[index * 4 + 2] = static_cast<std::uint8_t>(state_[index] >> 8);
    bytes[index * 4 + 3] = static_cast<std::uint8_t>(state_[index]);
  }
  return Digest::from_bytes(bytes);
}

Digest Digest::from_bytes(const std::array<std::uint8_t, kBytes>& bytes) noexcept {
  Digest digest;
  digest.bytes_ = bytes;
  return digest;
}

Result<Digest> Digest::from_hex(std::string_view hex) {
  if (hex.size() != kHexLength) {
    return Status::error(StatusCode::InvalidArgument,
                         "digest hex must be exactly " + std::to_string(kHexLength) +
                             " characters, got " + std::to_string(hex.size()));
  }
  Digest digest;
  for (std::size_t index = 0; index < kBytes; ++index) {
    const int high = hex_value(hex[index * 2]);
    const int low = hex_value(hex[index * 2 + 1]);
    if (high < 0 || low < 0) {
      return Status::error(StatusCode::InvalidArgument, "digest hex contains a non-hexadecimal character");
    }
    digest.bytes_[index] = static_cast<std::uint8_t>((high << 4) | low);
  }
  return digest;
}

Digest Digest::of(std::string_view content) noexcept {
  Sha256 hasher;
  hasher.update(content);
  return hasher.finish();
}

std::string Digest::hex() const {
  std::string result;
  result.resize(kHexLength);
  for (std::size_t index = 0; index < kBytes; ++index) {
    result[index * 2] = kHexDigits[bytes_[index] >> 4];
    result[index * 2 + 1] = kHexDigits[bytes_[index] & 0x0Fu];
  }
  return result;
}

}  // namespace load_shedding
