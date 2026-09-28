# Load Shedding store format

Format version **1**. This document is the specification the implementation is
checked against; the automated suite attacks every field described here.

## Directory layout

```
<store>/
  HEAD                              commit marker: the only authority
  WATERMARK                         highest generation ever committed (rollback boundary)
  LOCK                              cross-process writer lock
  LOCK.owner                        best-effort note of the current holder (not authority)
  state-<20 digits>.lsg             committed and retained generations
  state-<20 digits>.lsg.staging     publication residue, never authoritative
  HEAD.staging                      marker publication residue
  WATERMARK.staging                 watermark publication residue
```

Generation file names use exactly 20 zero-padded decimal digits. Names that do
not match are treated as stray files: reported by the audit, ignored by recovery.

## Encoding rules

* Every multi-byte integer is written **one byte at a time, least significant
  byte first**. The layout does not depend on host endianness or struct padding.
* Booleans are one byte, 0 or 1; any other value is a decode error.
* Enumerations are one byte and are validated against the highest value this
  build implements; an unknown value is `unsupported`, never coerced.
* Strings and byte strings are a 32-bit little-endian length followed by exactly
  that many bytes. The length is checked against the documented bound *before*
  anything is allocated. Text fields are additionally validated as UTF-8 without
  control characters.
* All lengths and counts are bounded; see `include/load_shedding/limits.hpp`.

## Generation container (`state-*.lsg`)

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 4 | magic `LSSG` |
| 4 | 4 | format version (currently 1) |
| 8 | 8 | payload length in bytes |
| 16 | N | payload (the canonical state, below) |
| 16+N | 32 | SHA-256 over bytes `[0, 16+N)` |

The file size must equal `16 + payload length + 32` exactly; trailing bytes and
short files are refused.

## Commit marker (`HEAD`)

Fixed size, 116 bytes, CRC-32C protected.

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 4 | magic `LSHD` |
| 4 | 4 | format version |
| 8 | 8 | committed generation |
| 16 | 32 | SHA-256 of the whole generation file it names |
| 48 | 8 | generation file length in bytes |
| 56 | 8 | authority epoch |
| 64 | 8 | incarnation |
| 72 | 8 | state revision |
| 80 | 8 | policy generation |
| 88 | 8 | evidence generation |
| 96 | 8 | effect generation |
| 104 | 8 | plan generation |
| 112 | 4 | CRC-32C of bytes `[0, 112)` |

## Rollback watermark (`WATERMARK`)

Fixed size, 20 bytes, CRC-32C protected.

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 4 | magic `LSWM` |
| 4 | 4 | format version |
| 8 | 8 | highest committed generation |
| 16 | 4 | CRC-32C of bytes `[0, 16)` |

On open, a marker whose generation is **below** the watermark is refused
(`corrupt`): the store will not adopt a rolled-back state. A marker at or above
the watermark is accepted, which is the ordering that makes a crash between the
marker commit and the watermark advance harmless.

## Canonical state payload

Fixed field order. Every collection is in canonical order, so the encoding is a
function of the logical value alone.

1. format version (u32)
2. state revision, policy generation, evidence generation, effect generation,
   plan generation, tick, authority epoch, incarnation (u64 each)
3. `next_sequence` (u64), `policy_installed` (bool)
4. policy: schema version, name, generation, mode, shedable classes, protected
   priorities, stages (index, name, priorities, optional absolute cap, optional
   ppm cap, optional load cap, emergency flag), evidence age window, evidence
   generation-lag window, recovery order, optional minimum-off and minimum-on
   ticks, emergency-override flag
5. facility snapshot: id, evidence generation, revision, tick, total demand
   evidence, loads sorted by reference, obligations sorted by reference
6. observed load states, sorted by load reference
7. retained plans, ascending plan id: identity, mode, coverage, closure, actions
   in selection order, explanation trace sorted by load reference, stage summary
8. retained recovery decisions, most recent first
9. retained effect records, ascending sequence
10. audit entries, ascending sequence
11. replay records, most recent first

The plan's own id, generation and attempt are part of the stored record but are
excluded from `content_digest()`, so the same decision taken twice has the same
digest. Domain separation strings (`load-shedding/plan-content/1` and friends)
prefix every digest input, so digests of different artifact kinds cannot collide.

## Publication protocol

```
 1. encode the whole state canonically
 2. write state-<next>.lsg.staging and flush it to the device
 3. read it back and verify its length and digest
 4. atomically rename it to state-<next>.lsg
 5. write HEAD.staging and flush it
 6. atomically replace HEAD                      <-- COMMIT POINT
 7. write WATERMARK.staging and flush it, then atomically replace WATERMARK
 8. remove staging residue and generations outside the retained window
```

Steps 1–5 are not authoritative. Step 6 is the commit point. A crash before step
6 leaves the previous generation authoritative; a crash after step 6 leaves the
new generation authoritative and the residue is cleaned up on the next open.

The retained window is two generations (`limits::kGenerationsRetained`).
Generation files newer than the marker, staging files, and generations older than
the window are removed on open.

## Recovery on open

| On-disk condition | Result |
| --- | --- |
| no marker, no published generation | empty store; staging residue removed |
| no marker, published generation present | `corrupt`: refuses to guess whether the generation was meant to be authoritative |
| marker malformed, wrong magic, wrong version, bad CRC, wrong size | `corrupt` / `unsupported` |
| named generation missing | `not-found` |
| generation size or digest does not match the marker | `corrupt` |
| generation decodes but fails structural validation | `corrupt` |
| marker below the watermark | `corrupt` (rollback) |
| unpublished generation or staging residue | removed, open succeeds |

Dynamic observations are never made fresh by recovery: the adopted generation
carries the evidence generations and ticks it recorded, and every freshness
decision is recomputed from them.

## Fault injection

`EngineOptions::crash_after` accepts `none`, `after-staging-write`,
`after-state-publish`, `before-head-commit`, `after-head-commit` and
`after-watermark`. When the named durable stage is reached during a mutation
performed through the engine, the process terminates immediately through the
platform's process-termination primitive: no unwinding, no `atexit` handlers, and
no interactive error-reporting path. The authority takeover that `open` performs
is never faulted.

## Path trust model

Before anything is opened:

* the path must be non-empty, at most 4096 bytes, valid UTF-8 and free of NUL
  bytes;
* the store directory and **every existing ancestor** is checked for symbolic
  link, junction or other reparse-point substitution;
* the checks run on the path exactly as given, so a substituted path cannot be
  normalized into an accepted one.

Once a store directory has passed those checks it is trusted: the library assumes
an attacker cannot write arbitrary files inside it. Files inside the store are
not separately checked for link substitution.
