# Four gaps left by the ecosystem batch, and what to do about each

2026-09-04. The batch described in
`2026-09-04-named-runners-and-the-universal-command-surface.md` delivered four of
its six numbered batches in full. This note covers what is left: two items that
were in scope and were not finished, and two defects the batch surfaced and did
not close. Section 1 is the root-cause work for the fourth, because the plan for
it depends on the analysis.

Each section states what is ESTABLISHED by measurement separately from what is
INFERRED, because one of these four has a signature and no stack trace.

---

## 1. Gap 4 analysed: the ninja that spins with nothing to do

### 1.1 What is established

Observed three times, in three separate fresh sandboxes, always on the `mcpp run`
that follows a `mcpp build` in the same project, on `riscv64-none-elf` and
`aarch64-none-elf`:

| observation | reading |
|---|---|
| CPU | 99.9% of one core, sustained (utime 165168 ticks over 1629s elapsed) |
| child processes | zero |
| system time | zero |
| `/proc/<pid>/syscall` | empty — the process is not in a syscall |
| the graph | 8 edges, every declared output present, no missing input, no future mtime |
| the same project on the host | builds and runs in about two seconds |

A ninja that is neither running commands nor making system calls, on a graph
whose outputs all exist, is not working. It is looping in its own scheduler.

### 1.2 What that signature narrows it to

ninja's build loop is, in shape:

```
while (plan.more_to_do()) {
    edge = plan.FindWork();
    if (edge) { StartEdge(edge); continue; }
    if (commands_running) WaitForCommand();   // a syscall
    else break;
}
```

Zero children and zero system time exclude both branches that leave user space.
What remains is a loop in which `more_to_do()` stays true and `FindWork()` keeps
returning an edge whose completion never decrements the outstanding count. For
that to burn CPU without spawning anything, the edge must be one ninja finishes
WITHOUT running a command.

**In this graph exactly one edge finishes without a command: the phony.**

```
build _mcpp_staged_cache : phony obj/…/riscv_virt_rt.m.o pcm.cache/….pcm
build obj/main.cpp.ddi   : cxx_scan   …/src/main.cpp   || _mcpp_staged_cache
build obj/main.o         : cxx_object …/src/main.cpp | obj/main.cpp.ddi.dd \
                                                      || _mcpp_staged_cache
```

Its two inputs are `stage_file` edges, and `stage_file` is the one rule in mcpp's
generated manifest whose command is REGULARLY SKIPPED while succeeding:
`mcpp stage` does not write when the destination is already equivalent, which is
the case the rule's `restat = 1` exists to exploit — a skipped stage must not
dirty every importer of the staged BMI.

So the shape present here is: **restat edges whose outputs do not change, feeding
a phony, consumed as an order-only dependency.** That is the narrowest
description of the suspect, and every part of it is mcpp's own construction
rather than something a user wrote.

### 1.3 What is inferred and not proven

That this shape CAUSES the loop is inference, not measurement. Three attempts to
obtain a stack all failed, and the failure of each is itself informative:

* `gdb -p` is refused: `ptrace_scope = 1`, and the spinning ninja is not a
  descendant of any shell that can attach to it.
* `perf record -p` is refused: `perf_event_paranoid = 4`.
* Wrapping ninja so that gdb is its PARENT — which is legal under yama level 1 —
  makes the spin stop happening. Under gdb the same invocation ran seven
  subprocesses and exited normally.

The last of those is the important one. It says the trigger is timing- or
environment-sensitive rather than a pure function of the graph, which is
consistent with a scheduler bookkeeping race and inconsistent with "this manifest
always loops".

Two further measurements bound the suspect from the other side:

* Both ninja binaries, run 60 times each over the completed graph by hand, never
  hung and never took longer than 500ms. A no-op run of this manifest is not
  sufficient to trigger it.
* The same project, built and then run on the host, completes in two seconds,
  and its `build.ninja` carries the same two `stage_file` edges and the same
  phony. The graph shape alone is not sufficient either.

### 1.4 A separate defect found while looking, which is fully established

**mcpp does not kill the ninja it spawned.** Every `timeout`-terminated `mcpp
run` left an orphan spinning at 100% of a core. One of them outlived the removal
of the entire sandbox it belonged to; its working directory read `(deleted)` and
it was still burning a core half an hour later.

This is deterministic, has nothing to do with the spin's cause, and makes the
spin far more expensive than it would otherwise be: any CI that wraps mcpp in
`timeout` leaks a busy core per timeout.

### 1.5 A third finding, independent of both

`xim:ninja@1.12.1` does not name one artefact. The descriptor promises
xlings-res's glibc-static build (`ninja-1.12.1-linux-x86_64.tar.gz`); this host
has that one, at 2202320 bytes, statically linked. Several sandboxes have a
273768-byte dynamically linked binary built with GCC 4.8.5 on Red Hat — the
shape and build date (2024-05-11, ninja 1.12.1's release day) of upstream's
official `ninja-linux.zip`.

Both answer `--version` with `1.12.1`, and their SHA-256 differ. Neither hung in
the 120-run comparison above, so this is not offered as the cause. It is offered
as a fact that has to be false before any conclusion about "ninja 1.12.1
behaves like X" can be trusted.

---

## 2. Gap 1: openarch has no 32-bit machine with an address space

### 2.1 Why the discovery is still owed

The plan expected the first 32-bit backend to surface a width assumption:
`arch_pte_make_leaf` returns `arch_u64`, and nothing had ever asked whether that
is right on a machine whose page-table entry is 32 bits wide.

Cortex-M arrived and did not settle it. M-profile has an MPU and no MMU, so the
backend's `pte_impl.cpp` implements the group as functions that exist and refuse,
and the package withholds the `openarch:address-space` capability so that a
consumer needing an address space is refused at resolution rather than at run
time. That is the correct design for that machine, and it means the width
assumption was never executed.

The machine that settles it is 32-bit AND has paging: ARMv7-A.

### 2.2 Proposal

Add `backends/armv7a` implementing all four groups, with the pte group backed by
the ARMv7-A **short-descriptor** format, whose second-level entries are 32 bits.

The deliverable is not the backend. **The deliverable is the answer to one
question**: can a 32-bit page-table entry be expressed through an interface that
types it as `arch_u64`?

* If yes — the value fits, the extra bits are ignored, and no caller stores the
  return into a machine-width slot — then record that the interface is
  width-independent, with the ARMv7-A implementation as the evidence, and the
  question is closed rather than open.
* If no — some caller or some table needs the machine's own width — then the
  interface changes now, while openarch is at 0.x and four backends exist to
  change together, rather than after a fifth consumer has depended on it.

Either answer is worth the work; only the second changes the interface.

### 2.3 Shape of the work

* `backends/armv7a/` with `cpu_impl`, `context`, `trap_impl`, `pte_impl`.
* mcpp already has the target rows (`armv7a-none-eabi`, `armv7a-none-eabihf`),
  the matrix cells and e2e `336_armv7a_builds_and_boots.sh`, so the toolchain
  side needs nothing.
* qemu `-M virt -cpu cortex-a15` runs it; the existing e2e already boots an
  ARMv7-A image.
* Release openarch 0.9.0. The `arch_trap_switch` signature is already frozen by
  four implementations, so this backend implements a settled interface.

The one assertion that must exist: a test that builds a leaf entry, reads it
back, and compares against a hand-written 32-bit descriptor. Without it the
backend can be written to whatever the interface says and the width question
stays unasked a second time.

Independent of everything else here; blocks nothing.

---

## 3. Gap 2: two board packages provision an emulator they may not run

### 3.1 What is wrong

`riscv-virt-rt` and `aarch64-virt-rt` declare

```toml
[xlings.workspace]
"xim:qemu-riscv" = "9.2.4-1"
```

which is the untiered form, meaning `ToolWhen::Always`: provisioned by every verb
that builds. A CI job that compiles firmware and never runs it downloads a
33 MB emulator. `cortex-m-rt` declares the tiered form and does not.

The engine side of batch 2 shipped all four values (`build`, `run`, `dev`, and
the implicit `always`); only the adoption is incomplete, and it is incomplete in
the two packages that most obviously want `run`.

### 3.2 Proposal

```toml
[xlings.workspace]
"xim:qemu-riscv" = { version = "9.2.4-1", when = "run" }
```

Release `riscv-virt-rt 0.7.1` and `aarch64-virt-rt 0.2.1`, one index entry each.

The assertion goes in each package's CI and follows the criterion batch 2 already
settled (§14.4 of the plan): **test what would be installed, not what was
installed.** With `MCPP_NO_AUTO_INSTALL` set, a build must report that it needs
no emulator and a run must report that it needs exactly one. That runs in seconds
and needs no download, so it can sit in the ordinary build job rather than in a
job that has an emulator.

Both halves are load-bearing. "The build does not install qemu" is also true when
the package is broken and installs nothing at all; the pair distinguishes them.

Low risk, small, and it removes an inconsistency inside one batch.

---

## 4. Gap 3: `mcpp run` folds every non-zero exit status to 1

### 4.1 The current contract

Both spawn sites in `execute.cppm` end in `return rc == 0 ? 0 : 1`, and the
comment states the intent: 2 means "could not start", 1 means "ran and failed",
distinct from each other on purpose.

Measured consequences:

* Hosted: a program whose `main` returns 3 makes `mcpp run` exit 1.
* Freestanding: the same image under qemu directly exits 3; through `mcpp run`
  it exits 1.

So a program's own status is not observable through the command this ecosystem
tells people to type. That matters more after this batch than before it, because
the batch's claim is that running on a device is like running hosted, and a
hosted `run` that cannot report a status is not that.

### 4.2 Three options

**A. Pass the child's status through; move mcpp's own failures to 125–127.**
Borrows the convention `env`, `timeout` and `nice` already use and that shells
document: 125 = the tool itself failed, 126 = found but not executable, 127 = not
found. "Could not start" then lands on 126/127 by meaning rather than by
allocation, and every value below 125 belongs to the program.

Cost: the current 2 stops appearing from these two sites, which is a
compatibility change for anything that tests for it. Scripts testing `!= 0` are
unaffected.

**B. Pass the child's status through; keep 2 for mcpp's own failures.**
Smaller change, but a program that exits 2 becomes indistinguishable from a
launcher failure — reintroducing exactly the ambiguity the current code was
written to avoid.

**C. Keep the current contract; expose the real status in the machine
interface.** `mcpp run --format json` (or the existing NDJSON path) carries
`exit_code`, and the human exit stays 0/1/2.

Cost: the common case still cannot be scripted with `$?`, which is what people
actually do.

**Recommendation: A.** It is the only one of the three in which the obvious
thing a user types produces the obvious thing they expect, and the convention it
borrows is old enough that 126/127 will read correctly to anyone who meets them.

### 4.3 What A costs to implement

* Both sites in `execute.cppm`, plus `mcpp test`'s aggregation, which reports
  per-test status already and would report the true code.
* `docs/11-machine-output.md` and its Chinese counterpart: the exit-status table
  is part of the machine interface contract and must state the new allocation.
  A published contract page that describes the old one is worse than none.
* One e2e asserting a hosted program returning 3 gives 3, and one asserting a
  missing runner still gives 127 rather than 3.

**This is the one item in this note that needs a decision before work starts.**
The other three have a right answer; this one has a trade-off.

---

## 5. Gap 4: what to do about the spin

Split into two, because one half is established and the other is not.

### 5.1 Stage one — fix what is proven, now

**mcpp must terminate the ninja it spawned.** On POSIX, put the child in its own
process group and signal the group; on Windows, a job object with
`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` (the console-control-event route is already
known to be wrong here — it reaches the whole console, which once took down an
entire e2e runner with no `FAIL:` line to explain it).

The criterion is not "the code calls kill". It is: **after mcpp is terminated
mid-build, no ninja remains.** An e2e that starts a slow build, kills mcpp, waits,
and then asserts that no ninja process survives — asserting on process state, not
on a log line.

This is worth doing whatever causes the spin, and it converts the spin from
"leaks a core forever" into "one build hangs until its timeout".

### 5.2 Stage two — instrument, then decide

The one instrument that works on a process nobody can attach to is ninja's own
`-d explain`, which prints its dirtiness reasoning as it goes. A spinning ninja
that is re-deciding the same edge will emit a repeating line naming that edge,
which either confirms §1.2's suspect or names a different one.

Concretely: an `MCPP_NINJA_DEBUG` escape hatch that appends `-d explain` and
captures the output, used to re-run the verification until it hangs again. The
sandbox reproduces it about once per run, so a handful of runs suffices.

Second instrument, if the machine's owner is willing: `ptrace_scope = 0` on the
development box makes `gdb -p` work on the live spinner, which answers it in one
attempt.

### 5.3 The fix, if §1.2's suspect is confirmed

The phony exists to give consumers one order-only token to depend on instead of
listing every staged output. If it is implicated, the direct alternative is to
drop it and attach the staged outputs to each consumer's order-only list
directly. That costs a longer edge line per consumer and removes from the plan
the only edge that completes without a command.

Removing `restat = 1` from `stage_file` is the other obvious candidate and is
the WORSE one: the rule's comment states what restat buys — a skipped stage must
not dirty every importer of the staged BMI — and that benefit is real and
measured. Do not trade it away for a defect whose cause is not yet established.

### 5.4 Independent, and cheap

Make `xim:ninja@1.12.1` name one artefact. Whichever build is intended, the
descriptor and the payload should agree, and a machine that already has the other
one should be able to tell. A single-line assertion — the installed binary's
SHA-256 against the descriptor's — would have made §1.5 a report rather than a
discovery.

---

## 6. Order, and what needs an answer

| # | item | depends on | needs a decision |
|---|---|---|---|
| 5.1 | mcpp kills its ninja | nothing | no |
| 3 | two boards adopt `when = "run"` | nothing | no |
| 5.4 | one artefact per ninja version | nothing | no |
| 2 | openarch ARMv7-A backend | nothing | no |
| 4 | `mcpp run` exit status | — | **yes, before work starts** |
| 5.2 | instrument the spin | 5.1 landing first is convenient, not required | no |
| 5.3 | change the staging graph | 5.2 | not until 5.2 answers |

Everything except item 4 can start without further input. Item 4 is a
compatibility decision about a published contract, and the recommendation in
§4.2 is a recommendation rather than a conclusion.
