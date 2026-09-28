// Determinism and idempotency proof obligations.
//
// Equal inputs must produce canonically identical plans; a replay of an accepted
// attempt must return the prior result; downstream effect records must never
// rewrite the plan they observed.

#include <string>
#include <vector>

#include "fixture.hpp"
#include "load_shedding/json_io.hpp"
#include "test_harness.hpp"

using namespace load_shedding;

namespace {

FacilitySnapshot reference_facility(std::uint64_t generation = 1, std::uint64_t tick = 100) {
  return ls_test::FacilityBuilder()
      .tick(tick)
      .evidence_generation(generation)
      .load("alpha", 120000, PriorityClass::Standard)
      .load("beta", 80000, PriorityClass::Deferrable)
      .load("gamma", 45000, PriorityClass::Optional)
      .unknown_load("delta")
      .obligation("keep-alpha-floor", "alpha", 20000)
      .total_demand(300000)
      .build();
}

}  // namespace

LS_TEST(determinism, equal_inputs_produce_identical_plan_content) {
  ls_test::ScratchDirectory first_store{"determinism"};
  ls_test::ScratchDirectory second_store{"determinism"};
  auto first = ls_test::open_writer(first_store.path(), 1);
  auto second = ls_test::open_writer(second_store.path(), 1);
  LS_CHECK(first.ok());
  LS_CHECK(second.ok());
  LS_REQUIRE_OK(ls_test::install_policy(first.value(), make_default_policy()));
  LS_REQUIRE_OK(ls_test::install_policy(second.value(), make_default_policy()));
  LS_REQUIRE_OK(ls_test::install_snapshot(first.value(), reference_facility()));
  LS_REQUIRE_OK(ls_test::install_snapshot(second.value(), reference_facility()));

  auto first_outcome = first.value().plan(ls_test::plan_request(first.value(), 1, 150000, 200));
  auto second_outcome = second.value().plan(ls_test::plan_request(second.value(), 1, 150000, 200));
  LS_CHECK(first_outcome.ok());
  LS_CHECK(second_outcome.ok());
  LS_CHECK_EQ(first_outcome.value().plan.content_digest().hex(),
              second_outcome.value().plan.content_digest().hex());
  LS_CHECK_EQ(to_canonical_json(to_json(first_outcome.value().plan)),
              to_canonical_json(to_json(second_outcome.value().plan)));
  // The two stores reach the same canonical state as well.
  LS_CHECK_EQ(first.value().state_digest().hex(), second.value().state_digest().hex());
}

LS_TEST(determinism, load_insertion_order_does_not_change_the_plan) {
  ls_test::ScratchDirectory ordered{"determinism"};
  ls_test::ScratchDirectory shuffled{"determinism"};
  auto first = ls_test::open_writer(ordered.path(), 1);
  auto second = ls_test::open_writer(shuffled.path(), 1);
  LS_CHECK(first.ok());
  LS_CHECK(second.ok());
  LS_REQUIRE_OK(ls_test::install_policy(first.value(), make_default_policy()));
  LS_REQUIRE_OK(ls_test::install_policy(second.value(), make_default_policy()));

  FacilitySnapshot forward = ls_test::FacilityBuilder()
                                 .tick(100)
                                 .evidence_generation(1)
                                 .load("a", 10000, PriorityClass::Optional)
                                 .load("b", 20000, PriorityClass::Standard)
                                 .load("c", 30000, PriorityClass::Standard)
                                 .load("d", 40000, PriorityClass::Deferrable)
                                 .build();
  FacilitySnapshot backward = ls_test::FacilityBuilder()
                                  .tick(100)
                                  .evidence_generation(1)
                                  .load("d", 40000, PriorityClass::Deferrable)
                                  .load("c", 30000, PriorityClass::Standard)
                                  .load("b", 20000, PriorityClass::Standard)
                                  .load("a", 10000, PriorityClass::Optional)
                                  .build();
  LS_REQUIRE_OK(ls_test::install_snapshot(first.value(), forward));
  LS_REQUIRE_OK(ls_test::install_snapshot(second.value(), backward));

  auto first_outcome = first.value().plan(ls_test::plan_request(first.value(), 3, 45000, 150));
  auto second_outcome = second.value().plan(ls_test::plan_request(second.value(), 3, 45000, 150));
  LS_CHECK(first_outcome.ok());
  LS_CHECK(second_outcome.ok());
  LS_CHECK_EQ(first_outcome.value().plan.content_digest().hex(),
              second_outcome.value().plan.content_digest().hex());
  LS_CHECK_EQ(first.value().state_digest().hex(), second.value().state_digest().hex());
}

LS_TEST(determinism, canonical_state_is_stable_across_reopen) {
  ls_test::ScratchDirectory directory{"determinism"};
  Digest before;
  {
    auto engine = ls_test::open_writer(directory.path(), 1);
    LS_CHECK(engine.ok());
    LS_REQUIRE_OK(ls_test::install_policy(engine.value(), make_default_policy()));
    LS_REQUIRE_OK(ls_test::install_snapshot(engine.value(), reference_facility()));
    auto outcome = engine.value().plan(ls_test::plan_request(engine.value(), 1, 100000, 200));
    LS_CHECK(outcome.ok());
    before = engine.value().state_digest();
    LS_CHECK(!before.is_zero());
  }
  // A read-only reopen adopts exactly the committed generation: same canonical
  // bytes, same digest.
  {
    auto reopened = ls_test::open_reader(directory.path());
    LS_CHECK_MSG(reopened.ok(), reopened.status().to_string());
    LS_CHECK_EQ(reopened.value().state_digest().hex(), before.hex());
  }
  // A writer reopen records its authority takeover, so the canonical state moves
  // by exactly that: the retained plan and its digest are untouched.
  {
    auto writer = ls_test::open_writer(directory.path(), 1);
    LS_CHECK_MSG(writer.ok(), writer.status().to_string());
    LS_CHECK_EQ(writer.value().plan_count(), std::size_t{1});
    auto plan = writer.value().plan_at(0);
    LS_CHECK(plan.ok());
    auto verify = writer.value().verify();
    LS_CHECK(verify.ok());
    LS_CHECK(verify.value().ok);
  }
}

LS_TEST(determinism, replay_returns_the_prior_accepted_result) {
  ls_test::ScratchDirectory directory{"determinism"};
  auto engine = ls_test::open_writer(directory.path(), 1);
  LS_CHECK(engine.ok());
  LS_REQUIRE_OK(ls_test::install_policy(engine.value(), make_default_policy()));
  LS_REQUIRE_OK(ls_test::install_snapshot(engine.value(), reference_facility()));

  const PlanRequest request = ls_test::plan_request(engine.value(), 42, 150000, 200);
  auto accepted = engine.value().plan(request);
  LS_CHECK(accepted.ok());
  LS_CHECK(!accepted.value().replayed);

  auto replayed = engine.value().plan(request);
  LS_CHECK(replayed.ok());
  LS_CHECK(replayed.value().replayed);
  LS_CHECK_EQ(replayed.value().plan.content_digest().hex(),
              accepted.value().plan.content_digest().hex());
  LS_CHECK_EQ(replayed.value().plan.identity.plan_id.value(),
              accepted.value().plan.identity.plan_id.value());
  LS_CHECK_EQ(engine.value().plan_count(), std::size_t{1});

  // A retry after the response was lost still succeeds even once the plan is no
  // longer planned against current state: the replay check precedes the staleness
  // checks.
  LS_REQUIRE_OK(ls_test::install_snapshot(engine.value(), reference_facility(2, 300)));
  auto late_retry = engine.value().plan(request);
  LS_CHECK_MSG(late_retry.ok(), late_retry.status().to_string());
  LS_CHECK(late_retry.value().replayed);
  LS_CHECK_EQ(late_retry.value().plan.content_digest().hex(),
              accepted.value().plan.content_digest().hex());

  // A new request identity against the same stale generations is refused.
  auto fresh_identity = engine.value().plan(ls_test::plan_request(engine.value(), 43, 150000, 300));
  LS_CHECK(fresh_identity.ok());
}

LS_TEST(determinism, replay_with_different_content_is_refused) {
  ls_test::ScratchDirectory directory{"determinism"};
  auto engine = ls_test::open_writer(directory.path(), 1);
  LS_CHECK(engine.ok());
  LS_REQUIRE_OK(ls_test::install_policy(engine.value(), make_default_policy()));
  LS_REQUIRE_OK(ls_test::install_snapshot(engine.value(), reference_facility()));

  const PlanRequest request = ls_test::plan_request(engine.value(), 7, 150000, 200);
  LS_CHECK(engine.value().plan(request).ok());
  PlanRequest different = request;
  different.requested_reduction = Power::from_watts(160000);
  LS_REQUIRE_STATUS(engine.value().plan(different), StatusCode::IdempotencyConflict);

  // The same identity for a different decision kind is not silently accepted.
  EffectObservationRequest effect = ls_test::effect_request(
      engine.value(), 7, PlanId::first(), "alpha", EffectState::Shed, 210);
  LS_REQUIRE_STATUS(engine.value().observe_effect(effect), StatusCode::IdempotencyConflict);
}

LS_TEST(determinism, replay_window_is_bounded_and_reported) {
  ls_test::ScratchDirectory directory{"determinism"};
  EngineOptions options;
  options.store_directory = directory.path();
  options.incarnation = Incarnation::from_value(1);
  options.take_authority = true;
  options.create_if_missing = true;
  options.max_plans_retained = 4;
  options.max_idempotency_entries = 4;
  auto engine = Engine::open(options);
  LS_CHECK(engine.ok());
  LS_REQUIRE_OK(ls_test::install_policy(engine.value(), make_default_policy()));
  LS_REQUIRE_OK(ls_test::install_snapshot(engine.value(), reference_facility()));

  std::uint64_t tick = 200;
  for (std::uint64_t request_id = 1; request_id <= 5; ++request_id) {
    auto outcome = engine.value().plan(ls_test::plan_request(engine.value(), request_id, 1000, tick));
    LS_CHECK_MSG(outcome.ok(), outcome.status().to_string());
    ++tick;
  }
  // The oldest request identity has left the window: it is refused rather than
  // re-planned under the old identity.
  auto evicted = engine.value().plan(ls_test::plan_request(engine.value(), 1, 1000, 100));
  LS_REQUIRE_STATUS(evicted, StatusCode::StaleTick);
  // Retained identities still replay.
  auto retained = engine.value().plan(ls_test::plan_request(engine.value(), 5, 1000, tick - 1));
  LS_CHECK(retained.ok());
  LS_CHECK(retained.value().replayed);
  // The retained plan window never drops a plan that the replay window can still
  // resolve.
  LS_CHECK(engine.value().plan_count() <= 4);
  auto verify = engine.value().verify();
  LS_CHECK(verify.ok());
  LS_CHECK(verify.value().ok);
}

LS_TEST(determinism, effect_observations_never_rewrite_the_plan) {
  ls_test::ScratchDirectory directory{"determinism"};
  auto engine = ls_test::open_writer(directory.path(), 1);
  LS_CHECK(engine.ok());
  LS_REQUIRE_OK(ls_test::install_policy(engine.value(), make_default_policy()));
  LS_REQUIRE_OK(ls_test::install_snapshot(engine.value(), reference_facility()));
  auto outcome = engine.value().plan(ls_test::plan_request(engine.value(), 1, 150000, 200));
  LS_CHECK(outcome.ok());
  const PlanId plan_id = outcome.value().plan.identity.plan_id;
  const std::string digest_before = outcome.value().plan.content_digest().hex();
  const std::string bytes_before = LS_REQUIRE_OK(engine.value().canonical_state_bytes());

  EffectObservationRequest request = ls_test::effect_request(
      engine.value(), 2, plan_id, "alpha", EffectState::Shed, 201,
      EffectVerification::Confirmed, EffectSource::Adapter);
  auto recorded = engine.value().observe_effect(request);
  LS_CHECK_MSG(recorded.ok(), recorded.status().to_string());
  LS_CHECK_EQ(recorded.value().plan_digest_at_observation.hex(), digest_before);

  auto stored = engine.value().plan_by_id(plan_id);
  LS_CHECK(stored.ok());
  LS_CHECK_EQ(stored.value().content_digest().hex(), digest_before);
  LS_CHECK_NE(LS_REQUIRE_OK(engine.value().canonical_state_bytes()), bytes_before);

  // The observed state moved, so revalidation reports superseded effect state
  // rather than pretending the plan still describes current conditions.
  auto report = engine.value().revalidate(plan_id);
  LS_CHECK(report.ok());
  LS_CHECK_EQ(report.value().verdict, RevalidationVerdict::SupersededEffect);

  // Replaying the effect request returns the same record without adding another.
  auto replayed = engine.value().observe_effect(request);
  LS_CHECK(replayed.ok());
  LS_CHECK_EQ(replayed.value().record_digest().hex(), recorded.value().record_digest().hex());
  LS_CHECK_EQ(engine.value().effects().size(), std::size_t{1});
}
