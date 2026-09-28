#pragma once

// Strongly typed identities. Identity, identity generation, state revision,
// policy generation, evidence generation, authority epoch, incarnation, attempt,
// and tick are semantically different things and therefore different types: a
// value of one kind cannot be passed where another is expected.

#include <cstdint>
#include <string>
#include <string_view>

#include "load_shedding/limits.hpp"
#include "load_shedding/status.hpp"
#include "load_shedding/text.hpp"

namespace load_shedding {

/// Monotonic counter wrapper. `Tag::kName` gives the type its stable external
/// token; the tag also makes every instantiation a distinct type.
template <class Tag>
class Id {
 public:
  using tag_type = Tag;

  constexpr Id() noexcept = default;
  constexpr explicit Id(std::uint64_t value) noexcept : value_(value) {}
  static constexpr Id from_value(std::uint64_t value) noexcept { return Id(value); }

  constexpr std::uint64_t value() const noexcept { return value_; }
  /// Zero means "unset" for every identifier in this library.
  constexpr bool is_set() const noexcept { return value_ != 0; }

  /// Smallest representable value, used for "beginning of time" comparisons.
  static constexpr Id first() noexcept { return Id(1); }

  Result<Id> next() const {
    if (value_ == UINT64_MAX) {
      return Status::error(StatusCode::Overflow, std::string(Tag::kName) + " counter exhausted");
    }
    return Id(value_ + 1);
  }

  std::string to_string() const {
    return std::string(Tag::kName) + ":" + std::to_string(value_);
  }

  /// Parses the `<name>:<decimal>` form produced by `to_string()`.
  static Result<Id> parse(std::string_view text) {
    const std::string prefix = std::string(Tag::kName) + ":";
    if (text.size() <= prefix.size() || text.substr(0, prefix.size()) != prefix) {
      return Status::error(StatusCode::InvalidArgument,
                           "expected '" + prefix + "<decimal>', got '" + std::string(text) + "'");
    }
    std::uint64_t value = 0;
    for (const char digit : text.substr(prefix.size())) {
      if (digit < '0' || digit > '9') {
        return Status::error(StatusCode::InvalidArgument,
                             "expected digits after '" + prefix + "', got '" + std::string(text) + "'");
      }
      const std::uint64_t previous = value;
      value = value * 10u + static_cast<std::uint64_t>(digit - '0');
      if (value < previous) {
        return Status::error(StatusCode::Overflow, std::string(Tag::kName) + " value out of range");
      }
    }
    return Id(value);
  }

  friend constexpr bool operator==(Id a, Id b) noexcept { return a.value_ == b.value_; }
  friend constexpr bool operator!=(Id a, Id b) noexcept { return a.value_ != b.value_; }
  friend constexpr bool operator<(Id a, Id b) noexcept { return a.value_ < b.value_; }
  friend constexpr bool operator<=(Id a, Id b) noexcept { return a.value_ <= b.value_; }
  friend constexpr bool operator>(Id a, Id b) noexcept { return a.value_ > b.value_; }
  friend constexpr bool operator>=(Id a, Id b) noexcept { return a.value_ >= b.value_; }

 private:
  std::uint64_t value_ = 0;
};

struct PolicyGenerationTag {
  static constexpr std::string_view kName = "policy-generation";
};
struct EvidenceGenerationTag {
  static constexpr std::string_view kName = "evidence-generation";
};
struct AuthorityEpochTag {
  static constexpr std::string_view kName = "authority-epoch";
};
struct IncarnationTag {
  static constexpr std::string_view kName = "incarnation";
};
struct PlanGenerationTag {
  static constexpr std::string_view kName = "plan-generation";
};
struct StateRevisionTag {
  static constexpr std::string_view kName = "state-revision";
};
struct AttemptTag {
  static constexpr std::string_view kName = "attempt";
};
struct RequestTag {
  static constexpr std::string_view kName = "request";
};
struct PlanTag {
  static constexpr std::string_view kName = "plan";
};
struct LoadGenerationTag {
  static constexpr std::string_view kName = "load-generation";
};
struct TickTag {
  static constexpr std::string_view kName = "tick";
};
struct SequenceTag {
  static constexpr std::string_view kName = "sequence";
};
struct StageTag {
  static constexpr std::string_view kName = "stage";
};
struct SnapshotTag {
  static constexpr std::string_view kName = "snapshot";
};
struct EffectGenerationTag {
  static constexpr std::string_view kName = "effect-generation";
};

/// Generation of the active shedding policy.
using PolicyGeneration = Id<PolicyGenerationTag>;
/// Generation of the facility evidence snapshot.
using EvidenceGeneration = Id<EvidenceGenerationTag>;
/// Fencing epoch of writer authority for a store.
using AuthorityEpoch = Id<AuthorityEpochTag>;
/// Identity of one running writer (one engine process instance).
using Incarnation = Id<IncarnationTag>;
/// Generation of a committed shedding plan.
using PlanGeneration = Id<PlanGenerationTag>;
/// Revision of authoritative state. Distinct from any generation counter.
using StateRevision = Id<StateRevisionTag>;
/// Identity of a downstream actuation attempt, supplied by the caller.
using AttemptId = Id<AttemptTag>;
/// Identity of a client request, chosen by the client so replays are detectable.
using RequestId = Id<RequestTag>;
/// Identity of a committed plan.
using PlanId = Id<PlanTag>;
/// Generation of one load's identity record.
using LoadGeneration = Id<LoadGenerationTag>;
/// Logical clock value. Load Shedding never reads a wall clock: all freshness is
/// expressed against caller-supplied logical ticks.
using Tick = Id<TickTag>;
/// Position in an append-only, store-assigned sequence.
using Sequence = Id<SequenceTag>;
/// Ordinal of a stage within a policy.
///
/// This is not an `Id`: stage ordinals start at zero, so zero is a valid stage
/// and "unset" needs its own representation. The distinction is explicit rather
/// than being smuggled through a sentinel value.
class StageIndex {
 public:
  constexpr StageIndex() noexcept = default;

  static constexpr StageIndex from_ordinal(std::uint32_t ordinal) noexcept {
    StageIndex index;
    index.value_ = ordinal;
    index.set_ = true;
    return index;
  }

  static constexpr StageIndex unset() noexcept { return StageIndex(); }

  constexpr std::uint32_t value() const noexcept { return value_; }
  constexpr std::uint32_t ordinal() const noexcept { return value_; }
  constexpr bool is_set() const noexcept { return set_; }

  std::string to_string() const {
    return set_ ? ("stage:" + std::to_string(value_)) : std::string("stage:none");
  }

  friend constexpr bool operator==(StageIndex left, StageIndex right) noexcept {
    return left.set_ == right.set_ && left.value_ == right.value_;
  }
  friend constexpr bool operator!=(StageIndex left, StageIndex right) noexcept {
    return !(left == right);
  }
  friend constexpr bool operator<(StageIndex left, StageIndex right) noexcept {
    if (left.set_ != right.set_) {
      return left.set_ < right.set_;
    }
    return left.value_ < right.value_;
  }
  friend constexpr bool operator>(StageIndex left, StageIndex right) noexcept { return right < left; }
  friend constexpr bool operator<=(StageIndex left, StageIndex right) noexcept {
    return !(right < left);
  }
  friend constexpr bool operator>=(StageIndex left, StageIndex right) noexcept {
    return !(left < right);
  }

 private:
  std::uint32_t value_ = 0;
  bool set_ = false;
};
using SnapshotId = Id<SnapshotTag>;
/// Generation of the downstream observed-load-state table. Distinct from the
/// facility evidence generation: one describes what loads are declared to be,
/// the other what downstream controllers have reported about their state.
using EffectGeneration = Id<EffectGenerationTag>;

/// Stable opaque identity of a facility load. The library never interprets the
/// value; it only requires it to be unambiguous, printable, and comparable.
class LoadRef {
 public:
  LoadRef() = default;

  static Result<LoadRef> parse(std::string_view text);

  const std::string& value() const noexcept { return value_; }
  bool empty() const noexcept { return value_.empty(); }
  std::string to_string() const { return value_; }

  friend bool operator==(const LoadRef& a, const LoadRef& b) noexcept { return a.value_ == b.value_; }
  friend bool operator!=(const LoadRef& a, const LoadRef& b) noexcept { return a.value_ != b.value_; }
  friend bool operator<(const LoadRef& a, const LoadRef& b) noexcept { return a.value_ < b.value_; }

 private:
  std::string value_;
};

/// Stable opaque identity of a protected obligation.
class ObligationRef {
 public:
  ObligationRef() = default;

  static Result<ObligationRef> parse(std::string_view text);

  const std::string& value() const noexcept { return value_; }
  bool empty() const noexcept { return value_.empty(); }
  std::string to_string() const { return value_; }

  friend bool operator==(const ObligationRef& a, const ObligationRef& b) noexcept {
    return a.value_ == b.value_;
  }
  friend bool operator<(const ObligationRef& a, const ObligationRef& b) noexcept {
    return a.value_ < b.value_;
  }

 private:
  std::string value_;
};

}  // namespace load_shedding
