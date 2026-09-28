#include "load_shedding/plan.hpp"

#include <algorithm>
#include <string>

#include "detail/serialization.hpp"

namespace load_shedding {
namespace {

Status invariant_failure(std::string_view invariant, const std::string& detail) {
  return Status::error(StatusCode::Corrupt, std::string(invariant) + ": " + detail);
}

Status checked_sum(const std::vector<ShedAction>& actions, Power& total) {
  Power sum = Power::zero();
  for (const ShedAction& action : actions) {
    auto next = checked_add(sum, action.expected_contribution);
    if (!next.ok()) {
      return invariant_failure(invariants::kSelectedSumMatches,
                               "selected contributions overflow signed 64-bit arithmetic");
    }
    sum = next.value();
  }
  total = sum;
  return Status::success();
}

}  // namespace

Digest SheddingPlan::content_digest() const { return detail::plan_content_digest(*this); }

const ShedAction* SheddingPlan::find_action(const LoadRef& ref) const {
  const auto found = std::find_if(actions.begin(), actions.end(),
                                  [&ref](const ShedAction& action) { return action.load == ref; });
  return found == actions.end() ? nullptr : &(*found);
}

const LoadConsideration* SheddingPlan::find_consideration(const LoadRef& ref) const {
  const auto found =
      std::lower_bound(considerations.begin(), considerations.end(), ref,
                       [](const LoadConsideration& consideration, const LoadRef& value) {
                         return consideration.load < value;
                       });
  if (found == considerations.end() || !(found->load == ref)) {
    return nullptr;
  }
  return &(*found);
}

Status SheddingPlan::verify() const {
  // 1. Actions are unique by load reference and their contributions are positive.
  for (std::size_t index = 0; index < actions.size(); ++index) {
    if (actions[index].expected_contribution.is_zero() ||
        actions[index].expected_contribution.is_negative()) {
      return invariant_failure(invariants::kActionContributionMatches,
                               "selected action carries a non-positive expected contribution");
    }
    for (std::size_t other = index + 1; other < actions.size(); ++other) {
      if (actions[index].load == actions[other].load) {
        return invariant_failure(invariants::kUniqueActions,
                                 "load '" + actions[index].load.value() + "' is selected twice");
      }
    }
    if (!actions[index].stage.is_set()) {
      return invariant_failure(invariants::kStageOrderRespected,
                               "selected action carries no stage ordinal");
    }
    if (index > 0 && actions[index].stage < actions[index - 1].stage) {
      return invariant_failure(invariants::kStageOrderRespected,
                               "selection order moves backwards through the stage plan");
    }
    if (actions[index].selection_index != index) {
      return invariant_failure(invariants::kSelectedCountMatches,
                               "selection index " + std::to_string(actions[index].selection_index) +
                                   " does not match plan position " + std::to_string(index));
    }
    const bool stage_known =
        std::any_of(stages.begin(), stages.end(), [this, index](const StageSummary& summary) {
          return summary.index == actions[index].stage;
        });
    if (!stage_known) {
      return invariant_failure(invariants::kStageOrderRespected,
                               "selected action names a stage that the plan does not summarise");
    }
  }

  // 2. The selected total is exactly the sum of the actions.
  Power selected_sum = Power::zero();
  const Status sum_status = checked_sum(actions, selected_sum);
  if (!sum_status.ok()) {
    return sum_status;
  }
  if (selected_sum != closure.selected_expected_reduction) {
    return invariant_failure(invariants::kSelectedSumMatches,
                             "closure records " + closure.selected_expected_reduction.to_string() +
                                 " but the actions sum to " + selected_sum.to_string());
  }

  // 3. Selection never exceeds the known sheddable capacity.
  if (closure.selected_expected_reduction > closure.eligible_known_capacity) {
    return invariant_failure(invariants::kSelectedWithinEligible,
                             "selected " + closure.selected_expected_reduction.to_string() +
                                 " exceeds eligible known capacity " +
                                 closure.eligible_known_capacity.to_string());
  }

  // 4. Closure identity: requested == selected + residual - overshoot.
  auto residual = positive_difference(closure.requested_reduction, closure.selected_expected_reduction);
  if (!residual.ok()) {
    return invariant_failure(invariants::kClosureIdentity, residual.message());
  }
  auto overshoot = positive_difference(closure.selected_expected_reduction, closure.requested_reduction);
  if (!overshoot.ok()) {
    return invariant_failure(invariants::kClosureIdentity, overshoot.message());
  }
  if (residual.value() != closure.residual_deficit || overshoot.value() != closure.overshoot) {
    return invariant_failure(invariants::kClosureIdentity,
                             "recorded residual/overshoot do not match the recomputed values");
  }
  auto recomposed = checked_add(closure.selected_expected_reduction, closure.residual_deficit);
  if (!recomposed.ok()) {
    return invariant_failure(invariants::kClosureIdentity, recomposed.message());
  }
  auto recomposed_exact = checked_sub(recomposed.value(), closure.overshoot);
  if (!recomposed_exact.ok()) {
    return invariant_failure(invariants::kClosureIdentity, recomposed_exact.message());
  }
  if (recomposed_exact.value() != closure.requested_reduction) {
    return invariant_failure(invariants::kClosureIdentity,
                             "requested " + closure.requested_reduction.to_string() +
                                 " is not reproduced by selected + residual - overshoot");
  }

  // 5. One verdict per load, and every selected action's load carries the
  //    Selected verdict with matching, known, positive evidence.
  std::size_t selected_considerations = 0;
  for (std::size_t index = 0; index < considerations.size(); ++index) {
    const LoadConsideration& consideration = considerations[index];
    if (index > 0 && !(considerations[index - 1].load < consideration.load)) {
      return invariant_failure(invariants::kConsiderationTotal,
                               "considerations are not strictly ordered by load reference");
    }
    if (consideration.verdict == LoadVerdict::Selected) {
      ++selected_considerations;
    }
  }
  for (const ShedAction& action : actions) {
    const LoadConsideration* consideration = find_consideration(action.load);
    if (consideration == nullptr) {
      return invariant_failure(invariants::kActionIsSelected,
                               "selected load '" + action.load.value() +
                                   "' has no explanation record");
    }
    if (consideration->verdict != LoadVerdict::Selected) {
      return invariant_failure(invariants::kActionIsSelected,
                               "selected load '" + action.load.value() +
                                   "' carries verdict " +
                                   std::string(to_string(consideration->verdict)));
    }
    if (consideration->evidence_state != EvidenceState::Known) {
      return invariant_failure(invariants::kNoStaleSelected,
                               "selected load '" + action.load.value() +
                                   "' has evidence state " +
                                   std::string(to_string(consideration->evidence_state)));
    }
    if (consideration->contribution != action.expected_contribution) {
      return invariant_failure(invariants::kActionContributionMatches,
                               "selected load '" + action.load.value() +
                                   "' records a different contribution in the explanation trace");
    }
    if (consideration->contribution.is_zero() || consideration->contribution.is_negative()) {
      return invariant_failure(invariants::kActionContributionMatches,
                               "selected load '" + action.load.value() +
                                   "' has a non-positive contribution");
    }
  }

  // 6. Selected count agreement.
  if (selected_considerations != actions.size() ||
      closure.selected_load_count != static_cast<std::uint64_t>(actions.size())) {
    return invariant_failure(invariants::kSelectedCountMatches,
                             "selected verdicts, actions, and the recorded count disagree");
  }

  // 6b. Every recorded classification count is re-derived from the explanation
  //     trace. The counters are deliberately re-derived here rather than trusted:
  //     a plan whose accounting does not follow from its own verdicts is corrupt.
  struct Tally {
    std::uint64_t eligible = 0;
    std::uint64_t selected = 0;
    std::uint64_t protected_loads = 0;
    std::uint64_t indeterminate = 0;
    std::uint64_t unavailable = 0;
    std::uint64_t stale = 0;
    std::uint64_t denied = 0;
    std::uint64_t unsafe = 0;
    std::uint64_t unsupported = 0;
    std::uint64_t zero = 0;
  } tally;
  for (const LoadConsideration& consideration : considerations) {
    switch (consideration.verdict) {
      case LoadVerdict::Selected:
        ++tally.eligible;
        ++tally.selected;
        break;
      case LoadVerdict::SkippedCoverageMet:
      case LoadVerdict::SkippedStageCap:
      case LoadVerdict::SkippedStageLoadCap:
      case LoadVerdict::SkippedNoOvershoot:
        ++tally.eligible;
        break;
      case LoadVerdict::ProtectedClass:
      case LoadVerdict::NonSheddableClass:
      case LoadVerdict::ProtectedPriority:
      case LoadVerdict::ObligationActive:
        ++tally.protected_loads;
        break;
      case LoadVerdict::OutOfService:
      case LoadVerdict::MinOnNotElapsed:
        ++tally.unavailable;
        break;
      case LoadVerdict::EvidenceStale:
      case LoadVerdict::EvidenceGenerationLag:
        ++tally.unavailable;
        ++tally.stale;
        break;
      case LoadVerdict::EvidenceUnknown:
        ++tally.indeterminate;
        break;
      case LoadVerdict::EvidenceUnavailable:
        ++tally.unavailable;
        ++tally.indeterminate;
        break;
      case LoadVerdict::EvidenceUnsupported:
        ++tally.unavailable;
        ++tally.indeterminate;
        ++tally.unsupported;
        break;
      case LoadVerdict::EvidenceDenied:
        ++tally.indeterminate;
        ++tally.denied;
        break;
      case LoadVerdict::EvidenceUnsafe:
        ++tally.indeterminate;
        ++tally.unsafe;
        break;
      case LoadVerdict::ZeroContribution:
        ++tally.zero;
        break;
      case LoadVerdict::NegativeContribution:
      case LoadVerdict::ClassNotEligible:
      case LoadVerdict::PriorityNotInPolicy:
      case LoadVerdict::StageEmergencyDenied:
        break;
    }
  }
  const struct {
    const char* name;
    std::uint64_t recorded;
    std::uint64_t derived;
  } count_checks[] = {
      {"closure.eligible_load_count", closure.eligible_load_count, tally.eligible},
      {"closure.selected_load_count", closure.selected_load_count, tally.selected},
      {"closure.protected_load_count", closure.protected_load_count, tally.protected_loads},
      {"closure.indeterminate_load_count", closure.indeterminate_load_count, tally.indeterminate},
      {"closure.unavailable_load_count", closure.unavailable_load_count, tally.unavailable},
      {"closure.stale_load_count", closure.stale_load_count, tally.stale},
      {"closure.denied_load_count", closure.denied_load_count, tally.denied},
      {"closure.unsafe_load_count", closure.unsafe_load_count, tally.unsafe},
      {"closure.unsupported_load_count", closure.unsupported_load_count, tally.unsupported},
      {"closure.zero_contribution_load_count", closure.zero_contribution_load_count, tally.zero},
  };
  for (const auto& check : count_checks) {
    if (check.recorded != check.derived) {
      return invariant_failure(invariants::kClosureCountsMatch,
                               std::string(check.name) + " is recorded as " +
                                   std::to_string(check.recorded) +
                                   " but the explanation trace derives " +
                                   std::to_string(check.derived));
    }
  }

  // 7. Coverage status follows the documented precedence exactly.
  const bool residual_positive = !closure.residual_deficit.is_zero();
  CoverageStatus expected_coverage = CoverageStatus::FullyCovered;
  if (residual_positive) {
    if (closure.capacity_indeterminate) {
      expected_coverage = CoverageStatus::Indeterminate;
    } else if (!closure.remaining_eligible_known_capacity.is_zero()) {
      expected_coverage = CoverageStatus::PolicyLimited;
    } else {
      expected_coverage = CoverageStatus::Insufficient;
    }
  }
  if (coverage != expected_coverage) {
    return invariant_failure(invariants::kCoverageStatusMatches,
                             "coverage is recorded as " + std::string(to_string(coverage)) +
                                 " but the closure implies " +
                                 std::string(to_string(expected_coverage)));
  }

  // 8. Emergency authority consistency.
  bool emergency_reason_used = false;
  for (const ShedAction& action : actions) {
    if (action.reason == ReasonCode::EmergencyStageEligible) {
      emergency_reason_used = true;
    }
  }
  if (emergency_reason_used && !emergency_authority_used) {
    return invariant_failure(invariants::kEmergencyJustified,
                             "an emergency stage selected a load without an emergency grant");
  }
  if (emergency_authority_used && emergency_justification.empty()) {
    return invariant_failure(invariants::kEmergencyJustified,
                             "an emergency grant is recorded without a justification");
  }

  return Status::success();
}

std::string_view to_string(LoadVerdict value) noexcept {
  switch (value) {
    case LoadVerdict::Selected: return "selected";
    case LoadVerdict::SkippedCoverageMet: return "skipped-coverage-met";
    case LoadVerdict::SkippedStageCap: return "skipped-stage-cap";
    case LoadVerdict::SkippedStageLoadCap: return "skipped-stage-load-cap";
    case LoadVerdict::SkippedNoOvershoot: return "skipped-no-overshoot";
    case LoadVerdict::ProtectedClass: return "protected-class";
    case LoadVerdict::NonSheddableClass: return "non-sheddable-class";
    case LoadVerdict::ProtectedPriority: return "protected-priority";
    case LoadVerdict::ObligationActive: return "obligation-active";
    case LoadVerdict::ClassNotEligible: return "class-not-eligible";
    case LoadVerdict::PriorityNotInPolicy: return "priority-not-in-policy";
    case LoadVerdict::StageEmergencyDenied: return "stage-emergency-denied";
    case LoadVerdict::OutOfService: return "out-of-service";
    case LoadVerdict::EvidenceUnknown: return "evidence-unknown";
    case LoadVerdict::EvidenceUnavailable: return "evidence-unavailable";
    case LoadVerdict::EvidenceUnsupported: return "evidence-unsupported";
    case LoadVerdict::EvidenceDenied: return "evidence-denied";
    case LoadVerdict::EvidenceUnsafe: return "evidence-unsafe";
    case LoadVerdict::EvidenceStale: return "evidence-stale";
    case LoadVerdict::EvidenceGenerationLag: return "evidence-generation-lag";
    case LoadVerdict::ZeroContribution: return "zero-contribution";
    case LoadVerdict::NegativeContribution: return "negative-contribution";
    case LoadVerdict::MinOnNotElapsed: return "min-on-not-elapsed";
  }
  return "unknown";
}

std::string_view to_string(ReasonCode value) noexcept {
  switch (value) {
    case ReasonCode::StageEligible: return "stage-eligible";
    case ReasonCode::EmergencyStageEligible: return "emergency-stage-eligible";
  }
  return "unknown";
}

std::string_view to_string(StageStopReason value) noexcept {
  switch (value) {
    case StageStopReason::Covered: return "covered";
    case StageStopReason::PowerCapReached: return "power-cap-reached";
    case StageStopReason::LoadCapReached: return "load-cap-reached";
    case StageStopReason::Exhausted: return "exhausted";
    case StageStopReason::EmergencyAuthorityMissing: return "emergency-authority-missing";
    case StageStopReason::Empty: return "empty";
  }
  return "unknown";
}

std::string_view to_string(CoverageStatus value) noexcept {
  switch (value) {
    case CoverageStatus::FullyCovered: return "fully-covered";
    case CoverageStatus::Indeterminate: return "indeterminate";
    case CoverageStatus::PolicyLimited: return "policy-limited";
    case CoverageStatus::Insufficient: return "insufficient";
  }
  return "unknown";
}

Result<LoadVerdict> load_verdict_from_string(std::string_view text) {
  static const struct {
    const char* name;
    LoadVerdict verdict;
  } kTable[] = {
      {"selected", LoadVerdict::Selected},
      {"skipped-coverage-met", LoadVerdict::SkippedCoverageMet},
      {"skipped-stage-cap", LoadVerdict::SkippedStageCap},
      {"skipped-stage-load-cap", LoadVerdict::SkippedStageLoadCap},
      {"skipped-no-overshoot", LoadVerdict::SkippedNoOvershoot},
      {"protected-class", LoadVerdict::ProtectedClass},
      {"non-sheddable-class", LoadVerdict::NonSheddableClass},
      {"protected-priority", LoadVerdict::ProtectedPriority},
      {"obligation-active", LoadVerdict::ObligationActive},
      {"class-not-eligible", LoadVerdict::ClassNotEligible},
      {"priority-not-in-policy", LoadVerdict::PriorityNotInPolicy},
      {"stage-emergency-denied", LoadVerdict::StageEmergencyDenied},
      {"out-of-service", LoadVerdict::OutOfService},
      {"evidence-unknown", LoadVerdict::EvidenceUnknown},
      {"evidence-unavailable", LoadVerdict::EvidenceUnavailable},
      {"evidence-unsupported", LoadVerdict::EvidenceUnsupported},
      {"evidence-denied", LoadVerdict::EvidenceDenied},
      {"evidence-unsafe", LoadVerdict::EvidenceUnsafe},
      {"evidence-stale", LoadVerdict::EvidenceStale},
      {"evidence-generation-lag", LoadVerdict::EvidenceGenerationLag},
      {"zero-contribution", LoadVerdict::ZeroContribution},
      {"negative-contribution", LoadVerdict::NegativeContribution},
      {"min-on-not-elapsed", LoadVerdict::MinOnNotElapsed},
  };
  for (const auto& entry : kTable) {
    if (text == entry.name) {
      return entry.verdict;
    }
  }
  return Status::error(StatusCode::InvalidArgument, "unknown load verdict '" + std::string(text) + "'");
}

Result<CoverageStatus> coverage_status_from_string(std::string_view text) {
  if (text == "fully-covered") return CoverageStatus::FullyCovered;
  if (text == "indeterminate") return CoverageStatus::Indeterminate;
  if (text == "policy-limited") return CoverageStatus::PolicyLimited;
  if (text == "insufficient") return CoverageStatus::Insufficient;
  return Status::error(StatusCode::InvalidArgument, "unknown coverage status '" + std::string(text) + "'");
}

}  // namespace load_shedding
