// Example: the complete shedding lifecycle.
//
// Installs a policy and facility evidence, plans a deficit that is fully covered,
// records what a downstream adapter reported, and then restores service in the
// documented order once capacity returns.
//
// SYNTHETIC: the effect records below come from a deterministic simulator. They
// demonstrate the control semantics; they are not hardware evidence.

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>

#include "load_shedding/engine.hpp"
#include "load_shedding/json_io.hpp"

namespace {

using namespace load_shedding;

void print(const std::string& label, const JsonValue& value) {
  std::cout << "--- " << label << " ---\n" << to_pretty_json(value);
}

FacilitySnapshot make_facility(std::uint64_t generation, std::uint64_t tick) {
  FacilitySnapshot snapshot;
  snapshot.id = SnapshotId::first();
  snapshot.generation = EvidenceGeneration::from_value(generation);
  snapshot.tick = Tick::from_value(tick);
  const struct {
    const char* ref;
    std::int64_t watts;
    PriorityClass priority;
    LoadClass load_class;
  } kLoads[] = {
      {"life-safety-panel", 60000, PriorityClass::Critical, LoadClass::Protected},
      {"hall-lighting", 25000, PriorityClass::Optional, LoadClass::Sheddable},
      {"office-hvac", 90000, PriorityClass::Standard, LoadClass::Sheddable},
      {"batch-lab", 120000, PriorityClass::Deferrable, LoadClass::Deferrable},
  };
  for (const auto& item : kLoads) {
    LoadRecord load;
    load.ref = LoadRef::parse(item.ref).value();
    load.identity_generation = LoadGeneration::from_value(snapshot.loads.size() + 1);
    load.priority = item.priority;
    load.load_class = item.load_class;
    load.contribution.state = EvidenceState::Known;
    load.contribution.value = Power::from_watts(item.watts);
    load.contribution.generation = snapshot.generation;
    load.contribution.tick = snapshot.tick;
    snapshot.loads.push_back(load);
  }
  ProtectedObligation obligation;
  obligation.ref = ObligationRef::parse("keep-life-safety").value();
  obligation.load = LoadRef::parse("life-safety-panel").value();
  obligation.reserved = Power::from_watts(60000);
  obligation.generation = snapshot.generation;
  obligation.tick = snapshot.tick;
  snapshot.obligations.push_back(obligation);
  snapshot.total_demand.state = EvidenceState::Known;
  snapshot.total_demand.value = Power::from_watts(295000);
  snapshot.total_demand.generation = snapshot.generation;
  snapshot.total_demand.tick = snapshot.tick;
  return snapshot;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string store =
      argc > 1 ? argv[1]
               : (std::filesystem::temp_directory_path() / "load-shedding-example-lifecycle").string();
  std::error_code error;
  std::filesystem::remove_all(store, error);

  EngineOptions options;
  options.store_directory = store;
  options.incarnation = Incarnation::from_value(11);
  options.take_authority = true;
  options.create_if_missing = true;
  auto engine = Engine::open(options);
  if (!engine.ok()) {
    std::cerr << engine.status().to_string() << "\n";
    return 1;
  }
  const AuthorityStatus authority = engine.value().authority();

  auto policy_generation = engine.value().install_policy(make_default_policy(), authority.epoch,
                                                         authority.incarnation);
  if (!policy_generation.ok()) {
    std::cerr << policy_generation.status().to_string() << "\n";
    return 1;
  }
  auto snapshot_revision = engine.value().replace_snapshot(make_facility(1, 100), authority.epoch,
                                                           authority.incarnation);
  if (!snapshot_revision.ok()) {
    std::cerr << snapshot_revision.status().to_string() << "\n";
    return 1;
  }
  print("policy", to_json(engine.value().policy()));

  PlanRequest request;
  request.request_id = RequestId::first();
  request.requested_reduction = Power::from_watts(110000);
  request.policy_generation = engine.value().policy_generation();
  request.evidence_generation = engine.value().evidence_generation();
  request.effect_generation = engine.value().effect_generation();
  request.base_revision = engine.value().revision();
  request.authority_epoch = authority.epoch;
  request.incarnation = authority.incarnation;
  request.tick = Tick::from_value(101);
  auto outcome = engine.value().plan(request);
  if (!outcome.ok()) {
    std::cerr << outcome.status().to_string() << "\n";
    return 1;
  }
  const SheddingPlan plan = outcome.value().plan;
  std::cout << "\nplan " << plan.identity.plan_id.value() << ": requested "
            << plan.closure.requested_reduction.to_string() << ", selected "
            << plan.closure.selected_expected_reduction.to_string() << ", residual "
            << plan.closure.residual_deficit.to_string() << ", overshoot "
            << plan.closure.overshoot.to_string() << ", coverage "
            << to_string(plan.coverage) << "\n";
  for (const ShedAction& action : plan.actions) {
    std::cout << "  stage " << action.stage.value() << ": shed " << action.load.value() << " for "
              << action.expected_contribution.to_string() << " (" << to_string(action.reason) << ")\n";
  }
  print("explanation trace", to_json(plan).find("considerations") == nullptr
                                ? JsonValue::array()
                                : *to_json(plan).find("considerations"));

  // A downstream adapter reports what it actually did. This is evidence, not
  // authority: the plan above is unchanged by it.
  for (const ShedAction& action : plan.actions) {
    EffectObservationRequest observation;
    observation.request_id = RequestId::from_value(action.selection_index + 2);
    observation.plan_id = plan.identity.plan_id;
    observation.plan_generation = plan.identity.generation;
    observation.load = action.load;
    observation.attempt = AttemptId::from_value(action.selection_index + 1);
    observation.observed = EffectState::Shed;
    observation.source = EffectSource::Synthetic;
    observation.verification = EffectVerification::Confirmed;
    observation.tick = Tick::from_value(102 + action.selection_index);
    observation.base_revision = engine.value().revision();
    observation.authority_epoch = authority.epoch;
    observation.incarnation = authority.incarnation;
    auto recorded = engine.value().observe_effect(observation);
    if (!recorded.ok()) {
      std::cerr << recorded.status().to_string() << "\n";
      return 1;
    }
  }
  auto stored_plan = engine.value().plan_by_id(plan.identity.plan_id);
  std::cout << "\nplan digest before and after the effect records: "
            << (stored_plan.ok() && stored_plan.value().content_digest() == plan.content_digest()
                    ? "unchanged"
                    : "CHANGED")
            << "\n";

  // Capacity returns: the recovery decision is a separate decision with its own
  // documented order.
  RecoveryRequest recovery;
  recovery.request_id = RequestId::from_value(50);
  recovery.plan_id = plan.identity.plan_id;
  recovery.plan_generation = plan.identity.generation;
  recovery.available_headroom = Power::from_watts(500000);
  recovery.policy_generation = engine.value().policy_generation();
  recovery.evidence_generation = engine.value().evidence_generation();
  recovery.effect_generation = engine.value().effect_generation();
  recovery.base_revision = engine.value().revision();
  recovery.authority_epoch = authority.epoch;
  recovery.incarnation = authority.incarnation;
  recovery.tick = Tick::from_value(200);
  auto decision = engine.value().recovery(recovery);
  if (!decision.ok()) {
    std::cerr << decision.status().to_string() << "\n";
    return 1;
  }
  std::cout << "\nrecovery order (" << to_string(decision.value().order) << "):\n";
  for (const RecoveryCandidate& candidate : decision.value().candidates) {
    if (!candidate.order_index_set) {
      continue;
    }
    std::cout << "  " << candidate.order_index << ": " << candidate.load.value() << " "
              << candidate.contribution.to_string() << " -> " << to_string(candidate.verdict) << "\n";
  }
  std::cout << "restored " << decision.value().restored_expected.to_string() << ", headroom left "
            << decision.value().remaining_headroom.to_string() << "\n";

  auto report = engine.value().verify();
  if (!report.ok() || !report.value().ok) {
    std::cerr << "verification failed\n";
    return 1;
  }
  print("verification", to_json(report.value()));
  std::filesystem::remove_all(store, error);
  return 0;
}
