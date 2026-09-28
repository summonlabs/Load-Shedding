#pragma once

// Append-only audit history. History records what the runtime did and what it
// refused; it is not a redo log and never participates in recovery. Entries carry
// a logical tick and a store-assigned sequence, never a wall-clock reading, so
// canonical state stays byte-deterministic.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "load_shedding/digest.hpp"
#include "load_shedding/ids.hpp"
#include "load_shedding/limits.hpp"

namespace load_shedding {

enum class AuditKind : std::uint8_t {
  StoreOpened = 0,
  StoreRecovered = 1,
  AuthorityTaken = 2,
  PolicyInstalled = 3,
  LoadUpserted = 4,
  LoadRemoved = 5,
  ObligationUpserted = 6,
  ObligationRemoved = 7,
  SnapshotReplaced = 8,
  PlanCommitted = 9,
  PlanReplayed = 10,
  PlanRefused = 11,
  RecoveryDecided = 12,
  EffectObserved = 13,
  EffectRefused = 14,
};

struct AuditEntry {
  Sequence sequence;
  AuditKind kind = AuditKind::StoreOpened;
  Tick tick;
  AuthorityEpoch authority_epoch;
  Incarnation incarnation;
  RequestId request;
  PlanId plan;
  /// Digest of the structured detail that was recorded with the entry.
  Digest detail_digest;
  /// Short human-readable detail. Bounded by limits::kMaxNoteBytes.
  std::string detail;
};

struct HistoryQuery {
  std::size_t offset = 0;
  std::size_t limit = 64;
  std::optional<AuditKind> kind;
  std::optional<PlanId> plan;
};

struct HistoryPage {
  std::vector<AuditEntry> entries;
  std::size_t total_matching = 0;
  std::size_t total_entries = 0;
};

std::string_view to_string(AuditKind value) noexcept;
Result<AuditKind> audit_kind_from_string(std::string_view text);

}  // namespace load_shedding
