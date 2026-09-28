// Selection and accounting proof obligations.
//
// Every test here is a statement the runtime must be able to defend: protected
// loads are never selected, stale evidence never creates eligibility, the
// explanation trace is total, and the checked-integer closure is exact.

#include <string>
#include <vector>

#include "fixture.hpp"
#include "load_shedding/json_io.hpp"
#include "test_harness.hpp"

using namespace load_shedding;

namespace {

/// A store with the default policy installed and the given facility evidence.
struct Rig {
  ls_test::ScratchDirectory directory{"planning"};
  Engine engine;

  explicit Rig(const FacilitySnapshot& snapshot, SheddingPolicy policy = make_default_policy()) {
    auto opened = ls_test::open_writer(directory.path());
    LS_CHECK_MSG(opened.ok(), opened.status().to_string());
    engine = std::move(opened.value());
    LS_REQUIRE_OK(ls_test::install_policy(engine, policy));
    LS_REQUIRE_OK(ls_test::install_snapshot(engine, snapshot));
  }

  Result<PlanOutcome> plan(std::int64_t deficit, std::uint64_t request_id = 1,
                           std::uint64_t tick = 100, bool emergency = false,
                           std::string justification = std::string()) {
    PlanRequest request = ls_test::plan_request(engine, request_id, deficit, tick);
    request.emergency_authority = emergency;
    request.emergency_justification = std::move(justification);
    return engine.plan(request);
  }
};

std::vector<std::string> selected_refs(const SheddingPlan& plan) {
  std::vector<std::string> refs;
  for (const ShedAction& action : plan.actions) {
    refs.push_back(action.load.value());
  }
  return refs;
}

/// Verdict of one load, reporting a failure instead of dereferencing a missing
/// explanation record.
LoadVerdict verdict_of(const SheddingPlan& plan, const std::string& ref) {
  const LoadConsideration* consideration = plan.find_consideration(LoadRef::parse(ref).value());
  if (consideration == nullptr) {
    LS_CHECK_MSG(false, "no explanation record for load '" + ref + "'");
    return LoadVerdict::EvidenceUnknown;
  }
  return consideration->verdict;
}

const ShedAction* action_of(const SheddingPlan& plan, const std::string& ref) {
  return plan.find_action(LoadRef::parse(ref).value());
}

}  // namespace

LS_TEST(planning, deficit_fully_satisfied_selects_the_documented_prefix) {
  Rig rig(ls_test::FacilityBuilder()
              .tick(100)
              .evidence_generation(1)
              .load("a-standard-big", 400000, PriorityClass::Standard)
              .load("b-standard-small", 100000, PriorityClass::Standard)
              .load("c-optional", 30000, PriorityClass::Optional)
              .total_demand(600000)
              .build());
  auto outcome = rig.plan(100000);
  LS_CHECK_MSG(outcome.ok(), outcome.status().to_string());
  const SheddingPlan& plan = outcome.value().plan;
  LS_CHECK_EQ(plan.coverage, CoverageStatus::FullyCovered);
  LS_CHECK_EQ(plan.closure.residual_deficit.watts(), std::int64_t{0});
  // Stage 0 takes the optional load; stage 1 then takes the largest standard
  // load whole, so the plan overshoots rather than splitting a load.
  LS_CHECK_EQ(plan.closure.selected_expected_reduction.watts(), std::int64_t{430000});
  LS_CHECK_EQ(plan.closure.overshoot.watts(), std::int64_t{330000});
  // Stage 0 (optional) runs first, so the optional load is always taken first.
  const std::vector<std::string> refs = selected_refs(plan);
  LS_CHECK_EQ(refs.size(), std::size_t{2});
  LS_CHECK_EQ(refs[0], std::string("c-optional"));
  LS_CHECK_EQ(refs[1], std::string("a-standard-big"));
  LS_CHECK_EQ(plan.closure.eligible_known_capacity.watts(), std::int64_t{530000});
  LS_CHECK_EQ(plan.closure.remaining_eligible_known_capacity.watts(), std::int64_t{100000});
  LS_CHECK_EQ(plan.closure.selected_load_count, std::uint64_t{2});
  LS_CHECK_EQ(plan.closure.eligible_load_count, std::uint64_t{3});
  LS_CHECK(plan.verify().ok());
  LS_CHECK_EQ(verdict_of(plan, "b-standard-small"), LoadVerdict::SkippedCoverageMet);
}

LS_TEST(planning, protected_obligations_are_absolute_even_under_emergency) {
  SheddingPolicy policy = make_default_policy();
  StageDefinition emergency;
  emergency.index = StageIndex::from_ordinal(3);
  emergency.name = "emergency";
  emergency.priorities = {PriorityClass::Critical, PriorityClass::Essential};
  emergency.requires_emergency_authority = true;
  policy.stages.push_back(emergency);
  policy.allow_emergency_override = true;
  policy.protected_priorities = {PriorityClass::Critical};

  Rig rig(ls_test::FacilityBuilder()
              .tick(100)
              .evidence_generation(1)
              .load("life-safety", 90000, PriorityClass::Critical, LoadClass::Protected)
              .load("cooling", 60000, PriorityClass::Essential, LoadClass::NonSheddable)
              .load("office", 20000, PriorityClass::Optional)
              .obligation("keep-life-safety", "life-safety", 90000)
              .total_demand(170000)
              .build(),
          policy);

  // Without an emergency grant nothing protected is selectable.
  auto refused = rig.plan(100000, 1, 100, true, "");
  LS_REQUIRE_STATUS(refused, StatusCode::InvalidArgument);

  auto ordinary = rig.plan(100000, 2, 100);
  LS_CHECK(ordinary.ok());
  LS_CHECK_EQ(ordinary.value().plan.coverage, CoverageStatus::Insufficient);
  // Both protected loads land in the protected bucket: the obligation-named load
  // and the one protected by its class.
  LS_CHECK_EQ(ordinary.value().plan.closure.protected_known_amount.watts(), std::int64_t{150000});
  LS_CHECK_EQ(ordinary.value().plan.closure.non_sheddable_known_amount.watts(), std::int64_t{0});
  LS_CHECK_EQ(verdict_of(ordinary.value().plan, "life-safety"), LoadVerdict::ObligationActive);
  LS_CHECK_EQ(verdict_of(ordinary.value().plan, "cooling"), LoadVerdict::NonSheddableClass);

  // With a grant the non-sheddable class becomes reachable, but the obligation
  // that names life-safety does not.
  auto granted = rig.plan(150000, 3, 100, true, "loss of utility feed A");
  LS_CHECK_MSG(granted.ok(), granted.status().to_string());
  const SheddingPlan& plan = granted.value().plan;
  LS_CHECK(plan.emergency_authority_used);
  LS_CHECK_EQ(plan.emergency_justification, std::string("loss of utility feed A"));
  LS_CHECK_EQ(verdict_of(plan, "life-safety"), LoadVerdict::ObligationActive);
  LS_CHECK(action_of(plan, "life-safety") == nullptr);
  const ShedAction* cooling = action_of(plan, "cooling");
  LS_CHECK(cooling != nullptr);
  if (cooling != nullptr) {
    LS_CHECK_EQ(cooling->reason, ReasonCode::EmergencyStageEligible);
    LS_CHECK_EQ(cooling->stage.value(), std::uint32_t{3});
  }
  LS_CHECK(plan.verify().ok());
}

LS_TEST(planning, policy_that_forbids_emergency_refuses_the_grant) {
  Rig rig(ls_test::FacilityBuilder()
              .tick(100)
              .evidence_generation(1)
              .load("only-load", 50000, PriorityClass::Standard)
              .build());
  auto refused = rig.plan(10000, 1, 100, true, "because");
  LS_REQUIRE_STATUS(refused, StatusCode::PolicyViolation);
}

LS_TEST(planning, stage_without_emergency_authority_is_reported_not_silently_skipped) {
  SheddingPolicy policy = make_default_policy();
  StageDefinition guarded;
  guarded.index = StageIndex::from_ordinal(3);
  guarded.name = "guarded";
  guarded.priorities = {PriorityClass::Essential};
  guarded.requires_emergency_authority = true;
  policy.stages.push_back(guarded);
  policy.allow_emergency_override = false;

  Rig rig(ls_test::FacilityBuilder()
              .tick(100)
              .evidence_generation(1)
              .load("essential-load", 70000, PriorityClass::Essential)
              .build(),
          policy);
  auto outcome = rig.plan(50000);
  LS_CHECK(outcome.ok());
  LS_CHECK_EQ(verdict_of(outcome.value().plan, "essential-load"),
              LoadVerdict::StageEmergencyDenied);
  LS_CHECK_EQ(outcome.value().plan.actions.size(), std::size_t{0});
  LS_CHECK_EQ(outcome.value().plan.coverage, CoverageStatus::Insufficient);
}

LS_TEST(planning, stale_evidence_never_creates_eligibility) {
  // The stale load is the only one that could cover the request.
  Rig rig(ls_test::FacilityBuilder()
              .tick(100)
              .evidence_generation(4)
              .stale_load("stale", 900000, 50, PriorityClass::Standard)
              .load("fresh", 1000, PriorityClass::Standard)
              .build());
  auto outcome = rig.plan(500000, 1, 100);
  LS_CHECK(outcome.ok());
  const SheddingPlan& plan = outcome.value().plan;
  LS_CHECK_EQ(verdict_of(plan, "stale"), LoadVerdict::EvidenceStale);
  LS_CHECK(action_of(plan, "stale") == nullptr);
  LS_CHECK_EQ(plan.closure.residual_deficit.watts(), std::int64_t{499000});
  LS_CHECK_EQ(plan.closure.stale_load_count, std::uint64_t{1});
  LS_CHECK_EQ(plan.closure.unavailable_known_amount.watts(), std::int64_t{900000});
  // Stale capacity is unknown capacity: the residual cannot be proven minimal.
  LS_CHECK(plan.closure.capacity_indeterminate);
  LS_CHECK_EQ(plan.coverage, CoverageStatus::Indeterminate);
}

LS_TEST(planning, unknown_contribution_is_never_read_as_zero) {
  Rig rig(ls_test::FacilityBuilder()
              .tick(100)
              .evidence_generation(1)
              .unknown_load("unknown-a", EvidenceState::Unknown)
              .unknown_load("unknown-b", EvidenceState::Unavailable)
              .unknown_load("unknown-c", EvidenceState::Denied)
              .unknown_load("unknown-d", EvidenceState::Unsafe)
              .unknown_load("unknown-e", EvidenceState::Unsupported)
              .load("known", 1000)
              .build());
  auto outcome = rig.plan(1000000, 1, 100);
  LS_CHECK(outcome.ok());
  const SheddingPlan& plan = outcome.value().plan;
  LS_CHECK_EQ(verdict_of(plan, "unknown-a"), LoadVerdict::EvidenceUnknown);
  LS_CHECK_EQ(verdict_of(plan, "unknown-b"), LoadVerdict::EvidenceUnavailable);
  LS_CHECK_EQ(verdict_of(plan, "unknown-c"), LoadVerdict::EvidenceDenied);
  LS_CHECK_EQ(verdict_of(plan, "unknown-d"), LoadVerdict::EvidenceUnsafe);
  LS_CHECK_EQ(verdict_of(plan, "unknown-e"), LoadVerdict::EvidenceUnsupported);
  LS_CHECK_EQ(plan.closure.indeterminate_load_count, std::uint64_t{5});
  LS_CHECK_EQ(plan.closure.denied_load_count, std::uint64_t{1});
  LS_CHECK_EQ(plan.closure.unsafe_load_count, std::uint64_t{1});
  LS_CHECK_EQ(plan.closure.unsupported_load_count, std::uint64_t{1});
  LS_CHECK(plan.closure.capacity_indeterminate);
  LS_CHECK_EQ(plan.coverage, CoverageStatus::Indeterminate);
  LS_CHECK_EQ(plan.closure.residual_deficit.watts(), std::int64_t{999000});
  // The unknown loads contribute nothing to the known capacity totals.
  LS_CHECK_EQ(plan.closure.eligible_known_capacity.watts(), std::int64_t{1000});
  LS_CHECK(plan.verify().ok());
}

LS_TEST(planning, confirmed_zero_and_negative_contributions_are_refused) {
  Rig rig(ls_test::FacilityBuilder()
              .tick(100)
              .evidence_generation(1)
              .load("zero-load", 0, PriorityClass::Standard)
              .build());
  auto outcome = rig.plan(1000, 1, 100);
  LS_CHECK(outcome.ok());
  LS_CHECK_EQ(verdict_of(outcome.value().plan, "zero-load"), LoadVerdict::ZeroContribution);
  LS_CHECK_EQ(outcome.value().plan.closure.zero_contribution_load_count, std::uint64_t{1});
  LS_CHECK_EQ(outcome.value().plan.closure.eligible_known_capacity.watts(), std::int64_t{0});
  LS_CHECK_EQ(outcome.value().plan.coverage, CoverageStatus::Insufficient);
  LS_CHECK_EQ(outcome.value().plan.closure.residual_deficit.watts(), std::int64_t{1000});
  // Zero contribution is a known fact, not an unknown one.
  LS_CHECK(!outcome.value().plan.closure.capacity_indeterminate);
}

LS_TEST(planning, negative_contribution_is_a_rejected_observation) {
  FacilitySnapshot snapshot = ls_test::FacilityBuilder()
                                  .tick(100)
                                  .evidence_generation(1)
                                  .load("known", 5000)
                                  .build();
  LoadRecord negative;
  negative.ref = LoadRef::parse("negative").value();
  negative.identity_generation = LoadGeneration::from_value(9);
  negative.contribution.state = EvidenceState::Known;
  negative.contribution.value = Power::from_watts(-1);
  negative.contribution.generation = snapshot.generation;
  negative.contribution.tick = snapshot.tick;
  snapshot.loads.push_back(negative);
  Rig rig(snapshot);
  auto outcome = rig.plan(1000, 1, 100);
  LS_CHECK(outcome.ok());
  LS_CHECK_EQ(verdict_of(outcome.value().plan, "negative"), LoadVerdict::NegativeContribution);
  LS_CHECK_EQ(outcome.value().plan.closure.eligible_known_capacity.watts(), std::int64_t{5000});
}

LS_TEST(planning, out_of_service_and_unmapped_priorities_are_reported) {
  Rig rig(ls_test::FacilityBuilder()
              .tick(100)
              .evidence_generation(1)
              .out_of_service("offline", 80000)
              .load("critical-load", 90000, PriorityClass::Critical)
              .load("deferrable-load", 10000, PriorityClass::Deferrable)
              .build());
  auto outcome = rig.plan(5000, 1, 100);
  LS_CHECK(outcome.ok());
  const SheddingPlan& plan = outcome.value().plan;
  LS_CHECK_EQ(verdict_of(plan, "offline"), LoadVerdict::OutOfService);
  // Critical is protected by the default policy, so it is not selectable.
  LS_CHECK_EQ(verdict_of(plan, "critical-load"), LoadVerdict::ProtectedPriority);
  LS_CHECK_EQ(plan.actions.size(), std::size_t{1});
  LS_CHECK_EQ(plan.actions[0].load.value(), std::string("deferrable-load"));
}

LS_TEST(planning, priority_not_covered_by_any_stage_is_reported) {
  SheddingPolicy policy = make_default_policy();
  policy.stages.erase(policy.stages.begin() + 2);  // drops the "important" stage
  policy.stages[1].index = StageIndex::from_ordinal(1);
  Rig rig(ls_test::FacilityBuilder()
              .tick(100)
              .evidence_generation(1)
              .load("important-load", 80000, PriorityClass::Important)
              .build(),
          policy);
  auto outcome = rig.plan(50000, 1, 100);
  LS_CHECK(outcome.ok());
  LS_CHECK_EQ(verdict_of(outcome.value().plan, "important-load"),
              LoadVerdict::PriorityNotInPolicy);
  LS_CHECK_EQ(outcome.value().plan.closure.non_sheddable_known_amount.watts(), std::int64_t{80000});
}

LS_TEST(planning, stage_caps_bound_selection_and_are_reported) {
  SheddingPolicy policy = make_default_policy();
  policy.stages[1].max_shed = Power::from_watts(150000);
  policy.stages[1].max_loads = 1;
  Rig rig(ls_test::FacilityBuilder()
              .tick(100)
              .evidence_generation(1)
              .load("standard-fits", 100000, PriorityClass::Standard)
              .load("standard-too-big", 200000, PriorityClass::Standard)
              .load("standard-after-cap", 40000, PriorityClass::Standard)
              .build(),
          policy);
  auto outcome = rig.plan(500000, 1, 100);
  LS_CHECK(outcome.ok());
  const SheddingPlan& plan = outcome.value().plan;
  // Largest first: the 200 kW load does not fit the 150 kW stage cap, the 100 kW
  // load does and uses the stage's single load slot, and the 40 kW load is then
  // blocked by the load cap.
  LS_CHECK_EQ(verdict_of(plan, "standard-too-big"), LoadVerdict::SkippedStageCap);
  LS_CHECK_EQ(plan.actions.size(), std::size_t{1});
  LS_CHECK_EQ(plan.actions[0].load.value(), std::string("standard-fits"));
  LS_CHECK_EQ(verdict_of(plan, "standard-after-cap"), LoadVerdict::SkippedStageLoadCap);
  LS_CHECK_EQ(plan.coverage, CoverageStatus::PolicyLimited);
  LS_CHECK_EQ(plan.closure.remaining_eligible_known_capacity.watts(), std::int64_t{240000});
  LS_CHECK_EQ(plan.closure.residual_deficit.watts(), std::int64_t{400000});
  LS_CHECK_EQ(plan.stages[1].stop_reason, StageStopReason::LoadCapReached);
  LS_CHECK(plan.verify().ok());
}

LS_TEST(planning, stage_power_cap_skips_a_load_that_does_not_fit) {
  SheddingPolicy policy = make_default_policy();
  policy.stages[1].max_shed = Power::from_watts(100000);
  Rig rig(ls_test::FacilityBuilder()
              .tick(100)
              .evidence_generation(1)
              .load("too-big", 300000, PriorityClass::Standard)
              .load("fits", 60000, PriorityClass::Standard)
              .build(),
          policy);
  auto outcome = rig.plan(200000, 1, 100);
  LS_CHECK(outcome.ok());
  const SheddingPlan& plan = outcome.value().plan;
  LS_CHECK_EQ(verdict_of(plan, "too-big"), LoadVerdict::SkippedStageCap);
  LS_CHECK_EQ(plan.actions[0].load.value(), std::string("fits"));
  LS_CHECK_EQ(plan.coverage, CoverageStatus::PolicyLimited);
  LS_CHECK_EQ(plan.stages[1].stop_reason, StageStopReason::PowerCapReached);
}

LS_TEST(planning, parts_per_million_stage_cap_is_exact) {
  SheddingPolicy policy = make_default_policy();
  policy.stages[1].max_shed_ppm = 500000;  // half of known sheddable demand
  Rig rig(ls_test::FacilityBuilder()
              .tick(100)
              .evidence_generation(1)
              .load("standard-1", 400000, PriorityClass::Standard)
              .load("standard-2", 400000, PriorityClass::Standard)
              .build(),
          policy);
  auto outcome = rig.plan(800000, 1, 100);
  LS_CHECK(outcome.ok());
  const SheddingPlan& plan = outcome.value().plan;
  LS_CHECK_EQ(plan.stages[1].effective_power_cap->watts(), std::int64_t{400000});
  LS_CHECK_EQ(plan.actions.size(), std::size_t{1});
  LS_CHECK_EQ(plan.closure.selected_expected_reduction.watts(), std::int64_t{400000});
  LS_CHECK_EQ(plan.closure.residual_deficit.watts(), std::int64_t{400000});
  LS_CHECK_EQ(plan.coverage, CoverageStatus::PolicyLimited);
}

LS_TEST(planning, no_overshoot_mode_leaves_an_explicit_residual) {
  SheddingPolicy policy = make_default_policy();
  policy.mode = SelectionMode::NoOvershootGreedy;
  Rig rig(ls_test::FacilityBuilder()
              .tick(100)
              .evidence_generation(1)
              .load("big", 300000, PriorityClass::Standard)
              .load("small", 50000, PriorityClass::Standard)
              .build(),
          policy);
  auto outcome = rig.plan(100000, 1, 100);
  LS_CHECK(outcome.ok());
  const SheddingPlan& plan = outcome.value().plan;
  LS_CHECK_EQ(verdict_of(plan, "big"), LoadVerdict::SkippedNoOvershoot);
  LS_CHECK_EQ(plan.actions.size(), std::size_t{1});
  LS_CHECK_EQ(plan.actions[0].load.value(), std::string("small"));
  LS_CHECK_EQ(plan.closure.overshoot.watts(), std::int64_t{0});
  LS_CHECK_EQ(plan.closure.residual_deficit.watts(), std::int64_t{50000});
  LS_CHECK_EQ(plan.coverage, CoverageStatus::PolicyLimited);
  LS_CHECK(plan.verify().ok());
}

LS_TEST(planning, ordering_rule_is_least_important_then_largest_then_reference) {
  Rig rig(ls_test::FacilityBuilder()
              .tick(100)
              .evidence_generation(1)
              .load("standard-large", 200000, PriorityClass::Standard)
              .load("standard-small", 100000, PriorityClass::Standard)
              .load("deferrable-a", 10000, PriorityClass::Deferrable)
              .load("deferrable-b", 10000, PriorityClass::Deferrable)
              .build());
  auto outcome = rig.plan(320000, 1, 100);
  LS_CHECK(outcome.ok());
  const std::vector<std::string> refs = selected_refs(outcome.value().plan);
  // Stage 0 first (deferrable, equal size, so reference order decides), then the
  // standard stage takes the largest first.
  LS_CHECK_EQ(refs.size(), std::size_t{4});
  LS_CHECK_EQ(refs[0], std::string("deferrable-a"));
  LS_CHECK_EQ(refs[1], std::string("deferrable-b"));
  LS_CHECK_EQ(refs[2], std::string("standard-large"));
  LS_CHECK_EQ(refs[3], std::string("standard-small"));
  for (std::size_t index = 0; index < refs.size(); ++index) {
    LS_CHECK_EQ(outcome.value().plan.actions[index].selection_index, static_cast<std::uint32_t>(index));
  }
}

LS_TEST(planning, minimum_on_time_fails_closed_without_observation) {
  SheddingPolicy policy = make_default_policy();
  policy.min_on_ticks = 10;
  Rig rig(ls_test::FacilityBuilder()
              .tick(100)
              .evidence_generation(1)
              .load("recently-energized", 90000, PriorityClass::Standard)
              .build(),
          policy);
  // No observed state at all: the constraint cannot be shown to hold.
  auto blocked = rig.plan(50000, 1, 100);
  LS_CHECK(blocked.ok());
  LS_CHECK_EQ(verdict_of(blocked.value().plan, "recently-energized"),
              LoadVerdict::MinOnNotElapsed);
  LS_CHECK_EQ(blocked.value().plan.actions.size(), std::size_t{0});
  LS_CHECK_EQ(blocked.value().plan.closure.unavailable_known_amount.watts(), std::int64_t{90000});
  LS_CHECK(!blocked.value().plan.closure.capacity_indeterminate);
}

LS_TEST(planning, explanation_trace_is_total_and_unique) {
  Rig rig(ls_test::FacilityBuilder()
              .tick(100)
              .evidence_generation(1)
              .load("a", 10000, PriorityClass::Standard)
              .load("b", 20000, PriorityClass::Optional)
              .load("c", 30000, PriorityClass::Critical)
              .unknown_load("d")
              .out_of_service("e", 40000)
              .load("f", 50000, PriorityClass::Deferrable)
              .build());
  auto outcome = rig.plan(15000, 1, 100);
  LS_CHECK(outcome.ok());
  const SheddingPlan& plan = outcome.value().plan;
  LS_CHECK_EQ(plan.considerations.size(), std::size_t{6});
  std::vector<std::string> seen;
  for (const LoadConsideration& consideration : plan.considerations) {
    seen.push_back(consideration.load.value());
  }
  for (std::size_t index = 1; index < seen.size(); ++index) {
    LS_CHECK(seen[index - 1] < seen[index]);
  }
  LS_CHECK(plan.verify().ok());
}

LS_TEST(planning, closure_identity_holds_for_every_shape) {
  const std::vector<std::int64_t> deficits = {1, 10000, 100000, 1000000};
  for (const std::int64_t deficit : deficits) {
    Rig rig(ls_test::FacilityBuilder()
                .tick(100)
                .evidence_generation(1)
                .load("a", 30000, PriorityClass::Optional)
                .load("b", 70000, PriorityClass::Standard)
                .unknown_load("c")
                .build());
    auto outcome = rig.plan(deficit, 1, 100);
    LS_CHECK(outcome.ok());
    const AccountingClosure& closure = outcome.value().plan.closure;
    LS_CHECK_EQ(closure.requested_reduction.watts(), deficit);
    LS_CHECK_EQ(closure.selected_expected_reduction.watts() + closure.residual_deficit.watts() -
                    closure.overshoot.watts(),
                deficit);
    LS_CHECK(closure.selected_expected_reduction <= closure.eligible_known_capacity);
    LS_CHECK(outcome.value().plan.verify().ok());
    LS_COUNT_ITERATION();
  }
}

LS_TEST(planning, protected_reserve_ceiling_is_reported_when_demand_is_known) {
  Rig rig(ls_test::FacilityBuilder()
              .tick(100)
              .evidence_generation(1)
              .load("floor", 120000, PriorityClass::Critical, LoadClass::Protected)
              .load("sheddable", 300000, PriorityClass::Standard)
              .obligation("floor-obligation", "floor", 100000)
              .total_demand(420000)
              .build());
  auto outcome = rig.plan(100000, 1, 100);
  LS_CHECK(outcome.ok());
  const AccountingClosure& closure = outcome.value().plan.closure;
  LS_CHECK(closure.ceiling_verified);
  LS_CHECK(closure.effective_shedding_ceiling.has_value());
  LS_CHECK_EQ(closure.effective_shedding_ceiling->watts(), std::int64_t{320000});
}

LS_TEST(planning, unknown_total_demand_leaves_the_ceiling_unverified) {
  Rig rig(ls_test::FacilityBuilder()
              .tick(100)
              .evidence_generation(1)
              .load("sheddable", 300000, PriorityClass::Standard)
              .build());
  auto outcome = rig.plan(100000, 1, 100);
  LS_CHECK(outcome.ok());
  LS_CHECK(!outcome.value().plan.closure.ceiling_verified);
  LS_CHECK(!outcome.value().plan.closure.effective_shedding_ceiling.has_value());
}

LS_TEST(planning, zero_and_negative_requests_are_refused) {
  Rig rig(ls_test::FacilityBuilder().tick(100).evidence_generation(1).load("load", 1000).build());
  PlanRequest request = ls_test::plan_request(rig.engine, 1, 0, 100);
  LS_REQUIRE_STATUS(rig.engine.plan(request), StatusCode::InvalidArgument);
  request = ls_test::plan_request(rig.engine, 1, -5, 100);
  LS_REQUIRE_STATUS(rig.engine.plan(request), StatusCode::InvalidArgument);
}

LS_TEST(planning, plan_identity_records_the_evidence_generation_set) {
  Rig rig(ls_test::FacilityBuilder()
              .tick(100)
              .evidence_generation(3)
              .load("a", 1000, PriorityClass::Optional)
              .load("b", 2000, PriorityClass::Standard)
              .build());
  auto outcome = rig.plan(1000, 7, 100);
  LS_CHECK(outcome.ok());
  const PlanIdentity& identity = outcome.value().plan.identity;
  LS_CHECK_EQ(identity.request_id.value(), std::uint64_t{7});
  LS_CHECK_EQ(identity.evidence_generation.value(), std::uint64_t{3});
  LS_CHECK_EQ(identity.policy_generation.value(), rig.engine.policy_generation().value());
  LS_CHECK_EQ(identity.policy_digest.hex(), rig.engine.policy().content_digest().hex());
  LS_CHECK(!identity.evidence_generations.empty());
  for (std::size_t index = 1; index < identity.evidence_generations.size(); ++index) {
    LS_CHECK(identity.evidence_generations[index - 1] < identity.evidence_generations[index]);
  }
  LS_CHECK_EQ(identity.tick.value(), std::uint64_t{100});
}
