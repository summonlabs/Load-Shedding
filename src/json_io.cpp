#include "load_shedding/json_io.hpp"

#include <algorithm>
#include <limits>
#include <string>

#include "load_shedding/text.hpp"

namespace load_shedding {
namespace {

JsonValue object_with(std::initializer_list<std::pair<const char*, JsonValue>> members) {
  JsonValue object = JsonValue::object();
  for (const auto& member : members) {
    object.set(member.first, member.second);
  }
  return object;
}

JsonValue id_json(std::uint64_t value) { return JsonValue::integer(static_cast<std::int64_t>(value)); }

JsonValue power_json(Power value) { return JsonValue::integer(value.watts()); }

JsonValue optional_power_json(const std::optional<Power>& value) {
  return value.has_value() ? power_json(value.value()) : JsonValue::null();
}

Status reject_unknown_members(const JsonValue& value, const char* what,
                              std::initializer_list<const char*> allowed) {
  if (!value.is_object()) {
    return Status::error(StatusCode::InvalidArgument, std::string(what) + " must be a JSON object");
  }
  for (const auto& member : value.members()) {
    bool known = false;
    for (const char* name : allowed) {
      if (member.first == name) {
        known = true;
        break;
      }
    }
    if (!known) {
      return Status::error(StatusCode::InvalidArgument,
                           std::string(what) + " has an unknown member '" + member.first + "'");
    }
  }
  return Status::success();
}

/// An explicit JSON null means "not configured", which is the same thing as an
/// absent member; anything else of the wrong shape is refused.
bool member_is_absent(const JsonValue* member) { return member == nullptr || member->is_null(); }

Result<std::uint64_t> uint_member(const JsonValue& value, const char* name, std::uint64_t fallback,
                                  bool required) {
  const JsonValue* member = value.find(name);
  if (member_is_absent(member)) {
    if (required) {
      return Status::error(StatusCode::InvalidArgument,
                           std::string("missing required member '") + name + "'");
    }
    return fallback;
  }
  if (!member->is_int()) {
    return Status::error(StatusCode::InvalidArgument,
                         std::string("member '") + name + "' must be an integer");
  }
  if (member->as_int() < 0) {
    return Status::error(StatusCode::OutOfRange,
                         std::string("member '") + name + "' must not be negative");
  }
  return static_cast<std::uint64_t>(member->as_int());
}

Result<std::string> string_member(const JsonValue& value, const char* name, const char* fallback,
                                  bool required) {
  const JsonValue* member = value.find(name);
  if (member_is_absent(member)) {
    if (required) {
      return Status::error(StatusCode::InvalidArgument,
                           std::string("missing required member '") + name + "'");
    }
    return std::string(fallback);
  }
  if (!member->is_string()) {
    return Status::error(StatusCode::InvalidArgument,
                         std::string("member '") + name + "' must be a string");
  }
  return member->as_string();
}

Result<bool> bool_member(const JsonValue& value, const char* name, bool fallback) {
  const JsonValue* member = value.find(name);
  if (member_is_absent(member)) {
    return fallback;
  }
  if (!member->is_bool()) {
    return Status::error(StatusCode::InvalidArgument,
                         std::string("member '") + name + "' must be a boolean");
  }
  return member->as_bool();
}

}  // namespace

JsonValue to_json(Power value) { return power_json(value); }

JsonValue to_json(const Digest& value) { return JsonValue::text(value.hex()); }

JsonValue to_json(const PowerEvidence& evidence) {
  return object_with({
      {"state", JsonValue::text(std::string(to_string(evidence.state)))},
      {"watts", power_json(evidence.value)},
      {"evidence_generation", id_json(evidence.generation.value())},
      {"tick", id_json(evidence.tick.value())},
  });
}

JsonValue to_json(const LoadRecord& load) {
  return object_with({
      {"ref", JsonValue::text(load.ref.value())},
      {"identity_generation", id_json(load.identity_generation.value())},
      {"revision", id_json(load.revision.value())},
      {"class", JsonValue::text(std::string(to_string(load.load_class)))},
      {"priority", JsonValue::text(std::string(to_string(load.priority)))},
      {"contribution", to_json(load.contribution)},
      {"in_service", JsonValue::boolean(load.in_service)},
      {"note", JsonValue::text(load.note)},
  });
}

JsonValue to_json(const ProtectedObligation& obligation) {
  return object_with({
      {"ref", JsonValue::text(obligation.ref.value())},
      {"load", JsonValue::text(obligation.load.value())},
      {"reserved_watts", power_json(obligation.reserved)},
      {"active", JsonValue::boolean(obligation.active)},
      {"evidence_generation", id_json(obligation.generation.value())},
      {"tick", id_json(obligation.tick.value())},
      {"revision", id_json(obligation.revision.value())},
      {"note", JsonValue::text(obligation.note)},
  });
}

JsonValue to_json(const FacilitySnapshot& snapshot) {
  JsonValue loads = JsonValue::array();
  for (const LoadRecord& load : snapshot.loads) {
    loads.push(to_json(load));
  }
  JsonValue obligations = JsonValue::array();
  for (const ProtectedObligation& obligation : snapshot.obligations) {
    obligations.push(to_json(obligation));
  }
  return object_with({
      {"id", id_json(snapshot.id.value())},
      {"evidence_generation", id_json(snapshot.generation.value())},
      {"revision", id_json(snapshot.revision.value())},
      {"tick", id_json(snapshot.tick.value())},
      {"total_demand", to_json(snapshot.total_demand)},
      {"loads", std::move(loads)},
      {"obligations", std::move(obligations)},
  });
}

JsonValue to_json(const StageDefinition& stage) {
  JsonValue priorities = JsonValue::array();
  for (const PriorityClass priority : stage.priorities) {
    priorities.push(JsonValue::text(std::string(to_string(priority))));
  }
  return object_with({
      {"index", id_json(stage.index.value())},
      {"name", JsonValue::text(stage.name)},
      {"priorities", std::move(priorities)},
      {"max_shed_watts", optional_power_json(stage.max_shed)},
      {"max_shed_ppm", stage.max_shed_ppm.has_value()
                           ? JsonValue::integer(stage.max_shed_ppm.value())
                           : JsonValue::null()},
      {"max_loads", stage.max_loads.has_value() ? JsonValue::integer(stage.max_loads.value())
                                                : JsonValue::null()},
      {"requires_emergency_authority", JsonValue::boolean(stage.requires_emergency_authority)},
  });
}

JsonValue to_json(const SheddingPolicy& policy) {
  JsonValue shed_classes = JsonValue::array();
  for (const LoadClass load_class : policy.shed_classes) {
    shed_classes.push(JsonValue::text(std::string(to_string(load_class))));
  }
  JsonValue protected_priorities = JsonValue::array();
  for (const PriorityClass priority : policy.protected_priorities) {
    protected_priorities.push(JsonValue::text(std::string(to_string(priority))));
  }
  JsonValue stages = JsonValue::array();
  for (const StageDefinition& stage : policy.stages) {
    stages.push(to_json(stage));
  }
  return object_with({
      {"name", JsonValue::text(policy.name)},
      {"schema_version", JsonValue::integer(policy.schema_version)},
      {"generation", id_json(policy.generation.value())},
      {"content_digest", JsonValue::text(policy.content_digest().hex())},
      {"mode", JsonValue::text(std::string(to_string(policy.mode)))},
      {"shed_classes", std::move(shed_classes)},
      {"protected_priorities", std::move(protected_priorities)},
      {"stages", std::move(stages)},
      {"max_evidence_age_ticks", JsonValue::integer(static_cast<std::int64_t>(policy.max_evidence_age_ticks))},
      {"max_evidence_generation_lag",
       JsonValue::integer(static_cast<std::int64_t>(policy.max_evidence_generation_lag))},
      {"recovery_order", JsonValue::text(std::string(to_string(policy.recovery_order)))},
      {"min_off_ticks", policy.min_off_ticks.has_value()
                            ? JsonValue::integer(static_cast<std::int64_t>(policy.min_off_ticks.value()))
                            : JsonValue::null()},
      {"min_on_ticks", policy.min_on_ticks.has_value()
                           ? JsonValue::integer(static_cast<std::int64_t>(policy.min_on_ticks.value()))
                           : JsonValue::null()},
      {"allow_emergency_override", JsonValue::boolean(policy.allow_emergency_override)},
  });
}

JsonValue to_json(const AccountingClosure& closure) {
  return object_with({
      {"requested_reduction_watts", power_json(closure.requested_reduction)},
      {"eligible_known_capacity_watts", power_json(closure.eligible_known_capacity)},
      {"selected_expected_reduction_watts", power_json(closure.selected_expected_reduction)},
      {"protected_known_amount_watts", power_json(closure.protected_known_amount)},
      {"non_sheddable_known_amount_watts", power_json(closure.non_sheddable_known_amount)},
      {"unavailable_known_amount_watts", power_json(closure.unavailable_known_amount)},
      {"residual_deficit_watts", power_json(closure.residual_deficit)},
      {"overshoot_watts", power_json(closure.overshoot)},
      {"remaining_eligible_known_capacity_watts", power_json(closure.remaining_eligible_known_capacity)},
      {"eligible_load_count", JsonValue::integer(static_cast<std::int64_t>(closure.eligible_load_count))},
      {"selected_load_count", JsonValue::integer(static_cast<std::int64_t>(closure.selected_load_count))},
      {"protected_load_count", JsonValue::integer(static_cast<std::int64_t>(closure.protected_load_count))},
      {"indeterminate_load_count", JsonValue::integer(static_cast<std::int64_t>(closure.indeterminate_load_count))},
      {"unavailable_load_count", JsonValue::integer(static_cast<std::int64_t>(closure.unavailable_load_count))},
      {"stale_load_count", JsonValue::integer(static_cast<std::int64_t>(closure.stale_load_count))},
      {"denied_load_count", JsonValue::integer(static_cast<std::int64_t>(closure.denied_load_count))},
      {"unsafe_load_count", JsonValue::integer(static_cast<std::int64_t>(closure.unsafe_load_count))},
      {"unsupported_load_count", JsonValue::integer(static_cast<std::int64_t>(closure.unsupported_load_count))},
      {"zero_contribution_load_count", JsonValue::integer(static_cast<std::int64_t>(closure.zero_contribution_load_count))},
      {"capacity_indeterminate", JsonValue::boolean(closure.capacity_indeterminate)},
      {"ceiling_verified", JsonValue::boolean(closure.ceiling_verified)},
      {"effective_shedding_ceiling_watts", optional_power_json(closure.effective_shedding_ceiling)},
  });
}

JsonValue to_json(const ShedAction& action) {
  return object_with({
      {"load", JsonValue::text(action.load.value())},
      {"stage", id_json(action.stage.value())},
      {"selection_index", JsonValue::integer(action.selection_index)},
      {"expected_contribution_watts", power_json(action.expected_contribution)},
      {"reason", JsonValue::text(std::string(to_string(action.reason)))},
      {"priority", JsonValue::text(std::string(to_string(action.priority)))},
      {"class", JsonValue::text(std::string(to_string(action.load_class)))},
      {"identity_generation", id_json(action.identity_generation.value())},
      {"load_revision", id_json(action.load_revision.value())},
      {"evidence_digest", JsonValue::text(action.evidence_digest.hex())},
  });
}

JsonValue to_json(const LoadConsideration& consideration) {
  return object_with({
      {"load", JsonValue::text(consideration.load.value())},
      {"stage", consideration.stage.is_set() ? JsonValue::integer(consideration.stage.value())
                                             : JsonValue::null()},
      {"priority", JsonValue::text(std::string(to_string(consideration.priority)))},
      {"class", JsonValue::text(std::string(to_string(consideration.load_class)))},
      {"evidence_state", JsonValue::text(std::string(to_string(consideration.evidence_state)))},
      {"verdict", JsonValue::text(std::string(to_string(consideration.verdict)))},
      {"contribution_watts", power_json(consideration.contribution)},
      {"identity_generation", id_json(consideration.identity_generation.value())},
      {"load_revision", id_json(consideration.load_revision.value())},
      {"evidence_generation", id_json(consideration.evidence_generation.value())},
      {"evidence_tick", id_json(consideration.evidence_tick.value())},
      {"order_index", consideration.order_index_set ? JsonValue::integer(consideration.order_index)
                                                    : JsonValue::null()},
  });
}

JsonValue to_json(const StageSummary& summary) {
  return object_with({
      {"index", id_json(summary.index.value())},
      {"name", JsonValue::text(summary.name)},
      {"considered", JsonValue::integer(summary.considered)},
      {"admissible", JsonValue::integer(summary.admissible)},
      {"selected", JsonValue::integer(summary.selected)},
      {"selected_power_watts", power_json(summary.selected_power)},
      {"effective_power_cap_watts", optional_power_json(summary.effective_power_cap)},
      {"load_cap", summary.load_cap.has_value() ? JsonValue::integer(summary.load_cap.value())
                                                : JsonValue::null()},
      {"stop_reason", JsonValue::text(std::string(to_string(summary.stop_reason)))},
      {"emergency_authority_required", JsonValue::boolean(summary.emergency_authority_required)},
  });
}

JsonValue to_json(const SheddingPlan& plan) {
  JsonValue generations = JsonValue::array();
  for (const EvidenceGeneration generation : plan.identity.evidence_generations) {
    generations.push(id_json(generation.value()));
  }
  JsonValue actions = JsonValue::array();
  for (const ShedAction& action : plan.actions) {
    actions.push(to_json(action));
  }
  JsonValue considerations = JsonValue::array();
  for (const LoadConsideration& consideration : plan.considerations) {
    considerations.push(to_json(consideration));
  }
  JsonValue stages = JsonValue::array();
  for (const StageSummary& summary : plan.stages) {
    stages.push(to_json(summary));
  }
  JsonValue identity = object_with({
      {"plan_id", id_json(plan.identity.plan_id.value())},
      {"plan_generation", id_json(plan.identity.generation.value())},
      {"request_id", id_json(plan.identity.request_id.value())},
      {"policy_generation", id_json(plan.identity.policy_generation.value())},
      {"policy_digest", JsonValue::text(plan.identity.policy_digest.hex())},
      {"base_revision", id_json(plan.identity.base_revision.value())},
      {"evidence_generation", id_json(plan.identity.evidence_generation.value())},
      {"effect_generation", id_json(plan.identity.effect_generation.value())},
      {"evidence_generations", std::move(generations)},
      {"authority_epoch", id_json(plan.identity.authority_epoch.value())},
      {"incarnation", id_json(plan.identity.incarnation.value())},
      {"tick", id_json(plan.identity.tick.value())},
      {"attempt", id_json(plan.identity.attempt.value())},
  });
  return object_with({
      {"identity", std::move(identity)},
      {"mode", JsonValue::text(std::string(to_string(plan.mode)))},
      {"coverage", JsonValue::text(std::string(to_string(plan.coverage)))},
      {"content_digest", JsonValue::text(plan.content_digest().hex())},
      {"emergency_authority_used", JsonValue::boolean(plan.emergency_authority_used)},
      {"emergency_justification", JsonValue::text(plan.emergency_justification)},
      {"closure", to_json(plan.closure)},
      {"actions", std::move(actions)},
      {"stages", std::move(stages)},
      {"considerations", std::move(considerations)},
  });
}

JsonValue to_json(const PlanOutcome& outcome) {
  return object_with({
      {"plan", to_json(outcome.plan)},
      {"attempt", id_json(outcome.attempt.value())},
      {"replayed", JsonValue::boolean(outcome.replayed)},
      {"request_digest", JsonValue::text(outcome.request_digest.hex())},
  });
}

JsonValue to_json(const PlanDiff& diff) {
  JsonValue entries = JsonValue::array();
  for (const PlanDiffEntry& entry : diff.entries) {
    JsonValue item = object_with({
        {"load", JsonValue::text(entry.load.value())},
        {"left_stage", entry.left_stage.has_value() ? JsonValue::integer(entry.left_stage->value())
                                                    : JsonValue::null()},
        {"right_stage", entry.right_stage.has_value() ? JsonValue::integer(entry.right_stage->value())
                                                      : JsonValue::null()},
        {"left_contribution_watts", power_json(entry.left_contribution)},
        {"right_contribution_watts", power_json(entry.right_contribution)},
    });
    switch (entry.kind) {
      case PlanDiffKind::Added: item.set("kind", JsonValue::text("added")); break;
      case PlanDiffKind::Removed: item.set("kind", JsonValue::text("removed")); break;
      case PlanDiffKind::StageChanged: item.set("kind", JsonValue::text("stage-changed")); break;
    }
    entries.push(std::move(item));
  }
  return object_with({
      {"left_plan", id_json(diff.left_plan.value())},
      {"left_generation", id_json(diff.left_generation.value())},
      {"left_digest", JsonValue::text(diff.left_digest.hex())},
      {"right_plan", id_json(diff.right_plan.value())},
      {"right_generation", id_json(diff.right_generation.value())},
      {"right_digest", JsonValue::text(diff.right_digest.hex())},
      {"left_selected_watts", power_json(diff.left_selected)},
      {"right_selected_watts", power_json(diff.right_selected)},
      {"left_residual_watts", power_json(diff.left_residual)},
      {"right_residual_watts", power_json(diff.right_residual)},
      {"entries", std::move(entries)},
  });
}

JsonValue to_json(const ObservedLoadState& state) {
  return object_with({
      {"load", JsonValue::text(state.load.value())},
      {"state", JsonValue::text(std::string(to_string(state.state)))},
      {"verification", JsonValue::text(std::string(to_string(state.verification)))},
      {"since_tick", id_json(state.since.value())},
      {"effect_generation", id_json(state.generation.value())},
      {"sequence", id_json(state.sequence.value())},
      {"source_plan", id_json(state.source_plan.value())},
      {"source_plan_generation", id_json(state.source_plan_generation.value())},
      {"attempt", id_json(state.attempt.value())},
  });
}

JsonValue to_json(const EffectObservation& observation) {
  return object_with({
      {"sequence", id_json(observation.sequence.value())},
      {"request_id", id_json(observation.request_id.value())},
      {"plan_id", id_json(observation.plan_id.value())},
      {"plan_generation", id_json(observation.plan_generation.value())},
      {"load", JsonValue::text(observation.load.value())},
      {"attempt", id_json(observation.attempt.value())},
      {"observed", JsonValue::text(std::string(to_string(observation.observed)))},
      {"source", JsonValue::text(std::string(to_string(observation.source)))},
      {"verification", JsonValue::text(std::string(to_string(observation.verification)))},
      {"tick", id_json(observation.tick.value())},
      {"effect_generation", id_json(observation.generation.value())},
      {"base_revision", id_json(observation.base_revision.value())},
      {"authority_epoch", id_json(observation.authority_epoch.value())},
      {"incarnation", id_json(observation.incarnation.value())},
      {"plan_digest_at_observation", JsonValue::text(observation.plan_digest_at_observation.hex())},
      {"record_digest", JsonValue::text(observation.record_digest().hex())},
  });
}

JsonValue to_json(const RecoveryCandidate& candidate) {
  return object_with({
      {"load", JsonValue::text(candidate.load.value())},
      {"stage", candidate.stage.is_set() ? JsonValue::integer(candidate.stage.value())
                                         : JsonValue::null()},
      {"priority", JsonValue::text(std::string(to_string(candidate.priority)))},
      {"class", JsonValue::text(std::string(to_string(candidate.load_class)))},
      {"contribution_watts", power_json(candidate.contribution)},
      {"shed_tick", id_json(candidate.shed_tick.value())},
      {"observed", JsonValue::text(std::string(to_string(candidate.observed)))},
      {"verification", JsonValue::text(std::string(to_string(candidate.verification)))},
      {"observed_generation", id_json(candidate.observed_generation.value())},
      {"verdict", JsonValue::text(std::string(to_string(candidate.verdict)))},
      {"order_index", candidate.order_index_set ? JsonValue::integer(candidate.order_index)
                                                : JsonValue::null()},
      {"shed_by_plan", JsonValue::boolean(candidate.shed_by_plan)},
  });
}

JsonValue to_json(const RecoveryDecision& decision) {
  JsonValue candidates = JsonValue::array();
  for (const RecoveryCandidate& candidate : decision.candidates) {
    candidates.push(to_json(candidate));
  }
  return object_with({
      {"request_id", id_json(decision.request_id.value())},
      {"source_plan", id_json(decision.source_plan.value())},
      {"source_plan_generation", id_json(decision.source_plan_generation.value())},
      {"source_plan_digest", JsonValue::text(decision.source_plan_digest.hex())},
      {"content_digest", JsonValue::text(decision.content_digest().hex())},
      {"policy_generation", id_json(decision.policy_generation.value())},
      {"evidence_generation", id_json(decision.evidence_generation.value())},
      {"effect_generation", id_json(decision.effect_generation.value())},
      {"base_revision", id_json(decision.base_revision.value())},
      {"authority_epoch", id_json(decision.authority_epoch.value())},
      {"incarnation", id_json(decision.incarnation.value())},
      {"tick", id_json(decision.tick.value())},
      {"order", JsonValue::text(std::string(to_string(decision.order)))},
      {"requested_headroom_watts", power_json(decision.requested_headroom)},
      {"restored_expected_watts", power_json(decision.restored_expected)},
      {"remaining_headroom_watts", power_json(decision.remaining_headroom)},
      {"restored_count", JsonValue::integer(static_cast<std::int64_t>(decision.restored_count))},
      {"candidate_count", JsonValue::integer(static_cast<std::int64_t>(decision.candidate_count))},
      {"awaiting_confirmation_count",
       JsonValue::integer(static_cast<std::int64_t>(decision.awaiting_confirmation_count))},
      {"candidates", std::move(candidates)},
  });
}

JsonValue to_json(const AuditEntry& entry) {
  return object_with({
      {"sequence", id_json(entry.sequence.value())},
      {"kind", JsonValue::text(std::string(to_string(entry.kind)))},
      {"tick", id_json(entry.tick.value())},
      {"authority_epoch", id_json(entry.authority_epoch.value())},
      {"incarnation", id_json(entry.incarnation.value())},
      {"request_id", id_json(entry.request.value())},
      {"plan_id", id_json(entry.plan.value())},
      {"detail", JsonValue::text(entry.detail)},
      {"detail_digest", JsonValue::text(entry.detail_digest.hex())},
  });
}

JsonValue to_json(const HistoryPage& page) {
  JsonValue entries = JsonValue::array();
  for (const AuditEntry& entry : page.entries) {
    entries.push(to_json(entry));
  }
  return object_with({
      {"total_entries", JsonValue::integer(static_cast<std::int64_t>(page.total_entries))},
      {"total_matching", JsonValue::integer(static_cast<std::int64_t>(page.total_matching))},
      {"entries", std::move(entries)},
  });
}

JsonValue to_json(const StoreGenerationInfo& info) {
  return object_with({
      {"generation", id_json(info.generation)},
      {"file", JsonValue::text(info.file_name)},
      {"bytes", id_json(info.bytes)},
      {"digest", JsonValue::text(info.digest.hex())},
      {"committed", JsonValue::boolean(info.committed)},
      {"verified", JsonValue::boolean(info.verified)},
      {"decodable", JsonValue::boolean(info.decodable)},
      {"note", JsonValue::text(info.note)},
  });
}

JsonValue to_json(const CommitRecord& record) {
  return object_with({
      {"generation", id_json(record.generation)},
      {"state_digest", JsonValue::text(record.state_digest.hex())},
      {"state_bytes", id_json(record.state_bytes)},
      {"authority_epoch", id_json(record.authority_epoch.value())},
      {"incarnation", id_json(record.incarnation.value())},
      {"revision", id_json(record.revision.value())},
      {"policy_generation", id_json(record.policy_generation.value())},
      {"evidence_generation", id_json(record.evidence_generation.value())},
      {"effect_generation", id_json(record.effect_generation.value())},
      {"plan_generation", id_json(record.plan_generation.value())},
  });
}

JsonValue to_json(const StoreAudit& audit) {
  JsonValue generations = JsonValue::array();
  for (const StoreGenerationInfo& info : audit.generations) {
    generations.push(to_json(info));
  }
  JsonValue findings = JsonValue::array();
  for (const std::string& finding : audit.findings) {
    findings.push(JsonValue::text(finding));
  }
  JsonValue stray = JsonValue::array();
  for (const std::string& name : audit.stray_files) {
    stray.push(JsonValue::text(name));
  }
  return object_with({
      {"directory", JsonValue::text(audit.directory)},
      {"format_version", JsonValue::integer(audit.format_version)},
      {"commit_marker_present", JsonValue::boolean(audit.commit_marker_present)},
      {"commit_marker_valid", JsonValue::boolean(audit.commit_marker_valid)},
      {"commit", to_json(audit.commit)},
      {"watermark_present", JsonValue::boolean(audit.watermark_present)},
      {"watermark_generation", id_json(audit.watermark_generation)},
      {"rollback_detected", JsonValue::boolean(audit.rollback_detected)},
      {"state_readable", JsonValue::boolean(audit.state_readable)},
      {"clean", JsonValue::boolean(audit.clean)},
      {"unsynchronized", JsonValue::boolean(audit.unsynchronized)},
      {"load_count", id_json(audit.load_count)},
      {"plan_count", id_json(audit.plan_count)},
      {"effect_count", id_json(audit.effect_count)},
      {"audit_count", id_json(audit.audit_count)},
      {"recovery_count", id_json(audit.recovery_count)},
      {"generations", std::move(generations)},
      {"stray_files", std::move(stray)},
      {"findings", std::move(findings)},
  });
}

JsonValue to_json(const AuthorityStatus& status) {
  return object_with({
      {"store_directory", JsonValue::text(status.store_directory)},
      {"authority_epoch", id_json(status.epoch.value())},
      {"incarnation", id_json(status.incarnation.value())},
      {"writer", JsonValue::boolean(status.writer)},
      {"state_generation", id_json(status.state_generation)},
  });
}

JsonValue to_json(const VerificationFinding& finding) {
  return object_with({
      {"check", JsonValue::text(finding.check)},
      {"detail", JsonValue::text(finding.detail)},
      {"subject", JsonValue::text(finding.subject.hex())},
  });
}

JsonValue to_json(const VerificationReport& report) {
  JsonValue checks = JsonValue::array();
  for (const std::string& check : report.checks) {
    checks.push(JsonValue::text(check));
  }
  JsonValue failures = JsonValue::array();
  for (const VerificationFinding& finding : report.failures) {
    failures.push(to_json(finding));
  }
  return object_with({
      {"ok", JsonValue::boolean(report.ok)},
      {"revision", id_json(report.revision.value())},
      {"state_digest", JsonValue::text(report.state_digest.hex())},
      {"plans_verified", id_json(report.plans_verified)},
      {"recovery_decisions_verified", id_json(report.recovery_decisions_verified)},
      {"effect_records_verified", id_json(report.effect_records_verified)},
      {"audit_entries_verified", id_json(report.audit_entries_verified)},
      {"checks", std::move(checks)},
      {"failures", std::move(failures)},
  });
}

JsonValue to_json(const RevalidationReport& report) {
  JsonValue reasons = JsonValue::array();
  for (const std::string& reason : report.reasons) {
    reasons.push(JsonValue::text(reason));
  }
  return object_with({
      {"plan", id_json(report.plan.value())},
      {"generation", id_json(report.generation.value())},
      {"recorded_digest", JsonValue::text(report.recorded_digest.hex())},
      {"recomputed_digest", JsonValue::text(report.recomputed_digest.hex())},
      {"verdict", JsonValue::text(std::string(to_string(report.verdict)))},
      {"reasons", std::move(reasons)},
  });
}

Result<SheddingPolicy> policy_from_json(const JsonValue& value) {
  // `generation` is assigned by the engine; `content_digest` is derived and is
  // verified when present, so a policy document cannot be altered without the
  // change being noticed.
  const Status known = reject_unknown_members(
      value, "policy",
      {"name", "schema_version", "mode", "shed_classes", "protected_priorities", "stages",
       "max_evidence_age_ticks", "max_evidence_generation_lag", "recovery_order", "min_off_ticks",
       "min_on_ticks", "allow_emergency_override", "generation", "content_digest"});
  if (!known.ok()) {
    return known;
  }
  SheddingPolicy policy;
  auto name = string_member(value, "name", "policy", true);
  if (!name.ok()) {
    return name.status();
  }
  policy.name = name.value();
  auto schema = uint_member(value, "schema_version", SheddingPolicy::kSchemaVersion, false);
  if (!schema.ok()) {
    return schema.status();
  }
  policy.schema_version = static_cast<std::uint32_t>(schema.value());
  auto mode = string_member(value, "mode", "whole-load-greedy", false);
  if (!mode.ok()) {
    return mode.status();
  }
  auto parsed_mode = selection_mode_from_string(mode.value());
  if (!parsed_mode.ok()) {
    return parsed_mode.status();
  }
  policy.mode = parsed_mode.value();

  const JsonValue* shed_classes = value.find("shed_classes");
  if (shed_classes != nullptr) {
    if (!shed_classes->is_array()) {
      return Status::error(StatusCode::InvalidArgument, "shed_classes must be an array");
    }
    policy.shed_classes.clear();
    for (const JsonValue& entry : shed_classes->items()) {
      if (!entry.is_string()) {
        return Status::error(StatusCode::InvalidArgument, "shed_classes entries must be strings");
      }
      auto parsed = load_class_from_string(entry.as_string());
      if (!parsed.ok()) {
        return parsed.status();
      }
      policy.shed_classes.push_back(parsed.value());
    }
  }
  const JsonValue* protected_priorities = value.find("protected_priorities");
  if (protected_priorities != nullptr) {
    if (!protected_priorities->is_array()) {
      return Status::error(StatusCode::InvalidArgument, "protected_priorities must be an array");
    }
    policy.protected_priorities.clear();
    for (const JsonValue& entry : protected_priorities->items()) {
      if (!entry.is_string()) {
        return Status::error(StatusCode::InvalidArgument,
                             "protected_priorities entries must be strings");
      }
      auto parsed = priority_class_from_string(entry.as_string());
      if (!parsed.ok()) {
        return parsed.status();
      }
      policy.protected_priorities.push_back(parsed.value());
    }
  }
  const JsonValue* stages = value.find("stages");
  if (stages != nullptr) {
    if (!stages->is_array()) {
      return Status::error(StatusCode::InvalidArgument, "stages must be an array");
    }
    if (stages->items().size() > limits::kMaxStages) {
      return Status::error(StatusCode::Overlong, "policy declares more stages than the bound");
    }
    policy.stages.clear();
    std::uint32_t ordinal = 0;
    for (const JsonValue& entry : stages->items()) {
      const Status stage_known = reject_unknown_members(
          entry, "stage",
          {"index", "name", "priorities", "max_shed_watts", "max_shed_ppm", "max_loads",
           "requires_emergency_authority"});
      if (!stage_known.ok()) {
        return stage_known;
      }
      StageDefinition stage;
      stage.index = StageIndex::from_ordinal(ordinal);
      auto stage_name = string_member(entry, "name", "", true);
      if (!stage_name.ok()) {
        return stage_name.status();
      }
      stage.name = stage_name.value();
      const JsonValue* priorities = entry.find("priorities");
      if (priorities == nullptr || !priorities->is_array() || priorities->items().empty()) {
        return Status::error(StatusCode::InvalidArgument, "stage priorities must be a non-empty array");
      }
      for (const JsonValue& priority : priorities->items()) {
        if (!priority.is_string()) {
          return Status::error(StatusCode::InvalidArgument, "stage priorities must be strings");
        }
        auto parsed = priority_class_from_string(priority.as_string());
        if (!parsed.ok()) {
          return parsed.status();
        }
        stage.priorities.push_back(parsed.value());
      }
      std::sort(stage.priorities.begin(), stage.priorities.end(),
                [](PriorityClass left, PriorityClass right) {
                  return priority_rank(left) < priority_rank(right);
                });
      const JsonValue* max_shed = entry.find("max_shed_watts");
      if (max_shed != nullptr && !max_shed->is_null()) {
        if (!max_shed->is_int() || max_shed->as_int() < 0) {
          return Status::error(StatusCode::InvalidArgument, "max_shed_watts must be a non-negative integer");
        }
        stage.max_shed = Power::from_watts(max_shed->as_int());
      }
      auto ppm = uint_member(entry, "max_shed_ppm",
                             std::numeric_limits<std::uint64_t>::max(), false);
      if (!ppm.ok()) {
        return ppm.status();
      }
      if (ppm.value() != std::numeric_limits<std::uint64_t>::max()) {
        if (ppm.value() > limits::kMaxPartsPerMillion) {
          return Status::error(StatusCode::OutOfRange, "max_shed_ppm exceeds 1000000");
        }
        stage.max_shed_ppm = static_cast<std::uint32_t>(ppm.value());
      }
      auto loads = uint_member(entry, "max_loads", std::numeric_limits<std::uint64_t>::max(), false);
      if (!loads.ok()) {
        return loads.status();
      }
      if (loads.value() != std::numeric_limits<std::uint64_t>::max()) {
        if (loads.value() > limits::kMaxLoads) {
          return Status::error(StatusCode::OutOfRange, "max_loads exceeds the load bound");
        }
        stage.max_loads = static_cast<std::uint32_t>(loads.value());
      }
      auto emergency = bool_member(entry, "requires_emergency_authority", false);
      if (!emergency.ok()) {
        return emergency.status();
      }
      stage.requires_emergency_authority = emergency.value();
      const Status valid = stage.validate();
      if (!valid.ok()) {
        return valid;
      }
      policy.stages.push_back(stage);
      ++ordinal;
    }
  }

  auto age = uint_member(value, "max_evidence_age_ticks", policy.max_evidence_age_ticks, false);
  if (!age.ok()) {
    return age.status();
  }
  policy.max_evidence_age_ticks = age.value();
  auto lag = uint_member(value, "max_evidence_generation_lag", policy.max_evidence_generation_lag, false);
  if (!lag.ok()) {
    return lag.status();
  }
  policy.max_evidence_generation_lag = lag.value();
  auto order = string_member(value, "recovery_order", "reverse-stage-then-priority", false);
  if (!order.ok()) {
    return order.status();
  }
  auto parsed_order = recovery_order_from_string(order.value());
  if (!parsed_order.ok()) {
    return parsed_order.status();
  }
  policy.recovery_order = parsed_order.value();
  auto min_off = uint_member(value, "min_off_ticks", std::numeric_limits<std::uint64_t>::max(), false);
  if (!min_off.ok()) {
    return min_off.status();
  }
  if (min_off.value() != std::numeric_limits<std::uint64_t>::max()) {
    policy.min_off_ticks = min_off.value();
  }
  auto min_on = uint_member(value, "min_on_ticks", std::numeric_limits<std::uint64_t>::max(), false);
  if (!min_on.ok()) {
    return min_on.status();
  }
  if (min_on.value() != std::numeric_limits<std::uint64_t>::max()) {
    policy.min_on_ticks = min_on.value();
  }
  auto emergency_allowed = bool_member(value, "allow_emergency_override", false);
  if (!emergency_allowed.ok()) {
    return emergency_allowed.status();
  }
  policy.allow_emergency_override = emergency_allowed.value();
  const Status valid = policy.validate();
  if (!valid.ok()) {
    return valid;
  }
  const JsonValue* declared_digest = value.find("content_digest");
  if (declared_digest != nullptr && !declared_digest->is_null()) {
    if (!declared_digest->is_string()) {
      return Status::error(StatusCode::InvalidArgument, "content_digest must be a string");
    }
    auto parsed_digest = Digest::from_hex(declared_digest->as_string());
    if (!parsed_digest.ok()) {
      return parsed_digest.status();
    }
    if (!(parsed_digest.value() == policy.content_digest())) {
      return Status::error(StatusCode::Corrupt,
                           "policy content does not match the declared content_digest");
    }
  }
  return policy;
}

Result<LoadRecord> load_record_from_json(const JsonValue& value) {
  const Status known = reject_unknown_members(
      value, "load",
      {"ref", "class", "priority", "in_service", "note", "contribution_watts", "evidence_state",
       "tick", "identity_generation"});
  if (!known.ok()) {
    return known;
  }
  LoadRecord load;
  auto ref = string_member(value, "ref", "", true);
  if (!ref.ok()) {
    return ref.status();
  }
  auto parsed_ref = LoadRef::parse(ref.value());
  if (!parsed_ref.ok()) {
    return parsed_ref.status();
  }
  load.ref = parsed_ref.value();
  auto load_class = string_member(value, "class", "sheddable", false);
  if (!load_class.ok()) {
    return load_class.status();
  }
  auto parsed_class = load_class_from_string(load_class.value());
  if (!parsed_class.ok()) {
    return parsed_class.status();
  }
  load.load_class = parsed_class.value();
  auto priority = string_member(value, "priority", "standard", false);
  if (!priority.ok()) {
    return priority.status();
  }
  auto parsed_priority = priority_class_from_string(priority.value());
  if (!parsed_priority.ok()) {
    return parsed_priority.status();
  }
  load.priority = parsed_priority.value();
  auto in_service = bool_member(value, "in_service", true);
  if (!in_service.ok()) {
    return in_service.status();
  }
  load.in_service = in_service.value();
  auto note = string_member(value, "note", "", false);
  if (!note.ok()) {
    return note.status();
  }
  load.note = note.value();
  auto state = string_member(value, "evidence_state", "known", false);
  if (!state.ok()) {
    return state.status();
  }
  auto parsed_state = evidence_state_from_string(state.value());
  if (!parsed_state.ok()) {
    return parsed_state.status();
  }
  load.contribution.state = parsed_state.value();
  auto tick = uint_member(value, "tick", 0, true);
  if (!tick.ok()) {
    return tick.status();
  }
  load.contribution.tick = Tick::from_value(tick.value());
  auto watts = uint_member(value, "contribution_watts", 0, false);
  if (!watts.ok()) {
    return watts.status();
  }
  load.contribution.value = Power::from_watts(static_cast<std::int64_t>(watts.value()));
  auto identity = uint_member(value, "identity_generation", 0, false);
  if (!identity.ok()) {
    return identity.status();
  }
  load.identity_generation = LoadGeneration::from_value(identity.value());
  const Status valid = load.validate();
  if (!valid.ok()) {
    return valid;
  }
  return load;
}

Result<ProtectedObligation> obligation_from_json(const JsonValue& value) {
  const Status known = reject_unknown_members(value, "obligation",
                                              {"ref", "load", "reserved_watts", "active", "tick", "note"});
  if (!known.ok()) {
    return known;
  }
  ProtectedObligation obligation;
  auto ref = string_member(value, "ref", "", true);
  if (!ref.ok()) {
    return ref.status();
  }
  auto parsed_ref = ObligationRef::parse(ref.value());
  if (!parsed_ref.ok()) {
    return parsed_ref.status();
  }
  obligation.ref = parsed_ref.value();
  auto load = string_member(value, "load", "", false);
  if (!load.ok()) {
    return load.status();
  }
  if (!load.value().empty()) {
    auto parsed_load = LoadRef::parse(load.value());
    if (!parsed_load.ok()) {
      return parsed_load.status();
    }
    obligation.load = parsed_load.value();
  }
  auto reserved = uint_member(value, "reserved_watts", 0, true);
  if (!reserved.ok()) {
    return reserved.status();
  }
  obligation.reserved = Power::from_watts(static_cast<std::int64_t>(reserved.value()));
  auto active = bool_member(value, "active", true);
  if (!active.ok()) {
    return active.status();
  }
  obligation.active = active.value();
  auto tick = uint_member(value, "tick", 0, true);
  if (!tick.ok()) {
    return tick.status();
  }
  obligation.tick = Tick::from_value(tick.value());
  auto note = string_member(value, "note", "", false);
  if (!note.ok()) {
    return note.status();
  }
  obligation.note = note.value();
  const Status valid = obligation.validate();
  if (!valid.ok()) {
    return valid;
  }
  return obligation;
}

Result<FacilitySnapshot> facility_snapshot_from_json(const JsonValue& value) {
  const Status known = reject_unknown_members(
      value, "snapshot",
      {"id", "evidence_generation", "tick", "total_demand_watts", "total_demand_state", "loads",
       "obligations"});
  if (!known.ok()) {
    return known;
  }
  FacilitySnapshot snapshot;
  auto id = uint_member(value, "id", 0, true);
  if (!id.ok()) {
    return id.status();
  }
  snapshot.id = SnapshotId::from_value(id.value());
  auto generation = uint_member(value, "evidence_generation", 0, true);
  if (!generation.ok()) {
    return generation.status();
  }
  snapshot.generation = EvidenceGeneration::from_value(generation.value());
  auto tick = uint_member(value, "tick", 0, true);
  if (!tick.ok()) {
    return tick.status();
  }
  snapshot.tick = Tick::from_value(tick.value());
  auto demand_state = string_member(value, "total_demand_state", "unknown", false);
  if (!demand_state.ok()) {
    return demand_state.status();
  }
  auto parsed_state = evidence_state_from_string(demand_state.value());
  if (!parsed_state.ok()) {
    return parsed_state.status();
  }
  snapshot.total_demand.state = parsed_state.value();
  auto demand = uint_member(value, "total_demand_watts", 0, false);
  if (!demand.ok()) {
    return demand.status();
  }
  snapshot.total_demand.value = Power::from_watts(static_cast<std::int64_t>(demand.value()));
  snapshot.total_demand.generation = snapshot.generation;
  snapshot.total_demand.tick = snapshot.tick;

  const JsonValue* loads = value.find("loads");
  if (loads != nullptr) {
    if (!loads->is_array()) {
      return Status::error(StatusCode::InvalidArgument, "loads must be an array");
    }
    if (loads->items().size() > limits::kMaxLoads) {
      return Status::error(StatusCode::Overlong, "snapshot declares more loads than the bound");
    }
    for (const JsonValue& entry : loads->items()) {
      auto load = load_record_from_json(entry);
      if (!load.ok()) {
        return load.status();
      }
      if (!load.value().contribution.generation.is_set()) {
        LoadRecord stamped = load.value();
        stamped.contribution.generation = snapshot.generation;
        snapshot.loads.push_back(stamped);
      } else {
        snapshot.loads.push_back(load.value());
      }
    }
  }
  const JsonValue* obligations = value.find("obligations");
  if (obligations != nullptr) {
    if (!obligations->is_array()) {
      return Status::error(StatusCode::InvalidArgument, "obligations must be an array");
    }
    if (obligations->items().size() > limits::kMaxObligations) {
      return Status::error(StatusCode::Overlong,
                           "snapshot declares more obligations than the bound");
    }
    for (const JsonValue& entry : obligations->items()) {
      auto obligation = obligation_from_json(entry);
      if (!obligation.ok()) {
        return obligation.status();
      }
      ProtectedObligation stamped = obligation.value();
      stamped.generation = snapshot.generation;
      snapshot.obligations.push_back(stamped);
    }
  }
  std::sort(snapshot.loads.begin(), snapshot.loads.end(),
            [](const LoadRecord& left, const LoadRecord& right) { return left.ref < right.ref; });
  for (std::size_t index = 1; index < snapshot.loads.size(); ++index) {
    if (snapshot.loads[index - 1].ref == snapshot.loads[index].ref) {
      return Status::error(StatusCode::DuplicateIdentity,
                           "snapshot contains load '" + snapshot.loads[index].ref.value() + "' twice");
    }
  }
  const Status valid = snapshot.validate();
  if (!valid.ok()) {
    return valid;
  }
  return snapshot;
}

}  // namespace load_shedding
