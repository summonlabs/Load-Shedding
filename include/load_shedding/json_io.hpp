#pragma once

// JSON projection of every public result type, plus strict parsing of the input
// documents the command line tool accepts. Kept separate from the domain headers
// so that the decision core has no serialization dependency.
//
// Canonical output ordering is fixed by the writer (object members are sorted),
// so the same value always produces the same bytes.

#include "load_shedding/effect.hpp"
#include "load_shedding/engine.hpp"
#include "load_shedding/facility.hpp"
#include "load_shedding/history.hpp"
#include "load_shedding/json.hpp"
#include "load_shedding/plan.hpp"
#include "load_shedding/policy.hpp"
#include "load_shedding/recovery.hpp"
#include "load_shedding/store.hpp"

namespace load_shedding {

JsonValue to_json(Power value);
JsonValue to_json(const Digest& value);

JsonValue to_json(const PowerEvidence& evidence);
JsonValue to_json(const LoadRecord& load);
JsonValue to_json(const ProtectedObligation& obligation);
JsonValue to_json(const FacilitySnapshot& snapshot);

JsonValue to_json(const StageDefinition& stage);
JsonValue to_json(const SheddingPolicy& policy);

JsonValue to_json(const AccountingClosure& closure);
JsonValue to_json(const ShedAction& action);
JsonValue to_json(const LoadConsideration& consideration);
JsonValue to_json(const StageSummary& summary);
JsonValue to_json(const SheddingPlan& plan);
JsonValue to_json(const PlanOutcome& outcome);
JsonValue to_json(const PlanDiff& diff);

JsonValue to_json(const ObservedLoadState& state);
JsonValue to_json(const EffectObservation& observation);

JsonValue to_json(const RecoveryCandidate& candidate);
JsonValue to_json(const RecoveryDecision& decision);

JsonValue to_json(const AuditEntry& entry);
JsonValue to_json(const HistoryPage& page);

JsonValue to_json(const StoreGenerationInfo& info);
JsonValue to_json(const StoreAudit& audit);
JsonValue to_json(const CommitRecord& record);
JsonValue to_json(const AuthorityStatus& status);
JsonValue to_json(const VerificationFinding& finding);
JsonValue to_json(const VerificationReport& report);
JsonValue to_json(const RevalidationReport& report);

/// Parses a policy document. Unknown members are refused so that a typo in an
/// operator's policy file cannot silently change the shedding behaviour.
Result<SheddingPolicy> policy_from_json(const JsonValue& value);

/// Parses one load record.
Result<LoadRecord> load_record_from_json(const JsonValue& value);

/// Parses one protected obligation.
Result<ProtectedObligation> obligation_from_json(const JsonValue& value);

/// Parses a facility snapshot document.
Result<FacilitySnapshot> facility_snapshot_from_json(const JsonValue& value);

}  // namespace load_shedding
