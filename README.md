# Load Shedding

Deterministic facility-level load-shedding authority for the Data Center Control
Plane. Load Shedding 1.0.0 answers one question and answers it the same way every
time:

> Given a quantified facility power shortfall and current protected obligations,
> priorities, eligibility, topology/capacity evidence and policy, which loads may
> be shed in which deterministic stages, how much deficit remains, and in what
> order may service later be restored?

The runtime produces an **authority-bound shedding plan** — which loads may be
shed, in which stage, with what expected contribution, and exactly how much of the
requested reduction remains uncovered. It does not touch equipment. Selecting a
load is a decision; whether that load actually went off is downstream evidence
that arrives later, is recorded separately, and never rewrites the plan.

## Systems boundary

**Owned by this runtime**

* protected obligations and non-sheddable classes;
* priority/service classes and the explicit staged reduction policy;
* eligible-load determination, including unknown, unavailable, unsupported,
  denied, unsafe and stale evidence states;
* the deterministic selection rule, its ordering, and its stage caps;
* requested deficit reduction, selected expected reduction, residual deficit,
  overshoot, and the checked-integer accounting closure;
* recovery eligibility and recovery order as a decision distinct from shedding;
* generation/epoch/incarnation identity, plan identity, and plan auditing;
* durable, integrity-checked persistence of policy generations, plans, authority
  epochs, replay metadata, downstream effect records and audit history.

**Not owned by this runtime**

* PDU, UPS or generator actuation;
* Power Control Plane operating state;
* Feed Authority;
* Power Capacity derivation;
* workload scheduling inside ASI;
* DFI path scheduling;
* Energy Ledger accounting.

It may issue an authority-bound shedding plan or decision for downstream
controllers. It must never pretend that selecting a load means the electrical
effect occurred, and it never does: a plan, an acknowledgement, an observed
effect, and a verified effect are four separate things in this model.

## Architecture

```
include/load_shedding/       public headers (installed)
  status.hpp                 explicit status/result types, closed error set
  ids.hpp                    strongly typed identities and generations
  units.hpp                  exact integer power, checked arithmetic
  limits.hpp                 every externally influenced bound
  policy.hpp                 priority classes, stages, selection and recovery rules
  facility.hpp               load evidence, protected obligations, snapshots
  plan.hpp                   actions, explanation trace, accounting closure
  recovery.hpp               recovery decisions
  effect.hpp                 downstream effect records and observed state
  engine.hpp                 the single entry point
  store.hpp                  store format constants and inspection audit
  history.hpp                audit history
  json.hpp / json_io.hpp     strict JSON reader/writer and domain projection
src/                         implementation
  detail/                    internal: codec, planner, durable store, locks, I/O
tools/ls_cli.cpp             administration CLI (thin adapter over the library)
examples/                    runnable lifecycle and failure-path examples
bench/bench_plan.cpp         completed-operation benchmark
tests/                       proof-obligation suites and their support code
downstream/consumer/         independent find_package consumer
```

The decision core (`src/detail/planner.cpp`) is a pure function: no I/O, no
clock, no global state. The engine owns authority, persistence and idempotency
around it.

## Authority, generations and fencing

Nine counters are semantically distinct and are distinct types, so one cannot be
passed where another is expected:

| Type | Meaning |
| --- | --- |
| `RequestId` | client-chosen identity of a request, which is what makes a replay detectable |
| `AttemptId` | identity of a downstream actuation attempt |
| `PolicyGeneration` | generation of the installed shedding policy |
| `PolicyGeneration` content digest | identity of the policy *content*, independent of its generation |
| `EvidenceGeneration` | generation of the facility evidence set |
| `EffectGeneration` | generation of the downstream observed-state table |
| `StateRevision` | revision of configuration and evidence (decisions do not advance it) |
| `PlanId` / `PlanGeneration` | identity and generation of a committed plan |
| `AuthorityEpoch` / `Incarnation` | fencing epoch and writer identity |
| `Tick` | logical clock; the library never reads a wall clock |

* A mutation must state the authority epoch, incarnation, and every generation it
  was planned against. A stale claim is refused, never merged.
* Writer authority is a real operating-system lock (an exclusive file lock held
  for the engine's lifetime). A second writer is refused by the kernel, and the
  lock is released by the kernel when the holding process dies — including
  abrupt termination.
* A **different incarnation** fences the previous writer by advancing the
  authority epoch. The **same incarnation** resuming after its process ended
  keeps its epoch, so reopening a store is byte-stable.
* Every plan records the authority epoch and incarnation under which it was
  issued. Revalidation reports `authority-moved` when they no longer hold.
* Validation precedence is fixed and tested: request identity, then request
  quantities, then the emergency grant, then authority, then policy generation,
  then evidence generation, then effect generation, then revision, then tick.
  The same invalid request always produces the same primary error.

### Idempotency

The engine retains a bounded replay window (default 64 requests, most recent
first; retained plans are at least as many, so an accepted attempt inside the
window is always still resolvable).

* A retry of an accepted request returns the prior accepted result **before** any
  staleness check runs, so a lost response can be retried safely even after the
  state moved on.
* The same request identity with different content is `idempotency-conflict`,
  never a silent re-plan.
* A request identity that has left the window is refused rather than re-planned
  under the old identity.

## Decision model

**Priority classes** run from `critical` (rank 0) to `optional` (rank 5); lower
rank is more important.

**Stages** are ordered, and each priority class belongs to at most one stage. A
stage may carry an absolute power cap, a parts-per-million cap relative to the
facility's known sheddable demand, a load-count cap, and a flag marking it as
requiring emergency authority.

**Selection** is deterministic and documented, not an optimiser:

1. Classify every load. Exactly one verdict per load, in this precedence:
   active obligation → protected class → protected priority → out of service →
   evidence state → evidence generation lag → evidence age → negative → zero →
   minimum-on-time → class eligibility → stage availability.
2. Admissible loads are ordered by (least important priority first, then larger
   contribution first, then load reference ascending). The reference is unique,
   so the order is total; container iteration order never decides anything.
3. Stages run in order. Within a stage the plan selects while the request is not
   yet covered, the stage power cap and load cap allow it, and the load fits.
   `whole-load-greedy` may overshoot and reports the excess;
   `no-overshoot-greedy` never overshoots and reports the residual it leaves.
   Complexity is O(n log n) in the number of loads; there is no optimisation
   objective to prove because there is no optimiser.

**Protected and non-sheddable loads are never selected.** The single exception is
modeled and audited: a stage marked `requires_emergency_authority`, under a
policy that permits emergency override, under a request that carries an emergency
grant with a justification. Even then, a load named by an active protected
obligation stays unselectable: obligations are absolute. The grant, its epoch,
its justification, and every action it admitted are recorded in the plan and in
the audit history.

**Accounting closure** (checked integer arithmetic throughout, overflow refused):

| Field | Meaning |
| --- | --- |
| `requested_reduction` | what the caller asked for |
| `eligible_known_capacity` | sum of known contributions of every admissible load |
| `selected_expected_reduction` | sum of the selected actions' expected contributions |
| `protected_known_amount` | known contribution of loads protected by class, priority or obligation |
| `non_sheddable_known_amount` | known contribution policy will never shed |
| `unavailable_known_amount` | known contribution that was not usable (out of service, stale, …) |
| `residual_deficit` | `max(requested − selected, 0)` |
| `overshoot` | `max(selected − requested, 0)`, recorded and never hidden |
| `remaining_eligible_known_capacity` | known capacity left unselected |
| `capacity_indeterminate` | at least one otherwise-admissible load's contribution is not established |

The closure identity is checked on every plan and re-derived by `verify()`:

```
requested == selected + residual − overshoot
selected  <= eligible_known_capacity
```

**Unknown is not zero.** Evidence has seven distinct states. A value may only be
attached to `known`; validation refuses a non-zero quantity attached to any other
state, and refuses a quantity outside the documented power domain. When the
residual is non-zero the plan says which of four things is true:

| Coverage | Meaning |
| --- | --- |
| `fully-covered` | the residual is exactly zero |
| `indeterminate` | a non-zero residual remains and at least one admissible load's contribution is unmeasured, so the residual cannot be proven irreducible |
| `policy-limited` | a non-zero residual remains, no unmeasured capacity exists, and policy caps stopped selection while known capacity was still available |
| `insufficient` | a non-zero residual remains and no known eligible capacity does |

A plan that cannot cover the deficit states the residual. Nothing is invented to
balance it.

**Recovery** is a separate decision. A load becomes a restoration candidate only
when downstream evidence says it is *confirmed* off: an acknowledgement is not an
effect, and contradicted evidence is reported as such. Candidates are ordered
either `reverse-stage-then-priority` (later stages restore first, more important
loads first inside a stage) or `priority-then-reverse-stage`, both with the same
total tie-break. Restoration is bounded by the headroom the caller reports, and a
per-load minimum off time is honoured. Recovery never modifies the plan.

## Determinism

* Canonical encoding is fixed-width, explicit little-endian, and total: the same
  logical value always produces the same bytes.
* Canonical content contains no wall-clock reading, no random identifier, no
  memory address, no process-specific value and no container iteration order.
  Time is a caller-supplied logical tick.
* Equal inputs produce equal plan content digests, including under a permuted
  insertion order of loads. Both are asserted by tests and by the randomized
  property suite.
* `content_digest()` covers the decision content (context, actions, explanation
  trace, closure) and excludes the plan's own id, generation and attempt, so the
  same decision taken twice has the same digest.

## Persistence and recovery

State lives in a store directory as a sequence of whole generations plus a commit
marker. The protocol is:

```
encode whole state → write staging generation → flush → read back and verify digest
→ atomically publish generation → write staging marker → flush
→ atomically replace marker        <-- COMMIT POINT
→ advance the rollback watermark → retire generations outside the retained window
```

* Nothing before the commit point is authoritative.
* After a crash, reopening **adopts the generation named by the marker, or
  refuses**. Partial generations are never stitched together.
* A marker with no published generation, a published generation with no marker, a
  digest mismatch, a declared length beyond the bound, an unsupported format
  version, or a malformed/truncated file is refused with a named error.
* The rollback watermark makes the store refuse a marker that points *behind* the
  highest generation it has committed, which is the rollback boundary a store can
  enforce on its own.
* Recovery never makes dynamic observations fresh: adopting a generation restores
  exactly the evidence that generation recorded, with its original generations and
  ticks.
* Unpublished generations, staging files and stale generations are retired on
  open; the retained window is two generations.
* Store paths are validated against the documented trust model: the directory and
  every existing ancestor must not be a symbolic link, junction or other reparse
  point, and the path must be valid UTF-8, bounded and free of NUL bytes. Checks
  run on the path as given, before any normalization.

The on-disk layout is specified in [docs/store-format.md](docs/store-format.md).

## Building

Requirements: CMake 3.21 or newer and a C++20 compiler. The primary exercised
platform is Windows with MSVC (Visual Studio 2022, toolset 19.44).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Options: `LOAD_SHEDDING_BUILD_TESTS`, `LOAD_SHEDDING_BUILD_EXAMPLES`,
`LOAD_SHEDDING_BUILD_BENCHMARKS`, `LOAD_SHEDDING_BUILD_TOOLS`,
`LOAD_SHEDDING_WARNINGS_AS_ERRORS` (default ON),
`LOAD_SHEDDING_ENABLE_ASAN` (MSVC AddressSanitizer).

First-party warnings are errors: `/W4 /WX /permissive-` with MSVC,
`-Wall -Wextra -Wpedantic -Werror` elsewhere. The suite contains no timeouts and
no watchdog logic; a test that does not finish is a defect to diagnose.

## Command line tool

```sh
load-shedding --store ./facility init --default-policy
load-shedding --store ./facility --tick 1 load add --ref hall-lighting --contribution 25000 --priority optional
load-shedding --store ./facility --tick 2 load add --ref office-hvac --contribution 90000
load-shedding --store ./facility --tick 3 protected add --ref keep-hvac --load office-hvac --reserved 40000
load-shedding --store ./facility policy show --json
load-shedding --store ./facility stages
load-shedding --store ./facility plan --request 1 --deficit 110000 --tick 4 --json
load-shedding --store ./facility explain --plan 1 --verdict protected-priority
load-shedding --store ./facility plans
load-shedding --store ./facility plan diff --left 1 --right 2
load-shedding --store ./facility effect observe --request 2 --plan 1 --load hall-lighting \
    --state shed --source adapter --verification confirmed --tick 5
load-shedding --store ./facility recovery-order --request 3 --plan 1 --headroom 400000 --tick 9
load-shedding --store ./facility revalidate --plan 1
load-shedding --store ./facility verify
load-shedding --store ./facility history --limit 20
load-shedding --store ./facility store-audit
```

Commands: `init`, `policy show|install`, `stages`, `load add|list|remove`,
`protected add|list|remove`, `snapshot install|show`, `plan`,
`plan show|list|diff`, `explain`, `recovery-order`, `recovery show`,
`effect observe|list`, `observed`, `revalidate`, `verify`, `history`,
`store-audit`, `authority`, `version`, `help`.

Read-only commands take a shared lock and never take authority. Mutating commands
take writer authority. Exit codes: `0` success, `1` the command ran and the
operation was refused, `2` usage error. `--json` emits canonical JSON;
the default is indented JSON. `--file -` reads a document from standard input.

## Examples

Four runnable examples cover the lifecycle and the failure paths:

| Example | Shows |
| --- | --- |
| `example_lifecycle` | policy and evidence install, a fully covered staged shed, effect records that leave the plan digest unchanged, and recovery order once capacity returns |
| `example_protected_and_unknown` | protected obligations plus an unmeasured load: an `indeterminate` result with an explicit residual |
| `example_stale_authority` | second writer refused by the OS lock, stale evidence refused, replay of an accepted request returning the prior result, idempotency conflict, fenced epoch refused |
| `example_persistence_recovery` | close and reopen as reader and writer, identical canonical bytes, clean store audit |

Effect records in the examples come from a deterministic simulator and are
labelled SYNTHETIC. No example claims hardware behaviour.

## Consuming the package

```cmake
find_package(LoadShedding 1.0.0 REQUIRED CONFIG)
target_link_libraries(your_target PRIVATE LoadShedding::load_shedding)
```

```sh
cmake --install build --prefix /opt/load-shedding
cmake -S downstream/consumer -B consumer-build -DCMAKE_PREFIX_PATH=/opt/load-shedding
cmake --build consumer-build
```

`downstream/consumer` is an independent project: it knows nothing about this
build tree and finds the package only through `CMAKE_PREFIX_PATH`. The CTest
entry `install_downstream` performs exactly that install, configure, build and
run against a clean prefix.

## Validation

Release, Debug and AddressSanitizer builds are warning-free and green, on the
toolchain recorded in [docs/validation.md](docs/validation.md). The suites are
organized by proof obligation: core types, policy rules, selection and accounting,
determinism and idempotency, authority and staleness, persistence, crash and
multiprocess behaviour, recovery and effects, randomized property tests against an
independent reference model, concurrency, the command line tool, and a separate
adversarial hardening pass.

Crash semantics are exercised with real processes that terminate themselves at
each durable stage without unwinding, and writer exclusion, lock release on
process death and authority fencing are exercised across real process boundaries.
Store attacks include forged but correctly framed state, every declared-length
field one past its bound, damaged checksums, rollback, directory substitution and
link substitution.

## Benchmarks

`bench_plan` measures **completed operations**: one sample is a batch of
`Engine::plan` calls that returned a committed plan, including validation,
classification, ordering, selection, accounting, canonical encoding, the staging
write, the device flush, read-back verification, the atomic publish, the marker
commit, the watermark advance and residue retirement. Submission latency is not
measured, because it is not completion.

Measured results are recorded in [docs/validation.md](docs/validation.md) and
labelled REAL: they were produced by running the durable path on the host named
there. The benchmark verifies the resulting state and store audit before it
reports, and removes its store afterwards.

## Limitations

* The durable write path is the dominant cost of a plan (roughly 20–30 ms per
  committed plan on the measured host, dominated by the device flush). This is the
  price of the documented commit protocol, not a defect; a batch interface that
  commits several plans per generation is not implemented.
* Selection is greedy, not optimal. `whole-load-greedy` may overshoot;
  `no-overshoot-greedy` may leave a residual that a different packing could have
  covered. Both are reported exactly, and neither is claimed to be optimal.
* Retrieval of a plan or effect record beyond the retained window is
  `not-found`; the window is bounded by configuration.
* Rollback detection is bounded by the store's own watermark. An attacker who can
  restore both the marker and the watermark is outside the documented trust model.
* POSIX and GCC/Clang branches exist and are structurally sound, but only the
  Windows/MSVC configuration has been built and run. They are unverified.
* No physical electrical equipment was used. All actuation-adjacent evidence in
  the tests and examples is synthetic and labelled as such.
* The store directory is trusted once validated; the library does not defend
  against an attacker who can write arbitrary files inside it.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
