# Validation record

Load Shedding 1.0.0. Everything below is a result that was produced by running the
commands shown, on the environment named. Nothing in this document is projected
from a design intent, and nothing is claimed that was not executed.

## Environment

| Item | Value |
| --- | --- |
| Operating system | Microsoft Windows 11 Pro |
| CPU | AMD Ryzen 7 9800X3D, 8 cores / 16 threads |
| Compiler | MSVC 19.44.35207 (Visual Studio 2022, 17.14) |
| CMake | 4.3.2 |
| Generator | Ninja |
| AddressSanitizer runtime | `clang_rt.asan_dynamic-x86_64.dll` from the same toolset |

## Build matrix

| Configuration | Flags | Result |
| --- | --- | --- |
| Release | `/W4 /permissive- /utf-8 /Zc:__cplusplus /WX` | builds clean; 17/17 CTest entries pass |
| Debug | same first-party warning set, `/RTC1`, iterator debugging | builds clean; 17/17 pass |
| AddressSanitizer | RelWithDebInfo + `/fsanitize=address` | builds clean; 17/17 pass |

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Seventeen CTest entries: twelve test suites (`core`, `policy`, `planning`,
`determinism`, `authority`, `store`, `crash`, `recovery`, `property`,
`concurrency`, `cli`, `hardening`), four examples, and `install_downstream`.
No test carries a timeout, and no test relies on one.

### Sanitizer evidence

AddressSanitizer is genuinely active in the sanitizer build: the test binary
imports `clang_rt.asan_dynamic-x86_64.dll`, and a positive control compiled with
`/fsanitize=address` and given a deliberate 4-byte heap overflow produced
`ERROR: AddressSanitizer: heap-buffer-overflow`. The sanitizer suite run is
therefore a real result and not a no-op.

No static-analysis substitute was needed: the sanitizer is available and was run.

## Proof-obligation suites

| Suite | Proof obligations exercised |
| --- | --- |
| `core` | SHA-256 against published vectors, digest hex round trips, checked-arithmetic overflow refusal at `INT64` boundaries, strict UTF-8 (overlong, surrogate, above U+10FFFF, truncated), identifier and path-component rules, and a strict JSON reader attacked with 21 malformed documents (duplicates, comments, trailing commas, lone surrogates, raw control characters, non-integer numbers, oversized documents, depth and member bounds) |
| `policy` | structural policy validation, stage exclusivity, contiguity, caps and bounds, content-digest independence from generation, JSON projection round trip, refusal of unknown members and bad values |
| `planning` | fully covered deficit, protected class / non-sheddable class / protected priority / active obligation never selected, emergency grant modelled and audited with obligations still absolute, stale evidence that never creates eligibility, every unknown-evidence state kept distinct from zero, out-of-service, unmapped priorities, stage power/ppm/load caps, no-overshoot mode, ordering rule and tie-breaks, minimum-on-time failing closed, total explanation trace, closure identity over several deficit shapes, ceiling verification |
| `determinism` | equal inputs produce identical plan content and identical canonical state across independent stores, permutation invariance, canonical state stable across reopen, replay returning the prior accepted result after the state moved on, idempotency conflict, bounded replay window, effect records that never rewrite a plan |
| `authority` | reader cannot mutate, second writer refused by the OS lock, epoch fencing and the idempotent same-epoch no-op, stale policy/evidence/effect/revision/tick refusals, fixed validation precedence, authority recorded and surviving reopen, revalidation verdicts, obligations bound to unknown loads and live obligations protecting a load from withdrawal |
| `store` | reopen adopting exactly the committed generation, clean audit, truncated/corrupt/oversized generation files refused, corrupt marker refused, marker without generation and generation without marker refused, unpublished generation and staging residue retired, rollback detection, retention bounds on every retained collection, retention-limit consistency, path safety (file where a directory is required, NUL byte, missing directory, invalid UTF-8, link substitution), audit history monotonicity |
| `crash` | crash injection at each of five durable stages in a real child process, exactly one whole state adopted afterwards, store clean and verified, refusal to guess when no marker exists, writer exclusion across processes, lock release and epoch advance after abrupt process death, only one of two contenders granted authority, store consistent after a terminated writer |
| `recovery` | confirmed shed evidence required for restoration, acknowledgement is not an effect, contradicted evidence reported, both documented recovery orders, headroom bounding and accounting, minimum-off time, recovery never rewriting its plan, stale references refused, effect record validation and ordering |
| `property` | 60 randomized facilities compared field by field against an independent reference model (selection, ordering, eligible capacity, selected total, residual, overshoot, protected amount, coverage status); 30 randomized permutation-invariance comparisons; an 80-step randomized state machine checking every plan invariant and the store-level verification |
| `concurrency` | four reader threads against a committing writer with a fixed iteration count, mutual exclusion between engines in one process, reader/writer exclusion, accessors returning values rather than references, independent engines not sharing state |
| `cli` | end-to-end lifecycle through the real binary, explanation trace totality, recovery and effect commands, policy and snapshot documents from files, refusal of malformed documents with named errors, exit-code contract, missing store and missing plan paths |
| `hardening` | forged but correctly framed state payloads (including recomputed SHA-256 and CRC-32C from an independent CRC implementation), one-past-the-bound framing, unsupported marker version, directory where a generation belongs, duplicate identities, over-bound load and obligation counts, unknown evidence carrying a value, evidence from the snapshot's future, backwards logical time, extreme power quantities, malformed identifiers, over-bound stage counts, read-only engine upgrading to writer authority, oversized CLI inputs, nested JSON, relative path components, empty directory handling, repeated open/close stability |

Total: 113 test cases across the twelve suites, all passing in Release, Debug and
AddressSanitizer configurations.

## Multiprocess and crash evidence

Crash and exclusion tests use a real helper executable and real child processes.

* Five durable crash points were injected in separate child processes. After each,
  the store reopened to exactly one whole state, either the baseline or the
  child's committed plan, never a mixture; `verify()` passed and the store audit
  was clean; the plan was present exactly for the crash points at or after the
  commit point and absent before it.
* A child holding writer authority made a second process's authority claim fail
  with `lock-conflict`. The holder was then terminated through the platform's
  process-termination primitive; the next process acquired authority with no
  cleanup step, at a strictly higher epoch, wrote successfully, and left a store
  that opened and verified.
* A store with a published generation and no commit marker was refused rather
  than guessed at.

## Install, export and downstream consumption

`ctest -R install_downstream` installs the project into a clean prefix,
configures `downstream/consumer` against that prefix with
`find_package(LoadShedding 1.0.0 REQUIRED CONFIG)`, builds it, and runs it. The
consumer installs a policy, installs evidence, plans a deficit, records an effect,
takes a recovery decision and runs `verify()`; it links only
`LoadShedding::load_shedding` and knows nothing about this build tree. Result:
pass, in every configuration.

## Examples

All four examples run to completion and exit zero:

* `example_lifecycle` — plan covers the deficit, effect records leave the plan
  digest unchanged, recovery restores in the documented order.
* `example_protected_and_unknown` — coverage `indeterminate`, requested
  120000 W, selected 40000 W, residual 80000 W, protected 150000 W, one unmeasured
  load, ceiling 40000 W.
* `example_stale_authority` — second writer `lock-conflict`, stale evidence
  `stale-evidence-generation`, replay returned the prior result with an unchanged
  digest, idempotency conflict, fenced epoch `stale-authority`.
* `example_persistence_recovery` — reader digest and plan digest match the
  committed generation, writer resumes at the same epoch, `verify` ok, store audit
  clean.

## Benchmark

`bench_plan` measures completed operations: a sample is a batch of
`Engine::plan` calls that each returned a committed plan, including validation,
classification, ordering, selection, accounting, invariant verification, canonical
encoding, the staging write, the device flush, read-back verification, the atomic
publish, the marker commit, the watermark advance and residue retirement.

Label: **REAL**. Measured on the host above with the Release build, five
repetitions per scale.

| Loads | Operations per repetition | Median sample | Completed plans/s | Per operation | Canonical state |
| --- | --- | --- | --- | --- | --- |
| 256 | 200 | 4577 ms | 43.7 | 22.9 ms | 326724 B |
| 1024 | 100 | 2758 ms | 36.3 | 27.6 ms | 695272 B |

The benchmark verifies `verify()` and the store audit before reporting, and
removes its store afterwards. No before/after pair is published: there is no
second implementation to compare against, and comparing across scales would not
isolate a changed variable.

## Deliberately not claimed

* No physical electrical equipment was used. Every effect record in tests and
  examples came from deterministic adapters or simulators and is labelled
  SYNTHETIC. **No hardware behaviour is proven.**
* POSIX and GCC/Clang branches exist in the source and are structurally sound, but
  they were not built or run in this environment. They are unverified.
* The differential comparison is against an independently written reference model
  of the documented rules, not against a third-party implementation.
