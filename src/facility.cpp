#include "load_shedding/facility.hpp"

#include <algorithm>
#include <string>

#include "load_shedding/text.hpp"

namespace load_shedding {

PowerEvidence PowerEvidence::known(Power value, EvidenceGeneration generation, Tick tick) {
  PowerEvidence evidence;
  evidence.state = EvidenceState::Known;
  evidence.value = value;
  evidence.generation = generation;
  evidence.tick = tick;
  return evidence;
}

Status PowerEvidence::validate(const char* what) const {
  const std::string label = what == nullptr ? "power evidence" : what;
  switch (state) {
    case EvidenceState::Known:
      break;
    case EvidenceState::Unknown:
    case EvidenceState::Unavailable:
    case EvidenceState::Unsupported:
    case EvidenceState::Denied:
    case EvidenceState::Unsafe:
      if (!value.is_zero()) {
        return Status::error(StatusCode::Rejected,
                            label + " carries a non-zero value while its state is '" +
                                std::string(to_string(state)) +
                                "'; unknown evidence must never be encoded as a number");
      }
      break;
    default:
      return Status::error(StatusCode::Unsupported, label + " has an unsupported evidence state");
  }
  if (value.watts() > limits::kMaxPowerWatts || value.watts() < -limits::kMaxPowerWatts) {
    return Status::error(StatusCode::OutOfRange, label + " is outside the supported power range");
  }
  return Status::success();
}

Status LoadRecord::validate() const {
  if (ref.empty()) {
    return Status::error(StatusCode::InvalidArgument, "load reference must not be empty");
  }
  const Status ref_valid = validate_identifier(ref.value(), limits::kMaxLoadRefBytes, "load reference");
  if (!ref_valid.ok()) {
    return ref_valid;
  }
  const Status contribution_valid = contribution.validate("load contribution");
  if (!contribution_valid.ok()) {
    return contribution_valid;
  }
  const Status note_valid = validate_text(note, limits::kMaxNoteBytes, "load note");
  if (!note_valid.ok()) {
    return note_valid;
  }
  return Status::success();
}

Status ProtectedObligation::validate() const {
  if (ref.empty()) {
    return Status::error(StatusCode::InvalidArgument, "obligation reference must not be empty");
  }
  const Status ref_valid =
      validate_identifier(ref.value(), limits::kMaxObligationRefBytes, "obligation reference");
  if (!ref_valid.ok()) {
    return ref_valid;
  }
  if (!load.empty()) {
    const Status load_valid = validate_identifier(load.value(), limits::kMaxLoadRefBytes, "obligation load reference");
    if (!load_valid.ok()) {
      return load_valid;
    }
  }
  if (reserved.is_negative()) {
    return Status::error(StatusCode::Rejected, "obligation reserved power must not be negative");
  }
  if (reserved.watts() > limits::kMaxPowerWatts) {
    return Status::error(StatusCode::OutOfRange, "obligation reserved power is outside the supported range");
  }
  const Status note_valid = validate_text(note, limits::kMaxNoteBytes, "obligation note");
  if (!note_valid.ok()) {
    return note_valid;
  }
  return Status::success();
}

Status FacilitySnapshot::validate() const {
  if (loads.size() > limits::kMaxLoads) {
    return Status::error(StatusCode::Overlong, "snapshot contains more loads than the bound");
  }
  if (obligations.size() > limits::kMaxObligations) {
    return Status::error(StatusCode::Overlong, "snapshot contains more obligations than the bound");
  }
  const Status demand_valid = total_demand.validate("total facility demand");
  if (!demand_valid.ok()) {
    return demand_valid;
  }
  for (const LoadRecord& load : loads) {
    const Status load_valid = load.validate();
    if (!load_valid.ok()) {
      return load_valid;
    }
  }
  for (const ProtectedObligation& obligation : obligations) {
    const Status obligation_valid = obligation.validate();
    if (!obligation_valid.ok()) {
      return obligation_valid;
    }
  }
  return Status::success();
}

const LoadRecord* FacilitySnapshot::find(const LoadRef& ref) const {
  const auto found = std::lower_bound(loads.begin(), loads.end(), ref,
                                      [](const LoadRecord& load, const LoadRef& value) {
                                        return load.ref < value;
                                      });
  if (found == loads.end() || !(found->ref == ref)) {
    return nullptr;
  }
  return &(*found);
}

std::string_view to_string(EvidenceState value) noexcept {
  switch (value) {
    case EvidenceState::Known: return "known";
    case EvidenceState::Unknown: return "unknown";
    case EvidenceState::Unavailable: return "unavailable";
    case EvidenceState::Unsupported: return "unsupported";
    case EvidenceState::Denied: return "denied";
    case EvidenceState::Unsafe: return "unsafe";
  }
  return "unknown";
}

Result<EvidenceState> evidence_state_from_string(std::string_view text) {
  if (text == "known") return EvidenceState::Known;
  if (text == "unknown") return EvidenceState::Unknown;
  if (text == "unavailable") return EvidenceState::Unavailable;
  if (text == "unsupported") return EvidenceState::Unsupported;
  if (text == "denied") return EvidenceState::Denied;
  if (text == "unsafe") return EvidenceState::Unsafe;
  return Status::error(StatusCode::InvalidArgument, "unknown evidence state '" + std::string(text) + "'");
}

}  // namespace load_shedding
