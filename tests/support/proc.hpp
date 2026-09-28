#pragma once

// Real child processes for multiprocess and crash tests. These are operating
// system processes, not threads: writer exclusion, process-death lock release,
// and authority fencing are only meaningful across a real process boundary.
//
// There are no timeouts here. A child that never answers is a defect in the test
// or in the library; it shows up as a test that does not finish.

#include <cstdint>
#include <string>
#include <vector>

#include "load_shedding/status.hpp"

namespace ls_test {

struct ProcessResult {
  int exit_code = 0;
  std::string standard_output;
  std::string standard_error;
};

/// Runs a process to completion with its output redirected to files, then reads
/// those files. Output redirection through files rather than pipes removes any
/// possibility of a pipe-buffer deadlock, so no reader thread is needed.
load_shedding::Result<ProcessResult> run_process(const std::string& executable,
                                                 const std::vector<std::string>& arguments,
                                                 const std::string& output_directory);

/// An interactive child with a line protocol on standard input and standard
/// output. Used for coordination that must not depend on timing.
class ChildProcess {
 public:
  ChildProcess() = default;
  ~ChildProcess();
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;
  ChildProcess(ChildProcess&& other) noexcept;
  ChildProcess& operator=(ChildProcess&& other) noexcept;

  static load_shedding::Result<ChildProcess> spawn(const std::string& executable,
                                                   const std::vector<std::string>& arguments,
                                                   const std::string& output_directory);

  load_shedding::Status write_line(const std::string& line);
  load_shedding::Status close_input();
  /// Reads one line from the child's standard output. Returns `NotFound` at end
  /// of stream.
  load_shedding::Result<std::string> read_line();
  /// Waits for the child to exit and returns its exit code.
  load_shedding::Result<int> wait();
  /// Terminates the child immediately. This is the non-interactive termination
  /// mechanism used by the crash and fencing tests: it is a kernel operation on
  /// the process handle, not a signal, an exception, or a CRT abort path.
  load_shedding::Status terminate();
  bool running() const;

 private:
#if defined(_WIN32)
  void* process_ = nullptr;
  void* thread_ = nullptr;
  void* input_write_ = nullptr;
  void* output_read_ = nullptr;
#else
  int input_write_ = -1;
  int output_read_ = -1;
  int child_pid_ = -1;
#endif
  std::string output_path_;
};

/// True when the process identified by `result` was terminated rather than
/// exiting on its own.
bool was_terminated(const ProcessResult& result);

}  // namespace ls_test
