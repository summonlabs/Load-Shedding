#pragma once

// Canonical binary codec primitives. The layout is fixed-width and explicit
// little-endian: every multi-byte field is written one byte at a time, so the
// encoding does not depend on host endianness or on struct padding.
//
// This header is internal and is not installed.

#include <cstdint>
#include <string>
#include <string_view>

#include "load_shedding/status.hpp"

namespace load_shedding::detail {

class Writer {
 public:
  void raw(const void* data, std::size_t size) { buffer_.append(static_cast<const char*>(data), size); }
  void bytes(std::string_view data) { buffer_.append(data); }

  void u8(std::uint8_t value) { buffer_.push_back(static_cast<char>(value)); }
  void u16(std::uint16_t value);
  void u32(std::uint32_t value);
  void u64(std::uint64_t value);
  void i64(std::int64_t value) { u64(static_cast<std::uint64_t>(value)); }
  void boolean(bool value) { u8(value ? 1u : 0u); }

  /// Length-prefixed byte string. The length is validated before it is written.
  Status text(std::string_view value, std::size_t max_bytes, const char* what);

  const std::string& bytes_ref() const noexcept { return buffer_; }
  std::string take() { return std::move(buffer_); }
  std::size_t size() const noexcept { return buffer_.size(); }

 private:
  std::string buffer_;
};

class Reader {
 public:
  explicit Reader(std::string_view data) : data_(data) {}

  Result<std::uint8_t> u8();
  Result<std::uint16_t> u16();
  Result<std::uint32_t> u32();
  Result<std::uint64_t> u64();
  Result<std::int64_t> i64();
  Result<bool> boolean();

  /// Reads a length-prefixed byte string. Refuses a length above `max_bytes`
  /// before allocating anything.
  Result<std::string> text(std::size_t max_bytes, const char* what);

  Result<std::string_view> raw(std::size_t size);

  std::size_t remaining() const noexcept { return data_.size() - position_; }
  std::size_t position() const noexcept { return position_; }

  /// Fails when unread bytes remain. Used to refuse trailing garbage.
  Status expect_end() const;

 private:
  Status need(std::size_t size) const;

  std::string_view data_;
  std::size_t position_ = 0;
};

}  // namespace load_shedding::detail
