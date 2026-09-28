#include "detail/platform_io.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <system_error>

#include "load_shedding/text.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace load_shedding::detail {
namespace {

Status system_error_status(StatusCode code, const std::string& action, const std::string& path,
                           int error) {
  return Status::error(code, action + " '" + path + "': " + std::generic_category().message(error));
}

}  // namespace

std::string path_utf8(const std::filesystem::path& path) {
#if defined(_WIN32)
  const std::u8string utf8 = path.u8string();
  return std::string(utf8.begin(), utf8.end());
#else
  return path.string();
#endif
}

std::filesystem::path path_from_utf8(std::string_view text) {
#if defined(_WIN32)
  const auto* begin = reinterpret_cast<const char8_t*>(text.data());
  return std::filesystem::path(std::u8string(begin, begin + text.size()));
#else
  return std::filesystem::path(std::string(text));
#endif
}

Result<std::string> read_file_bounded(const std::filesystem::path& path, std::size_t max_bytes) {
  const std::string display = path_utf8(path);
#if defined(_WIN32)
  const HANDLE handle = ::CreateFileW(path.wstring().c_str(), GENERIC_READ,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    const DWORD error = ::GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
      return Status::error(StatusCode::NotFound, "file '" + display + "' does not exist");
    }
    return Status::error(StatusCode::IoFailure,
                         "could not open '" + display + "' (windows error " + std::to_string(error) + ")");
  }
  LARGE_INTEGER size{};
  if (::GetFileSizeEx(handle, &size) == 0) {
    ::CloseHandle(handle);
    return Status::error(StatusCode::IoFailure, "could not size '" + display + "'");
  }
  if (size.QuadPart < 0 || static_cast<std::uint64_t>(size.QuadPart) > max_bytes) {
    ::CloseHandle(handle);
    return Status::error(StatusCode::Overlong,
                         "file '" + display + "' is " + std::to_string(size.QuadPart) +
                             " bytes, above the bound of " + std::to_string(max_bytes));
  }
  std::string content(static_cast<std::size_t>(size.QuadPart), '\0');
  std::size_t offset = 0;
  while (offset < content.size()) {
    const DWORD chunk = static_cast<DWORD>(
        std::min<std::size_t>(content.size() - offset, 1u << 20));
    DWORD read = 0;
    if (::ReadFile(handle, content.data() + offset, chunk, &read, nullptr) == 0) {
      ::CloseHandle(handle);
      return Status::error(StatusCode::IoFailure, "could not read '" + display + "'");
    }
    if (read == 0) {
      break;
    }
    offset += read;
  }
  ::CloseHandle(handle);
  if (offset != content.size()) {
    return Status::error(StatusCode::Truncated,
                         "file '" + display + "' ended after " + std::to_string(offset) +
                             " of " + std::to_string(content.size()) + " bytes");
  }
  return content;
#else
  const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (descriptor < 0) {
    if (errno == ENOENT) {
      return Status::error(StatusCode::NotFound, "file '" + display + "' does not exist");
    }
    return system_error_status(StatusCode::IoFailure, "could not open", display, errno);
  }
  struct stat info {};
  if (::fstat(descriptor, &info) != 0) {
    const int error = errno;
    ::close(descriptor);
    return system_error_status(StatusCode::IoFailure, "could not stat", display, error);
  }
  if (static_cast<std::uint64_t>(info.st_size) > max_bytes) {
    ::close(descriptor);
    return Status::error(StatusCode::Overlong,
                         "file '" + display + "' is " + std::to_string(info.st_size) +
                             " bytes, above the bound of " + std::to_string(max_bytes));
  }
  std::string content;
  content.resize(static_cast<std::size_t>(info.st_size));
  std::size_t offset = 0;
  while (offset < content.size()) {
    const ssize_t read = ::read(descriptor, content.data() + offset, content.size() - offset);
    if (read < 0) {
      if (errno == EINTR) {
        continue;
      }
      const int error = errno;
      ::close(descriptor);
      return system_error_status(StatusCode::IoFailure, "could not read", display, error);
    }
    if (read == 0) {
      break;
    }
    offset += static_cast<std::size_t>(read);
  }
  ::close(descriptor);
  if (offset != content.size()) {
    return Status::error(StatusCode::Truncated, "file '" + display + "' ended early");
  }
  return content;
#endif
}

Status write_file_durable(const std::filesystem::path& path, std::string_view content) {
  const std::string display = path_utf8(path);
#if defined(_WIN32)
  const HANDLE handle = ::CreateFileW(path.wstring().c_str(), GENERIC_WRITE, 0, nullptr,
                                      CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return Status::error(StatusCode::IoFailure,
                         "could not create '" + display + "' (windows error " +
                             std::to_string(::GetLastError()) + ")");
  }
  std::size_t offset = 0;
  while (offset < content.size()) {
    const DWORD chunk =
        static_cast<DWORD>(std::min<std::size_t>(content.size() - offset, 1u << 20));
    DWORD written = 0;
    if (::WriteFile(handle, content.data() + offset, chunk, &written, nullptr) == 0) {
      ::CloseHandle(handle);
      return Status::error(StatusCode::IoFailure, "could not write '" + display + "'");
    }
    offset += written;
  }
  if (::FlushFileBuffers(handle) == 0) {
    ::CloseHandle(handle);
    return Status::error(StatusCode::IoFailure, "could not flush '" + display + "'");
  }
  if (::CloseHandle(handle) == 0) {
    return Status::error(StatusCode::IoFailure, "could not close '" + display + "'");
  }
  return Status::success();
#else
  const int descriptor =
      ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (descriptor < 0) {
    return system_error_status(StatusCode::IoFailure, "could not create", display, errno);
  }
  std::size_t offset = 0;
  while (offset < content.size()) {
    const ssize_t written = ::write(descriptor, content.data() + offset, content.size() - offset);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      const int error = errno;
      ::close(descriptor);
      return system_error_status(StatusCode::IoFailure, "could not write", display, error);
    }
    offset += static_cast<std::size_t>(written);
  }
  if (::fsync(descriptor) != 0) {
    const int error = errno;
    ::close(descriptor);
    return system_error_status(StatusCode::IoFailure, "could not flush", display, error);
  }
  if (::close(descriptor) != 0) {
    return system_error_status(StatusCode::IoFailure, "could not close", display, errno);
  }
  return Status::success();
#endif
}

Status replace_file_atomic(const std::filesystem::path& staging,
                           const std::filesystem::path& target) {
  const std::string display = path_utf8(target);
#if defined(_WIN32)
  if (::MoveFileExW(staging.wstring().c_str(), target.wstring().c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
    return Status::error(StatusCode::IoFailure,
                         "could not publish '" + display + "' (windows error " +
                             std::to_string(::GetLastError()) + ")");
  }
  return Status::success();
#else
  if (::rename(staging.c_str(), target.c_str()) != 0) {
    return system_error_status(StatusCode::IoFailure, "could not publish", display, errno);
  }
  return Status::success();
#endif
}

Status remove_file(const std::filesystem::path& path) {
  const std::string display = path_utf8(path);
#if defined(_WIN32)
  if (::DeleteFileW(path.wstring().c_str()) == 0) {
    const DWORD error = ::GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
      return Status::success();
    }
    return Status::error(StatusCode::IoFailure,
                         "could not remove '" + display + "' (windows error " +
                             std::to_string(error) + ")");
  }
  return Status::success();
#else
  if (::unlink(path.c_str()) != 0 && errno != ENOENT) {
    return system_error_status(StatusCode::IoFailure, "could not remove", display, errno);
  }
  return Status::success();
#endif
}

bool file_exists(const std::filesystem::path& path) {
  std::error_code error;
  return std::filesystem::exists(path, error) && !error;
}

Result<std::uint64_t> file_size(const std::filesystem::path& path) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error) {
    return Status::error(StatusCode::IoFailure,
                         "could not size '" + path_utf8(path) + "': " + error.message());
  }
  return static_cast<std::uint64_t>(size);
}

Result<std::vector<DirectoryEntry>> list_directory(const std::filesystem::path& path) {
  std::vector<DirectoryEntry> entries;
  std::error_code error;
  std::filesystem::directory_iterator iterator(path, error);
  if (error) {
    return Status::error(StatusCode::IoFailure,
                         "could not list '" + path_utf8(path) + "': " + error.message());
  }
  for (const std::filesystem::directory_entry& entry : iterator) {
    DirectoryEntry record;
    record.name = path_utf8(entry.path().filename());
    std::error_code status_error;
    const std::filesystem::file_status status = entry.symlink_status(status_error);
    if (status_error) {
      return Status::error(StatusCode::IoFailure,
                           "could not stat '" + path_utf8(entry.path()) + "': " + status_error.message());
    }
    record.symlink = std::filesystem::is_symlink(status);
    record.regular_file = std::filesystem::is_regular_file(status);
    record.directory = std::filesystem::is_directory(status);
    if (record.regular_file) {
      std::error_code size_error;
      const auto size = entry.file_size(size_error);
      record.bytes = size_error ? 0 : static_cast<std::uint64_t>(size);
    }
    entries.push_back(std::move(record));
  }
  std::sort(entries.begin(), entries.end(),
            [](const DirectoryEntry& left, const DirectoryEntry& right) {
              return left.name < right.name;
            });
  return entries;
}

Status ensure_directory(const std::filesystem::path& path) {
  std::error_code error;
  if (std::filesystem::exists(path, error)) {
    if (!std::filesystem::is_directory(path, error)) {
      return Status::error(StatusCode::InvalidArgument,
                           "store path '" + path_utf8(path) + "' exists and is not a directory");
    }
    return Status::success();
  }
  std::filesystem::create_directories(path, error);
  if (error) {
    return Status::error(StatusCode::IoFailure,
                         "could not create '" + path_utf8(path) + "': " + error.message());
  }
  return Status::success();
}

Status sync_directory(const std::filesystem::path& path) {
#if defined(_WIN32)
  (void)path;
  return Status::success();
#else
  const int descriptor = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (descriptor < 0) {
    return system_error_status(StatusCode::IoFailure, "could not open directory", path_utf8(path), errno);
  }
  const int result = ::fsync(descriptor);
  const int error = errno;
  ::close(descriptor);
  if (result != 0) {
    return system_error_status(StatusCode::IoFailure, "could not flush directory", path_utf8(path), error);
  }
  return Status::success();
#endif
}

std::uint64_t current_process_id() {
#if defined(_WIN32)
  return static_cast<std::uint64_t>(::GetCurrentProcessId());
#else
  return static_cast<std::uint64_t>(::getpid());
#endif
}

void terminate_process_now(int exit_code) {
#if defined(_WIN32)
  ::TerminateProcess(::GetCurrentProcess(), static_cast<UINT>(exit_code));
  ::_exit(exit_code);
#else
  ::_exit(exit_code);
#endif
}

bool is_link_or_reparse_point(const std::filesystem::path& path) {
  std::error_code error;
  const std::filesystem::file_status status = std::filesystem::symlink_status(path, error);
  if (error) {
    return false;
  }
  if (std::filesystem::is_symlink(status)) {
    return true;
  }
#if defined(_WIN32)
  const DWORD attributes = ::GetFileAttributesW(path.wstring().c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    return false;
  }
  return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
  return false;
#endif
}

Result<bool> is_plain_directory(const std::filesystem::path& path) {
  std::error_code error;
  const std::filesystem::file_status status = std::filesystem::symlink_status(path, error);
  if (error) {
    if (error.value() == static_cast<int>(std::errc::no_such_file_or_directory)) {
      return false;
    }
    return Status::error(StatusCode::IoFailure,
                         "could not inspect '" + path_utf8(path) + "': " + error.message());
  }
  if (std::filesystem::is_symlink(status)) {
    return false;
  }
  return std::filesystem::is_directory(status);
}

Status validate_store_path(const std::string& directory) {
  if (directory.empty()) {
    return Status::error(StatusCode::InvalidArgument, "store directory must not be empty");
  }
  if (directory.size() > 4096) {
    return Status::error(StatusCode::Overlong, "store directory path is longer than 4096 bytes");
  }
  if (directory.find('\0') != std::string::npos) {
    return Status::error(StatusCode::Rejected, "store directory path contains a NUL byte");
  }
  if (!is_valid_utf8(directory)) {
    return Status::error(StatusCode::InvalidArgument, "store directory path is not valid UTF-8");
  }
  const std::filesystem::path path = path_from_utf8(directory);

  // Every existing ancestor is checked for link substitution before anything is
  // normalized, so a swapped-in path cannot be canonicalized into acceptance.
  std::filesystem::path prefix;
  for (const std::filesystem::path& component : path) {
    prefix /= component;
    if (!file_exists(prefix)) {
      continue;
    }
    if (is_link_or_reparse_point(prefix)) {
      return Status::error(StatusCode::Rejected,
                           "store path component '" + path_utf8(prefix) +
                               "' is a symbolic link, junction, or reparse point");
    }
  }
  return Status::success();
}

}  // namespace load_shedding::detail
