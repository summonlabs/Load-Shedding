// Test helper process.
//
// Every mode is a real process doing real work against the real library: opening
// a store, taking writer authority, committing generations, and (for the crash
// modes) terminating itself at a named durable stage. Crash termination uses the
// platform's immediate process-termination primitive, so no unwinding happens,
// no atexit handler runs, and no interactive error-reporting path is reachable.

#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#include "fixture.hpp"
#include "load_shedding/engine.hpp"
#include "load_shedding/json_io.hpp"

namespace {

using load_shedding::Engine;
using load_shedding::EngineOptions;
using load_shedding::Incarnation;
using load_shedding::Power;
using load_shedding::Tick;

int usage() {
  std::cerr << "helper <mode> ...\n"
               "  crash <store> <crash-point> <incarnation> <tick> <deficit>\n"
               "  hold <store> <incarnation>\n"
               "  write <store> <incarnation> <tick> <ref> <watts>\n"
               "  read <store>\n"
               "  race <store> <incarnation> <tick> <ref>\n";
  return 2;
}

std::uint64_t to_uint(const char* text) { return std::strtoull(text, nullptr, 10); }

EngineOptions base_options(const std::string& store, std::uint64_t incarnation) {
  EngineOptions options;
  options.store_directory = store;
  options.incarnation = Incarnation::from_value(incarnation);
  options.take_authority = true;
  options.create_if_missing = true;
  return options;
}

/// Seeds a store so that a crash test has something to commit.
int seed(Engine& engine, std::uint64_t tick) {
  if (!engine.policy_generation().is_set()) {
    load_shedding::SheddingPolicy policy = load_shedding::make_default_policy();
    const auto authority = engine.authority();
    auto installed = engine.install_policy(policy, authority.epoch, authority.incarnation);
    if (!installed.ok()) {
      std::cerr << installed.status().to_string() << "\n";
      return 1;
    }
  }
  load_shedding::FacilitySnapshot snapshot;
  snapshot.id = load_shedding::SnapshotId::first();
  snapshot.generation = load_shedding::EvidenceGeneration::from_value(
      engine.evidence_generation().value() + 1);
  snapshot.tick = Tick::from_value(tick);
  load_shedding::LoadRecord load;
  load.ref = load_shedding::LoadRef::parse("crash-load").value();
  load.identity_generation = load_shedding::LoadGeneration::first();
  load.contribution.state = load_shedding::EvidenceState::Known;
  load.contribution.value = Power::from_watts(500000);
  load.contribution.generation = snapshot.generation;
  load.contribution.tick = snapshot.tick;
  load.in_service = true;
  snapshot.loads.push_back(load);
  snapshot.total_demand.state = load_shedding::EvidenceState::Known;
  snapshot.total_demand.value = Power::from_watts(600000);
  snapshot.total_demand.generation = snapshot.generation;
  snapshot.total_demand.tick = snapshot.tick;
  const auto authority = engine.authority();
  auto installed = engine.replace_snapshot(snapshot, authority.epoch, authority.incarnation);
  if (!installed.ok()) {
    std::cerr << installed.status().to_string() << "\n";
    return 1;
  }
  return 0;
}

int mode_crash(int argc, char** argv) {
  if (argc < 7) {
    return usage();
  }
  const std::string store = argv[2];
  const std::string crash_point = argv[3];
  const std::uint64_t incarnation = to_uint(argv[4]);
  const std::uint64_t tick = to_uint(argv[5]);
  const std::uint64_t deficit = to_uint(argv[6]);

  // Seed first, without fault injection, so that the injected crash lands on the
  // publication of the plan rather than on the publication of the baseline.
  {
    auto engine = Engine::open(base_options(store, incarnation));
    if (!engine.ok()) {
      std::cerr << engine.status().to_string() << "\n";
      return 1;
    }
    if (!engine.value().policy_generation().is_set()) {
      const int seeded = seed(engine.value(), tick > 1 ? tick - 1 : 1);
      if (seeded != 0) {
        return seeded;
      }
    }
  }

  EngineOptions options = base_options(store, incarnation);
  options.crash_after = crash_point;
  auto engine = Engine::open(options);
  if (!engine.ok()) {
    std::cerr << engine.status().to_string() << "\n";
    return 1;
  }
  const auto authority = engine.value().authority();
  load_shedding::PlanRequest request;
  request.request_id = load_shedding::RequestId::from_value(tick);
  request.requested_reduction = Power::from_watts(static_cast<std::int64_t>(deficit));
  request.policy_generation = engine.value().policy_generation();
  request.evidence_generation = engine.value().evidence_generation();
  request.effect_generation = engine.value().effect_generation();
  request.base_revision = engine.value().revision();
  request.authority_epoch = authority.epoch;
  request.incarnation = authority.incarnation;
  request.tick = Tick::from_value(tick);
  auto outcome = engine.value().plan(request);
  if (!outcome.ok()) {
    std::cerr << outcome.status().to_string() << "\n";
    return 1;
  }
  std::cout << "COMMITTED " << outcome.value().plan.identity.plan_id.value() << "\n";
  return 0;
}

int mode_hold(int argc, char** argv) {
  if (argc < 4) {
    return usage();
  }
  auto engine = Engine::open(base_options(argv[2], to_uint(argv[3])));
  if (!engine.ok()) {
    std::cerr << engine.status().to_string() << "\n";
    return 1;
  }
  std::cout << "READY " << engine.value().authority().epoch.value() << std::endl;
  // Blocks until the parent closes the pipe; that is the only signal used.
  std::string line;
  while (std::getline(std::cin, line)) {
    if (line == "exit") {
      break;
    }
  }
  return 0;
}

int mode_write(int argc, char** argv) {
  if (argc < 7) {
    return usage();
  }
  auto engine = Engine::open(base_options(argv[2], to_uint(argv[3])));
  if (!engine.ok()) {
    std::cerr << engine.status().to_string() << "\n";
    return 1;
  }
  const std::uint64_t tick = to_uint(argv[4]);
  const std::string ref = argv[5];
  const std::uint64_t watts = to_uint(argv[6]);
  load_shedding::LoadRecord load;
  auto parsed_ref = load_shedding::LoadRef::parse(ref);
  if (!parsed_ref.ok()) {
    std::cerr << parsed_ref.status().to_string() << "\n";
    return 1;
  }
  load.ref = parsed_ref.value();
  load.identity_generation = load_shedding::LoadGeneration::first();
  load.contribution.state = load_shedding::EvidenceState::Known;
  load.contribution.value = Power::from_watts(static_cast<std::int64_t>(watts));
  load.contribution.tick = Tick::from_value(tick);
  const auto authority = engine.value().authority();
  auto revision = engine.value().upsert_load(load, authority.epoch, authority.incarnation);
  if (!revision.ok()) {
    std::cerr << revision.status().to_string() << "\n";
    return 1;
  }
  std::cout << "WROTE " << revision.value().value() << " "
            << engine.value().commit().generation << "\n";
  return 0;
}

int mode_read(int argc, char** argv) {
  if (argc < 3) {
    return usage();
  }
  EngineOptions options;
  options.store_directory = argv[2];
  options.incarnation = Incarnation::first();
  options.take_authority = false;
  options.create_if_missing = false;
  auto engine = Engine::open(options);
  if (!engine.ok()) {
    std::cerr << engine.status().to_string() << "\n";
    return 1;
  }
  const auto snapshot = engine.value().snapshot();
  std::cout << "STATE generations=" << engine.value().commit().generation
            << " revision=" << engine.value().revision().value()
            << " loads=" << snapshot.loads.size()
            << " plans=" << engine.value().plan_count()
            << " epoch=" << engine.value().authority().epoch.value() << "\n";
  return 0;
}

int mode_race(int argc, char** argv) {
  if (argc < 6) {
    return usage();
  }
  const std::string store = argv[2];
  auto engine = Engine::open(base_options(store, to_uint(argv[3])));
  if (!engine.ok()) {
    std::cout << "DENIED " << load_shedding::to_string(engine.status().code()) << std::endl;
    return 0;
  }
  std::cout << "GRANTED " << engine.value().authority().epoch.value() << " " << std::endl;
  const std::uint64_t tick = to_uint(argv[4]);
  const std::string ref = argv[5];
  load_shedding::LoadRecord load;
  load.ref = load_shedding::LoadRef::parse(ref).value();
  load.identity_generation = load_shedding::LoadGeneration::first();
  load.contribution.state = load_shedding::EvidenceState::Known;
  load.contribution.value = Power::from_watts(1000);
  load.contribution.tick = Tick::from_value(tick);
  const auto authority = engine.value().authority();
  auto revision = engine.value().upsert_load(load, authority.epoch, authority.incarnation);
  if (!revision.ok()) {
    std::cout << "WRITE-DENIED " << load_shedding::to_string(revision.status().code()) << std::endl;
    return 0;
  }
  std::cout << "WROTE " << revision.value().value() << std::endl;
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    return usage();
  }
  const std::string mode = argv[1];
  if (mode == "crash") {
    return mode_crash(argc, argv);
  }
  if (mode == "hold") {
    return mode_hold(argc, argv);
  }
  if (mode == "write") {
    return mode_write(argc, argv);
  }
  if (mode == "read") {
    return mode_read(argc, argv);
  }
  if (mode == "race") {
    return mode_race(argc, argv);
  }
  return usage();
}
