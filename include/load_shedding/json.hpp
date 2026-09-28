#pragma once

// A small, strict JSON reader/writer used for CLI input and output.
//
// Strictness is deliberate: duplicate member names, comments, trailing commas,
// control characters in strings, lone surrogates, invalid UTF-8, non-integer
// numbers, and documents beyond the configured bounds are refused with a named
// error rather than accepted leniently. Object members are held in a sorted map,
// so canonical output is byte-deterministic without any extra sorting step.

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "load_shedding/limits.hpp"
#include "load_shedding/status.hpp"

namespace load_shedding {

struct JsonLimits {
  std::size_t max_bytes = limits::kMaxJsonBytes;
  std::size_t max_depth = limits::kMaxJsonDepth;
  std::size_t max_string_bytes = limits::kMaxJsonStringBytes;
  std::size_t max_members = limits::kMaxJsonMembers;
};

class JsonValue {
 public:
  enum class Kind : std::uint8_t { Null = 0, Bool = 1, Int = 2, String = 3, Array = 4, Object = 5 };

  JsonValue() = default;

  static JsonValue null() { return JsonValue(); }
  static JsonValue boolean(bool value);
  static JsonValue integer(std::int64_t value);
  static JsonValue text(std::string value);
  static JsonValue array();
  static JsonValue object();

  Kind kind() const noexcept { return kind_; }
  bool is_null() const noexcept { return kind_ == Kind::Null; }
  bool is_bool() const noexcept { return kind_ == Kind::Bool; }
  bool is_int() const noexcept { return kind_ == Kind::Int; }
  bool is_string() const noexcept { return kind_ == Kind::String; }
  bool is_array() const noexcept { return kind_ == Kind::Array; }
  bool is_object() const noexcept { return kind_ == Kind::Object; }

  /// Boolean payload, or `fallback` when this value is not a boolean.
  bool as_bool(bool fallback = false) const noexcept;
  /// Integer payload, or `fallback` when this value is not an integer.
  std::int64_t as_int(std::int64_t fallback = 0) const noexcept;
  /// String payload, or an empty string when this value is not a string.
  const std::string& as_string() const noexcept;

  /// Array elements. Empty for a non-array value.
  const std::vector<JsonValue>& items() const noexcept;
  /// Object members, sorted by name. Empty for a non-object value.
  const std::map<std::string, JsonValue, std::less<>>& members() const noexcept;

  /// Member lookup, or null when absent or when this value is not an object.
  const JsonValue* find(std::string_view key) const noexcept;

  /// Number of array elements or object members.
  std::size_t size() const noexcept;

  /// Appends to an array value. Does nothing when this value is not an array.
  void push(JsonValue value);
  /// Inserts or replaces an object member. Does nothing when this value is not an
  /// object.
  void set(std::string key, JsonValue value);

 private:
  Kind kind_ = Kind::Null;
  bool bool_ = false;
  std::int64_t int_ = 0;
  std::string string_;
  std::vector<JsonValue> items_;
  std::map<std::string, JsonValue, std::less<>> members_;
};

/// Parses one complete JSON document. Trailing content is an error.
Result<JsonValue> parse_json(std::string_view text, const JsonLimits& limits = {});

/// Compact canonical form: sorted members, no insignificant whitespace, control
/// characters escaped as \uXXXX, everything else emitted as UTF-8.
std::string to_canonical_json(const JsonValue& value);

/// Indented form for human output. Member order is the same as canonical form.
std::string to_pretty_json(const JsonValue& value, unsigned indent = 2);

/// Escapes a string as a JSON string literal, including the surrounding quotes.
std::string json_escape(std::string_view text);

// ---- Typed accessors used by the command line layer ------------------------
const JsonValue* json_member(const JsonValue& object, std::string_view key);
Result<JsonValue> json_require_member(const JsonValue& object, std::string_view key);
Result<std::int64_t> json_require_int(const JsonValue& object, std::string_view key);
Result<std::uint64_t> json_require_uint(const JsonValue& object, std::string_view key);
Result<std::string> json_require_string(const JsonValue& object, std::string_view key);
Result<bool> json_require_bool(const JsonValue& object, std::string_view key);
Result<std::int64_t> json_optional_int(const JsonValue& object, std::string_view key, std::int64_t fallback);
Result<std::string> json_optional_string(const JsonValue& object, std::string_view key,
                                         std::string fallback);

}  // namespace load_shedding
