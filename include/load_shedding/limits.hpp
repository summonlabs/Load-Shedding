#pragma once

// Every externally influenced size is bounded here before allocation. A bound in
// this header is a contract: the library refuses input that exceeds it, and the
// store format refuses to decode content that could exceed it.

#include <cstddef>
#include <cstdint>

namespace load_shedding::limits {

// ---- Identity and text -----------------------------------------------------
inline constexpr std::size_t kMaxLoadRefBytes = 128;
inline constexpr std::size_t kMaxObligationRefBytes = 128;
inline constexpr std::size_t kMaxPolicyNameBytes = 64;
inline constexpr std::size_t kMaxStageNameBytes = 64;
inline constexpr std::size_t kMaxNoteBytes = 256;
inline constexpr std::size_t kMaxJustificationBytes = 512;

// ---- Domain collection sizes ----------------------------------------------
inline constexpr std::size_t kMaxLoads = 4096;
inline constexpr std::size_t kMaxObligations = 1024;
inline constexpr std::size_t kMaxStages = 32;
inline constexpr std::size_t kMaxPrioritiesPerStage = 16;
inline constexpr std::size_t kMaxSelectedActions = kMaxLoads;
inline constexpr std::size_t kMaxConsiderations = kMaxLoads;
inline constexpr std::size_t kMaxRecoveryCandidates = kMaxLoads;
inline constexpr std::size_t kMaxEvidenceGenerations = kMaxLoads + 8;

// ---- Retained history ------------------------------------------------------
/// Retained plans. Must be at least `kMaxIdempotencyEntries` so that an
/// accepted attempt inside the replay window is always still replayable.
inline constexpr std::size_t kMaxPlansRetained = 64;
inline constexpr std::size_t kMaxEffectRecords = 4096;
inline constexpr std::size_t kMaxAuditEntries = 8192;
inline constexpr std::size_t kMaxRecoveryDecisionsRetained = 128;
inline constexpr std::size_t kMaxHistoryPage = 256;
inline constexpr std::size_t kMaxPlanDiffEntries = 2 * kMaxLoads;

// ---- Idempotency -----------------------------------------------------------
/// Retained replay records, most recent first. A replay older than the retained
/// window is reported as `NotFound` rather than silently re-planned.
inline constexpr std::size_t kMaxIdempotencyEntries = 64;

// ---- Power domain ----------------------------------------------------------
/// 1 PW expressed in watts. Quantities outside [-kMaxPowerWatts, kMaxPowerWatts]
/// are refused by checked arithmetic rather than clamped.
inline constexpr std::int64_t kMaxPowerWatts = 1'000'000'000'000'000;
inline constexpr std::uint32_t kMaxPartsPerMillion = 1'000'000;

// ---- Encoded artifacts -----------------------------------------------------
inline constexpr std::size_t kMaxStateBytes = 64u * 1024u * 1024u;
inline constexpr std::size_t kMaxJsonBytes = 4u * 1024u * 1024u;
inline constexpr std::size_t kMaxJsonDepth = 64;
inline constexpr std::size_t kMaxJsonStringBytes = 4096;
inline constexpr std::size_t kMaxJsonMembers = 4096;

// ---- Store generations -----------------------------------------------------
/// Committed state generations retained on disk. Older generations are retired
/// after a successful commit; the retained window is what a rollback check can
/// compare against.
inline constexpr std::uint64_t kGenerationsRetained = 2;

}  // namespace load_shedding::limits
