#include "load_shedding/text.hpp"

#include <string>
#include <vector>

namespace load_shedding {
namespace {

/// Decodes one UTF-8 sequence starting at `index`. Returns the code point and
/// advances `index`. Strict: overlong forms, surrogates, and values above
/// U+10FFFF are rejected.
bool decode_one(std::string_view text, std::size_t& index, std::uint32_t& code_point) noexcept {
  const auto byte = [&](std::size_t offset) {
    return static_cast<std::uint8_t>(static_cast<unsigned char>(text[index + offset]));
  };
  const std::size_t remaining = text.size() - index;
  const std::uint8_t lead = byte(0);
  if (lead < 0x80) {
    code_point = lead;
    index += 1;
    return true;
  }
  if (lead < 0xC2) {
    // 0x80..0xBF are continuation bytes; 0xC0/0xC1 can only start an overlong form.
    return false;
  }
  if (lead < 0xE0) {
    if (remaining < 2 || (byte(1) & 0xC0) != 0x80) {
      return false;
    }
    code_point = (static_cast<std::uint32_t>(lead & 0x1Fu) << 6) |
                 static_cast<std::uint32_t>(byte(1) & 0x3Fu);
    index += 2;
    return true;
  }
  if (lead < 0xF0) {
    if (remaining < 3 || (byte(1) & 0xC0) != 0x80 || (byte(2) & 0xC0) != 0x80) {
      return false;
    }
    const std::uint8_t second = byte(1);
    if (lead == 0xE0 && second < 0xA0) {
      return false;  // overlong
    }
    if (lead == 0xED && second > 0x9F) {
      return false;  // surrogate half
    }
    code_point = (static_cast<std::uint32_t>(lead & 0x0Fu) << 12) |
                 (static_cast<std::uint32_t>(second & 0x3Fu) << 6) |
                 static_cast<std::uint32_t>(byte(2) & 0x3Fu);
    index += 3;
    return true;
  }
  if (lead < 0xF5) {
    if (remaining < 4 || (byte(1) & 0xC0) != 0x80 || (byte(2) & 0xC0) != 0x80 ||
        (byte(3) & 0xC0) != 0x80) {
      return false;
    }
    const std::uint8_t second = byte(1);
    if (lead == 0xF0 && second < 0x90) {
      return false;  // overlong
    }
    if (lead == 0xF4 && second > 0x8F) {
      return false;  // above U+10FFFF
    }
    code_point = (static_cast<std::uint32_t>(lead & 0x07u) << 18) |
                 (static_cast<std::uint32_t>(second & 0x3Fu) << 12) |
                 (static_cast<std::uint32_t>(byte(2) & 0x3Fu) << 6) |
                 static_cast<std::uint32_t>(byte(3) & 0x3Fu);
    index += 4;
    return true;
  }
  return false;
}

bool is_control(std::uint32_t code_point) noexcept {
  return code_point < 0x20u || (code_point >= 0x7Fu && code_point <= 0x9Fu);
}

bool is_reserved_device_name(std::string_view upper) noexcept {
  static constexpr std::string_view kExact[] = {"CON", "PRN", "AUX", "NUL"};
  for (const std::string_view candidate : kExact) {
    if (upper == candidate) {
      return true;
    }
  }
  if (upper.size() == 4) {
    const std::string_view prefix = upper.substr(0, 3);
    const char suffix = upper[3];
    if ((prefix == "COM" || prefix == "LPT") && suffix >= '1' && suffix <= '9') {
      return true;
    }
  }
  return false;
}

std::string ascii_upper(std::string_view text) {
  std::string result;
  result.reserve(text.size());
  for (const char character : text) {
    if (character >= 'a' && character <= 'z') {
      result.push_back(static_cast<char>(character - 'a' + 'A'));
    } else {
      result.push_back(character);
    }
  }
  return result;
}

}  // namespace

bool is_valid_utf8(std::string_view text) noexcept {
  std::size_t index = 0;
  while (index < text.size()) {
    std::uint32_t code_point = 0;
    if (!decode_one(text, index, code_point)) {
      return false;
    }
  }
  return true;
}

Result<std::size_t> utf8_length(std::string_view text) {
  std::size_t index = 0;
  std::size_t count = 0;
  while (index < text.size()) {
    std::uint32_t code_point = 0;
    if (!decode_one(text, index, code_point)) {
      return Status::error(StatusCode::InvalidArgument, "text is not valid UTF-8");
    }
    ++count;
  }
  return count;
}

bool has_no_control_characters(std::string_view text) noexcept {
  std::size_t index = 0;
  while (index < text.size()) {
    std::uint32_t code_point = 0;
    if (!decode_one(text, index, code_point)) {
      return false;
    }
    if (is_control(code_point)) {
      return false;
    }
  }
  return true;
}

bool is_printable_ascii(std::string_view text) noexcept {
  for (const char character : text) {
    const auto byte = static_cast<unsigned char>(character);
    if (byte < 0x21 || byte > 0x7E) {
      return false;
    }
  }
  return true;
}

Status validate_identifier(std::string_view text, std::size_t max_bytes, const char* what) {
  const std::string label = what == nullptr ? "identifier" : what;
  if (text.empty()) {
    return Status::error(StatusCode::InvalidArgument, label + " must not be empty");
  }
  if (text.size() > max_bytes) {
    return Status::error(StatusCode::Overlong, label + " exceeds " + std::to_string(max_bytes) +
                                                    " bytes (got " + std::to_string(text.size()) + ")");
  }
  if (!is_valid_utf8(text)) {
    return Status::error(StatusCode::InvalidArgument, label + " is not valid UTF-8");
  }
  if (!has_no_control_characters(text)) {
    return Status::error(StatusCode::InvalidArgument, label + " contains a control character");
  }
  if (text.front() == ' ' || text.back() == ' ') {
    return Status::error(StatusCode::InvalidArgument, label + " has a leading or trailing space");
  }
  if (text.find('/') != std::string_view::npos || text.find('\\') != std::string_view::npos) {
    return Status::error(StatusCode::InvalidArgument, label + " contains a path separator");
  }
  // "." and ".." are reserved by every filesystem and read as traversal in a log
  // or an operator console, so they are refused as identities even though the
  // library never uses an identity as a path.
  if (text == "." || text == "..") {
    return Status::error(StatusCode::Rejected, label + " must not be a relative traversal name");
  }
  return Status::success();
}

Status validate_text(std::string_view text, std::size_t max_bytes, const char* what) {
  const std::string label = what == nullptr ? "text" : what;
  if (text.size() > max_bytes) {
    return Status::error(StatusCode::Overlong, label + " exceeds " + std::to_string(max_bytes) +
                                                    " bytes (got " + std::to_string(text.size()) + ")");
  }
  if (!is_valid_utf8(text)) {
    return Status::error(StatusCode::InvalidArgument, label + " is not valid UTF-8");
  }
  if (!has_no_control_characters(text)) {
    return Status::error(StatusCode::InvalidArgument, label + " contains a control character");
  }
  return Status::success();
}

Status validate_relative_component(std::string_view text, const char* what) {
  const std::string label = what == nullptr ? "path component" : what;
  if (text.empty()) {
    return Status::error(StatusCode::InvalidArgument, label + " must not be empty");
  }
  if (text == "." || text == "..") {
    return Status::error(StatusCode::Rejected, label + " must not be a relative traversal component");
  }
  if (text.size() > 128) {
    return Status::error(StatusCode::Overlong,
                         label + " exceeds 128 bytes (got " + std::to_string(text.size()) + ")");
  }
  if (!is_valid_utf8(text)) {
    return Status::error(StatusCode::InvalidArgument, label + " is not valid UTF-8");
  }
  for (const char character : text) {
    const auto byte = static_cast<unsigned char>(character);
    if (byte < 0x20 || byte == 0x7F) {
      return Status::error(StatusCode::InvalidArgument, label + " contains a control character");
    }
    if (character == '/' || character == '\\' || character == ':') {
      return Status::error(StatusCode::Rejected,
                           label + " contains a path separator or stream separator");
    }
  }
  if (text.back() == ' ' || text.back() == '.') {
    return Status::error(StatusCode::Rejected,
                         label + " ends with a space or dot, which the platform would rewrite");
  }
  const std::string upper = ascii_upper(text);
  const std::string_view stem =
      upper.find('.') == std::string::npos ? std::string_view(upper) : std::string_view(upper).substr(0, upper.find('.'));
  if (is_reserved_device_name(stem)) {
    return Status::error(StatusCode::Rejected, label + " names a reserved platform device");
  }
  return Status::success();
}

Result<std::string> join_relative_path(std::string_view base, std::string_view component) {
  const Status valid = validate_relative_component(component, "path component");
  if (!valid.ok()) {
    return valid;
  }
  std::string result(base);
  if (!result.empty() && result.back() != '/' && result.back() != '\\') {
    result.push_back('/');
  }
  result.append(component);
  return result;
}

}  // namespace load_shedding
