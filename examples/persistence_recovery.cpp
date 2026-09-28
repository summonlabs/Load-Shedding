// Example: durable state across process lifetimes.
//
// Commits a plan, closes the store, reopens it read-only and as a writer, and
// shows that the adopted state is exactly the committed generation: same
// canonical bytes for a reader, same plan digest, and a clean store audit.
//
// The crash-injection story is exercised by the automated suite, which kills real
// processes at each durable stage; this example covers the ordinary reopen path
// that operators see.

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>

#include "load_shedding/engine.hpp"
#include "load_shedding/json_io.hpp"

using namespace load_shedding;

namespace {

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
  return snapshot;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string store =
      argc > 1 ? argv[1]
               : (std::filesystem::temp_directory_path() / "load-shedding-example-persist").string();
  std::error_code error;
  std::filesystem::remove_all(store, error);

  Digest committed_digest;
  Digest committed_state;
  std::uint64_t committed_generation = 0;
  {
    EngineOptions options;
    options.store_directory = store;
    options.incarnation = Incarnation::from_value(41);
    auto engine = Engine::open(options);
    if (!engine.ok()) {
      std::cerr << engine.status().to_string() << "\n";
      return 1;
    }
    const AuthorityStatus authority = engine.value().authority();
    if (!engine.value().install_policy(make_default_policy(), authority.epoch, authority.incarnation).ok() ||
        !engine.value().replace_snapshot(facility(1, 10), authority.epoch, authority.incarnation).ok()) {
      std::cerr << "setup failed\n";
      return 1;
    }
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
    auto outcome = engine.value().plan(request);
    if (!outcome.ok()) {
      std::cerr << outcome.status().to_string() << "\n";
      return 1;
    }
    committed_digest = outcome.value().plan.content_digest();
    committed_state = engine.value().state_digest();
    committed_generation = engine.value().commit().generation;
    std::cout << "committed generation " << committed_generation << ", state digest "
              << committed_state.hex().substr(0, 16) << "...\n";
  }

  {
    // A reader adopts the committed generation without taking authority and sees
    // exactly the same canonical bytes.
    EngineOptions options;
    options.store_directory = store;
    options.incarnation = Incarnation::from_value(41);
    options.take_authority = false;
    options.create_if_missing = false;
    auto reader = Engine::open(options);
    if (!reader.ok()) {
      std::cerr << reader.status().to_string() << "\n";
      return 1;
    }
    std::cout << "reader state digest "
              << (reader.value().state_digest() == committed_state ? "matches" : "DIFFERS") << "\n";
    auto plan = reader.value().plan_by_id(PlanId::first());
    std::cout << "reader plan digest "
              << (plan.ok() && plan.value().content_digest() == committed_digest ? "matches"
                                                                                 : "DIFFERS")
              << "\n";
  }

  {
    // A writer reopen resumes the same authority epoch (same incarnation) and
    // keeps the committed plan intact.
    EngineOptions options;
    options.store_directory = store;
    options.incarnation = Incarnation::from_value(41);
    auto writer = Engine::open(options);
    if (!writer.ok()) {
      std::cerr << writer.status().to_string() << "\n";
      return 1;
    }
    std::cout << "writer resumed at authority epoch " << writer.value().authority().epoch.value()
              << ", committed generation " << writer.value().commit().generation << "\n";
    auto plan = writer.value().plan_by_id(PlanId::first());
    std::cout << "writer plan digest "
              << (plan.ok() && plan.value().content_digest() == committed_digest ? "matches"
                                                                                 : "DIFFERS")
              << "\n";
    auto report = writer.value().verify();
    std::cout << "verify: " << (report.ok() && report.value().ok ? "ok" : "FAILED") << "\n";
  }

  auto audit = inspect_store(store);
  if (!audit.ok()) {
    std::cerr << audit.status().to_string() << "\n";
    return 1;
  }
  std::cout << "store audit clean: " << (audit.value().clean ? "yes" : "no")
            << ", committed generation " << audit.value().commit.generation
            << ", plans retained " << audit.value().plan_count << "\n";
  std::filesystem::remove_all(store, error);
  return 0;
}
