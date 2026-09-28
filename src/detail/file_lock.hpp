#pragma once

// Cross-process store lock. This is an operating-system guarantee, not a
// convention: the kernel releases the lock when the holding process exits,
// including abrupt termination, so process death relinquishes writer authority
// with no recovery step and no stale lock file to clean up.
//
// Exclusive mode is writer authority. Shared mode is a reader that must not
// observe a half-published state.
//
// Internal header: not installed.

#include <filesystem>
#include <string>

#include "load_shedding/status.hpp"

namespace load_shedding::detail {

class FileLock {
 public:
  enum class Mode { Shared, Exclusive };

  FileLock() = default;
  ~FileLock();
  FileLock(const FileLock&) = delete;
  FileLock& operator=(const FileLock&) = delete;
  FileLock(FileLock&& other) noexcept;
  FileLock& operator=(FileLock&& other) noexcept;

  /// Acquires the lock immediately or fails with `LockConflict` when another
  /// process or handle holds a conflicting lock. Never blocks.
  static Result<FileLock> acquire(const std::filesystem::path& path, Mode mode);

  Status release();

  bool held() const noexcept;
  Mode mode() const noexcept { return mode_; }
  const std::filesystem::path& path() const noexcept { return path_; }

 private:
#if defined(_WIN32)
  void* handle_ = nullptr;
#else
  int descriptor_ = -1;
#endif
  Mode mode_ = Mode::Shared;
  std::filesystem::path path_;
};

std::string_view to_string(FileLock::Mode mode) noexcept;

}  // namespace load_shedding::detail
