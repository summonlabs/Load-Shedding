// Authority, fencing, and stale-generation proof obligations.
//
// Every state-dependent mutation must state the authority and version it was
// planned against, and a stale claim must be refused rather than merged. The
// validation order is fixed so that the same invalid request always produces the
// same primary error.

#include <string>

#include "fixture.hpp"
#include "load_shedding/json_io.hpp"
#include "test_harness.hpp"

using namespace load_shedding;

namespace {

FacilitySnapshot simple_facility(std::uint64_t generation = 1) {
  return ls_test::FacilityBuilder()
      .tick(50)
      .evidence_generation(generation)
      .load("load-a", 120000, PriorityClass::Standard)
      .build();
}

}  // namespace

LS_TEST(authority, read_only_engines_cannot_mutate) {
  ls_test::ScratchDirectory directory{"authority"};
  {
    auto writer = ls_test::open_writer(directory.path());
    LS_CHECK(writer.ok());
    LS_REQUIRE_OK(ls_test::install_policy(writer.value(), make_default_policy()));
    LS_REQUIRE_OK(ls_test::install_snapshot(writer.value(), simple_facility()));
  }
  auto reader = ls_test::open_reader(directory.path());
  LS_CHECK(reader.ok());
  LS_CHECK(!reader.value().has_writer_authority());
  const AuthorityStatus status = reader.value().authority();
  LS_CHECK(!status.writer);
  LoadRecord load;
  load.ref = LoadRef::parse("new-load").value();
  load.contribution.state = EvidenceState::Known;
  load.contribution.value = Power::from_watts(1000);
  load.contribution.tick = Tick::from_value(60);
  LS_REQUIRE_STATUS(reader.value().upsert_load(load, status.epoch, status.incarnation),
                    StatusCode::AuthorityRequired);
  PlanRequest request = ls_test::plan_request(reader.value(), 1, 10000, 60);
  LS_REQUIRE_STATUS(reader.value().plan(request), StatusCode::AuthorityRequired);
}

LS_TEST(authority, a_second_writer_is_refused_by_the_operating_system_lock) {
  ls_test::ScratchDirectory directory{"authority"};
  auto first = ls_test::open_writer(directory.path());
  LS_CHECK(first.ok());
  auto second = ls_test::open_writer(directory.path());
  LS_REQUIRE_STATUS(second, StatusCode::LockConflict);
  // A read-only open is refused too: a reader must not observe a half-published
  // state while a writer is publishing.
  auto reader = ls_test::open_reader(directory.path());
  LS_REQUIRE_STATUS(reader, StatusCode::LockConflict);
  LS_REQUIRE_OK_STATUS(first.value().close());
  auto after_release = ls_test::open_writer(directory.path(), 2);
  LS_CHECK_MSG(after_release.ok(), after_release.status().to_string());
}

LS_TEST(authority, takeover_bumps_the_fencing_epoch_and_fences_the_previous_writer) {
  ls_test::ScratchDirectory directory{"authority"};
  auto engine = ls_test::open_writer(directory.path(), 1);
  LS_CHECK(engine.ok());
  const AuthorityEpoch first_epoch = engine.value().authority().epoch;
  LS_CHECK_EQ(first_epoch.value(), std::uint64_t{1});
  auto bumped = engine.value().take_authority(Incarnation::from_value(1),
                                              AuthorityEpoch::from_value(5));
  LS_CHECK(bumped.ok());
  LS_CHECK_EQ(bumped.value().value(), std::uint64_t{5});
  // A request that still claims the old epoch is fenced out.
  FacilitySnapshot facility = simple_facility();
  LS_REQUIRE_STATUS(engine.value().replace_snapshot(facility, first_epoch,
                                                    Incarnation::from_value(1)),
                    StatusCode::StaleAuthority);
  LS_REQUIRE_STATUS(engine.value().replace_snapshot(facility, AuthorityEpoch::from_value(5),
                                                    Incarnation::from_value(2)),
                    StatusCode::StaleAuthority);
  LS_REQUIRE_OK(engine.value().replace_snapshot(facility, AuthorityEpoch::from_value(5),
                                                Incarnation::from_value(1)));
  // Re-taking the authority this engine already holds is an idempotent no-op,
  // not a conflict.
  auto idempotent = engine.value().take_authority(Incarnation::from_value(1),
                                                  AuthorityEpoch::from_value(5));
  LS_CHECK(idempotent.ok());
  LS_CHECK_EQ(idempotent.value().value(), std::uint64_t{5});
  // Epochs only move forward.
  LS_REQUIRE_STATUS(engine.value().take_authority(Incarnation::from_value(1),
                                                  AuthorityEpoch::from_value(4)),
                    StatusCode::StaleAuthority);
  auto forward = engine.value().take_authority(Incarnation::from_value(2),
                                               AuthorityEpoch::from_value(6));
  LS_CHECK(forward.ok());
  LS_CHECK_EQ(engine.value().authority().incarnation.value(), std::uint64_t{2});
}

LS_TEST(authority, stale_generations_are_refused) {
  ls_test::ScratchDirectory directory{"authority"};
  auto engine = ls_test::open_writer(directory.path(), 1);
  LS_CHECK(engine.ok());
  LS_REQUIRE_OK(ls_test::install_policy(engine.value(), make_default_policy()));
  LS_REQUIRE_OK(ls_test::install_snapshot(engine.value(), simple_facility()));

  PlanRequest request = ls_test::plan_request(engine.value(), 1, 50000, 60);

  PlanRequest stale_policy = request;
  stale_policy.policy_generation = PolicyGeneration::from_value(99);
  LS_REQUIRE_STATUS(engine.value().plan(stale_policy), StatusCode::StalePolicyGeneration);

  PlanRequest stale_evidence = request;
  stale_evidence.evidence_generation = EvidenceGeneration::from_value(99);
  LS_REQUIRE_STATUS(engine.value().plan(stale_evidence), StatusCode::StaleEvidenceGeneration);

  PlanRequest stale_effect = request;
  stale_effect.effect_generation = EffectGeneration::from_value(99);
  LS_REQUIRE_STATUS(engine.value().plan(stale_effect), StatusCode::StaleEffectGeneration);

  PlanRequest stale_revision = request;
  stale_revision.base_revision = StateRevision::from_value(request.base_revision.value() + 3);
  LS_REQUIRE_STATUS(engine.value().plan(stale_revision), StatusCode::RevisionConflict);

  PlanRequest stale_tick = request;
  stale_tick.tick = Tick::from_value(1);
  LS_REQUIRE_STATUS(engine.value().plan(stale_tick), StatusCode::StaleTick);

  LS_CHECK(engine.value().plan(request).ok());
}

LS_TEST(authority, validation_precedence_is_fixed) {
  ls_test::ScratchDirectory directory{"authority"};
  auto engine = ls_test::open_writer(directory.path(), 1);
  LS_CHECK(engine.ok());
  LS_REQUIRE_OK(ls_test::install_policy(engine.value(), make_default_policy()));
  LS_REQUIRE_OK(ls_test::install_snapshot(engine.value(), simple_facility()));

  // A request that is wrong in several ways always reports the earliest check.
  PlanRequest request = ls_test::plan_request(engine.value(), 0, 50000, 60);
  request.policy_generation = PolicyGeneration::from_value(99);
  request.authority_epoch = AuthorityEpoch::from_value(99);
  LS_REQUIRE_STATUS(engine.value().plan(request), StatusCode::InvalidArgument);  // request id first

  request = ls_test::plan_request(engine.value(), 1, 0, 60);
  request.policy_generation = PolicyGeneration::from_value(99);
  request.authority_epoch = AuthorityEpoch::from_value(99);
  LS_REQUIRE_STATUS(engine.value().plan(request), StatusCode::InvalidArgument);  // then the deficit

  request = ls_test::plan_request(engine.value(), 1, 50000, 60);
  request.emergency_authority = true;
  request.policy_generation = PolicyGeneration::from_value(99);
  request.authority_epoch = AuthorityEpoch::from_value(99);
  LS_REQUIRE_STATUS(engine.value().plan(request), StatusCode::InvalidArgument);  // then the grant

  request = ls_test::plan_request(engine.value(), 1, 50000, 60);
  request.policy_generation = PolicyGeneration::from_value(99);
  request.authority_epoch = AuthorityEpoch::from_value(99);
  LS_REQUIRE_STATUS(engine.value().plan(request), StatusCode::StaleAuthority);  // authority next

  request = ls_test::plan_request(engine.value(), 1, 50000, 60);
  request.policy_generation = PolicyGeneration::from_value(99);
  request.evidence_generation = EvidenceGeneration::from_value(99);
  request.base_revision = StateRevision::from_value(999);
  LS_REQUIRE_STATUS(engine.value().plan(request), StatusCode::StalePolicyGeneration);

  request = ls_test::plan_request(engine.value(), 1, 50000, 60);
  request.evidence_generation = EvidenceGeneration::from_value(99);
  request.base_revision = StateRevision::from_value(999);
  LS_REQUIRE_STATUS(engine.value().plan(request), StatusCode::StaleEvidenceGeneration);

  request = ls_test::plan_request(engine.value(), 1, 50000, 60);
  request.base_revision = StateRevision::from_value(999);
  LS_REQUIRE_STATUS(engine.value().plan(request), StatusCode::RevisionConflict);

  // Repeating the identical invalid request produces the identical error.
  request = ls_test::plan_request(engine.value(), 2, 50000, 60);
  request.evidence_generation = EvidenceGeneration::from_value(77);
  const Status first = engine.value().plan(request).status();
  const Status second = engine.value().plan(request).status();
  LS_CHECK_EQ(first.code(), second.code());
  LS_CHECK_EQ(first.message(), second.message());
}

LS_TEST(authority, authority_takeover_is_recorded_and_survives_reopen) {
  ls_test::ScratchDirectory directory{"authority"};
  {
    auto engine = ls_test::open_writer(directory.path(), 3);
    LS_CHECK(engine.ok());
    LS_CHECK_EQ(engine.value().authority().epoch.value(), std::uint64_t{1});
    LS_CHECK_EQ(engine.value().authority().incarnation.value(), std::uint64_t{3});
  }
  auto second = ls_test::open_writer(directory.path(), 4);
  LS_CHECK_MSG(second.ok(), second.status().to_string());
  LS_CHECK_EQ(second.value().authority().epoch.value(), std::uint64_t{2});
  LS_CHECK_EQ(second.value().authority().incarnation.value(), std::uint64_t{4});
  auto page = second.value().history(HistoryQuery{0, 100, std::nullopt, std::nullopt});
  LS_CHECK(page.ok());
  std::size_t takeovers = 0;
  for (const AuditEntry& entry : page.value().entries) {
    if (entry.kind == AuditKind::AuthorityTaken) {
      ++takeovers;
    }
  }
  LS_CHECK_EQ(takeovers, std::size_t{2});
}

LS_TEST(authority, revalidation_distinguishes_every_supersession_reason) {
  ls_test::ScratchDirectory directory{"authority"};
  auto engine = ls_test::open_writer(directory.path(), 1);
  LS_CHECK(engine.ok());
  LS_REQUIRE_OK(ls_test::install_policy(engine.value(), make_default_policy()));
  LS_REQUIRE_OK(ls_test::install_snapshot(engine.value(), simple_facility()));
  auto outcome = engine.value().plan(ls_test::plan_request(engine.value(), 1, 50000, 60));
  LS_CHECK(outcome.ok());
  const PlanId plan_id = outcome.value().plan.identity.plan_id;

  auto current = engine.value().revalidate(plan_id);
  LS_CHECK(current.ok());
  LS_CHECK_EQ(current.value().verdict, RevalidationVerdict::Current);
  LS_CHECK_EQ(current.value().recorded_digest.hex(), current.value().recomputed_digest.hex());

  auto unknown = engine.value().revalidate(PlanId::from_value(9999));
  LS_CHECK(unknown.ok());
  LS_CHECK_EQ(unknown.value().verdict, RevalidationVerdict::PlanUnknown);

  // Any accepted mutation moves the revision, and evidence changes move the
  // evidence generation: the plan is no longer current for either reason.
  LS_REQUIRE_OK(ls_test::install_snapshot(
      engine.value(), ls_test::FacilityBuilder()
                          .tick(70)
                          .evidence_generation(2)
                          .load("load-a", 120000, PriorityClass::Standard)
                          .build()));
  auto superseded = engine.value().revalidate(plan_id);
  LS_CHECK(superseded.ok());
  LS_CHECK_EQ(superseded.value().verdict, RevalidationVerdict::SupersededEvidence);

  // A policy change is reported ahead of an evidence change.
  LS_REQUIRE_OK(ls_test::install_policy(engine.value(), make_default_policy()));
  LS_REQUIRE_OK(ls_test::install_snapshot(
      engine.value(), ls_test::FacilityBuilder()
                          .tick(90)
                          .evidence_generation(3)
                          .load("load-a", 120000, PriorityClass::Standard)
                          .build()));
  auto policy_moved = engine.value().revalidate(plan_id);
  LS_CHECK(policy_moved.ok());
  LS_CHECK_EQ(policy_moved.value().verdict, RevalidationVerdict::SupersededPolicy);

  // A fresh plan revalidates as current: committing a decision does not move the
  // configuration revision it was planned against.
  auto fresh = engine.value().plan(ls_test::plan_request(engine.value(), 2, 50000, 95));
  LS_CHECK(fresh.ok());
  auto fresh_report = engine.value().revalidate(fresh.value().plan.identity.plan_id);
  LS_CHECK(fresh_report.ok());
  LS_CHECK_EQ(fresh_report.value().verdict, RevalidationVerdict::Current);

  // Authority moving is reported ahead of everything except a digest mismatch.
  LS_REQUIRE_OK(engine.value().take_authority(Incarnation::from_value(9),
                                              AuthorityEpoch::from_value(50)));
  auto authority_moved = engine.value().revalidate(fresh.value().plan.identity.plan_id);
  LS_CHECK(authority_moved.ok());
  LS_CHECK_EQ(authority_moved.value().verdict, RevalidationVerdict::AuthorityMoved);
}

LS_TEST(authority, mutations_require_an_installed_policy_for_decisions) {
  ls_test::ScratchDirectory directory{"authority"};
  auto engine = ls_test::open_writer(directory.path(), 1);
  LS_CHECK(engine.ok());
  PlanRequest request = ls_test::plan_request(engine.value(), 1, 1000, 10);
  LS_REQUIRE_STATUS(engine.value().plan(request), StatusCode::PolicyViolation);
  LS_CHECK(!engine.value().policy_generation().is_set());
}

LS_TEST(authority, load_evidence_must_move_forward) {
  ls_test::ScratchDirectory directory{"authority"};
  auto engine = ls_test::open_writer(directory.path(), 1);
  LS_CHECK(engine.ok());
  const AuthorityStatus authority = engine.value().authority();
  LoadRecord load;
  load.ref = LoadRef::parse("load-a").value();
  load.contribution.state = EvidenceState::Known;
  load.contribution.value = Power::from_watts(5000);
  load.contribution.tick = Tick::from_value(20);
  LS_REQUIRE_OK(engine.value().upsert_load(load, authority.epoch, authority.incarnation));
  LoadRecord older = load;
  older.contribution.tick = Tick::from_value(10);
  LS_REQUIRE_STATUS(engine.value().upsert_load(older, authority.epoch, authority.incarnation),
                    StatusCode::StaleTick);
  LoadRecord older_but_current_tick = load;
  older_but_current_tick.contribution.tick = Tick::from_value(20);
  older_but_current_tick.contribution.value = Power::from_watts(6000);
  LS_CHECK(engine.value()
               .upsert_load(older_but_current_tick, authority.epoch, authority.incarnation)
               .ok());
}

LS_TEST(authority, obligations_bound_to_unknown_loads_and_live_obligations_are_refused) {
  ls_test::ScratchDirectory directory{"authority"};
  auto engine = ls_test::open_writer(directory.path(), 1);
  LS_CHECK(engine.ok());
  const AuthorityStatus authority = engine.value().authority();
  ProtectedObligation obligation;
  obligation.ref = ObligationRef::parse("obligation-1").value();
  obligation.load = LoadRef::parse("missing-load").value();
  obligation.reserved = Power::from_watts(1000);
  obligation.tick = Tick::from_value(60);
  LS_REQUIRE_STATUS(engine.value().upsert_obligation(obligation, authority.epoch,
                                                     authority.incarnation),
                    StatusCode::NotFound);

  LS_REQUIRE_STATUS(engine.value().remove_load(LoadRef::parse("load-a").value(), authority.epoch,
                                               authority.incarnation),
                    StatusCode::NotFound);
  LS_REQUIRE_OK(ls_test::install_snapshot(engine.value(), simple_facility()));
  ProtectedObligation bound = obligation;
  bound.load = LoadRef::parse("load-a").value();
  LS_REQUIRE_OK(engine.value().upsert_obligation(bound, authority.epoch, authority.incarnation));
  LS_REQUIRE_STATUS(engine.value().remove_load(LoadRef::parse("load-a").value(), authority.epoch,
                                               authority.incarnation),
                    StatusCode::ProtectedObligation);
  // Withdrawing the obligation releases the load.
  LS_REQUIRE_OK(engine.value().remove_obligation(bound.ref, authority.epoch,
                                                 authority.incarnation));
  LS_CHECK(engine.value().remove_load(LoadRef::parse("load-a").value(), authority.epoch,
                                      authority.incarnation)
               .ok());
}
