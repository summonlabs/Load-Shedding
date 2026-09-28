// Policy proof obligations: structural validation, the documented default, stage
// exclusivity, and content-addressed policy identity.

#include <string>

#include "load_shedding/json_io.hpp"
#include "load_shedding/policy.hpp"
#include "test_harness.hpp"

using namespace load_shedding;

namespace {

StageDefinition stage(std::uint32_t ordinal, std::string name,
                      std::vector<PriorityClass> priorities) {
  StageDefinition definition;
  definition.index = StageIndex::from_ordinal(ordinal);
  definition.name = std::move(name);
  definition.priorities = std::move(priorities);
  return definition;
}

}  // namespace

LS_TEST(policy, default_policy_is_valid_and_protects_critical) {
  const SheddingPolicy policy = make_default_policy();
  LS_CHECK(policy.validate().ok());
  LS_CHECK_EQ(policy.stages.size(), std::size_t{3});
  LS_CHECK_EQ(policy.mode, SelectionMode::WholeLoadGreedy);
  LS_CHECK(!policy.allow_emergency_override);
  LS_CHECK_EQ(policy.protected_priorities.size(), std::size_t{1});
  LS_CHECK_EQ(policy.protected_priorities[0], PriorityClass::Critical);
  for (std::size_t index = 0; index < policy.stages.size(); ++index) {
    LS_CHECK_EQ(policy.stages[index].index.value(), static_cast<std::uint32_t>(index));
    LS_CHECK(!policy.stages[index].requires_emergency_authority);
  }
  // A policy that lists a protected class as shedable is refused outright.
  SheddingPolicy broken = policy;
  broken.shed_classes.push_back(LoadClass::Protected);
  LS_REQUIRE_STATUS(broken.validate(), StatusCode::PolicyViolation);
  broken = policy;
  broken.schema_version = 99;
  LS_REQUIRE_STATUS(broken.validate(), StatusCode::Unsupported);
  broken = policy;
  broken.name.clear();
  LS_REQUIRE_STATUS(broken.validate(), StatusCode::InvalidArgument);
  broken = policy;
  broken.shed_classes.clear();
  LS_REQUIRE_STATUS(broken.validate(), StatusCode::InvalidArgument);
  broken = policy;
  broken.max_evidence_age_ticks = 1000000001ULL;
  LS_REQUIRE_STATUS(broken.validate(), StatusCode::OutOfRange);
}

LS_TEST(policy, stage_rules_are_enforced) {
  StageDefinition empty = stage(0, "empty", {});
  LS_REQUIRE_STATUS(empty.validate(), StatusCode::InvalidArgument);

  StageDefinition unsorted = stage(0, "unsorted", {PriorityClass::Optional, PriorityClass::Standard});
  LS_REQUIRE_STATUS(unsorted.validate(), StatusCode::InvalidArgument);

  StageDefinition duplicated = stage(0, "duplicated", {PriorityClass::Standard, PriorityClass::Standard});
  LS_REQUIRE_STATUS(duplicated.validate(), StatusCode::DuplicateIdentity);

  StageDefinition unnamed = stage(0, "", {PriorityClass::Standard});
  LS_REQUIRE_STATUS(unnamed.validate(), StatusCode::InvalidArgument);

  StageDefinition too_many_priorities = stage(0, "many", {PriorityClass::Critical});
  for (std::uint32_t rank = 1; rank <= limits::kMaxPrioritiesPerStage; ++rank) {
    too_many_priorities.priorities.push_back(static_cast<PriorityClass>(rank));
  }
  LS_REQUIRE_STATUS(too_many_priorities.validate(), StatusCode::Overlong);

  SheddingPolicy policy = make_default_policy();
  policy.stages[1].index = StageIndex::from_ordinal(5);
  LS_REQUIRE_STATUS(policy.validate(), StatusCode::InvalidArgument);

  policy = make_default_policy();
  policy.stages[1].priorities = {PriorityClass::Optional};
  LS_REQUIRE_STATUS(policy.validate(), StatusCode::DuplicateIdentity);

  policy = make_default_policy();
  policy.stages[0].max_shed_ppm = 1000001;
  LS_REQUIRE_STATUS(policy.validate(), StatusCode::OutOfRange);

  policy = make_default_policy();
  policy.stages[0].max_loads = 0;
  LS_REQUIRE_STATUS(policy.validate(), StatusCode::OutOfRange);

  policy = make_default_policy();
  policy.stages.clear();
  LS_CHECK(policy.validate().ok());  // a policy that may shed nothing is valid and fails closed
}

LS_TEST(policy, content_digest_tracks_content_not_generation) {
  SheddingPolicy first = make_default_policy();
  first.generation = PolicyGeneration::from_value(1);
  SheddingPolicy second = make_default_policy();
  second.generation = PolicyGeneration::from_value(99);
  LS_CHECK_EQ(first.content_digest().hex(), second.content_digest().hex());

  second.stages[2].max_shed = Power::from_watts(1000);
  LS_CHECK_NE(first.content_digest().hex(), second.content_digest().hex());

  // The digest is stable across repeated computation.
  LS_CHECK_EQ(first.content_digest().hex(), make_default_policy().content_digest().hex());
}

LS_TEST(policy, json_projection_round_trips_through_the_parser) {
  const SheddingPolicy policy = make_default_policy();
  const JsonValue json = to_json(policy);
  auto parsed = policy_from_json(json);
  LS_CHECK(parsed.ok());
  LS_CHECK_EQ(parsed.value().content_digest().hex(), policy.content_digest().hex());
  LS_CHECK_EQ(parsed.value().name, policy.name);
  LS_CHECK_EQ(parsed.value().stages.size(), policy.stages.size());
}

LS_TEST(policy, json_parser_refuses_unknown_members_and_bad_values) {
  JsonValue document = JsonValue::object();
  document.set("name", JsonValue::text("x"));
  document.set("priorities", JsonValue::text("oops"));
  LS_REQUIRE_STATUS(policy_from_json(document), StatusCode::InvalidArgument);

  JsonValue with_unknown = to_json(make_default_policy());
  with_unknown.set("shed_everything", JsonValue::boolean(true));
  LS_REQUIRE_STATUS(policy_from_json(with_unknown), StatusCode::InvalidArgument);

  JsonValue no_name = JsonValue::object();
  LS_REQUIRE_STATUS(policy_from_json(no_name), StatusCode::InvalidArgument);

  JsonValue stage_unknown = to_json(make_default_policy());
  JsonValue stages = JsonValue::array();
  JsonValue stage_object = JsonValue::object();
  stage_object.set("name", JsonValue::text("s"));
  stage_object.set("priorities", JsonValue::array());
  stages.push(std::move(stage_object));
  stage_unknown.set("stages", std::move(stages));
  LS_REQUIRE_STATUS(policy_from_json(stage_unknown), StatusCode::InvalidArgument);

  JsonValue bad_class = to_json(make_default_policy());
  JsonValue classes = JsonValue::array();
  classes.push(JsonValue::text("everything"));
  bad_class.set("shed_classes", std::move(classes));
  LS_REQUIRE_STATUS(policy_from_json(bad_class), StatusCode::InvalidArgument);

  JsonValue bad_mode = to_json(make_default_policy());
  bad_mode.set("mode", JsonValue::text("optimal"));
  LS_REQUIRE_STATUS(policy_from_json(bad_mode), StatusCode::InvalidArgument);
}

LS_TEST(policy, enum_tokens_are_stable) {
  LS_CHECK_EQ(to_string(LoadClass::NonSheddable), std::string_view("non-sheddable"));
  LS_CHECK_EQ(to_string(PriorityClass::Deferrable), std::string_view("deferrable"));
  LS_CHECK_EQ(to_string(SelectionMode::NoOvershootGreedy), std::string_view("no-overshoot-greedy"));
  LS_CHECK_EQ(to_string(RecoveryOrder::PriorityThenReverseStage),
              std::string_view("priority-then-reverse-stage"));
  LS_CHECK(load_class_from_string("protected").ok());
  LS_CHECK(priority_class_from_string("critical").ok());
  LS_CHECK(selection_mode_from_string("whole-load-greedy").ok());
  LS_CHECK(recovery_order_from_string("reverse-stage-then-priority").ok());
  LS_REQUIRE_STATUS(load_class_from_string("unknown"), StatusCode::InvalidArgument);
  LS_REQUIRE_STATUS(priority_class_from_string("unknown"), StatusCode::InvalidArgument);
  LS_CHECK(priority_rank(PriorityClass::Critical) < priority_rank(PriorityClass::Optional));
  LS_CHECK(is_less_important(PriorityClass::Optional, PriorityClass::Critical));
  LS_CHECK(!is_less_important(PriorityClass::Critical, PriorityClass::Optional));
}
