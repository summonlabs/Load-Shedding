#include "fixture.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace ls_test {
namespace {

/// Reads an environment variable without tripping the MSVC deprecation of
/// `std::getenv`, and without suppressing the warning.
std::string environment_value(const char* name) {
#if defined(_WIN32)
  char* buffer = nullptr;
  std::size_t size = 0;
  if (::_dupenv_s(&buffer, &size, name) != 0 || buffer == nullptr) {
    return std::string();
  }
  std::string value(buffer);
  std::free(buffer);
  return value;
#else
  const char* value = std::getenv(name);
  return value == nullptr ? std::string() : std::string(value);
#endif
}

std::uint64_t next_scratch_serial() {
  static std::uint64_t serial = 0;
  return ++serial;
}

void remove_tree(const std::string& path) {
  std::error_code error;
  std::filesystem::remove_all(std::filesystem::path(path), error);
}

}  // namespace

std::string scratch_root() {
  const std::string configured = environment_value("LOAD_SHEDDING_TEST_SCRATCH");
  if (!configured.empty()) {
    std::filesystem::create_directories(configured);
    return configured;
  }
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "load-shedding-tests";
  std::filesystem::create_directories(root);
  return root.string();
}

ScratchDirectory::ScratchDirectory(const std::string& label) {
  path_ = (std::filesystem::path(scratch_root()) /
           (label + "-" + std::to_string(next_scratch_serial())))
              .string();
  // A store left behind by an earlier run would otherwise be adopted, which
  // would make the test depend on history rather than on its own inputs.
  remove_tree(path_);
  std::filesystem::create_directories(path_);
}

ScratchDirectory::~ScratchDirectory() {
  if (!keep_) {
    remove_tree(path_);
  }
}

std::string ScratchDirectory::child(const std::string& name) const {
  return (std::filesystem::path(path_) / name).string();
}

void ScratchDirectory::remove_now() { remove_tree(path_); }

FacilityBuilder& FacilityBuilder::load(std::string ref, std::int64_t watts,
                                       load_shedding::PriorityClass priority,
                                       load_shedding::LoadClass load_class) {
  load_shedding::LoadRecord record;
  record.ref = load_shedding::LoadRef::parse(ref).value();
  record.identity_generation = load_shedding::LoadGeneration::from_value(snapshot_.loads.size() + 1);
  record.load_class = load_class;
  record.priority = priority;
  record.contribution.state = load_shedding::EvidenceState::Known;
  record.contribution.value = load_shedding::Power::from_watts(watts);
  record.contribution.generation = snapshot_.generation;
  record.contribution.tick = snapshot_.tick;
  record.in_service = true;
  snapshot_.loads.push_back(record);
  return *this;
}

FacilityBuilder& FacilityBuilder::unknown_load(std::string ref, load_shedding::EvidenceState state,
                                               load_shedding::PriorityClass priority) {
  load_shedding::LoadRecord record;
  record.ref = load_shedding::LoadRef::parse(ref).value();
  record.identity_generation = load_shedding::LoadGeneration::from_value(snapshot_.loads.size() + 1);
  record.priority = priority;
  record.contribution.state = state;
  record.contribution.generation = snapshot_.generation;
  record.contribution.tick = snapshot_.tick;
  record.in_service = true;
  snapshot_.loads.push_back(record);
  return *this;
}

FacilityBuilder& FacilityBuilder::out_of_service(std::string ref, std::int64_t watts,
                                                 load_shedding::PriorityClass priority) {
  load(std::move(ref), watts, priority);
  snapshot_.loads.back().in_service = false;
  return *this;
}

FacilityBuilder& FacilityBuilder::stale_load(std::string ref, std::int64_t watts,
                                             std::uint64_t evidence_tick,
                                             load_shedding::PriorityClass priority) {
  load(std::move(ref), watts, priority);
  snapshot_.loads.back().contribution.tick = load_shedding::Tick::from_value(evidence_tick);
  return *this;
}

FacilityBuilder& FacilityBuilder::obligation(std::string ref, std::string load_ref,
                                             std::int64_t reserved_watts, bool active) {
  load_shedding::ProtectedObligation record;
  record.ref = load_shedding::ObligationRef::parse(ref).value();
  if (!load_ref.empty()) {
    record.load = load_shedding::LoadRef::parse(load_ref).value();
  }
  record.reserved = load_shedding::Power::from_watts(reserved_watts);
  record.active = active;
  record.generation = snapshot_.generation;
  record.tick = snapshot_.tick;
  snapshot_.obligations.push_back(record);
  return *this;
}

FacilityBuilder& FacilityBuilder::total_demand(std::int64_t watts) {
  demand_known_ = true;
  total_demand_watts_ = watts;
  return *this;
}

FacilityBuilder& FacilityBuilder::evidence_generation(std::uint64_t generation) {
  snapshot_.generation = load_shedding::EvidenceGeneration::from_value(generation);
  return *this;
}

FacilityBuilder& FacilityBuilder::tick(std::uint64_t tick) {
  snapshot_.tick = load_shedding::Tick::from_value(tick);
  return *this;
}

load_shedding::FacilitySnapshot FacilityBuilder::build() const {
  load_shedding::FacilitySnapshot snapshot = snapshot_;
  if (snapshot.id.value() == 0) {
    snapshot.id = load_shedding::SnapshotId::first();
  }
  if (demand_known_) {
    snapshot.total_demand.state = load_shedding::EvidenceState::Known;
    snapshot.total_demand.value = load_shedding::Power::from_watts(total_demand_watts_);
    snapshot.total_demand.generation = snapshot.generation;
    snapshot.total_demand.tick = snapshot.tick;
  } else {
    snapshot.total_demand.state = load_shedding::EvidenceState::Unknown;
    snapshot.total_demand.value = load_shedding::Power::zero();
    snapshot.total_demand.generation = snapshot.generation;
    snapshot.total_demand.tick = snapshot.tick;
  }
  std::sort(snapshot.loads.begin(), snapshot.loads.end(),
            [](const load_shedding::LoadRecord& left, const load_shedding::LoadRecord& right) {
              return left.ref < right.ref;
            });
  // Identity generations are stamped from the canonical order, not from the
  // order the caller happened to add loads: the same set of loads always produces
  // the same records, which is what makes permutation invariance testable.
  if (stamp_identity_) {
    for (std::size_t index = 0; index < snapshot.loads.size(); ++index) {
      snapshot.loads[index].identity_generation = load_shedding::LoadGeneration::from_value(index + 1);
    }
  }
  std::sort(snapshot.obligations.begin(), snapshot.obligations.end(),
            [](const load_shedding::ProtectedObligation& left,
               const load_shedding::ProtectedObligation& right) { return left.ref < right.ref; });
  return snapshot;
}

load_shedding::Result<load_shedding::Engine> open_writer(const std::string& directory,
                                                         std::uint64_t incarnation,
                                                         std::uint64_t tick) {
  load_shedding::EngineOptions options;
  options.store_directory = directory;
  options.incarnation = load_shedding::Incarnation::from_value(incarnation);
  options.take_authority = true;
  options.create_if_missing = true;
  if (tick != 0) {
    options.initial_tick = load_shedding::Tick::from_value(tick);
  }
  return load_shedding::Engine::open(options);
}

load_shedding::Result<load_shedding::Engine> open_reader(const std::string& directory) {
  load_shedding::EngineOptions options;
  options.store_directory = directory;
  options.incarnation = load_shedding::Incarnation::from_value(1);
  options.take_authority = false;
  options.create_if_missing = false;
  return load_shedding::Engine::open(options);
}

load_shedding::Result<load_shedding::PolicyGeneration> install_policy(
    load_shedding::Engine& engine, const load_shedding::SheddingPolicy& policy) {
  const load_shedding::AuthorityStatus authority = engine.authority();
  return engine.install_policy(policy, authority.epoch, authority.incarnation);
}

load_shedding::Result<load_shedding::StateRevision> install_snapshot(
    load_shedding::Engine& engine, const load_shedding::FacilitySnapshot& snapshot) {
  const load_shedding::AuthorityStatus authority = engine.authority();
  return engine.replace_snapshot(snapshot, authority.epoch, authority.incarnation);
}

load_shedding::PlanRequest plan_request(const load_shedding::Engine& engine,
                                        std::uint64_t request_id, std::int64_t deficit_watts,
                                        std::uint64_t tick) {
  const load_shedding::AuthorityStatus authority = engine.authority();
  load_shedding::PlanRequest request;
  request.request_id = load_shedding::RequestId::from_value(request_id);
  request.requested_reduction = load_shedding::Power::from_watts(deficit_watts);
  request.policy_generation = engine.policy_generation();
  request.evidence_generation = engine.evidence_generation();
  request.effect_generation = engine.effect_generation();
  request.base_revision = engine.revision();
  request.authority_epoch = authority.epoch;
  request.incarnation = authority.incarnation;
  request.tick = load_shedding::Tick::from_value(tick);
  return request;
}

load_shedding::RecoveryRequest recovery_request(const load_shedding::Engine& engine,
                                                std::uint64_t request_id,
                                                load_shedding::PlanId plan,
                                                std::int64_t headroom_watts, std::uint64_t tick) {
  const load_shedding::AuthorityStatus authority = engine.authority();
  load_shedding::RecoveryRequest request;
  request.request_id = load_shedding::RequestId::from_value(request_id);
  request.plan_id = plan;
  const auto stored = engine.plan_by_id(plan);
  if (stored.ok()) {
    request.plan_generation = stored.value().identity.generation;
  }
  request.available_headroom = load_shedding::Power::from_watts(headroom_watts);
  request.policy_generation = engine.policy_generation();
  request.evidence_generation = engine.evidence_generation();
  request.effect_generation = engine.effect_generation();
  request.base_revision = engine.revision();
  request.authority_epoch = authority.epoch;
  request.incarnation = authority.incarnation;
  request.tick = load_shedding::Tick::from_value(tick);
  return request;
}

load_shedding::EffectObservationRequest effect_request(
    const load_shedding::Engine& engine, std::uint64_t request_id, load_shedding::PlanId plan,
    std::string load_ref, load_shedding::EffectState state, std::uint64_t tick,
    load_shedding::EffectVerification verification, load_shedding::EffectSource source) {
  const load_shedding::AuthorityStatus authority = engine.authority();
  load_shedding::EffectObservationRequest request;
  request.request_id = load_shedding::RequestId::from_value(request_id);
  request.plan_id = plan;
  const auto stored = engine.plan_by_id(plan);
  if (stored.ok()) {
    request.plan_generation = stored.value().identity.generation;
  }
  request.load = load_shedding::LoadRef::parse(load_ref).value();
  request.attempt = load_shedding::AttemptId::from_value(1);
  request.observed = state;
  request.source = source;
  request.verification = verification;
  request.tick = load_shedding::Tick::from_value(tick);
  request.base_revision = engine.revision();
  request.authority_epoch = authority.epoch;
  request.incarnation = authority.incarnation;
  return request;
}

load_shedding::Result<std::string> read_bytes(const std::string& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return load_shedding::Status::error(load_shedding::StatusCode::NotFound,
                                        "could not open '" + path + "'");
  }
  std::string content((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
  return content;
}

load_shedding::Status write_bytes(const std::string& path, const std::string& content) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream) {
    return load_shedding::Status::error(load_shedding::StatusCode::IoFailure,
                                        "could not open '" + path + "' for writing");
  }
  stream.write(content.data(), static_cast<std::streamsize>(content.size()));
  if (!stream) {
    return load_shedding::Status::error(load_shedding::StatusCode::IoFailure,
                                        "could not write '" + path + "'");
  }
  return load_shedding::Status::success();
}

load_shedding::Status remove_file_for_test(const std::string& path) {
  std::error_code error;
  const bool removed = std::filesystem::remove(std::filesystem::path(path), error);
  if (error || !removed) {
    return load_shedding::Status::error(load_shedding::StatusCode::IoFailure,
                                        "could not remove '" + path + "'");
  }
  return load_shedding::Status::success();
}

std::vector<std::string> list_names(const std::string& directory) {
  std::vector<std::string> names;
  std::error_code error;
  for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
    names.push_back(entry.path().filename().string());
  }
  std::sort(names.begin(), names.end());
  return names;
}

std::string helper_executable() { return environment_value("LOAD_SHEDDING_HELPER_EXE"); }

std::string cli_executable() { return environment_value("LOAD_SHEDDING_CLI_EXE"); }

}  // namespace ls_test
