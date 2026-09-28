#pragma once

// Canonical encoding of every artifact the runtime persists or digests.
//
// Two rules hold for everything in this file:
//   * encoding is total and deterministic: the same logical value always
//     produces the same bytes, independent of container iteration order;
//   * decoding is strict: unknown enum values, lengths above their bound, and
//     trailing bytes are refused before anything is adopted.
//
// Internal header: not installed.

#include <string>
#include <string_view>

#include "detail/codec.hpp"
#include "detail/state.hpp"
#include "load_shedding/effect.hpp"
#include "load_shedding/engine.hpp"
#include "load_shedding/recovery.hpp"

namespace load_shedding::detail {

/// Digest of the decision content of a plan. Excludes the plan's own identity and
/// generation, so two runs over equal inputs produce equal digests.
Digest plan_content_digest(const SheddingPlan& plan);
std::string encode_plan(const SheddingPlan& plan, bool include_instance_identity);

Digest policy_content_digest(const SheddingPolicy& policy);
Digest recovery_content_digest(const RecoveryDecision& decision);
Digest effect_record_digest(const EffectObservation& observation);

/// Digest of the evidence a selected action was based on: the load's identity
/// generation, revision, contribution state, quantity, generation, and tick.
Digest load_evidence_digest(const LoadRecord& load);

Digest plan_request_digest(const PlanRequest& request);
Digest recovery_request_digest(const RecoveryRequest& request);
Digest effect_request_digest(const EffectObservationRequest& request);

/// Encodes the whole authoritative state. The state must satisfy
/// `StoreState::validate()`; the encoder refuses a state that does not.
Result<std::string> encode_state(const StoreState& state);

/// Decodes and structurally validates a whole authoritative state.
Result<StoreState> decode_state(std::string_view payload);

}  // namespace load_shedding::detail
