// Command line proof obligations.
//
// The tool is exercised as a real process against real stores. It must report
// library behaviour rather than reimplement it, and its exit codes must
// distinguish "the command ran and refused" from "there is no such command".

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "fixture.hpp"
#include "load_shedding/json.hpp"
#include "load_shedding/json_io.hpp"
#include "proc.hpp"
#include "test_harness.hpp"

using namespace load_shedding;

namespace {

struct CliRun {
  int exit_code = 0;
  std::string out;
  std::string err;
  bool json_ok = false;
  JsonValue json;
};

CliRun run_cli(const std::string& scratch, const std::vector<std::string>& arguments) {
  CliRun result;
  const std::string executable = ls_test::cli_executable();
  if (executable.empty()) {
    return result;
  }
  auto run = ls_test::run_process(executable, arguments, scratch);
  if (!run.ok()) {
    return result;
  }
  result.exit_code = run.value().exit_code;
  result.out = run.value().standard_output;
  result.err = run.value().standard_error;
  auto parsed = parse_json(result.out);
  if (parsed.ok()) {
    result.json_ok = true;
    result.json = parsed.value();
  }
  return result;
}

const JsonValue* member(const CliRun& run, const char* name) { return run.json.find(name); }

}  // namespace

LS_TEST(cli, the_tool_is_available) {
  LS_CHECK_MSG(!ls_test::cli_executable().empty(),
               "LOAD_SHEDDING_CLI_EXE is not set; the suite must be run through CTest");
}

LS_TEST(cli, lifecycle_from_the_command_line) {
  if (ls_test::cli_executable().empty()) {
    return;
  }
  ls_test::ScratchDirectory scratch{"cli"};
  const std::string store = scratch.child("store");

  auto init = run_cli(scratch.path(), {"--store", store, "init", "--default-policy", "--json"});
  LS_CHECK_EQ(init.exit_code, 0);
  LS_CHECK(init.json_ok);
  LS_CHECK_EQ(member(init, "policy_generation")->as_int(), std::int64_t{1});

  auto add_a = run_cli(scratch.path(), {"--store", store, "--tick", "1", "load", "add", "--ref",
                                        "pump-a", "--contribution", "250000", "--json"});
  LS_CHECK_EQ(add_a.exit_code, 0);
  auto add_b = run_cli(scratch.path(), {"--store", store, "--tick", "2", "load", "add", "--ref",
                                        "lab-lighting", "--contribution", "40000", "--priority",
                                        "optional", "--json"});
  LS_CHECK_EQ(add_b.exit_code, 0);

  auto list = run_cli(scratch.path(), {"--store", store, "load", "list", "--json"});
  LS_CHECK_EQ(list.exit_code, 0);
  LS_CHECK_EQ(member(list, "loads")->items().size(), std::size_t{2});

  auto protected_add = run_cli(scratch.path(),
                               {"--store", store, "--tick", "3", "protected", "add", "--ref",
                                "keep-pump", "--load", "pump-a", "--reserved", "200000", "--json"});
  LS_CHECK_EQ(protected_add.exit_code, 0);
  auto protected_list = run_cli(scratch.path(), {"--store", store, "protected", "list", "--json"});
  LS_CHECK_EQ(protected_list.exit_code, 0);
  LS_CHECK_EQ(member(protected_list, "obligations")->items().size(), std::size_t{1});

  auto plan = run_cli(scratch.path(), {"--store", store, "plan", "--request", "1", "--deficit",
                                       "100000", "--tick", "4", "--json"});
  LS_CHECK_EQ(plan.exit_code, 0);
  LS_CHECK(plan.json_ok);
  const JsonValue* plan_object = member(plan, "plan");
  LS_CHECK(plan_object != nullptr);
  if (plan_object == nullptr) {
    return;
  }
  // pump-a carries a protected obligation, so only the optional load is
  // selectable and the deficit cannot be covered.
  LS_CHECK_EQ(plan_object->find("coverage")->as_string(), std::string("insufficient"));
  LS_CHECK_EQ(plan_object->find("closure")->find("residual_deficit_watts")->as_int(),
              std::int64_t{60000});
  const std::string digest = plan_object->find("content_digest")->as_string();
  LS_CHECK_EQ(digest.size(), std::size_t{64});

  auto explain = run_cli(scratch.path(), {"--store", store, "explain", "--plan", "1", "--json"});
  LS_CHECK_EQ(explain.exit_code, 0);
  LS_CHECK_EQ(member(explain, "total")->as_int(), std::int64_t{2});
  // The explanation trace is total: one record per considered load.
  LS_CHECK_EQ(member(explain, "considerations")->items().size(), std::size_t{2});

  auto plans = run_cli(scratch.path(), {"--store", store, "plans", "--json"});
  LS_CHECK_EQ(plans.exit_code, 0);
  LS_CHECK_EQ(member(plans, "plan_count")->as_int(), std::int64_t{1});

  auto show = run_cli(scratch.path(), {"--store", store, "plan", "show", "--plan", "1", "--json"});
  LS_CHECK_EQ(show.exit_code, 0);
  const JsonValue* closure = show.json.find("closure");
  LS_CHECK(closure != nullptr);

  auto verify = run_cli(scratch.path(), {"--store", store, "verify", "--json"});
  LS_CHECK_EQ(verify.exit_code, 0);
  LS_CHECK(member(verify, "ok")->as_bool());

  auto history = run_cli(scratch.path(), {"--store", store, "history", "--limit", "8", "--json"});
  LS_CHECK_EQ(history.exit_code, 0);
  LS_CHECK(member(history, "entries")->items().size() > 0);

  auto audit = run_cli(scratch.path(), {"--store", store, "store-audit", "--json"});
  LS_CHECK_EQ(audit.exit_code, 0);
  LS_CHECK(member(audit, "clean")->as_bool());

  auto authority = run_cli(scratch.path(), {"--store", store, "authority", "--json"});
  LS_CHECK_EQ(authority.exit_code, 0);
  LS_CHECK(member(authority, "epoch") == nullptr);  // authority_epoch is the member name
  LS_CHECK(member(authority, "authority_epoch") != nullptr);

  auto stages = run_cli(scratch.path(), {"--store", store, "stages", "--json"});
  LS_CHECK_EQ(stages.exit_code, 0);
  LS_CHECK_EQ(member(stages, "stages")->items().size(), std::size_t{3});

  auto policy_show = run_cli(scratch.path(), {"--store", store, "policy", "show", "--json"});
  LS_CHECK_EQ(policy_show.exit_code, 0);
  LS_CHECK_EQ(member(policy_show, "name")->as_string(), std::string("default"));

  auto revalidate = run_cli(scratch.path(), {"--store", store, "revalidate", "--plan", "1", "--json"});
  LS_CHECK_EQ(revalidate.exit_code, 0);
  LS_CHECK_EQ(member(revalidate, "verdict")->as_string(), std::string("current"));

  auto version = run_cli(scratch.path(), {"version", "--json"});
  LS_CHECK_EQ(version.exit_code, 0);
  LS_CHECK_EQ(member(version, "version")->as_string(), std::string("1.0.0"));
}

LS_TEST(cli, recovery_and_effect_commands) {
  if (ls_test::cli_executable().empty()) {
    return;
  }
  ls_test::ScratchDirectory scratch{"cli"};
  const std::string store = scratch.child("store");
  LS_CHECK_EQ(run_cli(scratch.path(), {"--store", store, "init", "--default-policy", "--quiet"})
                  .exit_code,
              0);
  LS_CHECK_EQ(run_cli(scratch.path(), {"--store", store, "--tick", "1", "load", "add", "--ref",
                                       "sheddable-a", "--contribution", "120000", "--quiet"})
                  .exit_code,
              0);
  LS_CHECK_EQ(run_cli(scratch.path(), {"--store", store, "plan", "--request", "1", "--deficit",
                                       "60000", "--tick", "2", "--quiet"})
                  .exit_code,
              0);
  auto observe = run_cli(scratch.path(),
                         {"--store", store, "effect", "observe", "--request", "2", "--plan", "1",
                          "--load", "sheddable-a", "--state", "shed", "--source", "adapter",
                          "--verification", "confirmed", "--tick", "3", "--json"});
  LS_CHECK_EQ(observe.exit_code, 0);
  LS_CHECK_EQ(member(observe, "observed")->as_string(), std::string("shed"));

  auto observed = run_cli(scratch.path(), {"--store", store, "observed", "--json"});
  LS_CHECK_EQ(observed.exit_code, 0);
  LS_CHECK_EQ(member(observed, "observed")->items().size(), std::size_t{1});

  auto effects = run_cli(scratch.path(), {"--store", store, "effect", "list", "--json"});
  LS_CHECK_EQ(effects.exit_code, 0);
  LS_CHECK_EQ(member(effects, "records")->items().size(), std::size_t{1});

  auto recovery = run_cli(scratch.path(),
                          {"--store", store, "recovery-order", "--request", "3", "--plan", "1",
                           "--headroom", "500000", "--tick", "10", "--json"});
  LS_CHECK_EQ(recovery.exit_code, 0);
  LS_CHECK_EQ(member(recovery, "restored_count")->as_int(), std::int64_t{1});
  LS_CHECK_EQ(member(recovery, "restored_expected_watts")->as_int(), std::int64_t{120000});

  auto show = run_cli(scratch.path(), {"--store", store, "recovery", "show", "--request", "3", "--json"});
  LS_CHECK_EQ(show.exit_code, 0);
  LS_CHECK_EQ(member(show, "candidate_count")->as_int(), std::int64_t{1});

  auto revalidate = run_cli(scratch.path(), {"--store", store, "revalidate", "--plan", "1", "--json"});
  LS_CHECK_EQ(revalidate.exit_code, 0);
  LS_CHECK_EQ(member(revalidate, "verdict")->as_string(), std::string("superseded-effect"));
}

LS_TEST(cli, policy_and_snapshot_documents_come_from_files) {
  if (ls_test::cli_executable().empty()) {
    return;
  }
  ls_test::ScratchDirectory scratch{"cli"};
  const std::string store = scratch.child("store");
  LS_CHECK_EQ(run_cli(scratch.path(), {"--store", store, "init", "--quiet"}).exit_code, 0);

  SheddingPolicy policy = make_default_policy();
  policy.name = "operator-policy";
  policy.stages[0].max_shed = Power::from_watts(50000);
  LS_REQUIRE_OK_STATUS(
      ls_test::write_bytes(scratch.child("policy.json"), to_canonical_json(to_json(policy))));
  auto install = run_cli(scratch.path(), {"--store", store, "policy", "install", "--file",
                                          scratch.child("policy.json"), "--json"});
  LS_CHECK_MSG(install.exit_code == 0, install.err);
  LS_CHECK_EQ(member(install, "policy_generation")->as_int(), std::int64_t{1});

  const std::string snapshot_json =
      "{\"id\":1,\"evidence_generation\":1,\"tick\":5,\"total_demand_state\":\"known\","
      "\"total_demand_watts\":300000,\"loads\":["
      "{\"ref\":\"chiller\",\"contribution_watts\":150000,\"priority\":\"standard\",\"tick\":5},"
      "{\"ref\":\"office\",\"contribution_watts\":30000,\"priority\":\"optional\",\"tick\":5}],"
      "\"obligations\":[{\"ref\":\"keep-chiller\",\"load\":\"chiller\","
      "\"reserved_watts\":100000,\"tick\":5}]}";
  LS_REQUIRE_OK_STATUS(ls_test::write_bytes(scratch.child("snapshot.json"), snapshot_json));
  auto snapshot = run_cli(scratch.path(), {"--store", store, "snapshot", "install", "--file",
                                           scratch.child("snapshot.json"), "--json"});
  LS_CHECK_MSG(snapshot.exit_code == 0, snapshot.err);
  LS_CHECK_EQ(member(snapshot, "evidence_generation")->as_int(), std::int64_t{1});

  auto show = run_cli(scratch.path(), {"--store", store, "snapshot", "show", "--json"});
  LS_CHECK_EQ(show.exit_code, 0);
  LS_CHECK_EQ(member(show, "loads")->items().size(), std::size_t{2});

  // A malformed document is refused with a named error, not a crash.
  LS_REQUIRE_OK_STATUS(ls_test::write_bytes(scratch.child("bad.json"), "{\"loads\":[{\"ref\":1}]}"));
  auto bad = run_cli(scratch.path(), {"--store", store, "snapshot", "install", "--file",
                                      scratch.child("bad.json"), "--json"});
  LS_CHECK_NE(bad.exit_code, 0);
  LS_CHECK_EQ(member(bad, "error")->as_string(), std::string("invalid-argument"));
}

LS_TEST(cli, failure_paths_are_explicit) {
  if (ls_test::cli_executable().empty()) {
    return;
  }
  ls_test::ScratchDirectory scratch{"cli"};
  const std::string store = scratch.child("store");

  auto unknown = run_cli(scratch.path(), {"--store", store, "frobnicate"});
  LS_CHECK_EQ(unknown.exit_code, 2);
  LS_CHECK(unknown.err.find("unknown command") != std::string::npos);

  auto no_store = run_cli(scratch.path(), {"plan", "--request", "1", "--deficit", "1", "--json"});
  LS_CHECK_NE(no_store.exit_code, 0);
  LS_CHECK_EQ(member(no_store, "error")->as_string(), std::string("invalid-argument"));

  auto missing_store =
      run_cli(scratch.path(), {"--store", scratch.child("absent"), "load", "list", "--json"});
  LS_CHECK_NE(missing_store.exit_code, 0);
  LS_CHECK_EQ(member(missing_store, "error")->as_string(), std::string("not-found"));

  // A refused operation against a real store exits non-zero but is not a usage
  // error: the command was understood.
  LS_CHECK_EQ(run_cli(scratch.path(), {"--store", store, "init", "--default-policy", "--quiet"})
                  .exit_code,
              0);
  auto missing_plan = run_cli(scratch.path(), {"--store", store, "explain", "--plan", "99", "--json"});
  LS_CHECK_EQ(missing_plan.exit_code, 1);
  LS_CHECK_EQ(member(missing_plan, "error")->as_string(), std::string("not-found"));

  auto bad_number = run_cli(scratch.path(), {"--store", store, "plan", "--request", "x", "--deficit",
                                             "1", "--json"});
  LS_CHECK_NE(bad_number.exit_code, 0);

  auto bad_option = run_cli(scratch.path(), {"--store", store, "load", "add", "--ref"});
  LS_CHECK_NE(bad_option.exit_code, 0);

  auto audit = run_cli(scratch.path(), {"--store", store, "store-audit", "--json"});
  LS_CHECK_EQ(audit.exit_code, 0);
}
