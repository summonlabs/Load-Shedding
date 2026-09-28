#pragma once

// Command line plumbing: argument parsing with explicit errors, engine opening
// with the documented authority rules, and a single result printer. The tool
// contains no decision logic of its own; everything it reports comes from the
// library.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "load_shedding/engine.hpp"
#include "load_shedding/json.hpp"
#include "load_shedding/status.hpp"

namespace load_shedding::cli {

enum class ExitCode : int {
  Ok = 0,
  Failure = 1,
  Usage = 2,
};

struct Arguments {
  std::vector<std::string> raw;
  std::vector<std::string> positional;

  bool has(std::string_view name) const;
  /// Value of `--name value`. Fails when the flag is present without a value.
  Result<std::string> value(std::string_view name) const;
  Result<std::uint64_t> unsigned_value(std::string_view name) const;
  Result<std::int64_t> integer_value(std::string_view name) const;
  Result<std::string> value_or(std::string_view name, std::string fallback) const;
  std::uint64_t unsigned_or(std::string_view name, std::uint64_t fallback, Status* error) const;
};

/// Splits the process arguments into flags and positionals.
Result<Arguments> parse_arguments(int argc, char** argv);

struct Session {
  std::string store;
  std::uint64_t incarnation = 1;
  std::uint64_t tick = 1;
  bool tick_given = false;
  bool json_output = false;
  bool quiet = false;
};

/// Reads the shared flags that appear before the subcommand.
Result<Session> parse_session(const Arguments& arguments);

/// Opens the store for a read-only command (shared lock) or a mutating command
/// (exclusive lock, writer authority taken).
Result<Engine> open_engine(const Session& session, bool for_write);

/// Prints a JSON document or a plain line, according to the session format.
void emit(const Session& session, const JsonValue& value);
void emit_error(const Session& session, const Status& status);
void emit_line(const Session& session, std::string_view text);

/// Reads a whole JSON document from a file, with bounded size and strict
/// parsing. Reports the file name in errors.
Result<JsonValue> read_json_file(const std::string& path);

/// Reads UTF-8 text from stdin, bounded.
Result<std::string> read_stdin(std::size_t max_bytes);

std::string format_bytes(std::uint64_t value);

}  // namespace load_shedding::cli
