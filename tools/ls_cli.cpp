// Load Shedding administration tool.
//
// The tool is a thin adapter: every value it prints comes from the library, and
// every state change goes through the library's authority-checked mutations. It
// contains no shedding policy, no selection rule, and no accounting of its own.

#include <algorithm>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "cli_support.hpp"
#include "load_shedding/engine.hpp"
#include "load_shedding/json_io.hpp"
#include "load_shedding/version.hpp"

namespace load_shedding::cli {
namespace {

using ExitCode = ExitCode;

constexpr std::string_view kUsage =
    "load-shedding <command> [options]\n"
    "\n"
    "Common options:\n"
    "  --store <dir>        store directory (required for store commands)\n"
    "  --incarnation <n>    writer incarnation identity (default 1)\n"
    "  --tick <n>           logical tick for this operation\n"
    "  --json               emit machine-readable JSON\n"
    "  --quiet              suppress non-essential output\n"
    "\n"
    "Commands:\n"
    "  init [--default-policy]                     take authority, optionally install the default policy\n"
    "  policy show                                 print the installed policy\n"
    "  policy install --file <json> | --default    install a policy generation\n"
    "  stages                                      print the stage plan\n"
    "  load add --ref R [--class C] [--priority P] [--contribution W]\n"
    "           [--evidence-state S] [--in-service B] [--note T]\n"
    "  load list                                   print the facility evidence\n"
    "  load remove --ref R                         withdraw a load\n"
    "  protected add --ref O --load L --reserved W [--inactive]\n"
    "  protected list                              print protected obligations\n"
    "  protected remove --ref O                    withdraw an obligation\n"
    "  snapshot install --file <json>              install whole facility evidence\n"
    "  snapshot show                               print the facility snapshot\n"
    "  plan --request N --deficit W [--emergency --justification T]\n"
    "  explain --plan N [--verdict V]              print the explanation trace\n"
    "  plans | plan list                           list retained plans\n"
    "  plan show --plan N                          print one retained plan\n"
    "  plan-diff --left N --right N                structural difference of two plans\n"
    "  recovery-order --request N --plan N --headroom W\n"
    "  recovery show --request N                   print a retained recovery decision\n"
    "  effect observe --request N --plan N --load L --state shed|energized|unknown\n"
    "                 [--source adapter|operator|synthetic] [--verification acknowledged|confirmed|contradicted]\n"
    "                 [--attempt N]\n"
    "  effect list                                 print retained effect records\n"
    "  observed                                    print observed load states\n"
    "  revalidate --plan N                         re-check a plan against current state\n"
    "  verify                                      re-derive every retained invariant\n"
    "  history [--limit N] [--offset N] [--kind K] [--plan N]\n"
    "  store-audit                                 inspect the store directory\n"
    "  authority                                   print the authority view\n"
    "  version                                     print the library version\n";

struct Claim {
  AuthorityEpoch epoch;
  Incarnation incarnation;
};

Result<Claim> resolve_claim(const Arguments& arguments, const Engine& engine) {
  const AuthorityStatus status = engine.authority();
  Claim claim;
  claim.epoch = status.epoch;
  claim.incarnation = status.incarnation;
  if (arguments.has("authority-epoch")) {
    auto value = arguments.unsigned_value("authority-epoch");
    if (!value.ok()) {
      return value.status();
    }
    claim.epoch = AuthorityEpoch::from_value(value.value());
  }
  if (arguments.has("incarnation-claim")) {
    auto value = arguments.unsigned_value("incarnation-claim");
    if (!value.ok()) {
      return value.status();
    }
    claim.incarnation = Incarnation::from_value(value.value());
  }
  return claim;
}

Result<Tick> resolve_tick(const Arguments& arguments, const Engine& engine, const Session& session) {
  if (session.tick_given) {
    return Tick::from_value(session.tick);
  }
  if (arguments.has("tick")) {
    auto value = arguments.unsigned_value("tick");
    if (!value.ok()) {
      return value.status();
    }
    return Tick::from_value(value.value());
  }
  const Tick current = engine.tick();
  if (current.is_set()) {
    return current;
  }
  return Tick::first();
}

JsonValue authority_json(const Engine& engine) { return to_json(engine.authority()); }

Result<std::uint64_t> require_unsigned(const Arguments& arguments, const char* name) {
  return arguments.unsigned_value(name);
}

// ---------------------------------------------------------------------------

Result<int> command_init(const Session& session, const Arguments& arguments) {
  auto engine = open_engine(session, true);
  if (!engine.ok()) {
    return engine.status();
  }
  Engine& instance = engine.value();
  JsonValue report = JsonValue::object();
  if (arguments.has("default-policy") && !instance.policy_generation().is_set()) {
    SheddingPolicy policy = make_default_policy();
    auto installed = instance.install_policy(policy, instance.authority().epoch,
                                             instance.authority().incarnation);
    if (!installed.ok()) {
      return installed.status();
    }
    report.set("installed_policy_generation", JsonValue::integer(static_cast<std::int64_t>(
                                                  installed.value().value())));
  }
  report.set("authority", authority_json(instance));
  report.set("policy_generation",
             JsonValue::integer(static_cast<std::int64_t>(instance.policy_generation().value())));
  report.set("revision", JsonValue::integer(static_cast<std::int64_t>(instance.revision().value())));
  report.set("store_generation",
             JsonValue::integer(static_cast<std::int64_t>(instance.commit().generation)));
  emit(session, report);
  return static_cast<int>(ExitCode::Ok);
}

Result<int> command_policy(const Session& session, const Arguments& arguments,
                           const std::string& action) {
  if (action == "show") {
    auto engine = open_engine(session, false);
    if (!engine.ok()) {
      return engine.status();
    }
    const SheddingPolicy policy = engine.value().policy();
    if (!policy.generation.is_set()) {
      return Status::error(StatusCode::NotFound, "no policy is installed");
    }
    emit(session, to_json(policy));
    return static_cast<int>(ExitCode::Ok);
  }
  if (action != "install") {
    return Status::error(StatusCode::InvalidArgument, "usage: policy show | policy install --file F");
  }
  SheddingPolicy policy;
  if (arguments.has("default")) {
    policy = make_default_policy();
  } else {
    auto path = arguments.value("file");
    if (!path.ok()) {
      return path.status();
    }
    auto document = read_json_file(path.value());
    if (!document.ok()) {
      return document.status();
    }
    auto parsed = policy_from_json(document.value());
    if (!parsed.ok()) {
      return parsed.status();
    }
    policy = parsed.value();
  }
  auto engine = open_engine(session, true);
  if (!engine.ok()) {
    return engine.status();
  }
  auto claim = resolve_claim(arguments, engine.value());
  if (!claim.ok()) {
    return claim.status();
  }
  auto installed = engine.value().install_policy(policy, claim.value().epoch, claim.value().incarnation);
  if (!installed.ok()) {
    return installed.status();
  }
  JsonValue report = JsonValue::object();
  report.set("policy_generation",
             JsonValue::integer(static_cast<std::int64_t>(installed.value().value())));
  report.set("content_digest", JsonValue::text(engine.value().policy().content_digest().hex()));
  emit(session, report);
  return static_cast<int>(ExitCode::Ok);
}

Result<int> command_stages(const Session& session) {
  auto engine = open_engine(session, false);
  if (!engine.ok()) {
    return engine.status();
  }
  const SheddingPolicy policy = engine.value().policy();
  JsonValue stages = JsonValue::array();
  for (const StageDefinition& stage : policy.stages) {
    stages.push(to_json(stage));
  }
  JsonValue report = JsonValue::object();
  report.set("policy_generation",
             JsonValue::integer(static_cast<std::int64_t>(policy.generation.value())));
  report.set("mode", JsonValue::text(std::string(to_string(policy.mode))));
  report.set("stages", std::move(stages));
  emit(session, report);
  return static_cast<int>(ExitCode::Ok);
}

Result<int> command_load(const Session& session, const Arguments& arguments,
                         const std::string& action) {
  if (action == "list" || action == "show") {
    auto engine = open_engine(session, false);
    if (!engine.ok()) {
      return engine.status();
    }
    const FacilitySnapshot snapshot = engine.value().snapshot();
    JsonValue loads = JsonValue::array();
    for (const LoadRecord& load : snapshot.loads) {
      loads.push(to_json(load));
    }
    JsonValue report = JsonValue::object();
    report.set("evidence_generation",
               JsonValue::integer(static_cast<std::int64_t>(snapshot.generation.value())));
    report.set("tick", JsonValue::integer(static_cast<std::int64_t>(snapshot.tick.value())));
    report.set("loads", std::move(loads));
    emit(session, report);
    return static_cast<int>(ExitCode::Ok);
  }

  auto engine = open_engine(session, true);
  if (!engine.ok()) {
    return engine.status();
  }
  Engine& instance = engine.value();
  auto claim = resolve_claim(arguments, instance);
  if (!claim.ok()) {
    return claim.status();
  }
  if (action == "remove") {
    auto ref_text = arguments.value("ref");
    if (!ref_text.ok()) {
      return ref_text.status();
    }
    auto ref = LoadRef::parse(ref_text.value());
    if (!ref.ok()) {
      return ref.status();
    }
    auto revision = instance.remove_load(ref.value(), claim.value().epoch, claim.value().incarnation);
    if (!revision.ok()) {
      return revision.status();
    }
    JsonValue report = JsonValue::object();
    report.set("revision", JsonValue::integer(static_cast<std::int64_t>(revision.value().value())));
    emit(session, report);
    return static_cast<int>(ExitCode::Ok);
  }
  if (action != "add") {
    return Status::error(StatusCode::InvalidArgument, "usage: load add | load list | load remove");
  }
  JsonValue document = JsonValue::object();
  auto ref_text = arguments.value("ref");
  if (!ref_text.ok()) {
    return ref_text.status();
  }
  document.set("ref", JsonValue::text(ref_text.value()));
  auto load_class = arguments.value_or("class", "sheddable");
  if (!load_class.ok()) {
    return load_class.status();
  }
  document.set("class", JsonValue::text(load_class.value()));
  auto priority = arguments.value_or("priority", "standard");
  if (!priority.ok()) {
    return priority.status();
  }
  document.set("priority", JsonValue::text(priority.value()));
  auto state = arguments.value_or("evidence-state", "known");
  if (!state.ok()) {
    return state.status();
  }
  document.set("evidence_state", JsonValue::text(state.value()));
  auto tick = arguments.unsigned_value("tick");
  if (!tick.ok() && tick.code() != StatusCode::NotFound) {
    return tick.status();
  }
  if (tick.ok()) {
    document.set("tick", JsonValue::integer(static_cast<std::int64_t>(tick.value())));
  } else {
    auto resolved = resolve_tick(arguments, instance, session);
    if (!resolved.ok()) {
      return resolved.status();
    }
    document.set("tick", JsonValue::integer(static_cast<std::int64_t>(resolved.value().value())));
  }
  auto contribution = arguments.unsigned_value("contribution");
  if (contribution.ok()) {
    document.set("contribution_watts", JsonValue::integer(static_cast<std::int64_t>(contribution.value())));
  } else if (contribution.code() != StatusCode::NotFound) {
    return contribution.status();
  }
  if (arguments.has("in-service")) {
    auto in_service = arguments.value("in-service");
    if (!in_service.ok()) {
      return in_service.status();
    }
    if (in_service.value() == "true") {
      document.set("in_service", JsonValue::boolean(true));
    } else if (in_service.value() == "false") {
      document.set("in_service", JsonValue::boolean(false));
    } else {
      return Status::error(StatusCode::InvalidArgument, "--in-service takes true or false");
    }
  }
  auto note = arguments.value_or("note", "");
  if (!note.ok()) {
    return note.status();
  }
  document.set("note", JsonValue::text(note.value()));

  auto parsed = load_record_from_json(document);
  if (!parsed.ok()) {
    return parsed.status();
  }
  auto revision = instance.upsert_load(parsed.value(), claim.value().epoch, claim.value().incarnation);
  if (!revision.ok()) {
    return revision.status();
  }
  JsonValue report = JsonValue::object();
  report.set("revision", JsonValue::integer(static_cast<std::int64_t>(revision.value().value())));
  report.set("evidence_generation",
             JsonValue::integer(static_cast<std::int64_t>(instance.evidence_generation().value())));
  emit(session, report);
  return static_cast<int>(ExitCode::Ok);
}

Result<int> command_protected(const Session& session, const Arguments& arguments,
                              const std::string& action) {
  if (action == "list" || action == "show") {
    auto engine = open_engine(session, false);
    if (!engine.ok()) {
      return engine.status();
    }
    const FacilitySnapshot snapshot = engine.value().snapshot();
    JsonValue obligations = JsonValue::array();
    for (const ProtectedObligation& obligation : snapshot.obligations) {
      obligations.push(to_json(obligation));
    }
    JsonValue report = JsonValue::object();
    report.set("obligations", std::move(obligations));
    emit(session, report);
    return static_cast<int>(ExitCode::Ok);
  }
  auto engine = open_engine(session, true);
  if (!engine.ok()) {
    return engine.status();
  }
  Engine& instance = engine.value();
  auto claim = resolve_claim(arguments, instance);
  if (!claim.ok()) {
    return claim.status();
  }
  if (action == "remove") {
    auto ref_text = arguments.value("ref");
    if (!ref_text.ok()) {
      return ref_text.status();
    }
    auto ref = ObligationRef::parse(ref_text.value());
    if (!ref.ok()) {
      return ref.status();
    }
    auto revision = instance.remove_obligation(ref.value(), claim.value().epoch,
                                               claim.value().incarnation);
    if (!revision.ok()) {
      return revision.status();
    }
    JsonValue report = JsonValue::object();
    report.set("revision", JsonValue::integer(static_cast<std::int64_t>(revision.value().value())));
    emit(session, report);
    return static_cast<int>(ExitCode::Ok);
  }
  if (action != "add") {
    return Status::error(StatusCode::InvalidArgument,
                         "usage: protected add | protected list | protected remove");
  }
  JsonValue document = JsonValue::object();
  auto ref_text = arguments.value("ref");
  if (!ref_text.ok()) {
    return ref_text.status();
  }
  document.set("ref", JsonValue::text(ref_text.value()));
  document.set("load", JsonValue::text(arguments.value_or("load", "").value()));
  auto reserved = arguments.unsigned_value("reserved");
  if (!reserved.ok()) {
    return reserved.status();
  }
  document.set("reserved_watts", JsonValue::integer(static_cast<std::int64_t>(reserved.value())));
  document.set("active", JsonValue::boolean(!arguments.has("inactive")));
  auto resolved = resolve_tick(arguments, instance, session);
  if (!resolved.ok()) {
    return resolved.status();
  }
  document.set("tick", JsonValue::integer(static_cast<std::int64_t>(resolved.value().value())));
  auto parsed = obligation_from_json(document);
  if (!parsed.ok()) {
    return parsed.status();
  }
  auto revision = instance.upsert_obligation(parsed.value(), claim.value().epoch,
                                             claim.value().incarnation);
  if (!revision.ok()) {
    return revision.status();
  }
  JsonValue report = JsonValue::object();
  report.set("revision", JsonValue::integer(static_cast<std::int64_t>(revision.value().value())));
  emit(session, report);
  return static_cast<int>(ExitCode::Ok);
}

Result<int> command_snapshot(const Session& session, const Arguments& arguments,
                             const std::string& action) {
  if (action == "show") {
    auto engine = open_engine(session, false);
    if (!engine.ok()) {
      return engine.status();
    }
    emit(session, to_json(engine.value().snapshot()));
    return static_cast<int>(ExitCode::Ok);
  }
  if (action != "install") {
    return Status::error(StatusCode::InvalidArgument, "usage: snapshot install --file F | snapshot show");
  }
  auto path = arguments.value("file");
  if (!path.ok()) {
    return path.status();
  }
  auto document = read_json_file(path.value());
  if (!document.ok()) {
    return document.status();
  }
  auto parsed = facility_snapshot_from_json(document.value());
  if (!parsed.ok()) {
    return parsed.status();
  }
  auto engine = open_engine(session, true);
  if (!engine.ok()) {
    return engine.status();
  }
  auto claim = resolve_claim(arguments, engine.value());
  if (!claim.ok()) {
    return claim.status();
  }
  auto revision = engine.value().replace_snapshot(parsed.value(), claim.value().epoch,
                                                  claim.value().incarnation);
  if (!revision.ok()) {
    return revision.status();
  }
  JsonValue report = JsonValue::object();
  report.set("revision", JsonValue::integer(static_cast<std::int64_t>(revision.value().value())));
  report.set("evidence_generation",
             JsonValue::integer(static_cast<std::int64_t>(engine.value().evidence_generation().value())));
  emit(session, report);
  return static_cast<int>(ExitCode::Ok);
}

Result<int> command_plan(const Session& session, const Arguments& arguments) {
  auto engine = open_engine(session, true);
  if (!engine.ok()) {
    return engine.status();
  }
  Engine& instance = engine.value();
  auto claim = resolve_claim(arguments, instance);
  if (!claim.ok()) {
    return claim.status();
  }
  PlanRequest request;
  auto request_id = require_unsigned(arguments, "request");
  if (!request_id.ok()) {
    return request_id.status();
  }
  request.request_id = RequestId::from_value(request_id.value());
  auto deficit = require_unsigned(arguments, "deficit");
  if (!deficit.ok()) {
    return deficit.status();
  }
  request.requested_reduction = Power::from_watts(static_cast<std::int64_t>(deficit.value()));
  request.policy_generation = PolicyGeneration::from_value(
      arguments.unsigned_or("policy-generation", instance.policy_generation().value(), nullptr));
  request.evidence_generation = EvidenceGeneration::from_value(
      arguments.unsigned_or("evidence-generation", instance.evidence_generation().value(), nullptr));
  request.effect_generation = EffectGeneration::from_value(
      arguments.unsigned_or("effect-generation", instance.effect_generation().value(), nullptr));
  request.base_revision = StateRevision::from_value(
      arguments.unsigned_or("revision", instance.revision().value(), nullptr));
  request.authority_epoch = claim.value().epoch;
  request.incarnation = claim.value().incarnation;
  auto tick = resolve_tick(arguments, instance, session);
  if (!tick.ok()) {
    return tick.status();
  }
  request.tick = tick.value();
  request.emergency_authority = arguments.has("emergency");
  request.emergency_justification = arguments.value_or("justification", "").value();

  auto outcome = instance.plan(request);
  if (!outcome.ok()) {
    return outcome.status();
  }
  emit(session, to_json(outcome.value()));
  return static_cast<int>(ExitCode::Ok);
}

Result<int> command_explain(const Session& session, const Arguments& arguments) {
  auto engine = open_engine(session, false);
  if (!engine.ok()) {
    return engine.status();
  }
  auto plan_id = require_unsigned(arguments, "plan");
  if (!plan_id.ok()) {
    return plan_id.status();
  }
  auto plan = engine.value().plan_by_id(PlanId::from_value(plan_id.value()));
  if (!plan.ok()) {
    return plan.status();
  }
  std::optional<LoadVerdict> filter;
  if (arguments.has("verdict")) {
    auto text = arguments.value("verdict");
    if (!text.ok()) {
      return text.status();
    }
    auto parsed = load_verdict_from_string(text.value());
    if (!parsed.ok()) {
      return parsed.status();
    }
    filter = parsed.value();
  }
  JsonValue considerations = JsonValue::array();
  std::size_t matched = 0;
  for (const LoadConsideration& consideration : plan.value().considerations) {
    if (filter.has_value() && !(consideration.verdict == filter.value())) {
      continue;
    }
    considerations.push(to_json(consideration));
    ++matched;
  }
  JsonValue report = JsonValue::object();
  report.set("plan_id", JsonValue::integer(static_cast<std::int64_t>(plan.value().identity.plan_id.value())));
  report.set("plan_generation",
             JsonValue::integer(static_cast<std::int64_t>(plan.value().identity.generation.value())));
  report.set("content_digest", JsonValue::text(plan.value().content_digest().hex()));
  report.set("coverage", JsonValue::text(std::string(to_string(plan.value().coverage))));
  report.set("summary",
             JsonValue::text("selected " +
                             plan.value().closure.selected_expected_reduction.to_string() +
                             " of " + plan.value().closure.requested_reduction.to_string() +
                             ", residual " + plan.value().closure.residual_deficit.to_string()));
  report.set("matched", JsonValue::integer(static_cast<std::int64_t>(matched)));
  report.set("total", JsonValue::integer(static_cast<std::int64_t>(plan.value().considerations.size())));
  report.set("considerations", std::move(considerations));
  emit(session, report);
  return static_cast<int>(ExitCode::Ok);
}

Result<int> command_plans(const Session& session, const Arguments& arguments,
                          const std::string& action) {
  auto engine = open_engine(session, false);
  if (!engine.ok()) {
    return engine.status();
  }
  if (action == "show") {
    if (arguments.has("latest")) {
      auto plan = engine.value().latest_plan();
      if (!plan.ok()) {
        return plan.status();
      }
      emit(session, to_json(plan.value()));
      return static_cast<int>(ExitCode::Ok);
    }
    auto plan_id = require_unsigned(arguments, "plan");
    if (!plan_id.ok()) {
      return plan_id.status();
    }
    auto plan = engine.value().plan_by_id(PlanId::from_value(plan_id.value()));
    if (!plan.ok()) {
      return plan.status();
    }
    emit(session, to_json(plan.value()));
    return static_cast<int>(ExitCode::Ok);
  }
  JsonValue entries = JsonValue::array();
  for (std::size_t index = 0; index < engine.value().plan_count(); ++index) {
    auto plan = engine.value().plan_at(index);
    if (!plan.ok()) {
      return plan.status();
    }
    JsonValue entry = JsonValue::object();
    entry.set("plan_id", JsonValue::integer(static_cast<std::int64_t>(plan.value().identity.plan_id.value())));
    entry.set("plan_generation",
              JsonValue::integer(static_cast<std::int64_t>(plan.value().identity.generation.value())));
    entry.set("request_id",
              JsonValue::integer(static_cast<std::int64_t>(plan.value().identity.request_id.value())));
    entry.set("coverage", JsonValue::text(std::string(to_string(plan.value().coverage))));
    entry.set("action_count",
              JsonValue::integer(static_cast<std::int64_t>(plan.value().actions.size())));
    entry.set("selected_watts", JsonValue::integer(plan.value().closure.selected_expected_reduction.watts()));
    entry.set("residual_watts", JsonValue::integer(plan.value().closure.residual_deficit.watts()));
    entry.set("content_digest", JsonValue::text(plan.value().content_digest().hex()));
    entries.push(std::move(entry));
  }
  JsonValue report = JsonValue::object();
  report.set("plan_count", JsonValue::integer(static_cast<std::int64_t>(engine.value().plan_count())));
  report.set("plans", std::move(entries));
  emit(session, report);
  return static_cast<int>(ExitCode::Ok);
}

Result<int> command_recovery(const Session& session, const Arguments& arguments,
                             const std::string& action) {
  if (action == "show") {
    auto engine = open_engine(session, false);
    if (!engine.ok()) {
      return engine.status();
    }
    if (arguments.has("latest")) {
      auto decision = engine.value().last_recovery_decision();
      if (!decision.ok()) {
        return decision.status();
      }
      emit(session, to_json(decision.value()));
      return static_cast<int>(ExitCode::Ok);
    }
    auto request = require_unsigned(arguments, "request");
    if (!request.ok()) {
      return request.status();
    }
    auto decision = engine.value().recovery_by_request(RequestId::from_value(request.value()));
    if (!decision.ok()) {
      return decision.status();
    }
    emit(session, to_json(decision.value()));
    return static_cast<int>(ExitCode::Ok);
  }
  if (action != "order") {
    return Status::error(StatusCode::InvalidArgument, "usage: recovery-order | recovery show");
  }
  auto engine = open_engine(session, true);
  if (!engine.ok()) {
    return engine.status();
  }
  Engine& instance = engine.value();
  auto claim = resolve_claim(arguments, instance);
  if (!claim.ok()) {
    return claim.status();
  }
  RecoveryRequest request;
  auto request_id = require_unsigned(arguments, "request");
  if (!request_id.ok()) {
    return request_id.status();
  }
  request.request_id = RequestId::from_value(request_id.value());
  auto plan_id = require_unsigned(arguments, "plan");
  if (!plan_id.ok()) {
    return plan_id.status();
  }
  request.plan_id = PlanId::from_value(plan_id.value());
  auto plan = instance.plan_by_id(request.plan_id);
  if (!plan.ok()) {
    return plan.status();
  }
  request.plan_generation = PlanGeneration::from_value(
      arguments.unsigned_or("plan-generation", plan.value().identity.generation.value(), nullptr));
  auto headroom = require_unsigned(arguments, "headroom");
  if (!headroom.ok()) {
    return headroom.status();
  }
  request.available_headroom = Power::from_watts(static_cast<std::int64_t>(headroom.value()));
  request.policy_generation = PolicyGeneration::from_value(
      arguments.unsigned_or("policy-generation", instance.policy_generation().value(), nullptr));
  request.evidence_generation = EvidenceGeneration::from_value(
      arguments.unsigned_or("evidence-generation", instance.evidence_generation().value(), nullptr));
  request.effect_generation = EffectGeneration::from_value(
      arguments.unsigned_or("effect-generation", instance.effect_generation().value(), nullptr));
  request.base_revision = StateRevision::from_value(
      arguments.unsigned_or("revision", instance.revision().value(), nullptr));
  request.authority_epoch = claim.value().epoch;
  request.incarnation = claim.value().incarnation;
  auto tick = resolve_tick(arguments, instance, session);
  if (!tick.ok()) {
    return tick.status();
  }
  request.tick = tick.value();
  auto decision = instance.recovery(request);
  if (!decision.ok()) {
    return decision.status();
  }
  emit(session, to_json(decision.value()));
  return static_cast<int>(ExitCode::Ok);
}

Result<int> command_effect(const Session& session, const Arguments& arguments,
                           const std::string& action) {
  if (action == "list") {
    auto engine = open_engine(session, false);
    if (!engine.ok()) {
      return engine.status();
    }
    JsonValue records = JsonValue::array();
    for (const EffectObservation& observation : engine.value().effects()) {
      records.push(to_json(observation));
    }
    JsonValue report = JsonValue::object();
    report.set("effect_generation",
               JsonValue::integer(static_cast<std::int64_t>(engine.value().effect_generation().value())));
    report.set("records", std::move(records));
    emit(session, report);
    return static_cast<int>(ExitCode::Ok);
  }
  if (action != "observe") {
    return Status::error(StatusCode::InvalidArgument, "usage: effect observe | effect list");
  }
  auto engine = open_engine(session, true);
  if (!engine.ok()) {
    return engine.status();
  }
  Engine& instance = engine.value();
  auto claim = resolve_claim(arguments, instance);
  if (!claim.ok()) {
    return claim.status();
  }
  EffectObservationRequest request;
  auto request_id = require_unsigned(arguments, "request");
  if (!request_id.ok()) {
    return request_id.status();
  }
  request.request_id = RequestId::from_value(request_id.value());
  auto plan_id = require_unsigned(arguments, "plan");
  if (!plan_id.ok()) {
    return plan_id.status();
  }
  request.plan_id = PlanId::from_value(plan_id.value());
  auto plan = instance.plan_by_id(request.plan_id);
  if (!plan.ok()) {
    return plan.status();
  }
  request.plan_generation = PlanGeneration::from_value(
      arguments.unsigned_or("plan-generation", plan.value().identity.generation.value(), nullptr));
  auto load_ref = arguments.value("load");
  if (!load_ref.ok()) {
    return load_ref.status();
  }
  auto parsed_ref = LoadRef::parse(load_ref.value());
  if (!parsed_ref.ok()) {
    return parsed_ref.status();
  }
  request.load = parsed_ref.value();
  auto state_text = arguments.value_or("state", "unknown");
  if (!state_text.ok()) {
    return state_text.status();
  }
  auto state = effect_state_from_string(state_text.value());
  if (!state.ok()) {
    return state.status();
  }
  request.observed = state.value();
  auto source_text = arguments.value_or("source", "synthetic");
  if (!source_text.ok()) {
    return source_text.status();
  }
  auto source = effect_source_from_string(source_text.value());
  if (!source.ok()) {
    return source.status();
  }
  request.source = source.value();
  auto verification_text = arguments.value_or("verification", "acknowledged");
  if (!verification_text.ok()) {
    return verification_text.status();
  }
  auto verification = effect_verification_from_string(verification_text.value());
  if (!verification.ok()) {
    return verification.status();
  }
  request.verification = verification.value();
  request.attempt = AttemptId::from_value(arguments.unsigned_or("attempt", 1, nullptr));
  request.tick = Tick::from_value(arguments.unsigned_or("tick", instance.tick().value(), nullptr));
  request.base_revision = StateRevision::from_value(
      arguments.unsigned_or("revision", instance.revision().value(), nullptr));
  request.authority_epoch = claim.value().epoch;
  request.incarnation = claim.value().incarnation;

  auto observation = instance.observe_effect(request);
  if (!observation.ok()) {
    return observation.status();
  }
  emit(session, to_json(observation.value()));
  return static_cast<int>(ExitCode::Ok);
}

Result<int> command_observed(const Session& session) {
  auto engine = open_engine(session, false);
  if (!engine.ok()) {
    return engine.status();
  }
  JsonValue states = JsonValue::array();
  for (const ObservedLoadState& state : engine.value().observed_states()) {
    states.push(to_json(state));
  }
  JsonValue report = JsonValue::object();
  report.set("effect_generation",
             JsonValue::integer(static_cast<std::int64_t>(engine.value().effect_generation().value())));
  report.set("observed", std::move(states));
  emit(session, report);
  return static_cast<int>(ExitCode::Ok);
}

Result<int> command_revalidate(const Session& session, const Arguments& arguments) {
  auto engine = open_engine(session, false);
  if (!engine.ok()) {
    return engine.status();
  }
  auto plan_id = require_unsigned(arguments, "plan");
  if (!plan_id.ok()) {
    return plan_id.status();
  }
  auto report = engine.value().revalidate(PlanId::from_value(plan_id.value()));
  if (!report.ok()) {
    return report.status();
  }
  emit(session, to_json(report.value()));
  return static_cast<int>(ExitCode::Ok);
}

Result<int> command_verify(const Session& session) {
  auto engine = open_engine(session, false);
  if (!engine.ok()) {
    return engine.status();
  }
  auto report = engine.value().verify();
  if (!report.ok()) {
    return report.status();
  }
  emit(session, to_json(report.value()));
  return report.value().ok ? static_cast<int>(ExitCode::Ok) : static_cast<int>(ExitCode::Failure);
}

Result<int> command_history(const Session& session, const Arguments& arguments) {
  auto engine = open_engine(session, false);
  if (!engine.ok()) {
    return engine.status();
  }
  HistoryQuery query;
  query.limit = arguments.unsigned_or("limit", 32, nullptr);
  query.offset = arguments.unsigned_or("offset", 0, nullptr);
  if (arguments.has("kind")) {
    auto text = arguments.value("kind");
    if (!text.ok()) {
      return text.status();
    }
    auto kind = audit_kind_from_string(text.value());
    if (!kind.ok()) {
      return kind.status();
    }
    query.kind = kind.value();
  }
  if (arguments.has("plan")) {
    auto plan_id = arguments.unsigned_value("plan");
    if (!plan_id.ok()) {
      return plan_id.status();
    }
    query.plan = PlanId::from_value(plan_id.value());
  }
  auto page = engine.value().history(query);
  if (!page.ok()) {
    return page.status();
  }
  emit(session, to_json(page.value()));
  return static_cast<int>(ExitCode::Ok);
}

Result<int> command_store_audit(const Session& session) {
  if (session.store.empty()) {
    return Status::error(StatusCode::InvalidArgument, "--store <directory> is required");
  }
  auto audit = inspect_store(session.store);
  if (!audit.ok()) {
    return audit.status();
  }
  emit(session, to_json(audit.value()));
  return audit.value().clean ? static_cast<int>(ExitCode::Ok) : static_cast<int>(ExitCode::Failure);
}

Result<int> command_authority(const Session& session) {
  auto engine = open_engine(session, false);
  if (!engine.ok()) {
    return engine.status();
  }
  emit(session, authority_json(engine.value()));
  return static_cast<int>(ExitCode::Ok);
}

Result<int> command_plan_diff(const Session& session, const Arguments& arguments) {
  auto engine = open_engine(session, false);
  if (!engine.ok()) {
    return engine.status();
  }
  auto left = require_unsigned(arguments, "left");
  if (!left.ok()) {
    return left.status();
  }
  auto right = require_unsigned(arguments, "right");
  if (!right.ok()) {
    return right.status();
  }
  auto diff = engine.value().diff_plans(PlanId::from_value(left.value()), PlanId::from_value(right.value()));
  if (!diff.ok()) {
    return diff.status();
  }
  emit(session, to_json(diff.value()));
  return static_cast<int>(ExitCode::Ok);
}

int run(const Session& session, const Arguments& arguments) {
  if (arguments.positional.empty()) {
    std::cout << kUsage;
    return static_cast<int>(ExitCode::Usage);
  }
  const std::string& command = arguments.positional[0];
  const std::string action = arguments.positional.size() > 1 ? arguments.positional[1] : std::string();

  if (command == "help" || arguments.has("help")) {
    std::cout << kUsage;
    return static_cast<int>(ExitCode::Ok);
  }
  if (command == "version") {
    JsonValue report = JsonValue::object();
    report.set("version", JsonValue::text(Version::string));
    report.set("store_format_version", JsonValue::integer(1));
    emit(session, report);
    return static_cast<int>(ExitCode::Ok);
  }

  // A recognised command that fails is an operation failure (exit 1); an
  // unrecognised command is a usage error (exit 2). The distinction matters to a
  // script that must tell "refused" from "misinvoked".
  Result<int> outcome = Status::error(StatusCode::InvalidArgument, "unknown command '" + command + "'");
  bool recognised = true;
  if (command == "init") {
    outcome = command_init(session, arguments);
  } else if (command == "policy") {
    outcome = command_policy(session, arguments, action);
  } else if (command == "stages") {
    outcome = command_stages(session);
  } else if (command == "load") {
    outcome = command_load(session, arguments, action);
  } else if (command == "protected") {
    outcome = command_protected(session, arguments, action);
  } else if (command == "snapshot") {
    outcome = command_snapshot(session, arguments, action);
  } else if (command == "plan" && action == "show") {
    outcome = command_plans(session, arguments, "show");
  } else if (command == "plan" && action == "list") {
    outcome = command_plans(session, arguments, "list");
  } else if (command == "plan" && action == "diff") {
    outcome = command_plan_diff(session, arguments);
  } else if (command == "plan") {
    outcome = command_plan(session, arguments);
  } else if (command == "explain") {
    outcome = command_explain(session, arguments);
  } else if (command == "plans" || command == "plan-show") {
    outcome = command_plans(session, arguments, command == "plans" ? "list" : "show");
  } else if (command == "plan-diff") {
    outcome = command_plan_diff(session, arguments);
  } else if (command == "recovery-order") {
    outcome = command_recovery(session, arguments, "order");
  } else if (command == "recovery") {
    outcome = command_recovery(session, arguments, action);
  } else if (command == "effect") {
    outcome = command_effect(session, arguments, action);
  } else if (command == "observed") {
    outcome = command_observed(session);
  } else if (command == "revalidate") {
    outcome = command_revalidate(session, arguments);
  } else if (command == "verify") {
    outcome = command_verify(session);
  } else if (command == "history") {
    outcome = command_history(session, arguments);
  } else if (command == "store-audit") {
    outcome = command_store_audit(session);
  } else if (command == "authority") {
    outcome = command_authority(session);
  } else {
    recognised = false;
  }

  if (!outcome.ok()) {
    emit_error(session, outcome.status());
    if (!recognised) {
      if (!session.json_output) {
        std::cerr << "\n" << kUsage;
      }
      return static_cast<int>(ExitCode::Usage);
    }
    return static_cast<int>(ExitCode::Failure);
  }
  return outcome.value();
}

}  // namespace
}  // namespace load_shedding::cli

int main(int argc, char** argv) {
  using namespace load_shedding::cli;
  auto arguments = parse_arguments(argc, argv);
  if (!arguments.ok()) {
    std::cerr << "error: " << arguments.status().to_string() << "\n";
    return 2;
  }
  auto session = parse_session(arguments.value());
  if (!session.ok()) {
    std::cerr << "error: " << session.status().to_string() << "\n";
    return 2;
  }
  return run(session.value(), arguments.value());
}
