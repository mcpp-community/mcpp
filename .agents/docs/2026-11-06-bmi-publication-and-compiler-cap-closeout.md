---
subject: build-progress-and-scheduling
status: active
---

# BMI publication, compiler capacity and active download budgets

Audience: maintainers closing the 2026.11.5.2 candidate and reviewing PR #889.
This record separates three failures exposed during candidate validation.
It does not qualify a diagnostic object substitution as a released binary.

## Failure boundaries

| Failure | Confirmed cause | Owner |
|---|---|---|
| An importer cannot find a BMI after its interface is touched | GCC unlinks the old final path before renaming the new BMI; an absent identity was accepted as publication | Core detached compilation |
| A build exceeds its requested compiler count | Extra Ninja slots serve publication and join edges, while ordinary compilers bypass the detached semaphore | Core Ninja scheduling |
| A newly activated download is immediately considered stalled | Its queued state was observed earlier; starting without a source attempt or byte change retained the queue-entry timestamp | Core provider progress tracking |

No compiler toolchain payload, plugin semantic or timeout constant is changed
by these corrections.

## BMI publication

GCC 16.1 tracing shows an `unlink(final)` immediately before
`rename(temporary, final)`. The observed interval was approximately 26
microseconds. A changed identity alone is therefore insufficient: publication
requires a present file with a changed identity. The adapter still preserves
the previous BMI for equivalence checking and failure recovery.

The causal control amplifies only that existing unlink interval to 200
milliseconds. The previous core reproduces the candidate failure: the middle
module cannot read the core module. Replacing only the detached-compilation
implementation makes the same real GCC workload succeed. A separate
deterministic publisher confirms the boundary: the old phase returns zero
while the BMI is absent; the corrected phase returns only after publication.

An exit status of zero without the required BMI is diagnosed as a failure.
The previous BMI is restored. E2E 234 retains its cold execution, graph-shape,
incremental build and stale-token assertions, and adds both publication cases.

## Shared compiler permits

The candidate generated `sched_cap = 8`, while its scheduling policy allows
48 Ninja slots. The BMI supervisor used that semaphore; ordinary C/C++
compilation and compiler-driven scanning did not. A full local candidate
build exceeded eight compiler processes and encountered memory-pressure
kills. Its interrupted output is not build qualification.

The selected correction retains one actual compiler budget. Under the
detached strategy, ordinary built-in compilation and scanning use an internal
argv executor holding the same permit until the real child exits. BMI
supervisors continue to hold their permit across early publication and code
generation. Join edges acquire no permit. A holder never waits for another
permit, so waiting joins cannot form a permit dependency cycle.

The internal executor introduces no user mode and no argument serialization.
It receives the compiler's actual argv, inherits the existing streams and
returns its status. Existing response files retain their original owner and
encoding. Ordinary completion and interrupt cleanup release a permit once;
release cannot remove a permit already acquired by another compiler.
Failure to create or acquire the semaphore is reported rather than followed
by an unbounded compile. A failed supervisor start releases its permit.

The mixed control uses a real generated Ninja graph and GCC 16.1. A driver
records active invocations and remains alive briefly after BMI publication,
amplifying overlap with ordinary C and C++ units. With a requested cap of
one, the previous graph reaches six active drivers. Corrected controls use
caps of one and two, retain the extra Ninja slots, and observe peaks of
exactly one and two. They also check no-op builds, ordinary compiler failure,
a module failure after BMI publication, subsequent recovery, ordinary
compiler cancellation, child termination and permit cleanup. E2E 234 retains
the full-set stale-token reclamation check.

These local controls use selected recompiled objects and an extracted copy of
the production dispatch branch. They establish causality and small-load
behavior. The complete current sources still require the ordinary CI build
and all required checks before release.

## Download activation and deadline reasons

The pinned xlings 2026.10.26.1 provider reports queued files in each batch
with `started = false`. Once a worker obtains a slot, `started` becomes true
before a source attempt or byte count necessarily changes. That transition
starts the active budget. Waiting in the queue consumes no active budget;
repeated samples of an already active file do not renew it. A byte increase
or source change retains its existing renewal semantics.

An isolated provider invocation retained raw NDJSON from one download stream.
Replaying the same 347 snapshots through the actual C++ provider modules gives:

| Module | Expired snapshots | First expiration |
|---|---:|---:|
| Previous state machine | 11 | 95.751 seconds |
| Corrected activation transition | 0 | none |

The first false expiration belongs to a queued entry activated at 95.751
seconds with source attempt zero. Its inherited age was 95.651 seconds.
With queue time excluded, the maximum active idle interval at the observed
snapshots is 7.735 seconds. The isolated network invocation reached its
150-second observer budget; it was stopped by the experiment, not by a
provider progress deadline, and does not establish a completed graphics run.

The failed candidate CI log contains rendered status rather than raw NDJSON.
Its timing is consistent with activation of the final queued entries. It
does not identify which raw file state first expired, and is not presented as
proof of a particular file's network behavior. Progress on an independent
download remains insufficient to renew a genuinely stalled file.

The provider source was followed through its HTTP implementation: the
connect and idle environment budgets are consumed below the downloader
wrapper. They remain eight and thirty seconds, respectively; the parent's
per-file budget remains forty-five seconds. No budget is increased to hide
the state error. Progress timeouts now identify the file, interval and source
attempt; process deadlines remain distinct. Time-injected unit cases cover
queue waits longer than forty-five seconds, genuine per-file stalls, source
fallback and completion.

## Qualification sequence

The local controls precede the complete PR CI. The graphics job is accepted
only after its shader, rendered-image and pixel assertions actually run.
After every required check succeeds, an independent ecosystem review
precedes merge and release. Published tags remain unchanged.

Plugin PR #52 has independently passed all six jobs against the published
2026.11.5.1 core. Its final public core pin and real subproject-sidecar fixture
follow ordinary installation of the new release, in the same plugin PR.

Public records contain synthetic assertions and aggregate evidence. Raw
provider streams, user logs, usernames and private paths remain outside the
repository and pull-request description.
