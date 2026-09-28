#pragma once

// Narrow operating-system abstractions. Everything here is deliberately small:
// the library uses the C++ standard library for everything it can, and reaches
// for a platform primitive only where durability, atomicity, or process identity
// genuinely requires one.
//
// Internal header: not installed.

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "load_shedding/status.hpp"

namespace load_shedding::detail {

struct DirectoryEntry {
  std::string name;
  bool regular_file = false;
  bool directory = false;
  bool symlink = false;
  std::uint64_t bytes = 0;
};

/// UTF-8 rendering of a path, used in messages and in canonical metadata.
std::string path_utf8(const std::filesystem::path& path);

/// Builds a path from UTF-8 text without going through the deprecated
/// `std::filesystem::u8path` overloads.
std::filesystem::path path_from_utf8(std::string_view text);

/// Reads a whole file, refusing anything larger than `max_bytes` before it
/// allocates the buffer.
Result<std::string> read_file_bounded(const std::filesystem::path& path, std::size_t max_bytes);

/// Writes `content` to `path`, flushing the file's own buffers and the device
/// cache before returning. Truncates any existing content.
Status write_file_durable(const std::filesystem::path& path, std::string_view content);

/// Atomically replaces `target` with `staging`. After this returns, a reader
/// either sees the complete old content or the complete new content.
Status replace_file_atomic(const std::filesystem::path& staging, const std::filesystem::path& target);

Status remove_file(const std::filesystem::path& path);

bool file_exists(const std::filesystem::path& path);

Result<std::uint64_t> file_size(const std::filesystem::path& path);

/// Directory entries, sorted by name. Never recursive.
Result<std::vector<DirectoryEntry>> list_directory(const std::filesystem::path& path);

Status ensure_directory(const std::filesystem::path& path);

/// Flushes directory metadata where the platform requires it. A no-op on Windows,
/// where the atomic replacement API already provides the ordering guarantee.
Status sync_directory(const std::filesystem::path& path);

std::uint64_t current_process_id();

/// Immediately terminates the current process without unwinding, without running
/// atexit handlers, and without any interactive error-reporting path.
[[noreturn]] void terminate_process_now(int exit_code);

/// Validates a store directory path against the documented trust model: it must
/// be valid UTF-8, bounded, free of traversal, and neither the directory itself
/// nor any existing ancestor may be a symbolic link, junction, or other reparse
/// point. Checks run on the path as given, before any normalization, so that a
/// substituted path cannot be normalized into an accepted one.
Status validate_store_path(const std::string& directory);

/// True when the path exists and is a symbolic link, junction, or reparse point.
bool is_link_or_reparse_point(const std::filesystem::path& path);

/// True when the path exists and is a directory that is not a link.
Result<bool> is_plain_directory(const std::filesystem::path& path);

}  // namespace load_shedding::detail
