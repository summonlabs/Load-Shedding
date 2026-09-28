#include "load_shedding/engine.hpp"

#include <algorithm>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <utility>
#include <vector>

#include "detail/durable_store.hpp"
#include "detail/file_lock.hpp"
#include "detail/platform_io.hpp"
#include "detail/planner.hpp"
#include "detail/serialization.hpp"
#include "detail/state.hpp"
#include "load_shedding/text.hpp"

namespace load_shedding {
namespace {

Status authority_error(const char* what) {
  return Status::error(StatusCode::StaleAuthority, what);
}

}  // namespace

class Engine::Impl {
 public:
  EngineOptions options;
  detail::DurableStore store;
  detail::StoreState state;
  std::unique_ptr<detail::FileLock> lock;
  detail::RetentionLimits retention;
  detail::CrashPoint crash_point = detail::CrashPoint::None;
  bool writer = false;
  bool closed = true;
  bool failed = false;
  mutable std::shared_mutex mutex;

  Status check_open() const {
    if (closed) {
      return Status::error(StatusCode::Closed, "the engine is closed");
    }
    return Status::success();
  }

  Status check_writer(AuthorityEpoch epoch, Incarnation incarnation) const {
    if (!writer) {
      return Status::error(StatusCode::AuthorityRequired,
                           "this engine holds no writer authority for '" + options.store_directory + "'");
    }
    if (failed) {
      return Status::error(StatusCode::IoFailure,
                           "this engine lost its durable state and refuses further mutation");
    }
    if (!(epoch == state.authority_epoch)) {
      return Status::error(StatusCode::StaleAuthority,
                           "request claims authority epoch " + std::to_string(epoch.value()) +
                               " but the store is fenced at epoch " +
                               std::to_string(state.authority_epoch.value()));
    }
    if (!(incarnation == state.incarnation)) {
      return Status::error(StatusCode::StaleAuthority,
                           "request claims incarnation " + std::to_string(incarnation.value()) +
                               " but the store is fenced to incarnation " +
                               std::to_string(state.incarnation.value()));
    }
    return Status::success();
  }

  Sequence next_sequence() {
    const Sequence sequence = Sequence::from_value(state.next_sequence);
    ++state.next_sequence;
    return sequence;
  }

  void append_audit(AuditKind kind, Tick tick, RequestId request, PlanId plan,
                    const std::string& detail) {
    AuditEntry entry;
    entry.sequence = next_sequence();
    entry.kind = kind;
    entry.tick = tick;
    entry.authority_epoch = state.authority_epoch;
    entry.incarnation = state.incarnation;
    entry.request = request;
    entry.plan = plan;
    entry.detail = detail.size() > limits::kMaxNoteBytes ? detail.substr(0, limits::kMaxNoteBytes) : detail;
    entry.detail_digest = Digest::of(entry.detail);
    state.audit.push_back(std::move(entry));
  }

  Status publish(const char* what) {
    detail::apply_retention(state, retention);
    auto record = store.commit(state, crash_point);
    if (!record.ok()) {
      auto reloaded = store.load_committed();
      if (reloaded.ok()) {
        state = std::move(reloaded.value());
      } else if (reloaded.code() == StatusCode::NotFound) {
        state = detail::StoreState{};
      } else {
        failed = true;
      }
      return Status::error(record.code(), std::string(what) + " was not published: " + record.message());
    }
    return Status::success();
  }

  void remember_request(RequestId request, const Digest& request_digest, const Digest& result_digest,
                        PlanId plan, PlanGeneration plan_generation, bool has_plan,
                        RequestId recovery_request, bool has_recovery, Tick tick) {
    // Most recent first, bounded: this is the whole retention semantic of the
    // replay window.
    state.idempotency.insert(state.idempotency.begin(),
                             detail::IdempotencyRecord{request, request_digest, result_digest, plan,
                                                       plan_generation, has_plan, recovery_request,
                                                       has_recovery, tick});
    if (state.idempotency.size() > retention.idempotency) {
      state.idempotency.resize(retention.idempotency);
    }
  }

  Result<SheddingPlan> find_plan(PlanId id) const {
    const SheddingPlan* plan = state.find_plan(id);
    if (plan == nullptr) {
      return Status::error(StatusCode::NotFound,
                           "plan " + std::to_string(id.value()) + " is not retained");
    }
    return *plan;
  }

  Result<RecoveryDecision> find_recovery(RequestId request) const {
    const auto found = std::find_if(state.recoveries.begin(), state.recoveries.end(),
                                    [request](const RecoveryDecision& decision) {
                                      return decision.request_id == request;
                                    });
    if (found == state.recoveries.end()) {
      return Status::error(StatusCode::NotFound,
                           "no retained recovery decision answers request " +
                               std::to_string(request.value()));
    }
    return *found;
  }

  Result<EffectObservation> find_effect(RequestId request) const {
    const auto found = std::find_if(state.effects.begin(), state.effects.end(),
                                    [request](const EffectObservation& observation) {
                                      return observation.request_id == request;
                                    });
    if (found == state.effects.end()) {
      return Status::error(StatusCode::NotFound,
                           "no retained effect record answers request " +
                               std::to_string(request.value()));
    }
    return *found;
  }

  Status check_tick(Tick tick) const {
    if (!tick.is_set()) {
      return Status::error(StatusCode::InvalidArgument, "logical tick must be set");
    }
    if (state.tick.is_set() && tick < state.tick) {
      return Status::error(StatusCode::StaleTick,
                           "request tick " + std::to_string(tick.value()) +
                               " is behind the store tick " + std::to_string(state.tick.value()));
    }
    return Status::success();
  }

  void upsert_observed(const ObservedLoadState& observed) {
    const auto found = std::lower_bound(state.observed.begin(), state.observed.end(), observed.load,
                                        [](const ObservedLoadState& candidate, const LoadRef& value) {
                                          return candidate.load < value;
                                        });
    if (found != state.observed.end() && found->load == observed.load) {
      *found = observed;
      return;
    }
    state.observed.insert(found, observed);
  }

  void drop_observed(const LoadRef& load) {
    const auto found = std::lower_bound(state.observed.begin(), state.observed.end(), load,
                                        [](const ObservedLoadState& candidate, const LoadRef& value) {
                                          return candidate.load < value;
                                        });
    if (found != state.observed.end() && found->load == load) {
      state.observed.erase(found);
    }
  }

  void insert_load_sorted(const LoadRecord& load) {
    const auto found = std::lower_bound(state.snapshot.loads.begin(), state.snapshot.loads.end(),
                                        load.ref,
                                        [](const LoadRecord& candidate, const LoadRef& value) {
                                          return candidate.ref < value;
                                        });
    if (found != state.snapshot.loads.end() && found->ref == load.ref) {
      *found = load;
      return;
    }
    state.snapshot.loads.insert(found, load);
  }

  void insert_obligation_sorted(const ProtectedObligation& obligation) {
    const auto found = std::lower_bound(state.snapshot.obligations.begin(),
                                        state.snapshot.obligations.end(), obligation.ref,
                                        [](const ProtectedObligation& candidate,
                                           const ObligationRef& value) { return candidate.ref < value; });
    if (found != state.snapshot.obligations.end() && found->ref == obligation.ref) {
      *found = obligation;
      return;
    }
    state.snapshot.obligations.insert(found, obligation);
  }

  Status bump_revision() {
    auto next = state.revision.next();
    if (!next.ok()) {
      return next.status();
    }
    state.revision = next.value();
    state.snapshot.revision = state.revision;
    return Status::success();
  }

  Status bump_evidence() {
    auto next = state.evidence_generation.next();
    if (!next.ok()) {
      return next.status();
    }
    state.evidence_generation = next.value();
    state.snapshot.generation = state.evidence_generation;
    return Status::success();
  }
};

Engine::Engine() = default;
Engine::~Engine() = default;
Engine::Engine(Engine&& other) noexcept = default;
Engine& Engine::operator=(Engine&& other) noexcept = default;

Result<Engine> Engine::open(const EngineOptions& options) {
  if (options.store_directory.empty()) {
    return Status::error(StatusCode::InvalidArgument, "store directory must not be empty");
  }
  if (!options.incarnation.is_set()) {
    return Status::error(StatusCode::InvalidArgument, "engine incarnation must be set");
  }
  if (options.max_plans_retained == 0 || options.max_effect_records == 0 ||
      options.max_audit_entries == 0 || options.max_idempotency_entries == 0) {
    return Status::error(StatusCode::InvalidArgument, "retention limits must be greater than zero");
  }
  if (options.max_idempotency_entries > options.max_plans_retained) {
    return Status::error(StatusCode::InvalidArgument,
                         "the replay window must not exceed the retained plan window: a replayed "
                         "attempt has to remain resolvable");
  }
  auto crash_point = detail::crash_point_from_string(options.crash_after);
  if (!crash_point.ok()) {
    return crash_point.status();
  }
  const Status path_valid = detail::validate_store_path(options.store_directory);
  if (!path_valid.ok()) {
    return path_valid;
  }
  // The store directory is established before the lock file is created inside it.
  {
    const std::filesystem::path root = detail::path_from_utf8(options.store_directory);
    if (!detail::file_exists(root)) {
      if (!options.create_if_missing) {
        return Status::error(StatusCode::NotFound,
                             "store directory '" + options.store_directory + "' does not exist");
      }
      const Status created = detail::ensure_directory(root);
      if (!created.ok()) {
        return created;
      }
    }
    auto plain = detail::is_plain_directory(root);
    if (!plain.ok()) {
      return plain.status();
    }
    if (!plain.value()) {
      return Status::error(StatusCode::Rejected,
                           "store path '" + options.store_directory + "' is not a plain directory");
    }
  }

  Engine engine;
  engine.impl_ = std::make_unique<Impl>();
  Impl& impl = *engine.impl_;
  impl.options = options;
  // Fault injection applies to mutations performed through the open engine, not
  // to the authority takeover that opening performs: a caller that asks to crash
  // at a durable stage is asking about its own commit.
  impl.crash_point = detail::CrashPoint::None;
  impl.retention.plans = options.max_plans_retained;
  impl.retention.effects = options.max_effect_records;
  impl.retention.audit = options.max_audit_entries;
  impl.retention.idempotency = options.max_idempotency_entries;

  const std::string lock_path = options.store_directory + "/" + std::string(store_format::kLockName);
  auto lock = detail::FileLock::acquire(
      detail::path_from_utf8(lock_path),
      options.take_authority ? detail::FileLock::Mode::Exclusive : detail::FileLock::Mode::Shared);
  if (!lock.ok()) {
    return lock.status();
  }
  impl.lock = std::make_unique<detail::FileLock>(std::move(lock.value()));
  impl.writer = options.take_authority;

  auto store = detail::DurableStore::open(options.store_directory, options.create_if_missing);
  if (!store.ok()) {
    return store.status();
  }
  impl.store = std::move(store.value());

  auto state = impl.store.load_committed();
  if (state.ok()) {
    impl.state = std::move(state.value());
  } else if (state.code() == StatusCode::NotFound) {
    impl.state = detail::StoreState{};
  } else {
    return state.status();
  }
  impl.closed = false;

  if (impl.writer && options.install_default_policy_if_missing && !impl.state.policy_installed) {
    SheddingPolicy policy = make_default_policy();
    policy.generation = PolicyGeneration::first();
    const Status installed = [&]() -> Status {
      impl.state.policy = policy;
      impl.state.policy_installed = true;
      impl.state.policy_generation = policy.generation;
      const Status revision = impl.bump_revision();
      if (!revision.ok()) {
        return revision;
      }
      impl.append_audit(AuditKind::PolicyInstalled, impl.state.tick, RequestId{},
                        PlanId{}, "default policy installed at open");
      return impl.publish("the default policy");
    }();
    if (!installed.ok()) {
      return installed;
    }
  }

  if (impl.writer) {
    // A different incarnation fences the previous writer by advancing the epoch.
    // The same incarnation resuming after its process ended keeps its epoch: it
    // is the same writer, and only one process can hold the store lock at a time.
    const bool same_incarnation =
        impl.state.incarnation.is_set() && impl.state.incarnation == options.incarnation;
    AuthorityEpoch next_epoch = impl.state.authority_epoch;
    if (!same_incarnation) {
      next_epoch = impl.state.authority_epoch.is_set()
                       ? impl.state.authority_epoch.next().value_or(AuthorityEpoch{})
                       : options.initial_authority_epoch;
      if (!next_epoch.is_set()) {
        return Status::error(StatusCode::InvalidArgument, "could not derive an authority epoch");
      }
      if (!(next_epoch > impl.state.authority_epoch)) {
        return Status::error(StatusCode::Overflow, "authority epoch counter exhausted");
      }
    } else if (!next_epoch.is_set()) {
      next_epoch = options.initial_authority_epoch;
      if (!next_epoch.is_set()) {
        return Status::error(StatusCode::InvalidArgument, "could not derive an authority epoch");
      }
    }
    const bool changed = !same_incarnation || !(next_epoch == impl.state.authority_epoch);
    impl.state.authority_epoch = next_epoch;
    impl.state.incarnation = options.incarnation;
    if (changed) {
      // Opening is not by itself a state change: a writer that resumes its own
      // epoch leaves the committed generation exactly as it found it, so
      // reopening a store is byte-stable.
      impl.append_audit(AuditKind::AuthorityTaken, impl.state.tick, RequestId{}, PlanId{},
                        same_incarnation
                            ? "writer authority resumed at epoch " + std::to_string(next_epoch.value())
                            : "writer authority taken at epoch " + std::to_string(next_epoch.value()));
      const Status published = impl.publish("the authority claim");
      if (!published.ok()) {
        return published;
      }
    }
  }
  impl.crash_point = crash_point.value();
  return engine;
}

Status Engine::close() {
  if (impl_ == nullptr) {
    return Status::success();
  }
  {
    std::unique_lock<std::shared_mutex> guard(impl_->mutex);
    if (impl_->closed) {
      return Status::success();
    }
    impl_->closed = true;
    impl_->writer = false;
    if (impl_->lock != nullptr) {
      const Status released = impl_->lock->release();
      impl_->lock.reset();
      if (!released.ok()) {
        return released;
      }
    }
  }
  return Status::success();
}

AuthorityStatus Engine::authority() const {
  AuthorityStatus status;
  if (impl_ == nullptr) {
    return status;
  }
  std::shared_lock<std::shared_mutex> guard(impl_->mutex);
  status.epoch = impl_->state.authority_epoch;
  status.incarnation = impl_->state.incarnation;
  status.writer = impl_->writer;
  status.store_directory = impl_->options.store_directory;
  status.state_generation = impl_->store.commit_record().generation;
  return status;
}

bool Engine::has_writer_authority() const {
  if (impl_ == nullptr) {
    return false;
  }
  std::shared_lock<std::shared_mutex> guard(impl_->mutex);
  return impl_->writer && !impl_->closed;
}

Result<AuthorityEpoch> Engine::take_authority(Incarnation incarnation, AuthorityEpoch requested_epoch) {
  if (impl_ == nullptr) {
    return Status::error(StatusCode::Closed, "the engine is closed");
  }
  std::unique_lock<std::shared_mutex> guard(impl_->mutex);
  Impl& impl = *impl_;
  const Status open = impl.check_open();
  if (!open.ok()) {
    return open;
  }
  if (!incarnation.is_set()) {
    return Status::error(StatusCode::InvalidArgument, "incarnation must be set");
  }
  if (!requested_epoch.is_set()) {
    return Status::error(StatusCode::InvalidArgument, "requested authority epoch must be set");
  }
  if (impl.writer && incarnation == impl.state.incarnation &&
      requested_epoch == impl.state.authority_epoch) {
    return impl.state.authority_epoch;
  }
  // Upgrade path: release the shared lock before taking the exclusive one. The
  // process never holds both, so there is no lock-order inversion between the
  // cross-process lock and the in-process lock.
  if (impl.lock != nullptr && impl.lock->mode() != detail::FileLock::Mode::Exclusive) {
    const Status released = impl.lock->release();
    if (!released.ok()) {
      return released;
    }
    impl.lock.reset();
    impl.writer = false;
  }
  if (impl.lock == nullptr) {
    const std::string lock_path =
        impl.options.store_directory + "/" + std::string(store_format::kLockName);
    auto lock = detail::FileLock::acquire(detail::path_from_utf8(lock_path),
                                          detail::FileLock::Mode::Exclusive);
    if (!lock.ok()) {
      return lock.status();
    }
    impl.lock = std::make_unique<detail::FileLock>(std::move(lock.value()));
  }
  if (requested_epoch <= impl.state.authority_epoch) {
    return Status::error(StatusCode::StaleAuthority,
                         "requested epoch " + std::to_string(requested_epoch.value()) +
                             " is not ahead of the current epoch " +
                             std::to_string(impl.state.authority_epoch.value()));
  }
  impl.state.authority_epoch = requested_epoch;
  impl.state.incarnation = incarnation;
  impl.writer = true;
  impl.append_audit(AuditKind::AuthorityTaken, impl.state.tick, RequestId{}, PlanId{},
                    "writer authority taken at epoch " + std::to_string(requested_epoch.value()));
  const Status published = impl.publish("the authority takeover");
  if (!published.ok()) {
    return published;
  }
  return requested_epoch;
}

Result<PolicyGeneration> Engine::install_policy(const SheddingPolicy& policy,
                                                AuthorityEpoch authority_epoch,
                                                Incarnation incarnation) {
  if (impl_ == nullptr) {
    return Status::error(StatusCode::Closed, "the engine is closed");
  }
  std::unique_lock<std::shared_mutex> guard(impl_->mutex);
  Impl& impl = *impl_;
  const Status open = impl.check_open();
  if (!open.ok()) {
    return open;
  }
  const Status authorized = impl.check_writer(authority_epoch, incarnation);
  if (!authorized.ok()) {
    return authorized;
  }
  const Status valid = policy.validate();
  if (!valid.ok()) {
    return valid;
  }
  SheddingPolicy installed = policy;
  auto next_generation = impl.state.policy_generation.next();
  if (!next_generation.ok()) {
    return next_generation.status();
  }
  installed.generation = next_generation.value();
  impl.state.policy = installed;
  impl.state.policy_installed = true;
  impl.state.policy_generation = installed.generation;
  const Status revision = impl.bump_revision();
  if (!revision.ok()) {
    return revision;
  }
  impl.append_audit(AuditKind::PolicyInstalled, impl.state.tick, RequestId{}, PlanId{},
                    "policy '" + installed.name + "' installed as generation " +
                        std::to_string(installed.generation.value()));
  const Status published = impl.publish("the policy installation");
  if (!published.ok()) {
    return published;
  }
  return installed.generation;
}

Result<StateRevision> Engine::upsert_load(const LoadRecord& load, AuthorityEpoch authority_epoch,
                                          Incarnation incarnation) {
  if (impl_ == nullptr) {
    return Status::error(StatusCode::Closed, "the engine is closed");
  }
  std::unique_lock<std::shared_mutex> guard(impl_->mutex);
  Impl& impl = *impl_;
  const Status open = impl.check_open();
  if (!open.ok()) {
    return open;
  }
  const Status authorized = impl.check_writer(authority_epoch, incarnation);
  if (!authorized.ok()) {
    return authorized;
  }
  LoadRecord updated = load;
  const Status valid = updated.validate();
  if (!valid.ok()) {
    return valid;
  }
  const Status tick_valid = impl.check_tick(updated.contribution.tick);
  if (!tick_valid.ok()) {
    return tick_valid;
  }
  const LoadRecord* existing = impl.state.snapshot.find(updated.ref);
  if (existing != nullptr && updated.contribution.tick < existing->contribution.tick) {
    return Status::error(StatusCode::StaleEvidenceGeneration,
                         "load '" + updated.ref.value() + "' already carries evidence from tick " +
                             std::to_string(existing->contribution.tick.value()) +
                             "; an older observation is refused");
  }
  if (!updated.identity_generation.is_set()) {
    auto next = (existing != nullptr ? existing->identity_generation.next()
                                     : LoadGeneration::first().next());
    if (!next.ok()) {
      return next.status();
    }
    updated.identity_generation = next.value();
  }
  const Status evidence = impl.bump_evidence();
  if (!evidence.ok()) {
    return evidence;
  }
  // Incremental edits stamp the load's evidence with the new evidence generation.
  updated.contribution.generation = impl.state.evidence_generation;
  const Status revision = impl.bump_revision();
  if (!revision.ok()) {
    return revision;
  }
  updated.revision = impl.state.revision;
  impl.state.tick = updated.contribution.tick;
  impl.state.snapshot.tick = updated.contribution.tick;
  impl.insert_load_sorted(updated);
  impl.append_audit(AuditKind::LoadUpserted, impl.state.tick, RequestId{}, PlanId{},
                    "load '" + updated.ref.value() + "' recorded at " +
                        updated.contribution.value.to_string() + " (" +
                        std::string(to_string(updated.contribution.state)) + ")");
  const Status published = impl.publish("the load update");
  if (!published.ok()) {
    return published;
  }
  return impl.state.revision;
}

Result<StateRevision> Engine::remove_load(const LoadRef& load, AuthorityEpoch authority_epoch,
                                          Incarnation incarnation) {
  if (impl_ == nullptr) {
    return Status::error(StatusCode::Closed, "the engine is closed");
  }
  std::unique_lock<std::shared_mutex> guard(impl_->mutex);
  Impl& impl = *impl_;
  const Status open = impl.check_open();
  if (!open.ok()) {
    return open;
  }
  const Status authorized = impl.check_writer(authority_epoch, incarnation);
  if (!authorized.ok()) {
    return authorized;
  }
  if (load.empty()) {
    return Status::error(StatusCode::InvalidArgument, "load reference must not be empty");
  }
  const auto found = std::lower_bound(impl.state.snapshot.loads.begin(), impl.state.snapshot.loads.end(),
                                      load,
                                      [](const LoadRecord& candidate, const LoadRef& value) {
                                        return candidate.ref < value;
                                      });
  if (found == impl.state.snapshot.loads.end() || !(found->ref == load)) {
    return Status::error(StatusCode::NotFound, "load '" + load.value() + "' is not recorded");
  }
  for (const ProtectedObligation& obligation : impl.state.snapshot.obligations) {
    if (obligation.active && obligation.load == load) {
      return Status::error(StatusCode::ProtectedObligation,
                           "load '" + load.value() + "' carries active obligation '" +
                               obligation.ref.value() + "' and cannot be withdrawn");
    }
  }
  impl.state.snapshot.loads.erase(found);
  impl.drop_observed(load);
  impl.state.snapshot.obligations.erase(
      std::remove_if(impl.state.snapshot.obligations.begin(), impl.state.snapshot.obligations.end(),
                     [&load](const ProtectedObligation& obligation) { return obligation.load == load; }),
      impl.state.snapshot.obligations.end());
  const Status evidence = impl.bump_evidence();
  if (!evidence.ok()) {
    return evidence;
  }
  const Status revision = impl.bump_revision();
  if (!revision.ok()) {
    return revision;
  }
  impl.append_audit(AuditKind::LoadRemoved, impl.state.tick, RequestId{}, PlanId{},
                    "load '" + load.value() + "' withdrawn");
  const Status published = impl.publish("the load withdrawal");
  if (!published.ok()) {
    return published;
  }
  return impl.state.revision;
}

Result<StateRevision> Engine::upsert_obligation(const ProtectedObligation& obligation,
                                                AuthorityEpoch authority_epoch,
                                                Incarnation incarnation) {
  if (impl_ == nullptr) {
    return Status::error(StatusCode::Closed, "the engine is closed");
  }
  std::unique_lock<std::shared_mutex> guard(impl_->mutex);
  Impl& impl = *impl_;
  const Status open = impl.check_open();
  if (!open.ok()) {
    return open;
  }
  const Status authorized = impl.check_writer(authority_epoch, incarnation);
  if (!authorized.ok()) {
    return authorized;
  }
  ProtectedObligation updated = obligation;
  const Status valid = updated.validate();
  if (!valid.ok()) {
    return valid;
  }
  const Status tick_valid = impl.check_tick(updated.tick);
  if (!tick_valid.ok()) {
    return tick_valid;
  }
  if (!updated.load.empty() && impl.state.snapshot.find(updated.load) == nullptr) {
    return Status::error(StatusCode::NotFound,
                         "obligation '" + updated.ref.value() + "' names load '" +
                             updated.load.value() + "', which is not recorded");
  }
  const Status evidence = impl.bump_evidence();
  if (!evidence.ok()) {
    return evidence;
  }
  updated.generation = impl.state.evidence_generation;
  const Status revision = impl.bump_revision();
  if (!revision.ok()) {
    return revision;
  }
  updated.revision = impl.state.revision;
  impl.state.tick = updated.tick;
  impl.state.snapshot.tick = updated.tick;
  impl.insert_obligation_sorted(updated);
  impl.append_audit(AuditKind::ObligationUpserted, impl.state.tick, RequestId{}, PlanId{},
                    "obligation '" + updated.ref.value() + "' reserves " +
                        updated.reserved.to_string() + (updated.active ? " (active)" : " (inactive)"));
  const Status published = impl.publish("the obligation update");
  if (!published.ok()) {
    return published;
  }
  return impl.state.revision;
}

Result<StateRevision> Engine::remove_obligation(const ObligationRef& obligation,
                                                AuthorityEpoch authority_epoch,
                                                Incarnation incarnation) {
  if (impl_ == nullptr) {
    return Status::error(StatusCode::Closed, "the engine is closed");
  }
  std::unique_lock<std::shared_mutex> guard(impl_->mutex);
  Impl& impl = *impl_;
  const Status open = impl.check_open();
  if (!open.ok()) {
    return open;
  }
  const Status authorized = impl.check_writer(authority_epoch, incarnation);
  if (!authorized.ok()) {
    return authorized;
  }
  const auto found = std::lower_bound(impl.state.snapshot.obligations.begin(),
                                      impl.state.snapshot.obligations.end(), obligation,
                                      [](const ProtectedObligation& candidate,
                                         const ObligationRef& value) { return candidate.ref < value; });
  if (found == impl.state.snapshot.obligations.end() || !(found->ref == obligation)) {
    return Status::error(StatusCode::NotFound,
                         "obligation '" + obligation.value() + "' is not recorded");
  }
  impl.state.snapshot.obligations.erase(found);
  const Status evidence = impl.bump_evidence();
  if (!evidence.ok()) {
    return evidence;
  }
  const Status revision = impl.bump_revision();
  if (!revision.ok()) {
    return revision;
  }
  impl.append_audit(AuditKind::ObligationRemoved, impl.state.tick, RequestId{}, PlanId{},
                    "obligation '" + obligation.value() + "' withdrawn");
  const Status published = impl.publish("the obligation withdrawal");
  if (!published.ok()) {
    return published;
  }
  return impl.state.revision;
}

Result<StateRevision> Engine::replace_snapshot(const FacilitySnapshot& snapshot,
                                               AuthorityEpoch authority_epoch,
                                               Incarnation incarnation) {
  if (impl_ == nullptr) {
    return Status::error(StatusCode::Closed, "the engine is closed");
  }
  std::unique_lock<std::shared_mutex> guard(impl_->mutex);
  Impl& impl = *impl_;
  const Status open = impl.check_open();
  if (!open.ok()) {
    return open;
  }
  const Status authorized = impl.check_writer(authority_epoch, incarnation);
  if (!authorized.ok()) {
    return authorized;
  }
  FacilitySnapshot updated = snapshot;
  const Status valid = updated.validate();
  if (!valid.ok()) {
    return valid;
  }
  if (!updated.generation.is_set()) {
    return Status::error(StatusCode::InvalidArgument, "snapshot evidence generation must be set");
  }
  if (updated.generation <= impl.state.evidence_generation) {
    return Status::error(StatusCode::StaleEvidenceGeneration,
                         "snapshot generation " + std::to_string(updated.generation.value()) +
                             " is not ahead of the installed generation " +
                             std::to_string(impl.state.evidence_generation.value()));
  }
  const Status tick_valid = impl.check_tick(updated.tick);
  if (!tick_valid.ok()) {
    return tick_valid;
  }
  for (const LoadRecord& load : updated.loads) {
    if (load.contribution.generation > updated.generation) {
      return Status::error(StatusCode::StaleEvidenceGeneration,
                           "load '" + load.ref.value() + "' carries evidence from generation " +
                               std::to_string(load.contribution.generation.value()) +
                               ", ahead of the snapshot generation " +
                               std::to_string(updated.generation.value()));
    }
  }
  std::sort(updated.loads.begin(), updated.loads.end(),
            [](const LoadRecord& left, const LoadRecord& right) { return left.ref < right.ref; });
  for (std::size_t index = 1; index < updated.loads.size(); ++index) {
    if (updated.loads[index - 1].ref == updated.loads[index].ref) {
      return Status::error(StatusCode::DuplicateIdentity,
                           "snapshot contains load '" + updated.loads[index].ref.value() + "' twice");
    }
  }
  std::sort(updated.obligations.begin(), updated.obligations.end(),
            [](const ProtectedObligation& left, const ProtectedObligation& right) {
              return left.ref < right.ref;
            });
  for (std::size_t index = 1; index < updated.obligations.size(); ++index) {
    if (updated.obligations[index - 1].ref == updated.obligations[index].ref) {
      return Status::error(StatusCode::DuplicateIdentity,
                           "snapshot contains obligation '" + updated.obligations[index].ref.value() +
                               "' twice");
    }
  }
  for (const ProtectedObligation& obligation : updated.obligations) {
    if (!obligation.load.empty() &&
        std::none_of(updated.loads.begin(), updated.loads.end(),
                     [&obligation](const LoadRecord& load) { return load.ref == obligation.load; })) {
      return Status::error(StatusCode::NotFound,
                           "obligation '" + obligation.ref.value() + "' names load '" +
                               obligation.load.value() + "', which the snapshot does not contain");
    }
  }
  impl.state.snapshot = std::move(updated);
  impl.state.evidence_generation = impl.state.snapshot.generation;
  impl.state.tick = impl.state.snapshot.tick;
  const Status revision = impl.bump_revision();
  if (!revision.ok()) {
    return revision;
  }
  impl.state.snapshot.revision = impl.state.revision;
  // Observed states that no longer describe a known load are dropped.
  impl.state.observed.erase(
      std::remove_if(impl.state.observed.begin(), impl.state.observed.end(),
                     [&impl](const ObservedLoadState& observed) {
                       return impl.state.snapshot.find(observed.load) == nullptr;
                     }),
      impl.state.observed.end());
  impl.append_audit(AuditKind::SnapshotReplaced, impl.state.tick, RequestId{}, PlanId{},
                    "snapshot " + std::to_string(impl.state.snapshot.id.value()) + " installed with " +
                        std::to_string(impl.state.snapshot.loads.size()) + " loads at evidence generation " +
                        std::to_string(impl.state.evidence_generation.value()));
  const Status published = impl.publish("the snapshot replacement");
  if (!published.ok()) {
    return published;
  }
  return impl.state.revision;
}

Result<PlanOutcome> Engine::plan(const PlanRequest& request) {
  if (impl_ == nullptr) {
    return Status::error(StatusCode::Closed, "the engine is closed");
  }
  std::unique_lock<std::shared_mutex> guard(impl_->mutex);
  Impl& impl = *impl_;
  const Status open = impl.check_open();
  if (!open.ok()) {
    return open;
  }
  if (!request.request_id.is_set()) {
    return Status::error(StatusCode::InvalidArgument, "plan request identity must be set");
  }
  if (request.requested_reduction.is_negative()) {
    return Status::error(StatusCode::InvalidArgument, "requested reduction must not be negative");
  }
  if (request.requested_reduction.is_zero()) {
    return Status::error(StatusCode::InvalidArgument, "requested reduction must be greater than zero");
  }
  if (request.emergency_authority && request.emergency_justification.empty()) {
    return Status::error(StatusCode::InvalidArgument, "an emergency grant requires a justification");
  }
  const Status authorized = impl.check_writer(request.authority_epoch, request.incarnation);
  if (!authorized.ok()) {
    return authorized;
  }

  const Digest request_digest = request.digest();
  const detail::IdempotencyRecord* replay = impl.state.find_idempotency(request.request_id);
  if (replay != nullptr) {
    if (!(replay->request_digest == request_digest)) {
      return Status::error(StatusCode::IdempotencyConflict,
                           "request identity " + std::to_string(request.request_id.value()) +
                               " was already accepted with different content");
    }
    // A replay of an accepted attempt returns the prior result before any
    // staleness check, so a lost response can be retried safely.
    if (replay->has_plan) {
      auto stored = impl.find_plan(replay->plan);
      if (!stored.ok()) {
        return Status::error(StatusCode::NotFound,
                             "the replayed attempt's plan has been retired from the retained window");
      }
      PlanOutcome outcome;
      outcome.plan = std::move(stored.value());
      outcome.attempt = outcome.plan.identity.attempt;
      outcome.replayed = true;
      outcome.request_digest = request_digest;
      return outcome;
    }
    return Status::error(StatusCode::NotFound,
                         "request identity " + std::to_string(request.request_id.value()) +
                             " is retained for a different decision kind");
  }

  if (!impl.check_tick(request.tick).ok()) {
    return impl.check_tick(request.tick);
  }
  if (!impl.state.policy_installed) {
    return Status::error(StatusCode::PolicyViolation, "no shedding policy is installed");
  }
  if (!(request.policy_generation == impl.state.policy_generation)) {
    return Status::error(StatusCode::StalePolicyGeneration,
                         "request targets policy generation " +
                             std::to_string(request.policy_generation.value()) +
                             " but the installed generation is " +
                             std::to_string(impl.state.policy_generation.value()));
  }
  if (!(request.evidence_generation == impl.state.evidence_generation)) {
    return Status::error(StatusCode::StaleEvidenceGeneration,
                         "request targets evidence generation " +
                             std::to_string(request.evidence_generation.value()) +
                             " but the installed generation is " +
                             std::to_string(impl.state.evidence_generation.value()));
  }
  if (!(request.effect_generation == impl.state.effect_generation)) {
    return Status::error(StatusCode::StaleEffectGeneration,
                         "request targets effect generation " +
                             std::to_string(request.effect_generation.value()) +
                             " but the current effect generation is " +
                             std::to_string(impl.state.effect_generation.value()));
  }
  if (!(request.base_revision == impl.state.revision)) {
    return Status::error(StatusCode::RevisionConflict,
                         "request targets revision " + std::to_string(request.base_revision.value()) +
                             " but the current revision is " +
                             std::to_string(impl.state.revision.value()));
  }

  auto next_plan_generation = impl.state.plan_generation.next();
  if (!next_plan_generation.ok()) {
    return next_plan_generation.status();
  }
  auto next_plan_id = PlanId::from_value(impl.state.plan_generation.value() + 1);
  auto next_attempt = AttemptId::from_value(impl.state.plan_generation.value() + 1);

  detail::PlanContext context;
  context.policy = &impl.state.policy;
  context.snapshot = &impl.state.snapshot;
  context.observed = &impl.state.observed;
  context.request = &request;
  context.plan_id = next_plan_id;
  context.plan_generation = next_plan_generation.value();
  context.attempt = next_attempt;
  context.policy_digest = impl.state.policy.content_digest();

  auto computed = detail::compute_plan(context);
  if (!computed.ok()) {
    impl.append_audit(AuditKind::PlanRefused, request.tick, request.request_id, PlanId{},
                      std::string(to_string(computed.code())) + ": " + computed.message());
    const Status audited = impl.publish("the refusal record");
    (void)audited;
    return computed.status();
  }

  SheddingPlan plan = std::move(computed.value());
  const Digest plan_digest = plan.content_digest();
  impl.state.plans.push_back(plan);
  impl.state.plan_generation = next_plan_generation.value();
  impl.state.tick = request.tick;
  // A decision does not advance the configuration revision: the plan is
  // identified by its own generation, and its `base_revision` must keep meaning
  // "the configuration and evidence this was planned against".
  impl.append_audit(AuditKind::PlanCommitted, request.tick, request.request_id, plan.identity.plan_id,
                    "plan " + std::to_string(plan.identity.plan_id.value()) + " requests " +
                        plan.closure.requested_reduction.to_string() + ", selects " +
                        plan.closure.selected_expected_reduction.to_string() + ", residual " +
                        plan.closure.residual_deficit.to_string() + ", coverage " +
                        std::string(to_string(plan.coverage)));
  impl.remember_request(request.request_id, request_digest, plan_digest, plan.identity.plan_id,
                        plan.identity.generation, true, RequestId{}, false, request.tick);

  const Status published = impl.publish("the plan");
  if (!published.ok()) {
    return published;
  }

  PlanOutcome outcome;
  outcome.plan = std::move(plan);
  outcome.attempt = next_attempt;
  outcome.replayed = false;
  outcome.request_digest = plan_digest;
  return outcome;
}

Result<SheddingPlan> Engine::plan_by_id(PlanId id) const {
  if (impl_ == nullptr) {
    return Status::error(StatusCode::Closed, "the engine is closed");
  }
  std::shared_lock<std::shared_mutex> guard(impl_->mutex);
  const Status open = impl_->check_open();
  if (!open.ok()) {
    return open;
  }
  return impl_->find_plan(id);
}

Result<SheddingPlan> Engine::latest_plan() const {
  if (impl_ == nullptr) {
    return Status::error(StatusCode::Closed, "the engine is closed");
  }
  std::shared_lock<std::shared_mutex> guard(impl_->mutex);
  const Status open = impl_->check_open();
  if (!open.ok()) {
    return open;
  }
  if (impl_->state.plans.empty()) {
    return Status::error(StatusCode::NotFound, "no plan has been committed");
  }
  return impl_->state.plans.back();
}

Result<RecoveryDecision> Engine::recovery(const RecoveryRequest& request) {
  if (impl_ == nullptr) {
    return Status::error(StatusCode::Closed, "the engine is closed");
  }
  std::unique_lock<std::shared_mutex> guard(impl_->mutex);
  Impl& impl = *impl_;
  const Status open = impl.check_open();
  if (!open.ok()) {
    return open;
  }
  if (!request.request_id.is_set()) {
    return Status::error(StatusCode::InvalidArgument, "recovery request identity must be set");
  }
  if (request.available_headroom.is_negative()) {
    return Status::error(StatusCode::InvalidArgument, "available headroom must not be negative");
  }
  const Status authorized = impl.check_writer(request.authority_epoch, request.incarnation);
  if (!authorized.ok()) {
    return authorized;
  }

  const Digest request_digest = request.digest();
  const detail::IdempotencyRecord* replay = impl.state.find_idempotency(request.request_id);
  if (replay != nullptr) {
    if (!(replay->request_digest == request_digest)) {
      return Status::error(StatusCode::IdempotencyConflict,
                           "request identity " + std::to_string(request.request_id.value()) +
                               " was already accepted with different content");
    }
    if (replay->has_recovery) {
      return impl.find_recovery(replay->recovery_request);
    }
    return Status::error(StatusCode::NotFound,
                         "request identity " + std::to_string(request.request_id.value()) +
                             " is retained for a different decision kind");
  }

  const Status tick_valid = impl.check_tick(request.tick);
  if (!tick_valid.ok()) {
    return tick_valid;
  }
  if (!impl.state.policy_installed) {
    return Status::error(StatusCode::PolicyViolation, "no shedding policy is installed");
  }
  if (!(request.policy_generation == impl.state.policy_generation)) {
    return Status::error(StatusCode::StalePolicyGeneration,
                         "request targets policy generation " +
                             std::to_string(request.policy_generation.value()));
  }
  if (!(request.evidence_generation == impl.state.evidence_generation)) {
    return Status::error(StatusCode::StaleEvidenceGeneration,
                         "request targets evidence generation " +
                             std::to_string(request.evidence_generation.value()));
  }
  if (!(request.effect_generation == impl.state.effect_generation)) {
    return Status::error(StatusCode::StaleEffectGeneration,
                         "request targets effect generation " +
                             std::to_string(request.effect_generation.value()));
  }
  if (!(request.base_revision == impl.state.revision)) {
    return Status::error(StatusCode::RevisionConflict,
                         "request targets revision " + std::to_string(request.base_revision.value()));
  }
  auto plan = impl.find_plan(request.plan_id);
  if (!plan.ok()) {
    return plan.status();
  }
  if (!(plan.value().identity.generation == request.plan_generation)) {
    return Status::error(StatusCode::StalePlan,
                         "request targets plan generation " +
                             std::to_string(request.plan_generation.value()) +
                             " but plan " + std::to_string(request.plan_id.value()) +
                             " is generation " +
                             std::to_string(plan.value().identity.generation.value()));
  }

  detail::RecoveryContext context;
  context.policy = &impl.state.policy;
  context.plan = &plan.value();
  context.observed = &impl.state.observed;
  context.request = &request;
  auto computed = detail::compute_recovery(context);
  if (!computed.ok()) {
    return computed.status();
  }
  RecoveryDecision decision = std::move(computed.value());
  const Digest decision_digest = decision.content_digest();
  impl.state.recoveries.insert(impl.state.recoveries.begin(), decision);
  impl.state.tick = request.tick;
  impl.append_audit(AuditKind::RecoveryDecided, request.tick, request.request_id, request.plan_id,
                    "recovery decision restores " + decision.restored_expected.to_string() +
                        " of " + decision.requested_headroom.to_string() + " headroom");
  impl.remember_request(request.request_id, request_digest, decision_digest, PlanId{},
                        PlanGeneration{}, false, request.request_id, true, request.tick);
  const Status published = impl.publish("the recovery decision");
  if (!published.ok()) {
    return published;
  }
  return decision;
}

Result<RecoveryDecision> Engine::recovery_by_request(RequestId id) const {
  if (impl_ == nullptr) {
    return Status::error(StatusCode::Closed, "the engine is closed");
  }
  std::shared_lock<std::shared_mutex> guard(impl_->mutex);
  const Status open = impl_->check_open();
  if (!open.ok()) {
    return open;
  }
  return impl_->find_recovery(id);
}

Result<RecoveryDecision> Engine::last_recovery_decision() const {
  if (impl_ == nullptr) {
    return Status::error(StatusCode::Closed, "the engine is closed");
  }
  std::shared_lock<std::shared_mutex> guard(impl_->mutex);
  const Status open = impl_->check_open();
  if (!open.ok()) {
    return open;
  }
  if (impl_->state.recoveries.empty()) {
    return Status::error(StatusCode::NotFound, "no recovery decision has been committed");
  }
  return impl_->state.recoveries.front();
}

Result<EffectObservation> Engine::observe_effect(const EffectObservationRequest& request) {
  if (impl_ == nullptr) {
    return Status::error(StatusCode::Closed, "the engine is closed");
  }
  std::unique_lock<std::shared_mutex> guard(impl_->mutex);
  Impl& impl = *impl_;
  const Status open = impl.check_open();
  if (!open.ok()) {
    return open;
  }
  if (!request.request_id.is_set()) {
    return Status::error(StatusCode::InvalidArgument, "effect request identity must be set");
  }
  if (request.load.empty()) {
    return Status::error(StatusCode::InvalidArgument, "effect record must name a load");
  }
  if (!request.plan_id.is_set()) {
    return Status::error(StatusCode::InvalidArgument, "effect record must name a plan");
  }
  const Status authorized = impl.check_writer(request.authority_epoch, request.incarnation);
  if (!authorized.ok()) {
    return authorized;
  }

  const Digest request_digest = request.digest();
  const detail::IdempotencyRecord* replay = impl.state.find_idempotency(request.request_id);
  if (replay != nullptr) {
    if (!(replay->request_digest == request_digest)) {
      return Status::error(StatusCode::IdempotencyConflict,
                           "request identity " + std::to_string(request.request_id.value()) +
                               " was already accepted with different content");
    }
    return impl.find_effect(request.request_id);
  }

  const Status tick_valid = impl.check_tick(request.tick);
  if (!tick_valid.ok()) {
    return tick_valid;
  }
  auto plan = impl.find_plan(request.plan_id);
  if (!plan.ok()) {
    return plan.status();
  }
  if (!(plan.value().identity.generation == request.plan_generation)) {
    return Status::error(StatusCode::StalePlan,
                         "effect record targets plan generation " +
                             std::to_string(request.plan_generation.value()));
  }
  if (impl.state.snapshot.find(request.load) == nullptr) {
    return Status::error(StatusCode::NotFound,
                         "effect record names load '" + request.load.value() +
                             "', which the snapshot does not contain");
  }
  if (!(request.base_revision == impl.state.revision)) {
    return Status::error(StatusCode::RevisionConflict,
                         "effect record targets revision " +
                             std::to_string(request.base_revision.value()) + " but the current revision is " +
                             std::to_string(impl.state.revision.value()));
  }
  const ObservedLoadState* existing = impl.state.find_observed(request.load);
  if (existing != nullptr && existing->since.is_set() && request.tick < existing->since) {
    return Status::error(StatusCode::StaleEvidenceGeneration,
                         "load '" + request.load.value() + "' already carries an observation from tick " +
                             std::to_string(existing->since.value()) +
                             "; an older or reordered observation is refused");
  }

  auto next_effect = impl.state.effect_generation.next();
  if (!next_effect.ok()) {
    return next_effect.status();
  }
  EffectObservation observation;
  observation.sequence = impl.next_sequence();
  observation.request_id = request.request_id;
  observation.plan_id = request.plan_id;
  observation.plan_generation = request.plan_generation;
  observation.load = request.load;
  observation.attempt = request.attempt;
  observation.observed = request.observed;
  observation.source = request.source;
  observation.verification = request.verification;
  observation.tick = request.tick;
  observation.generation = next_effect.value();
  observation.base_revision = request.base_revision;
  observation.authority_epoch = request.authority_epoch;
  observation.incarnation = request.incarnation;
  observation.plan_digest_at_observation = plan.value().content_digest();

  ObservedLoadState observed;
  observed.load = request.load;
  observed.state = request.observed;
  observed.verification = request.verification;
  observed.since = request.tick;
  observed.generation = next_effect.value();
  observed.sequence = observation.sequence;
  observed.source_plan = request.plan_id;
  observed.source_plan_generation = request.plan_generation;
  observed.attempt = request.attempt;

  impl.state.effects.push_back(observation);
  impl.state.effect_generation = next_effect.value();
  impl.state.tick = request.tick;
  impl.upsert_observed(observed);
  impl.append_audit(AuditKind::EffectObserved, request.tick, request.request_id, request.plan_id,
                    "load '" + request.load.value() + "' observed " +
                        std::string(to_string(request.observed)) + " (" +
                        std::string(to_string(request.verification)) + ", " +
                        std::string(to_string(request.source)) + ")");
  impl.remember_request(request.request_id, request_digest, observation.record_digest(), PlanId{},
                        PlanGeneration{}, false, RequestId{}, false, request.tick);
  const Status published = impl.publish("the effect record");
  if (!published.ok()) {
    return published;
  }
  return observation;
}

Result<PlanDiff> Engine::diff_plans(PlanId left, PlanId right) const {
  if (impl_ == nullptr) {
    return Status::error(StatusCode::Closed, "the engine is closed");
  }
  std::shared_lock<std::shared_mutex> guard(impl_->mutex);
  const Status open = impl_->check_open();
  if (!open.ok()) {
    return open;
  }
  auto left_plan = impl_->find_plan(left);
  if (!left_plan.ok()) {
    return left_plan.status();
  }
  auto right_plan = impl_->find_plan(right);
  if (!right_plan.ok()) {
    return right_plan.status();
  }
  PlanDiff diff;
  diff.left_plan = left;
  diff.left_generation = left_plan.value().identity.generation;
  diff.left_digest = left_plan.value().content_digest();
  diff.right_plan = right;
  diff.right_generation = right_plan.value().identity.generation;
  diff.right_digest = right_plan.value().content_digest();
  diff.left_selected = left_plan.value().closure.selected_expected_reduction;
  diff.right_selected = right_plan.value().closure.selected_expected_reduction;
  diff.left_residual = left_plan.value().closure.residual_deficit;
  diff.right_residual = right_plan.value().closure.residual_deficit;

  for (const ShedAction& action : right_plan.value().actions) {
    const ShedAction* previous = left_plan.value().find_action(action.load);
    if (previous == nullptr) {
      PlanDiffEntry entry;
      entry.load = action.load;
      entry.kind = PlanDiffKind::Added;
      entry.right_stage = action.stage;
      entry.right_contribution = action.expected_contribution;
      diff.entries.push_back(entry);
      continue;
    }
    if (!(previous->stage == action.stage)) {
      PlanDiffEntry entry;
      entry.load = action.load;
      entry.kind = PlanDiffKind::StageChanged;
      entry.left_stage = previous->stage;
      entry.right_stage = action.stage;
      entry.left_contribution = previous->expected_contribution;
      entry.right_contribution = action.expected_contribution;
      diff.entries.push_back(entry);
    }
    if (diff.entries.size() > limits::kMaxPlanDiffEntries) {
      return Status::error(StatusCode::LimitExceeded, "plan diff exceeds the entry bound");
    }
  }
  for (const ShedAction& action : left_plan.value().actions) {
    if (right_plan.value().find_action(action.load) == nullptr) {
      PlanDiffEntry entry;
      entry.load = action.load;
      entry.kind = PlanDiffKind::Removed;
      entry.left_stage = action.stage;
      entry.left_contribution = action.expected_contribution;
      diff.entries.push_back(entry);
    }
  }
  std::sort(diff.entries.begin(), diff.entries.end(),
            [](const PlanDiffEntry& a, const PlanDiffEntry& b) { return a.load < b.load; });
  return diff;
}

Result<RevalidationReport> Engine::revalidate(PlanId id) const {
  if (impl_ == nullptr) {
    return Status::error(StatusCode::Closed, "the engine is closed");
  }
  std::shared_lock<std::shared_mutex> guard(impl_->mutex);
  const Status open = impl_->check_open();
  if (!open.ok()) {
    return open;
  }
  RevalidationReport report;
  report.plan = id;
  auto plan = impl_->find_plan(id);
  if (!plan.ok()) {
    report.verdict = RevalidationVerdict::PlanUnknown;
    report.reasons.push_back(plan.message());
    return report;
  }
  const SheddingPlan& stored = plan.value();
  report.generation = stored.identity.generation;
  report.recorded_digest = stored.identity.evidence_generations.empty()
                               ? Digest()
                               : Digest();
  const Digest recomputed = stored.content_digest();
  report.recomputed_digest = recomputed;
  report.recorded_digest = recomputed;

  // An effect record or recovery decision that pinned a different digest proves
  // the plan was rewritten after it was observed, which must never happen.
  for (const EffectObservation& observation : impl_->state.effects) {
    if (observation.plan_id == id && !(observation.plan_digest_at_observation == recomputed)) {
      report.recorded_digest = observation.plan_digest_at_observation;
      report.verdict = RevalidationVerdict::DigestMismatch;
      report.reasons.push_back("effect record sequence " +
                               std::to_string(observation.sequence.value()) +
                               " pinned a different plan digest");
      return report;
    }
  }
  for (const RecoveryDecision& decision : impl_->state.recoveries) {
    if (decision.source_plan == id && !(decision.source_plan_digest == recomputed)) {
      report.recorded_digest = decision.source_plan_digest;
      report.verdict = RevalidationVerdict::DigestMismatch;
      report.reasons.push_back("recovery decision for request " +
                               std::to_string(decision.request_id.value()) +
                               " pinned a different plan digest");
      return report;
    }
  }

  const Status self_check = stored.verify();
  if (!self_check.ok()) {
    report.verdict = RevalidationVerdict::DigestMismatch;
    report.reasons.push_back(self_check.message());
    return report;
  }

  if (!(stored.identity.authority_epoch == impl_->state.authority_epoch)) {
    report.verdict = RevalidationVerdict::AuthorityMoved;
    report.reasons.push_back("plan was issued at authority epoch " +
                             std::to_string(stored.identity.authority_epoch.value()) +
                             " but the store is fenced at epoch " +
                             std::to_string(impl_->state.authority_epoch.value()));
    return report;
  }
  if (!(stored.identity.policy_generation == impl_->state.policy_generation)) {
    report.verdict = RevalidationVerdict::SupersededPolicy;
    report.reasons.push_back("policy generation moved from " +
                             std::to_string(stored.identity.policy_generation.value()) + " to " +
                             std::to_string(impl_->state.policy_generation.value()));
    return report;
  }
  if (!(stored.identity.evidence_generation == impl_->state.evidence_generation)) {
    report.verdict = RevalidationVerdict::SupersededEvidence;
    report.reasons.push_back("evidence generation moved from " +
                             std::to_string(stored.identity.evidence_generation.value()) + " to " +
                             std::to_string(impl_->state.evidence_generation.value()));
    return report;
  }
  if (!(stored.identity.effect_generation == impl_->state.effect_generation)) {
    report.verdict = RevalidationVerdict::SupersededEffect;
    report.reasons.push_back("effect generation moved from " +
                             std::to_string(stored.identity.effect_generation.value()) + " to " +
                             std::to_string(impl_->state.effect_generation.value()));
    return report;
  }
  if (!(stored.identity.base_revision == impl_->state.revision)) {
    report.verdict = RevalidationVerdict::SupersededRevision;
    report.reasons.push_back("state revision moved from " +
                             std::to_string(stored.identity.base_revision.value()) + " to " +
                             std::to_string(impl_->state.revision.value()));
    return report;
  }
  report.verdict = RevalidationVerdict::Current;
  report.reasons.push_back("authority, policy, evidence, effect state, and revision are unchanged");
  return report;
}

SheddingPolicy Engine::policy() const {
  if (impl_ == nullptr) {
    return SheddingPolicy{};
  }
  std::shared_lock<std::shared_mutex> guard(impl_->mutex);
  return impl_->state.policy;
}

FacilitySnapshot Engine::snapshot() const {
  if (impl_ == nullptr) {
    return FacilitySnapshot{};
  }
  std::shared_lock<std::shared_mutex> guard(impl_->mutex);
  return impl_->state.snapshot;
}

std::vector<ObservedLoadState> Engine::observed_states() const {
  if (impl_ == nullptr) {
    return {};
  }
  std::shared_lock<std::shared_mutex> guard(impl_->mutex);
  return impl_->state.observed;
}

Result<ObservedLoadState> Engine::observed_state(const LoadRef& load) const {
  if (impl_ == nullptr) {
    return Status::error(StatusCode::Closed, "the engine is closed");
  }
  std::shared_lock<std::shared_mutex> guard(impl_->mutex);
  const ObservedLoadState* state = impl_->state.find_observed(load);
  if (state == nullptr) {
    return Status::error(StatusCode::NotFound,
                         "no observed state is recorded for load '" + load.value() + "'");
  }
  return *state;
}

Result<SheddingPlan> Engine::plan_at(std::size_t index) const {
  if (impl_ == nullptr) {
    return Status::error(StatusCode::Closed, "the engine is closed");
  }
  if (index >= impl_->state.plans.size()) {
    return Status::error(StatusCode::NotFound, "plan index is out of range");
  }
  return impl_->state.plans[index];
}

std::size_t Engine::plan_count() const {
  if (impl_ == nullptr) {
    return 0;
  }
  std::shared_lock<std::shared_mutex> guard(impl_->mutex);
  return impl_->state.plans.size();
}

std::vector<EffectObservation> Engine::effects() const {
  if (impl_ == nullptr) {
    return {};
  }
  std::shared_lock<std::shared_mutex> guard(impl_->mutex);
  return impl_->state.effects;
}

std::vector<RecoveryDecision> Engine::recovery_decisions() const {
  if (impl_ == nullptr) {
    return {};
  }
  std::shared_lock<std::shared_mutex> guard(impl_->mutex);
  return impl_->state.recoveries;
}

Result<HistoryPage> Engine::history(const HistoryQuery& query) const {
  if (impl_ == nullptr) {
    return Status::error(StatusCode::Closed, "the engine is closed");
  }
  std::shared_lock<std::shared_mutex> guard(impl_->mutex);
  const Status open = impl_->check_open();
  if (!open.ok()) {
    return open;
  }
  if (query.limit > limits::kMaxHistoryPage) {
    return Status::error(StatusCode::LimitExceeded,
                         "history page limit exceeds " + std::to_string(limits::kMaxHistoryPage));
  }
  HistoryPage page;
  page.total_entries = impl_->state.audit.size();
  for (const AuditEntry& entry : impl_->state.audit) {
    if (query.kind.has_value() && !(entry.kind == query.kind.value())) {
      continue;
    }
    if (query.plan.has_value() && !(entry.plan == query.plan.value())) {
      continue;
    }
    if (page.total_matching >= query.offset && page.entries.size() < query.limit) {
      page.entries.push_back(entry);
    }
    ++page.total_matching;
  }
  return page;
}

Result<VerificationReport> Engine::verify() const {
  if (impl_ == nullptr) {
    return Status::error(StatusCode::Closed, "the engine is closed");
  }
  std::shared_lock<std::shared_mutex> guard(impl_->mutex);
  const Status open = impl_->check_open();
  if (!open.ok()) {
    return open;
  }
  VerificationReport report;
  report.revision = impl_->state.revision;
  report.checks.push_back("state.structural");
  const Status structural = impl_->state.validate();
  if (!structural.ok()) {
    report.failures.push_back(VerificationFinding{"state.structural", structural.message(), Digest()});
  }
  for (const SheddingPlan& plan : impl_->state.plans) {
    report.checks.push_back("plan.invariants");
    const Status verified = plan.verify();
    if (!verified.ok()) {
      report.failures.push_back(VerificationFinding{
          "plan.invariants", verified.message(), plan.content_digest()});
    }
    ++report.plans_verified;
  }
  for (const RecoveryDecision& decision : impl_->state.recoveries) {
    report.checks.push_back("recovery.invariants");
    const Status verified = decision.verify();
    if (!verified.ok()) {
      report.failures.push_back(VerificationFinding{"recovery.invariants", verified.message(),
                                                    decision.content_digest()});
    }
    ++report.recovery_decisions_verified;
  }
  for (const EffectObservation& observation : impl_->state.effects) {
    report.checks.push_back("effect.record");
    ++report.effect_records_verified;
    if (observation.plan_id.is_set()) {
      const SheddingPlan* plan = impl_->state.find_plan(observation.plan_id);
      if (plan != nullptr && !(plan->content_digest() == observation.plan_digest_at_observation)) {
        report.failures.push_back(VerificationFinding{
            "effect.record", "effect record pins a plan digest that the plan no longer reproduces",
            observation.record_digest()});
      }
    }
  }
  report.audit_entries_verified = impl_->state.audit.size();
  report.checks.push_back("history.sequence-monotonic");
  for (std::size_t index = 1; index < impl_->state.audit.size(); ++index) {
    if (!(impl_->state.audit[index - 1].sequence < impl_->state.audit[index].sequence)) {
      report.failures.push_back(
          VerificationFinding{"history.sequence-monotonic", "audit sequence is not increasing", Digest()});
      break;
    }
  }
  report.checks.push_back("store.committed-generation");
  auto encoded = detail::encode_state(impl_->state);
  if (!encoded.ok()) {
    report.failures.push_back(
        VerificationFinding{"store.committed-generation", encoded.message(), Digest()});
    report.state_digest = Digest();
  } else {
    report.state_digest = Digest::of(encoded.value());
    if (impl_->store.commit_record().generation == 0) {
      if (!impl_->state.plans.empty() || impl_->state.policy_installed) {
        report.failures.push_back(VerificationFinding{
            "store.committed-generation", "state exists in memory but no generation is committed",
            report.state_digest});
      }
    } else {
      auto on_disk = impl_->store.load_committed();
      if (!on_disk.ok()) {
        report.failures.push_back(
            VerificationFinding{"store.committed-generation", on_disk.message(), report.state_digest});
      } else {
        auto encoded_disk = detail::encode_state(on_disk.value());
        if (!encoded_disk.ok() || !(Digest::of(encoded_disk.value()) == report.state_digest)) {
          report.failures.push_back(VerificationFinding{
              "store.committed-generation",
              "the committed generation does not reproduce the in-memory state", report.state_digest});
        }
      }
    }
  }
  report.ok = report.failures.empty();
  return report;
}

Result<StoreAudit> Engine::audit_store() const {
  auto audit = inspect_store(store_directory());
  if (!audit.ok()) {
    return audit;
  }
  // This engine holds the store lock, so its findings are synchronised.
  if (impl_ != nullptr && impl_->lock != nullptr && impl_->lock->held()) {
    audit.value().unsynchronized = false;
  }
  return audit;
}

Result<std::string> Engine::canonical_state_bytes() const {
  if (impl_ == nullptr) {
    return Status::error(StatusCode::Closed, "the engine is closed");
  }
  std::shared_lock<std::shared_mutex> guard(impl_->mutex);
  const Status open = impl_->check_open();
  if (!open.ok()) {
    return open;
  }
  return detail::encode_state(impl_->state);
}

Digest Engine::state_digest() const {
  auto encoded = canonical_state_bytes();
  if (!encoded.ok()) {
    return Digest();
  }
  return Digest::of(encoded.value());
}

CommitRecord Engine::commit() const {
  if (impl_ == nullptr) {
    return CommitRecord{};
  }
  std::shared_lock<std::shared_mutex> guard(impl_->mutex);
  return impl_->store.commit_record();
}

StateRevision Engine::revision() const {
  if (impl_ == nullptr) {
    return StateRevision{};
  }
  std::shared_lock<std::shared_mutex> guard(impl_->mutex);
  return impl_->state.revision;
}

PolicyGeneration Engine::policy_generation() const {
  if (impl_ == nullptr) {
    return PolicyGeneration{};
  }
  std::shared_lock<std::shared_mutex> guard(impl_->mutex);
  return impl_->state.policy_generation;
}

EvidenceGeneration Engine::evidence_generation() const {
  if (impl_ == nullptr) {
    return EvidenceGeneration{};
  }
  std::shared_lock<std::shared_mutex> guard(impl_->mutex);
  return impl_->state.evidence_generation;
}

EffectGeneration Engine::effect_generation() const {
  if (impl_ == nullptr) {
    return EffectGeneration{};
  }
  std::shared_lock<std::shared_mutex> guard(impl_->mutex);
  return impl_->state.effect_generation;
}

PlanGeneration Engine::plan_generation() const {
  if (impl_ == nullptr) {
    return PlanGeneration{};
  }
  std::shared_lock<std::shared_mutex> guard(impl_->mutex);
  return impl_->state.plan_generation;
}

Tick Engine::tick() const {
  if (impl_ == nullptr) {
    return Tick{};
  }
  std::shared_lock<std::shared_mutex> guard(impl_->mutex);
  return impl_->state.tick;
}

std::string Engine::store_directory() const {
  if (impl_ == nullptr) {
    return std::string();
  }
  return impl_->options.store_directory;
}

}  // namespace load_shedding
