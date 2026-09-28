#pragma once

// Recovery is a decision, not the inverse of shedding. A load becomes a
// restoration candidate only when downstream evidence says it is actually off,
// and it is restored in a documented deterministic order bounded by the headroom
// that capacity recovery made available.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "load_shedding/digest.hpp"
#include "load_shedding/effect.hpp"
#include "load_shedding/ids.hpp"
#include "load_shedding/plan.hpp"
#include "load_shedding/status.hpp"

namespace load_shedding {

enum class RecoveryVerdict : std::uint8_t {
  /// Selected for restoration by this decision.
  Restored = 0,
  /// Eligible, but the remaining headroom could not absorb its contribution.
  SkippedHeadroom,
  /// The plan selected it, but no downstream evidence says it is off.
  AwaitingEffectConfirmation,
  /// The minimum off time required by policy has not elapsed.
  MinOffNotElapsed,
  /// The plan did not select this load.
  NotShedByPlan,
  /// Downstream evidence reports it energized while the plan selected it.
  EvidenceContradicted,
  /// Downstream evidence reports an unusable state.
  UnknownObservedState,
};

struct RecoveryCandidate {
  LoadRef load;
  StageIndex stage;
  PriorityClass priority = PriorityClass::Standard;
  LoadClass load_class = LoadClass::Sheddable;
  Power contribution;
  Tick shed_tick;
  EffectState observed = EffectState::Unknown;
  EffectVerification verification = EffectVerification::Acknowledged;
  EffectGeneration observed_generation;
  RecoveryVerdict verdict = RecoveryVerdict::NotShedByPlan;
  /// Position in the recovery ordering, when the load was a candidate at all.
  std::uint32_t order_index = 0;
  bool order_index_set = false;
  /// True when the candidate was shed by the source plan.
  bool shed_by_plan = false;
};

struct RecoveryRequest {
  RequestId request_id;
  PlanId plan_id;
  PlanGeneration plan_generation;
  /// Power that recovered capacity made available for restoration.
  Power available_headroom;
  PolicyGeneration policy_generation;
  EvidenceGeneration evidence_generation;
  EffectGeneration effect_generation;
  StateRevision base_revision;
  AuthorityEpoch authority_epoch;
  Incarnation incarnation;
  Tick tick;

  Digest digest() const;
};

struct RecoveryDecision {
  RequestId request_id;
  PlanId source_plan;
  PlanGeneration source_plan_generation;
  Digest source_plan_digest;
  PolicyGeneration policy_generation;
  EvidenceGeneration evidence_generation;
  EffectGeneration effect_generation;
  StateRevision base_revision;
  AuthorityEpoch authority_epoch;
  Incarnation incarnation;
  Tick tick;
  RecoveryOrder order = RecoveryOrder::ReverseStageThenPriority;

  Power requested_headroom;
  Power restored_expected;
  Power remaining_headroom;
  std::uint64_t restored_count = 0;
  std::uint64_t candidate_count = 0;
  std::uint64_t awaiting_confirmation_count = 0;

  /// Candidates in recovery order first (each with its order index), followed by
  /// every other considered load ordered by reference.
  std::vector<RecoveryCandidate> candidates;

  Digest content_digest() const;
  Status verify() const;
};

std::string_view to_string(RecoveryVerdict value) noexcept;

namespace recovery_invariants {
inline constexpr std::string_view kRestoredWithinHeadroom = "recovery.restored-within-headroom";
inline constexpr std::string_view kRestoredSumMatches = "recovery.restored-sum-matches";
inline constexpr std::string_view kNoUnconfirmedRestored = "recovery.no-restored-without-observed-shed";
inline constexpr std::string_view kMinOffRespected = "recovery.minimum-off-time-respected";
inline constexpr std::string_view kOrderDense = "recovery.order-indices-dense";
inline constexpr std::string_view kHeadroomAccounted = "recovery.headroom-accounted";
}  // namespace recovery_invariants

}  // namespace load_shedding
