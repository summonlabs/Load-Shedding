#include "cli_support.hpp"

#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <utility>

#include "load_shedding/limits.hpp"
#include "load_shedding/text.hpp"

namespace load_shedding::cli {
namespace {

bool is_flag(const std::string& token) {
  return token.size() > 2 && token[0] == '-' && token[1] == '-';
}

/// Flags that stand alone. Every other flag consumes the following token, so
/// `--json plan` and `--store DIR plan` both parse the way they read.
bool is_standalone_flag(std::string_view token) {
  static constexpr std::string_view kStandalone[] = {
      "--json", "--quiet", "--force", "--emergency", "--help", "--text", "--all"};
  for (const std::string_view candidate : kStandalone) {
    if (token == candidate) {
      return true;
    }
  }
  return false;
}

}  // namespace

bool Arguments::has(std::string_view name) const {
  const std::string token = "--" + std::string(name);
  for (const std::string& candidate : raw) {
    if (candidate == token) {
      return true;
    }
  }
  return false;
}

Result<std::string> Arguments::value(std::string_view name) const {
  const std::string token = "--" + std::string(name);
  for (std::size_t index = 0; index < raw.size(); ++index) {
    if (raw[index] == token) {
      if (index + 1 >= raw.size()) {
        return Status::error(StatusCode::InvalidArgument,
                             "option '" + token + "' requires a value");
      }
      if (is_flag(raw[index + 1])) {
        return Status::error(StatusCode::InvalidArgument,
                             "option '" + token + "' requires a value, found '" + raw[index + 1] + "'");
      }
      return raw[index + 1];
    }
  }
  return Status::error(StatusCode::NotFound, "option '" + token + "' was not given");
}

Result<std::int64_t> Arguments::integer_value(std::string_view name) const {
  auto text = value(name);
  if (!text.ok()) {
    return text.status();
  }
  const std::string& digits = text.value();
  if (digits.empty()) {
    return Status::error(StatusCode::InvalidArgument, "option value is empty");
  }
  std::size_t index = 0;
  bool negative = false;
  if (digits[0] == '-') {
    negative = true;
    index = 1;
    if (digits.size() == 1) {
      return Status::error(StatusCode::InvalidArgument, "option value is not an integer");
    }
  }
  std::int64_t magnitude = 0;
  for (; index < digits.size(); ++index) {
    if (digits[index] < '0' || digits[index] > '9') {
      return Status::error(StatusCode::InvalidArgument,
                           "option value '" + digits + "' is not an integer");
    }
    if (magnitude > (std::numeric_limits<std::int64_t>::max() - (digits[index] - '0')) / 10) {
      return Status::error(StatusCode::Overflow, "option value '" + digits + "' is out of range");
    }
    magnitude = magnitude * 10 + (digits[index] - '0');
  }
  return negative ? -magnitude : magnitude;
}

Result<std::uint64_t> Arguments::unsigned_value(std::string_view name) const {
  auto parsed = integer_value(name);
  if (!parsed.ok()) {
    return parsed.status();
  }
  if (parsed.value() < 0) {
    return Status::error(StatusCode::OutOfRange,
                         "option '--" + std::string(name) + "' must not be negative");
  }
  return static_cast<std::uint64_t>(parsed.value());
}

Result<std::string> Arguments::value_or(std::string_view name, std::string fallback) const {
  auto parsed = value(name);
  if (!parsed.ok()) {
    if (parsed.code() == StatusCode::NotFound) {
      return fallback;
    }
    return parsed.status();
  }
  return parsed.value();
}

std::uint64_t Arguments::unsigned_or(std::string_view name, std::uint64_t fallback,
                                     Status* error) const {
  auto parsed = unsigned_value(name);
  if (!parsed.ok()) {
    if (parsed.code() == StatusCode::NotFound) {
      return fallback;
    }
    if (error != nullptr) {
      *error = parsed.status();
    }
    return fallback;
  }
  return parsed.value();
}

Result<Arguments> parse_arguments(int argc, char** argv) {
  Arguments arguments;
  for (int index = 0; index < argc; ++index) {
    arguments.raw.emplace_back(argv[index]);
  }
  for (std::size_t index = 1; index < arguments.raw.size(); ++index) {
    const std::string& token = arguments.raw[index];
    if (is_flag(token)) {
      if (!is_standalone_flag(token) && index + 1 < arguments.raw.size() &&
          !is_flag(arguments.raw[index + 1])) {
        ++index;
      }
      continue;
    }
    arguments.positional.push_back(token);
  }
  return arguments;
}

Result<Session> parse_session(const Arguments& arguments) {
  Session session;
  Status error;
  session.store = arguments.value_or("store", "").value();
  session.incarnation = arguments.unsigned_or("incarnation", 1, &error);
  if (!error.ok()) {
    return error;
  }
  session.tick = arguments.unsigned_or("tick", 0, &error);
  if (!error.ok()) {
    return error;
  }
  session.tick_given = arguments.has("tick");
  session.json_output = arguments.has("json");
  session.quiet = arguments.has("quiet");
  return session;
}

Result<Engine> open_engine(const Session& session, bool for_write) {
  if (session.store.empty()) {
    return Status::error(StatusCode::InvalidArgument, "--store <directory> is required");
  }
  EngineOptions options;
  options.store_directory = session.store;
  options.incarnation = Incarnation::from_value(session.incarnation);
  options.take_authority = for_write;
  options.create_if_missing = for_write;
  if (session.tick_given) {
    options.initial_tick = Tick::from_value(session.tick);
  }
  return Engine::open(options);
}

void emit(const Session& session, const JsonValue& value) {
  if (session.json_output) {
    std::cout << to_canonical_json(value) << "\n";
    return;
  }
  std::cout << to_pretty_json(value);
}

void emit_error(const Session& session, const Status& status) {
  if (session.json_output) {
    JsonValue object = JsonValue::object();
    object.set("ok", JsonValue::boolean(false));
    object.set("error", JsonValue::text(std::string(to_string(status.code()))));
    object.set("message", JsonValue::text(status.message()));
    std::cout << to_canonical_json(object) << "\n";
    return;
  }
  std::cerr << "error: " << status.to_string() << "\n";
}

void emit_line(const Session& session, std::string_view text) {
  if (!session.quiet) {
    std::cout << text << "\n";
  }
}

Result<JsonValue> read_json_file(const std::string& path) {
  if (path == "-") {
    auto content = read_stdin(limits::kMaxJsonBytes);
    if (!content.ok()) {
      return content.status();
    }
    return parse_json(content.value());
  }
  const Status path_valid = validate_text(path, 4096, "input path");
  if (!path_valid.ok()) {
    return path_valid;
  }
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return Status::error(StatusCode::NotFound, "could not open '" + path + "'");
  }
  std::string content;
  content.resize(limits::kMaxJsonBytes + 1);
  stream.read(content.data(), static_cast<std::streamsize>(content.size()));
  const std::streamsize read = stream.gcount();
  if (read < 0) {
    return Status::error(StatusCode::IoFailure, "could not read '" + path + "'");
  }
  content.resize(static_cast<std::size_t>(read));
  return parse_json(content);
}

Result<std::string> read_stdin(std::size_t max_bytes) {
  std::ostringstream buffer;
  std::string line;
  std::size_t total = 0;
  while (std::getline(std::cin, line)) {
    total += line.size() + 1;
    if (total > max_bytes) {
      return Status::error(StatusCode::Overlong, "standard input exceeds the accepted bound");
    }
    buffer << line << "\n";
  }
  return buffer.str();
}

std::string format_bytes(std::uint64_t value) { return std::to_string(value) + " B"; }

}  // namespace load_shedding::cli
