#pragma once

// Durable store surface: format constants, commit record, and an inspection audit
// that can be run against a store directory without opening it for writing.
//
// The on-disk protocol is documented in docs/store-format.md. This header exposes
// only what an operator or a test needs to reason about it.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "load_shedding/digest.hpp"
#include "load_shedding/ids.hpp"
#include "load_shedding/status.hpp"

namespace load_shedding {

namespace store_format {
/// Bumped whenever the encoded layout changes in a way older readers cannot
/// refuse cleanly. A reader refuses any version it does not implement.
inline constexpr std::uint32_t kFormatVersion = 1;
inline constexpr std::string_view kCommitMarkerName = "HEAD";
inline constexpr std::string_view kWatermarkName = "WATERMARK";
inline constexpr std::string_view kLockName = "LOCK";
inline constexpr std::string_view kStatePrefix = "state-";
inline constexpr std::string_view kStateSuffix = ".lsg";
inline constexpr std::string_view kStagingSuffix = ".staging";
inline constexpr std::string_view kStateMagic = "LSSG";
inline constexpr std::string_view kMarkerMagic = "LSHD";
inline constexpr std::string_view kWatermarkMagic = "LSWM";
}  // namespace store_format

/// Identity of the state that is currently committed. This is what a reader
/// adopts after a crash, and what a writer must match before it may publish.
struct CommitRecord {
  std::uint64_t generation = 0;
  Digest state_digest;
  std::uint64_t state_bytes = 0;
  AuthorityEpoch authority_epoch;
  Incarnation incarnation;
  StateRevision revision;
  PolicyGeneration policy_generation;
  EvidenceGeneration evidence_generation;
  EffectGeneration effect_generation;
  PlanGeneration plan_generation;
};

struct StoreGenerationInfo {
  std::uint64_t generation = 0;
  std::string file_name;
  std::uint64_t bytes = 0;
  Digest digest;
  /// Referenced by the commit marker.
  bool committed = false;
  /// Integrity verified by re-reading and hashing the file.
  bool verified = false;
  /// Header and payload decoded successfully.
  bool decodable = false;
  std::string note;
};

/// Result of inspecting a store directory. Inspection never mutates anything and
/// never decides authority; it reports what is on disk.
struct StoreAudit {
  std::string directory;
  std::uint32_t format_version = 0;
  bool commit_marker_present = false;
  bool commit_marker_valid = false;
  CommitRecord commit;
  bool watermark_present = false;
  std::uint64_t watermark_generation = 0;
  /// True when the commit marker points behind the recorded watermark, which is
  /// the rollback boundary this store can detect on its own.
  bool rollback_detected = false;
  bool state_readable = false;
  std::vector<StoreGenerationInfo> generations;
  std::vector<std::string> stray_files;
  std::vector<std::string> findings;
  std::size_t load_count = 0;
  std::size_t plan_count = 0;
  std::size_t effect_count = 0;
  std::size_t audit_count = 0;
  std::size_t recovery_count = 0;
  /// The audit ran while another writer held the store lock.
  bool unsynchronized = false;
  /// Every check passed.
  bool clean = false;
};

/// Inspects a store directory. Read-only, lock-free, and safe to run against a
/// store that another process is writing: in that case the audit reports
/// `unsynchronized` and its findings are advisory.
Result<StoreAudit> inspect_store(const std::string& directory);

}  // namespace load_shedding
