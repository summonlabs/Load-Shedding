// Independent downstream consumer of the installed Load Shedding package.
//
// It exercises the public API the way a facility controller would: install a
// policy and evidence, ask for a shedding plan, record the effect a downstream
// adapter reported, ask for a recovery order, and re-check the plan against
// current state. Nothing here reaches into the library's internals.

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>

#include "load_shedding/engine.hpp"
#include "load_shedding/json_io.hpp"
#include "load_shedding/version.hpp"

namespace {

using namespace load_shedding;

int fail(const Status& status) {
  std::cerr << "consumer failed: " << status.to_string() << "\n";
  return 1;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string store =
      argc > 1 ? argv[1]
               : (std::filesystem::temp_directory_path() / "load-shedding-consumer").string();
  std::error_code error;
  std::filesystem::remove_all(store, error);

  std::cout << "LoadShedding " << Version::string << " consumer\n";

  EngineOptions options;
  options.store_directory = store;
  options.incarnation = Incarnation::from_value(7);
  options.take_authority = true;
  options.create_if_missing = true;

  auto engine = Engine::open(options);
  if (!engine.ok()) {
    return fail(engine.status());
  }
  const AuthorityStatus authority = engine.value().authority();

  SheddingPolicy policy = make_default_policy();
  auto policy_generation = engine.value().install_policy(policy, authority.epoch, authority.incarnation);
  if (!policy_generation.ok()) {
    return fail(policy_generation.status());
  }

  FacilitySnapshot snapshot;
  snapshot.id = SnapshotId::first();
  snapshot.generation = EvidenceGeneration::first();
  snapshot.tick = Tick::from_value(10);
  for (const auto& item : {std::pair<const char*, std::int64_t>{"pump-a", 250000},
                           {"pump-b", 250000},
                           {"lab-lighting", 40000}}) {
    LoadRecord load;
    load.ref = LoadRef::parse(item.first).value();
    load.identity_generation = LoadGeneration::from_value(snapshot.loads.size() + 1);
    load.priority = std::string(item.first) == "lab-lighting" ? PriorityClass::Optional
                                                              : PriorityClass::Standard;
    load.contribution.state = EvidenceState::Known;
    load.contribution.value = Power::from_watts(item.second);
    load.contribution.generation = snapshot.generation;
    load.contribution.tick = snapshot.tick;
    snapshot.loads.push_back(load);
  }
  snapshot.total_demand.state = EvidenceState::Known;
  snapshot.total_demand.value = Power::from_watts(540000);
  snapshot.total_demand.generation = snapshot.generation;
  snapshot.total_demand.tick = snapshot.tick;
  auto installed = engine.value().replace_snapshot(snapshot, authority.epoch, authority.incarnation);
  if (!installed.ok()) {
    return fail(installed.status());
  }

  PlanRequest request;
  request.request_id = RequestId::first();
  request.requested_reduction = Power::from_watts(200000);
  request.policy_generation = engine.value().policy_generation();
  request.evidence_generation = engine.value().evidence_generation();
  request.effect_generation = engine.value().effect_generation();
  request.base_revision = engine.value().revision();
  request.authority_epoch = authority.epoch;
  request.incarnation = authority.incarnation;
  request.tick = Tick::from_value(11);
  auto outcome = engine.value().plan(request);
  if (!outcome.ok()) {
    return fail(outcome.status());
  }
  const SheddingPlan plan = outcome.value().plan;
  std::cout << "plan " << plan.identity.plan_id.value() << " coverage "
            << to_string(plan.coverage) << " selected "
            << plan.closure.selected_expected_reduction.watts() << "W residual "
            << plan.closure.residual_deficit.watts() << "W digest "
            << plan.content_digest().hex().substr(0, 16) << "...\n";

  const Status verified = plan.verify();
  if (!verified.ok()) {
    return fail(verified);
  }

  RecoveryRequest recovery;
  recovery.request_id = RequestId::from_value(2);
  recovery.plan_id = plan.identity.plan_id;
  recovery.plan_generation = plan.identity.generation;
  recovery.available_headroom = Power::from_watts(500000);
  recovery.policy_generation = engine.value().policy_generation();
  recovery.evidence_generation = engine.value().evidence_generation();
  recovery.effect_generation = engine.value().effect_generation();
  recovery.base_revision = engine.value().revision();
  recovery.authority_epoch = authority.epoch;
  recovery.incarnation = authority.incarnation;
  recovery.tick = Tick::from_value(12);
  auto decision = engine.value().recovery(recovery);
  if (!decision.ok()) {
    return fail(decision.status());
  }
  std::cout << "recovery restores " << decision.value().restored_expected.watts()
            << "W across " << decision.value().restored_count << " load(s)\n";

  auto report = engine.value().verify();
  if (!report.ok()) {
    return fail(report.status());
  }
  if (!report.value().ok) {
    std::cerr << "consumer verify reported failures\n";
    return 1;
  }
  std::cout << "verify ok: " << to_canonical_json(to_json(report.value())) << "\n";

  std::cout << "consumer run completed\n";
  std::filesystem::remove_all(store, error);
  return 0;
}
