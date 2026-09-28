#include "load_shedding/ids.hpp"

namespace load_shedding {

Result<LoadRef> LoadRef::parse(std::string_view text) {
  const Status valid = validate_identifier(text, limits::kMaxLoadRefBytes, "load reference");
  if (!valid.ok()) {
    return valid;
  }
  LoadRef ref;
  ref.value_.assign(text);
  return ref;
}

Result<ObligationRef> ObligationRef::parse(std::string_view text) {
  const Status valid = validate_identifier(text, limits::kMaxObligationRefBytes, "obligation reference");
  if (!valid.ok()) {
    return valid;
  }
  ObligationRef ref;
  ref.value_.assign(text);
  return ref;
}

}  // namespace load_shedding
