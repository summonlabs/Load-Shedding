#include "detail/file_lock.hpp"

#include <cerrno>
#include <string>
#include <system_error>
#include <utility>

#include "detail/platform_io.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace load_shedding::detail {
namespace {

/// Best-effort record of the holder, written beside the lock file rather than
/// into it so that annotation can never interact with the lock itself. A failure
/// to annotate never changes whether the lock is held.
void annotate(const std::filesystem::path& path, FileLock::Mode mode) {
  const std::string text = "pid=" + std::to_string(current_process_id()) +
                           " mode=" + std::string(to_string(mode)) + "\n";
  const std::filesystem::path owner = path.parent_path() / (path.filename().string() + ".owner");
  const Status written = write_file_durable(owner, text);
  (void)written;
}

}  // namespace

std::string_view to_string(FileLock::Mode mode) noexcept {
  return mode == FileLock::Mode::Exclusive ? "exclusive" : "shared";
}

FileLock::~FileLock() {
  if (held()) {
    release();
  }
}

FileLock::FileLock(FileLock&& other) noexcept {
#if defined(_WIN32)
  handle_ = other.handle_;
  other.handle_ = nullptr;
#else
  descriptor_ = other.descriptor_;
  other.descriptor_ = -1;
#endif
  mode_ = other.mode_;
  path_ = std::move(other.path_);
}

FileLock& FileLock::operator=(FileLock&& other) noexcept {
  if (this != &other) {
    if (held()) {
      release();
    }
#if defined(_WIN32)
    handle_ = other.handle_;
    other.handle_ = nullptr;
#else
    descriptor_ = other.descriptor_;
    other.descriptor_ = -1;
#endif
    mode_ = other.mode_;
    path_ = std::move(other.path_);
  }
  return *this;
}

bool FileLock::held() const noexcept {
#if defined(_WIN32)
  return handle_ != nullptr;
#else
  return descriptor_ >= 0;
#endif
}

Result<FileLock> FileLock::acquire(const std::filesystem::path& path, Mode mode) {
  if (path.empty()) {
    return Status::error(StatusCode::InvalidArgument, "lock path must not be empty");
  }
  FileLock lock;
  lock.mode_ = mode;
  lock.path_ = path;
  const std::string display = path_utf8(path);

#if defined(_WIN32)
  const HANDLE handle =
      ::CreateFileW(path.wstring().c_str(), GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    const DWORD error = ::GetLastError();
    return Status::error(error == ERROR_ACCESS_DENIED ? StatusCode::PermissionDenied
                                                      : StatusCode::IoFailure,
                         "could not open the lock file '" + display + "' (windows error " +
                             std::to_string(error) + ")");
  }
  OVERLAPPED overlapped{};
  DWORD flags = LOCKFILE_FAIL_IMMEDIATELY;
  if (mode == Mode::Exclusive) {
    flags |= LOCKFILE_EXCLUSIVE_LOCK;
  }
  if (::LockFileEx(handle, flags, 0, 1, 0, &overlapped) == 0) {
    const DWORD error = ::GetLastError();
    ::CloseHandle(handle);
    if (error == ERROR_LOCK_VIOLATION || error == ERROR_IO_PENDING) {
      return Status::error(StatusCode::LockConflict,
                           std::string(mode == Mode::Exclusive ? "an exclusive" : "a shared") +
                               " lock on '" + display + "' is held by another process");
    }
    return Status::error(StatusCode::IoFailure,
                         "could not lock '" + display + "' (windows error " + std::to_string(error) + ")");
  }
  lock.handle_ = handle;
#else
  const int descriptor = ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0666);
  if (descriptor < 0) {
    return Status::error(errno == EACCES ? StatusCode::PermissionDenied : StatusCode::IoFailure,
                         "could not open the lock file '" + display +
                             "': " + std::generic_category().message(errno));
  }
  const int operation = mode == Mode::Exclusive ? LOCK_EX | LOCK_NB : LOCK_SH | LOCK_NB;
  if (::flock(descriptor, operation) != 0) {
    const int error = errno;
    ::close(descriptor);
    if (error == EWOULDBLOCK || error == EAGAIN) {
      return Status::error(StatusCode::LockConflict,
                           "a conflicting lock on '" + display + "' is held by another process");
    }
    return Status::error(StatusCode::IoFailure,
                         "could not lock '" + display + "': " + std::generic_category().message(error));
  }
  lock.descriptor_ = descriptor;
#endif

  annotate(path, mode);
  return lock;
}

Status FileLock::release() {
  if (!held()) {
    return Status::success();
  }
  const std::string display = path_utf8(path_);
#if defined(_WIN32)
  const HANDLE handle = static_cast<HANDLE>(handle_);
  handle_ = nullptr;
  OVERLAPPED overlapped{};
  const BOOL unlocked = ::UnlockFileEx(handle, 0, 1, 0, &overlapped);
  const BOOL closed = ::CloseHandle(handle);
  if (unlocked == 0 || closed == 0) {
    return Status::error(StatusCode::IoFailure, "could not release the lock on '" + display + "'");
  }
  return Status::success();
#else
  const int descriptor = descriptor_;
  descriptor_ = -1;
  const int unlocked = ::flock(descriptor, LOCK_UN);
  const int closed = ::close(descriptor);
  if (unlocked != 0 || closed != 0) {
    return Status::error(StatusCode::IoFailure, "could not release the lock on '" + display + "'");
  }
  return Status::success();
#endif
}

}  // namespace load_shedding::detail
