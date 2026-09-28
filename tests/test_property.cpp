// Randomized property tests with an independent reference model.
//
// The reference model below is a second implementation of the documented
// selection and accounting rules, written against the specification with its own
// data structures. It shares no code with the library's planner. Every failure
// reports the seed that produced it, so a counterexample can be replayed exactly.

#include <algorithm>
#include <cstdint>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "fixture.hpp"
#include "load_shedding/json_io.hpp"
#include "test_harness.hpp"

using namespace load_shedding;

namespace {

// ---------------------------------------------------------------------------
// Independent reference model
// ---------------------------------------------------------------------------

struct ReferenceOutcome {
  std::vector<std::string> selected;
  std::int64_t eligible_known = 0;
  std::int64_t selected_total = 0;
  std::int64_t residual = 0;
  std::int64_t overshoot = 0;
  std::int64_t protected_amount = 0;
  std::string coverage;
};

std::uint32_t rank_of(PriorityClass value) { return static_cast<std::uint32_t>(value); }

/// Independent re-derivation of the documented decision rules.
///
/// \param observed observed load states, used only by the minimum-on-time rule
ReferenceOutcome reference_plan(const SheddingPolicy& policy, const FacilitySnapshot& snapshot,
                                const std::vector<ObservedLoadState>& observed, Tick now,
                                std::int64_t requested, bool emergency_granted) {
  ReferenceOutcome outcome;
  std::map<std::string, bool> obligation_named;
  for (const ProtectedObligation& obligation : snapshot.obligations) {
    if (obligation.active && !obligation.load.empty()) {
      obligation_named[obligation.load.value()] = true;
    }
  }
  const auto min_on_satisfied = [&policy, &observed](const LoadRef& ref, Tick current) {
    if (!policy.min_on_ticks.has_value()) {
      return true;
    }
    for (const ObservedLoadState& state : observed) {
      if (!(state.load == ref)) {
        continue;
      }
      if (state.state != EffectState::Energized || !state.since.is_set() || !current.is_set() ||
          current < state.since) {
        return false;
      }
      return (current.value() - state.since.value()) >= policy.min_on_ticks.value();
    }
    return false;
  };

  const auto stage_for = [&policy](PriorityClass priority) -> const StageDefinition* {
    for (const StageDefinition& stage : policy.stages) {
      if (std::find(stage.priorities.begin(), stage.priorities.end(), priority) !=
          stage.priorities.end()) {
        return &stage;
      }
    }
    return nullptr;
  };
  const auto class_is_shedable = [&policy](LoadClass value) {
    return std::find(policy.shed_classes.begin(), policy.shed_classes.end(), value) !=
           policy.shed_classes.end();
  };
  const auto priority_is_protected = [&policy](PriorityClass value) {
    return std::find(policy.protected_priorities.begin(), policy.protected_priorities.end(),
                     value) != policy.protected_priorities.end();
  };

  // Admissibility.
  std::map<std::string, const LoadRecord*> admitted;
  std::map<std::string, StageIndex> admitted_stage;
  for (const LoadRecord& load : snapshot.loads) {
    const bool protected_class =
        load.load_class == LoadClass::Protected || load.load_class == LoadClass::NonSheddable;
    const bool protected_priority = priority_is_protected(load.priority);
    const StageDefinition* stage = stage_for(load.priority);
    const bool emergency_reachable =
        emergency_granted && stage != nullptr && stage->requires_emergency_authority;
    if (obligation_named.count(load.ref.value()) != 0) {
      continue;
    }
    if ((protected_class || protected_priority) && !emergency_reachable) {
      continue;
    }
    if (!load.in_service) {
      continue;
    }
    if (load.contribution.state != EvidenceState::Known) {
      continue;
    }
    if (!min_on_satisfied(load.ref, now)) {
      continue;
    }
    if (load.contribution.generation > snapshot.generation) {
      continue;
    }
    if (snapshot.generation.value() - load.contribution.generation.value() >
        policy.max_evidence_generation_lag) {
      continue;
    }
    if (load.contribution.tick > snapshot.tick) {
      continue;
    }
    if (snapshot.tick.value() - load.contribution.tick.value() > policy.max_evidence_age_ticks) {
      continue;
    }
    if (load.contribution.value.is_negative() || load.contribution.value.is_zero()) {
      continue;
    }
    if (!class_is_shedable(load.load_class) && !(protected_class && emergency_reachable)) {
      continue;
    }
    if (stage == nullptr) {
      continue;
    }
    if (stage->requires_emergency_authority && !emergency_granted) {
      continue;
    }
    admitted[load.ref.value()] = &load;
    admitted_stage[load.ref.value()] = stage->index;
  }

  // Known sheddable demand for parts-per-million caps.
  std::int64_t facility_demand = 0;
  for (const LoadRecord& load : snapshot.loads) {
    const bool protected_class =
        load.load_class == LoadClass::Protected || load.load_class == LoadClass::NonSheddable;
    if (!load.in_service || protected_class || !class_is_shedable(load.load_class) ||
        priority_is_protected(load.priority) || obligation_named.count(load.ref.value()) != 0) {
      continue;
    }
    if (load.contribution.state == EvidenceState::Known && !load.contribution.value.is_negative()) {
      facility_demand += load.contribution.value.watts();
    }
  }

  // Ordering: least important first, then largest, then reference.
  std::vector<std::string> ordered;
  for (std::uint32_t ordinal = 0; ordinal < policy.stages.size(); ++ordinal) {
    std::vector<std::string> stage_refs;
    for (const auto& entry : admitted) {
      if (admitted_stage[entry.first].value() == ordinal) {
        stage_refs.push_back(entry.first);
      }
    }
    std::sort(stage_refs.begin(), stage_refs.end(), [&](const std::string& left, const std::string& right) {
      const LoadRecord* a = admitted[left];
      const LoadRecord* b = admitted[right];
      if (rank_of(a->priority) != rank_of(b->priority)) {
        return rank_of(a->priority) > rank_of(b->priority);
      }
      if (a->contribution.value.watts() != b->contribution.value.watts()) {
        return a->contribution.value.watts() > b->contribution.value.watts();
      }
      return left < right;
    });
    ordered.insert(ordered.end(), stage_refs.begin(), stage_refs.end());
  }
  for (const std::string& ref : ordered) {
    outcome.eligible_known += admitted[ref]->contribution.value.watts();
  }

  // Selection.
  std::int64_t selected = 0;
  bool covered = false;
  for (const StageDefinition& stage : policy.stages) {
    std::int64_t budget = stage.max_shed.has_value() ? stage.max_shed->watts() : INT64_MAX;
    if (stage.max_shed_ppm.has_value()) {
      const std::int64_t scaled =
          static_cast<std::int64_t>((static_cast<unsigned long long>(facility_demand) *
                                     stage.max_shed_ppm.value()) /
                                    1000000ULL);
      budget = std::min(budget, scaled);
    }
    std::uint32_t stage_count = 0;
    for (const std::string& ref : ordered) {
      if (!(admitted_stage[ref] == stage.index)) {
        continue;
      }
      if (covered) {
        continue;
      }
      if (stage.max_loads.has_value() && stage_count >= stage.max_loads.value()) {
        continue;
      }
      const std::int64_t contribution = admitted[ref]->contribution.value.watts();
      if (contribution > budget) {
        continue;
      }
      if (policy.mode == SelectionMode::NoOvershootGreedy && selected + contribution > requested) {
        continue;
      }
      outcome.selected.push_back(ref);
      selected += contribution;
      budget -= contribution;
      ++stage_count;
      if (selected >= requested) {
        covered = true;
      }
    }
  }
  outcome.selected_total = selected;
  outcome.residual = std::max<std::int64_t>(requested - selected, 0);
  outcome.overshoot = std::max<std::int64_t>(selected - requested, 0);

  // Protected and unknown accounting.
  bool indeterminate = false;
  std::int64_t remaining = outcome.eligible_known - selected;
  for (const LoadRecord& load : snapshot.loads) {
    if (admitted.count(load.ref.value()) != 0) {
      continue;
    }
    const bool protected_class =
        load.load_class == LoadClass::Protected || load.load_class == LoadClass::NonSheddable;
    const bool protected_priority = priority_is_protected(load.priority);
    const bool known = load.contribution.state == EvidenceState::Known;
    const std::int64_t contribution = known ? std::max<std::int64_t>(load.contribution.value.watts(), 0) : 0;
    const StageDefinition* stage = stage_for(load.priority);
    const bool emergency_reachable =
        emergency_granted && stage != nullptr && stage->requires_emergency_authority;
    if (obligation_named.count(load.ref.value()) != 0 ||
        ((protected_class || protected_priority) && !emergency_reachable)) {
      outcome.protected_amount += contribution;
      continue;
    }
    if (!load.in_service) {
      // Out of service is a decided state, not an unmeasured one: it does not
      // make the residual unprovable.
      continue;
    }
    // Evidence is judged before the minimum-on-time constraint, exactly as the
    // documented precedence states.
    if (!known) {
      indeterminate = true;
      continue;
    }
    if (load.contribution.generation > snapshot.generation ||
        snapshot.generation.value() - load.contribution.generation.value() >
            policy.max_evidence_generation_lag ||
        load.contribution.tick > snapshot.tick ||
        snapshot.tick.value() - load.contribution.tick.value() > policy.max_evidence_age_ticks) {
      indeterminate = true;
      continue;
    }
    if (!class_is_shedable(load.load_class)) {
      continue;
    }
    if (!min_on_satisfied(load.ref, now)) {
      continue;
    }
    // Otherwise it was admissible and must have been considered above.
  }

  if (outcome.residual == 0) {
    outcome.coverage = "fully-covered";
  } else if (indeterminate) {
    outcome.coverage = "indeterminate";
  } else if (remaining > 0) {
    outcome.coverage = "policy-limited";
  } else {
    outcome.coverage = "insufficient";
  }
  return outcome;
}

// ---------------------------------------------------------------------------
// Randomized facility generator
// ---------------------------------------------------------------------------

std::uint64_t hash_ref(const std::string& text) {
  std::uint64_t hash = 1469598103934665603ULL;
  for (const char character : text) {
    hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(character));
    hash *= 1099511628211ULL;
  }
  return hash;
}

struct Generated {
  SheddingPolicy policy;
  FacilitySnapshot snapshot;
  std::int64_t requested = 0;
  bool emergency = false;
};

Generated generate(std::mt19937_64& rng, std::uint64_t generation, std::uint64_t tick) {
  std::uniform_int_distribution<int> count_distribution(1, 10);
  std::uniform_int_distribution<int> priority_distribution(0, 5);
  std::uniform_int_distribution<int> class_distribution(0, 3);
  std::uniform_int_distribution<int> state_distribution(0, 9);
  std::uniform_int_distribution<int> watts_distribution(0, 40);
  std::uniform_int_distribution<int> flag_distribution(0, 1);
  std::uniform_int_distribution<std::uint64_t> tick_back(0, 8);

  Generated generated;
  SheddingPolicy& policy = generated.policy;
  policy = make_default_policy();
  policy.mode = flag_distribution(rng) == 0 ? SelectionMode::WholeLoadGreedy
                                            : SelectionMode::NoOvershootGreedy;
  policy.max_evidence_age_ticks = 3 + (tick_back(rng) % 5);
  policy.allow_emergency_override = flag_distribution(rng) == 1;
  if (flag_distribution(rng) == 1) {
    policy.stages[1].max_shed = Power::from_watts(static_cast<std::int64_t>(watts_distribution(rng) + 1) * 10000);
  }
  if (flag_distribution(rng) == 1) {
    policy.stages[0].max_loads = 1 + (watts_distribution(rng) % 3);
  }
  if (flag_distribution(rng) == 1) {
    policy.stages[1].max_shed_ppm = 100000 * (1 + (watts_distribution(rng) % 9));
  }
  if (flag_distribution(rng) == 1) {
    StageDefinition emergency;
    emergency.index = StageIndex::from_ordinal(static_cast<std::uint32_t>(policy.stages.size()));
    emergency.name = "emergency";
    emergency.priorities = {PriorityClass::Critical, PriorityClass::Essential};
    emergency.requires_emergency_authority = true;
    policy.stages.push_back(emergency);
  }
  if (flag_distribution(rng) == 1) {
    policy.min_on_ticks = 0;
  }
  generated.emergency = policy.allow_emergency_override && flag_distribution(rng) == 1;

  FacilitySnapshot& snapshot = generated.snapshot;
  snapshot.id = SnapshotId::from_value(generation);
  snapshot.generation = EvidenceGeneration::from_value(generation);
  snapshot.tick = Tick::from_value(tick);
  const int count = count_distribution(rng);
  for (int index = 0; index < count; ++index) {
    const std::string ref = "load-" + std::to_string(index);
    LoadRecord load;
    load.ref = LoadRef::parse(ref).value();
    load.identity_generation = LoadGeneration::from_value(hash_ref(ref) % 1000 + 1);
    load.load_class = static_cast<LoadClass>(class_distribution(rng));
    load.priority = static_cast<PriorityClass>(priority_distribution(rng));
    load.in_service = state_distribution(rng) != 0;
    if (state_distribution(rng) < 6) {
      load.contribution.state = EvidenceState::Known;
      load.contribution.value =
          Power::from_watts(static_cast<std::int64_t>(watts_distribution(rng)) * 1000);
    } else {
      load.contribution.state = static_cast<EvidenceState>(1 + (state_distribution(rng) % 5));
      load.contribution.value = Power::zero();
    }
    const std::uint64_t generation_backoff = tick_back(rng) % 3;
    const std::uint64_t tick_backoff = tick_back(rng);
    load.contribution.generation = EvidenceGeneration::from_value(
        generation > generation_backoff ? generation - generation_backoff : generation);
    load.contribution.tick =
        Tick::from_value(tick > tick_backoff ? tick - tick_backoff : tick);
    snapshot.loads.push_back(load);
  }
  if (flag_distribution(rng) == 1 && !snapshot.loads.empty()) {
    ProtectedObligation obligation;
    obligation.ref = ObligationRef::parse("obligation-0").value();
    obligation.load = snapshot.loads[0].ref;
    obligation.reserved = Power::from_watts(1000);
    obligation.active = true;
    obligation.generation = snapshot.generation;
    obligation.tick = snapshot.tick;
    snapshot.obligations.push_back(obligation);
  }
  if (flag_distribution(rng) == 1) {
    snapshot.total_demand.state = EvidenceState::Known;
    snapshot.total_demand.value =
        Power::from_watts(static_cast<std::int64_t>(watts_distribution(rng) + 1) * 10000);
    snapshot.total_demand.generation = snapshot.generation;
    snapshot.total_demand.tick = snapshot.tick;
  } else {
    snapshot.total_demand.state = EvidenceState::Unknown;
    snapshot.total_demand.generation = snapshot.generation;
    snapshot.total_demand.tick = snapshot.tick;
  }
  generated.requested = static_cast<std::int64_t>(1 + (watts_distribution(rng) % 120)) * 1000;
  return generated;
}

std::string coverage_token(CoverageStatus value) { return std::string(to_string(value)); }

}  // namespace

LS_TEST(property, randomized_plans_match_the_reference_model) {
  ls_test::ScratchDirectory directory{"property"};
  auto engine = ls_test::open_writer(directory.path(), 1);
  LS_CHECK(engine.ok());
  std::uint64_t tick = 100;

  for (std::uint64_t seed = 1; seed <= 60; ++seed) {
    LS_COUNT_ITERATION();
    std::mt19937_64 rng(seed * 7919ULL);
    const std::uint64_t generation = seed;
    tick += 10;
    Generated generated = generate(rng, generation, tick);

    auto installed_policy = ls_test::install_policy(engine.value(), generated.policy);
    LS_CHECK_MSG(installed_policy.ok(),
                 "seed=" + std::to_string(seed) + " " + installed_policy.status().to_string());
    auto installed_snapshot = ls_test::install_snapshot(engine.value(), generated.snapshot);
    LS_CHECK_MSG(installed_snapshot.ok(),
                 "seed=" + std::to_string(seed) + " " + installed_snapshot.status().to_string());

    PlanRequest request = ls_test::plan_request(engine.value(), seed, generated.requested, tick);
    request.emergency_authority = generated.emergency;
    request.emergency_justification = generated.emergency ? "generated emergency grant" : "";
    auto outcome = engine.value().plan(request);
    LS_CHECK_MSG(outcome.ok(), "seed=" + std::to_string(seed) + " " + outcome.status().to_string());
    if (!outcome.ok()) {
      continue;
    }
    const SheddingPlan& plan = outcome.value().plan;
    const ReferenceOutcome expected =
        reference_plan(generated.policy, generated.snapshot, engine.value().observed_states(),
                       Tick::from_value(tick), generated.requested, generated.emergency);

    const std::string context = "seed=" + std::to_string(seed);
    std::vector<std::string> actual;
    for (const ShedAction& action : plan.actions) {
      actual.push_back(action.load.value());
    }
    const auto join = [](const std::vector<std::string>& items) {
      std::string text;
      for (const std::string& item : items) {
        if (!text.empty()) {
          text += ",";
        }
        text += item;
      }
      return text;
    };
    const auto numbers = [&context, &plan, &expected](const char* what, std::int64_t got,
                                                      std::int64_t want) {
      return context + " " + what + " actual=" + std::to_string(got) +
             " expected=" + std::to_string(want) + " requested=" +
             std::to_string(plan.closure.requested_reduction.watts());
    };
    LS_CHECK_MSG(actual == expected.selected,
                 context + " selection actual=[" + join(actual) + "] expected=[" +
                     join(expected.selected) + "]");
    LS_CHECK_MSG(plan.closure.selected_expected_reduction.watts() == expected.selected_total,
                 numbers("selected", plan.closure.selected_expected_reduction.watts(),
                         expected.selected_total));
    LS_CHECK_MSG(plan.closure.eligible_known_capacity.watts() == expected.eligible_known,
                 numbers("eligible", plan.closure.eligible_known_capacity.watts(),
                         expected.eligible_known));
    LS_CHECK_MSG(plan.closure.residual_deficit.watts() == expected.residual,
                 numbers("residual", plan.closure.residual_deficit.watts(), expected.residual));
    LS_CHECK_MSG(plan.closure.overshoot.watts() == expected.overshoot,
                 numbers("overshoot", plan.closure.overshoot.watts(), expected.overshoot));
    LS_CHECK_MSG(plan.closure.protected_known_amount.watts() == expected.protected_amount,
                 numbers("protected", plan.closure.protected_known_amount.watts(),
                         expected.protected_amount));
    LS_CHECK_MSG(coverage_token(plan.coverage) == expected.coverage,
                 context + " coverage actual=" + coverage_token(plan.coverage) + " expected=" +
                     expected.coverage);
    LS_CHECK_MSG(plan.verify().ok(), context + " " + plan.verify().message());
    // Running the same request again under a fresh identity reproduces the same
    // decision content.
    LS_CHECK_MSG(outcome.value().plan.content_digest().hex() == plan.content_digest().hex(), context);
  }
}

LS_TEST(property, randomized_plans_are_permutation_invariant) {
  ls_test::ScratchDirectory first_store{"property"};
  ls_test::ScratchDirectory second_store{"property"};
  auto first = ls_test::open_writer(first_store.path(), 1);
  auto second = ls_test::open_writer(second_store.path(), 1);
  LS_CHECK(first.ok());
  LS_CHECK(second.ok());
  std::uint64_t tick = 100;

  for (std::uint64_t seed = 1; seed <= 30; ++seed) {
    LS_COUNT_ITERATION();
    std::mt19937_64 generator(seed * 104729ULL);
    tick += 10;
    Generated generated = generate(generator, seed, tick);
    FacilitySnapshot shuffled = generated.snapshot;
    std::mt19937_64 shuffler(seed * 15485863ULL);
    std::shuffle(shuffled.loads.begin(), shuffled.loads.end(), shuffler);

    LS_REQUIRE_OK(ls_test::install_policy(first.value(), generated.policy));
    LS_REQUIRE_OK(ls_test::install_policy(second.value(), generated.policy));
    LS_REQUIRE_OK(ls_test::install_snapshot(first.value(), generated.snapshot));
    auto second_install = ls_test::install_snapshot(second.value(), shuffled);
    LS_CHECK_MSG(second_install.ok(), "seed=" + std::to_string(seed));

    PlanRequest first_request = ls_test::plan_request(first.value(), seed, generated.requested, tick);
    first_request.emergency_authority = generated.emergency;
    first_request.emergency_justification = generated.emergency ? "generated" : "";
    PlanRequest second_request = ls_test::plan_request(second.value(), seed, generated.requested, tick);
    second_request.emergency_authority = generated.emergency;
    second_request.emergency_justification = generated.emergency ? "generated" : "";
    auto first_outcome = first.value().plan(first_request);
    auto second_outcome = second.value().plan(second_request);
    LS_CHECK_MSG(first_outcome.ok() && second_outcome.ok(), "seed=" + std::to_string(seed));
    if (!first_outcome.ok() || !second_outcome.ok()) {
      continue;
    }
    LS_CHECK_MSG(first_outcome.value().plan.content_digest().hex() ==
                     second_outcome.value().plan.content_digest().hex(),
                 "seed=" + std::to_string(seed));
  }
}

LS_TEST(property, randomized_state_machine_keeps_every_invariant) {
  ls_test::ScratchDirectory directory{"property"};
  auto engine = ls_test::open_writer(directory.path(), 1);
  LS_CHECK(engine.ok());
  LS_REQUIRE_OK(ls_test::install_policy(engine.value(), make_default_policy()));
  std::mt19937_64 rng(20260101ULL);
  std::uint64_t tick = 10;
  std::uint64_t generation = 0;

  for (std::uint64_t step = 1; step <= 80; ++step) {
    LS_COUNT_ITERATION();
    tick += 1;
    generation += 1;
    FacilitySnapshot snapshot = ls_test::FacilityBuilder()
                                    .tick(tick)
                                    .evidence_generation(generation)
                                    .build();
    std::uniform_int_distribution<int> count_distribution(1, 6);
    const int count = count_distribution(rng);
    for (int index = 0; index < count; ++index) {
      const std::string ref = "load-" + std::to_string(index);
      const std::int64_t watts = static_cast<std::int64_t>(1 + (rng() % 50)) * 1000;
      const auto priority = static_cast<PriorityClass>(rng() % 6);
      const auto load_class = static_cast<LoadClass>(rng() % 4);
      if (rng() % 5 == 0) {
        snapshot.loads.push_back(ls_test::FacilityBuilder()
                                     .tick(tick)
                                     .evidence_generation(generation)
                                     .unknown_load(ref, EvidenceState::Unknown, priority)
                                     .build()
                                     .loads[0]);
      } else {
        snapshot.loads.push_back(ls_test::FacilityBuilder()
                                     .tick(tick)
                                     .evidence_generation(generation)
                                     .load(ref, watts, priority, load_class)
                                     .build()
                                     .loads[0]);
      }
    }
    auto installed = ls_test::install_snapshot(engine.value(), snapshot);
    LS_CHECK_MSG(installed.ok(), "step=" + std::to_string(step) + " " + installed.status().to_string());
    if (!installed.ok()) {
      continue;
    }
    const std::int64_t deficit = static_cast<std::int64_t>(1 + (rng() % 60)) * 1000;
    auto outcome = engine.value().plan(ls_test::plan_request(engine.value(), step, deficit, tick));
    LS_CHECK_MSG(outcome.ok(), "step=" + std::to_string(step) + " " + outcome.status().to_string());
    if (!outcome.ok()) {
      continue;
    }
    const SheddingPlan& plan = outcome.value().plan;
    const Status verified = plan.verify();
    LS_CHECK_MSG(verified.ok(), "step=" + std::to_string(step) + " " + verified.message());
    // Safety: nothing protected, stale, or unknown is ever selected.
    for (const ShedAction& action : plan.actions) {
      const LoadConsideration* consideration = plan.find_consideration(action.load);
      LS_CHECK(consideration != nullptr);
      if (consideration == nullptr) {
        continue;
      }
      LS_CHECK_EQ(consideration->verdict, LoadVerdict::Selected);
      LS_CHECK_EQ(consideration->evidence_state, EvidenceState::Known);
      LS_CHECK(consideration->contribution.watts() > 0);
    }
  }
  auto report = engine.value().verify();
  LS_CHECK(report.ok());
  LS_CHECK_MSG(report.value().ok, report.value().failures.empty() ? "" : report.value().failures[0].detail);
  LS_CHECK(report.value().plans_verified > 0);
}
