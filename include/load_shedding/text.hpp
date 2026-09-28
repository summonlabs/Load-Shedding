#pragma once

// Text validation used by every externally influenced string in the library.
// Validation happens before any normalization or truncation so that a rejected
// input is never silently rewritten into an accepted one.

#include <cstddef>
#include <string>
#include <string_view>

#include "load_shedding/limits.hpp"
#include "load_shedding/status.hpp"

namespace load_shedding {

/// Strict UTF-8 validation: rejects overlong encodings, surrogate code points,
/// code points above U+10FFFF, and truncated sequences.
bool is_valid_utf8(std::string_view text) noexcept;

/// Number of Unicode scalar values, or a failure when the text is not UTF-8.
Result<std::size_t> utf8_length(std::string_view text);

/// True when the text contains no C0/C1 control character.
bool has_no_control_characters(std::string_view text) noexcept;

/// True when every byte is printable ASCII (0x21..0x7E).
bool is_printable_ascii(std::string_view text) noexcept;

/// Validates a bounded opaque identifier: non-empty, at most `max_bytes`, valid
/// UTF-8, no control characters, no leading or trailing ASCII space, and no path
/// separator. Identifiers are opaque: the library never interprets them, and the
/// rules exist so that an identifier can be printed and compared unambiguously.
Status validate_identifier(std::string_view text, std::size_t max_bytes, const char* what);

/// Validates a bounded free-text field that may be empty but must be valid UTF-8
/// with no control characters other than none at all.
Status validate_text(std::string_view text, std::size_t max_bytes, const char* what);

/// Rejects `"."`, `".."`, empty input, absolute paths, drive-qualified paths,
/// backslashes, and any path that would escape the directory it is resolved
/// against. Returns the validated relative path for the caller to join.
Status validate_relative_component(std::string_view text, const char* what);

/// Joins `base` and a validated single path component, refusing traversal.
Result<std::string> join_relative_path(std::string_view base, std::string_view component);

}  // namespace load_shedding
