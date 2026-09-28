#pragma once

// A shedding plan is the only artifact this runtime issues. It states which loads
// may be shed, in which deterministic stage, with what expected contribution, and
// exactly how much of the requested deficit remains. It is an authority-bound
// decision, not an actuation: nothing in this header claims that a selected load
// was switched off.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "load_shedding/digest.hpp"
#include "load_shedding/facility.hpp"
#include "load_shedding/ids.hpp"
#include "load_shedding/policy.hpp"
#include "load_shedding/status.hpp"
#include "load_shedding/units.hpp"

namespace load_shedding {

/// Verdict for one considered load. Exactly one verdict is recorded per load in
/// the snapshot, so the explanation trace is total: there is no load whose
/// treatment is unstated.
enum class LoadVerdict : std::uint8_t {
  Selected = 0,
  /// The request was already covered by earlier selections.
  SkippedCoverageMet,
  /// The load's stage power cap was reached.
  SkippedStageCap,
  /// The load's stage load-count cap was reached.
  SkippedStageLoadCap,
  /// No-overshoot mode: the whole contribution would exceed the request.
  SkippedNoOvershoot,
  /// Load class is `Protected`.
  ProtectedClass,
  /// Load class is `NonSheddable`.
  NonSheddableClass,
  /// Priority class is protected by policy.
  ProtectedPriority,
  /// An active protected obligation names this load. Absolute: not overridable.
  ObligationActive,
  /// Load class is not in the policy's shedable classes.
  ClassNotEligible,
  /// No stage covers this load's priority class.
  PriorityNotInPolicy,
  /// The only stage covering this priority requires emergency authority that was
  /// not granted.
  StageEmergencyDenied,
  /// Load is out of service.
  OutOfService,
  EvidenceUnknown,
  EvidenceUnavailable,
  EvidenceUnsupported,
  EvidenceDenied,
  EvidenceUnsafe,
  /// Evidence is older than the policy freshness window.
  EvidenceStale,
  /// Evidence generation lags the snapshot by more than the policy allows.
  EvidenceGenerationLag,
  /// Known contribution is exactly zero.
  ZeroContribution,
  /// Known contribution is negative, which is not a valid sheddable quantity.
  NegativeContribution,
  /// The policy's minimum-on-time constraint is not satisfied. Missing observed
  /// evidence fails closed here: without proof that the load has been energized
  /// long enough, it is not selectable.
  MinOnNotElapsed,
};

/// Why a selected load was selected.
enum class ReasonCode : std::uint8_t {
  /// Admissible under a normal stage.
  StageEligible = 0,
  /// Admitted by a stage that requires emergency authority, with a grant.
  EmergencyStageEligible = 1,
};

/// Why a stage stopped selecting.
enum class StageStopReason : std::uint8_t {
  /// The request was covered while this stage was running.
  Covered = 0,
  /// The stage power cap was reached.
  PowerCapReached = 1,
  /// The stage load-count cap was reached.
  LoadCapReached = 2,
  /// No further admissible load remained in the stage.
  Exhausted = 3,
  /// The stage requires emergency authority that was not granted.
  EmergencyAuthorityMissing = 4,
  /// The stage had no admissible loads at all.
  Empty = 5,
};

/// Whether the plan covers the requested deficit. The four outcomes are distinct
/// and none of them is a synonym for failure.
enum class CoverageStatus : std::uint8_t {
  /// Residual is exactly zero.
  FullyCovered = 0,
  /// Residual is non-zero and at least one admissible load has unknown
  /// contribution: it cannot be proven that the target is unreachable.
  Indeterminate = 1,
  /// Residual is non-zero, no unknown contribution remains, but policy caps
  /// stopped selection while known eligible capacity was still available.
  PolicyLimited = 2,
  /// Residual is non-zero and no known eligible capacity remains.
  Insufficient = 3,
};

/// Checked-integer accounting closure for one plan. Every field is derived from
/// the same pass that produced the selection and is re-derived by `verify()`.
struct AccountingClosure {
  Power requested_reduction;
  /// Sum of known contributions over every load that passed admissibility.
  Power eligible_known_capacity;
  /// Sum of the expected contributions of the selected actions.
  Power selected_expected_reduction;
  /// Known contributions of loads protected by class, priority, or obligation.
  Power protected_known_amount;
  /// Known contributions of loads whose class or priority policy never sheds.
  Power non_sheddable_known_amount;
  /// Known contributions of loads that were not usable for another reason
  /// (out of service, stale, denied, unsafe, unsupported, unavailable).
  Power unavailable_known_amount;
  /// `max(requested - selected, 0)`.
  Power residual_deficit;
  /// `max(selected - requested, 0)`. Recorded, never hidden.
  Power overshoot;
  /// Known contributions left selectable when the plan stopped.
  Power remaining_eligible_known_capacity;

  std::uint64_t eligible_load_count = 0;
  std::uint64_t selected_load_count = 0;
  std::uint64_t protected_load_count = 0;
  std::uint64_t indeterminate_load_count = 0;
  std::uint64_t unavailable_load_count = 0;
  std::uint64_t stale_load_count = 0;
  std::uint64_t denied_load_count = 0;
  std::uint64_t unsafe_load_count = 0;
  std::uint64_t unsupported_load_count = 0;
  std::uint64_t zero_contribution_load_count = 0;

  /// At least one load that could have been selected has an unknown
  /// contribution, so the residual cannot be proven to be irreducible.
  bool capacity_indeterminate = false;
  /// The protected-reserve ceiling check could be performed.
  bool ceiling_verified = false;
  /// `known total demand - reserved protected power`, when verifiable.
  std::optional<Power> effective_shedding_ceiling;
};

struct ShedAction {
  LoadRef load;
  StageIndex stage;
  /// Position in the plan's selection order, starting at 0.
  std::uint32_t selection_index = 0;
  /// Expected contribution. Always exactly the load's known contribution; the
  /// plan never invents a partial amount.
  Power expected_contribution;
  ReasonCode reason = ReasonCode::StageEligible;
  PriorityClass priority = PriorityClass::Standard;
  LoadClass load_class = LoadClass::Sheddable;
  LoadGeneration identity_generation;
  StateRevision load_revision;
  Digest evidence_digest;
};

/// Explanation record for one load considered by the plan.
struct LoadConsideration {
  LoadRef load;
  /// Stage that handled the load, or an unset stage when no stage was reached.
  StageIndex stage;
  PriorityClass priority = PriorityClass::Standard;
  LoadClass load_class = LoadClass::Sheddable;
  EvidenceState evidence_state = EvidenceState::Unknown;
  LoadVerdict verdict = LoadVerdict::EvidenceUnknown;
  /// Known contribution, or zero when the evidence state is not `Known`. The
  /// evidence state field is what disambiguates zero from unknown.
  Power contribution;
  LoadGeneration identity_generation;
  StateRevision load_revision;
  EvidenceGeneration evidence_generation;
  Tick evidence_tick;
  /// Position of the load in the deterministic candidate ordering, when it was
  /// admissible. Unset otherwise.
  std::uint32_t order_index = 0;
  bool order_index_set = false;
};

struct StageSummary {
  StageIndex index;
  std::string name;
  std::uint32_t considered = 0;
  std::uint32_t admissible = 0;
  std::uint32_t selected = 0;
  Power selected_power;
  std::optional<Power> effective_power_cap;
  std::optional<std::uint32_t> load_cap;
  StageStopReason stop_reason = StageStopReason::Empty;
  bool emergency_authority_required = false;
};

/// Identity and provenance of a plan. Every counter here is a distinct type.
struct PlanIdentity {
  PlanId plan_id;
  PlanGeneration generation;
  RequestId request_id;
  PolicyGeneration policy_generation;
  Digest policy_digest;
  StateRevision base_revision;
  EvidenceGeneration evidence_generation;
  EffectGeneration effect_generation;
  /// Sorted, unique set of every evidence generation consulted.
  std::vector<EvidenceGeneration> evidence_generations;
  AuthorityEpoch authority_epoch;
  Incarnation incarnation;
  Tick tick;
  AttemptId attempt;
};

struct SheddingPlan {
  PlanIdentity identity;
  SelectionMode mode = SelectionMode::WholeLoadGreedy;
  CoverageStatus coverage = CoverageStatus::Insufficient;
  AccountingClosure closure;
  bool emergency_authority_used = false;
  std::string emergency_justification;
  /// Selected actions in selection order.
  std::vector<ShedAction> actions;
  /// One consideration per load in the snapshot, ordered by load reference.
  std::vector<LoadConsideration> considerations;
  /// One summary per stage, ordered by stage index.
  std::vector<StageSummary> stages;

  /// Canonical digest of the whole plan content.
  Digest content_digest() const;

  /// Re-derives the closure and every documented invariant from the plan's own
  /// content. Returns `Ok` when the plan is self-consistent.
  Status verify() const;

  const ShedAction* find_action(const LoadRef& ref) const;
  const LoadConsideration* find_consideration(const LoadRef& ref) const;
};

/// Structural change between two plans, computed from their recorded content.
enum class PlanDiffKind : std::uint8_t {
  Added = 0,
  Removed = 1,
  StageChanged = 2,
};

struct PlanDiffEntry {
  LoadRef load;
  PlanDiffKind kind = PlanDiffKind::Added;
  std::optional<StageIndex> left_stage;
  std::optional<StageIndex> right_stage;
  Power left_contribution;
  Power right_contribution;
};

struct PlanDiff {
  PlanId left_plan;
  PlanGeneration left_generation;
  Digest left_digest;
  PlanId right_plan;
  PlanGeneration right_generation;
  Digest right_digest;
  Power left_selected;
  Power right_selected;
  Power left_residual;
  Power right_residual;
  std::vector<PlanDiffEntry> entries;
};

std::string_view to_string(LoadVerdict value) noexcept;
std::string_view to_string(ReasonCode value) noexcept;
std::string_view to_string(StageStopReason value) noexcept;
std::string_view to_string(CoverageStatus value) noexcept;
Result<LoadVerdict> load_verdict_from_string(std::string_view text);
Result<CoverageStatus> coverage_status_from_string(std::string_view text);

/// Invariant identifiers reported by `SheddingPlan::verify()`. These names are
/// stable and are what the test suite asserts against.
namespace invariants {
inline constexpr std::string_view kSelectedSumMatches = "closure.selected-sum-matches";
inline constexpr std::string_view kSelectedWithinEligible = "closure.selected-within-eligible-capacity";
inline constexpr std::string_view kClosureIdentity = "closure.requested-equals-selected-plus-residual-minus-overshoot";
inline constexpr std::string_view kNoProtectedSelected = "safety.no-protected-load-selected";
inline constexpr std::string_view kNoStaleSelected = "safety.no-stale-load-selected";
inline constexpr std::string_view kUniqueActions = "selection.actions-unique";
inline constexpr std::string_view kConsiderationTotal = "explanation.one-verdict-per-load";
inline constexpr std::string_view kActionIsSelected = "explanation.selected-action-has-selected-verdict";
inline constexpr std::string_view kActionContributionMatches = "selection.action-contribution-matches-evidence";
inline constexpr std::string_view kCoverageStatusMatches = "closure.coverage-status-matches-residual";
inline constexpr std::string_view kStageOrderRespected = "selection.stage-order-respected";
inline constexpr std::string_view kEmergencyJustified = "authority.emergency-grant-justified";
inline constexpr std::string_view kSelectedCountMatches = "explanation.selected-count-matches";
inline constexpr std::string_view kClosureCountsMatch = "closure.counts-match-classification";
}  // namespace invariants

}  // namespace load_shedding
