#include "detail/serialization.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <utility>

#include "load_shedding/text.hpp"

namespace load_shedding::detail {
namespace {

// ---------------------------------------------------------------------------
// Primitive codecs
// ---------------------------------------------------------------------------

template <class Tag>
void put_id(Writer& writer, Id<Tag> value) {
  writer.u64(value.value());
}

template <class Tag>
Result<Id<Tag>> get_id(Reader& reader) {
  auto value = reader.u64();
  if (!value.ok()) {
    return value.status();
  }
  return Id<Tag>::from_value(value.value());
}

void put_stage_index(Writer& writer, StageIndex value) {
  writer.boolean(value.is_set());
  writer.u32(value.value());
}

Result<StageIndex> get_stage_index(Reader& reader) {
  auto set = reader.boolean();
  if (!set.ok()) {
    return set.status();
  }
  auto value = reader.u32();
  if (!value.ok()) {
    return value.status();
  }
  if (value.value() > limits::kMaxStages) {
    return Status::error(StatusCode::OutOfRange,
                         "encoded stage ordinal " + std::to_string(value.value()) +
                             " is outside the stage bound");
  }
  if (!set.value()) {
    return StageIndex::unset();
  }
  return StageIndex::from_ordinal(value.value());
}

template <class Enum>
void put_enum(Writer& writer, Enum value) {
  writer.u8(static_cast<std::uint8_t>(value));
}

template <class Enum>
Result<Enum> get_enum(Reader& reader, std::uint8_t highest, const char* what) {
  auto raw = reader.u8();
  if (!raw.ok()) {
    return raw.status();
  }
  if (raw.value() > highest) {
    return Status::error(StatusCode::Unsupported,
                         std::string("encoded ") + what + " value " + std::to_string(raw.value()) +
                             " is not supported by this build");
  }
  return static_cast<Enum>(raw.value());
}

void put_power(Writer& writer, Power value) { writer.i64(value.watts()); }

Result<Power> get_power(Reader& reader) {
  auto value = reader.i64();
  if (!value.ok()) {
    return value.status();
  }
  if (value.value() > limits::kMaxPowerWatts || value.value() < -limits::kMaxPowerWatts) {
    return Status::error(StatusCode::OutOfRange,
                         "encoded power quantity " + std::to_string(value.value()) +
                             "W is outside the supported range");
  }
  return Power::from_watts(value.value());
}

void put_digest(Writer& writer, const Digest& value) {
  writer.raw(value.bytes().data(), value.bytes().size());
}

Result<Digest> get_digest(Reader& reader) {
  auto raw = reader.raw(Digest::kBytes);
  if (!raw.ok()) {
    return raw.status();
  }
  std::array<std::uint8_t, Digest::kBytes> bytes{};
  for (std::size_t index = 0; index < Digest::kBytes; ++index) {
    bytes[index] = static_cast<std::uint8_t>(static_cast<unsigned char>(raw.value()[index]));
  }
  return Digest::from_bytes(bytes);
}

Status put_text(Writer& writer, std::string_view value, std::size_t max_bytes, const char* what) {
  const Status valid = validate_text(value, max_bytes, what);
  if (!valid.ok()) {
    return valid;
  }
  return writer.text(value, max_bytes, what);
}

Result<std::string> get_text(Reader& reader, std::size_t max_bytes, const char* what) {
  auto value = reader.text(max_bytes, what);
  if (!value.ok()) {
    return value.status();
  }
  const Status valid = validate_text(value.value(), max_bytes, what);
  if (!valid.ok()) {
    return valid;
  }
  return value.value();
}

Status put_ref(Writer& writer, const LoadRef& value, std::size_t max_bytes, const char* what) {
  const Status valid = validate_identifier(value.value(), max_bytes, what);
  if (!valid.ok()) {
    return valid;
  }
  return writer.text(value.value(), max_bytes, what);
}

Result<LoadRef> get_ref(Reader& reader, std::size_t max_bytes, const char* what) {
  auto text = reader.text(max_bytes, what);
  if (!text.ok()) {
    return text.status();
  }
  return LoadRef::parse(text.value());
}

Result<ObligationRef> get_obligation_ref(Reader& reader) {
  auto text = reader.text(limits::kMaxObligationRefBytes, "obligation reference");
  if (!text.ok()) {
    return text.status();
  }
  return ObligationRef::parse(text.value());
}

Result<std::uint32_t> get_count(Reader& reader, std::size_t limit, const char* what) {
  auto count = reader.u32();
  if (!count.ok()) {
    return count.status();
  }
  if (static_cast<std::size_t>(count.value()) > limit) {
    return Status::error(StatusCode::Overlong,
                         std::string("encoded ") + what + " count " + std::to_string(count.value()) +
                             " exceeds the bound of " + std::to_string(limit));
  }
  return count.value();
}

template <class T, class Put>
Status put_list(Writer& writer, const std::vector<T>& items, std::size_t limit, const char* what,
                Put put_one) {
  if (items.size() > limit) {
    return Status::error(StatusCode::Overlong, std::string(what) + " count " +
                                                   std::to_string(items.size()) +
                                                   " exceeds the bound of " + std::to_string(limit));
  }
  writer.u32(static_cast<std::uint32_t>(items.size()));
  for (const T& item : items) {
    const Status status = put_one(writer, item);
    if (!status.ok()) {
      return status;
    }
  }
  return Status::success();
}

template <class T, class Get>
Result<std::vector<T>> get_list(Reader& reader, std::size_t limit, const char* what, Get get_one) {
  auto count = get_count(reader, limit, what);
  if (!count.ok()) {
    return count.status();
  }
  std::vector<T> items;
  items.reserve(count.value());
  for (std::uint32_t index = 0; index < count.value(); ++index) {
    auto item = get_one(reader);
    if (!item.ok()) {
      return item.status();
    }
    items.push_back(std::move(item.value()));
  }
  return items;
}

// ---------------------------------------------------------------------------
// Domain codecs
// ---------------------------------------------------------------------------

Status put_power_evidence(Writer& writer, const PowerEvidence& evidence) {
  put_enum(writer, evidence.state);
  put_power(writer, evidence.value);
  put_id(writer, evidence.generation);
  put_id(writer, evidence.tick);
  return Status::success();
}

Result<PowerEvidence> get_power_evidence(Reader& reader) {
  PowerEvidence evidence;
  auto state = get_enum<EvidenceState>(reader, 5, "evidence state");
  if (!state.ok()) {
    return state.status();
  }
  evidence.state = state.value();
  auto value = get_power(reader);
  if (!value.ok()) {
    return value.status();
  }
  evidence.value = value.value();
  auto generation = get_id<EvidenceGenerationTag>(reader);
  if (!generation.ok()) {
    return generation.status();
  }
  evidence.generation = generation.value();
  auto tick = get_id<TickTag>(reader);
  if (!tick.ok()) {
    return tick.status();
  }
  evidence.tick = tick.value();
  const Status valid = evidence.validate("load contribution evidence");
  if (!valid.ok()) {
    return valid;
  }
  return evidence;
}

Status put_load(Writer& writer, const LoadRecord& load) {
  Status status = put_ref(writer, load.ref, limits::kMaxLoadRefBytes, "load reference");
  if (!status.ok()) {
    return status;
  }
  put_id(writer, load.identity_generation);
  put_id(writer, load.revision);
  put_enum(writer, load.load_class);
  put_enum(writer, load.priority);
  status = put_power_evidence(writer, load.contribution);
  if (!status.ok()) {
    return status;
  }
  writer.boolean(load.in_service);
  return put_text(writer, load.note, limits::kMaxNoteBytes, "load note");
}

Result<LoadRecord> get_load(Reader& reader) {
  LoadRecord load;
  auto ref = get_ref(reader, limits::kMaxLoadRefBytes, "load reference");
  if (!ref.ok()) {
    return ref.status();
  }
  load.ref = ref.value();
  auto identity = get_id<LoadGenerationTag>(reader);
  if (!identity.ok()) {
    return identity.status();
  }
  load.identity_generation = identity.value();
  auto revision = get_id<StateRevisionTag>(reader);
  if (!revision.ok()) {
    return revision.status();
  }
  load.revision = revision.value();
  auto load_class = get_enum<LoadClass>(reader, 3, "load class");
  if (!load_class.ok()) {
    return load_class.status();
  }
  load.load_class = load_class.value();
  auto priority = get_enum<PriorityClass>(reader, 5, "priority class");
  if (!priority.ok()) {
    return priority.status();
  }
  load.priority = priority.value();
  auto contribution = get_power_evidence(reader);
  if (!contribution.ok()) {
    return contribution.status();
  }
  load.contribution = contribution.value();
  auto in_service = reader.boolean();
  if (!in_service.ok()) {
    return in_service.status();
  }
  load.in_service = in_service.value();
  auto note = get_text(reader, limits::kMaxNoteBytes, "load note");
  if (!note.ok()) {
    return note.status();
  }
  load.note = note.value();
  return load;
}

Status put_obligation(Writer& writer, const ProtectedObligation& obligation) {
  // The obligation reference is its own identifier type with its own bound.
  const Status reference_valid = validate_identifier(
      obligation.ref.value(), limits::kMaxObligationRefBytes, "obligation reference");
  if (!reference_valid.ok()) {
    return reference_valid;
  }
  const Status written = writer.text(obligation.ref.value(), limits::kMaxObligationRefBytes,
                                     "obligation reference");
  if (!written.ok()) {
    return written;
  }
  const Status load_written =
      put_ref(writer, obligation.load, limits::kMaxLoadRefBytes, "obligation load reference");
  if (!load_written.ok()) {
    return load_written;
  }
  put_power(writer, obligation.reserved);
  writer.boolean(obligation.active);
  put_id(writer, obligation.generation);
  put_id(writer, obligation.tick);
  put_id(writer, obligation.revision);
  return put_text(writer, obligation.note, limits::kMaxNoteBytes, "obligation note");
}

Result<ProtectedObligation> get_obligation(Reader& reader) {
  ProtectedObligation obligation;
  auto ref = get_obligation_ref(reader);
  if (!ref.ok()) {
    return ref.status();
  }
  obligation.ref = ref.value();
  auto load = get_ref(reader, limits::kMaxLoadRefBytes, "obligation load reference");
  if (!load.ok()) {
    return load.status();
  }
  obligation.load = load.value();
  auto reserved = get_power(reader);
  if (!reserved.ok()) {
    return reserved.status();
  }
  obligation.reserved = reserved.value();
  auto active = reader.boolean();
  if (!active.ok()) {
    return active.status();
  }
  obligation.active = active.value();
  auto generation = get_id<EvidenceGenerationTag>(reader);
  if (!generation.ok()) {
    return generation.status();
  }
  obligation.generation = generation.value();
  auto tick = get_id<TickTag>(reader);
  if (!tick.ok()) {
    return tick.status();
  }
  obligation.tick = tick.value();
  auto revision = get_id<StateRevisionTag>(reader);
  if (!revision.ok()) {
    return revision.status();
  }
  obligation.revision = revision.value();
  auto note = get_text(reader, limits::kMaxNoteBytes, "obligation note");
  if (!note.ok()) {
    return note.status();
  }
  obligation.note = note.value();
  return obligation;
}

Status put_snapshot(Writer& writer, const FacilitySnapshot& snapshot) {
  put_id(writer, snapshot.id);
  put_id(writer, snapshot.generation);
  put_id(writer, snapshot.revision);
  put_id(writer, snapshot.tick);
  Status status = put_power_evidence(writer, snapshot.total_demand);
  if (!status.ok()) {
    return status;
  }
  status = put_list<LoadRecord>(writer, snapshot.loads, limits::kMaxLoads, "load", put_load);
  if (!status.ok()) {
    return status;
  }
  return put_list<ProtectedObligation>(writer, snapshot.obligations, limits::kMaxObligations,
                                       "obligation", put_obligation);
}

Result<FacilitySnapshot> get_snapshot(Reader& reader) {
  FacilitySnapshot snapshot;
  auto id = get_id<SnapshotTag>(reader);
  if (!id.ok()) {
    return id.status();
  }
  snapshot.id = id.value();
  auto generation = get_id<EvidenceGenerationTag>(reader);
  if (!generation.ok()) {
    return generation.status();
  }
  snapshot.generation = generation.value();
  auto revision = get_id<StateRevisionTag>(reader);
  if (!revision.ok()) {
    return revision.status();
  }
  snapshot.revision = revision.value();
  auto tick = get_id<TickTag>(reader);
  if (!tick.ok()) {
    return tick.status();
  }
  snapshot.tick = tick.value();
  auto demand = get_power_evidence(reader);
  if (!demand.ok()) {
    return demand.status();
  }
  snapshot.total_demand = demand.value();
  auto loads = get_list<LoadRecord>(reader, limits::kMaxLoads, "load", get_load);
  if (!loads.ok()) {
    return loads.status();
  }
  snapshot.loads = std::move(loads.value());
  auto obligations = get_list<ProtectedObligation>(reader, limits::kMaxObligations, "obligation",
                                                   get_obligation);
  if (!obligations.ok()) {
    return obligations.status();
  }
  snapshot.obligations = std::move(obligations.value());
  return snapshot;
}

Status put_stage(Writer& writer, const StageDefinition& stage) {
  put_stage_index(writer, stage.index);
  Status status = put_text(writer, stage.name, limits::kMaxStageNameBytes, "stage name");
  if (!status.ok()) {
    return status;
  }
  if (stage.priorities.size() > limits::kMaxPrioritiesPerStage) {
    return Status::error(StatusCode::Overlong, "stage declares more priorities than the bound");
  }
  writer.u32(static_cast<std::uint32_t>(stage.priorities.size()));
  for (const PriorityClass priority : stage.priorities) {
    put_enum(writer, priority);
  }
  writer.boolean(stage.max_shed.has_value());
  if (stage.max_shed.has_value()) {
    put_power(writer, stage.max_shed.value());
  }
  writer.boolean(stage.max_shed_ppm.has_value());
  if (stage.max_shed_ppm.has_value()) {
    writer.u32(stage.max_shed_ppm.value());
  }
  writer.boolean(stage.max_loads.has_value());
  if (stage.max_loads.has_value()) {
    writer.u32(stage.max_loads.value());
  }
  writer.boolean(stage.requires_emergency_authority);
  return Status::success();
}

Result<StageDefinition> get_stage(Reader& reader) {
  StageDefinition stage;
  auto index = get_stage_index(reader);
  if (!index.ok()) {
    return index.status();
  }
  stage.index = index.value();
  auto name = get_text(reader, limits::kMaxStageNameBytes, "stage name");
  if (!name.ok()) {
    return name.status();
  }
  stage.name = name.value();
  auto count = get_count(reader, limits::kMaxPrioritiesPerStage, "stage priority");
  if (!count.ok()) {
    return count.status();
  }
  for (std::uint32_t position = 0; position < count.value(); ++position) {
    auto priority = get_enum<PriorityClass>(reader, 5, "priority class");
    if (!priority.ok()) {
      return priority.status();
    }
    stage.priorities.push_back(priority.value());
  }
  auto has_max_shed = reader.boolean();
  if (!has_max_shed.ok()) {
    return has_max_shed.status();
  }
  if (has_max_shed.value()) {
    auto value = get_power(reader);
    if (!value.ok()) {
      return value.status();
    }
    stage.max_shed = value.value();
  }
  auto has_ppm = reader.boolean();
  if (!has_ppm.ok()) {
    return has_ppm.status();
  }
  if (has_ppm.value()) {
    auto value = reader.u32();
    if (!value.ok()) {
      return value.status();
    }
    stage.max_shed_ppm = value.value();
  }
  auto has_loads = reader.boolean();
  if (!has_loads.ok()) {
    return has_loads.status();
  }
  if (has_loads.value()) {
    auto value = reader.u32();
    if (!value.ok()) {
      return value.status();
    }
    stage.max_loads = value.value();
  }
  auto emergency = reader.boolean();
  if (!emergency.ok()) {
    return emergency.status();
  }
  stage.requires_emergency_authority = emergency.value();
  const Status valid = stage.validate();
  if (!valid.ok()) {
    return valid;
  }
  return stage;
}

Status put_policy(Writer& writer, const SheddingPolicy& policy, bool include_generation) {
  writer.u32(policy.schema_version);
  Status status = put_text(writer, policy.name, limits::kMaxPolicyNameBytes, "policy name");
  if (!status.ok()) {
    return status;
  }
  if (include_generation) {
    put_id(writer, policy.generation);
  }
  put_enum(writer, policy.mode);
  if (policy.shed_classes.size() > 4) {
    return Status::error(StatusCode::Overlong, "policy declares more load classes than the bound");
  }
  writer.u32(static_cast<std::uint32_t>(policy.shed_classes.size()));
  for (const LoadClass load_class : policy.shed_classes) {
    put_enum(writer, load_class);
  }
  if (policy.protected_priorities.size() > 6) {
    return Status::error(StatusCode::Overlong, "policy declares more protected priorities than the bound");
  }
  writer.u32(static_cast<std::uint32_t>(policy.protected_priorities.size()));
  for (const PriorityClass priority : policy.protected_priorities) {
    put_enum(writer, priority);
  }
  status = put_list<StageDefinition>(writer, policy.stages, limits::kMaxStages, "stage", put_stage);
  if (!status.ok()) {
    return status;
  }
  writer.u64(policy.max_evidence_age_ticks);
  writer.u64(policy.max_evidence_generation_lag);
  put_enum(writer, policy.recovery_order);
  writer.boolean(policy.min_off_ticks.has_value());
  if (policy.min_off_ticks.has_value()) {
    writer.u64(policy.min_off_ticks.value());
  }
  writer.boolean(policy.min_on_ticks.has_value());
  if (policy.min_on_ticks.has_value()) {
    writer.u64(policy.min_on_ticks.value());
  }
  writer.boolean(policy.allow_emergency_override);
  return Status::success();
}

Result<SheddingPolicy> get_policy(Reader& reader, bool include_generation) {
  SheddingPolicy policy;
  // The default-constructed policy carries documented defaults; a decoded policy
  // must contain exactly what was encoded, so the collections start empty.
  policy.shed_classes.clear();
  policy.protected_priorities.clear();
  policy.stages.clear();
  auto schema = reader.u32();
  if (!schema.ok()) {
    return schema.status();
  }
  policy.schema_version = schema.value();
  auto name = get_text(reader, limits::kMaxPolicyNameBytes, "policy name");
  if (!name.ok()) {
    return name.status();
  }
  policy.name = name.value();
  if (include_generation) {
    auto generation = get_id<PolicyGenerationTag>(reader);
    if (!generation.ok()) {
      return generation.status();
    }
    policy.generation = generation.value();
  }
  auto mode = get_enum<SelectionMode>(reader, 1, "selection mode");
  if (!mode.ok()) {
    return mode.status();
  }
  policy.mode = mode.value();
  auto class_count = get_count(reader, 4, "policy load class");
  if (!class_count.ok()) {
    return class_count.status();
  }
  for (std::uint32_t position = 0; position < class_count.value(); ++position) {
    auto load_class = get_enum<LoadClass>(reader, 3, "load class");
    if (!load_class.ok()) {
      return load_class.status();
    }
    policy.shed_classes.push_back(load_class.value());
  }
  auto priority_count = get_count(reader, 6, "policy protected priority");
  if (!priority_count.ok()) {
    return priority_count.status();
  }
  for (std::uint32_t position = 0; position < priority_count.value(); ++position) {
    auto priority = get_enum<PriorityClass>(reader, 5, "priority class");
    if (!priority.ok()) {
      return priority.status();
    }
    policy.protected_priorities.push_back(priority.value());
  }
  auto stages = get_list<StageDefinition>(reader, limits::kMaxStages, "stage", get_stage);
  if (!stages.ok()) {
    return stages.status();
  }
  policy.stages = std::move(stages.value());
  auto age = reader.u64();
  if (!age.ok()) {
    return age.status();
  }
  policy.max_evidence_age_ticks = age.value();
  auto lag = reader.u64();
  if (!lag.ok()) {
    return lag.status();
  }
  policy.max_evidence_generation_lag = lag.value();
  auto order = get_enum<RecoveryOrder>(reader, 1, "recovery order");
  if (!order.ok()) {
    return order.status();
  }
  policy.recovery_order = order.value();
  auto has_min_off = reader.boolean();
  if (!has_min_off.ok()) {
    return has_min_off.status();
  }
  if (has_min_off.value()) {
    auto value = reader.u64();
    if (!value.ok()) {
      return value.status();
    }
    policy.min_off_ticks = value.value();
  }
  auto has_min_on = reader.boolean();
  if (!has_min_on.ok()) {
    return has_min_on.status();
  }
  if (has_min_on.value()) {
    auto value = reader.u64();
    if (!value.ok()) {
      return value.status();
    }
    policy.min_on_ticks = value.value();
  }
  auto emergency = reader.boolean();
  if (!emergency.ok()) {
    return emergency.status();
  }
  policy.allow_emergency_override = emergency.value();
  // The policy's own schema rules are validated by `StoreState::validate()`
  // together with the installed flag: a store that has never had a policy
  // installed legitimately carries an empty placeholder here, and refusing to
  // decode it would make such a store unopenable.
  return policy;
}

Status put_closure(Writer& writer, const AccountingClosure& closure) {
  put_power(writer, closure.requested_reduction);
  put_power(writer, closure.eligible_known_capacity);
  put_power(writer, closure.selected_expected_reduction);
  put_power(writer, closure.protected_known_amount);
  put_power(writer, closure.non_sheddable_known_amount);
  put_power(writer, closure.unavailable_known_amount);
  put_power(writer, closure.residual_deficit);
  put_power(writer, closure.overshoot);
  put_power(writer, closure.remaining_eligible_known_capacity);
  writer.u64(closure.eligible_load_count);
  writer.u64(closure.selected_load_count);
  writer.u64(closure.protected_load_count);
  writer.u64(closure.indeterminate_load_count);
  writer.u64(closure.unavailable_load_count);
  writer.u64(closure.stale_load_count);
  writer.u64(closure.denied_load_count);
  writer.u64(closure.unsafe_load_count);
  writer.u64(closure.unsupported_load_count);
  writer.u64(closure.zero_contribution_load_count);
  writer.boolean(closure.capacity_indeterminate);
  writer.boolean(closure.ceiling_verified);
  writer.boolean(closure.effective_shedding_ceiling.has_value());
  if (closure.effective_shedding_ceiling.has_value()) {
    put_power(writer, closure.effective_shedding_ceiling.value());
  }
  return Status::success();
}

Result<AccountingClosure> get_closure(Reader& reader) {
  AccountingClosure closure;
  const auto power_field = [&reader](Power& target) -> Status {
    auto value = get_power(reader);
    if (!value.ok()) {
      return value.status();
    }
    target = value.value();
    return Status::success();
  };
  Status status = power_field(closure.requested_reduction);
  if (status.ok()) status = power_field(closure.eligible_known_capacity);
  if (status.ok()) status = power_field(closure.selected_expected_reduction);
  if (status.ok()) status = power_field(closure.protected_known_amount);
  if (status.ok()) status = power_field(closure.non_sheddable_known_amount);
  if (status.ok()) status = power_field(closure.unavailable_known_amount);
  if (status.ok()) status = power_field(closure.residual_deficit);
  if (status.ok()) status = power_field(closure.overshoot);
  if (status.ok()) status = power_field(closure.remaining_eligible_known_capacity);
  if (!status.ok()) {
    return status;
  }
  const auto count_field = [&reader](std::uint64_t& target) -> Status {
    auto value = reader.u64();
    if (!value.ok()) {
      return value.status();
    }
    if (value.value() > limits::kMaxLoads) {
      return Status::error(StatusCode::Overlong, "encoded closure count exceeds the load bound");
    }
    target = value.value();
    return Status::success();
  };
  if (status.ok()) status = count_field(closure.eligible_load_count);
  if (status.ok()) status = count_field(closure.selected_load_count);
  if (status.ok()) status = count_field(closure.protected_load_count);
  if (status.ok()) status = count_field(closure.indeterminate_load_count);
  if (status.ok()) status = count_field(closure.unavailable_load_count);
  if (status.ok()) status = count_field(closure.stale_load_count);
  if (status.ok()) status = count_field(closure.denied_load_count);
  if (status.ok()) status = count_field(closure.unsafe_load_count);
  if (status.ok()) status = count_field(closure.unsupported_load_count);
  if (status.ok()) status = count_field(closure.zero_contribution_load_count);
  if (!status.ok()) {
    return status;
  }
  auto indeterminate = reader.boolean();
  if (!indeterminate.ok()) return indeterminate.status();
  closure.capacity_indeterminate = indeterminate.value();
  auto verified = reader.boolean();
  if (!verified.ok()) return verified.status();
  closure.ceiling_verified = verified.value();
  auto has_ceiling = reader.boolean();
  if (!has_ceiling.ok()) return has_ceiling.status();
  if (has_ceiling.value()) {
    auto ceiling = get_power(reader);
    if (!ceiling.ok()) return ceiling.status();
    closure.effective_shedding_ceiling = ceiling.value();
  }
  return closure;
}

Status put_plan_identity(Writer& writer, const PlanIdentity& identity, bool include_instance) {
  if (include_instance) {
    put_id(writer, identity.plan_id);
    put_id(writer, identity.generation);
  }
  put_id(writer, identity.request_id);
  put_id(writer, identity.policy_generation);
  put_digest(writer, identity.policy_digest);
  put_id(writer, identity.base_revision);
  put_id(writer, identity.evidence_generation);
  put_id(writer, identity.effect_generation);
  if (identity.evidence_generations.size() > limits::kMaxEvidenceGenerations) {
    return Status::error(StatusCode::Overlong, "plan records more evidence generations than the bound");
  }
  writer.u32(static_cast<std::uint32_t>(identity.evidence_generations.size()));
  for (const EvidenceGeneration generation : identity.evidence_generations) {
    put_id(writer, generation);
  }
  put_id(writer, identity.authority_epoch);
  put_id(writer, identity.incarnation);
  put_id(writer, identity.tick);
  if (include_instance) {
    put_id(writer, identity.attempt);
  }
  return Status::success();
}

Result<PlanIdentity> get_plan_identity(Reader& reader, bool include_instance) {
  PlanIdentity identity;
  if (include_instance) {
    auto plan_id = get_id<PlanTag>(reader);
    if (!plan_id.ok()) return plan_id.status();
    identity.plan_id = plan_id.value();
    auto generation = get_id<PlanGenerationTag>(reader);
    if (!generation.ok()) return generation.status();
    identity.generation = generation.value();
  }
  auto request = get_id<RequestTag>(reader);
  if (!request.ok()) return request.status();
  identity.request_id = request.value();
  auto policy = get_id<PolicyGenerationTag>(reader);
  if (!policy.ok()) return policy.status();
  identity.policy_generation = policy.value();
  auto policy_digest = get_digest(reader);
  if (!policy_digest.ok()) return policy_digest.status();
  identity.policy_digest = policy_digest.value();
  auto revision = get_id<StateRevisionTag>(reader);
  if (!revision.ok()) return revision.status();
  identity.base_revision = revision.value();
  auto evidence = get_id<EvidenceGenerationTag>(reader);
  if (!evidence.ok()) return evidence.status();
  identity.evidence_generation = evidence.value();
  auto effect = get_id<EffectGenerationTag>(reader);
  if (!effect.ok()) return effect.status();
  identity.effect_generation = effect.value();
  auto generations = get_count(reader, limits::kMaxEvidenceGenerations, "evidence generation");
  if (!generations.ok()) return generations.status();
  for (std::uint32_t index = 0; index < generations.value(); ++index) {
    auto value = get_id<EvidenceGenerationTag>(reader);
    if (!value.ok()) return value.status();
    identity.evidence_generations.push_back(value.value());
  }
  auto epoch = get_id<AuthorityEpochTag>(reader);
  if (!epoch.ok()) return epoch.status();
  identity.authority_epoch = epoch.value();
  auto incarnation = get_id<IncarnationTag>(reader);
  if (!incarnation.ok()) return incarnation.status();
  identity.incarnation = incarnation.value();
  auto tick = get_id<TickTag>(reader);
  if (!tick.ok()) return tick.status();
  identity.tick = tick.value();
  if (include_instance) {
    auto attempt = get_id<AttemptTag>(reader);
    if (!attempt.ok()) return attempt.status();
    identity.attempt = attempt.value();
  }
  return identity;
}

Status put_action(Writer& writer, const ShedAction& action) {
  Status status = put_ref(writer, action.load, limits::kMaxLoadRefBytes, "action load reference");
  if (!status.ok()) return status;
  put_stage_index(writer, action.stage);
  writer.u32(action.selection_index);
  put_power(writer, action.expected_contribution);
  put_enum(writer, action.reason);
  put_enum(writer, action.priority);
  put_enum(writer, action.load_class);
  put_id(writer, action.identity_generation);
  put_id(writer, action.load_revision);
  put_digest(writer, action.evidence_digest);
  return Status::success();
}

Result<ShedAction> get_action(Reader& reader) {
  ShedAction action;
  auto load = get_ref(reader, limits::kMaxLoadRefBytes, "action load reference");
  if (!load.ok()) return load.status();
  action.load = load.value();
  auto stage = get_stage_index(reader);
  if (!stage.ok()) return stage.status();
  action.stage = stage.value();
  auto index = reader.u32();
  if (!index.ok()) return index.status();
  action.selection_index = index.value();
  auto contribution = get_power(reader);
  if (!contribution.ok()) return contribution.status();
  action.expected_contribution = contribution.value();
  auto reason = get_enum<ReasonCode>(reader, 1, "reason code");
  if (!reason.ok()) return reason.status();
  action.reason = reason.value();
  auto priority = get_enum<PriorityClass>(reader, 5, "priority class");
  if (!priority.ok()) return priority.status();
  action.priority = priority.value();
  auto load_class = get_enum<LoadClass>(reader, 3, "load class");
  if (!load_class.ok()) return load_class.status();
  action.load_class = load_class.value();
  auto identity = get_id<LoadGenerationTag>(reader);
  if (!identity.ok()) return identity.status();
  action.identity_generation = identity.value();
  auto revision = get_id<StateRevisionTag>(reader);
  if (!revision.ok()) return revision.status();
  action.load_revision = revision.value();
  auto digest = get_digest(reader);
  if (!digest.ok()) return digest.status();
  action.evidence_digest = digest.value();
  return action;
}

Status put_consideration(Writer& writer, const LoadConsideration& consideration) {
  Status status = put_ref(writer, consideration.load, limits::kMaxLoadRefBytes, "consideration load reference");
  if (!status.ok()) return status;
  put_stage_index(writer, consideration.stage);
  put_enum(writer, consideration.priority);
  put_enum(writer, consideration.load_class);
  put_enum(writer, consideration.evidence_state);
  put_enum(writer, consideration.verdict);
  put_power(writer, consideration.contribution);
  put_id(writer, consideration.identity_generation);
  put_id(writer, consideration.load_revision);
  put_id(writer, consideration.evidence_generation);
  put_id(writer, consideration.evidence_tick);
  writer.u32(consideration.order_index);
  writer.boolean(consideration.order_index_set);
  return Status::success();
}

Result<LoadConsideration> get_consideration(Reader& reader) {
  LoadConsideration consideration;
  auto load = get_ref(reader, limits::kMaxLoadRefBytes, "consideration load reference");
  if (!load.ok()) return load.status();
  consideration.load = load.value();
  auto stage = get_stage_index(reader);
  if (!stage.ok()) return stage.status();
  consideration.stage = stage.value();
  auto priority = get_enum<PriorityClass>(reader, 5, "priority class");
  if (!priority.ok()) return priority.status();
  consideration.priority = priority.value();
  auto load_class = get_enum<LoadClass>(reader, 3, "load class");
  if (!load_class.ok()) return load_class.status();
  consideration.load_class = load_class.value();
  auto evidence_state = get_enum<EvidenceState>(reader, 5, "evidence state");
  if (!evidence_state.ok()) return evidence_state.status();
  consideration.evidence_state = evidence_state.value();
  auto verdict = get_enum<LoadVerdict>(reader, 22, "load verdict");
  if (!verdict.ok()) return verdict.status();
  consideration.verdict = verdict.value();
  auto contribution = get_power(reader);
  if (!contribution.ok()) return contribution.status();
  consideration.contribution = contribution.value();
  auto identity = get_id<LoadGenerationTag>(reader);
  if (!identity.ok()) return identity.status();
  consideration.identity_generation = identity.value();
  auto revision = get_id<StateRevisionTag>(reader);
  if (!revision.ok()) return revision.status();
  consideration.load_revision = revision.value();
  auto evidence = get_id<EvidenceGenerationTag>(reader);
  if (!evidence.ok()) return evidence.status();
  consideration.evidence_generation = evidence.value();
  auto tick = get_id<TickTag>(reader);
  if (!tick.ok()) return tick.status();
  consideration.evidence_tick = tick.value();
  auto order = reader.u32();
  if (!order.ok()) return order.status();
  consideration.order_index = order.value();
  auto order_set = reader.boolean();
  if (!order_set.ok()) return order_set.status();
  consideration.order_index_set = order_set.value();
  return consideration;
}

Status put_stage_summary(Writer& writer, const StageSummary& summary) {
  put_stage_index(writer, summary.index);
  Status status = put_text(writer, summary.name, limits::kMaxStageNameBytes, "stage name");
  if (!status.ok()) return status;
  writer.u32(summary.considered);
  writer.u32(summary.admissible);
  writer.u32(summary.selected);
  put_power(writer, summary.selected_power);
  writer.boolean(summary.effective_power_cap.has_value());
  if (summary.effective_power_cap.has_value()) {
    put_power(writer, summary.effective_power_cap.value());
  }
  writer.boolean(summary.load_cap.has_value());
  if (summary.load_cap.has_value()) {
    writer.u32(summary.load_cap.value());
  }
  put_enum(writer, summary.stop_reason);
  writer.boolean(summary.emergency_authority_required);
  return Status::success();
}

Result<StageSummary> get_stage_summary(Reader& reader) {
  StageSummary summary;
  auto index = get_stage_index(reader);
  if (!index.ok()) return index.status();
  summary.index = index.value();
  auto name = get_text(reader, limits::kMaxStageNameBytes, "stage name");
  if (!name.ok()) return name.status();
  summary.name = name.value();
  auto considered = reader.u32();
  if (!considered.ok()) return considered.status();
  summary.considered = considered.value();
  auto admissible = reader.u32();
  if (!admissible.ok()) return admissible.status();
  summary.admissible = admissible.value();
  auto selected = reader.u32();
  if (!selected.ok()) return selected.status();
  summary.selected = selected.value();
  auto power = get_power(reader);
  if (!power.ok()) return power.status();
  summary.selected_power = power.value();
  auto has_cap = reader.boolean();
  if (!has_cap.ok()) return has_cap.status();
  if (has_cap.value()) {
    auto cap = get_power(reader);
    if (!cap.ok()) return cap.status();
    summary.effective_power_cap = cap.value();
  }
  auto has_load_cap = reader.boolean();
  if (!has_load_cap.ok()) return has_load_cap.status();
  if (has_load_cap.value()) {
    auto cap = reader.u32();
    if (!cap.ok()) return cap.status();
    summary.load_cap = cap.value();
  }
  auto stop = get_enum<StageStopReason>(reader, 5, "stage stop reason");
  if (!stop.ok()) return stop.status();
  summary.stop_reason = stop.value();
  auto emergency = reader.boolean();
  if (!emergency.ok()) return emergency.status();
  summary.emergency_authority_required = emergency.value();
  return summary;
}

Status put_plan(Writer& writer, const SheddingPlan& plan, bool include_instance) {
  Status status = put_plan_identity(writer, plan.identity, include_instance);
  if (!status.ok()) return status;
  put_enum(writer, plan.mode);
  put_enum(writer, plan.coverage);
  status = put_closure(writer, plan.closure);
  if (!status.ok()) return status;
  writer.boolean(plan.emergency_authority_used);
  status = put_text(writer, plan.emergency_justification, limits::kMaxJustificationBytes,
                    "emergency justification");
  if (!status.ok()) return status;
  status = put_list<ShedAction>(writer, plan.actions, limits::kMaxSelectedActions, "action", put_action);
  if (!status.ok()) return status;
  status = put_list<LoadConsideration>(writer, plan.considerations, limits::kMaxConsiderations,
                                       "consideration", put_consideration);
  if (!status.ok()) return status;
  return put_list<StageSummary>(writer, plan.stages, limits::kMaxStages, "stage summary",
                                put_stage_summary);
}

Result<SheddingPlan> get_plan(Reader& reader, bool include_instance) {
  SheddingPlan plan;
  auto identity = get_plan_identity(reader, include_instance);
  if (!identity.ok()) return identity.status();
  plan.identity = identity.value();
  auto mode = get_enum<SelectionMode>(reader, 1, "selection mode");
  if (!mode.ok()) return mode.status();
  plan.mode = mode.value();
  auto coverage = get_enum<CoverageStatus>(reader, 3, "coverage status");
  if (!coverage.ok()) return coverage.status();
  plan.coverage = coverage.value();
  auto closure = get_closure(reader);
  if (!closure.ok()) return closure.status();
  plan.closure = closure.value();
  auto emergency = reader.boolean();
  if (!emergency.ok()) return emergency.status();
  plan.emergency_authority_used = emergency.value();
  auto justification = get_text(reader, limits::kMaxJustificationBytes, "emergency justification");
  if (!justification.ok()) return justification.status();
  plan.emergency_justification = justification.value();
  auto actions = get_list<ShedAction>(reader, limits::kMaxSelectedActions, "action", get_action);
  if (!actions.ok()) return actions.status();
  plan.actions = std::move(actions.value());
  auto considerations = get_list<LoadConsideration>(reader, limits::kMaxConsiderations, "consideration",
                                                    get_consideration);
  if (!considerations.ok()) return considerations.status();
  plan.considerations = std::move(considerations.value());
  auto stages = get_list<StageSummary>(reader, limits::kMaxStages, "stage summary", get_stage_summary);
  if (!stages.ok()) return stages.status();
  plan.stages = std::move(stages.value());
  return plan;
}

Status put_observed(Writer& writer, const ObservedLoadState& observed) {
  Status status = put_ref(writer, observed.load, limits::kMaxLoadRefBytes, "observed load reference");
  if (!status.ok()) return status;
  put_enum(writer, observed.state);
  put_enum(writer, observed.verification);
  put_id(writer, observed.since);
  put_id(writer, observed.generation);
  put_id(writer, observed.sequence);
  put_id(writer, observed.source_plan);
  put_id(writer, observed.source_plan_generation);
  put_id(writer, observed.attempt);
  return Status::success();
}

Result<ObservedLoadState> get_observed(Reader& reader) {
  ObservedLoadState observed;
  auto load = get_ref(reader, limits::kMaxLoadRefBytes, "observed load reference");
  if (!load.ok()) return load.status();
  observed.load = load.value();
  auto state = get_enum<EffectState>(reader, 2, "effect state");
  if (!state.ok()) return state.status();
  observed.state = state.value();
  auto verification = get_enum<EffectVerification>(reader, 2, "effect verification");
  if (!verification.ok()) return verification.status();
  observed.verification = verification.value();
  auto since = get_id<TickTag>(reader);
  if (!since.ok()) return since.status();
  observed.since = since.value();
  auto generation = get_id<EffectGenerationTag>(reader);
  if (!generation.ok()) return generation.status();
  observed.generation = generation.value();
  auto sequence = get_id<SequenceTag>(reader);
  if (!sequence.ok()) return sequence.status();
  observed.sequence = sequence.value();
  auto plan = get_id<PlanTag>(reader);
  if (!plan.ok()) return plan.status();
  observed.source_plan = plan.value();
  auto plan_generation = get_id<PlanGenerationTag>(reader);
  if (!plan_generation.ok()) return plan_generation.status();
  observed.source_plan_generation = plan_generation.value();
  auto attempt = get_id<AttemptTag>(reader);
  if (!attempt.ok()) return attempt.status();
  observed.attempt = attempt.value();
  return observed;
}

Status put_effect(Writer& writer, const EffectObservation& observation) {
  put_id(writer, observation.sequence);
  put_id(writer, observation.request_id);
  put_id(writer, observation.plan_id);
  put_id(writer, observation.plan_generation);
  Status status = put_ref(writer, observation.load, limits::kMaxLoadRefBytes, "effect load reference");
  if (!status.ok()) return status;
  put_id(writer, observation.attempt);
  put_enum(writer, observation.observed);
  put_enum(writer, observation.source);
  put_enum(writer, observation.verification);
  put_id(writer, observation.tick);
  put_id(writer, observation.generation);
  put_id(writer, observation.base_revision);
  put_id(writer, observation.authority_epoch);
  put_id(writer, observation.incarnation);
  put_digest(writer, observation.plan_digest_at_observation);
  return Status::success();
}

Result<EffectObservation> get_effect(Reader& reader) {
  EffectObservation observation;
  auto sequence = get_id<SequenceTag>(reader);
  if (!sequence.ok()) return sequence.status();
  observation.sequence = sequence.value();
  auto request = get_id<RequestTag>(reader);
  if (!request.ok()) return request.status();
  observation.request_id = request.value();
  auto plan = get_id<PlanTag>(reader);
  if (!plan.ok()) return plan.status();
  observation.plan_id = plan.value();
  auto plan_generation = get_id<PlanGenerationTag>(reader);
  if (!plan_generation.ok()) return plan_generation.status();
  observation.plan_generation = plan_generation.value();
  auto load = get_ref(reader, limits::kMaxLoadRefBytes, "effect load reference");
  if (!load.ok()) return load.status();
  observation.load = load.value();
  auto attempt = get_id<AttemptTag>(reader);
  if (!attempt.ok()) return attempt.status();
  observation.attempt = attempt.value();
  auto observed = get_enum<EffectState>(reader, 2, "effect state");
  if (!observed.ok()) return observed.status();
  observation.observed = observed.value();
  auto source = get_enum<EffectSource>(reader, 2, "effect source");
  if (!source.ok()) return source.status();
  observation.source = source.value();
  auto verification = get_enum<EffectVerification>(reader, 2, "effect verification");
  if (!verification.ok()) return verification.status();
  observation.verification = verification.value();
  auto tick = get_id<TickTag>(reader);
  if (!tick.ok()) return tick.status();
  observation.tick = tick.value();
  auto generation = get_id<EffectGenerationTag>(reader);
  if (!generation.ok()) return generation.status();
  observation.generation = generation.value();
  auto revision = get_id<StateRevisionTag>(reader);
  if (!revision.ok()) return revision.status();
  observation.base_revision = revision.value();
  auto epoch = get_id<AuthorityEpochTag>(reader);
  if (!epoch.ok()) return epoch.status();
  observation.authority_epoch = epoch.value();
  auto incarnation = get_id<IncarnationTag>(reader);
  if (!incarnation.ok()) return incarnation.status();
  observation.incarnation = incarnation.value();
  auto digest = get_digest(reader);
  if (!digest.ok()) return digest.status();
  observation.plan_digest_at_observation = digest.value();
  return observation;
}

Status put_recovery_candidate(Writer& writer, const RecoveryCandidate& candidate) {
  Status status = put_ref(writer, candidate.load, limits::kMaxLoadRefBytes, "recovery load reference");
  if (!status.ok()) return status;
  put_stage_index(writer, candidate.stage);
  put_enum(writer, candidate.priority);
  put_enum(writer, candidate.load_class);
  put_power(writer, candidate.contribution);
  put_id(writer, candidate.shed_tick);
  put_enum(writer, candidate.observed);
  put_enum(writer, candidate.verification);
  put_id(writer, candidate.observed_generation);
  put_enum(writer, candidate.verdict);
  writer.u32(candidate.order_index);
  writer.boolean(candidate.order_index_set);
  writer.boolean(candidate.shed_by_plan);
  return Status::success();
}

Result<RecoveryCandidate> get_recovery_candidate(Reader& reader) {
  RecoveryCandidate candidate;
  auto load = get_ref(reader, limits::kMaxLoadRefBytes, "recovery load reference");
  if (!load.ok()) return load.status();
  candidate.load = load.value();
  auto stage = get_stage_index(reader);
  if (!stage.ok()) return stage.status();
  candidate.stage = stage.value();
  auto priority = get_enum<PriorityClass>(reader, 5, "priority class");
  if (!priority.ok()) return priority.status();
  candidate.priority = priority.value();
  auto load_class = get_enum<LoadClass>(reader, 3, "load class");
  if (!load_class.ok()) return load_class.status();
  candidate.load_class = load_class.value();
  auto contribution = get_power(reader);
  if (!contribution.ok()) return contribution.status();
  candidate.contribution = contribution.value();
  auto shed_tick = get_id<TickTag>(reader);
  if (!shed_tick.ok()) return shed_tick.status();
  candidate.shed_tick = shed_tick.value();
  auto observed = get_enum<EffectState>(reader, 2, "effect state");
  if (!observed.ok()) return observed.status();
  candidate.observed = observed.value();
  auto verification = get_enum<EffectVerification>(reader, 2, "effect verification");
  if (!verification.ok()) return verification.status();
  candidate.verification = verification.value();
  auto generation = get_id<EffectGenerationTag>(reader);
  if (!generation.ok()) return generation.status();
  candidate.observed_generation = generation.value();
  auto verdict = get_enum<RecoveryVerdict>(reader, 6, "recovery verdict");
  if (!verdict.ok()) return verdict.status();
  candidate.verdict = verdict.value();
  auto order = reader.u32();
  if (!order.ok()) return order.status();
  candidate.order_index = order.value();
  auto order_set = reader.boolean();
  if (!order_set.ok()) return order_set.status();
  candidate.order_index_set = order_set.value();
  auto shed_by_plan = reader.boolean();
  if (!shed_by_plan.ok()) return shed_by_plan.status();
  candidate.shed_by_plan = shed_by_plan.value();
  return candidate;
}

Status put_recovery(Writer& writer, const RecoveryDecision& decision) {
  put_id(writer, decision.request_id);
  put_id(writer, decision.source_plan);
  put_id(writer, decision.source_plan_generation);
  put_digest(writer, decision.source_plan_digest);
  put_id(writer, decision.policy_generation);
  put_id(writer, decision.evidence_generation);
  put_id(writer, decision.effect_generation);
  put_id(writer, decision.base_revision);
  put_id(writer, decision.authority_epoch);
  put_id(writer, decision.incarnation);
  put_id(writer, decision.tick);
  put_enum(writer, decision.order);
  put_power(writer, decision.requested_headroom);
  put_power(writer, decision.restored_expected);
  put_power(writer, decision.remaining_headroom);
  writer.u64(decision.restored_count);
  writer.u64(decision.candidate_count);
  writer.u64(decision.awaiting_confirmation_count);
  return put_list<RecoveryCandidate>(writer, decision.candidates, limits::kMaxRecoveryCandidates,
                                     "recovery candidate", put_recovery_candidate);
}

Result<RecoveryDecision> get_recovery(Reader& reader) {
  RecoveryDecision decision;
  auto request = get_id<RequestTag>(reader);
  if (!request.ok()) return request.status();
  decision.request_id = request.value();
  auto plan = get_id<PlanTag>(reader);
  if (!plan.ok()) return plan.status();
  decision.source_plan = plan.value();
  auto plan_generation = get_id<PlanGenerationTag>(reader);
  if (!plan_generation.ok()) return plan_generation.status();
  decision.source_plan_generation = plan_generation.value();
  auto plan_digest = get_digest(reader);
  if (!plan_digest.ok()) return plan_digest.status();
  decision.source_plan_digest = plan_digest.value();
  auto policy = get_id<PolicyGenerationTag>(reader);
  if (!policy.ok()) return policy.status();
  decision.policy_generation = policy.value();
  auto evidence = get_id<EvidenceGenerationTag>(reader);
  if (!evidence.ok()) return evidence.status();
  decision.evidence_generation = evidence.value();
  auto effect = get_id<EffectGenerationTag>(reader);
  if (!effect.ok()) return effect.status();
  decision.effect_generation = effect.value();
  auto revision = get_id<StateRevisionTag>(reader);
  if (!revision.ok()) return revision.status();
  decision.base_revision = revision.value();
  auto epoch = get_id<AuthorityEpochTag>(reader);
  if (!epoch.ok()) return epoch.status();
  decision.authority_epoch = epoch.value();
  auto incarnation = get_id<IncarnationTag>(reader);
  if (!incarnation.ok()) return incarnation.status();
  decision.incarnation = incarnation.value();
  auto tick = get_id<TickTag>(reader);
  if (!tick.ok()) return tick.status();
  decision.tick = tick.value();
  auto order = get_enum<RecoveryOrder>(reader, 1, "recovery order");
  if (!order.ok()) return order.status();
  decision.order = order.value();
  auto headroom = get_power(reader);
  if (!headroom.ok()) return headroom.status();
  decision.requested_headroom = headroom.value();
  auto restored = get_power(reader);
  if (!restored.ok()) return restored.status();
  decision.restored_expected = restored.value();
  auto remaining = get_power(reader);
  if (!remaining.ok()) return remaining.status();
  decision.remaining_headroom = remaining.value();
  auto restored_count = reader.u64();
  if (!restored_count.ok()) return restored_count.status();
  decision.restored_count = restored_count.value();
  auto candidate_count = reader.u64();
  if (!candidate_count.ok()) return candidate_count.status();
  decision.candidate_count = candidate_count.value();
  auto awaiting = reader.u64();
  if (!awaiting.ok()) return awaiting.status();
  decision.awaiting_confirmation_count = awaiting.value();
  auto candidates = get_list<RecoveryCandidate>(reader, limits::kMaxRecoveryCandidates,
                                                "recovery candidate", get_recovery_candidate);
  if (!candidates.ok()) return candidates.status();
  decision.candidates = std::move(candidates.value());
  return decision;
}

Status put_audit(Writer& writer, const AuditEntry& entry) {
  put_id(writer, entry.sequence);
  put_enum(writer, entry.kind);
  put_id(writer, entry.tick);
  put_id(writer, entry.authority_epoch);
  put_id(writer, entry.incarnation);
  put_id(writer, entry.request);
  put_id(writer, entry.plan);
  put_digest(writer, entry.detail_digest);
  return put_text(writer, entry.detail, limits::kMaxNoteBytes, "audit detail");
}

Result<AuditEntry> get_audit(Reader& reader) {
  AuditEntry entry;
  auto sequence = get_id<SequenceTag>(reader);
  if (!sequence.ok()) return sequence.status();
  entry.sequence = sequence.value();
  auto kind = get_enum<AuditKind>(reader, 14, "audit kind");
  if (!kind.ok()) return kind.status();
  entry.kind = kind.value();
  auto tick = get_id<TickTag>(reader);
  if (!tick.ok()) return tick.status();
  entry.tick = tick.value();
  auto epoch = get_id<AuthorityEpochTag>(reader);
  if (!epoch.ok()) return epoch.status();
  entry.authority_epoch = epoch.value();
  auto incarnation = get_id<IncarnationTag>(reader);
  if (!incarnation.ok()) return incarnation.status();
  entry.incarnation = incarnation.value();
  auto request = get_id<RequestTag>(reader);
  if (!request.ok()) return request.status();
  entry.request = request.value();
  auto plan = get_id<PlanTag>(reader);
  if (!plan.ok()) return plan.status();
  entry.plan = plan.value();
  auto digest = get_digest(reader);
  if (!digest.ok()) return digest.status();
  entry.detail_digest = digest.value();
  auto detail = get_text(reader, limits::kMaxNoteBytes, "audit detail");
  if (!detail.ok()) return detail.status();
  entry.detail = detail.value();
  return entry;
}

Status put_idempotency(Writer& writer, const IdempotencyRecord& record) {
  put_id(writer, record.request);
  put_digest(writer, record.request_digest);
  put_digest(writer, record.result_digest);
  put_id(writer, record.plan);
  put_id(writer, record.plan_generation);
  writer.boolean(record.has_plan);
  put_id(writer, record.recovery_request);
  writer.boolean(record.has_recovery);
  put_id(writer, record.tick);
  return Status::success();
}

Result<IdempotencyRecord> get_idempotency(Reader& reader) {
  IdempotencyRecord record;
  auto request = get_id<RequestTag>(reader);
  if (!request.ok()) return request.status();
  record.request = request.value();
  auto request_digest = get_digest(reader);
  if (!request_digest.ok()) return request_digest.status();
  record.request_digest = request_digest.value();
  auto result_digest = get_digest(reader);
  if (!result_digest.ok()) return result_digest.status();
  record.result_digest = result_digest.value();
  auto plan = get_id<PlanTag>(reader);
  if (!plan.ok()) return plan.status();
  record.plan = plan.value();
  auto plan_generation = get_id<PlanGenerationTag>(reader);
  if (!plan_generation.ok()) return plan_generation.status();
  record.plan_generation = plan_generation.value();
  auto has_plan = reader.boolean();
  if (!has_plan.ok()) return has_plan.status();
  record.has_plan = has_plan.value();
  auto recovery_request = get_id<RequestTag>(reader);
  if (!recovery_request.ok()) return recovery_request.status();
  record.recovery_request = recovery_request.value();
  auto has_recovery = reader.boolean();
  if (!has_recovery.ok()) return has_recovery.status();
  record.has_recovery = has_recovery.value();
  auto tick = get_id<TickTag>(reader);
  if (!tick.ok()) return tick.status();
  record.tick = tick.value();
  return record;
}

Digest digest_with_domain(std::string_view domain, const std::string& payload) {
  Sha256 hasher;
  hasher.update(domain);
  const std::uint8_t separator = 0;
  hasher.update(&separator, 1);
  hasher.update(payload);
  return hasher.finish();
}

}  // namespace

Digest plan_content_digest(const SheddingPlan& plan) {
  return digest_with_domain("load-shedding/plan-content/1", encode_plan(plan, false));
}

std::string encode_plan(const SheddingPlan& plan, bool include_instance_identity) {
  Writer writer;
  const Status status = put_plan(writer, plan, include_instance_identity);
  if (!status.ok()) {
    return std::string();
  }
  return writer.take();
}

Digest policy_content_digest(const SheddingPolicy& policy) {
  Writer writer;
  const Status status = put_policy(writer, policy, false);
  if (!status.ok()) {
    return Digest();
  }
  return digest_with_domain("load-shedding/policy-content/1", writer.bytes_ref());
}

Digest recovery_content_digest(const RecoveryDecision& decision) {
  Writer writer;
  const Status status = put_recovery(writer, decision);
  if (!status.ok()) {
    return Digest();
  }
  return digest_with_domain("load-shedding/recovery-content/1", writer.bytes_ref());
}

Digest effect_record_digest(const EffectObservation& observation) {
  Writer writer;
  const Status status = put_effect(writer, observation);
  if (!status.ok()) {
    return Digest();
  }
  return digest_with_domain("load-shedding/effect-record/1", writer.bytes_ref());
}

Digest load_evidence_digest(const LoadRecord& load) {
  Writer writer;
  const Status status = put_ref(writer, load.ref, limits::kMaxLoadRefBytes, "load reference");
  if (!status.ok()) {
    return Digest();
  }
  put_id(writer, load.identity_generation);
  put_id(writer, load.revision);
  put_enum(writer, load.load_class);
  put_enum(writer, load.priority);
  put_enum(writer, load.contribution.state);
  put_power(writer, load.contribution.value);
  put_id(writer, load.contribution.generation);
  put_id(writer, load.contribution.tick);
  writer.boolean(load.in_service);
  return digest_with_domain("load-shedding/load-evidence/1", writer.bytes_ref());
}

Digest plan_request_digest(const PlanRequest& request) {
  Writer writer;
  put_id(writer, request.request_id);
  put_power(writer, request.requested_reduction);
  put_id(writer, request.policy_generation);
  put_id(writer, request.evidence_generation);
  put_id(writer, request.effect_generation);
  put_id(writer, request.base_revision);
  put_id(writer, request.authority_epoch);
  put_id(writer, request.incarnation);
  put_id(writer, request.tick);
  writer.boolean(request.emergency_authority);
  const Status status = put_text(writer, request.emergency_justification,
                                 limits::kMaxJustificationBytes, "emergency justification");
  if (!status.ok()) {
    return Digest();
  }
  return digest_with_domain("load-shedding/plan-request/1", writer.bytes_ref());
}

Digest recovery_request_digest(const RecoveryRequest& request) {
  Writer writer;
  put_id(writer, request.request_id);
  put_id(writer, request.plan_id);
  put_id(writer, request.plan_generation);
  put_power(writer, request.available_headroom);
  put_id(writer, request.policy_generation);
  put_id(writer, request.evidence_generation);
  put_id(writer, request.effect_generation);
  put_id(writer, request.base_revision);
  put_id(writer, request.authority_epoch);
  put_id(writer, request.incarnation);
  put_id(writer, request.tick);
  return digest_with_domain("load-shedding/recovery-request/1", writer.bytes_ref());
}

Digest effect_request_digest(const EffectObservationRequest& request) {
  Writer writer;
  put_id(writer, request.request_id);
  put_id(writer, request.plan_id);
  put_id(writer, request.plan_generation);
  const Status status = put_ref(writer, request.load, limits::kMaxLoadRefBytes, "effect load reference");
  if (!status.ok()) {
    return Digest();
  }
  put_id(writer, request.attempt);
  put_enum(writer, request.observed);
  put_enum(writer, request.source);
  put_enum(writer, request.verification);
  put_id(writer, request.tick);
  put_id(writer, request.base_revision);
  put_id(writer, request.authority_epoch);
  put_id(writer, request.incarnation);
  return digest_with_domain("load-shedding/effect-request/1", writer.bytes_ref());
}

Result<std::string> encode_state(const StoreState& state) {
  const Status valid = state.validate();
  if (!valid.ok()) {
    return valid;
  }
  Writer writer;
  writer.u32(state.format_version);
  put_id(writer, state.revision);
  put_id(writer, state.policy_generation);
  put_id(writer, state.evidence_generation);
  put_id(writer, state.effect_generation);
  put_id(writer, state.plan_generation);
  put_id(writer, state.tick);
  put_id(writer, state.authority_epoch);
  put_id(writer, state.incarnation);
  writer.u64(state.next_sequence);
  writer.boolean(state.policy_installed);
  Status status = put_policy(writer, state.policy, true);
  if (!status.ok()) return status;
  status = put_snapshot(writer, state.snapshot);
  if (!status.ok()) return status;
  status = put_list<ObservedLoadState>(writer, state.observed, limits::kMaxLoads, "observed load",
                                       put_observed);
  if (!status.ok()) return status;
  status = put_list<SheddingPlan>(writer, state.plans, limits::kMaxPlansRetained, "plan",
                                  [](Writer& target, const SheddingPlan& plan) {
                                    return put_plan(target, plan, true);
                                  });
  if (!status.ok()) return status;
  status = put_list<RecoveryDecision>(writer, state.recoveries,
                                      limits::kMaxRecoveryDecisionsRetained, "recovery decision",
                                      put_recovery);
  if (!status.ok()) return status;
  status = put_list<EffectObservation>(writer, state.effects, limits::kMaxEffectRecords, "effect",
                                       put_effect);
  if (!status.ok()) return status;
  status = put_list<AuditEntry>(writer, state.audit, limits::kMaxAuditEntries, "audit entry",
                                put_audit);
  if (!status.ok()) return status;
  status = put_list<IdempotencyRecord>(writer, state.idempotency, limits::kMaxIdempotencyEntries,
                                       "replay record", put_idempotency);
  if (!status.ok()) return status;
  if (writer.size() > limits::kMaxStateBytes) {
    return Status::error(StatusCode::LimitExceeded,
                         "encoded state of " + std::to_string(writer.size()) +
                             " bytes exceeds the state bound of " +
                             std::to_string(limits::kMaxStateBytes));
  }
  return writer.take();
}

Result<StoreState> decode_state(std::string_view payload) {
  if (payload.size() > limits::kMaxStateBytes) {
    return Status::error(StatusCode::Overlong, "state payload exceeds the state bound");
  }
  Reader reader(payload);
  StoreState state;
  auto format = reader.u32();
  if (!format.ok()) return format.status();
  if (format.value() != store_format::kFormatVersion) {
    return Status::error(StatusCode::Unsupported,
                         "state format version " + std::to_string(format.value()) +
                             " is not supported by this build");
  }
  state.format_version = format.value();
  auto revision = get_id<StateRevisionTag>(reader);
  if (!revision.ok()) return revision.status();
  state.revision = revision.value();
  auto policy_generation = get_id<PolicyGenerationTag>(reader);
  if (!policy_generation.ok()) return policy_generation.status();
  state.policy_generation = policy_generation.value();
  auto evidence = get_id<EvidenceGenerationTag>(reader);
  if (!evidence.ok()) return evidence.status();
  state.evidence_generation = evidence.value();
  auto effect = get_id<EffectGenerationTag>(reader);
  if (!effect.ok()) return effect.status();
  state.effect_generation = effect.value();
  auto plan_generation = get_id<PlanGenerationTag>(reader);
  if (!plan_generation.ok()) return plan_generation.status();
  state.plan_generation = plan_generation.value();
  auto tick = get_id<TickTag>(reader);
  if (!tick.ok()) return tick.status();
  state.tick = tick.value();
  auto epoch = get_id<AuthorityEpochTag>(reader);
  if (!epoch.ok()) return epoch.status();
  state.authority_epoch = epoch.value();
  auto incarnation = get_id<IncarnationTag>(reader);
  if (!incarnation.ok()) return incarnation.status();
  state.incarnation = incarnation.value();
  auto sequence = reader.u64();
  if (!sequence.ok()) return sequence.status();
  state.next_sequence = sequence.value();
  auto installed = reader.boolean();
  if (!installed.ok()) return installed.status();
  state.policy_installed = installed.value();
  auto policy = get_policy(reader, true);
  if (!policy.ok()) return policy.status();
  state.policy = policy.value();
  auto snapshot = get_snapshot(reader);
  if (!snapshot.ok()) return snapshot.status();
  state.snapshot = std::move(snapshot.value());
  auto observed = get_list<ObservedLoadState>(reader, limits::kMaxLoads, "observed load", get_observed);
  if (!observed.ok()) return observed.status();
  state.observed = std::move(observed.value());
  auto plans = get_list<SheddingPlan>(reader, limits::kMaxPlansRetained, "plan",
                                      [](Reader& source) { return get_plan(source, true); });
  if (!plans.ok()) return plans.status();
  state.plans = std::move(plans.value());
  auto recoveries = get_list<RecoveryDecision>(reader, limits::kMaxRecoveryDecisionsRetained,
                                               "recovery decision", get_recovery);
  if (!recoveries.ok()) return recoveries.status();
  state.recoveries = std::move(recoveries.value());
  auto effects = get_list<EffectObservation>(reader, limits::kMaxEffectRecords, "effect", get_effect);
  if (!effects.ok()) return effects.status();
  state.effects = std::move(effects.value());
  auto audit = get_list<AuditEntry>(reader, limits::kMaxAuditEntries, "audit entry", get_audit);
  if (!audit.ok()) return audit.status();
  state.audit = std::move(audit.value());
  auto idempotency = get_list<IdempotencyRecord>(reader, limits::kMaxIdempotencyEntries,
                                                 "replay record", get_idempotency);
  if (!idempotency.ok()) return idempotency.status();
  state.idempotency = std::move(idempotency.value());
  const Status end = reader.expect_end();
  if (!end.ok()) {
    return end;
  }
  const Status valid = state.validate();
  if (!valid.ok()) {
    return valid;
  }
  return state;
}

}  // namespace load_shedding::detail
