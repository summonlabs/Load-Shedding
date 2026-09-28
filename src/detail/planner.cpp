#include "detail/planner.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "detail/serialization.hpp"

namespace load_shedding::detail {
namespace {

/// A load that passed every admissibility gate and is waiting to be considered by
/// its stage.
struct Candidate {
  std::size_t load_index = 0;
  StageIndex stage;
  std::uint32_t order_index = 0;
};

struct Classification {
  LoadVerdict verdict = LoadVerdict::EvidenceUnknown;
  StageIndex stage = StageIndex::unset();
  bool admissible = false;
};

struct ObligationIndex {
  /// Load references named by an active obligation.
  std::vector<LoadRef> loads;
  Power reserved_total;

  bool names(const LoadRef& ref) const {
    return std::binary_search(loads.begin(), loads.end(), ref);
  }
};

Status policy_error(const std::string& detail) {
  return Status::error(StatusCode::PolicyViolation, detail);
}

bool is_protected_class(LoadClass load_class) {
  return load_class == LoadClass::Protected || load_class == LoadClass::NonSheddable;
}

bool class_is_shedable(const SheddingPolicy& policy, LoadClass load_class) {
  return std::find(policy.shed_classes.begin(), policy.shed_classes.end(), load_class) !=
         policy.shed_classes.end();
}

bool priority_is_protected(const SheddingPolicy& policy, PriorityClass priority) {
  return std::find(policy.protected_priorities.begin(), policy.protected_priorities.end(),
                   priority) != policy.protected_priorities.end();
}

const StageDefinition* stage_for_priority(const SheddingPolicy& policy, PriorityClass priority) {
  for (const StageDefinition& stage : policy.stages) {
    if (std::find(stage.priorities.begin(), stage.priorities.end(), priority) !=
        stage.priorities.end()) {
      return &stage;
    }
  }
  return nullptr;
}

bool stage_allows_emergency(const SheddingPolicy& policy, PriorityClass priority) {
  const StageDefinition* stage = stage_for_priority(policy, priority);
  return stage != nullptr && stage->requires_emergency_authority;
}

LoadVerdict evidence_verdict(EvidenceState state) {
  switch (state) {
    case EvidenceState::Known: return LoadVerdict::Selected;
    case EvidenceState::Unknown: return LoadVerdict::EvidenceUnknown;
    case EvidenceState::Unavailable: return LoadVerdict::EvidenceUnavailable;
    case EvidenceState::Unsupported: return LoadVerdict::EvidenceUnsupported;
    case EvidenceState::Denied: return LoadVerdict::EvidenceDenied;
    case EvidenceState::Unsafe: return LoadVerdict::EvidenceUnsafe;
  }
  return LoadVerdict::EvidenceUnknown;
}

/// True when the verdict means "the evidence does not establish this load's
/// current sheddable contribution" for a load that policy would otherwise admit.
/// Those verdicts make the residual unprovable rather than merely non-zero.
bool verdict_is_evidence_failure(LoadVerdict verdict) {
  switch (verdict) {
    case LoadVerdict::EvidenceUnknown:
    case LoadVerdict::EvidenceUnavailable:
    case LoadVerdict::EvidenceUnsupported:
    case LoadVerdict::EvidenceDenied:
    case LoadVerdict::EvidenceUnsafe:
    case LoadVerdict::EvidenceStale:
    case LoadVerdict::EvidenceGenerationLag:
      return true;
    default:
      return false;
  }
}

Status add_amount(Power& total, Power addition, const char* what) {
  auto sum = checked_add(total, addition);
  if (!sum.ok()) {
    return Status::error(StatusCode::Overflow,
                         std::string("accounting overflow while accumulating ") + what);
  }
  total = sum.value();
  return Status::success();
}

Status increment(std::uint64_t& counter) {
  if (counter == UINT64_MAX) {
    return Status::error(StatusCode::Overflow, "accounting counter exhausted");
  }
  ++counter;
  return Status::success();
}

/// Known, positive sheddable contribution of a load, or zero when the evidence
/// does not establish a usable quantity.
Power known_contribution(const LoadRecord& load) {
  if (load.contribution.state != EvidenceState::Known) {
    return Power::zero();
  }
  if (load.contribution.value.is_negative()) {
    return Power::zero();
  }
  return load.contribution.value;
}

Status collect_obligations(const FacilitySnapshot& snapshot, ObligationIndex& index) {
  index.reserved_total = Power::zero();
  for (const ProtectedObligation& obligation : snapshot.obligations) {
    if (!obligation.active) {
      continue;
    }
    auto sum = checked_add(index.reserved_total, obligation.reserved);
    if (!sum.ok()) {
      return Status::error(StatusCode::Overflow, "protected reserve total overflow");
    }
    index.reserved_total = sum.value();
    if (!obligation.load.empty()) {
      index.loads.push_back(obligation.load);
    }
  }
  std::sort(index.loads.begin(), index.loads.end());
  index.loads.erase(std::unique(index.loads.begin(), index.loads.end()), index.loads.end());
  return Status::success();
}

/// Known sheddable demand of the facility, used by parts-per-million stage caps.
/// Deliberately independent of stage assignment so that a stage cap cannot depend
/// on the very selection it bounds.
Result<Power> sheddable_demand(const SheddingPolicy& policy, const FacilitySnapshot& snapshot,
                               const ObligationIndex& obligations) {
  Power total = Power::zero();
  for (const LoadRecord& load : snapshot.loads) {
    if (!load.in_service || is_protected_class(load.load_class) ||
        !class_is_shedable(policy, load.load_class) || priority_is_protected(policy, load.priority) ||
        obligations.names(load.ref)) {
      continue;
    }
    const Status added = add_amount(total, known_contribution(load), "facility sheddable demand");
    if (!added.ok()) {
      return added;
    }
  }
  return total;
}

Result<std::optional<Power>> stage_effective_cap(const StageDefinition& stage,
                                                 Power facility_sheddable_demand) {
  std::optional<Power> cap;
  if (stage.max_shed.has_value()) {
    cap = stage.max_shed.value();
  }
  if (stage.max_shed_ppm.has_value()) {
    auto scaled = scale_ppm(facility_sheddable_demand, stage.max_shed_ppm.value());
    if (!scaled.ok()) {
      return scaled.status();
    }
    if (!cap.has_value() || scaled.value() < cap.value()) {
      cap = scaled.value();
    }
  }
  return cap;
}

/// Evidence recorded at a tick or generation ahead of the snapshot it is read
/// against is treated as stale: a snapshot cannot be justified by evidence from
/// its own future.
bool is_stale(const SheddingPolicy& policy, const FacilitySnapshot& snapshot, const LoadRecord& load) {
  if (load.contribution.tick > snapshot.tick) {
    return true;
  }
  const std::uint64_t age = snapshot.tick.value() - load.contribution.tick.value();
  return age > policy.max_evidence_age_ticks;
}

bool is_generation_lagging(const SheddingPolicy& policy, const FacilitySnapshot& snapshot,
                           const LoadRecord& load) {
  if (load.contribution.generation > snapshot.generation) {
    return true;
  }
  const std::uint64_t lag = snapshot.generation.value() - load.contribution.generation.value();
  return lag > policy.max_evidence_generation_lag;
}

bool min_on_satisfied(const SheddingPolicy& policy, const std::vector<ObservedLoadState>& observed,
                      const LoadRef& ref, Tick now) {
  if (!policy.min_on_ticks.has_value()) {
    return true;
  }
  const auto found = std::lower_bound(observed.begin(), observed.end(), ref,
                                      [](const ObservedLoadState& state, const LoadRef& value) {
                                        return state.load < value;
                                      });
  if (found == observed.end() || !(found->load == ref)) {
    return false;
  }
  if (found->state != EffectState::Energized || !found->since.is_set() || !now.is_set()) {
    return false;
  }
  if (now < found->since) {
    return false;
  }
  return (now.value() - found->since.value()) >= policy.min_on_ticks.value();
}

struct PlanBuilder {
  const SheddingPolicy& policy;
  const FacilitySnapshot& snapshot;
  const std::vector<ObservedLoadState>& observed;
  const PlanRequest& request;
  const ObligationIndex& obligations;
  bool emergency_used = false;
  Power facility_sheddable_demand;

  std::vector<LoadConsideration> considerations;
  std::vector<Classification> classifications;
  std::vector<Candidate> candidates;
};

/// Number of loads whose priority the stage claims, whether or not they survived
/// the admissibility gates.
std::uint32_t stage_considered(const SheddingPolicy& policy, const StageDefinition& stage,
                               const FacilitySnapshot& snapshot) {
  (void)policy;
  std::uint32_t count = 0;
  for (const LoadRecord& load : snapshot.loads) {
    if (std::find(stage.priorities.begin(), stage.priorities.end(), load.priority) !=
        stage.priorities.end()) {
      if (count == UINT32_MAX) {
        return count;
      }
      ++count;
    }
  }
  return count;
}

}  // namespace

bool recovery_precedes(const RecoveryOrderKey& left, const RecoveryOrderKey& right,
                       RecoveryOrder order) {
  if (order == RecoveryOrder::PriorityThenReverseStage) {
    if (left.priority_rank != right.priority_rank) {
      return left.priority_rank < right.priority_rank;
    }
    if (left.stage_ordinal != right.stage_ordinal) {
      return left.stage_ordinal > right.stage_ordinal;
    }
  } else {
    if (left.stage_ordinal != right.stage_ordinal) {
      return left.stage_ordinal > right.stage_ordinal;
    }
    if (left.priority_rank != right.priority_rank) {
      return left.priority_rank < right.priority_rank;
    }
  }
  if (left.contribution_watts != right.contribution_watts) {
    return left.contribution_watts > right.contribution_watts;
  }
  return left.load < right.load;
}

Result<SheddingPlan> compute_plan(const PlanContext& context) {
  const SheddingPolicy& policy = *context.policy;
  const FacilitySnapshot& snapshot = *context.snapshot;
  const PlanRequest& request = *context.request;
  const std::vector<ObservedLoadState>& observed = *context.observed;

  const Status policy_valid = policy.validate();
  if (!policy_valid.ok()) {
    return policy_valid;
  }
  const Status snapshot_valid = snapshot.validate();
  if (!snapshot_valid.ok()) {
    return snapshot_valid;
  }
  if (request.requested_reduction.is_negative()) {
    return Status::error(StatusCode::InvalidArgument, "requested reduction must not be negative");
  }
  if (request.requested_reduction.is_zero()) {
    return Status::error(StatusCode::InvalidArgument, "requested reduction must be greater than zero");
  }
  if (request.requested_reduction.watts() > limits::kMaxPowerWatts) {
    return Status::error(StatusCode::OutOfRange, "requested reduction is outside the supported range");
  }

  const bool emergency_requested = request.emergency_authority;
  if (emergency_requested && !policy.allow_emergency_override) {
    return policy_error("policy '" + policy.name + "' does not permit emergency override");
  }
  if (emergency_requested && request.emergency_justification.empty()) {
    return Status::error(StatusCode::InvalidArgument,
                         "an emergency grant requires a justification");
  }

  ObligationIndex obligations;
  const Status obligations_status = collect_obligations(snapshot, obligations);
  if (!obligations_status.ok()) {
    return obligations_status;
  }
  auto demand = sheddable_demand(policy, snapshot, obligations);
  if (!demand.ok()) {
    return demand.status();
  }

  PlanBuilder builder{policy,          snapshot, observed, request, obligations,
                      emergency_requested, demand.value(), {}, {}, {}};

  // ---- classification pass -------------------------------------------------
  builder.considerations.reserve(snapshot.loads.size());
  builder.classifications.reserve(snapshot.loads.size());
  for (std::size_t index = 0; index < snapshot.loads.size(); ++index) {
    const LoadRecord& load = snapshot.loads[index];
    LoadConsideration consideration;
    consideration.load = load.ref;
    consideration.priority = load.priority;
    consideration.load_class = load.load_class;
    consideration.evidence_state = load.contribution.state;
    consideration.contribution = known_contribution(load);
    consideration.identity_generation = load.identity_generation;
    consideration.load_revision = load.revision;
    consideration.evidence_generation = load.contribution.generation;
    consideration.evidence_tick = load.contribution.tick;

    Classification classification;
    const bool protected_class = is_protected_class(load.load_class);
    const bool protected_priority = priority_is_protected(policy, load.priority);
    const bool emergency_admissible =
        builder.emergency_used && stage_allows_emergency(policy, load.priority);

    // Documented precedence. Protection is reported before service state and
    // before evidence state: a protected load is reported as protected whether or
    // not it is currently drawing power, so the protected bucket is a true
    // "policy will never shed this" total rather than an undercount.
    if (obligations.names(load.ref)) {
      classification.verdict = LoadVerdict::ObligationActive;
    } else if (protected_class && !emergency_admissible) {
      classification.verdict = load.load_class == LoadClass::Protected ? LoadVerdict::ProtectedClass
                                                                       : LoadVerdict::NonSheddableClass;
    } else if (protected_priority && !emergency_admissible) {
      classification.verdict = LoadVerdict::ProtectedPriority;
    } else if (!load.in_service) {
      classification.verdict = LoadVerdict::OutOfService;
    } else if (load.contribution.state != EvidenceState::Known) {
      classification.verdict = evidence_verdict(load.contribution.state);
    } else if (is_generation_lagging(policy, snapshot, load)) {
      classification.verdict = LoadVerdict::EvidenceGenerationLag;
    } else if (is_stale(policy, snapshot, load)) {
      classification.verdict = LoadVerdict::EvidenceStale;
    } else if (load.contribution.value.is_negative()) {
      classification.verdict = LoadVerdict::NegativeContribution;
    } else if (load.contribution.value.is_zero()) {
      classification.verdict = LoadVerdict::ZeroContribution;
    } else if (!min_on_satisfied(policy, builder.observed, load.ref, request.tick)) {
      classification.verdict = LoadVerdict::MinOnNotElapsed;
    } else if (!class_is_shedable(policy, load.load_class) &&
               !(protected_class && emergency_admissible)) {
      // A protected class is admitted only by an emergency stage that explicitly
      // covers this load's priority under a granted emergency authority.
      classification.verdict = LoadVerdict::ClassNotEligible;
    } else {
      const StageDefinition* stage = stage_for_priority(policy, load.priority);
      if (stage == nullptr) {
        classification.verdict = LoadVerdict::PriorityNotInPolicy;
      } else if (stage->requires_emergency_authority && !builder.emergency_used) {
        classification.verdict = LoadVerdict::StageEmergencyDenied;
      } else {
        classification.admissible = true;
        classification.stage = stage->index;
        classification.verdict = LoadVerdict::SkippedCoverageMet;
      }
    }
    consideration.stage = classification.stage;
    consideration.verdict = classification.verdict;
    builder.considerations.push_back(consideration);
    builder.classifications.push_back(classification);
  }

  // ---- candidate ordering --------------------------------------------------
  for (std::uint32_t stage_ordinal = 0; stage_ordinal < policy.stages.size(); ++stage_ordinal) {
    std::vector<std::size_t> stage_loads;
    for (std::size_t index = 0; index < builder.classifications.size(); ++index) {
      if (builder.classifications[index].admissible &&
          builder.classifications[index].stage.value() == stage_ordinal) {
        stage_loads.push_back(index);
      }
    }
    std::sort(stage_loads.begin(), stage_loads.end(), [&builder](std::size_t left, std::size_t right) {
      const LoadRecord& a = builder.snapshot.loads[left];
      const LoadRecord& b = builder.snapshot.loads[right];
      const std::uint32_t rank_a = priority_rank(a.priority);
      const std::uint32_t rank_b = priority_rank(b.priority);
      if (rank_a != rank_b) {
        return rank_a > rank_b;  // least important first
      }
      if (a.contribution.value.watts() != b.contribution.value.watts()) {
        return a.contribution.value.watts() > b.contribution.value.watts();
      }
      return a.ref < b.ref;
    });
    for (const std::size_t index : stage_loads) {
      Candidate candidate;
      candidate.load_index = index;
      candidate.stage = builder.classifications[index].stage;
      candidate.order_index = static_cast<std::uint32_t>(builder.candidates.size());
      builder.candidates.push_back(candidate);
      builder.considerations[index].order_index = candidate.order_index;
      builder.considerations[index].order_index_set = true;
    }
  }

  // ---- stage-by-stage selection -------------------------------------------
  SheddingPlan plan;
  plan.identity.plan_id = context.plan_id;
  plan.identity.generation = context.plan_generation;
  plan.identity.request_id = request.request_id;
  plan.identity.policy_generation = policy.generation;
  plan.identity.policy_digest = context.policy_digest;
  plan.identity.base_revision = request.base_revision;
  plan.identity.evidence_generation = snapshot.generation;
  plan.identity.effect_generation = request.effect_generation;
  plan.identity.authority_epoch = request.authority_epoch;
  plan.identity.incarnation = request.incarnation;
  plan.identity.tick = request.tick;
  plan.identity.attempt = context.attempt;
  plan.mode = policy.mode;
  plan.emergency_authority_used = builder.emergency_used;
  plan.emergency_justification = builder.emergency_used ? request.emergency_justification : std::string();

  Power selected_total = Power::zero();
  bool covered = false;

  for (const StageDefinition& stage : policy.stages) {
    StageSummary summary;
    summary.index = stage.index;
    summary.name = stage.name;
    summary.load_cap = stage.max_loads;
    summary.emergency_authority_required = stage.requires_emergency_authority;
    summary.considered = stage_considered(policy, stage, snapshot);

    auto cap = stage_effective_cap(stage, builder.facility_sheddable_demand);
    if (!cap.ok()) {
      return cap.status();
    }
    summary.effective_power_cap = cap.value();
    Power stage_budget = cap.value().value_or(Power::from_watts(limits::kMaxPowerWatts));
    std::uint32_t stage_selected = 0;
    bool stage_cap_reached = false;
    bool stage_load_cap_reached = false;
    bool stage_had_admissible = false;

    for (const Candidate& candidate : builder.candidates) {
      if (candidate.stage != stage.index) {
        continue;
      }
      stage_had_admissible = true;
      ++summary.admissible;
      LoadConsideration& consideration = builder.considerations[candidate.load_index];
      const LoadRecord& load = snapshot.loads[candidate.load_index];

      if (covered) {
        consideration.verdict = LoadVerdict::SkippedCoverageMet;
        continue;
      }
      if (stage.max_loads.has_value() && stage_selected >= stage.max_loads.value()) {
        consideration.verdict = LoadVerdict::SkippedStageLoadCap;
        stage_load_cap_reached = true;
        continue;
      }
      if (load.contribution.value > stage_budget) {
        consideration.verdict = LoadVerdict::SkippedStageCap;
        stage_cap_reached = true;
        continue;
      }
      if (policy.mode == SelectionMode::NoOvershootGreedy) {
        auto projected = checked_add(selected_total, load.contribution.value);
        if (!projected.ok()) {
          return projected.status();
        }
        if (projected.value() > request.requested_reduction) {
          consideration.verdict = LoadVerdict::SkippedNoOvershoot;
          continue;
        }
      }

      ShedAction action;
      action.load = load.ref;
      action.stage = stage.index;
      action.selection_index = static_cast<std::uint32_t>(plan.actions.size());
      action.expected_contribution = load.contribution.value;
      action.reason = stage.requires_emergency_authority ? ReasonCode::EmergencyStageEligible
                                                         : ReasonCode::StageEligible;
      action.priority = load.priority;
      action.load_class = load.load_class;
      action.identity_generation = load.identity_generation;
      action.load_revision = load.revision;
      action.evidence_digest = load_evidence_digest(load);
      plan.actions.push_back(action);

      auto next_total = checked_add(selected_total, load.contribution.value);
      if (!next_total.ok()) {
        return next_total.status();
      }
      selected_total = next_total.value();
      auto remaining_budget = checked_sub(stage_budget, load.contribution.value);
      if (!remaining_budget.ok()) {
        return remaining_budget.status();
      }
      stage_budget = remaining_budget.value();
      ++stage_selected;
      consideration.verdict = LoadVerdict::Selected;
      if (selected_total >= request.requested_reduction) {
        covered = true;
      }
    }

    summary.selected = stage_selected;
    summary.selected_power = Power::zero();
    for (const ShedAction& action : plan.actions) {
      if (action.stage == stage.index) {
        const Status added = add_amount(summary.selected_power, action.expected_contribution,
                                        "stage selected power");
        if (!added.ok()) {
          return added;
        }
      }
    }
    if (covered) {
      summary.stop_reason = StageStopReason::Covered;
    } else if (stage_load_cap_reached && stage_selected >= stage.max_loads.value_or(0)) {
      summary.stop_reason = StageStopReason::LoadCapReached;
    } else if (stage_cap_reached) {
      summary.stop_reason = StageStopReason::PowerCapReached;
    } else if (!stage_had_admissible) {
      summary.stop_reason = stage.requires_emergency_authority && !builder.emergency_used
                                ? StageStopReason::EmergencyAuthorityMissing
                                : StageStopReason::Empty;
    } else {
      summary.stop_reason = StageStopReason::Exhausted;
    }
    plan.stages.push_back(summary);
  }

  // ---- accounting closure --------------------------------------------------
  AccountingClosure closure;
  closure.requested_reduction = request.requested_reduction;
  closure.selected_expected_reduction = selected_total;
  closure.selected_load_count = static_cast<std::uint64_t>(plan.actions.size());

  for (std::size_t index = 0; index < snapshot.loads.size(); ++index) {
    const LoadConsideration& consideration = builder.considerations[index];
    const Power known = consideration.contribution;
    switch (consideration.verdict) {
      case LoadVerdict::Selected: {
        const Status added = add_amount(closure.eligible_known_capacity, known, "eligible capacity");
        if (!added.ok()) return added;
        const Status counted = increment(closure.eligible_load_count);
        if (!counted.ok()) return counted;
        break;
      }
      case LoadVerdict::SkippedCoverageMet:
      case LoadVerdict::SkippedStageCap:
      case LoadVerdict::SkippedStageLoadCap:
      case LoadVerdict::SkippedNoOvershoot: {
        const Status added = add_amount(closure.eligible_known_capacity, known, "eligible capacity");
        if (!added.ok()) return added;
        const Status counted = increment(closure.eligible_load_count);
        if (!counted.ok()) return counted;
        const Status remaining = add_amount(closure.remaining_eligible_known_capacity, known,
                                            "remaining eligible capacity");
        if (!remaining.ok()) return remaining;
        break;
      }
      case LoadVerdict::ProtectedClass:
      case LoadVerdict::NonSheddableClass:
      case LoadVerdict::ProtectedPriority:
      case LoadVerdict::ObligationActive: {
        const Status added = add_amount(closure.protected_known_amount, known, "protected amount");
        if (!added.ok()) return added;
        const Status counted = increment(closure.protected_load_count);
        if (!counted.ok()) return counted;
        break;
      }
      case LoadVerdict::ClassNotEligible:
      case LoadVerdict::PriorityNotInPolicy:
      case LoadVerdict::StageEmergencyDenied: {
        const Status added = add_amount(closure.non_sheddable_known_amount, known,
                                        "non-sheddable amount");
        if (!added.ok()) return added;
        break;
      }
      case LoadVerdict::OutOfService:
      case LoadVerdict::EvidenceStale:
      case LoadVerdict::EvidenceGenerationLag:
      case LoadVerdict::MinOnNotElapsed: {
        const Status added = add_amount(closure.unavailable_known_amount, known,
                                        "unavailable amount");
        if (!added.ok()) return added;
        const Status counted = increment(closure.unavailable_load_count);
        if (!counted.ok()) return counted;
        if (consideration.verdict == LoadVerdict::EvidenceStale ||
            consideration.verdict == LoadVerdict::EvidenceGenerationLag) {
          const Status stale = increment(closure.stale_load_count);
          if (!stale.ok()) return stale;
        }
        break;
      }
      case LoadVerdict::EvidenceUnknown: {
        const Status counted = increment(closure.indeterminate_load_count);
        if (!counted.ok()) return counted;
        break;
      }
      case LoadVerdict::EvidenceUnavailable: {
        const Status counted = increment(closure.unavailable_load_count);
        if (!counted.ok()) return counted;
        const Status indeterminate = increment(closure.indeterminate_load_count);
        if (!indeterminate.ok()) return indeterminate;
        break;
      }
      case LoadVerdict::EvidenceUnsupported: {
        const Status counted = increment(closure.unavailable_load_count);
        if (!counted.ok()) return counted;
        const Status unsupported = increment(closure.unsupported_load_count);
        if (!unsupported.ok()) return unsupported;
        const Status indeterminate = increment(closure.indeterminate_load_count);
        if (!indeterminate.ok()) return indeterminate;
        break;
      }
      case LoadVerdict::EvidenceDenied: {
        const Status counted = increment(closure.denied_load_count);
        if (!counted.ok()) return counted;
        const Status indeterminate = increment(closure.indeterminate_load_count);
        if (!indeterminate.ok()) return indeterminate;
        break;
      }
      case LoadVerdict::EvidenceUnsafe: {
        const Status counted = increment(closure.unsafe_load_count);
        if (!counted.ok()) return counted;
        const Status indeterminate = increment(closure.indeterminate_load_count);
        if (!indeterminate.ok()) return indeterminate;
        break;
      }
      case LoadVerdict::ZeroContribution: {
        const Status counted = increment(closure.zero_contribution_load_count);
        if (!counted.ok()) return counted;
        break;
      }
      case LoadVerdict::NegativeContribution:
        break;
    }
    if (verdict_is_evidence_failure(consideration.verdict)) {
      // Evidence did not establish this load's current contribution while policy
      // would otherwise have admitted it: the residual cannot be proven minimal.
      closure.capacity_indeterminate = true;
    }
  }
  if (closure.selected_expected_reduction > closure.eligible_known_capacity) {
    return Status::error(StatusCode::Corrupt,
                         "internal invariant: selected exceeds eligible known capacity");
  }

  auto residual = positive_difference(closure.requested_reduction,
                                      closure.selected_expected_reduction);
  if (!residual.ok()) return residual.status();
  closure.residual_deficit = residual.value();
  auto overshoot = positive_difference(closure.selected_expected_reduction,
                                       closure.requested_reduction);
  if (!overshoot.ok()) return overshoot.status();
  closure.overshoot = overshoot.value();

  if (snapshot.total_demand.state == EvidenceState::Known) {
    auto ceiling = checked_sub(snapshot.total_demand.value, obligations.reserved_total);
    if (!ceiling.ok()) {
      return ceiling.status();
    }
    closure.ceiling_verified = true;
    closure.effective_shedding_ceiling = ceiling.value();
  } else {
    closure.ceiling_verified = false;
  }

  plan.coverage = CoverageStatus::FullyCovered;
  if (!closure.residual_deficit.is_zero()) {
    if (closure.capacity_indeterminate) {
      plan.coverage = CoverageStatus::Indeterminate;
    } else if (!closure.remaining_eligible_known_capacity.is_zero()) {
      plan.coverage = CoverageStatus::PolicyLimited;
    } else {
      plan.coverage = CoverageStatus::Insufficient;
    }
  }
  plan.closure = closure;
  plan.considerations = std::move(builder.considerations);

  // Evidence generation set: every generation this decision consulted.
  std::vector<EvidenceGeneration> generations;
  generations.push_back(snapshot.generation);
  if (snapshot.total_demand.generation.is_set()) {
    generations.push_back(snapshot.total_demand.generation);
  }
  for (const ProtectedObligation& obligation : snapshot.obligations) {
    if (obligation.active && obligation.generation.is_set()) {
      generations.push_back(obligation.generation);
    }
  }
  for (const LoadRecord& load : snapshot.loads) {
    if (load.contribution.generation.is_set()) {
      generations.push_back(load.contribution.generation);
    }
  }
  std::sort(generations.begin(), generations.end());
  generations.erase(std::unique(generations.begin(), generations.end()), generations.end());
  plan.identity.evidence_generations = std::move(generations);

  const Status verified = plan.verify();
  if (!verified.ok()) {
    return Status::error(StatusCode::Corrupt,
                         "the planner produced a plan that fails its own invariants: " +
                             verified.message());
  }
  return plan;
}

Result<RecoveryDecision> compute_recovery(const RecoveryContext& context) {
  const SheddingPolicy& policy = *context.policy;
  const SheddingPlan& plan = *context.plan;
  const RecoveryRequest& request = *context.request;
  const std::vector<ObservedLoadState>& observed = *context.observed;

  if (request.available_headroom.is_negative()) {
    return Status::error(StatusCode::InvalidArgument, "available headroom must not be negative");
  }
  const Status plan_valid = plan.verify();
  if (!plan_valid.ok()) {
    return plan_valid;
  }

  RecoveryDecision decision;
  decision.request_id = request.request_id;
  decision.source_plan = plan.identity.plan_id;
  decision.source_plan_generation = plan.identity.generation;
  decision.source_plan_digest = plan.content_digest();
  decision.policy_generation = policy.generation;
  decision.evidence_generation = request.evidence_generation;
  decision.effect_generation = request.effect_generation;
  decision.base_revision = request.base_revision;
  decision.authority_epoch = request.authority_epoch;
  decision.incarnation = request.incarnation;
  decision.tick = request.tick;
  decision.order = policy.recovery_order;
  decision.requested_headroom = request.available_headroom;

  struct OrderedCandidate {
    RecoveryOrderKey key;
    RecoveryCandidate candidate;
  };
  std::vector<OrderedCandidate> ordered;
  ordered.reserve(plan.actions.size());
  for (const ShedAction& action : plan.actions) {
    OrderedCandidate entry;
    entry.key.priority_rank = priority_rank(action.priority);
    entry.key.stage_ordinal = action.stage.is_set() ? action.stage.value() : 0;
    entry.key.contribution_watts = action.expected_contribution.watts();
    entry.key.load = action.load.value();
    entry.candidate.load = action.load;
    entry.candidate.stage = action.stage;
    entry.candidate.priority = action.priority;
    entry.candidate.load_class = action.load_class;
    entry.candidate.contribution = action.expected_contribution;
    entry.candidate.shed_by_plan = true;
    ordered.push_back(std::move(entry));
  }
  std::sort(ordered.begin(), ordered.end(),
            [&policy](const OrderedCandidate& left, const OrderedCandidate& right) {
              return recovery_precedes(left.key, right.key, policy.recovery_order);
            });

  Power restored = Power::zero();
  for (std::size_t index = 0; index < ordered.size(); ++index) {
    RecoveryCandidate& candidate = ordered[index].candidate;
    candidate.order_index = static_cast<std::uint32_t>(index);
    candidate.order_index_set = true;

    const auto found = std::lower_bound(observed.begin(), observed.end(), candidate.load,
                                        [](const ObservedLoadState& state, const LoadRef& value) {
                                          return state.load < value;
                                        });
    const ObservedLoadState* state = nullptr;
    if (found != observed.end() && found->load == candidate.load) {
      state = &(*found);
    }
    if (state != nullptr) {
      candidate.observed = state->state;
      candidate.verification = state->verification;
      candidate.observed_generation = state->generation;
      candidate.shed_tick = state->since;
    }

    if (state == nullptr || state->state == EffectState::Unknown) {
      candidate.verdict = RecoveryVerdict::UnknownObservedState;
      decision.candidates.push_back(candidate);
      continue;
    }
    if (state->state == EffectState::Energized) {
      candidate.verdict = RecoveryVerdict::EvidenceContradicted;
      decision.candidates.push_back(candidate);
      continue;
    }
    if (state->verification != EffectVerification::Confirmed) {
      // An acknowledgement is not an effect. Restoration waits for confirmation.
      candidate.verdict = RecoveryVerdict::AwaitingEffectConfirmation;
      ++decision.awaiting_confirmation_count;
      decision.candidates.push_back(candidate);
      continue;
    }
    if (policy.min_off_ticks.has_value()) {
      const bool elapsed = state->since.is_set() && request.tick.is_set() &&
                           request.tick >= state->since &&
                           (request.tick.value() - state->since.value()) >= policy.min_off_ticks.value();
      if (!elapsed) {
        candidate.verdict = RecoveryVerdict::MinOffNotElapsed;
        decision.candidates.push_back(candidate);
        continue;
      }
    }
    auto projected = checked_add(restored, candidate.contribution);
    if (!projected.ok()) {
      return projected.status();
    }
    if (projected.value() > request.available_headroom) {
      candidate.verdict = RecoveryVerdict::SkippedHeadroom;
      decision.candidates.push_back(candidate);
      continue;
    }
    candidate.verdict = RecoveryVerdict::Restored;
    restored = projected.value();
    ++decision.restored_count;
    decision.candidates.push_back(candidate);
  }
  decision.candidate_count = static_cast<std::uint64_t>(ordered.size());

  // Every other load is reported as not shed by this plan, so the recovery
  // explanation is total over the facility.
  std::vector<RecoveryCandidate> others;
  for (std::size_t index = 0; index < plan.considerations.size(); ++index) {
    const LoadConsideration& consideration = plan.considerations[index];
    const bool already = std::any_of(decision.candidates.begin(), decision.candidates.end(),
                                     [&consideration](const RecoveryCandidate& candidate) {
                                       return candidate.load == consideration.load;
                                     });
    if (already) {
      continue;
    }
    RecoveryCandidate candidate;
    candidate.load = consideration.load;
    candidate.priority = consideration.priority;
    candidate.load_class = consideration.load_class;
    candidate.contribution = consideration.contribution;
    candidate.verdict = RecoveryVerdict::NotShedByPlan;
    candidate.shed_by_plan = false;
    others.push_back(std::move(candidate));
  }
  std::sort(others.begin(), others.end(), [](const RecoveryCandidate& left, const RecoveryCandidate& right) {
    return left.load < right.load;
  });
  for (RecoveryCandidate& candidate : others) {
    decision.candidates.push_back(std::move(candidate));
  }

  decision.restored_expected = restored;
  auto remaining = checked_sub(request.available_headroom, restored);
  if (!remaining.ok()) {
    return remaining.status();
  }
  decision.remaining_headroom = remaining.value();

  const Status verified = decision.verify();
  if (!verified.ok()) {
    return Status::error(StatusCode::Corrupt,
                         "the recovery planner produced a decision that fails its own invariants: " +
                             verified.message());
  }
  return decision;
}

}  // namespace load_shedding::detail
