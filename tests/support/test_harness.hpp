#pragma once

// Minimal deterministic test harness: registration, checks, and a runner.
//
// There are no timeouts, no watchdogs, and no process limits anywhere in the
// suite. A check either passes, fails, or the program does not finish, and an
// unfinished program is a defect to diagnose.

#include <cstdint>
#include <functional>
#include <ostream>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

#include "load_shedding/engine.hpp"
#include "load_shedding/json.hpp"

namespace ls_test {

using Body = std::function<void()>;

struct TestCase {
  std::string suite;
  std::string name;
  Body body;
};

std::vector<TestCase>& registry();

class Registrar {
 public:
  Registrar(const char* suite, const char* name, Body body);
};

/// Records a failure for the currently running test.
void report_failure(const char* file, int line, const std::string& expression,
                    const std::string& detail);

/// Repeated-check counter used by property tests: a property that is never
/// exercised is a vacuous property, so the test asserts the counter as well.
std::uint64_t& property_iterations();

template <typename T>
std::string display(const T& value) {
  if constexpr (std::is_same_v<T, std::string>) {
    return value;
  } else if constexpr (std::is_same_v<T, bool>) {
    return value ? "true" : "false";
  } else if constexpr (std::is_same_v<T, const char*>) {
    return std::string(value == nullptr ? "<null>" : value);
  } else if constexpr (requires(std::ostream& stream, const T& item) { stream << item; }) {
    std::ostringstream stream;
    stream << value;
    return stream.str();
  } else if constexpr (requires(const T& item) { item.to_string(); }) {
    return value.to_string();
  } else if constexpr (requires(const T& item) { to_string(item); }) {
    return std::string(to_string(value));
  } else if constexpr (std::is_enum_v<T>) {
    return std::string(load_shedding::to_string(value));
  } else if constexpr (std::is_integral_v<T>) {
    return std::to_string(value);
  } else {
    return "<value>";
  }
}

inline load_shedding::Status status_of(const load_shedding::Status& value) { return value; }

template <typename T>
load_shedding::Status status_of(const load_shedding::Result<T>& value) {
  return value.status();
}

int run_all(int argc, char** argv);

}  // namespace ls_test

#define LS_TEST(suite, name)                                                              static void ls_test_body_##suite##_##name();                                            static const ::ls_test::Registrar ls_test_registrar_##suite##_##name(                       #suite, #name, ls_test_body_##suite##_##name);                                      static void ls_test_body_##suite##_##name()

#define LS_CHECK(expression)                                                          do {                                                                                  if (!(expression)) {                                                                  ::ls_test::report_failure(__FILE__, __LINE__, #expression, "");                   }                                                                                 } while (false)

#define LS_CHECK_MSG(expression, detail)                                              do {                                                                                  if (!(expression)) {                                                                  ::ls_test::report_failure(__FILE__, __LINE__, #expression, (detail));             }                                                                                 } while (false)

#define LS_CHECK_EQ(left, right)                                                            do {                                                                                        const auto& ls_left_value = (left);                                                       const auto& ls_right_value = (right);                                                     if (!(ls_left_value == ls_right_value)) {                                                   ::ls_test::report_failure(__FILE__, __LINE__, #left " == " #right,                                                  "left=" + ::ls_test::display(ls_left_value) +                                                 " right=" + ::ls_test::display(ls_right_value));          }                                                                                       } while (false)

#define LS_CHECK_NE(left, right)                                                       do {                                                                                   const auto& ls_left_value = (left);                                                  const auto& ls_right_value = (right);                                                if (ls_left_value == ls_right_value) {                                                 ::ls_test::report_failure(__FILE__, __LINE__, #left " != " #right,                                             "both=" + ::ls_test::display(ls_left_value));            }                                                                                  } while (false)

/// Requires the result to hold and returns its value. On failure the check is
/// reported and a default-constructed value is returned, so the test continues
/// and reports every defect it can rather than dying at the first one.
#define LS_REQUIRE_OK(expression)                                                            ([&]() {                                                                                     auto ls_result_value = (expression);                                                       if (!ls_result_value.ok()) {                                                                 ::ls_test::report_failure(__FILE__, __LINE__, #expression " is ok",                                                  "status=" + ls_result_value.status().to_string());               return typename std::decay_t<decltype(ls_result_value)>::value_type{};                   }                                                                                          return ls_result_value.value();                                                          }())

/// Requires the status to fail with exactly the given code.
#define LS_REQUIRE_STATUS(expression, expected_code)                                          do {                                                                                          const ::load_shedding::Status ls_status_value = ::ls_test::status_of(expression);            if (ls_status_value.code() != (expected_code)) {                                               ::ls_test::report_failure(__FILE__, __LINE__,                                                                          #expression " fails with " #expected_code,                                                   "actual=" + ls_status_value.to_string());                        }                                                                                          } while (false)

/// Requires a plain `Status` to be ok.
#define LS_REQUIRE_OK_STATUS(expression)                                                       do {                                                                                           const ::load_shedding::Status ls_plain_status = (expression);                                 if (!ls_plain_status.ok()) {                                                                    ::ls_test::report_failure(__FILE__, __LINE__, #expression " is ok",                                                    "status=" + ls_plain_status.to_string());                         }                                                                                          } while (false)

/// Counts one exercised iteration of a property or randomized test.
#define LS_COUNT_ITERATION() (void)(++::ls_test::property_iterations())
