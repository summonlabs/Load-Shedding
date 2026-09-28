// Core proof obligations: digests, checked arithmetic, text validation, and the
// strict JSON reader.

#include <limits>
#include <string>
#include <vector>

#include "load_shedding/digest.hpp"
#include "load_shedding/ids.hpp"
#include "load_shedding/json.hpp"
#include "load_shedding/limits.hpp"
#include "load_shedding/text.hpp"
#include "load_shedding/units.hpp"
#include "test_harness.hpp"

using namespace load_shedding;

LS_TEST(core, sha256_known_vectors) {
  LS_CHECK_EQ(Digest::of("").hex(),
              std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
  LS_CHECK_EQ(Digest::of("abc").hex(),
              std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
  LS_CHECK_EQ(
      Digest::of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq").hex(),
      std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
  // 1,000,000 'a' characters, the classic multi-block vector.
  std::string million(1000000, 'a');
  LS_CHECK_EQ(Digest::of(million).hex(),
              std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
  // Streaming in one-byte chunks must agree with a single update.
  Sha256 streamed;
  for (const char character : million) {
    streamed.update(&character, 1);
  }
  LS_CHECK_EQ(streamed.finish().hex(), Digest::of(million).hex());
}

LS_TEST(core, digest_hex_round_trip_and_rejection) {
  const Digest digest = Digest::of("load-shedding");
  LS_CHECK_EQ(digest.hex().size(), std::size_t{64});
  auto parsed = Digest::from_hex(digest.hex());
  LS_CHECK(parsed.ok());
  LS_CHECK_EQ(parsed.value().hex(), digest.hex());
  LS_REQUIRE_STATUS(Digest::from_hex("00"), StatusCode::InvalidArgument);
  LS_REQUIRE_STATUS(Digest::from_hex(std::string(64, 'z')), StatusCode::InvalidArgument);
  LS_CHECK(Digest().is_zero());
  LS_CHECK(!digest.is_zero());
}

LS_TEST(core, checked_integer_arithmetic_refuses_overflow) {
  constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
  constexpr std::int64_t kMin = std::numeric_limits<std::int64_t>::min();
  LS_CHECK_EQ(LS_REQUIRE_OK(checked_add(kMax - 1, 1)), kMax);
  LS_REQUIRE_STATUS(checked_add(kMax, 1), StatusCode::Overflow);
  LS_REQUIRE_STATUS(checked_sub(kMin, 1), StatusCode::Overflow);
  LS_CHECK_EQ(LS_REQUIRE_OK(checked_sub(kMin + 1, 1)), kMin);
  LS_REQUIRE_STATUS(checked_mul(kMax, 2), StatusCode::Overflow);
  LS_REQUIRE_STATUS(checked_mul(kMin, -1), StatusCode::Overflow);
  LS_CHECK_EQ(LS_REQUIRE_OK(checked_mul(kMin, 1)), kMin);
  LS_CHECK_EQ(LS_REQUIRE_OK(checked_mul(-3, 7)), std::int64_t{-21});
  LS_REQUIRE_STATUS(checked_mul_div(10, 1, 0, false), StatusCode::InvalidArgument);
  LS_CHECK_EQ(LS_REQUIRE_OK(checked_mul_div(1000000000000LL, 1000000, 1000000, true)),
              std::int64_t{1000000000000LL});
  LS_REQUIRE_STATUS(checked_mul_div(7, 1, 2, true), StatusCode::OutOfRange);
  LS_CHECK_EQ(LS_REQUIRE_OK(checked_mul_div(7, 1, 2, false)), std::int64_t{3});
  LS_CHECK_EQ(LS_REQUIRE_OK(checked_mul_div(-7, 1, 2, false)), std::int64_t{-3});
}

LS_TEST(core, power_scaling_and_differences) {
  auto scaled = scale_ppm(Power::from_watts(1000000), 500000);
  LS_CHECK(scaled.ok());
  LS_CHECK_EQ(scaled.value().watts(), std::int64_t{500000});
  auto truncated = scale_ppm(Power::from_watts(3), 333333);
  LS_CHECK(truncated.ok());
  LS_CHECK_EQ(truncated.value().watts(), std::int64_t{0});
  LS_REQUIRE_STATUS(scale_ppm(Power::from_watts(1), 2000000), StatusCode::OutOfRange);
  LS_CHECK_EQ(LS_REQUIRE_OK(positive_difference(Power::from_watts(5), Power::from_watts(9))).watts(),
              std::int64_t{0});
  LS_CHECK_EQ(LS_REQUIRE_OK(positive_difference(Power::from_watts(9), Power::from_watts(5))).watts(),
              std::int64_t{4});
  LS_CHECK_EQ(Power::from_watts(7).to_string(), std::string("7W"));
}

LS_TEST(core, power_addition_overflow_is_refused) {
  constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
  LS_REQUIRE_STATUS(checked_add(Power::from_watts(kMax), Power::from_watts(1)), StatusCode::Overflow);
  LS_REQUIRE_STATUS(checked_sub(Power::from_watts(std::numeric_limits<std::int64_t>::min()),
                                Power::from_watts(1)),
                    StatusCode::Overflow);
  // The documented power domain is enforced where evidence is validated, not by
  // the arithmetic itself.
  LS_CHECK_EQ(LS_REQUIRE_OK(checked_add(Power::from_watts(limits::kMaxPowerWatts),
                                        Power::from_watts(limits::kMaxPowerWatts)))
                  .watts(),
              std::int64_t{2 * limits::kMaxPowerWatts});
}

LS_TEST(core, power_parsing_is_strict) {
  LS_CHECK_EQ(LS_REQUIRE_OK(parse_power("0")).watts(), std::int64_t{0});
  LS_CHECK_EQ(LS_REQUIRE_OK(parse_power("123456")).watts(), std::int64_t{123456});
  LS_REQUIRE_STATUS(parse_power(""), StatusCode::InvalidArgument);
  LS_REQUIRE_STATUS(parse_power("-1"), StatusCode::InvalidArgument);
  LS_REQUIRE_STATUS(parse_power("+1"), StatusCode::InvalidArgument);
  LS_REQUIRE_STATUS(parse_power("1.5"), StatusCode::InvalidArgument);
  LS_REQUIRE_STATUS(parse_power("12a"), StatusCode::InvalidArgument);
  LS_REQUIRE_STATUS(parse_power("99999999999999999999"), StatusCode::OutOfRange);
}

LS_TEST(core, utf8_validation_is_strict) {
  LS_CHECK(is_valid_utf8("plain ascii"));
  LS_CHECK(is_valid_utf8("caf\xc3\xa9"));
  LS_CHECK(is_valid_utf8("\xe2\x82\xac"));
  LS_CHECK(is_valid_utf8("\xf0\x9f\x94\x8c"));
  // Overlong encodings of '/' and 'A'.
  LS_CHECK(!is_valid_utf8("\xc0\xaf"));
  LS_CHECK(!is_valid_utf8("\xc1\x81"));
  LS_CHECK(!is_valid_utf8("\xe0\x80\xaf"));
  LS_CHECK(!is_valid_utf8("\xf0\x80\x80\xaf"));
  // Surrogate halves.
  LS_CHECK(!is_valid_utf8("\xed\xa0\x80"));
  // Above U+10FFFF.
  LS_CHECK(!is_valid_utf8("\xf5\x80\x80\x80"));
  LS_CHECK(!is_valid_utf8("\xf4\x90\x80\x80"));
  // Truncated and stray continuation bytes.
  LS_CHECK(!is_valid_utf8("\xe2\x82"));
  LS_CHECK(!is_valid_utf8("\x80"));
  // U+0000 is a valid UTF-8 sequence; it is refused by the control-character
  // rule instead, which is a different check with a different purpose.
  LS_CHECK(is_valid_utf8(std::string("a\0b", 3)));
  LS_CHECK(!has_no_control_characters(std::string("a\0b", 3)));
  LS_CHECK(has_no_control_characters("caf\xc3\xa9"));
  LS_CHECK(!has_no_control_characters("a\nb"));
  LS_CHECK(!has_no_control_characters(std::string("a\x7f", 2)));
  LS_CHECK(!has_no_control_characters("\xc2\x85"));
}

LS_TEST(core, identifier_and_path_component_rules) {
  LS_CHECK(validate_identifier("load-1", 32, "id").ok());
  LS_REQUIRE_STATUS(validate_identifier("", 32, "id"), StatusCode::InvalidArgument);
  LS_REQUIRE_STATUS(validate_identifier(" padded", 32, "id"), StatusCode::InvalidArgument);
  LS_REQUIRE_STATUS(validate_identifier("padded ", 32, "id"), StatusCode::InvalidArgument);
  LS_REQUIRE_STATUS(validate_identifier("a/b", 32, "id"), StatusCode::InvalidArgument);
  LS_REQUIRE_STATUS(validate_identifier("a\\b", 32, "id"), StatusCode::InvalidArgument);
  LS_REQUIRE_STATUS(validate_identifier("a\tb", 32, "id"), StatusCode::InvalidArgument);
  LS_REQUIRE_STATUS(validate_identifier(std::string(33, 'x'), 32, "id"), StatusCode::Overlong);

  LS_CHECK(validate_relative_component("state-0001.lsg", "file").ok());
  LS_REQUIRE_STATUS(validate_relative_component("..", "file"), StatusCode::Rejected);
  LS_REQUIRE_STATUS(validate_relative_component(".", "file"), StatusCode::Rejected);
  LS_REQUIRE_STATUS(validate_relative_component("a/b", "file"), StatusCode::Rejected);
  LS_REQUIRE_STATUS(validate_relative_component("a\\b", "file"), StatusCode::Rejected);
  LS_REQUIRE_STATUS(validate_relative_component("c:name", "file"), StatusCode::Rejected);
  LS_REQUIRE_STATUS(validate_relative_component("trailing.", "file"), StatusCode::Rejected);
  LS_REQUIRE_STATUS(validate_relative_component("trailing ", "file"), StatusCode::Rejected);
  LS_REQUIRE_STATUS(validate_relative_component("CON", "file"), StatusCode::Rejected);
  LS_REQUIRE_STATUS(validate_relative_component("nul.txt", "file"), StatusCode::Rejected);
  LS_REQUIRE_STATUS(validate_relative_component("LPT1", "file"), StatusCode::Rejected);
  LS_CHECK(validate_relative_component("console", "file").ok());
  auto joined = join_relative_path("root", "child");
  LS_CHECK(joined.ok());
  LS_CHECK_EQ(joined.value(), std::string("root/child"));
  LS_REQUIRE_STATUS(join_relative_path("root", "../escape"), StatusCode::Rejected);
}

LS_TEST(core, json_round_trip_is_canonical) {
  auto parsed = parse_json("{\"b\":1,\"a\":[true,null,\"x\"],\"c\":{\"z\":-2}}");
  LS_CHECK(parsed.ok());
  const std::string canonical = to_canonical_json(parsed.value());
  LS_CHECK_EQ(canonical, std::string("{\"a\":[true,null,\"x\"],\"b\":1,\"c\":{\"z\":-2}}"));
  auto reparsed = parse_json(canonical);
  LS_CHECK(reparsed.ok());
  LS_CHECK_EQ(to_canonical_json(reparsed.value()), canonical);
  LS_CHECK_EQ(json_escape("a\"b\\c\nd"), std::string("\"a\\\"b\\\\c\\nd\""));
  LS_CHECK_EQ(json_escape(std::string("a\x01", 2) + "b"), std::string("\"a\\u0001b\""));
}

LS_TEST(core, json_rejects_malformed_documents) {
  const std::vector<std::string> malformed = {
      "",
      "{",
      "}",
      "[1,]",
      "{\"a\":1,}",
      "{\"a\":1,\"a\":2}",
      "{'a':1}",
      "{\"a\" 1}",
      "tru",
      "nulll",
      "01",
      "-",
      "1.",
      "1e5",
      "\"unterminated",
      "\"bad\\xescape\"",
      "\"lone \\ud800 surrogate\"",
      "\"tab\tinside\"",
      "[1,2] trailing",
      "{\"a\":NaN}",
      "\xff\xfe",
  };
  for (const std::string& text : malformed) {
    auto parsed = parse_json(text);
    LS_CHECK_MSG(!parsed.ok(), "accepted malformed JSON: " + text);
    LS_COUNT_ITERATION();
  }
}

LS_TEST(core, json_accepts_and_normalizes_valid_documents) {
  auto escapes = parse_json("\"\\u0041\\u00e9\\ud83d\\ude00\"");
  LS_CHECK(escapes.ok());
  LS_CHECK_EQ(escapes.value().as_string(), std::string("A\xc3\xa9\xf0\x9f\x98\x80"));
  auto numbers = parse_json("[0,-0,9223372036854775807,-9223372036854775808]");
  LS_CHECK(numbers.ok());
  LS_CHECK_EQ(numbers.value().items().size(), std::size_t{4});
  LS_CHECK_EQ(numbers.value().items()[2].as_int(), std::numeric_limits<std::int64_t>::max());
  LS_CHECK_EQ(numbers.value().items()[3].as_int(), std::numeric_limits<std::int64_t>::min());
  LS_REQUIRE_STATUS(parse_json("[9223372036854775808]"), StatusCode::Overflow);
  // Whitespace is insignificant; the canonical form drops it.
  auto spaced = parse_json("  {\n  \"a\" : [ 1 , 2 ]\n}\r\n");
  LS_CHECK(spaced.ok());
  LS_CHECK_EQ(to_canonical_json(spaced.value()), std::string("{\"a\":[1,2]}"));
}

LS_TEST(core, json_bounds_are_enforced_before_allocation) {
  JsonLimits limits;
  limits.max_depth = 2;
  LS_CHECK(!parse_json("[[[[1]]]]", limits).ok());
  LS_CHECK(parse_json("[[1]]", limits).ok());
  limits = JsonLimits{};
  limits.max_string_bytes = 4;
  LS_CHECK(!parse_json("\"12345\"", limits).ok());
  LS_CHECK(parse_json("\"1234\"", limits).ok());
  limits = JsonLimits{};
  limits.max_members = 2;
  LS_CHECK(!parse_json("[1,2,3]", limits).ok());
  LS_CHECK(!parse_json("{\"a\":1,\"b\":2,\"c\":3}", limits).ok());
  limits = JsonLimits{};
  limits.max_bytes = 8;
  LS_CHECK(!parse_json("[1,2,3,4,5]", limits).ok());
}

LS_TEST(core, json_typed_accessors_report_the_missing_member) {
  auto parsed = parse_json("{\"count\":5,\"name\":\"x\",\"flag\":true}");
  LS_CHECK(parsed.ok());
  LS_CHECK_EQ(LS_REQUIRE_OK(json_require_int(parsed.value(), "count")), std::int64_t{5});
  LS_CHECK_EQ(LS_REQUIRE_OK(json_require_uint(parsed.value(), "count")), std::uint64_t{5});
  LS_CHECK_EQ(LS_REQUIRE_OK(json_require_string(parsed.value(), "name")), std::string("x"));
  LS_CHECK(LS_REQUIRE_OK(json_require_bool(parsed.value(), "flag")));
  LS_REQUIRE_STATUS(json_require_int(parsed.value(), "missing"), StatusCode::InvalidArgument);
  LS_REQUIRE_STATUS(json_require_int(parsed.value(), "name"), StatusCode::InvalidArgument);
  LS_REQUIRE_STATUS(json_require_uint(parsed.value(), "name"), StatusCode::InvalidArgument);
  LS_CHECK_EQ(LS_REQUIRE_OK(json_optional_int(parsed.value(), "missing", 9)), std::int64_t{9});
}

LS_TEST(core, identifiers_round_trip_and_refuse_bad_input) {
  const PolicyGeneration policy = PolicyGeneration::from_value(12);
  LS_CHECK_EQ(policy.to_string(), std::string("policy-generation:12"));
  auto parsed = PolicyGeneration::parse("policy-generation:12");
  LS_CHECK(parsed.ok());
  LS_CHECK_EQ(parsed.value().value(), std::uint64_t{12});
  LS_REQUIRE_STATUS(PolicyGeneration::parse("evidence-generation:12"), StatusCode::InvalidArgument);
  LS_REQUIRE_STATUS(PolicyGeneration::parse("policy-generation:x"), StatusCode::InvalidArgument);
  LS_REQUIRE_STATUS(PolicyGeneration::parse("policy-generation:999999999999999999999"),
                    StatusCode::Overflow);
  LS_CHECK(!PolicyGeneration().is_set());
  LS_CHECK(PolicyGeneration::first().is_set());
  LS_CHECK_EQ(LS_REQUIRE_OK(PolicyGeneration::from_value(4).next()).value(), std::uint64_t{5});
  LS_REQUIRE_STATUS(PolicyGeneration::from_value(UINT64_MAX).next(), StatusCode::Overflow);

  auto load_ref = LoadRef::parse("chiller-1");
  LS_CHECK(load_ref.ok());
  LS_CHECK_EQ(load_ref.value().value(), std::string("chiller-1"));
  LS_REQUIRE_STATUS(LoadRef::parse(""), StatusCode::InvalidArgument);
  LS_REQUIRE_STATUS(LoadRef::parse("a b/../c"), StatusCode::InvalidArgument);

  const StageIndex first_stage = StageIndex::from_ordinal(0);
  LS_CHECK(first_stage.is_set());
  LS_CHECK_EQ(first_stage.value(), std::uint32_t{0});
  LS_CHECK(!StageIndex::unset().is_set());
  LS_CHECK(first_stage < StageIndex::from_ordinal(1));
  LS_CHECK(StageIndex::unset() < first_stage);
}
