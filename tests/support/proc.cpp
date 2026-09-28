#include "proc.hpp"

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "fixture.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace ls_test {
namespace {

/// Each run gets its own output files: two children of the same test must not
/// fight over one pair of handles.
std::uint64_t next_run_serial() {
  static std::uint64_t serial = 0;
  return ++serial;
}

std::string quote_argument(const std::string& argument) {
  std::string quoted = "\"";
  for (const char character : argument) {
    if (character == '"') {
      quoted += "\\\"";
    } else {
      quoted.push_back(character);
    }
  }
  quoted.push_back('"');
  return quoted;
}

}  // namespace

bool was_terminated(const ProcessResult& result) {
#if defined(_WIN32)
  return result.exit_code == 0xC1 || result.exit_code == 3;
#else
  return result.exit_code != 0 && result.exit_code != 1;
#endif
}

load_shedding::Result<ProcessResult> run_process(const std::string& executable,
                                                 const std::vector<std::string>& arguments,
                                                 const std::string& output_directory) {
  const std::string serial = std::to_string(next_run_serial());
  const std::string out_path =
      (std::filesystem::path(output_directory) / ("child-" + serial + ".stdout")).string();
  const std::string err_path =
      (std::filesystem::path(output_directory) / ("child-" + serial + ".stderr")).string();
  std::string command = quote_argument(executable);
  for (const std::string& argument : arguments) {
    command.push_back(' ');
    command += quote_argument(argument);
  }
  int process_exit = 0;
#if defined(_WIN32)
  // The child is started directly with file handles for its output rather than
  // through a shell: no quoting layer, no pipe buffer to deadlock on, and the
  // exit code is the child's own.
  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;
  const HANDLE output_file =
      ::CreateFileA(out_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &attributes, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, nullptr);
  const HANDLE error_file =
      ::CreateFileA(err_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &attributes, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, nullptr);
  if (output_file == INVALID_HANDLE_VALUE || error_file == INVALID_HANDLE_VALUE) {
    return load_shedding::Status::error(load_shedding::StatusCode::IoFailure,
                                        "could not open the child output files");
  }
  std::vector<char> buffer(command.begin(), command.end());
  buffer.push_back('\0');
  STARTUPINFOA startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
  startup.hStdOutput = output_file;
  startup.hStdError = error_file;
  PROCESS_INFORMATION info{};
  const BOOL started = ::CreateProcessA(nullptr, buffer.data(), nullptr, nullptr, TRUE, 0, nullptr,
                                        nullptr, &startup, &info);
  ::CloseHandle(output_file);
  ::CloseHandle(error_file);
  if (started == 0) {
    return load_shedding::Status::error(load_shedding::StatusCode::IoFailure,
                                        "could not start '" + executable + "'");
  }
  ::WaitForSingleObject(info.hProcess, INFINITE);
  DWORD exit_code = 0;
  ::GetExitCodeProcess(info.hProcess, &exit_code);
  ::CloseHandle(info.hThread);
  ::CloseHandle(info.hProcess);
  process_exit = static_cast<int>(exit_code);
#else
  const std::string redirection =
      command + " > " + quote_argument(out_path) + " 2> " + quote_argument(err_path);
  const int status = std::system(redirection.c_str());
  process_exit = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
#endif
  ProcessResult result;
  result.exit_code = process_exit;
  auto out = read_bytes(out_path);
  if (out.ok()) {
    result.standard_output = out.value();
  }
  auto err = read_bytes(err_path);
  if (err.ok()) {
    result.standard_error = err.value();
  }
  return result;
}

ChildProcess::~ChildProcess() {
  if (running()) {
    terminate();
    wait();
  }
}

ChildProcess::ChildProcess(ChildProcess&& other) noexcept {
#if defined(_WIN32)
  process_ = other.process_;
  thread_ = other.thread_;
  input_write_ = other.input_write_;
  output_read_ = other.output_read_;
  other.process_ = nullptr;
  other.thread_ = nullptr;
  other.input_write_ = nullptr;
  other.output_read_ = nullptr;
#else
  input_write_ = other.input_write_;
  output_read_ = other.output_read_;
  child_pid_ = other.child_pid_;
  other.input_write_ = -1;
  other.output_read_ = -1;
  other.child_pid_ = -1;
#endif
  output_path_ = std::move(other.output_path_);
}

ChildProcess& ChildProcess::operator=(ChildProcess&& other) noexcept {
  if (this != &other) {
    if (running()) {
      terminate();
      wait();
    }
#if defined(_WIN32)
    process_ = other.process_;
    thread_ = other.thread_;
    input_write_ = other.input_write_;
    output_read_ = other.output_read_;
    other.process_ = nullptr;
    other.thread_ = nullptr;
    other.input_write_ = nullptr;
    other.output_read_ = nullptr;
#else
    input_write_ = other.input_write_;
    output_read_ = other.output_read_;
    child_pid_ = other.child_pid_;
    other.input_write_ = -1;
    other.output_read_ = -1;
    other.child_pid_ = -1;
#endif
    output_path_ = std::move(other.output_path_);
  }
  return *this;
}

load_shedding::Result<ChildProcess> ChildProcess::spawn(const std::string& executable,
                                                        const std::vector<std::string>& arguments,
                                                        const std::string& output_directory) {
  ChildProcess child;
  child.output_path_ = (std::filesystem::path(output_directory) / "child.stderr").string();
#if defined(_WIN32)
  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;
  HANDLE input_read = nullptr;
  HANDLE input_write = nullptr;
  HANDLE output_read = nullptr;
  HANDLE output_write = nullptr;
  if (::CreatePipe(&input_read, &input_write, &attributes, 0) == 0 ||
      ::CreatePipe(&output_read, &output_write, &attributes, 0) == 0) {
    return load_shedding::Status::error(load_shedding::StatusCode::IoFailure,
                                        "could not create pipes");
  }
  ::SetHandleInformation(input_write, HANDLE_FLAG_INHERIT, 0);
  ::SetHandleInformation(output_read, HANDLE_FLAG_INHERIT, 0);
  const HANDLE error_file =
      ::CreateFileA(child.output_path_.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &attributes,
                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

  std::string command = quote_argument(executable);
  for (const std::string& argument : arguments) {
    command.push_back(' ');
    command += quote_argument(argument);
  }
  std::vector<char> buffer(command.begin(), command.end());
  buffer.push_back('\0');
  STARTUPINFOA startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = input_read;
  startup.hStdOutput = output_write;
  startup.hStdError = error_file == INVALID_HANDLE_VALUE ? output_write : error_file;
  PROCESS_INFORMATION info{};
  const BOOL started = ::CreateProcessA(nullptr, buffer.data(), nullptr, nullptr, TRUE, 0, nullptr,
                                        nullptr, &startup, &info);
  ::CloseHandle(input_read);
  ::CloseHandle(output_write);
  if (error_file != INVALID_HANDLE_VALUE) {
    ::CloseHandle(error_file);
  }
  if (started == 0) {
    ::CloseHandle(input_write);
    ::CloseHandle(output_read);
    return load_shedding::Status::error(load_shedding::StatusCode::IoFailure,
                                        "could not start '" + executable + "'");
  }
  child.process_ = info.hProcess;
  child.thread_ = info.hThread;
  child.input_write_ = input_write;
  child.output_read_ = output_read;
#else
  (void)executable;
  (void)arguments;
#endif
  return child;
}

load_shedding::Status ChildProcess::write_line(const std::string& line) {
#if defined(_WIN32)
  if (input_write_ == nullptr) {
    return load_shedding::Status::error(load_shedding::StatusCode::Closed, "input is closed");
  }
  const std::string text = line + "\n";
  DWORD written = 0;
  if (::WriteFile(static_cast<HANDLE>(input_write_), text.data(),
                  static_cast<DWORD>(text.size()), &written, nullptr) == 0) {
    return load_shedding::Status::error(load_shedding::StatusCode::IoFailure,
                                        "could not write to the child");
  }
  return load_shedding::Status::success();
#else
  (void)line;
  return load_shedding::Status::error(load_shedding::StatusCode::Unsupported,
                                      "interactive children are not implemented on this platform");
#endif
}

load_shedding::Status ChildProcess::close_input() {
#if defined(_WIN32)
  if (input_write_ != nullptr) {
    ::CloseHandle(static_cast<HANDLE>(input_write_));
    input_write_ = nullptr;
  }
  return load_shedding::Status::success();
#else
  return load_shedding::Status::success();
#endif
}

load_shedding::Result<std::string> ChildProcess::read_line() {
#if defined(_WIN32)
  if (output_read_ == nullptr) {
    return load_shedding::Status::error(load_shedding::StatusCode::Closed, "output is closed");
  }
  std::string line;
  char character = 0;
  while (true) {
    DWORD read = 0;
    if (::ReadFile(static_cast<HANDLE>(output_read_), &character, 1, &read, nullptr) == 0) {
      return load_shedding::Status::error(load_shedding::StatusCode::IoFailure,
                                          "could not read from the child");
    }
    if (read == 0) {
      if (line.empty()) {
        return load_shedding::Status::error(load_shedding::StatusCode::NotFound,
                                            "the child closed its output");
      }
      break;
    }
    if (character == '\n') {
      break;
    }
    if (character != '\r') {
      line.push_back(character);
    }
    if (line.size() > 65536) {
      return load_shedding::Status::error(load_shedding::StatusCode::Overlong,
                                          "the child produced an oversized line");
    }
  }
  return line;
#else
  return load_shedding::Status::error(load_shedding::StatusCode::Unsupported,
                                      "interactive children are not implemented on this platform");
#endif
}

load_shedding::Result<int> ChildProcess::wait() {
#if defined(_WIN32)
  if (process_ == nullptr) {
    return 0;
  }
  ::WaitForSingleObject(static_cast<HANDLE>(process_), INFINITE);
  DWORD exit_code = 0;
  ::GetExitCodeProcess(static_cast<HANDLE>(process_), &exit_code);
  ::CloseHandle(static_cast<HANDLE>(process_));
  if (thread_ != nullptr) {
    ::CloseHandle(static_cast<HANDLE>(thread_));
    thread_ = nullptr;
  }
  process_ = nullptr;
  return static_cast<int>(exit_code);
#else
  return 0;
#endif
}

load_shedding::Status ChildProcess::terminate() {
#if defined(_WIN32)
  if (process_ == nullptr) {
    return load_shedding::Status::success();
  }
  if (::TerminateProcess(static_cast<HANDLE>(process_), 0xC1) == 0) {
    return load_shedding::Status::error(load_shedding::StatusCode::IoFailure,
                                        "could not terminate the child");
  }
  return load_shedding::Status::success();
#else
  return load_shedding::Status::success();
#endif
}

bool ChildProcess::running() const {
#if defined(_WIN32)
  if (process_ == nullptr) {
    return false;
  }
  return ::WaitForSingleObject(static_cast<HANDLE>(process_), 0) == WAIT_TIMEOUT;
#else
  return child_pid_ > 0;
#endif
}

}  // namespace ls_test
