#pragma once

// The authoritative state object. One `StoreState` is exactly what a committed
// store generation contains: it is encoded canonically, hashed, published, and
// adopted whole. There is no partial adoption path.
//
// Internal header: not installed.

#include <cstdint>
#include <string>
#include <vector>

#include "load_shedding/effect.hpp"
#include "load_shedding/facility.hpp"
#include "load_shedding/history.hpp"
#include "load_shedding/ids.hpp"
#include "load_shedding/plan.hpp"
#include "load_shedding/policy.hpp"
#include "load_shedding/recovery.hpp"
#include "load_shedding/status.hpp"
#include "load_shedding/store.hpp"

namespace load_shedding::detail {

/// Bounded replay record. Retention is a sliding window of
/// `limits::kMaxIdempotencyEntries` requests, most recent first. A replay whose
/// record has been evicted is reported as `NotFound`, never re-planned under the
/// old identity.
struct IdempotencyRecord {
  RequestId request;
  Digest request_digest;
  Digest result_digest;
  PlanId plan;
  PlanGeneration plan_generation;
  bool has_plan = false;
  RequestId recovery_request;
  bool has_recovery = false;
  Tick tick;
};

struct StoreState {
  std::uint32_t format_version = store_format::kFormatVersion;

  StateRevision revision;
  PolicyGeneration policy_generation;
  EvidenceGeneration evidence_generation;
  EffectGeneration effect_generation;
  PlanGeneration plan_generation;
  Tick tick;

  /// Authority epoch and incarnation of the writer that produced this generation.
  AuthorityEpoch authority_epoch;
  Incarnation incarnation;

  std::uint64_t next_sequence = 1;
  bool policy_installed = false;

  SheddingPolicy policy;
  FacilitySnapshot snapshot;
  /// Observed load states, sorted by load reference.
  std::vector<ObservedLoadState> observed;
  /// Retained plans, ordered by ascending plan id.
  std::vector<SheddingPlan> plans;
  /// Retained recovery decisions, most recent first.
  std::vector<RecoveryDecision> recoveries;
  /// Retained effect records, ordered by ascending sequence.
  std::vector<EffectObservation> effects;
  /// Retained audit entries, ordered by ascending sequence.
  std::vector<AuditEntry> audit;
  /// Replay window, most recent first.
  std::vector<IdempotencyRecord> idempotency;

  /// Structural validation of the whole state. Called before encoding and after
  /// decoding: a state that fails this is never written and never adopted.
  Status validate() const;

  const SheddingPlan* find_plan(PlanId plan) const;
  const IdempotencyRecord* find_idempotency(RequestId request) const;
  const ObservedLoadState* find_observed(const LoadRef& load) const;
};

/// Retention limits applied when a state grows past its configured bounds.
struct RetentionLimits {
  std::size_t plans = limits::kMaxPlansRetained;
  std::size_t effects = limits::kMaxEffectRecords;
  std::size_t audit = limits::kMaxAuditEntries;
  std::size_t idempotency = limits::kMaxIdempotencyEntries;
  std::size_t recoveries = limits::kMaxRecoveryDecisionsRetained;
};

/// Evicts the oldest retained artifacts until the state fits its limits.
/// Deterministic: plans are evicted oldest first, then effect records, then audit
/// entries, then recovery decisions, then replay records.
void apply_retention(StoreState& state, const RetentionLimits& limits);

}  // namespace load_shedding::detail
