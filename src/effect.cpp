#include "load_shedding/effect.hpp"

#include "detail/serialization.hpp"

namespace load_shedding {

Digest EffectObservationRequest::digest() const { return detail::effect_request_digest(*this); }

Digest EffectObservation::record_digest() const { return detail::effect_record_digest(*this); }

std::string_view to_string(EffectState value) noexcept {
  switch (value) {
    case EffectState::Unknown: return "unknown";
    case EffectState::Energized: return "energized";
    case EffectState::Shed: return "shed";
  }
  return "unknown";
}

std::string_view to_string(EffectSource value) noexcept {
  switch (value) {
    case EffectSource::Adapter: return "adapter";
    case EffectSource::Operator: return "operator";
    case EffectSource::Synthetic: return "synthetic";
  }
  return "unknown";
}

std::string_view to_string(EffectVerification value) noexcept {
  switch (value) {
    case EffectVerification::Acknowledged: return "acknowledged";
    case EffectVerification::Confirmed: return "confirmed";
    case EffectVerification::Contradicted: return "contradicted";
  }
  return "unknown";
}

Result<EffectState> effect_state_from_string(std::string_view text) {
  if (text == "unknown") return EffectState::Unknown;
  if (text == "energized") return EffectState::Energized;
  if (text == "shed") return EffectState::Shed;
  return Status::error(StatusCode::InvalidArgument, "unknown effect state '" + std::string(text) + "'");
}

Result<EffectSource> effect_source_from_string(std::string_view text) {
  if (text == "adapter") return EffectSource::Adapter;
  if (text == "operator") return EffectSource::Operator;
  if (text == "synthetic") return EffectSource::Synthetic;
  return Status::error(StatusCode::InvalidArgument, "unknown effect source '" + std::string(text) + "'");
}

Result<EffectVerification> effect_verification_from_string(std::string_view text) {
  if (text == "acknowledged") return EffectVerification::Acknowledged;
  if (text == "confirmed") return EffectVerification::Confirmed;
  if (text == "contradicted") return EffectVerification::Contradicted;
  return Status::error(StatusCode::InvalidArgument,
                       "unknown effect verification '" + std::string(text) + "'");
}

}  // namespace load_shedding
