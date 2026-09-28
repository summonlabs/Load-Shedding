#include "detail/state.hpp"

#include <algorithm>
#include <string>

namespace load_shedding::detail {
namespace {

Status invalid(const std::string& detail) {
  return Status::error(StatusCode::Corrupt, detail);
}

template <class T, class Key>
Status require_strictly_sorted(const std::vector<T>& items, const char* what, Key key) {
  for (std::size_t index = 1; index < items.size(); ++index) {
    if (!(key(items[index - 1]) < key(items[index]))) {
      return invalid(std::string(what) + " are not strictly ordered at position " +
                     std::to_string(index));
    }
  }
  return Status::success();
}

}  // namespace

const SheddingPlan* StoreState::find_plan(PlanId plan) const {
  const auto found = std::lower_bound(plans.begin(), plans.end(), plan,
                                      [](const SheddingPlan& candidate, PlanId value) {
                                        return candidate.identity.plan_id < value;
                                      });
  if (found == plans.end() || !(found->identity.plan_id == plan)) {
    return nullptr;
  }
  return &(*found);
}

const IdempotencyRecord* StoreState::find_idempotency(RequestId request) const {
  const auto found = std::find_if(idempotency.begin(), idempotency.end(),
                                  [request](const IdempotencyRecord& record) {
                                    return record.request == request;
                                  });
  return found == idempotency.end() ? nullptr : &(*found);
}

const ObservedLoadState* StoreState::find_observed(const LoadRef& load) const {
  const auto found = std::lower_bound(observed.begin(), observed.end(), load,
                                      [](const ObservedLoadState& candidate, const LoadRef& value) {
                                        return candidate.load < value;
                                      });
  if (found == observed.end() || !(found->load == load)) {
    return nullptr;
  }
  return &(*found);
}

Status StoreState::validate() const {
  if (format_version != store_format::kFormatVersion) {
    return Status::error(StatusCode::Unsupported,
                         "state format version " + std::to_string(format_version) +
                             " is not supported by this build");
  }
  if (policy_installed) {
    const Status policy_valid = policy.validate();
    if (!policy_valid.ok()) {
      return invalid("installed policy is invalid: " + policy_valid.message());
    }
    if (!policy.generation.is_set()) {
      return invalid("installed policy carries no generation");
    }
    if (policy.generation != policy_generation) {
      return invalid("policy generation " + std::to_string(policy.generation.value()) +
                     " does not match the state counter " + std::to_string(policy_generation.value()));
    }
  }

  const Status snapshot_valid = snapshot.validate();
  if (!snapshot_valid.ok()) {
    return invalid("facility snapshot is invalid: " + snapshot_valid.message());
  }
  Status ordering = require_strictly_sorted(snapshot.loads, "snapshot loads",
                                            [](const LoadRecord& load) { return load.ref; });
  if (!ordering.ok()) {
    return ordering;
  }
  ordering = require_strictly_sorted(snapshot.obligations, "snapshot obligations",
                                     [](const ProtectedObligation& obligation) { return obligation.ref; });
  if (!ordering.ok()) {
    return ordering;
  }
  ordering = require_strictly_sorted(observed, "observed load states",
                                     [](const ObservedLoadState& state) { return state.load; });
  if (!ordering.ok()) {
    return ordering;
  }
  for (const ObservedLoadState& state : observed) {
    if (snapshot.find(state.load) == nullptr) {
      return invalid("observed state names load '" + state.load.value() +
                     "' which the snapshot does not contain");
    }
  }
  ordering = require_strictly_sorted(plans, "retained plans",
                                     [](const SheddingPlan& plan) { return plan.identity.plan_id; });
  if (!ordering.ok()) {
    return ordering;
  }
  ordering = require_strictly_sorted(effects, "effect records",
                                     [](const EffectObservation& effect) { return effect.sequence; });
  if (!ordering.ok()) {
    return ordering;
  }
  ordering = require_strictly_sorted(audit, "audit entries",
                                     [](const AuditEntry& entry) { return entry.sequence; });
  if (!ordering.ok()) {
    return ordering;
  }

  for (std::size_t index = 0; index < idempotency.size(); ++index) {
    for (std::size_t other = index + 1; other < idempotency.size(); ++other) {
      if (idempotency[index].request == idempotency[other].request) {
        return invalid("replay window contains request identity " +
                       std::to_string(idempotency[index].request.value()) + " twice");
      }
    }
    if (!idempotency[index].request.is_set()) {
      return invalid("replay window contains an unset request identity");
    }
  }

  // Counter monotonicity: nothing retained may describe a generation ahead of
  // the state counters.
  for (const SheddingPlan& plan : plans) {
    const Status plan_valid = plan.verify();
    if (!plan_valid.ok()) {
      return invalid("retained plan " + std::to_string(plan.identity.plan_id.value()) +
                     " is not self-consistent: " + plan_valid.message());
    }
    if (plan.identity.generation > plan_generation) {
      return invalid("retained plan generation exceeds the state plan generation");
    }
    if (plan.identity.evidence_generation > evidence_generation) {
      return invalid("retained plan references future evidence");
    }
    if (plan.identity.effect_generation > effect_generation) {
      return invalid("retained plan references future effect state");
    }
    if (plan.identity.policy_generation > policy_generation) {
      return invalid("retained plan references a future policy generation");
    }
  }
  for (const RecoveryDecision& decision : recoveries) {
    const Status decision_valid = decision.verify();
    if (!decision_valid.ok()) {
      return invalid("retained recovery decision is not self-consistent: " +
                     decision_valid.message());
    }
    if (decision.effect_generation > effect_generation) {
      return invalid("retained recovery decision references future effect state");
    }
  }
  for (const EffectObservation& effect : effects) {
    if (effect.generation > effect_generation) {
      return invalid("retained effect record references a future effect generation");
    }
    if (effect.base_revision > revision) {
      return invalid("retained effect record was based on a future revision");
    }
  }
  for (const AuditEntry& entry : audit) {
    if (!entry.sequence.is_set()) {
      return invalid("audit entry carries no sequence");
    }
    if (entry.sequence.value() >= next_sequence) {
      return invalid("audit entry sequence is not below the next sequence counter");
    }
  }
  for (const EffectObservation& effect : effects) {
    if (effect.sequence.value() >= next_sequence) {
      return invalid("effect record sequence is not below the next sequence counter");
    }
  }
  if (snapshot.generation > evidence_generation) {
    return invalid("facility snapshot generation exceeds the state evidence generation");
  }
  for (const ObservedLoadState& state : observed) {
    if (state.generation > effect_generation) {
      return invalid("observed load state references a future effect generation");
    }
  }
  return Status::success();
}

void apply_retention(StoreState& state, const RetentionLimits& limits) {
  if (limits.plans != 0 && state.plans.size() > limits.plans) {
    const std::size_t excess = state.plans.size() - limits.plans;
    state.plans.erase(state.plans.begin(), state.plans.begin() + static_cast<std::ptrdiff_t>(excess));
  }
  if (limits.effects != 0 && state.effects.size() > limits.effects) {
    const std::size_t excess = state.effects.size() - limits.effects;
    state.effects.erase(state.effects.begin(), state.effects.begin() + static_cast<std::ptrdiff_t>(excess));
  }
  if (limits.audit != 0 && state.audit.size() > limits.audit) {
    const std::size_t excess = state.audit.size() - limits.audit;
    state.audit.erase(state.audit.begin(), state.audit.begin() + static_cast<std::ptrdiff_t>(excess));
  }
  if (limits.recoveries != 0 && state.recoveries.size() > limits.recoveries) {
    const std::size_t excess = state.recoveries.size() - limits.recoveries;
    state.recoveries.erase(state.recoveries.begin(),
                           state.recoveries.begin() + static_cast<std::ptrdiff_t>(excess));
  }
  if (limits.idempotency != 0 && state.idempotency.size() > limits.idempotency) {
    state.idempotency.resize(limits.idempotency);
  }
}

}  // namespace load_shedding::detail
