// Concurrency proof obligations.
//
// The engine is safe to call from several threads: methods are serialized
// internally, accessors return snapshot values rather than references into
// mutable state, and no callback or external process runs under the state lock.
// These tests prove the structure by running it, with a fixed worker count and no
// timing assumptions.

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
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
      .load("load-b", 40000, PriorityClass::Optional)
      .load("load-c", 60000, PriorityClass::Standard)
      .total_demand(220000)
      .build();
}

}  // namespace

LS_TEST(concurrency, readers_and_a_writer_share_one_engine) {
  ls_test::ScratchDirectory directory{"concurrency"};
  auto engine = ls_test::open_writer(directory.path(), 1);
  LS_CHECK(engine.ok());
  LS_REQUIRE_OK(ls_test::install_policy(engine.value(), make_default_policy()));
  LS_REQUIRE_OK(ls_test::install_snapshot(engine.value(), facility(1, 100)));

  constexpr int kReaderCount = 4;
  constexpr int kIterations = 25;
  constexpr std::uint64_t kPlanCount = 12;
  std::atomic<int> reader_failures{0};
  std::atomic<std::uint64_t> reads_completed{0};
  std::atomic<bool> start{false};

  std::vector<std::thread> readers;
  for (int index = 0; index < kReaderCount; ++index) {
    readers.emplace_back([&engine, &reader_failures, &reads_completed, &start]() {
      while (!start.load()) {
        std::this_thread::yield();
      }
      for (int iteration = 0; iteration < kIterations; ++iteration) {
        auto report = engine.value().verify();
        if (!report.ok() || !report.value().ok) {
          ++reader_failures;
        }
        const std::size_t plans = engine.value().plan_count();
        for (std::size_t position = 0; position < plans; ++position) {
          auto plan = engine.value().plan_at(position);
          if (!plan.ok()) {
            ++reader_failures;
            continue;
          }
          if (!plan.value().verify().ok()) {
            ++reader_failures;
          }
          if (plan.value().closure.selected_expected_reduction >
              plan.value().closure.eligible_known_capacity) {
            ++reader_failures;
          }
        }
        auto page = engine.value().history(HistoryQuery{0, 16, std::nullopt, std::nullopt});
        if (!page.ok()) {
          ++reader_failures;
        }
        const FacilitySnapshot snapshot = engine.value().snapshot();
        if (snapshot.loads.size() != 3) {
          ++reader_failures;
        }
        reads_completed.fetch_add(1);
      }
    });
  }

  std::uint64_t tick = 100;
  std::uint64_t request_id = 0;
  for (std::uint64_t plan_index = 0; plan_index < kPlanCount; ++plan_index) {
    ++tick;
    ++request_id;
    start.store(true);
    auto outcome = engine.value().plan(ls_test::plan_request(engine.value(), request_id, 90000, tick));
    if (!outcome.ok()) {
      ++reader_failures;
    }
  }
  start.store(true);
  for (std::thread& reader : readers) {
    reader.join();
  }
  LS_CHECK_EQ(reader_failures.load(), 0);
  LS_CHECK_EQ(reads_completed.load(), static_cast<std::uint64_t>(kReaderCount * kIterations));
  auto report = engine.value().verify();
  LS_CHECK(report.ok());
  LS_CHECK(report.value().ok);
  LS_CHECK_EQ(engine.value().plan_count(), static_cast<std::size_t>(kPlanCount));
}

LS_TEST(concurrency, two_engines_in_one_process_exclude_each_other) {
  ls_test::ScratchDirectory directory{"concurrency"};
  auto writer = ls_test::open_writer(directory.path(), 1);
  LS_CHECK(writer.ok());
  auto second_writer = ls_test::open_writer(directory.path(), 2);
  LS_REQUIRE_STATUS(second_writer, StatusCode::LockConflict);
  auto reader = ls_test::open_reader(directory.path());
  LS_REQUIRE_STATUS(reader, StatusCode::LockConflict);
  LS_REQUIRE_OK_STATUS(writer.value().close());
  {
    auto reopened_reader = ls_test::open_reader(directory.path());
    LS_CHECK_MSG(reopened_reader.ok(), reopened_reader.status().to_string());
    auto blocked_writer = ls_test::open_writer(directory.path(), 3);
    LS_REQUIRE_STATUS(blocked_writer, StatusCode::LockConflict);
  }
  auto after_release = ls_test::open_writer(directory.path(), 4);
  LS_CHECK_MSG(after_release.ok(), after_release.status().to_string());
}

LS_TEST(concurrency, accessors_return_values_not_references) {
  ls_test::ScratchDirectory directory{"concurrency"};
  auto engine = ls_test::open_writer(directory.path(), 1);
  LS_CHECK(engine.ok());
  LS_REQUIRE_OK(ls_test::install_policy(engine.value(), make_default_policy()));
  LS_REQUIRE_OK(ls_test::install_snapshot(engine.value(), facility(1, 100)));
  const FacilitySnapshot before = engine.value().snapshot();
  const SheddingPolicy policy_before = engine.value().policy();
  LS_CHECK_EQ(before.loads.size(), std::size_t{3});

  LS_REQUIRE_OK(ls_test::install_snapshot(engine.value(), facility(2, 200)));
  const FacilitySnapshot after = engine.value().snapshot();
  LS_CHECK_EQ(after.loads.size(), std::size_t{3});
  // The earlier value is untouched by the later mutation.
  LS_CHECK_EQ(before.generation.value(), std::uint64_t{1});
  LS_CHECK_EQ(before.loads[0].contribution.tick.value(), std::uint64_t{100});
  LS_CHECK_EQ(after.generation.value(), std::uint64_t{2});
  LS_CHECK_EQ(policy_before.content_digest().hex(), engine.value().policy().content_digest().hex());
}

LS_TEST(concurrency, independent_engines_do_not_share_state) {
  ls_test::ScratchDirectory first_directory{"concurrency"};
  ls_test::ScratchDirectory second_directory{"concurrency"};
  auto first = ls_test::open_writer(first_directory.path(), 1);
  auto second = ls_test::open_writer(second_directory.path(), 1);
  LS_CHECK(first.ok());
  LS_CHECK(second.ok());
  LS_REQUIRE_OK(ls_test::install_policy(first.value(), make_default_policy()));
  LS_REQUIRE_OK(ls_test::install_policy(second.value(), make_default_policy()));
  LS_REQUIRE_OK(ls_test::install_snapshot(first.value(), facility(1, 100)));
  LS_REQUIRE_OK(ls_test::install_snapshot(second.value(), facility(1, 100)));

  auto first_outcome = first.value().plan(ls_test::plan_request(first.value(), 1, 50000, 150));
  LS_CHECK(first_outcome.ok());
  LS_CHECK_EQ(second.value().plan_count(), std::size_t{0});
  LS_CHECK_EQ(second.value().revision().value(), first.value().revision().value());
  LS_CHECK_NE(first.value().state_digest().hex(), std::string());
}
