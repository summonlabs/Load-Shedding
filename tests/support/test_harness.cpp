#include "test_harness.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace ls_test {
namespace {

std::string g_current_test;
int g_current_failures = 0;
std::uint64_t g_property_iterations = 0;

}  // namespace

std::vector<TestCase>& registry() {
  static std::vector<TestCase> cases;
  return cases;
}

Registrar::Registrar(const char* suite, const char* name, Body body) {
  registry().push_back(TestCase{suite, name, std::move(body)});
}

void report_failure(const char* file, int line, const std::string& expression,
                    const std::string& detail) {
  ++g_current_failures;
  std::cout << "    FAIL " << file << ":" << line << ": " << expression;
  if (!detail.empty()) {
    std::cout << "  [" << detail << "]";
  }
  std::cout << "\n";
}

std::uint64_t& property_iterations() { return g_property_iterations; }

int run_all(int argc, char** argv) {
  // Unbuffered output: if a test aborts the process, the report up to that point
  // is still on the console instead of being lost with the buffer.
  std::cout << std::unitbuf;
  std::string filter;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--filter" && index + 1 < argc) {
      filter = argv[index + 1];
      ++index;
    } else if (argument.rfind("--filter=", 0) == 0) {
      filter = argument.substr(9);
    }
  }
  int failed_tests = 0;
  int executed = 0;
  for (const TestCase& test : registry()) {
    const std::string full_name = test.suite + "." + test.name;
    if (!filter.empty() && full_name.find(filter) == std::string::npos) {
      continue;
    }
    ++executed;
    g_current_failures = 0;
    g_property_iterations = 0;
    std::cout << "[" << full_name << "]\n";
    // An escaping exception is reported as a failure of that test rather than
    // being allowed to abort the process: a test binary that dies loses every
    // result that follows, and an abort path is an interactive CRT path.
    try {
      test.body();
    } catch (const std::exception& error) {
      report_failure(__FILE__, __LINE__, "test body did not throw",
                     std::string("threw: ") + error.what());
    } catch (...) {
      report_failure(__FILE__, __LINE__, "test body did not throw", "threw an unknown exception");
    }
    if (g_current_failures == 0) {
      std::cout << "  ok (" << g_property_iterations << " iterations)\n";
    } else {
      ++failed_tests;
      std::cout << "  " << g_current_failures << " failure(s)\n";
    }
  }
  std::cout << "\n" << (executed - failed_tests) << "/" << executed << " test(s) passed\n";
  return failed_tests == 0 && executed > 0 ? 0 : 1;
}

}  // namespace ls_test

int main(int argc, char** argv) { return ::ls_test::run_all(argc, argv); }
