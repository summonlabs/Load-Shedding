#include "load_shedding/policy.hpp"

#include <algorithm>
#include <string>

#include "detail/serialization.hpp"
#include "load_shedding/text.hpp"

namespace load_shedding {
namespace {

Status require_unique_priorities(const std::vector<PriorityClass>& priorities, const char* what) {
  if (priorities.empty()) {
    return Status::error(StatusCode::InvalidArgument, std::string(what) + " must not be empty");
  }
  if (priorities.size() > limits::kMaxPrioritiesPerStage) {
    return Status::error(StatusCode::Overlong, std::string(what) + " lists more priorities than the bound");
  }
  for (std::size_t index = 0; index < priorities.size(); ++index) {
    for (std::size_t other = index + 1; other < priorities.size(); ++other) {
      if (priorities[index] == priorities[other]) {
        return Status::error(StatusCode::DuplicateIdentity,
                             std::string(what) + " lists a priority class twice");
      }
    }
    if (index > 0 && priority_rank(priorities[index - 1]) >= priority_rank(priorities[index])) {
      return Status::error(StatusCode::InvalidArgument,
                           std::string(what) + " must be ordered by ascending priority rank");
    }
  }
  return Status::success();
}

}  // namespace

std::uint32_t priority_rank(PriorityClass value) noexcept {
  return static_cast<std::uint32_t>(value);
}

std::optional<PriorityClass> priority_from_rank(std::uint32_t rank) noexcept {
  if (rank > priority_rank(PriorityClass::Optional)) {
    return std::nullopt;
  }
  return static_cast<PriorityClass>(rank);
}

bool is_less_important(PriorityClass left, PriorityClass right) noexcept {
  return priority_rank(left) > priority_rank(right);
}

Status StageDefinition::validate() const {
  if (!index.is_set()) {
    return Status::error(StatusCode::InvalidArgument, "stage index must be set");
  }
  const Status name_valid = validate_text(name, limits::kMaxStageNameBytes, "stage name");
  if (!name_valid.ok()) {
    return name_valid;
  }
  if (name.empty()) {
    return Status::error(StatusCode::InvalidArgument, "stage name must not be empty");
  }
  const Status priorities_valid = require_unique_priorities(priorities, "stage priority list");
  if (!priorities_valid.ok()) {
    return priorities_valid;
  }
  if (max_shed.has_value()) {
    if (max_shed->is_negative() || max_shed->watts() > limits::kMaxPowerWatts) {
      return Status::error(StatusCode::OutOfRange, "stage power cap is outside the supported range");
    }
  }
  if (max_shed_ppm.has_value() && max_shed_ppm.value() > limits::kMaxPartsPerMillion) {
    return Status::error(StatusCode::OutOfRange, "stage parts-per-million cap exceeds 1000000");
  }
  if (max_loads.has_value() && (max_loads.value() == 0 || max_loads.value() > limits::kMaxLoads)) {
    return Status::error(StatusCode::OutOfRange, "stage load cap must be between 1 and the load bound");
  }
  return Status::success();
}

Status SheddingPolicy::validate() const {
  if (schema_version != kSchemaVersion) {
    return Status::error(StatusCode::Unsupported,
                         "policy schema version " + std::to_string(schema_version) +
                             " is not supported by this build");
  }
  const Status name_valid = validate_text(name, limits::kMaxPolicyNameBytes, "policy name");
  if (!name_valid.ok()) {
    return name_valid;
  }
  if (name.empty()) {
    return Status::error(StatusCode::InvalidArgument, "policy name must not be empty");
  }
  if (shed_classes.empty()) {
    return Status::error(StatusCode::InvalidArgument, "policy must consider at least one load class");
  }
  if (shed_classes.size() > 4) {
    return Status::error(StatusCode::Overlong, "policy lists more load classes than the bound");
  }
  for (std::size_t index = 0; index < shed_classes.size(); ++index) {
    for (std::size_t other = index + 1; other < shed_classes.size(); ++other) {
      if (shed_classes[index] == shed_classes[other]) {
        return Status::error(StatusCode::DuplicateIdentity, "policy lists a load class twice");
      }
    }
    if (shed_classes[index] == LoadClass::Protected || shed_classes[index] == LoadClass::NonSheddable) {
      return Status::error(StatusCode::PolicyViolation,
                           "policy must not list a protected or non-sheddable class as shedable");
    }
  }
  if (protected_priorities.size() > 6) {
    return Status::error(StatusCode::Overlong, "policy lists more protected priorities than the bound");
  }
  for (std::size_t index = 0; index < protected_priorities.size(); ++index) {
    for (std::size_t other = index + 1; other < protected_priorities.size(); ++other) {
      if (protected_priorities[index] == protected_priorities[other]) {
        return Status::error(StatusCode::DuplicateIdentity, "policy lists a protected priority twice");
      }
    }
  }
  if (stages.size() > limits::kMaxStages) {
    return Status::error(StatusCode::Overlong, "policy declares more stages than the bound");
  }
  for (std::size_t index = 0; index < stages.size(); ++index) {
    const StageDefinition& stage = stages[index];
    const Status stage_valid = stage.validate();
    if (!stage_valid.ok()) {
      return stage_valid;
    }
    if (stage.index.value() != index) {
      return Status::error(StatusCode::InvalidArgument,
                           "stage indices must be contiguous from zero; stage at position " +
                               std::to_string(index) + " declares index " +
                               std::to_string(stage.index.value()));
    }
    for (std::size_t other = index + 1; other < stages.size(); ++other) {
      for (const PriorityClass priority : stage.priorities) {
        if (std::find(stages[other].priorities.begin(), stages[other].priorities.end(), priority) !=
            stages[other].priorities.end()) {
          return Status::error(StatusCode::DuplicateIdentity,
                               "priority class " + std::string(to_string(priority)) +
                                   " is claimed by two stages");
        }
      }
    }
  }
  if (max_evidence_age_ticks > 1000000000ULL) {
    return Status::error(StatusCode::OutOfRange, "evidence age window is unreasonably large");
  }
  if (max_evidence_generation_lag > 1000000000ULL) {
    return Status::error(StatusCode::OutOfRange, "evidence generation lag is unreasonably large");
  }
  if (min_off_ticks.has_value() && min_off_ticks.value() > 1000000000ULL) {
    return Status::error(StatusCode::OutOfRange, "minimum off time is unreasonably large");
  }
  if (min_on_ticks.has_value() && min_on_ticks.value() > 1000000000ULL) {
    return Status::error(StatusCode::OutOfRange, "minimum on time is unreasonably large");
  }
  return Status::success();
}

Digest SheddingPolicy::content_digest() const { return detail::policy_content_digest(*this); }

SheddingPolicy make_default_policy() {
  SheddingPolicy policy;
  policy.name = "default";
  policy.mode = SelectionMode::WholeLoadGreedy;
  policy.shed_classes = {LoadClass::Sheddable, LoadClass::Deferrable};
  policy.protected_priorities = {PriorityClass::Critical};
  policy.max_evidence_age_ticks = 5;
  // Incremental evidence edits advance the facility evidence generation, so the
  // lag window is a facility-wide tolerance rather than a per-load counter. The
  // tight freshness control is `max_evidence_age_ticks`.
  policy.max_evidence_generation_lag = 32;
  policy.recovery_order = RecoveryOrder::ReverseStageThenPriority;
  policy.allow_emergency_override = false;

  StageDefinition deferrable;
  deferrable.index = StageIndex::from_ordinal(0);
  deferrable.name = "deferrable";
  deferrable.priorities = {PriorityClass::Deferrable, PriorityClass::Optional};

  StageDefinition standard;
  standard.index = StageIndex::from_ordinal(1);
  standard.name = "standard";
  standard.priorities = {PriorityClass::Standard};

  StageDefinition important;
  important.index = StageIndex::from_ordinal(2);
  important.name = "important";
  important.priorities = {PriorityClass::Important};

  policy.stages = {deferrable, standard, important};
  return policy;
}

std::string_view to_string(LoadClass value) noexcept {
  switch (value) {
    case LoadClass::Protected: return "protected";
    case LoadClass::NonSheddable: return "non-sheddable";
    case LoadClass::Sheddable: return "sheddable";
    case LoadClass::Deferrable: return "deferrable";
  }
  return "unknown";
}

std::string_view to_string(PriorityClass value) noexcept {
  switch (value) {
    case PriorityClass::Critical: return "critical";
    case PriorityClass::Essential: return "essential";
    case PriorityClass::Important: return "important";
    case PriorityClass::Standard: return "standard";
    case PriorityClass::Deferrable: return "deferrable";
    case PriorityClass::Optional: return "optional";
  }
  return "unknown";
}

std::string_view to_string(SelectionMode value) noexcept {
  switch (value) {
    case SelectionMode::WholeLoadGreedy: return "whole-load-greedy";
    case SelectionMode::NoOvershootGreedy: return "no-overshoot-greedy";
  }
  return "unknown";
}

std::string_view to_string(RecoveryOrder value) noexcept {
  switch (value) {
    case RecoveryOrder::ReverseStageThenPriority: return "reverse-stage-then-priority";
    case RecoveryOrder::PriorityThenReverseStage: return "priority-then-reverse-stage";
  }
  return "unknown";
}

Result<LoadClass> load_class_from_string(std::string_view text) {
  if (text == "protected") return LoadClass::Protected;
  if (text == "non-sheddable") return LoadClass::NonSheddable;
  if (text == "sheddable") return LoadClass::Sheddable;
  if (text == "deferrable") return LoadClass::Deferrable;
  return Status::error(StatusCode::InvalidArgument, "unknown load class '" + std::string(text) + "'");
}

Result<PriorityClass> priority_class_from_string(std::string_view text) {
  if (text == "critical") return PriorityClass::Critical;
  if (text == "essential") return PriorityClass::Essential;
  if (text == "important") return PriorityClass::Important;
  if (text == "standard") return PriorityClass::Standard;
  if (text == "deferrable") return PriorityClass::Deferrable;
  if (text == "optional") return PriorityClass::Optional;
  return Status::error(StatusCode::InvalidArgument, "unknown priority class '" + std::string(text) + "'");
}

Result<SelectionMode> selection_mode_from_string(std::string_view text) {
  if (text == "whole-load-greedy") return SelectionMode::WholeLoadGreedy;
  if (text == "no-overshoot-greedy") return SelectionMode::NoOvershootGreedy;
  return Status::error(StatusCode::InvalidArgument, "unknown selection mode '" + std::string(text) + "'");
}

Result<RecoveryOrder> recovery_order_from_string(std::string_view text) {
  if (text == "reverse-stage-then-priority") return RecoveryOrder::ReverseStageThenPriority;
  if (text == "priority-then-reverse-stage") return RecoveryOrder::PriorityThenReverseStage;
  return Status::error(StatusCode::InvalidArgument, "unknown recovery order '" + std::string(text) + "'");
}

}  // namespace load_shedding
