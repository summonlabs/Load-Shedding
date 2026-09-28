#include "load_shedding/recovery.hpp"

#include <string>

#include "detail/serialization.hpp"

namespace load_shedding {
namespace {

Status invariant_failure(std::string_view invariant, const std::string& detail) {
  return Status::error(StatusCode::Corrupt, std::string(invariant) + ": " + detail);
}

}  // namespace

Digest RecoveryDecision::content_digest() const { return detail::recovery_content_digest(*this); }

Status RecoveryDecision::verify() const {
  Power restored_sum = Power::zero();
  std::uint64_t restored_count_actual = 0;
  std::uint64_t ordered_count = 0;
  std::uint64_t awaiting = 0;
  std::uint32_t expected_order = 0;
  bool seen_unordered = false;

  for (const RecoveryCandidate& candidate : candidates) {
    if (candidate.order_index_set) {
      if (seen_unordered) {
        return invariant_failure(recovery_invariants::kOrderDense,
                                 "ordered candidates are not contiguous at the head of the list");
      }
      if (candidate.order_index != expected_order) {
        return invariant_failure(recovery_invariants::kOrderDense,
                                 "recovery order index " + std::to_string(candidate.order_index) +
                                     " does not match position " + std::to_string(expected_order));
      }
      ++expected_order;
      ++ordered_count;
    } else {
      seen_unordered = true;
    }

    if (candidate.verdict == RecoveryVerdict::AwaitingEffectConfirmation) {
      ++awaiting;
    }

    if (candidate.verdict != RecoveryVerdict::Restored) {
      continue;
    }
    ++restored_count_actual;
    if (!candidate.shed_by_plan) {
      return invariant_failure(recovery_invariants::kNoUnconfirmedRestored,
                               "load '" + candidate.load.value() +
                                   "' is restored although the source plan did not select it");
    }
    if (candidate.observed != EffectState::Shed) {
      return invariant_failure(recovery_invariants::kNoUnconfirmedRestored,
                               "load '" + candidate.load.value() +
                                   "' is restored without observed shed evidence");
    }
    if (candidate.shed_tick.is_set() && tick.is_set() && tick < candidate.shed_tick) {
      return invariant_failure(recovery_invariants::kMinOffRespected,
                               "load '" + candidate.load.value() +
                                   "' would be restored before the tick at which it was shed");
    }
    if (candidate.contribution.is_negative()) {
      return invariant_failure(recovery_invariants::kRestoredSumMatches,
                               "load '" + candidate.load.value() + "' carries a negative contribution");
    }
    auto next = checked_add(restored_sum, candidate.contribution);
    if (!next.ok()) {
      return invariant_failure(recovery_invariants::kRestoredSumMatches, next.message());
    }
    restored_sum = next.value();
  }

  if (restored_sum != restored_expected) {
    return invariant_failure(recovery_invariants::kRestoredSumMatches,
                             "recorded restored power " + restored_expected.to_string() +
                                 " does not match the candidate sum " + restored_sum.to_string());
  }
  if (restored_count_actual != restored_count) {
    return invariant_failure(recovery_invariants::kRestoredSumMatches,
                             "recorded restored count does not match the candidate list");
  }
  if (ordered_count != candidate_count) {
    return invariant_failure(recovery_invariants::kOrderDense,
                             "recorded candidate count does not match the ordered candidates");
  }
  if (awaiting != awaiting_confirmation_count) {
    return invariant_failure(recovery_invariants::kNoUnconfirmedRestored,
                             "recorded awaiting-confirmation count does not match the candidate list");
  }
  if (restored_expected > requested_headroom) {
    return invariant_failure(recovery_invariants::kRestoredWithinHeadroom,
                             "restored " + restored_expected.to_string() +
                                 " exceeds the available headroom " + requested_headroom.to_string());
  }
  auto leftover = checked_sub(requested_headroom, restored_expected);
  if (!leftover.ok()) {
    return invariant_failure(recovery_invariants::kHeadroomAccounted, leftover.message());
  }
  if (leftover.value() != remaining_headroom) {
    return invariant_failure(recovery_invariants::kHeadroomAccounted,
                             "remaining headroom " + remaining_headroom.to_string() +
                                 " is not headroom minus restored");
  }
  return Status::success();
}

std::string_view to_string(RecoveryVerdict value) noexcept {
  switch (value) {
    case RecoveryVerdict::Restored: return "restored";
    case RecoveryVerdict::SkippedHeadroom: return "skipped-headroom";
    case RecoveryVerdict::AwaitingEffectConfirmation: return "awaiting-effect-confirmation";
    case RecoveryVerdict::MinOffNotElapsed: return "min-off-not-elapsed";
    case RecoveryVerdict::NotShedByPlan: return "not-shed-by-plan";
    case RecoveryVerdict::EvidenceContradicted: return "evidence-contradicted";
    case RecoveryVerdict::UnknownObservedState: return "unknown-observed-state";
  }
  return "unknown";
}

}  // namespace load_shedding
