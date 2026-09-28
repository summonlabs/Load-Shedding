#pragma once

// Downstream effect records.
//
// This header exists to keep two things apart that are easy to confuse:
//
//   * the plan, which is this runtime's authority-bound decision, and
//   * the effect record, which is what a downstream controller reported.
//
// Recording an effect never rewrites a plan. Plans are immutable once committed;
// effects are append-only evidence kept beside them. An acknowledgement is not an
// effect, and an observed effect is not a verified effect: those are separate
// states here.

#include <cstdint>
#include <string>
#include <string_view>

#include "load_shedding/digest.hpp"
#include "load_shedding/ids.hpp"
#include "load_shedding/plan.hpp"
#include "load_shedding/status.hpp"

namespace load_shedding {

/// Reported instance state of a load.
enum class EffectState : std::uint8_t {
  /// Nothing usable was reported. Never treated as "still energized".
  Unknown = 0,
  Energized = 1,
  Shed = 2,
};

/// Where an effect record came from.
enum class EffectSource : std::uint8_t {
  /// A downstream controller adapter.
  Adapter = 0,
  /// A human operator.
  Operator = 1,
  /// A deterministic simulator. Evidence produced this way is SYNTHETIC and is
  /// labelled as such wherever it is reported.
  Synthetic = 2,
};

/// How far an effect has been established.
enum class EffectVerification : std::uint8_t {
  /// The downstream controller acknowledged the request. This is not an effect.
  Acknowledged = 0,
  /// The effect was independently confirmed by observation.
  Confirmed = 1,
  /// Evidence contradicts the requested effect.
  Contradicted = 2,
};

/// Latest observed state of one load, derived from the effect record sequence.
struct ObservedLoadState {
  LoadRef load;
  EffectState state = EffectState::Unknown;
  EffectVerification verification = EffectVerification::Acknowledged;
  /// Logical tick at which the observed state began.
  Tick since;
  EffectGeneration generation;
  Sequence sequence;
  PlanId source_plan;
  PlanGeneration source_plan_generation;
  AttemptId attempt;
};

struct EffectObservationRequest {
  RequestId request_id;
  PlanId plan_id;
  PlanGeneration plan_generation;
  LoadRef load;
  /// Identity of the downstream actuation attempt this observation belongs to.
  AttemptId attempt;
  EffectState observed = EffectState::Unknown;
  EffectSource source = EffectSource::Synthetic;
  EffectVerification verification = EffectVerification::Acknowledged;
  Tick tick;
  StateRevision base_revision;
  AuthorityEpoch authority_epoch;
  Incarnation incarnation;

  Digest digest() const;
};

struct EffectObservation {
  Sequence sequence;
  RequestId request_id;
  PlanId plan_id;
  PlanGeneration plan_generation;
  LoadRef load;
  AttemptId attempt;
  EffectState observed = EffectState::Unknown;
  EffectSource source = EffectSource::Synthetic;
  EffectVerification verification = EffectVerification::Acknowledged;
  Tick tick;
  /// Effect generation produced by this record.
  EffectGeneration generation;
  StateRevision base_revision;
  AuthorityEpoch authority_epoch;
  Incarnation incarnation;
  /// Digest of the source plan at the moment the observation was recorded. A
  /// reader can compare it against the plan's current digest to prove that the
  /// observation did not rewrite the plan.
  Digest plan_digest_at_observation;

  Digest record_digest() const;
};

std::string_view to_string(EffectState value) noexcept;
std::string_view to_string(EffectSource value) noexcept;
std::string_view to_string(EffectVerification value) noexcept;
Result<EffectState> effect_state_from_string(std::string_view text);
Result<EffectSource> effect_source_from_string(std::string_view text);
Result<EffectVerification> effect_verification_from_string(std::string_view text);

}  // namespace load_shedding
