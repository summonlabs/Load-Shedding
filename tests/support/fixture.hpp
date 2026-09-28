#pragma once

// Deterministic fixtures for the suite. Everything here builds real library
// values; nothing reimplements library behaviour.

#include <cstdint>
#include <string>
#include <vector>

#include "load_shedding/engine.hpp"
#include "load_shedding/json_io.hpp"

namespace ls_test {

/// A scratch directory that removes itself. Every store the suite creates lives
/// under one of these, so a passing run leaves no residue behind.
class ScratchDirectory {
 public:
  explicit ScratchDirectory(const std::string& label);
  ~ScratchDirectory();
  ScratchDirectory(const ScratchDirectory&) = delete;
  ScratchDirectory& operator=(const ScratchDirectory&) = delete;

  const std::string& path() const noexcept { return path_; }
  /// A path inside the scratch directory that does not exist yet.
  std::string child(const std::string& name) const;
  /// Removes the directory and everything under it. Safe to call twice.
  void remove_now();
  /// Forgets the directory without removing it, so a failing run can be
  /// inspected. Only used by tests that intentionally leave residue.
  void release() noexcept { keep_ = true; }

 private:
  std::string path_;
  bool keep_ = false;
};

/// Root under which every scratch directory is created.
std::string scratch_root();

/// Builds loads for a facility snapshot.
class FacilityBuilder {
 public:
  FacilityBuilder& load(std::string ref, std::int64_t watts,
                        load_shedding::PriorityClass priority = load_shedding::PriorityClass::Standard,
                        load_shedding::LoadClass load_class = load_shedding::LoadClass::Sheddable);
  FacilityBuilder& unknown_load(std::string ref,
                                load_shedding::EvidenceState state =
                                    load_shedding::EvidenceState::Unknown,
                                load_shedding::PriorityClass priority =
                                    load_shedding::PriorityClass::Standard);
  FacilityBuilder& out_of_service(std::string ref, std::int64_t watts,
                                  load_shedding::PriorityClass priority =
                                      load_shedding::PriorityClass::Standard);
  FacilityBuilder& stale_load(std::string ref, std::int64_t watts, std::uint64_t evidence_tick,
                              load_shedding::PriorityClass priority =
                                  load_shedding::PriorityClass::Standard);
  FacilityBuilder& obligation(std::string ref, std::string load_ref, std::int64_t reserved_watts,
                              bool active = true);
  FacilityBuilder& total_demand(std::int64_t watts);
  FacilityBuilder& evidence_generation(std::uint64_t generation);
  FacilityBuilder& tick(std::uint64_t tick);
  /// Stamps each load's identity generation from the canonical load order.
  /// Disable when a test needs to control the value itself.
  FacilityBuilder& stamp_identity_generations(bool enabled) {
    stamp_identity_ = enabled;
    return *this;
  }

  load_shedding::FacilitySnapshot build() const;

 private:
  load_shedding::FacilitySnapshot snapshot_;
  bool demand_known_ = false;
  bool stamp_identity_ = true;
  std::int64_t total_demand_watts_ = 0;
};

/// Opens a writer engine on `directory`.
load_shedding::Result<load_shedding::Engine> open_writer(const std::string& directory,
                                                         std::uint64_t incarnation = 1,
                                                         std::uint64_t tick = 0);

/// Opens a read-only engine on `directory`.
load_shedding::Result<load_shedding::Engine> open_reader(const std::string& directory);

/// Installs `policy` through the engine's own authority-checked mutation.
load_shedding::Result<load_shedding::PolicyGeneration> install_policy(
    load_shedding::Engine& engine, const load_shedding::SheddingPolicy& policy);

/// Installs a whole facility snapshot.
load_shedding::Result<load_shedding::StateRevision> install_snapshot(
    load_shedding::Engine& engine, const load_shedding::FacilitySnapshot& snapshot);

/// Builds a plan request against the engine's current generations.
load_shedding::PlanRequest plan_request(const load_shedding::Engine& engine,
                                        std::uint64_t request_id, std::int64_t deficit_watts,
                                        std::uint64_t tick);

/// Builds a recovery request against the engine's current generations.
load_shedding::RecoveryRequest recovery_request(const load_shedding::Engine& engine,
                                                std::uint64_t request_id,
                                                load_shedding::PlanId plan, std::int64_t headroom_watts,
                                                std::uint64_t tick);

/// Builds an effect request against the engine's current generations.
load_shedding::EffectObservationRequest effect_request(
    const load_shedding::Engine& engine, std::uint64_t request_id, load_shedding::PlanId plan,
    std::string load_ref, load_shedding::EffectState state, std::uint64_t tick,
    load_shedding::EffectVerification verification = load_shedding::EffectVerification::Confirmed,
    load_shedding::EffectSource source = load_shedding::EffectSource::Synthetic);

/// Byte-exact copy of a file, or a failure describing why it could not be read.
load_shedding::Result<std::string> read_bytes(const std::string& path);

/// Overwrites a file with the exact bytes given.
load_shedding::Status write_bytes(const std::string& path, const std::string& content);

/// Removes a file, reporting a failure when it cannot be removed.
load_shedding::Status remove_file_for_test(const std::string& path);

/// Every file name in a directory, sorted.
std::vector<std::string> list_names(const std::string& directory);

/// Path of the test helper executable that performs crash and multiprocess work.
std::string helper_executable();

/// Path of the command line tool built alongside the suite.
std::string cli_executable();

}  // namespace ls_test
