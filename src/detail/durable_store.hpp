#pragma once

// Durable, integrity-checked store with an explicit publication protocol.
//
// Protocol (the commit point is the atomic replacement of the commit marker):
//
//   1. encode the whole state canonically
//   2. write the staging generation file and flush it
//   3. read it back and verify its digest
//   4. atomically publish the generation file
//   5. write the staging commit marker and flush it
//   6. atomically replace the commit marker          <-- COMMIT POINT
//   7. advance the rollback watermark
//   8. retire generations outside the retained window and stray staging files
//
// Nothing before step 6 is authoritative. After a crash the store adopts the
// generation named by the marker, or refuses.
//
// Internal header: not installed.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "detail/state.hpp"
#include "load_shedding/store.hpp"
#include "load_shedding/status.hpp"

namespace load_shedding::detail {

/// Deterministic fault-injection points, documented in docs/store-format.md. A
/// crash point terminates the current process immediately at that durable stage,
/// without unwinding and without any interactive error-reporting path.
enum class CrashPoint : std::uint8_t {
  None = 0,
  AfterStagingWrite = 1,
  AfterStatePublish = 2,
  BeforeHeadCommit = 3,
  AfterHeadCommit = 4,
  AfterWatermark = 5,
};

std::string_view to_string(CrashPoint point) noexcept;
Result<CrashPoint> crash_point_from_string(std::string_view text);

struct StoreOpenOutcome {
  StoreState state;
  CommitRecord commit;
  bool created = false;
  std::vector<std::string> findings;
};

class DurableStore {
 public:
  static Result<DurableStore> open(const std::string& directory, bool create_if_missing);

  /// Publishes `state` as the next generation. The caller must hold the writer
  /// lock and must have validated the state.
  Result<CommitRecord> commit(const StoreState& state, CrashPoint crash_point);

  /// Re-reads the committed generation from disk.
  Result<StoreState> load_committed() const;

  /// Reads and verifies the committed generation and reports it.
  Result<StoreOpenOutcome> read_committed() const;

  const CommitRecord& commit_record() const noexcept { return commit_; }
  const std::string& directory() const noexcept { return directory_; }
  std::uint64_t watermark_generation() const noexcept { return watermark_; }
  bool created() const noexcept { return created_; }

  std::string state_file_name(std::uint64_t generation) const;
  std::string state_file_path(std::uint64_t generation) const;

 private:
  std::string directory_;
  CommitRecord commit_;
  std::uint64_t watermark_ = 0;
  bool created_ = false;
};

/// Parses the generation out of a `state-<digits>.lsg` file name.
Result<std::uint64_t> parse_state_file_name(std::string_view name);

}  // namespace load_shedding::detail
