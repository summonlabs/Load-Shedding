#pragma once

// The Load Shedding engine: the only entry point through which policy, facility
// evidence, plans, recovery decisions, and downstream effect records are read or
// mutated.
//
// Ownership model
// ---------------
// One engine instance owns one store directory. Writer authority is a real
// operating-system file lock plus a fencing epoch and incarnation recorded in the
// commit marker, so a writer that lost authority (because it was replaced, or
// because its process died and the lock was released) is refused at commit time
// rather than silently merged.
//
// Concurrency model
// -----------------
// The engine is safe to call from several threads. Methods are internally
// serialized; the lock order is always (1) the cross-process store lock, then
// (2) the in-process state lock, and no internal helper ever re-enters a public
// method. No callback, user type, or external process is invoked while the state
// lock is held.

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "load_shedding/digest.hpp"
#include "load_shedding/effect.hpp"
#include "load_shedding/facility.hpp"
#include "load_shedding/history.hpp"
#include "load_shedding/ids.hpp"
#include "load_shedding/plan.hpp"
#include "load_shedding/policy.hpp"
#include "load_shedding/recovery.hpp"
#include "load_shedding/status.hpp"
#include "load_shedding/store.hpp"

namespace load_shedding {

struct EngineOptions {
  std::string store_directory;
  /// Identity of this writer. Must be stable for the lifetime of the instance.
  Incarnation incarnation;
  /// Acquire exclusive writer authority on open. A read-only engine cannot mutate
  /// and cannot commit.
  bool take_authority = true;
  bool create_if_missing = true;
  /// Authority epoch used when the store has no commit marker yet.
  AuthorityEpoch initial_authority_epoch = AuthorityEpoch::first();
  Tick initial_tick;
  std::size_t max_plans_retained = limits::kMaxPlansRetained;
  std::size_t max_effect_records = limits::kMaxEffectRecords;
  std::size_t max_audit_entries = limits::kMaxAuditEntries;
  std::size_t max_idempotency_entries = limits::kMaxIdempotencyEntries;
  /// Install the default policy when the store has none.
  bool install_default_policy_if_missing = false;
  /// Deterministic fault-injection point for durability testing. One of
  /// "none" (default), "after-staging-write", "after-state-publish",
  /// "before-head-commit", "after-head-commit", "after-watermark". When the
  /// named durable stage is reached, the process terminates immediately without
  /// unwinding and without any interactive error-reporting path. It is a
  /// deliberate, documented test hook, not a runtime mode.
  ///
  /// The hook applies to mutations performed through the returned engine. The
  /// authority takeover that `open` itself performs is never faulted, so a caller
  /// that asks to crash at a durable stage is asking about its own commit.
  std::string crash_after;
};

struct PlanRequest {
  RequestId request_id;
  Power requested_reduction;
  PolicyGeneration policy_generation;
  EvidenceGeneration evidence_generation;
  EffectGeneration effect_generation;
  StateRevision base_revision;
  AuthorityEpoch authority_epoch;
  Incarnation incarnation;
  Tick tick;
  /// Requests admission to stages marked `requires_emergency_authority`.
  bool emergency_authority = false;
  /// Required when `emergency_authority` is set. Recorded in the plan and in
  /// history.
  std::string emergency_justification;

  /// Canonical digest of the request content. Replays are detected with this, so
  /// the same request identity with different content is refused rather than
  /// served from the idempotency window.
  Digest digest() const;
};

struct PlanOutcome {
  SheddingPlan plan;
  AttemptId attempt;
  /// True when this call returned a previously committed plan from the
  /// idempotency window instead of planning again.
  bool replayed = false;
  Digest request_digest;
};

enum class RevalidationVerdict : std::uint8_t {
  /// The plan still describes current authority, policy, evidence, and effect
  /// state, and its content digest re-derives.
  Current = 0,
  SupersededPolicy = 1,
  SupersededEvidence = 2,
  SupersededEffect = 3,
  /// State moved on since the plan was computed.
  SupersededRevision = 4,
  /// Authority moved to another epoch or incarnation.
  AuthorityMoved = 5,
  /// The plan is not retained any more.
  PlanUnknown = 6,
  /// The stored bytes no longer hash to the recorded digest.
  DigestMismatch = 7,
};

struct RevalidationReport {
  PlanId plan;
  PlanGeneration generation;
  Digest recorded_digest;
  Digest recomputed_digest;
  RevalidationVerdict verdict = RevalidationVerdict::PlanUnknown;
  std::vector<std::string> reasons;
};

struct VerificationFinding {
  std::string check;
  std::string detail;
  Digest subject;
};

struct VerificationReport {
  bool ok = true;
  StateRevision revision;
  Digest state_digest;
  std::size_t plans_verified = 0;
  std::size_t recovery_decisions_verified = 0;
  std::size_t effect_records_verified = 0;
  std::size_t audit_entries_verified = 0;
  std::vector<std::string> checks;
  std::vector<VerificationFinding> failures;
};

/// Immutable authority view of one engine instance.
struct AuthorityStatus {
  AuthorityEpoch epoch;
  Incarnation incarnation;
  bool writer = false;
  std::string store_directory;
  std::uint64_t state_generation = 0;
};

class Engine {
 public:
  /// Opens a store. Never adopts a partial state: the commit marker names exactly
  /// one generation, which is verified before it is adopted, and anything else on
  /// disk is refused or retired according to the documented protocol.
  static Result<Engine> open(const EngineOptions& options);

  Engine();
  ~Engine();
  Engine(Engine&& other) noexcept;
  Engine& operator=(Engine&& other) noexcept;
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  Status close();

  // ---- authority -----------------------------------------------------------
  AuthorityStatus authority() const;
  bool has_writer_authority() const;
  /// Takes writer authority, bumping the fencing epoch. Refused when another
  /// incarnation holds the store lock, or when the epoch asked for is not ahead
  /// of the current one.
  Result<AuthorityEpoch> take_authority(Incarnation incarnation, AuthorityEpoch requested_epoch);

  // ---- configuration -------------------------------------------------------
  Result<PolicyGeneration> install_policy(const SheddingPolicy& policy,
                                          AuthorityEpoch authority_epoch,
                                          Incarnation incarnation);
  Result<StateRevision> upsert_load(const LoadRecord& load, AuthorityEpoch authority_epoch,
                                    Incarnation incarnation);
  Result<StateRevision> remove_load(const LoadRef& load, AuthorityEpoch authority_epoch,
                                    Incarnation incarnation);
  Result<StateRevision> upsert_obligation(const ProtectedObligation& obligation,
                                          AuthorityEpoch authority_epoch, Incarnation incarnation);
  Result<StateRevision> remove_obligation(const ObligationRef& obligation,
                                          AuthorityEpoch authority_epoch, Incarnation incarnation);
  Result<StateRevision> replace_snapshot(const FacilitySnapshot& snapshot,
                                         AuthorityEpoch authority_epoch, Incarnation incarnation);

  // ---- decisions -----------------------------------------------------------
  Result<PlanOutcome> plan(const PlanRequest& request);
  Result<SheddingPlan> plan_by_id(PlanId id) const;
  Result<SheddingPlan> latest_plan() const;
  Result<RecoveryDecision> recovery(const RecoveryRequest& request);
  Result<RecoveryDecision> recovery_by_request(RequestId id) const;
  Result<PlanDiff> diff_plans(PlanId left, PlanId right) const;
  Result<RevalidationReport> revalidate(PlanId id) const;

  // ---- downstream evidence -------------------------------------------------
  Result<EffectObservation> observe_effect(const EffectObservationRequest& request);

  // ---- inspection ----------------------------------------------------------
  //
  // Every accessor returns a snapshot value, never a reference into mutable
  // state, so a concurrent mutation cannot invalidate a caller's view.
  SheddingPolicy policy() const;
  FacilitySnapshot snapshot() const;
  std::vector<ObservedLoadState> observed_states() const;
  Result<ObservedLoadState> observed_state(const LoadRef& load) const;
  Result<SheddingPlan> plan_at(std::size_t index) const;
  std::size_t plan_count() const;
  std::vector<EffectObservation> effects() const;
  std::vector<RecoveryDecision> recovery_decisions() const;
  Result<HistoryPage> history(const HistoryQuery& query) const;
  Result<VerificationReport> verify() const;
  Result<StoreAudit> audit_store() const;
  Result<std::string> canonical_state_bytes() const;
  Digest state_digest() const;
  CommitRecord commit() const;
  StateRevision revision() const;
  PolicyGeneration policy_generation() const;
  EvidenceGeneration evidence_generation() const;
  EffectGeneration effect_generation() const;
  PlanGeneration plan_generation() const;
  Tick tick() const;
  std::string store_directory() const;
  /// Replays the most recent recovery decision identity, if retained.
  Result<RecoveryDecision> last_recovery_decision() const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

std::string_view to_string(RevalidationVerdict value) noexcept;

}  // namespace load_shedding
