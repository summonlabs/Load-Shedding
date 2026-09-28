#pragma once

// The shedding policy: priority/service classes, the ordered stage plan, the
// freshness window, and the recovery ordering rule. A policy is immutable once
// installed; installing a new one produces a new policy generation, and every
// plan records the generation and digest it was planned against.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "load_shedding/digest.hpp"
#include "load_shedding/ids.hpp"
#include "load_shedding/limits.hpp"
#include "load_shedding/status.hpp"
#include "load_shedding/units.hpp"

namespace load_shedding {

/// Service class of a facility load.
enum class LoadClass : std::uint8_t {
  /// Carries a protected obligation. Never selected.
  Protected = 0,
  /// Structurally cannot be shed without breaching a protected service.
  NonSheddable = 1,
  /// Sheddable under policy.
  Sheddable = 2,
  /// Sheddable, and expected to tolerate later restoration.
  Deferrable = 3,
};

/// Priority class. Lower rank is more important; shedding proceeds from the
/// least important class downwards.
enum class PriorityClass : std::uint8_t {
  Critical = 0,
  Essential = 1,
  Important = 2,
  Standard = 3,
  Deferrable = 4,
  Optional = 5,
};

/// Rank used by every ordering rule. `Critical` is 0, `Optional` is 5.
std::uint32_t priority_rank(PriorityClass value) noexcept;
std::optional<PriorityClass> priority_from_rank(std::uint32_t rank) noexcept;
bool is_less_important(PriorityClass left, PriorityClass right) noexcept;

/// How a stage selects loads once they are ordered.
enum class SelectionMode : std::uint8_t {
  /// Select every admissible load until the request is covered. A load whose
  /// contribution crosses the target is still selected whole; the excess is
  /// reported as `overshoot`.
  WholeLoadGreedy = 0,
  /// Select a load only when its whole contribution still fits inside the
  /// request. Never overshoots; may leave a residual that a different pack
  /// could have covered. Deterministic, documented, and not an optimiser.
  NoOvershootGreedy = 1,
};

/// Order in which shed loads become restoration candidates.
enum class RecoveryOrder : std::uint8_t {
  /// Later stages first; inside a stage, more important loads first.
  ReverseStageThenPriority = 0,
  /// More important loads first, regardless of the stage that shed them.
  PriorityThenReverseStage = 1,
};

/// One stage of the shedding plan.
struct StageDefinition {
  StageIndex index;
  std::string name;
  /// Priority classes handled by this stage. Must be non-empty and unique across
  /// the whole policy: a priority belongs to at most one stage.
  std::vector<PriorityClass> priorities;
  /// Absolute power cap for this stage.
  std::optional<Power> max_shed;
  /// Cap relative to the facility's known sheddable demand, in parts per million,
  /// truncated toward zero.
  std::optional<std::uint32_t> max_shed_ppm;
  /// Cap on the number of loads this stage may select.
  std::optional<std::uint32_t> max_loads;
  /// Stage is usable only with an explicit emergency grant.
  bool requires_emergency_authority = false;

  Status validate() const;
};

/// A complete shedding policy.
struct SheddingPolicy {
  /// Stable schema version of the policy document itself.
  static constexpr std::uint32_t kSchemaVersion = 1;

  std::string name;
  PolicyGeneration generation;
  std::uint32_t schema_version = kSchemaVersion;
  SelectionMode mode = SelectionMode::WholeLoadGreedy;
  /// Load classes that policy considers shedable at all.
  std::vector<LoadClass> shed_classes{LoadClass::Sheddable, LoadClass::Deferrable};
  /// Priority classes that policy protects by default.
  std::vector<PriorityClass> protected_priorities{PriorityClass::Critical};
  /// Stages in evaluation order; indices must be contiguous from zero.
  std::vector<StageDefinition> stages;
  /// Maximum age, in logical ticks, of load contribution evidence.
  std::uint64_t max_evidence_age_ticks = 5;
  /// Maximum distance a load's evidence generation may lag the snapshot.
  std::uint64_t max_evidence_generation_lag = 1;
  RecoveryOrder recovery_order = RecoveryOrder::ReverseStageThenPriority;
  /// Minimum logical ticks a load must stay shed before it may be restored.
  std::optional<std::uint64_t> min_off_ticks;
  /// Minimum logical ticks a load must stay energized before it may be shed.
  std::optional<std::uint64_t> min_on_ticks;
  /// Policy permits stages marked `requires_emergency_authority`.
  bool allow_emergency_override = false;

  Status validate() const;

  /// Canonical digest of the policy content. Does not include the generation, so
  /// the same content installed twice produces the same content digest.
  Digest content_digest() const;
};

/// The documented default policy: three stages that shed Optional/Deferrable,
/// then Standard, then Important, leaving Critical protected. No stage requires
/// emergency authority, so protected loads are never selectable under it.
SheddingPolicy make_default_policy();

std::string_view to_string(LoadClass value) noexcept;
std::string_view to_string(PriorityClass value) noexcept;
std::string_view to_string(SelectionMode value) noexcept;
std::string_view to_string(RecoveryOrder value) noexcept;
Result<LoadClass> load_class_from_string(std::string_view text);
Result<PriorityClass> priority_class_from_string(std::string_view text);
Result<SelectionMode> selection_mode_from_string(std::string_view text);
Result<RecoveryOrder> recovery_order_from_string(std::string_view text);

}  // namespace load_shedding
