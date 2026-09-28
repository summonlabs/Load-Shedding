// Benchmark: completed shedding operations.
//
// A "completed operation" here is a call to Engine::plan that returns a committed
// plan. That includes everything the operation actually costs: request
// validation, admissibility classification, deterministic ordering, selection,
// checked-integer accounting closure, invariant verification, canonical
// encoding, the staging write, the device flush, read-back verification, the
// atomic publish, the commit-marker commit, the watermark advance, and residue
// retirement. Submission latency is not measured because it is not completion.
//
// Evidence label: REAL. These numbers were produced by running the durable path
// on the host recorded in the report; nothing is simulated or extrapolated.
//
// The benchmark creates its own store under a scratch directory, verifies the
// final state, and removes the residue before it exits.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "load_shedding/engine.hpp"
#include "load_shedding/json_io.hpp"
#include "load_shedding/version.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#endif

using namespace load_shedding;

namespace {

struct Timing {
  double median_ms = 0;
  double mean_ms = 0;
  double min_ms = 0;
  double max_ms = 0;
  double operations_per_second = 0;
};

Timing summarise(std::vector<double> samples, std::size_t operations_per_sample) {
  Timing timing;
  if (samples.empty()) {
    return timing;
  }
  std::sort(samples.begin(), samples.end());
  timing.median_ms = samples[samples.size() / 2];
  timing.min_ms = samples.front();
  timing.max_ms = samples.back();
  double total = 0;
  for (const double sample : samples) {
    total += sample;
  }
  timing.mean_ms = total / static_cast<double>(samples.size());
  if (timing.median_ms > 0) {
    const double operations = static_cast<double>(operations_per_sample);
    timing.operations_per_second = operations / (timing.median_ms / 1000.0);
  }
  return timing;
}

std::string host_description() {
#if defined(_WIN32)
  SYSTEM_INFO info{};
  ::GetNativeSystemInfo(&info);
  const char* architecture = "unknown";
  switch (info.wProcessorArchitecture) {
    case PROCESSOR_ARCHITECTURE_AMD64: architecture = "x86_64"; break;
    case PROCESSOR_ARCHITECTURE_ARM64: architecture = "arm64"; break;
    case PROCESSOR_ARCHITECTURE_INTEL: architecture = "x86"; break;
    default: break;
  }
  return std::string("windows/") + architecture + "/msvc-" + std::to_string(_MSC_VER);
#else
  return "posix";
#endif
}

FacilitySnapshot build_facility(std::size_t loads, std::uint64_t generation, std::uint64_t tick) {
  FacilitySnapshot snapshot;
  snapshot.id = SnapshotId::from_value(generation);
  snapshot.generation = EvidenceGeneration::from_value(generation);
  snapshot.tick = Tick::from_value(tick);
  std::int64_t total = 0;
  for (std::size_t index = 0; index < loads; ++index) {
    LoadRecord load;
    const std::string ref = "load-" + std::to_string(index);
    load.ref = LoadRef::parse(ref).value();
    load.identity_generation = LoadGeneration::from_value(index + 1);
    load.priority = static_cast<PriorityClass>(index % 6);
    load.load_class = static_cast<LoadClass>((index % 3) + 1);
    load.contribution.state = EvidenceState::Known;
    load.contribution.value = Power::from_watts(static_cast<std::int64_t>(1000 + (index % 50) * 500));
    load.contribution.generation = snapshot.generation;
    load.contribution.tick = snapshot.tick;
    snapshot.loads.push_back(load);
    total += load.contribution.value.watts();
  }
  snapshot.total_demand.state = EvidenceState::Known;
  snapshot.total_demand.value = Power::from_watts(total);
  snapshot.total_demand.generation = snapshot.generation;
  snapshot.total_demand.tick = snapshot.tick;
  return snapshot;
}

struct Options {
  std::size_t loads = 256;
  std::size_t operations = 200;
  std::size_t repetitions = 5;
  std::int64_t deficit = 250000;
  std::string store;
};

Options parse(int argc, char** argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    const auto next = [&index, argc, argv]() -> std::string {
      return index + 1 < argc ? argv[++index] : std::string();
    };
    if (argument == "--loads") {
      options.loads = std::strtoull(next().c_str(), nullptr, 10);
    } else if (argument == "--operations") {
      options.operations = std::strtoull(next().c_str(), nullptr, 10);
    } else if (argument == "--repetitions") {
      options.repetitions = std::strtoull(next().c_str(), nullptr, 10);
    } else if (argument == "--deficit") {
      options.deficit = std::strtoll(next().c_str(), nullptr, 10);
    } else if (argument == "--store") {
      options.store = next();
    }
  }
  return options;
}

}  // namespace

int main(int argc, char** argv) {
  const Options options = parse(argc, argv);
  const std::string store =
      options.store.empty()
          ? (std::filesystem::temp_directory_path() / "load-shedding-benchmark").string()
          : options.store;
  std::error_code error;
  std::filesystem::remove_all(store, error);

  EngineOptions engine_options;
  engine_options.store_directory = store;
  engine_options.incarnation = Incarnation::from_value(1);
  engine_options.take_authority = true;
  engine_options.create_if_missing = true;
  engine_options.max_plans_retained = 8;
  engine_options.max_idempotency_entries = 8;
  auto engine = Engine::open(engine_options);
  if (!engine.ok()) {
    std::cerr << "setup failed: " << engine.status().to_string() << "\n";
    return 1;
  }
  const AuthorityStatus authority = engine.value().authority();
  if (!engine.value().install_policy(make_default_policy(), authority.epoch, authority.incarnation).ok()) {
    std::cerr << "policy install failed\n";
    return 1;
  }

  std::cout << "load-shedding plan benchmark\n";
  std::cout << "  evidence label     : REAL (durable path measured on this host)\n";
  std::cout << "  library version    : " << Version::string << "\n";
  std::cout << "  host               : " << host_description() << "\n";
  std::cout << "  loads              : " << options.loads << "\n";
  std::cout << "  operations per rep : " << options.operations << "\n";
  std::cout << "  repetitions        : " << options.repetitions << "\n";
  std::cout << "  requested deficit  : " << options.deficit << " W\n";

  // Setup is excluded from the measurement, but it is verified: a benchmark that
  // measures a path that is not working proves nothing.
  std::uint64_t generation = 1;
  std::uint64_t tick = 1000;
  std::vector<double> samples;
  std::uint64_t request_id = 0;
  std::uint64_t state_bytes = 0;
  std::uint64_t commit_generation = 0;

  for (std::size_t repetition = 0; repetition < options.repetitions; ++repetition) {
    auto snapshot = engine.value().replace_snapshot(build_facility(options.loads, generation, tick),
                                                    authority.epoch, authority.incarnation);
    if (!snapshot.ok()) {
      std::cerr << "snapshot install failed: " << snapshot.status().to_string() << "\n";
      return 1;
    }
    const auto started = std::chrono::steady_clock::now();
    for (std::size_t operation = 0; operation < options.operations; ++operation) {
      ++request_id;
      ++tick;
      PlanRequest request;
      request.request_id = RequestId::from_value(request_id);
      request.requested_reduction = Power::from_watts(options.deficit);
      request.policy_generation = engine.value().policy_generation();
      request.evidence_generation = engine.value().evidence_generation();
      request.effect_generation = engine.value().effect_generation();
      request.base_revision = engine.value().revision();
      request.authority_epoch = authority.epoch;
      request.incarnation = authority.incarnation;
      request.tick = Tick::from_value(tick);
      auto outcome = engine.value().plan(request);
      if (!outcome.ok()) {
        std::cerr << "operation " << request_id << " failed: " << outcome.status().to_string() << "\n";
        std::filesystem::remove_all(store, error);
        return 1;
      }
      if (operation + 1 == options.operations) {
        commit_generation = engine.value().commit().generation;
        auto bytes = engine.value().canonical_state_bytes();
        state_bytes = bytes.ok() ? bytes.value().size() : 0;
      }
    }
    const auto finished = std::chrono::steady_clock::now();
    const double elapsed_ms =
        std::chrono::duration<double, std::milli>(finished - started).count();
    samples.push_back(elapsed_ms);
    ++generation;
  }

  auto report = engine.value().verify();
  if (!report.ok() || !report.value().ok) {
    std::cerr << "the measured state does not verify; refusing to report a benchmark\n";
    std::filesystem::remove_all(store, error);
    return 1;
  }
  auto audit = engine.value().audit_store();
  if (!audit.ok() || !audit.value().clean) {
    std::cerr << "the measured store is not clean; refusing to report a benchmark\n";
    std::filesystem::remove_all(store, error);
    return 1;
  }

  const Timing timing = summarise(samples, options.operations);
  std::cout << "\nresults (one sample = " << options.operations << " completed operations)\n";
  std::cout << "  median             : " << timing.median_ms << " ms  => "
            << timing.operations_per_second << " completed plans/s\n";
  std::cout << "  mean               : " << timing.mean_ms << " ms\n";
  std::cout << "  min / max          : " << timing.min_ms << " / " << timing.max_ms << " ms\n";
  std::cout << "  per-operation      : " << (timing.median_ms / static_cast<double>(options.operations))
            << " ms (median sample, including validation, canonical encoding, staging write, flush, "
               "read-back verification, atomic publish, marker commit, watermark advance)\n";
  std::cout << "  final generation   : " << commit_generation << "\n";
  std::cout << "  canonical state    : " << state_bytes << " bytes\n";
  std::cout << "  plans verified     : " << report.value().plans_verified << "\n";
  std::cout << "  store audit        : clean\n";

  std::filesystem::remove_all(store, error);
  return 0;
}
