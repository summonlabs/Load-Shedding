// Adversarial hardening pass.
//
// Every test here attacks the implementation on purpose: forged but correctly
// framed storage, bounds that are one past the limit, contradictory inputs,
// byte-level damage in every declared-length field. The suite is deliberately
// separate from the proof-obligation suites so that "the hardening pass found
// nothing new" is a statement that can be read off the results.

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "fixture.hpp"
#include "load_shedding/json_io.hpp"
#include "proc.hpp"
#include "test_harness.hpp"

using namespace load_shedding;

namespace {

/// Independent CRC-32C (Castagnoli). Written from the polynomial definition so
/// that forging a commit marker does not depend on the library's own
/// implementation being right.
std::uint32_t crc32c_reference(const std::string& data) {
  std::uint32_t crc = 0xFFFFFFFFu;
  for (const char character : data) {
    crc ^= static_cast<std::uint8_t>(character);
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 1u) != 0u ? (crc >> 1) ^ 0x82F63B78u : crc >> 1;
    }
  }
  return crc ^ 0xFFFFFFFFu;
}

void append_u32(std::string& out, std::uint32_t value) {
  for (int index = 0; index < 4; ++index) {
    out.push_back(static_cast<char>((value >> (8 * index)) & 0xFFu));
  }
}

void append_u64(std::string& out, std::uint64_t value) {
  for (int index = 0; index < 8; ++index) {
    out.push_back(static_cast<char>((value >> (8 * index)) & 0xFFu));
  }
}

std::string store_path(const std::string& directory, const std::string& name) {
  return (std::filesystem::path(directory) / name).string();
}

std::string state_file_name(std::uint64_t generation) {
  std::string digits = std::to_string(generation);
  if (digits.size() < 20) {
    digits.insert(digits.begin(), 20 - digits.size(), '0');
  }
  return "state-" + digits + ".lsg";
}

/// Rewrites the committed generation with a well-framed container of the
/// attacker's choosing, and updates the commit marker so that the framing is
/// internally consistent. Only the payload is wrong.
load_shedding::Status forge_committed_payload(const std::string& store, std::uint64_t generation,
                                              const std::string& payload) {
  std::string container = "LSSG";
  append_u32(container, 1);
  append_u64(container, payload.size());
  container += payload;
  const Digest digest = Digest::of(container);
  container.append(reinterpret_cast<const char*>(digest.bytes().data()), digest.bytes().size());
  auto marker = ls_test::read_bytes(store_path(store, "HEAD"));
  if (!marker.ok()) {
    return marker.status();
  }
  std::string forged = marker.value();
  for (std::size_t index = 0; index < Digest::kBytes; ++index) {
    forged[16 + index] = static_cast<char>(digest.bytes()[index]);
  }
  for (int index = 0; index < 8; ++index) {
    forged[48 + index] = static_cast<char>((container.size() >> (8 * index)) & 0xFFu);
  }
  const std::uint32_t checksum = crc32c_reference(forged.substr(0, 112));
  for (int index = 0; index < 4; ++index) {
    forged[112 + index] = static_cast<char>((checksum >> (8 * index)) & 0xFFu);
  }
  const Status written = ls_test::write_bytes(store_path(store, state_file_name(generation)), container);
  if (!written.ok()) {
    return written;
  }
  return ls_test::write_bytes(store_path(store, "HEAD"), forged);
}

FacilitySnapshot small_facility(std::uint64_t generation, std::uint64_t tick) {
  return ls_test::FacilityBuilder()
      .tick(tick)
      .evidence_generation(generation)
      .load("load-a", 50000, PriorityClass::Standard)
      .build();
}

}  // namespace

LS_TEST(hardening, a_forged_but_well_framed_state_is_refused) {
  ls_test::ScratchDirectory scratch{"hardening"};
  const std::string store = scratch.child("store");
  const std::vector<std::string> payloads = {
      std::string(),            // empty payload
      std::string(64, '\0'),    // zeros
      std::string(16, '\xff'),   // invalid format version and enum values
  };
  std::size_t attack = 0;
  for (const std::string& payload : payloads) {
    LS_COUNT_ITERATION();
    // Each attack gets its own store: a store that has been attacked is left in
    // the state the attack produced.
    ++attack;
    const std::string attacked = scratch.child("store-" + std::to_string(attack));
    {
      auto engine = ls_test::open_writer(attacked, 1);
      LS_CHECK(engine.ok());
      LS_REQUIRE_OK(ls_test::install_policy(engine.value(), make_default_policy()));
    }
    std::uint64_t committed = 0;
    {
      auto engine = ls_test::open_writer(attacked, 1);
      LS_CHECK_MSG(engine.ok(), engine.status().to_string());
      committed = engine.value().commit().generation;
      LS_REQUIRE_OK_STATUS(engine.value().close());
    }
    LS_REQUIRE_OK_STATUS(forge_committed_payload(attacked, committed, payload));
    auto rejected = ls_test::open_writer(attacked, 2);
    LS_CHECK_MSG(!rejected.ok(), "forged payload of " + std::to_string(payload.size()) +
                                     " bytes was accepted");
    auto audit = inspect_store(attacked);
    LS_CHECK(audit.ok());
    LS_CHECK(!audit.value().clean);
  }
  (void)store;
}

LS_TEST(hardening, framing_bounds_are_enforced) {
  ls_test::ScratchDirectory scratch{"hardening"};
  const std::string store = scratch.child("store");
  {
    auto engine = ls_test::open_writer(store, 1);
    LS_CHECK(engine.ok());
    LS_REQUIRE_OK(ls_test::install_policy(engine.value(), make_default_policy()));
  }
  auto engine = ls_test::open_writer(store, 1);
  LS_CHECK(engine.ok());
  const std::uint64_t committed = engine.value().commit().generation;
  LS_REQUIRE_OK_STATUS(engine.value().close());
  auto original = ls_test::read_bytes(store_path(store, state_file_name(committed)));
  LS_CHECK(original.ok());

  // A declared payload far beyond the state bound is refused before allocation.
  std::string oversized = "LSSG";
  append_u32(oversized, 1);
  append_u64(oversized, 1ULL << 40);
  oversized += "payload";
  auto refused = ls_test::write_bytes(store_path(store, state_file_name(committed)), oversized);
  LS_REQUIRE_OK_STATUS(refused);
  auto reopened = ls_test::open_writer(store, 2);
  LS_CHECK(!reopened.ok());

  // An unsupported container version is refused.
  std::string versioned = original.value();
  versioned[4] = 9;
  LS_REQUIRE_OK_STATUS(ls_test::write_bytes(store_path(store, state_file_name(committed)), versioned));
  auto bad_version = ls_test::open_writer(store, 3);
  LS_CHECK(!bad_version.ok());
  LS_CHECK_EQ(bad_version.status().code(), StatusCode::Corrupt);

  // A one-byte file is refused.
  LS_REQUIRE_OK_STATUS(ls_test::write_bytes(store_path(store, state_file_name(committed)), "L"));
  auto tiny = ls_test::open_writer(store, 4);
  LS_CHECK(!tiny.ok());
  LS_CHECK_EQ(tiny.status().code(), StatusCode::Corrupt);
}

LS_TEST(hardening, an_unsupported_marker_version_is_refused) {
  ls_test::ScratchDirectory scratch{"hardening"};
  const std::string store = scratch.child("store");
  {
    auto engine = ls_test::open_writer(store, 1);
    LS_CHECK(engine.ok());
    LS_REQUIRE_OK(ls_test::install_policy(engine.value(), make_default_policy()));
  }
  auto marker = ls_test::read_bytes(store_path(store, "HEAD"));
  LS_CHECK(marker.ok());
  std::string forged = marker.value();
  forged[4] = 7;  // format version
  const std::uint32_t checksum = crc32c_reference(forged.substr(0, 112));
  for (int index = 0; index < 4; ++index) {
    forged[112 + index] = static_cast<char>((checksum >> (8 * index)) & 0xFFu);
  }
  LS_REQUIRE_OK_STATUS(ls_test::write_bytes(store_path(store, "HEAD"), forged));
  auto engine = ls_test::open_writer(store, 2);
  LS_CHECK(!engine.ok());
  LS_CHECK_EQ(engine.status().code(), StatusCode::Unsupported);
}

LS_TEST(hardening, a_directory_where_a_generation_belongs_is_refused) {
  ls_test::ScratchDirectory scratch{"hardening"};
  const std::string store = scratch.child("store");
  {
    auto engine = ls_test::open_writer(store, 1);
    LS_CHECK(engine.ok());
    LS_REQUIRE_OK(ls_test::install_policy(engine.value(), make_default_policy()));
  }
  auto engine = ls_test::open_writer(store, 1);
  LS_CHECK(engine.ok());
  const std::uint64_t committed = engine.value().commit().generation;
  LS_REQUIRE_OK_STATUS(engine.value().close());
  std::error_code error;
  std::filesystem::remove(store_path(store, state_file_name(committed)), error);
  std::filesystem::create_directory(store_path(store, state_file_name(committed)), error);
  LS_CHECK(!error);
  auto reopened = ls_test::open_writer(store, 2);
  LS_CHECK(!reopened.ok());
}

LS_TEST(hardening, evidence_documents_are_attacked_at_every_bound) {
  ls_test::ScratchDirectory scratch{"hardening"};
  const std::string store = scratch.child("store");
  auto engine = ls_test::open_writer(store, 1);
  LS_CHECK(engine.ok());
  const AuthorityStatus authority = engine.value().authority();

  // Duplicate identities.
  {
    FacilitySnapshot snapshot = ls_test::FacilityBuilder()
                                    .tick(10)
                                    .evidence_generation(1)
                                    .load("duplicate", 1000)
                                    .load("duplicate", 2000)
                                    .build();
    snapshot.loads.push_back(snapshot.loads.front());
    LS_REQUIRE_STATUS(engine.value().replace_snapshot(snapshot, authority.epoch, authority.incarnation),
                      StatusCode::DuplicateIdentity);
  }
  // More loads than the bound.
  {
    FacilitySnapshot snapshot;
    snapshot.id = SnapshotId::first();
    snapshot.generation = EvidenceGeneration::first();
    snapshot.tick = Tick::from_value(10);
    for (std::size_t index = 0; index <= limits::kMaxLoads; ++index) {
      LoadRecord load;
      load.ref = LoadRef::parse("load-" + std::to_string(index)).value();
      load.contribution.state = EvidenceState::Known;
      load.contribution.value = Power::from_watts(1000);
      load.contribution.generation = snapshot.generation;
      load.contribution.tick = snapshot.tick;
      snapshot.loads.push_back(load);
    }
    LS_REQUIRE_STATUS(engine.value().replace_snapshot(snapshot, authority.epoch, authority.incarnation),
                      StatusCode::Overlong);
  }
  // More obligations than the bound.
  {
    FacilitySnapshot snapshot = small_facility(1, 10);
    for (std::size_t index = 0; index <= limits::kMaxObligations; ++index) {
      ProtectedObligation obligation;
      obligation.ref = ObligationRef::parse("obligation-" + std::to_string(index)).value();
      obligation.reserved = Power::from_watts(1);
      obligation.generation = snapshot.generation;
      obligation.tick = snapshot.tick;
      snapshot.obligations.push_back(obligation);
    }
    LS_REQUIRE_STATUS(engine.value().replace_snapshot(snapshot, authority.epoch, authority.incarnation),
                      StatusCode::Overlong);
  }
  // Unknown evidence carrying a value is refused outright.
  {
    FacilitySnapshot snapshot = ls_test::FacilityBuilder().tick(10).evidence_generation(1).build();
    LoadRecord load;
    load.ref = LoadRef::parse("liar").value();
    load.contribution.state = EvidenceState::Unknown;
    load.contribution.value = Power::from_watts(5000);
    load.contribution.generation = snapshot.generation;
    load.contribution.tick = snapshot.tick;
    snapshot.loads.push_back(load);
    LS_REQUIRE_STATUS(engine.value().replace_snapshot(snapshot, authority.epoch, authority.incarnation),
                      StatusCode::Rejected);
  }
  // A load whose evidence is from the snapshot's future is refused.
  {
    FacilitySnapshot snapshot = small_facility(5, 10);
    snapshot.loads[0].contribution.generation = EvidenceGeneration::from_value(6);
    LS_REQUIRE_STATUS(engine.value().replace_snapshot(snapshot, authority.epoch, authority.incarnation),
                      StatusCode::StaleEvidenceGeneration);
  }
  // A snapshot that moves logical time backwards is refused.
  {
    LS_REQUIRE_OK(engine.value().replace_snapshot(small_facility(10, 100), authority.epoch,
                                                  authority.incarnation));
    LS_REQUIRE_STATUS(engine.value().replace_snapshot(small_facility(11, 50), authority.epoch,
                                                      authority.incarnation),
                      StatusCode::StaleTick);
    // The same evidence generation again is refused as stale.
    LS_REQUIRE_STATUS(engine.value().replace_snapshot(small_facility(10, 200), authority.epoch,
                                                      authority.incarnation),
                      StatusCode::StaleEvidenceGeneration);
  }
}

LS_TEST(hardening, extreme_power_quantities_are_refused) {
  ls_test::ScratchDirectory scratch{"hardening"};
  const std::string store = scratch.child("store");
  auto engine = ls_test::open_writer(store, 1);
  LS_CHECK(engine.ok());
  LS_REQUIRE_OK(ls_test::install_policy(engine.value(), make_default_policy()));
  LS_REQUIRE_OK(ls_test::install_snapshot(engine.value(), small_facility(1, 10)));

  const std::vector<std::int64_t> deficits = {limits::kMaxPowerWatts + 1, INT64_MAX, -1, 0};
  for (const std::int64_t deficit : deficits) {
    LS_COUNT_ITERATION();
    PlanRequest request = ls_test::plan_request(engine.value(), 100 + static_cast<std::uint64_t>(deficits.size()), deficit, 20);
    auto outcome = engine.value().plan(request);
    LS_CHECK(!outcome.ok());
    LS_CHECK(outcome.status().code() == StatusCode::OutOfRange ||
             outcome.status().code() == StatusCode::InvalidArgument);
  }
  // A power quantity beyond the documented domain cannot be stored either.
  LoadRecord load;
  load.ref = LoadRef::parse("huge").value();
  load.contribution.state = EvidenceState::Known;
  load.contribution.value = Power::from_watts(limits::kMaxPowerWatts + 1);
  load.contribution.tick = Tick::from_value(20);
  LS_REQUIRE_STATUS(engine.value().upsert_load(load, engine.value().authority().epoch,
                                               engine.value().authority().incarnation),
                    StatusCode::OutOfRange);
}

LS_TEST(hardening, malformed_identifiers_are_refused_at_the_door) {
  ls_test::ScratchDirectory scratch{"hardening"};
  const std::string store = scratch.child("store");
  auto engine = ls_test::open_writer(store, 1);
  LS_CHECK(engine.ok());
  const AuthorityStatus authority = engine.value().authority();

  const std::vector<std::string> bad_refs = {
      "", std::string(limits::kMaxLoadRefBytes + 1, 'x'), "with\ttab",   "slash/inside",
      "back\\slash", "..",                               ".",            std::string("\xff\xfe", 2),
      " leading",     "trailing "};
  // An internal space is unambiguous and is accepted: facility naming uses it.
  LS_CHECK(LoadRef::parse("chiller 1").ok());
  for (const std::string& ref : bad_refs) {
    LS_COUNT_ITERATION();
    auto parsed = LoadRef::parse(ref);
    LS_CHECK_MSG(!parsed.ok(), "accepted load reference '" + ref + "'");
    LoadRecord load;
    load.ref = LoadRef();
    load.contribution.state = EvidenceState::Known;
    load.contribution.value = Power::from_watts(100);
    load.contribution.tick = Tick::from_value(10);
    auto stored = engine.value().upsert_load(load, authority.epoch, authority.incarnation);
    LS_CHECK(!stored.ok());
  }
}

LS_TEST(hardening, a_policy_with_too_many_stages_is_refused) {
  SheddingPolicy policy = make_default_policy();
  policy.stages.clear();
  for (std::uint32_t ordinal = 0; ordinal <= limits::kMaxStages; ++ordinal) {
    StageDefinition stage;
    stage.index = StageIndex::from_ordinal(ordinal);
    stage.name = "stage-" + std::to_string(ordinal);
    stage.priorities = {static_cast<PriorityClass>(ordinal % 6)};
    policy.stages.push_back(stage);
  }
  LS_REQUIRE_STATUS(policy.validate(), StatusCode::Overlong);
  SheddingPolicy huge_priorities = make_default_policy();
  for (std::uint32_t index = 0; index <= limits::kMaxPrioritiesPerStage; ++index) {
    huge_priorities.stages[0].priorities.push_back(static_cast<PriorityClass>(index % 6));
  }
  LS_CHECK(!huge_priorities.validate().ok());
}

LS_TEST(hardening, a_read_only_engine_can_upgrade_to_writer_authority) {
  ls_test::ScratchDirectory scratch{"hardening"};
  const std::string store = scratch.child("store");
  {
    auto writer = ls_test::open_writer(store, 1);
    LS_CHECK(writer.ok());
    LS_REQUIRE_OK(ls_test::install_policy(writer.value(), make_default_policy()));
  }
  auto reader = ls_test::open_reader(store);
  LS_CHECK(reader.ok());
  LS_CHECK(!reader.value().has_writer_authority());
  const AuthorityEpoch before = reader.value().authority().epoch;
  auto upgraded = reader.value().take_authority(Incarnation::from_value(5),
                                                AuthorityEpoch::from_value(before.value() + 1));
  LS_CHECK_MSG(upgraded.ok(), upgraded.status().to_string());
  LS_CHECK(reader.value().has_writer_authority());
  LS_CHECK(reader.value().authority().epoch > before);
  // The upgraded engine can now mutate.
  LS_CHECK(reader.value()
               .replace_snapshot(small_facility(1, 10), reader.value().authority().epoch,
                                 reader.value().authority().incarnation)
               .ok());
}

LS_TEST(hardening, oversized_cli_inputs_are_refused_without_crashing) {
  if (ls_test::cli_executable().empty()) {
    return;
  }
  ls_test::ScratchDirectory scratch{"hardening"};
  const std::string store = scratch.child("store");
  auto run = [&scratch](const std::vector<std::string>& arguments) {
    return ls_test::run_process(ls_test::cli_executable(), arguments, scratch.path());
  };
  auto init = run({"--store", store, "init", "--default-policy", "--quiet"});
  LS_CHECK(init.ok());

  auto huge_deficit = run({"--store", store, "plan", "--request", "1", "--deficit",
                           "999999999999999999999999", "--json"});
  LS_CHECK(huge_deficit.ok());
  LS_CHECK_NE(huge_deficit.value().exit_code, 0);

  auto huge_ref = run({"--store", store, "--tick", "5", "load", "add", "--ref",
                       std::string(600, 'r'), "--contribution", "1000", "--json"});
  LS_CHECK(huge_ref.ok());
  LS_CHECK_NE(huge_ref.value().exit_code, 0);

  // A deeply nested JSON document is refused by the depth bound.
  std::string nested;
  for (int index = 0; index < 200; ++index) {
    nested += "[";
  }
  LS_REQUIRE_OK_STATUS(ls_test::write_bytes(scratch.child("nested.json"), nested));
  auto nested_policy = run({"--store", store, "policy", "install", "--file",
                            scratch.child("nested.json"), "--json"});
  LS_CHECK(nested_policy.ok());
  LS_CHECK_NE(nested_policy.value().exit_code, 0);

  // A path with a relative component is resolved by the operating system, and
  // the library never rewrites it: the same store is reached either way.
  const std::string indirect = store + "/../" + std::filesystem::path(store).filename().string();
  auto direct_audit = run({"--store", store, "store-audit", "--json"});
  auto indirect_audit = run({"--store", indirect, "store-audit", "--json"});
  LS_CHECK(direct_audit.ok());
  LS_CHECK(indirect_audit.ok());
  LS_CHECK_EQ(direct_audit.value().exit_code, 0);
  LS_CHECK_EQ(indirect_audit.value().exit_code, 0);
  auto direct_json = parse_json(direct_audit.value().standard_output);
  auto indirect_json = parse_json(indirect_audit.value().standard_output);
  LS_CHECK(direct_json.ok());
  LS_CHECK(indirect_json.ok());
  if (direct_json.ok() && indirect_json.ok()) {
    // The echoed path differs textually; the committed state must not.
    LS_CHECK_EQ(direct_json.value().find("commit")->find("state_digest")->as_string(),
                indirect_json.value().find("commit")->find("state_digest")->as_string());
    LS_CHECK_EQ(direct_json.value().find("commit")->find("generation")->as_int(),
                indirect_json.value().find("commit")->find("generation")->as_int());
  }
}

LS_TEST(hardening, an_empty_directory_is_not_a_store_but_is_inspectable) {
  ls_test::ScratchDirectory scratch{"hardening"};
  const std::string empty = scratch.child("empty");
  std::error_code error;
  std::filesystem::create_directories(empty, error);
  auto audit = inspect_store(empty);
  LS_CHECK(audit.ok());
  LS_CHECK(!audit.value().clean);
  LS_CHECK(!audit.value().commit_marker_present);
  LS_CHECK(!audit.value().findings.empty());
  // Opening it read-only adopts an empty store; a writer then creates the first
  // generation. The reader must be released first: writer authority excludes
  // readers by design.
  {
    auto reader = ls_test::open_reader(empty);
    LS_CHECK_MSG(reader.ok(), reader.status().to_string());
    LS_CHECK_EQ(reader.value().commit().generation, std::uint64_t{0});
  }
  auto writer = ls_test::open_writer(empty, 1);
  LS_CHECK_MSG(writer.ok(), writer.status().to_string());
}

LS_TEST(hardening, repeated_open_and_close_is_stable) {
  ls_test::ScratchDirectory scratch{"hardening"};
  const std::string store = scratch.child("store");
  {
    auto engine = ls_test::open_writer(store, 1);
    LS_CHECK(engine.ok());
    LS_REQUIRE_OK(ls_test::install_policy(engine.value(), make_default_policy()));
    LS_REQUIRE_OK(ls_test::install_snapshot(engine.value(), small_facility(1, 10)));
  }
  Digest digest;
  for (int round = 0; round < 6; ++round) {
    LS_COUNT_ITERATION();
    auto engine = ls_test::open_writer(store, 1);
    LS_CHECK_MSG(engine.ok(), engine.status().to_string());
    const Digest current = engine.value().state_digest();
    if (round == 0) {
      digest = current;
    } else {
      LS_CHECK_EQ(current.hex(), digest.hex());
    }
    auto report = engine.value().verify();
    LS_CHECK(report.ok());
    LS_CHECK(report.value().ok);
    LS_REQUIRE_OK_STATUS(engine.value().close());
    // Closing twice is harmless.
    LS_REQUIRE_OK_STATUS(engine.value().close());
  }
  auto audit = inspect_store(store);
  LS_CHECK(audit.ok());
  LS_CHECK(audit.value().clean);
}
