#include "load_shedding/status.hpp"

namespace load_shedding {

std::string_view to_string(StatusCode code) noexcept {
  switch (code) {
    case StatusCode::Ok: return "ok";
    case StatusCode::InvalidArgument: return "invalid-argument";
    case StatusCode::OutOfRange: return "out-of-range";
    case StatusCode::Overflow: return "overflow";
    case StatusCode::NotFound: return "not-found";
    case StatusCode::AlreadyExists: return "already-exists";
    case StatusCode::DuplicateIdentity: return "duplicate-identity";
    case StatusCode::StalePolicyGeneration: return "stale-policy-generation";
    case StatusCode::StaleEvidenceGeneration: return "stale-evidence-generation";
    case StatusCode::StaleEffectGeneration: return "stale-effect-generation";
    case StatusCode::StaleTick: return "stale-tick";
    case StatusCode::StaleAuthority: return "stale-authority";
    case StatusCode::StalePlan: return "stale-plan";
    case StatusCode::AuthorityRequired: return "authority-required";
    case StatusCode::AuthorityConflict: return "authority-conflict";
    case StatusCode::RevisionConflict: return "revision-conflict";
    case StatusCode::IdempotencyConflict: return "idempotency-conflict";
    case StatusCode::PolicyViolation: return "policy-violation";
    case StatusCode::ProtectedObligation: return "protected-obligation";
    case StatusCode::LimitExceeded: return "limit-exceeded";
    case StatusCode::LockConflict: return "lock-conflict";
    case StatusCode::PermissionDenied: return "permission-denied";
    case StatusCode::IoFailure: return "io-failure";
    case StatusCode::Corrupt: return "corrupt";
    case StatusCode::Truncated: return "truncated";
    case StatusCode::Overlong: return "overlong";
    case StatusCode::Unsupported: return "unsupported";
    case StatusCode::Closed: return "closed";
    case StatusCode::Rejected: return "rejected";
  }
  return "unknown";
}

std::string_view describe(StatusCode code) noexcept {
  switch (code) {
    case StatusCode::Ok: return "the operation completed";
    case StatusCode::InvalidArgument: return "the request is structurally invalid";
    case StatusCode::OutOfRange: return "a value is outside its accepted domain";
    case StatusCode::Overflow: return "checked integer arithmetic refused the operation";
    case StatusCode::NotFound: return "the referenced object does not exist";
    case StatusCode::AlreadyExists: return "the referenced object already exists";
    case StatusCode::DuplicateIdentity: return "two load identities in one snapshot are equal";
    case StatusCode::StalePolicyGeneration: return "the request targets a policy generation that is not current";
    case StatusCode::StaleEvidenceGeneration: return "the request targets evidence that is not current";
    case StatusCode::StaleEffectGeneration: return "the request targets downstream effect state that is not current";
    case StatusCode::StaleTick: return "the request carries a logical tick behind the store clock";
    case StatusCode::StaleAuthority: return "the request claims authority that has been superseded";
    case StatusCode::StalePlan: return "the referenced plan is not current for its scope";
    case StatusCode::AuthorityRequired: return "this engine has no writer authority";
    case StatusCode::AuthorityConflict: return "another incarnation holds writer authority";
    case StatusCode::RevisionConflict: return "the request targets a state revision that is not current";
    case StatusCode::IdempotencyConflict: return "a replayed request identity carried different content";
    case StatusCode::PolicyViolation: return "the request violates the active policy";
    case StatusCode::ProtectedObligation: return "the request would breach a protected obligation";
    case StatusCode::LimitExceeded: return "a documented resource bound was exceeded";
    case StatusCode::LockConflict: return "another process holds the store lock";
    case StatusCode::PermissionDenied: return "the operating system refused the operation";
    case StatusCode::IoFailure: return "an operating-system level operation failed";
    case StatusCode::Corrupt: return "stored bytes failed integrity or structural validation";
    case StatusCode::Truncated: return "stored or supplied bytes ended early";
    case StatusCode::Overlong: return "a length, count, or depth exceeds its documented bound";
    case StatusCode::Unsupported: return "the format or enum value is not supported by this build";
    case StatusCode::Closed: return "the engine or store is closed";
    case StatusCode::Rejected: return "an explicit safety interlock refused the decision";
  }
  return "unknown status";
}

Status Status::error(StatusCode code, std::string message) {
  Status status;
  status.code_ = code;
  status.message_ = std::move(message);
  return status;
}

std::string Status::to_string() const {
  if (ok()) {
    return "ok";
  }
  return std::string(load_shedding::to_string(code_)) + ": " + message_;
}

}  // namespace load_shedding
