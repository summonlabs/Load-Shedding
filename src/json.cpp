#include "load_shedding/json.hpp"

#include <limits>
#include <string>
#include <utility>

#include "load_shedding/text.hpp"

namespace load_shedding {
namespace {

std::string position_message(std::size_t position, const std::string& detail) {
  return detail + " at byte " + std::to_string(position);
}

class Parser {
 public:
  Parser(std::string_view text, const JsonLimits& limits) : text_(text), limits_(limits) {}

  Result<JsonValue> parse_document() {
    if (text_.size() > limits_.max_bytes) {
      return Status::error(StatusCode::Overlong,
                           "JSON document of " + std::to_string(text_.size()) +
                               " bytes exceeds the limit of " + std::to_string(limits_.max_bytes));
    }
    skip_whitespace();
    auto value = parse_value(0);
    if (!value.ok()) {
      return value.status();
    }
    skip_whitespace();
    if (position_ != text_.size()) {
      return Status::error(StatusCode::InvalidArgument,
                           position_message(position_, "unexpected trailing content"));
    }
    return value.value();
  }

 private:
  void skip_whitespace() {
    while (position_ < text_.size()) {
      const char character = text_[position_];
      if (character == ' ' || character == '\t' || character == '\n' || character == '\r') {
        ++position_;
      } else {
        break;
      }
    }
  }

  bool consume(char expected) {
    if (position_ < text_.size() && text_[position_] == expected) {
      ++position_;
      return true;
    }
    return false;
  }

  Status literal(std::string_view expected) {
    if (text_.size() - position_ < expected.size() ||
        text_.compare(position_, expected.size(), expected) != 0) {
      return Status::error(StatusCode::InvalidArgument,
                           position_message(position_, "expected '" + std::string(expected) + "'"));
    }
    position_ += expected.size();
    return Status::success();
  }

  Result<JsonValue> parse_value(std::size_t depth) {
    if (depth > limits_.max_depth) {
      return Status::error(StatusCode::Overlong,
                           position_message(position_, "nesting depth exceeds " +
                                                           std::to_string(limits_.max_depth)));
    }
    if (position_ >= text_.size()) {
      return Status::error(StatusCode::Truncated, position_message(position_, "expected a value"));
    }
    switch (text_[position_]) {
      case '{':
        return parse_object(depth);
      case '[':
        return parse_array(depth);
      case '"': {
        auto text = parse_string();
        if (!text.ok()) {
          return text.status();
        }
        return JsonValue::text(std::move(text.value()));
      }
      case 't': {
        const Status status = literal("true");
        if (!status.ok()) {
          return status;
        }
        return JsonValue::boolean(true);
      }
      case 'f': {
        const Status status = literal("false");
        if (!status.ok()) {
          return status;
        }
        return JsonValue::boolean(false);
      }
      case 'n': {
        const Status status = literal("null");
        if (!status.ok()) {
          return status;
        }
        return JsonValue::null();
      }
      default:
        return parse_number();
    }
  }

  Result<JsonValue> parse_object(std::size_t depth) {
    consume('{');
    JsonValue object = JsonValue::object();
    skip_whitespace();
    if (consume('}')) {
      return object;
    }
    std::size_t members = 0;
    while (true) {
      skip_whitespace();
      if (position_ >= text_.size() || text_[position_] != '"') {
        return Status::error(StatusCode::InvalidArgument,
                             position_message(position_, "expected a member name string"));
      }
      auto name = parse_string();
      if (!name.ok()) {
        return name.status();
      }
      skip_whitespace();
      if (!consume(':')) {
        return Status::error(StatusCode::InvalidArgument,
                             position_message(position_, "expected ':' after a member name"));
      }
      skip_whitespace();
      auto value = parse_value(depth + 1);
      if (!value.ok()) {
        return value.status();
      }
      if (object.members().find(name.value()) != object.members().end()) {
        return Status::error(StatusCode::InvalidArgument,
                             position_message(position_, "duplicate member name '" + name.value() + "'"));
      }
      object.set(std::move(name.value()), std::move(value.value()));
      ++members;
      if (members > limits_.max_members) {
        return Status::error(StatusCode::Overlong,
                             position_message(position_, "object exceeds " +
                                                             std::to_string(limits_.max_members) +
                                                             " members"));
      }
      skip_whitespace();
      if (consume(',')) {
        continue;
      }
      if (consume('}')) {
        return object;
      }
      return Status::error(StatusCode::InvalidArgument,
                           position_message(position_, "expected ',' or '}' in object"));
    }
  }

  Result<JsonValue> parse_array(std::size_t depth) {
    consume('[');
    JsonValue array = JsonValue::array();
    skip_whitespace();
    if (consume(']')) {
      return array;
    }
    std::size_t elements = 0;
    while (true) {
      skip_whitespace();
      auto value = parse_value(depth + 1);
      if (!value.ok()) {
        return value.status();
      }
      array.push(std::move(value.value()));
      ++elements;
      if (elements > limits_.max_members) {
        return Status::error(StatusCode::Overlong,
                             position_message(position_, "array exceeds " +
                                                             std::to_string(limits_.max_members) +
                                                             " elements"));
      }
      skip_whitespace();
      if (consume(',')) {
        continue;
      }
      if (consume(']')) {
        return array;
      }
      return Status::error(StatusCode::InvalidArgument,
                           position_message(position_, "expected ',' or ']' in array"));
    }
  }

  Result<JsonValue> parse_number() {
    const std::size_t start = position_;
    bool negative = false;
    if (consume('-')) {
      negative = true;
    }
    if (position_ >= text_.size()) {
      return Status::error(StatusCode::Truncated, position_message(position_, "expected a digit"));
    }
    if (text_[position_] < '0' || text_[position_] > '9') {
      return Status::error(StatusCode::InvalidArgument,
                           position_message(position_, "expected a digit"));
    }
    if (text_[position_] == '0') {
      ++position_;
      if (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') {
        return Status::error(StatusCode::InvalidArgument,
                             position_message(position_, "a leading zero is not allowed"));
      }
    } else {
      while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') {
        ++position_;
      }
    }
    if (position_ < text_.size() &&
        (text_[position_] == '.' || text_[position_] == 'e' || text_[position_] == 'E')) {
      return Status::error(StatusCode::Unsupported,
                           position_message(position_,
                                            "non-integer numbers are not supported by this schema"));
    }
    const std::string_view digits = text_.substr(start, position_ - start);
    // The magnitude limit is 2^63 so that the most negative 64-bit value can be
    // represented; anything larger is refused rather than wrapped.
    constexpr std::uint64_t kMagnitudeLimit = 0x8000000000000000ULL;
    std::uint64_t magnitude_value = 0;
    std::size_t index = negative ? 1 : 0;
    for (; index < digits.size(); ++index) {
      const auto digit = static_cast<std::uint64_t>(digits[index] - '0');
      if (magnitude_value > (kMagnitudeLimit - digit) / 10ULL) {
        return Status::error(StatusCode::Overflow,
                             position_message(start, "integer literal is out of range"));
      }
      magnitude_value = magnitude_value * 10ULL + digit;
    }
    if (negative) {
      if (magnitude_value > kMagnitudeLimit) {
        return Status::error(StatusCode::Overflow,
                             position_message(start, "integer literal is out of range"));
      }
      if (magnitude_value == kMagnitudeLimit) {
        return JsonValue::integer(std::numeric_limits<std::int64_t>::min());
      }
      return JsonValue::integer(-static_cast<std::int64_t>(magnitude_value));
    }
    if (magnitude_value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      return Status::error(StatusCode::Overflow,
                           position_message(start, "integer literal is out of range"));
    }
    return JsonValue::integer(static_cast<std::int64_t>(magnitude_value));
  }

  Result<std::string> parse_string() {
    if (!consume('"')) {
      return Status::error(StatusCode::InvalidArgument, position_message(position_, "expected a string"));
    }
    std::string result;
    while (true) {
      if (position_ >= text_.size()) {
        return Status::error(StatusCode::Truncated, position_message(position_, "unterminated string"));
      }
      const auto byte = static_cast<unsigned char>(text_[position_]);
      if (byte == '"') {
        ++position_;
        break;
      }
      if (byte < 0x20) {
        return Status::error(StatusCode::InvalidArgument,
                             position_message(position_, "unescaped control character in string"));
      }
      if (byte == '\\') {
        ++position_;
        if (position_ >= text_.size()) {
          return Status::error(StatusCode::Truncated,
                               position_message(position_, "unterminated escape sequence"));
        }
        const char escape = text_[position_++];
        switch (escape) {
          case '"': result.push_back('"'); break;
          case '\\': result.push_back('\\'); break;
          case '/': result.push_back('/'); break;
          case 'b': result.push_back('\b'); break;
          case 'f': result.push_back('\f'); break;
          case 'n': result.push_back('\n'); break;
          case 'r': result.push_back('\r'); break;
          case 't': result.push_back('\t'); break;
          case 'u': {
            auto code_point = parse_hex4();
            if (!code_point.ok()) {
              return code_point.status();
            }
            std::uint32_t value = code_point.value();
            if (value >= 0xD800u && value <= 0xDBFFu) {
              if (position_ + 1 >= text_.size() || text_[position_] != '\\' ||
                  text_[position_ + 1] != 'u') {
                return Status::error(StatusCode::InvalidArgument,
                                     position_message(position_,
                                                      "high surrogate without a following low surrogate"));
              }
              position_ += 2;
              auto low = parse_hex4();
              if (!low.ok()) {
                return low.status();
              }
              if (low.value() < 0xDC00u || low.value() > 0xDFFFu) {
                return Status::error(StatusCode::InvalidArgument,
                                     position_message(position_, "invalid low surrogate"));
              }
              value = 0x10000u + ((value - 0xD800u) << 10) + (low.value() - 0xDC00u);
            } else if (value >= 0xDC00u && value <= 0xDFFFu) {
              return Status::error(StatusCode::InvalidArgument,
                                   position_message(position_, "lone low surrogate"));
            }
            append_utf8(result, value);
            break;
          }
          default:
            return Status::error(StatusCode::InvalidArgument,
                                 position_message(position_, "unknown escape sequence"));
        }
        continue;
      }
      result.push_back(static_cast<char>(byte));
      ++position_;
      if (result.size() > limits_.max_string_bytes) {
        return Status::error(StatusCode::Overlong,
                             position_message(position_, "string exceeds " +
                                                             std::to_string(limits_.max_string_bytes) +
                                                             " bytes"));
      }
    }
    if (result.size() > limits_.max_string_bytes) {
      return Status::error(StatusCode::Overlong,
                           position_message(position_, "string exceeds " +
                                                           std::to_string(limits_.max_string_bytes) +
                                                           " bytes"));
    }
    if (!is_valid_utf8(result)) {
      return Status::error(StatusCode::InvalidArgument,
                           position_message(position_, "string is not valid UTF-8"));
    }
    return result;
  }

  Result<std::uint32_t> parse_hex4() {
    if (text_.size() - position_ < 4) {
      return Status::error(StatusCode::Truncated,
                           position_message(position_, "incomplete \\u escape"));
    }
    std::uint32_t value = 0;
    for (int index = 0; index < 4; ++index) {
      const char character = text_[position_ + static_cast<std::size_t>(index)];
      std::uint32_t digit = 0;
      if (character >= '0' && character <= '9') {
        digit = static_cast<std::uint32_t>(character - '0');
      } else if (character >= 'a' && character <= 'f') {
        digit = static_cast<std::uint32_t>(character - 'a' + 10);
      } else if (character >= 'A' && character <= 'F') {
        digit = static_cast<std::uint32_t>(character - 'A' + 10);
      } else {
        return Status::error(StatusCode::InvalidArgument,
                             position_message(position_, "non-hexadecimal digit in \\u escape"));
      }
      value = (value << 4) | digit;
    }
    position_ += 4;
    return value;
  }

  static void append_utf8(std::string& out, std::uint32_t code_point) {
    if (code_point < 0x80u) {
      out.push_back(static_cast<char>(code_point));
    } else if (code_point < 0x800u) {
      out.push_back(static_cast<char>(0xC0u | (code_point >> 6)));
      out.push_back(static_cast<char>(0x80u | (code_point & 0x3Fu)));
    } else if (code_point < 0x10000u) {
      out.push_back(static_cast<char>(0xE0u | (code_point >> 12)));
      out.push_back(static_cast<char>(0x80u | ((code_point >> 6) & 0x3Fu)));
      out.push_back(static_cast<char>(0x80u | (code_point & 0x3Fu)));
    } else {
      out.push_back(static_cast<char>(0xF0u | (code_point >> 18)));
      out.push_back(static_cast<char>(0x80u | ((code_point >> 12) & 0x3Fu)));
      out.push_back(static_cast<char>(0x80u | ((code_point >> 6) & 0x3Fu)));
      out.push_back(static_cast<char>(0x80u | (code_point & 0x3Fu)));
    }
  }

  std::string_view text_;
  const JsonLimits& limits_;
  std::size_t position_ = 0;
};

void write_string(std::string& out, std::string_view text) {
  out.push_back('"');
  for (const char character : text) {
    const auto byte = static_cast<unsigned char>(character);
    switch (character) {
      case '"': out.append("\\\""); break;
      case '\\': out.append("\\\\"); break;
      case '\b': out.append("\\b"); break;
      case '\f': out.append("\\f"); break;
      case '\n': out.append("\\n"); break;
      case '\r': out.append("\\r"); break;
      case '\t': out.append("\\t"); break;
      default:
        if (byte < 0x20 || byte == 0x7F) {
          static const char* kHex = "0123456789abcdef";
          out.append("\\u00");
          out.push_back(kHex[(byte >> 4) & 0x0Fu]);
          out.push_back(kHex[byte & 0x0Fu]);
        } else {
          out.push_back(character);
        }
        break;
    }
  }
  out.push_back('"');
}

void write_compact(std::string& out, const JsonValue& value) {
  switch (value.kind()) {
    case JsonValue::Kind::Null: out.append("null"); break;
    case JsonValue::Kind::Bool: out.append(value.as_bool() ? "true" : "false"); break;
    case JsonValue::Kind::Int: out.append(std::to_string(value.as_int())); break;
    case JsonValue::Kind::String: write_string(out, value.as_string()); break;
    case JsonValue::Kind::Array: {
      out.push_back('[');
      bool first = true;
      for (const JsonValue& item : value.items()) {
        if (!first) {
          out.push_back(',');
        }
        first = false;
        write_compact(out, item);
      }
      out.push_back(']');
      break;
    }
    case JsonValue::Kind::Object: {
      out.push_back('{');
      bool first = true;
      for (const auto& [name, member] : value.members()) {
        if (!first) {
          out.push_back(',');
        }
        first = false;
        write_string(out, name);
        out.push_back(':');
        write_compact(out, member);
      }
      out.push_back('}');
      break;
    }
  }
}

void write_indent(std::string& out, unsigned depth, unsigned indent) {
  out.push_back('\n');
  out.append(static_cast<std::size_t>(depth) * indent, ' ');
}

void write_pretty(std::string& out, const JsonValue& value, unsigned depth, unsigned indent) {
  switch (value.kind()) {
    case JsonValue::Kind::Array: {
      if (value.items().empty()) {
        out.append("[]");
        return;
      }
      out.push_back('[');
      bool first = true;
      for (const JsonValue& item : value.items()) {
        if (!first) {
          out.push_back(',');
        }
        first = false;
        write_indent(out, depth + 1, indent);
        write_pretty(out, item, depth + 1, indent);
      }
      write_indent(out, depth, indent);
      out.push_back(']');
      return;
    }
    case JsonValue::Kind::Object: {
      if (value.members().empty()) {
        out.append("{}");
        return;
      }
      out.push_back('{');
      bool first = true;
      for (const auto& [name, member] : value.members()) {
        if (!first) {
          out.push_back(',');
        }
        first = false;
        write_indent(out, depth + 1, indent);
        write_string(out, name);
        out.append(": ");
        write_pretty(out, member, depth + 1, indent);
      }
      write_indent(out, depth, indent);
      out.push_back('}');
      return;
    }
    default:
      write_compact(out, value);
      return;
  }
}

Result<std::int64_t> require_integer(const JsonValue& member, std::string_view key) {
  if (!member.is_int()) {
    return Status::error(StatusCode::InvalidArgument,
                         "member '" + std::string(key) + "' must be an integer");
  }
  return member.as_int();
}

}  // namespace

JsonValue JsonValue::boolean(bool value) {
  JsonValue result;
  result.kind_ = Kind::Bool;
  result.bool_ = value;
  return result;
}

JsonValue JsonValue::integer(std::int64_t value) {
  JsonValue result;
  result.kind_ = Kind::Int;
  result.int_ = value;
  return result;
}

JsonValue JsonValue::text(std::string value) {
  JsonValue result;
  result.kind_ = Kind::String;
  result.string_ = std::move(value);
  return result;
}

JsonValue JsonValue::array() {
  JsonValue result;
  result.kind_ = Kind::Array;
  return result;
}

JsonValue JsonValue::object() {
  JsonValue result;
  result.kind_ = Kind::Object;
  return result;
}

bool JsonValue::as_bool(bool fallback) const noexcept { return is_bool() ? bool_ : fallback; }

std::int64_t JsonValue::as_int(std::int64_t fallback) const noexcept {
  return is_int() ? int_ : fallback;
}

const std::string& JsonValue::as_string() const noexcept {
  static const std::string kEmpty;
  return is_string() ? string_ : kEmpty;
}

const std::vector<JsonValue>& JsonValue::items() const noexcept {
  static const std::vector<JsonValue> kEmpty;
  return is_array() ? items_ : kEmpty;
}

const std::map<std::string, JsonValue, std::less<>>& JsonValue::members() const noexcept {
  static const std::map<std::string, JsonValue, std::less<>> kEmpty;
  return is_object() ? members_ : kEmpty;
}

const JsonValue* JsonValue::find(std::string_view key) const noexcept {
  if (!is_object()) {
    return nullptr;
  }
  const auto found = members_.find(key);
  return found == members_.end() ? nullptr : &found->second;
}

std::size_t JsonValue::size() const noexcept {
  if (is_array()) {
    return items_.size();
  }
  if (is_object()) {
    return members_.size();
  }
  return 0;
}

void JsonValue::push(JsonValue value) {
  if (is_array()) {
    items_.push_back(std::move(value));
  }
}

void JsonValue::set(std::string key, JsonValue value) {
  if (is_object()) {
    members_.insert_or_assign(std::move(key), std::move(value));
  }
}

Result<JsonValue> parse_json(std::string_view text, const JsonLimits& limits) {
  Parser parser(text, limits);
  return parser.parse_document();
}

std::string to_canonical_json(const JsonValue& value) {
  std::string out;
  write_compact(out, value);
  return out;
}

std::string to_pretty_json(const JsonValue& value, unsigned indent) {
  std::string out;
  write_pretty(out, value, 0, indent == 0 ? 2 : indent);
  out.push_back('\n');
  return out;
}

std::string json_escape(std::string_view text) {
  std::string out;
  write_string(out, text);
  return out;
}

const JsonValue* json_member(const JsonValue& object, std::string_view key) {
  return object.find(key);
}

Result<JsonValue> json_require_member(const JsonValue& object, std::string_view key) {
  if (!object.is_object()) {
    return Status::error(StatusCode::InvalidArgument, "expected a JSON object");
  }
  const JsonValue* member = object.find(key);
  if (member == nullptr) {
    return Status::error(StatusCode::InvalidArgument, "missing required member '" + std::string(key) + "'");
  }
  return *member;
}

Result<std::int64_t> json_require_int(const JsonValue& object, std::string_view key) {
  auto member = json_require_member(object, key);
  if (!member.ok()) {
    return member.status();
  }
  return require_integer(member.value(), key);
}

Result<std::uint64_t> json_require_uint(const JsonValue& object, std::string_view key) {
  auto member = json_require_member(object, key);
  if (!member.ok()) {
    return member.status();
  }
  if (!member.value().is_int()) {
    return Status::error(StatusCode::InvalidArgument,
                         "member '" + std::string(key) + "' must be an integer");
  }
  const std::int64_t value = member.value().as_int();
  if (value < 0) {
    return Status::error(StatusCode::OutOfRange,
                         "member '" + std::string(key) + "' must not be negative");
  }
  return static_cast<std::uint64_t>(value);
}

Result<std::string> json_require_string(const JsonValue& object, std::string_view key) {
  auto member = json_require_member(object, key);
  if (!member.ok()) {
    return member.status();
  }
  if (!member.value().is_string()) {
    return Status::error(StatusCode::InvalidArgument,
                         "member '" + std::string(key) + "' must be a string");
  }
  return member.value().as_string();
}

Result<bool> json_require_bool(const JsonValue& object, std::string_view key) {
  auto member = json_require_member(object, key);
  if (!member.ok()) {
    return member.status();
  }
  if (!member.value().is_bool()) {
    return Status::error(StatusCode::InvalidArgument,
                         "member '" + std::string(key) + "' must be a boolean");
  }
  return member.value().as_bool();
}

Result<std::int64_t> json_optional_int(const JsonValue& object, std::string_view key,
                                       std::int64_t fallback) {
  const JsonValue* member = object.find(key);
  if (member == nullptr) {
    return fallback;
  }
  return require_integer(*member, key);
}

Result<std::string> json_optional_string(const JsonValue& object, std::string_view key,
                                         std::string fallback) {
  const JsonValue* member = object.find(key);
  if (member == nullptr) {
    return fallback;
  }
  if (!member->is_string()) {
    return Status::error(StatusCode::InvalidArgument,
                         "member '" + std::string(key) + "' must be a string");
  }
  return member->as_string();
}

}  // namespace load_shedding
