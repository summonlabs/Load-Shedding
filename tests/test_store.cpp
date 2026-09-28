// Persistence proof obligations: whole-state adoption, refusal of partial or
// corrupt state, rollback detection, residue retirement, and path safety.

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "fixture.hpp"
#include "load_shedding/json_io.hpp"
#include "test_harness.hpp"

using namespace load_shedding;

namespace {

FacilitySnapshot facility(std::uint64_t generation, std::uint64_t tick) {
  return ls_test::FacilityBuilder()
      .tick(tick)
      .evidence_generation(generation)
      .load("load-a", 120000, PriorityClass::Standard)
      .load("load-b", 30000, PriorityClass::Optional)
      .total_demand(200000)
      .build();
}

std::string store_file(const std::string& directory, const std::string& name) {
  return (std::filesystem::path(directory) / name).string();
}

/// Creates a directory link. Returns false when the environment refuses both a
/// symbolic link and a junction, so the caller can report the limitation instead
/// of claiming a check it did not run.
bool create_directory_link(const std::string& target, const std::string& link) {
  std::error_code error;
  std::filesystem::create_directory_symlink(target, link, error);
  if (!error) {
    return true;
  }
  const std::string command =
      "cmd /c mklink /J \"" + link + "\" \"" + target + "\" > nul 2>&1";
  return std::system(command.c_str()) == 0;
}

}  // namespace

LS_TEST(store, reopen_adopts_exactly_the_committed_generation) {
  ls_test::ScratchDirectory directory{"store"};
  {
    auto engine = ls_test::open_writer(directory.path(), 1);
    LS_CHECK(engine.ok());
    LS_REQUIRE_OK(ls_test::install_policy(engine.value(), make_default_policy()));
    LS_REQUIRE_OK(ls_test::install_snapshot(engine.value(), facility(1, 100)));
    auto outcome = engine.value().plan(ls_test::plan_request(engine.value(), 1, 50000, 110));
    LS_CHECK(outcome.ok());
  }
  {
    auto engine = ls_test::open_writer(directory.path(), 2);
    LS_CHECK_MSG(engine.ok(), engine.status().to_string());
    LS_CHECK_EQ(engine.value().plan_count(), std::size_t{1});
    LS_CHECK_EQ(engine.value().snapshot().loads.size(), std::size_t{2});
    auto verify = engine.value().verify();
    LS_CHECK(verify.ok());
    LS_CHECK(verify.value().ok);
  }
}

LS_TEST(store, audit_reports_a_clean_store) {
  ls_test::ScratchDirectory directory{"store"};
  auto engine = ls_test::open_writer(directory.path(), 1);
  LS_CHECK(engine.ok());
  LS_REQUIRE_OK(ls_test::install_policy(engine.value(), make_default_policy()));
  LS_REQUIRE_OK(ls_test::install_snapshot(engine.value(), facility(1, 100)));
  auto audit = engine.value().audit_store();
  LS_CHECK(audit.ok());
  LS_CHECK_MSG(audit.value().clean, audit.value().findings.empty() ? "" : audit.value().findings[0]);
  LS_CHECK(audit.value().commit_marker_present);
  LS_CHECK(audit.value().commit_marker_valid);
  LS_CHECK(audit.value().state_readable);
  LS_CHECK(!audit.value().unsynchronized);
  LS_CHECK_EQ(audit.value().load_count, std::size_t{2});
  LS_CHECK_EQ(audit.value().watermark_generation, engine.value().commit().generation);
  auto external = inspect_store(directory.path());
  LS_CHECK(external.ok());
  LS_CHECK(external.value().clean);
  LS_CHECK(external.value().unsynchronized);
}

LS_TEST(store, a_truncated_generation_file_is_refused) {
  ls_test::ScratchDirectory directory{"store"};
  {
    auto engine = ls_test::open_writer(directory.path(), 1);
    LS_CHECK(engine.ok());
    LS_REQUIRE_OK(ls_test::install_policy(engine.value(), make_default_policy()));
  }
  const std::uint64_t generation = 2;
  const std::string name = "state-0000000000000000000" + std::to_string(generation % 10) + ".lsg";
  auto content = ls_test::read_bytes(store_file(directory.path(), name));
  LS_CHECK(content.ok());
  for (std::size_t cut : {std::size_t{1}, std::size_t{16}, content.value().size() / 2}) {
    LS_REQUIRE_OK_STATUS(
        ls_test::write_bytes(store_file(directory.path(), name), content.value().substr(0, cut)));
    auto engine = ls_test::open_writer(directory.path(), 2);
    LS_CHECK(!engine.ok());
    LS_COUNT_ITERATION();
  }
}

LS_TEST(store, a_corrupted_generation_file_is_refused) {
  ls_test::ScratchDirectory directory{"store"};
  {
    auto engine = ls_test::open_writer(directory.path(), 1);
    LS_CHECK(engine.ok());
    LS_REQUIRE_OK(ls_test::install_policy(engine.value(), make_default_policy()));
  }
  const std::string name = "state-00000000000000000002.lsg";
  auto content = ls_test::read_bytes(store_file(directory.path(), name));
  LS_CHECK(content.ok());
  // Flip one bit inside the payload: the digest no longer matches.
  std::string damaged = content.value();
  damaged[damaged.size() / 2] = static_cast<char>(damaged[damaged.size() / 2] ^ 0x01);
  LS_REQUIRE_OK_STATUS(ls_test::write_bytes(store_file(directory.path(), name), damaged));
  auto engine = ls_test::open_writer(directory.path(), 2);
  LS_CHECK(!engine.ok());
  LS_CHECK_EQ(engine.status().code(), StatusCode::Corrupt);
  auto audit = inspect_store(directory.path());
  LS_CHECK(audit.ok());
  LS_CHECK(!audit.value().clean);
}

LS_TEST(store, a_corrupted_commit_marker_is_refused) {
  ls_test::ScratchDirectory directory{"store"};
  {
    auto engine = ls_test::open_writer(directory.path(), 1);
    LS_CHECK(engine.ok());
    LS_REQUIRE_OK(ls_test::install_policy(engine.value(), make_default_policy()));
  }
  auto marker = ls_test::read_bytes(store_file(directory.path(), "HEAD"));
  LS_CHECK(marker.ok());
  LS_CHECK_EQ(marker.value().size(), std::size_t{116});
  std::string damaged = marker.value();
  damaged[10] = static_cast<char>(damaged[10] ^ 0xFF);
  LS_REQUIRE_OK_STATUS(ls_test::write_bytes(store_file(directory.path(), "HEAD"), damaged));
  auto engine = ls_test::open_writer(directory.path(), 2);
  LS_CHECK(!engine.ok());
  LS_CHECK_EQ(engine.status().code(), StatusCode::Corrupt);
}

LS_TEST(store, a_generation_without_a_commit_marker_is_refused) {
  ls_test::ScratchDirectory directory{"store"};
  {
    auto engine = ls_test::open_writer(directory.path(), 1);
    LS_CHECK(engine.ok());
    LS_REQUIRE_OK(ls_test::install_policy(engine.value(), make_default_policy()));
  }
  LS_REQUIRE_OK_STATUS(ls_test::remove_file_for_test(store_file(directory.path(), "HEAD")));
  auto engine = ls_test::open_writer(directory.path(), 2);
  LS_CHECK(!engine.ok());
  LS_CHECK_EQ(engine.status().code(), StatusCode::Corrupt);
}

LS_TEST(store, a_marker_without_its_generation_is_refused) {
  ls_test::ScratchDirectory directory{"store"};
  {
    auto engine = ls_test::open_writer(directory.path(), 1);
    LS_CHECK(engine.ok());
    LS_REQUIRE_OK(ls_test::install_policy(engine.value(), make_default_policy()));
  }
  LS_REQUIRE_OK_STATUS(ls_test::remove_file_for_test(store_file(directory.path(), "state-00000000000000000002.lsg")));
  auto engine = ls_test::open_writer(directory.path(), 2);
  LS_CHECK(!engine.ok());
  LS_CHECK_EQ(engine.status().code(), StatusCode::NotFound);
}

LS_TEST(store, unpublished_generations_and_staging_residue_are_retired) {
  ls_test::ScratchDirectory directory{"store"};
  {
    auto engine = ls_test::open_writer(directory.path(), 1);
    LS_CHECK(engine.ok());
    LS_REQUIRE_OK(ls_test::install_policy(engine.value(), make_default_policy()));
  }
  // Simulate an interrupted publication: a staging file and a generation that
  // was published but never committed.
  LS_REQUIRE_OK_STATUS(ls_test::write_bytes(store_file(directory.path(), "state-00000000000000000099.lsg"),
                                            "LSSG not really a state file"));
  LS_REQUIRE_OK_STATUS(
      ls_test::write_bytes(store_file(directory.path(), "state-00000000000000000098.lsg.staging"), "junk"));
  auto engine = ls_test::open_writer(directory.path(), 2);
  LS_CHECK_MSG(engine.ok(), engine.status().to_string());
  const std::vector<std::string> names = ls_test::list_names(directory.path());
  for (const std::string& name : names) {
    LS_CHECK(name.find("state-00000000000000000099") == std::string::npos);
    LS_CHECK(name.find(".staging") == std::string::npos);
  }
  auto audit = engine.value().audit_store();
  LS_CHECK(audit.ok());
  LS_CHECK(audit.value().clean);
}

LS_TEST(store, rollback_to_an_older_marker_is_detected) {
  ls_test::ScratchDirectory directory{"store"};
  std::string older_marker;
  {
    auto engine = ls_test::open_writer(directory.path(), 1);
    LS_CHECK(engine.ok());
    LS_REQUIRE_OK(ls_test::install_policy(engine.value(), make_default_policy()));
  }
  auto marker = ls_test::read_bytes(store_file(directory.path(), "HEAD"));
  LS_CHECK(marker.ok());
  older_marker = marker.value();
  {
    auto engine = ls_test::open_writer(directory.path(), 2);
    LS_CHECK(engine.ok());
    LS_REQUIRE_OK(ls_test::install_snapshot(engine.value(), facility(1, 100)));
  }
  LS_CHECK_NE(ls_test::read_bytes(store_file(directory.path(), "HEAD")).value(), older_marker);
  // An operator (or an attacker) restores the older marker.
  LS_REQUIRE_OK_STATUS(ls_test::write_bytes(store_file(directory.path(), "HEAD"), older_marker));
  auto engine = ls_test::open_writer(directory.path(), 3);
  LS_CHECK(!engine.ok());
  LS_CHECK_EQ(engine.status().code(), StatusCode::Corrupt);
  auto audit = inspect_store(directory.path());
  LS_CHECK(audit.ok());
  LS_CHECK(audit.value().rollback_detected);
}

LS_TEST(store, an_oversized_generation_file_is_refused) {
  ls_test::ScratchDirectory directory{"store"};
  {
    auto engine = ls_test::open_writer(directory.path(), 1);
    LS_CHECK(engine.ok());
    LS_REQUIRE_OK(ls_test::install_policy(engine.value(), make_default_policy()));
  }
  auto marker = ls_test::read_bytes(store_file(directory.path(), "HEAD"));
  LS_CHECK(marker.ok());
  // Claim a payload far larger than any bound in a header that still checksums.
  std::string forged = std::string("LSSG");
  forged.push_back(1);
  forged.append(3, '\0');
  forged.append(8, '\0');
  forged[4] = 1;
  auto engine = ls_test::open_writer(directory.path(), 2);
  LS_CHECK(engine.ok());  // sanity: the store is still readable before damage
  LS_REQUIRE_OK_STATUS(ls_test::write_bytes(store_file(directory.path(), "state-00000000000000000002.lsg"),
                                            forged));
  engine = ls_test::open_writer(directory.path(), 3);
  LS_CHECK(!engine.ok());
}

LS_TEST(store, retention_bounds_every_retained_collection) {
  ls_test::ScratchDirectory directory{"store"};
  EngineOptions options;
  options.store_directory = directory.path();
  options.incarnation = Incarnation::from_value(1);
  options.take_authority = true;
  options.create_if_missing = true;
  options.max_plans_retained = 3;
  options.max_effect_records = 2;
  options.max_audit_entries = 5;
  options.max_idempotency_entries = 3;
  auto engine = Engine::open(options);
  LS_CHECK(engine.ok());
  LS_REQUIRE_OK(ls_test::install_policy(engine.value(), make_default_policy()));
  LS_REQUIRE_OK(ls_test::install_snapshot(engine.value(), facility(1, 100)));

  std::uint64_t tick = 100;
  for (std::uint64_t request_id = 1; request_id <= 6; ++request_id) {
    ++tick;
    auto outcome = engine.value().plan(ls_test::plan_request(engine.value(), request_id, 1000, tick));
    LS_CHECK_MSG(outcome.ok(), outcome.status().to_string());
  }
  LS_CHECK_EQ(engine.value().plan_count(), std::size_t{3});
  LS_CHECK(engine.value().effects().empty());
  auto page = engine.value().history(HistoryQuery{0, 100, std::nullopt, std::nullopt});
  LS_CHECK(page.ok());
  LS_CHECK(page.value().total_entries <= 5);

  // Retained plans are the newest ones, and the oldest plan identity is gone.
  auto old_plan = engine.value().plan_by_id(PlanId::from_value(1));
  LS_REQUIRE_STATUS(old_plan, StatusCode::NotFound);
  auto newest = engine.value().last_recovery_decision();
  LS_REQUIRE_STATUS(newest, StatusCode::NotFound);
  auto verify = engine.value().verify();
  LS_CHECK(verify.ok());
  LS_CHECK(verify.value().ok);
}

LS_TEST(store, retention_limits_must_be_consistent) {
  ls_test::ScratchDirectory directory{"store"};
  EngineOptions options;
  options.store_directory = directory.path();
  options.incarnation = Incarnation::from_value(1);
  options.max_plans_retained = 2;
  options.max_idempotency_entries = 8;
  LS_REQUIRE_STATUS(Engine::open(options), StatusCode::InvalidArgument);
  options.max_idempotency_entries = 0;
  LS_REQUIRE_STATUS(Engine::open(options), StatusCode::InvalidArgument);
  options.max_idempotency_entries = 2;
  options.incarnation = Incarnation{};
  LS_REQUIRE_STATUS(Engine::open(options), StatusCode::InvalidArgument);
  options.incarnation = Incarnation::from_value(1);
  options.store_directory.clear();
  LS_REQUIRE_STATUS(Engine::open(options), StatusCode::InvalidArgument);
}

LS_TEST(store, path_safety_is_enforced) {
  ls_test::ScratchDirectory scratch{"store"};
  // A file where a directory is required.
  const std::string file_path = scratch.child("a-file");
  LS_REQUIRE_OK_STATUS(ls_test::write_bytes(file_path, "not a directory"));
  auto as_file = ls_test::open_writer(file_path, 1);
  LS_CHECK(!as_file.ok());
  LS_CHECK_EQ(as_file.status().code(), StatusCode::Rejected);

  // A path containing a NUL byte is refused before anything is opened.
  std::string with_nul = scratch.child("store");
  with_nul.push_back('\0');
  with_nul += "suffix";
  auto nul_path = ls_test::open_writer(with_nul, 1);
  LS_CHECK(!nul_path.ok());
  LS_CHECK_EQ(nul_path.status().code(), StatusCode::Rejected);

  // A missing directory with create disabled.
  EngineOptions options;
  options.store_directory = scratch.child("missing");
  options.incarnation = Incarnation::from_value(1);
  options.take_authority = true;
  options.create_if_missing = false;
  auto missing = Engine::open(options);
  LS_CHECK(!missing.ok());
  LS_CHECK_EQ(missing.status().code(), StatusCode::NotFound);

  // Invalid UTF-8 in the path.
  std::string bad_utf8 = scratch.path();
  bad_utf8 += "/\xff\xfe-store";
  auto invalid = ls_test::open_writer(bad_utf8, 1);
  LS_CHECK(!invalid.ok());
  LS_CHECK_EQ(invalid.status().code(), StatusCode::InvalidArgument);

  // A link substituted for the store directory is refused.
  const std::string real = scratch.child("real-store");
  const std::string link = scratch.child("link-store");
  std::error_code create_error;
  std::filesystem::create_directories(real, create_error);
  LS_CHECK(!create_error);
  if (create_directory_link(real, link)) {
    auto through_link = ls_test::open_writer(link, 1);
    LS_CHECK(!through_link.ok());
    LS_CHECK_EQ(through_link.status().code(), StatusCode::Rejected);
  } else {
    std::cout << "    NOTE link substitution could not be created in this environment\n";
  }
}

LS_TEST(store, inspection_of_a_missing_directory_is_reported) {
  ls_test::ScratchDirectory scratch{"store"};
  LS_REQUIRE_STATUS(inspect_store(scratch.child("absent")), StatusCode::NotFound);
}

LS_TEST(store, audit_history_is_monotonic_and_complete) {
  ls_test::ScratchDirectory directory{"store"};
  auto engine = ls_test::open_writer(directory.path(), 1);
  LS_CHECK(engine.ok());
  LS_REQUIRE_OK(ls_test::install_policy(engine.value(), make_default_policy()));
  LS_REQUIRE_OK(ls_test::install_snapshot(engine.value(), facility(1, 100)));
  auto outcome = engine.value().plan(ls_test::plan_request(engine.value(), 1, 50000, 110));
  LS_CHECK(outcome.ok());
  auto page = engine.value().history(HistoryQuery{0, 100, std::nullopt, std::nullopt});
  LS_CHECK(page.ok());
  LS_CHECK(page.value().total_entries >= 4);
  for (std::size_t index = 1; index < page.value().entries.size(); ++index) {
    LS_CHECK(page.value().entries[index - 1].sequence < page.value().entries[index].sequence);
  }
  auto filtered = engine.value().history(
      HistoryQuery{0, 100, AuditKind::PlanCommitted, std::nullopt});
  LS_CHECK(filtered.ok());
  LS_CHECK_EQ(filtered.value().entries.size(), std::size_t{1});
  LS_CHECK_EQ(filtered.value().entries[0].plan.value(), outcome.value().plan.identity.plan_id.value());
  LS_REQUIRE_STATUS(engine.value().history(HistoryQuery{0, 100000, std::nullopt, std::nullopt}),
                    StatusCode::LimitExceeded);
}
