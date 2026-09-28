#include "detail/codec.hpp"

namespace load_shedding::detail {

void Writer::u16(std::uint16_t value) {
  u8(static_cast<std::uint8_t>(value & 0xFFu));
  u8(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
}

void Writer::u32(std::uint32_t value) {
  for (int index = 0; index < 4; ++index) {
    u8(static_cast<std::uint8_t>((value >> (8 * index)) & 0xFFu));
  }
}

void Writer::u64(std::uint64_t value) {
  for (int index = 0; index < 8; ++index) {
    u8(static_cast<std::uint8_t>((value >> (8 * index)) & 0xFFu));
  }
}

Status Writer::text(std::string_view value, std::size_t max_bytes, const char* what) {
  if (value.size() > max_bytes) {
    return Status::error(StatusCode::Overlong,
                         std::string(what == nullptr ? "field" : what) + " exceeds " +
                             std::to_string(max_bytes) + " bytes");
  }
  u32(static_cast<std::uint32_t>(value.size()));
  bytes(value);
  return Status::success();
}

Status Reader::need(std::size_t size) const {
  if (data_.size() - position_ < size) {
    return Status::error(StatusCode::Truncated,
                         "encoded content ended after " + std::to_string(position_) +
                             " bytes, " + std::to_string(size) + " more were required");
  }
  return Status::success();
}

Result<std::uint8_t> Reader::u8() {
  const Status available = need(1);
  if (!available.ok()) {
    return available;
  }
  return static_cast<std::uint8_t>(static_cast<unsigned char>(data_[position_++]));
}

Result<std::uint16_t> Reader::u16() {
  auto low = u8();
  if (!low.ok()) {
    return low.status();
  }
  auto high = u8();
  if (!high.ok()) {
    return high.status();
  }
  return static_cast<std::uint16_t>(static_cast<std::uint16_t>(low.value()) |
                                    static_cast<std::uint16_t>(high.value() << 8));
}

Result<std::uint32_t> Reader::u32() {
  std::uint32_t value = 0;
  for (int index = 0; index < 4; ++index) {
    auto byte = u8();
    if (!byte.ok()) {
      return byte.status();
    }
    value |= static_cast<std::uint32_t>(byte.value()) << (8 * index);
  }
  return value;
}

Result<std::uint64_t> Reader::u64() {
  std::uint64_t value = 0;
  for (int index = 0; index < 8; ++index) {
    auto byte = u8();
    if (!byte.ok()) {
      return byte.status();
    }
    value |= static_cast<std::uint64_t>(byte.value()) << (8 * index);
  }
  return value;
}

Result<std::int64_t> Reader::i64() {
  auto value = u64();
  if (!value.ok()) {
    return value.status();
  }
  return static_cast<std::int64_t>(value.value());
}

Result<bool> Reader::boolean() {
  auto byte = u8();
  if (!byte.ok()) {
    return byte.status();
  }
  if (byte.value() > 1u) {
    return Status::error(StatusCode::Corrupt,
                         "boolean field encoded as " + std::to_string(byte.value()));
  }
  return byte.value() == 1u;
}

Result<std::string> Reader::text(std::size_t max_bytes, const char* what) {
  auto length = u32();
  if (!length.ok()) {
    return length.status();
  }
  const std::size_t size = static_cast<std::size_t>(length.value());
  if (size > max_bytes) {
    return Status::error(StatusCode::Overlong,
                         std::string(what == nullptr ? "field" : what) + " declares " +
                             std::to_string(size) + " bytes, above the bound of " +
                             std::to_string(max_bytes));
  }
  auto content = raw(size);
  if (!content.ok()) {
    return content.status();
  }
  return std::string(content.value());
}

Result<std::string_view> Reader::raw(std::size_t size) {
  const Status available = need(size);
  if (!available.ok()) {
    return available;
  }
  const std::string_view slice = data_.substr(position_, size);
  position_ += size;
  return slice;
}

Status Reader::expect_end() const {
  if (position_ != data_.size()) {
    return Status::error(StatusCode::Corrupt,
                         "encoded content has " + std::to_string(data_.size() - position_) +
                             " trailing bytes");
  }
  return Status::success();
}

}  // namespace load_shedding::detail
