#include "load_shedding/effect.hpp"
#include "load_shedding/engine.hpp"
#include "load_shedding/recovery.hpp"

#include "detail/serialization.hpp"

namespace load_shedding {

Digest PlanRequest::digest() const { return detail::plan_request_digest(*this); }

Digest RecoveryRequest::digest() const { return detail::recovery_request_digest(*this); }

std::string_view to_string(RevalidationVerdict value) noexcept {
  switch (value) {
    case RevalidationVerdict::Current: return "current";
    case RevalidationVerdict::SupersededPolicy: return "superseded-policy";
    case RevalidationVerdict::SupersededEvidence: return "superseded-evidence";
    case RevalidationVerdict::SupersededEffect: return "superseded-effect";
    case RevalidationVerdict::SupersededRevision: return "superseded-revision";
    case RevalidationVerdict::AuthorityMoved: return "authority-moved";
    case RevalidationVerdict::PlanUnknown: return "plan-unknown";
    case RevalidationVerdict::DigestMismatch: return "digest-mismatch";
  }
  return "unknown";
}

}  // namespace load_shedding
