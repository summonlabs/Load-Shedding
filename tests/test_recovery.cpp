// Recovery and downstream-effect proof obligations.
//
// Recovery is a separate decision from shedding. It depends on observed effect
// evidence, follows a documented deterministic order, is bounded by available
// headroom, and never rewrites the plan it is based on.

#include <string>
#include <vector>

#include "fixture.hpp"
#include "load_shedding/json_io.hpp"
#include "test_harness.hpp"

using namespace load_shedding;

namespace {

struct Rig {
  ls_test::ScratchDirectory directory{"recovery"};
  Engine engine;
  PlanId plan_id;
  Digest plan_digest;

  Rig(const FacilitySnapshot& snapshot, SheddingPolicy policy, std::int64_t deficit) {
    auto opened = ls_test::open_writer(directory.path());
    LS_CHECK_MSG(opened.ok(), opened.status().to_string());
    engine = std::move(opened.value());
    LS_REQUIRE_OK(ls_test::install_policy(engine, policy));
    LS_REQUIRE_OK(ls_test::install_snapshot(engine, snapshot));
    auto outcome = engine.plan(ls_test::plan_request(engine, 1, deficit, 200));
    LS_CHECK_MSG(outcome.ok(), outcome.status().to_string());
    plan_id = outcome.value().plan.identity.plan_id;
    plan_digest = outcome.value().plan.content_digest();
  }

  void observe(const std::string& ref, EffectState state, std::uint64_t tick,
               EffectVerification verification = EffectVerification::Confirmed,
               std::uint64_t request_id = 100) {
    auto request = ls_test::effect_request(engine, request_id, plan_id, ref, state, tick, verification);
    auto recorded = engine.observe_effect(request);
    LS_CHECK_MSG(recorded.ok(), recorded.status().to_string());
  }

  Result<RecoveryDecision> recover(std::int64_t headroom, std::uint64_t request_id, std::uint64_t tick) {
    return engine.recovery(ls_test::recovery_request(engine, request_id, plan_id, headroom, tick));
  }
};

FacilitySnapshot three_load_facility() {
  return ls_test::FacilityBuilder()
      .tick(100)
      .evidence_generation(1)
      .load("optional-a", 30000, PriorityClass::Optional)
      .load("standard-b", 50000, PriorityClass::Standard)
      .load("standard-c", 70000, PriorityClass::Standard)
      .build();
}

std::vector<std::string> ordered_refs(const RecoveryDecision& decision) {
  std::vector<std::string> refs;
  for (const RecoveryCandidate& candidate : decision.candidates) {
    if (candidate.order_index_set) {
      refs.push_back(candidate.load.value());
    }
  }
  return refs;
}

RecoveryVerdict verdict_of(const RecoveryDecision& decision, const std::string& ref) {
  for (const RecoveryCandidate& candidate : decision.candidates) {
    if (candidate.load.value() == ref) {
      return candidate.verdict;
    }
  }
  LS_CHECK_MSG(false, "no recovery candidate for load '" + ref + "'");
  return RecoveryVerdict::NotShedByPlan;
}

}  // namespace

LS_TEST(recovery, shedding_plan_for_the_recovery_fixture) {
  Rig rig(three_load_facility(), make_default_policy(), 150000);
  auto plan = rig.engine.plan_by_id(rig.plan_id);
  LS_CHECK(plan.ok());
  LS_CHECK_EQ(plan.value().actions.size(), std::size_t{3});
  LS_CHECK_EQ(plan.value().actions[0].load.value(), std::string("optional-a"));
  LS_CHECK_EQ(plan.value().actions[1].load.value(), std::string("standard-c"));
  LS_CHECK_EQ(plan.value().actions[2].load.value(), std::string("standard-b"));
}

LS_TEST(recovery, restoration_requires_confirmed_shed_evidence) {
  Rig rig(three_load_facility(), make_default_policy(), 150000);
  // Nothing observed yet: nothing can be restored.
  auto none = rig.recover(500000, 10, 210);
  LS_CHECK(none.ok());
  LS_CHECK_EQ(none.value().restored_count, std::uint64_t{0});
  LS_CHECK_EQ(ordered_refs(none.value()).size(), std::size_t{3});
  LS_CHECK_EQ(verdict_of(none.value(), "standard-c"), RecoveryVerdict::UnknownObservedState);
  LS_CHECK_EQ(none.value().awaiting_confirmation_count, std::uint64_t{0});

  // An acknowledgement is not an effect.
  rig.observe("standard-c", EffectState::Shed, 211, EffectVerification::Acknowledged, 101);
  auto acknowledged = rig.recover(500000, 11, 212);
  LS_CHECK(acknowledged.ok());
  LS_CHECK_EQ(verdict_of(acknowledged.value(), "standard-c"),
              RecoveryVerdict::AwaitingEffectConfirmation);
  LS_CHECK_EQ(acknowledged.value().restored_count, std::uint64_t{0});

  // Confirmed evidence makes it a candidate.
  rig.observe("standard-c", EffectState::Shed, 213, EffectVerification::Confirmed, 102);
  auto confirmed = rig.recover(500000, 12, 214);
  LS_CHECK(confirmed.ok());
  LS_CHECK_EQ(verdict_of(confirmed.value(), "standard-c"), RecoveryVerdict::Restored);

  // Contradicted evidence is reported rather than silently ignored.
  rig.observe("standard-b", EffectState::Energized, 215, EffectVerification::Confirmed, 103);
  auto contradicted = rig.recover(500000, 13, 216);
  LS_CHECK(contradicted.ok());
  LS_CHECK_EQ(verdict_of(contradicted.value(), "standard-b"), RecoveryVerdict::EvidenceContradicted);
}

LS_TEST(recovery, documented_order_is_reverse_stage_then_priority) {
  Rig rig(three_load_facility(), make_default_policy(), 150000);
  rig.observe("optional-a", EffectState::Shed, 201, EffectVerification::Confirmed, 100);
  rig.observe("standard-b", EffectState::Shed, 202, EffectVerification::Confirmed, 101);
  rig.observe("standard-c", EffectState::Shed, 203, EffectVerification::Confirmed, 102);
  auto decision = rig.recover(500000, 10, 210);
  LS_CHECK(decision.ok());
  // Later stage first (standard is stage 1, optional is stage 0); inside the
  // stage the larger contribution comes first.
  const std::vector<std::string> expected = {"standard-c", "standard-b", "optional-a"};
  LS_CHECK_EQ(ordered_refs(decision.value()), expected);
  LS_CHECK_EQ(decision.value().order, RecoveryOrder::ReverseStageThenPriority);
  LS_CHECK(decision.value().verify().ok());
}

LS_TEST(recovery, priority_first_order_differs_when_stages_are_out_of_priority_order) {
  SheddingPolicy policy = make_default_policy();
  policy.stages[0].priorities = {PriorityClass::Standard};
  policy.stages[1].priorities = {PriorityClass::Optional};
  policy.stages.pop_back();
  policy.recovery_order = RecoveryOrder::PriorityThenReverseStage;
  Rig rig(three_load_facility(), policy, 150000);
  rig.observe("optional-a", EffectState::Shed, 201, EffectVerification::Confirmed, 100);
  rig.observe("standard-b", EffectState::Shed, 202, EffectVerification::Confirmed, 101);
  rig.observe("standard-c", EffectState::Shed, 203, EffectVerification::Confirmed, 102);
  auto decision = rig.recover(500000, 10, 210);
  LS_CHECK(decision.ok());
  // Standard is stage 0 here but Optional is stage 1: priority-first restores the
  // more important class first regardless of the stage that shed it.
  const std::vector<std::string> expected = {"standard-c", "standard-b", "optional-a"};
  LS_CHECK_EQ(ordered_refs(decision.value()), expected);
  LS_CHECK_EQ(decision.value().order, RecoveryOrder::PriorityThenReverseStage);

  SheddingPolicy reverse = policy;
  reverse.recovery_order = RecoveryOrder::ReverseStageThenPriority;
  ls_test::ScratchDirectory other{"recovery"};
  auto engine = ls_test::open_writer(other.path());
  LS_CHECK(engine.ok());
  LS_REQUIRE_OK(ls_test::install_policy(engine.value(), reverse));
  LS_REQUIRE_OK(ls_test::install_snapshot(engine.value(), three_load_facility()));
  auto outcome = engine.value().plan(ls_test::plan_request(engine.value(), 1, 150000, 200));
  LS_CHECK(outcome.ok());
  // Stage 0 covers Standard here, so shedding takes the standard loads first,
  // while reverse-stage recovery restores the optional load first: the two orders
  // are genuinely different rules.
  const std::vector<std::string> expected_selection = {"standard-c", "standard-b", "optional-a"};
  std::vector<std::string> selected;
  for (const ShedAction& action : outcome.value().plan.actions) {
    selected.push_back(action.load.value());
  }
  LS_CHECK_EQ(selected, expected_selection);

  auto effect = [&](const std::string& ref, std::uint64_t tick, std::uint64_t request_id) {
    auto request = ls_test::effect_request(engine.value(), request_id,
                                           outcome.value().plan.identity.plan_id, ref,
                                           EffectState::Shed, tick, EffectVerification::Confirmed);
    LS_REQUIRE_OK(engine.value().observe_effect(request));
  };
  effect("optional-a", 201, 300);
  effect("standard-b", 202, 301);
  effect("standard-c", 203, 302);
  auto reverse_decision = engine.value().recovery(ls_test::recovery_request(
      engine.value(), 400, outcome.value().plan.identity.plan_id, 500000, 210));
  LS_CHECK(reverse_decision.ok());
  const std::vector<std::string> expected_reverse = {"optional-a", "standard-c", "standard-b"};
  std::vector<std::string> reverse_refs;
  for (const RecoveryCandidate& candidate : reverse_decision.value().candidates) {
    if (candidate.order_index_set) {
      reverse_refs.push_back(candidate.load.value());
    }
  }
  LS_CHECK_EQ(reverse_refs, expected_reverse);
}

LS_TEST(recovery, headroom_bounds_restoration_and_is_accounted) {
  Rig rig(three_load_facility(), make_default_policy(), 150000);
  rig.observe("optional-a", EffectState::Shed, 201, EffectVerification::Confirmed, 100);
  rig.observe("standard-b", EffectState::Shed, 202, EffectVerification::Confirmed, 101);
  rig.observe("standard-c", EffectState::Shed, 203, EffectVerification::Confirmed, 102);
  auto decision = rig.recover(100000, 10, 210);
  LS_CHECK(decision.ok());
  LS_CHECK_EQ(verdict_of(decision.value(), "standard-c"), RecoveryVerdict::Restored);
  LS_CHECK_EQ(verdict_of(decision.value(), "standard-b"), RecoveryVerdict::SkippedHeadroom);
  LS_CHECK_EQ(verdict_of(decision.value(), "optional-a"), RecoveryVerdict::Restored);
  LS_CHECK_EQ(decision.value().restored_expected.watts(), std::int64_t{100000});
  LS_CHECK_EQ(decision.value().remaining_headroom.watts(), std::int64_t{0});
  LS_CHECK_EQ(decision.value().restored_count, std::uint64_t{2});
  LS_CHECK_EQ(decision.value().candidate_count, std::uint64_t{3});
  LS_CHECK(decision.value().verify().ok());

  auto zero = rig.recover(0, 11, 211);
  LS_CHECK(zero.ok());
  LS_CHECK_EQ(zero.value().restored_count, std::uint64_t{0});
  LS_CHECK_EQ(zero.value().remaining_headroom.watts(), std::int64_t{0});
}

LS_TEST(recovery, minimum_off_time_is_respected) {
  SheddingPolicy policy = make_default_policy();
  policy.min_off_ticks = 50;
  Rig rig(three_load_facility(), policy, 150000);
  rig.observe("standard-c", EffectState::Shed, 210, EffectVerification::Confirmed, 100);
  auto too_soon = rig.recover(500000, 10, 220);
  LS_CHECK(too_soon.ok());
  LS_CHECK_EQ(verdict_of(too_soon.value(), "standard-c"), RecoveryVerdict::MinOffNotElapsed);
  LS_CHECK_EQ(too_soon.value().restored_count, std::uint64_t{0});
  auto allowed = rig.recover(500000, 11, 260);
  LS_CHECK(allowed.ok());
  LS_CHECK_EQ(verdict_of(allowed.value(), "standard-c"), RecoveryVerdict::Restored);
}

LS_TEST(recovery, recovery_never_rewrites_the_plan) {
  Rig rig(three_load_facility(), make_default_policy(), 150000);
  rig.observe("standard-c", EffectState::Shed, 201, EffectVerification::Confirmed, 100);
  auto decision = rig.recover(500000, 10, 210);
  LS_CHECK(decision.ok());
  LS_CHECK_EQ(decision.value().source_plan_digest.hex(), rig.plan_digest.hex());
  auto plan = rig.engine.plan_by_id(rig.plan_id);
  LS_CHECK(plan.ok());
  LS_CHECK_EQ(plan.value().content_digest().hex(), rig.plan_digest.hex());
  // Replaying the recovery request returns the same decision and adds nothing.
  auto replayed = rig.engine.recovery(
      ls_test::recovery_request(rig.engine, 10, rig.plan_id, 500000, 210));
  LS_CHECK(replayed.ok());
  LS_CHECK_EQ(replayed.value().content_digest().hex(), decision.value().content_digest().hex());
  LS_CHECK_EQ(rig.engine.recovery_decisions().size(), std::size_t{1});
}

LS_TEST(recovery, stale_and_unknown_references_are_refused) {
  Rig rig(three_load_facility(), make_default_policy(), 150000);
  RecoveryRequest request = ls_test::recovery_request(rig.engine, 10, rig.plan_id, 1000, 210);
  request.plan_id = PlanId::from_value(999);
  LS_REQUIRE_STATUS(rig.engine.recovery(request), StatusCode::NotFound);

  request = ls_test::recovery_request(rig.engine, 11, rig.plan_id, 1000, 210);
  request.plan_generation = PlanGeneration::from_value(99);
  LS_REQUIRE_STATUS(rig.engine.recovery(request), StatusCode::StalePlan);

  request = ls_test::recovery_request(rig.engine, 12, rig.plan_id, 1000, 210);
  request.available_headroom = Power::from_watts(-1);
  LS_REQUIRE_STATUS(rig.engine.recovery(request), StatusCode::InvalidArgument);

  request = ls_test::recovery_request(rig.engine, 13, rig.plan_id, 1000, 210);
  request.effect_generation = EffectGeneration::from_value(9);
  LS_REQUIRE_STATUS(rig.engine.recovery(request), StatusCode::StaleEffectGeneration);
}

LS_TEST(recovery, effect_records_are_validated_and_ordered) {
  Rig rig(three_load_facility(), make_default_policy(), 150000);
  // An unknown load cannot be observed.
  auto unknown_load = ls_test::effect_request(rig.engine, 200, rig.plan_id, "not-a-load",
                                              EffectState::Shed, 205);
  LS_REQUIRE_STATUS(rig.engine.observe_effect(unknown_load), StatusCode::NotFound);
  // An unknown plan cannot be observed.
  auto unknown_plan = ls_test::effect_request(rig.engine, 201, PlanId::from_value(999), "standard-b",
                                              EffectState::Shed, 205);
  LS_REQUIRE_STATUS(rig.engine.observe_effect(unknown_plan), StatusCode::NotFound);
  // A plan generation mismatch is refused.
  auto wrong_generation = ls_test::effect_request(rig.engine, 202, rig.plan_id, "standard-b",
                                                  EffectState::Shed, 205);
  wrong_generation.plan_generation = PlanGeneration::from_value(99);
  LS_REQUIRE_STATUS(rig.engine.observe_effect(wrong_generation), StatusCode::StalePlan);

  rig.observe("standard-b", EffectState::Shed, 205, EffectVerification::Confirmed, 203);
  // A reordered observation is refused rather than rolling the state back.
  auto reordered = ls_test::effect_request(rig.engine, 204, rig.plan_id, "standard-b",
                                           EffectState::Energized, 200);
  LS_REQUIRE_STATUS(rig.engine.observe_effect(reordered), StatusCode::StaleTick);

  auto observed = rig.engine.observed_state(LoadRef::parse("standard-b").value());
  LS_CHECK(observed.ok());
  LS_CHECK_EQ(observed.value().state, EffectState::Shed);
  LS_CHECK_EQ(observed.value().verification, EffectVerification::Confirmed);
  LS_CHECK_EQ(observed.value().since.value(), std::uint64_t{205});

  auto missing = rig.engine.observed_state(LoadRef::parse("optional-a").value());
  LS_REQUIRE_STATUS(missing, StatusCode::NotFound);
}
