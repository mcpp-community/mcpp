# Compiler attempt lifetime and output recovery

Status: source implementation and focused acceptance in progress. This record
supplements the release candidate's compiler scheduling corrections. Native
source-group API checks do not qualify core or the released dependency group.

## Problem and responsibility

A compiler driver may start a descendant that continues writing after the
supervisor and directly bound driver have ended. Releasing a kernel permit
when its holder dies therefore does not prove that its compiler unit is quiet.
A separate failure occurs after early BMI publication: Ninja has recorded the
BMI edge while the object/status never completes, so a later build can enter
an orphan await instead of recompiling.

Core owns the attempt journal, concurrency reservation and recovery of its
outputs. OS owns declared units, sealed admission and observation through an
original native reference. OpenKal and the native backends own the kernel
identity and completion semantics. Plugins do not manage process units. The
core implementation uses those generic capabilities and does not inspect
native process identifiers, process groups or platform job handles.

## Runtime protocol

One build drive owns a generation and an actor barrier. Generation identity is
runtime state; it changes neither build identity nor Ninja command hashes.
Phase one, ordinary wrappers, supervisors and await helpers acquire actor
leases before their final owner check. Recovery waits for every actor, including
phase-one BMI settling and status writers, rather than only compiler permits.

Each attempt publishes a checksummed prepared record before modifying outputs.
It resolves parent directories in the original execution context and records
absolute BMI, object, dependency and status identities. A leaf symlink is not
resolved into an unrelated output target. A previous BMI is copied into a
protected baseline; its content identity is certified after the copy completes.
Separate immutable stage records avoid competing writers replacing one record.

A helper acquires a kernel slot, declares its execution unit, publishes that
unit and its logical slot reservation, and registers with the live owner. The
owner retains the original native observation reference before acknowledging
registration over existing directory-local blocking IPC. Only then may the
helper publish admission and start the compiler. Record, registration or ACK
failure does not start an unregistered compiler.

An unlocked kernel slot whose reservation remains occupied or unknown is not
available. A hard-killed supervisor therefore cannot admit a replacement while
its compiler descendant continues writing. The live owner's listener also
notices controller loss, so an orphan descendant holding Ninja's output pipe
does not leave the build waiting indefinitely for EOF.

After the direct compiler exits, retirement seals admission and establishes
that no external writer remains. On systems that permit leaving a unit, the
helper uses the build owner's full program identity as a continuation; the
short-lived BMI publisher is insufficient. A dead, reused or inaccessible
continuation, including a different native session, yields unknown. Retirement
does not grant termination or admission authority over the continuation unit.
The helper publishes its quiet-unit certificate before atomic status and
releases its logical reservation only after this barrier.

## Recovery and boundaries

Recovery follows the actor barrier and requires either a verified retirement
certificate, positive native emptiness, or proof that admission never occurred.
A successful status also needs matching output identities. An incomplete early
BMI publication is removed to force a new BMI edge; its verified prior baseline
is retained with a certificate for that retry's equivalence/failure handling.
A successful cached object carries a quiet completion proof so a later missing
object can invalidate the BMI without inferring unit death from leader absence.

Malformed records, invalid stage order, write refusal, occupied units and
unknown observation retain reservations and evidence. Recovery is repeatable
between file operations. If every original native reference holder is forcibly
lost before retirement can be certified, Windows may no longer identify the
original job. That case remains a bounded refusal and is not automatic recovery.
A C observation job has an observe/close caller precondition; the OS observer
class hides every native control path.

## Validation layers

The OpenKal actual-source API workflow
[42110324914](https://github.com/mcpplibs/openkal/actions/runs/42110324914)
and OS actual-source lifecycle workflow
[42110397318](https://github.com/mcpplibs/os/actions/runs/42110397318)
passed on Linux, macOS and Windows. They cover declared-unit registration,
blocking ACK failure/timeout, driver descendants, sealed retirement, retained
observation, an expired publisher, forged continuation birth and observer
control rejection. Linux additionally kills the continuation at native migration
entry and immediately after successful migration; both report unknown.

Each OS native run recorded two batches of eight paired startup samples. The
median paired ACK increments were 0.175 ms on Linux, 0.409 ms on macOS and
2.508 ms on Windows. Listener start/wake/close medians were 0.177, 0.289 and
0.721 ms respectively. All samples, including negative deltas and tail values,
are retained in the run artifacts. These measurements describe the source
consumer, rather than a complete build or core no-op benchmark.

Core acceptance includes checksummed records, baseline staging/corruption,
atomic status gaps, late failures, missing cached objects, repeated recovery,
write refusal and changed directory symlinks. The actual helper matrix also
covers supervisor-only hard loss with a still-writing descendant, a free kernel
slot with a retained logical reservation, owner cancellation and immediate
restart, phase-one state writers, and normal/quiet cached replay. Syscall-boundary
fault controls belong to the test driver and add no product fault hooks.

Final acceptance still requires the completed core fault matrix, actual GCC
mixed-cap/replay controls, updated user/spec documentation, the coherent public
OpenKal/backend/runtime/platform/OS version group and full core CI. Core receives
an independent ecosystem review after those checks and before merge/release.

## 2026-11-06 cached object admission closeout

A completed generation already leaves a quiet completion certificate beside
each module's object slot. A later missing object originally made cached await
remove its BMI and fail, requiring a second invocation. The corrected boundary
reuses those existing certificates under the configuration's object tree after
old owners, actors and units are quiet and before any new Ninja reader starts.
Only a certificate's own declared object is checked. A missing object invokes
the existing certified-baseline invalidation; Ninja then rebuilds in its normal
graph on that same invocation. There is no additional success index, source
dirty model, dry run or alternate compiler entry.

Certificate names are bound to their slot/object paths. Absolute BMI scope,
field values, checksums, regular certificate files and unchanged BMI parents
are checked before invalidation. Symlinked state does not authorize recovery.
The checksum detects accidental corruption; it is not authentication against
a user who can already replace the build tool and its outputs.

The transaction suite passes 19 cases. The actual fresh engine recovers one
and two deleted completed module objects in a single build and preserves the
following no-op. Actual CLI controls reject a corrupt certificate before Ninja
starts and reject a directory symlink without changing an unrelated artifact.
The implementation's 30 warm admission samples have medians of 0.057 ms for
two real certificates, 2.200 ms for the core object tree's 1908 entries without
these new certificates, and 1.089 ms for 200 format-correct synthetic records
with real object existence checks. The latter is a scale fixture, not evidence
that those synthetic records came from real retirements.

A separate paired protocol control preserves the actual GCC argument multiset,
cap two and Ninja twelve. Eight no-op pairs have a median paired difference of
-0.041 ms. Five small cold-build pairs have a median new/old ratio of 1.101 and
a median paired increase of 250 ms. The comparison includes the complete old
protocol/OS and the current source group, and its controlled compiler driver
sleeps 150 ms after real compilation. It does not isolate IPC, establish an
absence of build overhead, or extrapolate to large module workloads.

## 2026-11-06 process-visible publication scope

The recovery contract covers owner, supervisor and driver process loss while
the filesystem continues operating. It requires complete atomic visibility,
checked write/close outcomes and rejection of damaged records. It does not
promise persistence across power loss or a filesystem/storage service failure.
The OS library's existing durable `write_atomic` operation remains unchanged.

The runtime publisher creates an exclusive same-directory temporary, writes
and flushes the user-space stream, checks its explicit close, then uses the
existing generic replacing rename. Failed write/close/rename does not publish
the final name or acknowledge admission. It removes only the temporary it
created. This composition preserves process-loss visibility without flushing
each short-lived runtime record to storage.

A controlled libc close returns EIO after closing the actual temporary; the
publisher rejects the replacement, preserves the previous record and removes
its temporary. The transaction suite passes 20 functional cases plus one
subprocess-driver skip. The same candidate passes the actual lifecycle and
syscall-boundary fault controls, including occupied descendants, reservation
retention and restart.

A frozen current-protocol durable binary/driver is the comparison baseline.
Five paired small cold builds preserve source, actual GCC argument multiset,
cap two and Ninja twelve; the candidate/durable median ratio is 0.900, with a
median paired reduction of 232 ms. Eight no-op pairs have a median paired
reduction of 1.639 ms. These samples use blocking child waits, a timeout guard
and alternating order. The earlier timeout-based sampler rounded short waits
and its small no-op delta was within that measurement's noise.

A corresponding syscall control records 200 fsync calls and 554 ms of summed
call wall-time for the durable protocol, and zero fsync calls for the candidate.
The sampled process wall-times are 2.868 and 2.668 seconds. Summed syscall
wall-time includes concurrent threads and is not the build wall-time. These
small-workload results support this publication choice; they are not an
extrapolation to large module builds or a general benchmark of IPC.

The capacity counter also distinguishes a forcibly ended driver from live
activity. It records each original pid/birth pair, preserves entries whose
finally handler did not execute, and counts only the original live processes.
A deterministic cancellation control kills a real wrapper, retains that
history, then starts a command returning seven without inflating peak one.
The prior false failure and its dead-process/empty-reservation evidence remain
part of the experiment record.
