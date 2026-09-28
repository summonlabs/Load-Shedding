// Example: a deficit that cannot be covered, with protected obligations and an
// unknown contribution in the mix.
//
// The plan reports the residual it cannot cover and marks the result
// indeterminate, because one admissible load's contribution was never measured.
// Nothing is invented to "balance" the deficit.

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>

#include "load_shedding/engine.hpp"
#include "load_shedding/json_io.hpp"

using namespace load_shedding;

int main(int argc, char** argv) {
  const std::string store =
      argc > 1 ? argv[1]
               : (std::filesystem::temp_directory_path() / "load-shedding-example-unknown").string();
  std::error_code error;
  std::filesystem::remove_all(store, error);

  EngineOptions options;
  options.store_directory = store;
  options.incarnation = Incarnation::from_value(21);
  auto engine = Engine::open(options);
  if (!engine.ok()) {
    std::cerr << engine.status().to_string() << "\n";
    return 1;
  }
  const AuthorityStatus authority = engine.value().authority();
  if (!engine.value().install_policy(make_default_policy(), authority.epoch, authority.incarnation).ok()) {
    std::cerr << "policy install failed\n";
    return 1;
  }

  FacilitySnapshot snapshot;
  snapshot.id = SnapshotId::first();
  snapshot.generation = EvidenceGeneration::first();
  snapshot.tick = Tick::from_value(10);
  const auto add_known = [&snapshot](const char* ref, std::int64_t watts, PriorityClass priority,
                                     LoadClass load_class) {
    LoadRecord load;
    load.ref = LoadRef::parse(ref).value();
    load.identity_generation = LoadGeneration::from_value(snapshot.loads.size() + 1);
    load.priority = priority;
    load.load_class = load_class;
    load.contribution.state = EvidenceState::Known;
    load.contribution.value = Power::from_watts(watts);
    load.contribution.generation = snapshot.generation;
    load.contribution.tick = snapshot.tick;
    snapshot.loads.push_back(load);
  };
  add_known("data-hall-critical", 150000, PriorityClass::Critical, LoadClass::Protected);
  add_known("office-hvac", 40000, PriorityClass::Standard, LoadClass::Sheddable);
  LoadRecord unmeasured;
  unmeasured.ref = LoadRef::parse("legacy-chiller").value();
  unmeasured.identity_generation = LoadGeneration::from_value(9);
  unmeasured.priority = PriorityClass::Standard;
  unmeasured.contribution.state = EvidenceState::Unknown;
  unmeasured.contribution.generation = snapshot.generation;
  unmeasured.contribution.tick = snapshot.tick;
  snapshot.loads.push_back(unmeasured);
  ProtectedObligation obligation;
  obligation.ref = ObligationRef::parse("data-hall-floor").value();
  obligation.load = LoadRef::parse("data-hall-critical").value();
  obligation.reserved = Power::from_watts(150000);
  obligation.generation = snapshot.generation;
  obligation.tick = snapshot.tick;
  snapshot.obligations.push_back(obligation);
  snapshot.total_demand.state = EvidenceState::Known;
  snapshot.total_demand.value = Power::from_watts(190000);
  snapshot.total_demand.generation = snapshot.generation;
  snapshot.total_demand.tick = snapshot.tick;
  if (!engine.value().replace_snapshot(snapshot, authority.epoch, authority.incarnation).ok()) {
    std::cerr << "snapshot install failed\n";
    return 1;
  }

  PlanRequest request;
  request.request_id = RequestId::first();
  request.requested_reduction = Power::from_watts(120000);
  request.policy_generation = engine.value().policy_generation();
  request.evidence_generation = engine.value().evidence_generation();
  request.effect_generation = engine.value().effect_generation();
  request.base_revision = engine.value().revision();
  request.authority_epoch = authority.epoch;
  request.incarnation = authority.incarnation;
  request.tick = Tick::from_value(11);
  auto outcome = engine.value().plan(request);
  if (!outcome.ok()) {
    std::cerr << outcome.status().to_string() << "\n";
    return 1;
  }
  const SheddingPlan plan = outcome.value().plan;
  std::cout << "coverage: " << to_string(plan.coverage) << "\n";
  std::cout << "requested " << plan.closure.requested_reduction.to_string() << ", selected "
            << plan.closure.selected_expected_reduction.to_string() << ", residual "
            << plan.closure.residual_deficit.to_string() << "\n";
  std::cout << "protected " << plan.closure.protected_known_amount.to_string() << ", eligible known "
            << plan.closure.eligible_known_capacity.to_string() << "\n";
  std::cout << "capacity indeterminate: "
            << (plan.closure.capacity_indeterminate ? "yes" : "no") << " ("
            << plan.closure.indeterminate_load_count << " load(s) unmeasured)\n";
  std::cout << "shedding ceiling ";
  if (plan.closure.effective_shedding_ceiling.has_value()) {
    std::cout << plan.closure.effective_shedding_ceiling->to_string() << "\n";
  } else {
    std::cout << "unverified\n";
  }
  for (const LoadConsideration& consideration : plan.considerations) {
    std::cout << "  " << consideration.load.value() << " (" << to_string(consideration.priority)
              << ", " << to_string(consideration.load_class) << "): "
              << to_string(consideration.verdict) << "\n";
  }
  std::filesystem::remove_all(store, error);
  return 0;
}
