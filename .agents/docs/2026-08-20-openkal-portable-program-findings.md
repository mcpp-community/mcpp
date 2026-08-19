# openkal 0.4: what one portable program found, and what it says about the method

This document records the second round of work on openkal: the writing of a
program above all eight interfaces, the two defects in the specification it
exposed within minutes, the three defects in continuous integration it exposed
alongside them, and the conclusions about method that follow. It continues
[`2026-08-20-openkal-implementation-plan.md`](2026-08-20-openkal-implementation-plan.md)
and [`2026-08-20-openkal-design.md`](2026-08-20-openkal-design.md).

## 1. What was published

| Repository | Version | Change |
| --- | --- | --- |
| `mcpplibs/openkal` | 0.4.0 | clauses 7.6 and 7.7; `examples/portable` |
| `mcpplibs/openkal-linux` | 0.4.0 | argv passed unaltered; an assertion that observes it; three CI defects |
| `mcpplibs/openkal-macos` | 0.2.0 | the same, and CI that had never run a step it appeared to run |
| `mcpplibs/openkal-libc` | 0.2.0 | its tests given an implementation; its example asserted against `wc` |
| `mcpplibs/mcpp-index` | PR #223 | four descriptors, merged and propagated |

No declaration changed, so `SURFACE.txt` is unchanged and an implementation
conforming to 0.3 exports exactly what 0.4 requires. What changed is behaviour
the earlier version left open.

## 2. The two defects in the specification

Both were found by writing one program against the specification and running it.
Neither was visible in the specification text, and neither would have been found
by reading it more carefully.

### 2.1 The argument vector (clause 7.6)

`kal_process_spawn` takes a path and an argument vector. Version 0.3 did not say
whether the vector includes the started program's own name. Both implementations
prepended the path, so a caller's `argv[0]` arrived at index 1.

The defect surfaced as a shell reporting `cannot open sh`: given
`argv = {"sh", "-c", "exit 0"}`, the prepending implementation delivered
`{"bin/sh", "sh", "-c", "exit 0"}`, and the shell read `sh` as a script to open.

The rule adopted is that the vector is complete and is passed unaltered. Three
reasons, in order of weight:

1. The two sides must agree. A started program reads its own name through
   `kal_env_arg(0)`, which does include it. A caller that did not supply it could
   not predict what the program would read.
2. The name a program observes as its own is behaviour on every environment that
   has an argument vector. The choice belongs to the caller.
3. It is what every adjacent interface does — `posix_spawn`, `fdio_spawn`, and
   the conventional form of `CreateProcess`. The outlier needed a reason and did
   not have one.

⭐ **Both implementations agreed, and their agreement was worth nothing.** They
were written by one author from one reading. A second implementation by the same
author tests less than a second implementation by another; this is the limit of
what the two can establish between them, and it is now recorded in the macOS
implementation's own history so that the limit is not mistaken for evidence.

### 2.2 Absence as an answer (clause 7.7)

`kal_fs_info` on a name that does not exist: version 0.3 declared
`kal_node_absent` and did not say when it is reported. The implementations
return `kal_ok` with `kind = kal_node_absent`. A third implementer could as
reasonably have returned an error, and a portable program cannot be written
against a point on which two conforming implementations may differ.

The rule adopted is that enquiry succeeds and reports absence, while opening the
same name reports `kal_err_not_found`. Enquiry and access are different
operations: a caller that asks what a name refers to has been answered when told
that it refers to nothing. It is the same distinction `openkal.env` already draws
between a variable that is absent and one whose value is empty.

⚠️ **The test I wrote first was wrong, and the implementation was right.** I
asserted that enquiry after removal fails. It succeeded, and my first reading was
that removal had not worked. The file was gone from the disk. Had I trusted the
assertion over the artefact, I would have "fixed" a correct implementation.

## 3. The three defects in continuous integration

All four pipelines were green before this round, on packages containing the two
defects above. They share one shape.

### 3.1 A fact stated twice, which drifted

`openkal-linux`'s workflow pinned `OPENKAL_VERSION: 0.2.0` while its manifest
named 0.3.0. The surface comparison therefore fetched the 0.2 list, and reported
the four `kal_time_*` names — which that implementation is *required* to export —
as unspecified. The step's own comment said the list was fetched "so that the
comparison has one source rather than a copy that can drift", and the version
selecting it was the copy that drifted.

The fix is to read it from the manifest, which is where it is declared.

### 3.2 An assertion naming a subset

Each implementation asserted that `conformance_stream` and `conformance_memory`
had run. Five suites were present. The assertion exists precisely because *a
suite that discovered nothing reports success* — and a hand-written list defeats
that purpose the moment a suite is added.

The list is now derived from `tests/*.cpp`. ⭐ The general form: **an assertion
about coverage must be derived from what exists, not from what existed.**

### 3.3 A step that had never run

Every implementation's final step ran an example and grepped for
`openkal: vectored writes unavailable` — a line from a 0.1-era optional
operation that no example prints. In `openkal-macos` and `openkal-libc` it also
named a directory the repository does not contain.

It had never passed, and nobody could have noticed: the step before it failed
first, so it was never reached. ⚠️ **A pipeline whose steps fail in order hides
every later defect, and reports the first as though it were the only one.**
`openkal-macos`'s failure was reported as an inability to start `/bin/bash` in a
working directory, which reads as an infrastructure fault rather than as a
missing example.

### 3.4 The one that only linking could find

`openkal-libc`'s three test suites all failed to link, naming sixteen undefined
operations. The package declares against the interface and links against no
implementation — correct for the package, wrong for its own tests, which run.

This is the first occasion on which clause 4.2's claim (that a missing
implementation is reported by the linker, late but legible) was tested by
something other than a demonstration built to test it. The claim held: the
diagnostic named the operations, and the fix was one manifest section.

## 4. The portable program

`openkal/examples/portable` exercises the eight interfaces and prints one line
per observation plus a count of those that did not hold. Each implementation's
continuous integration checks out the specification at the version its manifest
names and builds the program from there. **The program is fetched, never
copied** — a copy in each implementation is a copy that can diverge, and the
whole value of the program is that it does not.

Two properties were designed in after the first version had neither.

**It does not start itself.** The first version spawned its own executable with a
`--child` marker. The marker did not arrive — because of the argv defect the
program existed to find — and the program started itself without end. ⚠️ **A
conformance program must not have an unbounded failure mode, and least of all one
armed by the defect it is looking for.** It now starts a shell, and reports the
observation as unobservable if the environment supplies no directory to start it
from.

**It asserts in both directions.** The workflow requires the summary line *and*
`observations that did not hold: 0` *and* the absence of `NOT HELD`. Asserting
only that the program reported would pass for a program that reported failures.

## 5. Method

⭐ **The specification's silences are not visible in the specification.** Two
rules were missing. Reading the text, twice, over two rounds, found neither.
Writing one program and running it found both in under an hour. Clause 9 requires
a conformance procedure for this reason, and the requirement was under-served by
a suite that started programs without observing what they received.

⭐ **A test that does not observe the thing cannot detect the thing.** The suite
started `/bin/true` and read its status. `/bin/true` ignores its arguments, so
the same status was produced whether the vector arrived intact or shifted. The
replacement starts a shell, whose behaviour depends on the vector.

⚠️ **Green is a property of the assertions, not of the software.** Four
pipelines were green across two published packages containing an ABI defect, a
CI comparison against the wrong version, an assertion covering two of five
suites, and a step that had never executed.

⚠️ **The revert probe is what separates an assertion from a decoration.** The new
argv assertion was confirmed to fail against the previous behaviour before the
behaviour was changed. On restoring the fix it still failed — because `mv`
restored an older mtime and nothing rebuilt. Had I read that as "the fix does not
work", I would have chased a defect that was not there.

⚠️ **One edit, several places.** Writing the index descriptors, a regex that
matched the first version block updated one of three platform tables per file,
leaving `openkal-macos` advertising the new version for Linux and the old one for
macOS. It parsed, and it would have installed. The check that caught it enumerated
what the file *says* rather than what the edit *intended*.

## 6. What remains

| Matter | State |
| --- | --- |
| A third implementation by another author | the gate that governs everything beyond; unchanged, and now with a specification worth implementing against |
| `openkal.net`, `openkal.channel` | reserved; no program above the stack needs them yet |
| Symbol versioning | clause 8 protects the interface by prohibiting change; two rules were added by prohibition-compatible means this round, which does not prove the next will be |
| A shared conformance package | the portable program is now the shared artefact in practice; formalising it needs a way for a test package to be compiled against an implementation a third project chooses |
