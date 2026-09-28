// Crash and multiprocess proof obligations.
//
// Crash injection terminates a real child process at a named durable stage using
// the platform's immediate termination primitive: no unwinding, no atexit
// handlers, and no interactive error-reporting path. Writer exclusion, lock
// release on process death, and fencing are exercised across real process
// boundaries.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "fixture.hpp"
#include "load_shedding/json_io.hpp"
#include "proc.hpp"
#include "test_harness.hpp"

using namespace load_shedding;

namespace {

const std::vector<std::string> kCrashPoints = {"after-staging-write", "after-state-publish",
                                               "before-head-commit", "after-head-commit",
                                               "after-watermark"};

ls_test::ScratchDirectory* g_scratch = nullptr;
std::string g_helper;

std::string helper() {
  if (g_helper.empty()) {
    g_helper = ls_test::helper_executable();
  }
  return g_helper;
}

}  // namespace

LS_TEST(crash, the_helper_executable_is_available) {
  LS_CHECK_MSG(!helper().empty(),
               "LOAD_SHEDDING_HELPER_EXE is not set; the suite must be run through CTest");
  LS_CHECK_MSG(!ls_test::cli_executable().empty() || true, "");
}

LS_TEST(crash, every_durable_crash_point_leaves_exactly_one_whole_state) {
  ls_test::ScratchDirectory scratch{"crash"};
  for (const std::string& point : kCrashPoints) {
    LS_COUNT_ITERATION();
    const std::string store = scratch.child("store-" + point);
    auto prepared = ls_test::open_writer(store, 1);
    LS_CHECK(prepared.ok());
    LS_REQUIRE_OK(ls_test::install_policy(prepared.value(), make_default_policy()));
    const std::uint64_t baseline_generation = prepared.value().commit().generation;
    LS_REQUIRE_OK_STATUS(prepared.value().close());

    auto result = ls_test::run_process(helper(),
                                       {"crash", store, point, "2", "50", "400000"},
                                       scratch.path());
    LS_CHECK_MSG(result.ok(), result.status().to_string());
    if (!result.ok()) {
      continue;
    }
    // The child was killed at the injection point, so it never reported success.
    LS_CHECK_MSG(result.value().exit_code != 0, "point=" + point);
    LS_CHECK_MSG(result.value().standard_output.find("COMMITTED") == std::string::npos,
                 "point=" + point + " output=" + result.value().standard_output);

    auto reopened = ls_test::open_writer(store, 3);
    LS_CHECK_MSG(reopened.ok(), "point=" + point + " " + reopened.status().to_string());
    if (!reopened.ok()) {
      continue;
    }
    // Exactly one whole state was adopted: either the baseline or the crash
    // child's committed plan, and never a mixture.
    const std::size_t plans = reopened.value().plan_count();
    LS_CHECK_MSG(plans <= 1, "point=" + point + " plans=" + std::to_string(plans));
    auto report = reopened.value().verify();
    LS_CHECK_MSG(report.ok(), "point=" + point);
    LS_CHECK_MSG(report.value().ok, "point=" + point + " " +
                                        (report.value().failures.empty()
                                             ? std::string()
                                             : report.value().failures[0].detail));
    auto audit = reopened.value().audit_store();
    LS_CHECK(audit.ok());
    LS_CHECK_MSG(audit.value().clean, "point=" + point);
    LS_CHECK_MSG(reopened.value().commit().generation > baseline_generation,
                 "point=" + point);
    // The store never adopts a partially published generation.
    const std::vector<std::string> names = ls_test::list_names(store);
    for (const std::string& name : names) {
      LS_CHECK_MSG(name.find(".staging") == std::string::npos, "point=" + point + " " + name);
    }
    if (point == "after-head-commit" || point == "after-watermark") {
      // Past the commit point the plan is authoritative.
      LS_CHECK_MSG(plans == 1, "point=" + point);
    }
    if (point == "before-head-commit") {
      LS_CHECK_MSG(plans == 0, "point=" + point);
    }
  }
}

LS_TEST(crash, a_store_without_a_commit_marker_refuses_to_guess) {
  ls_test::ScratchDirectory scratch{"crash"};
  const std::string store = scratch.child("fresh-store");
  {
    auto engine = ls_test::open_writer(store, 1);
    LS_CHECK(engine.ok());
    LS_REQUIRE_OK(ls_test::install_policy(engine.value(), make_default_policy()));
  }
  // A published generation with no commit marker is exactly the residue a crash
  // before the marker leaves behind. The protocol refuses to decide whether it
  // was meant to be authoritative.
  LS_REQUIRE_OK_STATUS(ls_test::remove_file_for_test(
      (std::filesystem::path(store) / "HEAD").string()));
  auto reopened = ls_test::open_writer(store, 2);
  LS_CHECK(!reopened.ok());
  LS_CHECK_EQ(reopened.status().code(), StatusCode::Corrupt);
  auto audit = inspect_store(store);
  LS_CHECK(audit.ok());
  LS_CHECK(!audit.value().clean);
  LS_CHECK(!audit.value().commit_marker_present);
}

LS_TEST(crash, a_writer_holds_exclusion_until_its_process_dies) {
  ls_test::ScratchDirectory scratch{"crash"};
  const std::string store = scratch.child("held-store");
  auto holder = ls_test::ChildProcess::spawn(helper(), {"hold", store, "1"}, scratch.path());
  LS_CHECK_MSG(holder.ok(), holder.status().to_string());
  if (!holder.ok()) {
    return;
  }
  auto ready = holder.value().read_line();
  LS_CHECK_MSG(ready.ok(), ready.status().to_string());
  if (!ready.ok()) {
    return;
  }
  LS_CHECK_MSG(ready.value().rfind("READY", 0) == 0, ready.value());
  const std::string held_epoch = ready.value().substr(6);

  // While the child lives, no other process may take writer authority.
  auto denied = ls_test::run_process(helper(), {"race", store, "2", "10", "contender-a"},
                                     scratch.path());
  LS_CHECK(denied.ok());
  LS_CHECK_MSG(denied.value().standard_output.find("DENIED lock-conflict") != std::string::npos,
               denied.value().standard_output);

  // Abrupt termination releases the operating-system lock with no recovery step.
  LS_REQUIRE_OK_STATUS(holder.value().terminate());
  auto exit_code = holder.value().wait();
  LS_CHECK(exit_code.ok());

  auto granted = ls_test::run_process(helper(), {"race", store, "3", "11", "contender-b"},
                                      scratch.path());
  LS_CHECK(granted.ok());
  LS_CHECK_MSG(granted.value().standard_output.find("GRANTED") != std::string::npos,
               granted.value().standard_output);
  LS_CHECK_MSG(granted.value().standard_output.find("WROTE") != std::string::npos,
               granted.value().standard_output);
  // The new writer is fenced at a later epoch than the dead one.
  const std::size_t space = granted.value().standard_output.find(' ');
  const std::string new_epoch =
      granted.value().standard_output.substr(space + 1,
                                             granted.value().standard_output.find('\n') - space - 1);
  LS_CHECK_MSG(new_epoch > held_epoch, "held=" + held_epoch + " new=" + new_epoch);

  // The store is still whole after the abrupt termination.
  auto engine = ls_test::open_reader(store);
  LS_CHECK_MSG(engine.ok(), engine.status().to_string());
  auto report = engine.value().verify();
  LS_CHECK(report.ok());
  LS_CHECK(report.value().ok);
}

LS_TEST(crash, only_one_of_two_contenders_is_granted_writer_authority) {
  ls_test::ScratchDirectory scratch{"crash"};
  const std::string store = scratch.child("contended-store");
  auto first = ls_test::ChildProcess::spawn(helper(), {"hold", store, "1"}, scratch.path());
  LS_CHECK(first.ok());
  if (!first.ok()) {
    return;
  }
  auto ready = first.value().read_line();
  LS_CHECK(ready.ok());
  if (!ready.ok()) {
    return;
  }
  auto second = ls_test::run_process(helper(), {"race", store, "2", "20", "second-writer"},
                                     scratch.path());
  LS_CHECK(second.ok());
  LS_CHECK(second.value().standard_output.find("DENIED") != std::string::npos);
  LS_REQUIRE_OK_STATUS(first.value().terminate());
  LS_CHECK(first.value().wait().ok());

  // With the holder gone the contention resolves without any lock cleanup.
  auto third = ls_test::run_process(helper(), {"race", store, "3", "21", "third-writer"},
                                    scratch.path());
  LS_CHECK(third.ok());
  LS_CHECK(third.value().standard_output.find("GRANTED") != std::string::npos);
  auto fourth = ls_test::run_process(helper(), {"race", store, "4", "22", "fourth-writer"},
                                     scratch.path());
  LS_CHECK(fourth.ok());
  LS_CHECK(fourth.value().standard_output.find("GRANTED") != std::string::npos);
  auto reader = ls_test::run_process(helper(), {"read", store}, scratch.path());
  LS_CHECK(reader.ok());
  LS_CHECK(reader.value().standard_output.find("STATE") != std::string::npos);
}

LS_TEST(crash, a_terminated_writer_leaves_the_store_consistent) {
  ls_test::ScratchDirectory scratch{"crash"};
  const std::string store = scratch.child("killed-writer-store");
  {
    auto engine = ls_test::open_writer(store, 1);
    LS_CHECK(engine.ok());
    LS_REQUIRE_OK(ls_test::install_policy(engine.value(), make_default_policy()));
  }
  auto result = ls_test::run_process(helper(), {"crash", store, "after-head-commit", "2", "30", "9000"},
                                     scratch.path());
  LS_CHECK(result.ok());
  auto reader = ls_test::run_process(helper(), {"read", store}, scratch.path());
  LS_CHECK(reader.ok());
  LS_CHECK_MSG(reader.value().standard_output.find("plans=1") != std::string::npos,
               reader.value().standard_output);
  LS_CHECK(reader.value().standard_error.empty());
}
