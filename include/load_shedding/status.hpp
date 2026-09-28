#pragma once

// Explicit status and result types. The library never throws across its public
// API boundary and never reports "success with a silently wrong value": every
// state-dependent operation either returns a value or a status that names the
// exact reason it was refused.

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace load_shedding {

/// Failure classification. The set is closed: a new failure mode must be named
/// here rather than smuggled through a generic code with prose in the message.
enum class StatusCode : std::uint8_t {
  Ok = 0,
  /// A structurally invalid request: a field is empty, malformed, or out of the
  /// documented shape.
  InvalidArgument,
  /// A structurally valid value outside the accepted domain.
  OutOfRange,
  /// Checked integer arithmetic refused the operation.
  Overflow,
  /// The referenced object does not exist.
  NotFound,
  /// The referenced object already exists.
  AlreadyExists,
  /// Two load identities in one snapshot compare equal.
  DuplicateIdentity,
  /// The request was planned against a policy generation that is no longer current.
  StalePolicyGeneration,
  /// The request was planned against evidence that is no longer current.
  StaleEvidenceGeneration,
  /// The request was planned against downstream effect state that is no longer
  /// current.
  StaleEffectGeneration,
  /// The request carries a logical tick behind the store's logical clock.
  StaleTick,
  /// The request claims an authority epoch/incarnation that no longer holds.
  StaleAuthority,
  /// The referenced plan is not the current plan for its scope.
  StalePlan,
  /// The caller holds no writer authority for the store.
  AuthorityRequired,
  /// The caller tried to take authority that another incarnation holds.
  AuthorityConflict,
  /// The state revision the request was planned against is not current.
  RevisionConflict,
  /// A replay of a request identity carried a different request body.
  IdempotencyConflict,
  /// The request violates the active policy.
  PolicyViolation,
  /// The request would breach a protected obligation or non-sheddable floor.
  ProtectedObligation,
  /// A documented resource bound was exceeded.
  LimitExceeded,
  /// Another process or handle holds the store lock.
  LockConflict,
  /// The operating system refused the operation.
  PermissionDenied,
  /// An operating-system level operation failed.
  IoFailure,
  /// Stored bytes failed integrity or structural validation.
  Corrupt,
  /// Stored or supplied bytes ended early.
  Truncated,
  /// A length, count, or nesting depth exceeds its documented bound.
  Overlong,
  /// The format, schema, or enum value is not supported by this build.
  Unsupported,
  /// The store or engine is closed.
  Closed,
  /// The decision is refused by an explicit safety interlock.
  Rejected,
};

/// Stable machine-readable token for a status code.
std::string_view to_string(StatusCode code) noexcept;

/// Human-readable explanation of a status code.
std::string_view describe(StatusCode code) noexcept;

class Status {
 public:
  Status() noexcept = default;

  static Status success() noexcept { return Status(); }
  static Status error(StatusCode code, std::string message);

  bool ok() const noexcept { return code_ == StatusCode::Ok; }
  StatusCode code() const noexcept { return code_; }
  const std::string& message() const noexcept { return message_; }

  /// `"code: message"`, or `"ok"`.
  std::string to_string() const;

 private:
  StatusCode code_ = StatusCode::Ok;
  std::string message_;
};

/// Value-or-status result. `value()` is only valid when `ok()` is true; calling
/// it on a failure throws `std::logic_error` rather than returning a fabricated
/// value. Tests must inspect `status()` instead of relying on that throw.
template <class T>
class Result {
 public:
  using value_type = T;

  Result(T value) : value_(std::move(value)) {}          // NOLINT(google-explicit-constructor)
  Result(Status status) : status_(std::move(status)) {   // NOLINT(google-explicit-constructor)
    if (status_.ok()) {
      status_ = Status::error(StatusCode::InvalidArgument, "Result constructed from a success status");
    }
  }

  bool ok() const noexcept { return status_.ok(); }
  StatusCode code() const noexcept { return status_.code(); }
  const Status& status() const noexcept { return status_; }
  const std::string& message() const noexcept { return status_.message(); }

  T& value() & {
    require_ok();
    return value_;
  }
  const T& value() const& {
    require_ok();
    return value_;
  }
  T&& value() && {
    require_ok();
    return std::move(value_);
  }

  T value_or(T fallback) const {
    return ok() ? value_ : std::move(fallback);
  }

 private:
  void require_ok() const {
    if (!status_.ok()) {
      throw std::logic_error("Result::value() called on a failure: " + status_.to_string());
    }
  }

  Status status_;
  T value_{};
};

}  // namespace load_shedding
