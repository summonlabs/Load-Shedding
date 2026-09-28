# Contributing to Load Shedding

Load Shedding is licensed under the Apache License 2.0. Contributions are accepted
under the same license.

## Licensing of contributions

By submitting a contribution to this repository you agree that your contribution is
provided under the terms of the Apache License 2.0, without additional terms or
conditions, as described in section 5 of that license. There is no Contributor
License Agreement (CLA) and no copyright assignment requirement. You retain the
copyright to your contribution.

Do not add copyright headers that attribute work to anyone other than the actual
author, and do not add `Co-authored-by` trailers or other attribution trailers to
commits in this repository.

## Building and testing

Requirements: CMake 3.21 or newer and a C++20 compiler. The primary exercised
platform is Windows with MSVC (Visual Studio 2022, toolset 19.44 or newer).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The build must be warning-free. First-party warnings are errors: MSVC builds with
`/W4 /WX /permissive-`, other compilers with `-Wall -Wextra -Wpedantic -Werror`.
Do not disable a warning globally to make a change compile; fix the defect or, when
a warning is genuinely wrong for a specific construct, suppress it narrowly at the
site with a comment that explains why.

Tests must not use timeouts. CTest timeouts, shell timeout wrappers, watchdog
processes, and process-limit wrappers are prohibited. A test that hangs is a defect
to diagnose, not to bound.

## Code quality requirements

* C++20, standard library only. A new third-party dependency needs a written
  justification, a packaging story, and a validation plan before it is accepted.
* Public headers live in `include/load_shedding/` and must stay usable by long-lived
  downstream infrastructure: strongly typed identities and generations, narrow
  mutation authority, deterministic outcomes, explicit errors, no implicit global
  state, no exceptions across the public API boundary.
* All authoritative arithmetic is checked integer arithmetic. Overflow is refused,
  never wrapped or silently clamped. Unknown evidence is never coerced to zero.
* Canonical serialization is byte-deterministic: no wall-clock values, no
  nondeterministic container iteration, no process-specific values, no random
  identifiers in canonical content.
* Every new behavior needs a test that would fail if the behavior regressed. Tests
  are proof obligations, not coverage targets.
* Determinism claims must be exercised by a test that runs the same logical input
  twice (or under permuted input order) and compares canonical bytes or digests.

## Safety posture

This library decides which facility loads *may* be shed. It does not actuate
equipment. Never describe a plan, an issued command, or a downstream acknowledgement
as a physical effect, and never let missing or stale evidence be promoted to
permission. Safety interlocks fail closed.
