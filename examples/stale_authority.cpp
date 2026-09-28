// Example: the failure paths that keep a stale writer honest.
//
// A plan accepted once must remain replayable even after the state moved on; a
// request that claims superseded authority, policy, or evidence must be refused;
// and a second writer must be refused by the operating system rather than by
// convention.

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>

#include "load_shedding/engine.hpp"
#include "load_shedding/json_io.hpp"

using namespace load_shedding;

namespace {

void report(const char* what, const Status& status) {
  std::cout << what << ": " << (status.ok() ? "accepted" : status.to_string()) << "\n";
}

/// Reports the status of any library result, whatever it carries on success.
template <class T>
void report(const char* what, const Result<T>& result) {
  report(what, result.status());
}

FacilitySnapshot facility(std::uint64_t generation, std::uint64_t tick) {
  FacilitySnapshot snapshot;
  snapshot.id = SnapshotId::from_value(generation);
  snapshot.generation = EvidenceGeneration::from_value(generation);
  snapshot.tick = Tick::from_value(tick);
  LoadRecord load;
  load.ref = LoadRef::parse("sheddable-bank").value();
  load.identity_generation = LoadGeneration::first();
  load.priority = PriorityClass::Standard;
  load.contribution.state = EvidenceState::Known;
  load.contribution.value = Power::from_watts(200000);
  load.contribution.generation = snapshot.generation;
  load.contribution.tick = snapshot.tick;
  snapshot.loads.push_back(load);
  snapshot.total_demand.state = EvidenceState::Known;
  snapshot.total_demand.value = Power::from_watts(250000);
  snapshot.total_demand.generation = snapshot.generation;
  snapshot.total_demand.tick = snapshot.tick;
  return snapshot;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string store =
      argc > 1 ? argv[1]
               : (std::filesystem::temp_directory_path() / "load-shedding-example-stale").string();
  std::error_code error;
  std::filesystem::remove_all(store, error);

  EngineOptions options;
  options.store_directory = store;
  options.incarnation = Incarnation::from_value(31);
  auto engine = Engine::open(options);
  if (!engine.ok()) {
    std::cerr << engine.status().to_string() << "\n";
    return 1;
  }
  AuthorityStatus authority = engine.value().authority();
  report("install policy", engine.value().install_policy(make_default_policy(), authority.epoch,
                                                         authority.incarnation));
  report("install evidence", engine.value().replace_snapshot(facility(1, 10), authority.epoch,
                                                             authority.incarnation));

  PlanRequest request;
  request.request_id = RequestId::first();
  request.requested_reduction = Power::from_watts(50000);
  request.policy_generation = engine.value().policy_generation();
  request.evidence_generation = engine.value().evidence_generation();
  request.effect_generation = engine.value().effect_generation();
  request.base_revision = engine.value().revision();
  request.authority_epoch = authority.epoch;
  request.incarnation = authority.incarnation;
  request.tick = Tick::from_value(11);

  auto accepted = engine.value().plan(request);
  report("plan", accepted.status());
  if (!accepted.ok()) {
    return 1;
  }
  const Digest original_digest = accepted.value().plan.content_digest();

  // A second writer is refused by the operating-system lock, not by convention.
  {
    EngineOptions other = options;
    other.incarnation = Incarnation::from_value(32);
    auto competing = Engine::open(other);
    report("second writer opening the same store", competing.status());
  }

  // Moving evidence forward makes the plan stale for a *new* request identity.
  report("install newer evidence",
         engine.value().replace_snapshot(facility(2, 20), authority.epoch, authority.incarnation));
  PlanRequest stale = request;
  stale.request_id = RequestId::from_value(2);
  stale.tick = Tick::from_value(21);
  report("plan against superseded evidence", engine.value().plan(stale).status());
  stale.evidence_generation = engine.value().evidence_generation();
  stale.base_revision = engine.value().revision();
  report("plan against current evidence", engine.value().plan(stale).status());

  // Replaying the accepted request returns the original decision, byte for byte,
  // even though the state has moved on.
  auto replayed = engine.value().plan(request);
  report("replay of the accepted request", replayed.status());
  if (replayed.ok()) {
    std::cout << "replay returned " << (replayed.value().replayed ? "the prior result" : "a new plan")
              << "; digest "
              << (replayed.value().plan.content_digest() == original_digest ? "unchanged" : "CHANGED")
              << "\n";
  }

  // The same identity with different content is a conflict, never a silent
  // re-plan.
  PlanRequest conflicting = request;
  conflicting.requested_reduction = Power::from_watts(60000);
  report("same identity, different content", engine.value().plan(conflicting).status());

  // Authority is fenced: a request that claims a superseded epoch is refused.
  report("take a new authority epoch",
         engine.value().take_authority(Incarnation::from_value(33), AuthorityEpoch::from_value(9)));
  PlanRequest fenced = request;
  fenced.request_id = RequestId::from_value(3);
  fenced.authority_epoch = authority.epoch;
  fenced.incarnation = authority.incarnation;
  fenced.evidence_generation = engine.value().evidence_generation();
  fenced.base_revision = engine.value().revision();
  fenced.tick = Tick::from_value(22);
  report("request claiming the fenced epoch", engine.value().plan(fenced).status());

  // A fresh epoch succeeds.
  const AuthorityStatus current = engine.value().authority();
  fenced.authority_epoch = current.epoch;
  fenced.incarnation = current.incarnation;
  report("request claiming the current epoch", engine.value().plan(fenced).status());

  std::filesystem::remove_all(store, error);
  return 0;
}
