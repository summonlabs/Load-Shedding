#pragma once

// The decision core. `compute_plan` and `compute_recovery` are pure functions of
// their inputs: same inputs, same output, including the explanation trace. They
// perform no I/O, read no clock, and consult no global state.
//
// Internal header: not installed.

#include "detail/state.hpp"
#include "load_shedding/engine.hpp"
#include "load_shedding/plan.hpp"
#include "load_shedding/recovery.hpp"

namespace load_shedding::detail {

struct PlanContext {
  const SheddingPolicy* policy = nullptr;
  const FacilitySnapshot* snapshot = nullptr;
  /// Observed load states, used only by the minimum-on-time constraint.
  const std::vector<ObservedLoadState>* observed = nullptr;
  const PlanRequest* request = nullptr;
  PlanId plan_id;
  PlanGeneration plan_generation;
  AttemptId attempt;
  Digest policy_digest;
};

struct RecoveryContext {
  const SheddingPolicy* policy = nullptr;
  const SheddingPlan* plan = nullptr;
  const std::vector<ObservedLoadState>* observed = nullptr;
  const RecoveryRequest* request = nullptr;
};

Result<SheddingPlan> compute_plan(const PlanContext& context);

Result<RecoveryDecision> compute_recovery(const RecoveryContext& context);

/// The deterministic recovery ordering key. Exposed for tests that assert the
/// ordering rule directly rather than only through a decision.
struct RecoveryOrderKey {
  std::uint32_t priority_rank = 0;
  std::uint32_t stage_ordinal = 0;
  std::int64_t contribution_watts = 0;
  std::string load;
};

/// True when `left` must be restored before `right` under `order`.
bool recovery_precedes(const RecoveryOrderKey& left, const RecoveryOrderKey& right, RecoveryOrder order);

}  // namespace load_shedding::detail
