#pragma once

// Facility evidence: what the runtime is told about loads, their sheddable
// contribution, and the protected obligations that bound any shed decision.
//
// Evidence is not authority. A load record says "this is what was observed"; it
// never says "this may be shed". Admissibility is decided by the policy, and the
// result of that decision is a plan that some other controller may choose to act
// on.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "load_shedding/digest.hpp"
#include "load_shedding/ids.hpp"
#include "load_shedding/policy.hpp"
#include "load_shedding/status.hpp"
#include "load_shedding/units.hpp"

namespace load_shedding {

/// State of one piece of evidence. These are seven distinct outcomes; only
/// `Known` carries a usable number, and every other state is preserved rather
/// than collapsed into zero.
enum class EvidenceState : std::uint8_t {
  Known = 0,
  /// No observation has ever been made.
  Unknown = 1,
  /// The source exists but is not currently answering.
  Unavailable = 2,
  /// The source answered with something this build cannot interpret.
  Unsupported = 3,
  /// The source refused to answer.
  Denied = 4,
  /// The source reported a condition in which the value must not be used.
  Unsafe = 5,
};

/// An exact power quantity together with its provenance and freshness. The value
/// is only meaningful when `state == Known`; validation refuses a non-zero value
/// attached to any other state so that unknown can never be read as zero and zero
/// can never be read as unknown.
struct PowerEvidence {
  EvidenceState state = EvidenceState::Unknown;
  Power value;
  EvidenceGeneration generation;
  Tick tick;

  static PowerEvidence known(Power value, EvidenceGeneration generation, Tick tick);
  Status validate(const char* what) const;
};

/// One facility load.
struct LoadRecord {
  LoadRef ref;
  LoadGeneration identity_generation;
  StateRevision revision;
  LoadClass load_class = LoadClass::Sheddable;
  PriorityClass priority = PriorityClass::Standard;
  /// Sheddable contribution of this load, as observed.
  PowerEvidence contribution;
  bool in_service = true;
  std::string note;

  Status validate() const;
};

/// A protected obligation: an explicit floor that shedding must not breach.
/// Obligations are absolute. They are not overridable by the emergency stage rule,
/// which can only reach loads protected by class or priority defaults.
struct ProtectedObligation {
  ObligationRef ref;
  /// Load that carries the obligation. May be the empty reference for an
  /// obligation that is not bound to a single load; such an obligation still
  /// contributes its reserved power to the shedding ceiling.
  LoadRef load;
  /// Power that must remain available to the obligation.
  Power reserved;
  bool active = true;
  EvidenceGeneration generation;
  Tick tick;
  StateRevision revision;
  std::string note;

  Status validate() const;
};

/// Complete facility evidence snapshot supplied by the operator or an upstream
/// evidence source.
struct FacilitySnapshot {
  SnapshotId id;
  EvidenceGeneration generation;
  StateRevision revision;
  Tick tick;
  /// Known total facility demand. Used for the protected-reserve ceiling check;
  /// when it is not known the ceiling cannot be verified and the plan says so.
  PowerEvidence total_demand;
  std::vector<LoadRecord> loads;
  std::vector<ProtectedObligation> obligations;

  Status validate() const;

  const LoadRecord* find(const LoadRef& ref) const;
};

std::string_view to_string(EvidenceState value) noexcept;
Result<EvidenceState> evidence_state_from_string(std::string_view text);

}  // namespace load_shedding
