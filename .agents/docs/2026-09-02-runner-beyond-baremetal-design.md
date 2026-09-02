# The runner beyond bare metal: design

Date: 2026-09-02. Status: proposal, revised after self-review, awaiting review.
Issue: #544. Code references are against `origin/main` at aeba151 and were
re-verified line by line for this revision.

## 0. Decisions for review

The first draft of this document was reviewed against the code, the index,
the kernel, and this repository's recorded history. Six positions changed or
need a decision. Each is stated with what the draft said, what the review
found, and the recommendation the rest of this document is written to. The
alternative is recorded in section 9 so that flipping a decision is an edit to
one section, not a rewrite.

**D1. A declared runner whose program cannot be started is an error. The
draft fell back to direct execution with a warning.** The fallback contradicts
the draft's own rejection of `when = "cannot-execute"` (section 9): on a host
with binfmt_misc registered, falling back runs the artifact under the kernel's
interpreter without the declared arguments, which is the "runs and misbehaves"
failure the draft used to reject the alternative. The draft's claim that the
fallback is "safe by construction because it reaches `ENOEXEC` immediately" is
false on the very host section 2 was measured on. `ENOENT` from `posix_spawnp`
also means "installed but its interpreter or loader is missing", not only
"absent", so the fallback would run the artifact directly past a broken runner.
Recommendation: no fallback; principle P2 of the draft is withdrawn and
replaced (section 3). The one case the fallback served, a triple that is native
on this host, gets an explicit escape instead (D3).

**D2. `mcpp test` exits 2 when any test was not run. The draft exited 0.** The
freestanding path already exits 2 for exactly this situation, a target this
host cannot execute with no runner to stand in front of it
(`src/build/execute.cppm:1866`), so the draft gave one situation two exit codes
depending on the triple's `os` field. This repository's own record is that
skip-exits-zero is how `# requires: llvm` tests never ran in CI while every
job stayed green, and CI reads exit codes, not summary lines. Recommendation:
`NotRun` is reported truthfully in the summary and in JSON, and the exit code
is 2 whenever the count is non-zero. `--require-runner` disappears; no
relaxation flag is proposed until someone asks for one with a use.

**D3. `--no-runner` on `mcpp run` and `mcpp test`.** "This host can execute
the artifact" is a fact about the host, and the manifest has no host axis:
`[target.<triple>]` is keyed by target and `[xlings] deps` has no key at all.
A runner written for x86_64 developers is therefore also consulted on an
aarch64 host resolving the same triple, where the emulator is pointless and,
for the index's `qemu-user-aarch64`, uninstallable. The draft handled that host
by guessing (D1). The replacement is a flag the operator on that host passes,
which is the only place the fact is known. Recommendation: adopt; it is small
and it is what keeps P1 true without P2.

**D4. Provisioning the runner's package through `[xlings] deps` is a shape
for single-host-class projects, and the document must say so.** Two facts the
draft did not have: `[xlings] deps` has no conditional form
(`modules/manifest/src/toml.cppm:1350-1361`), and a package that cannot be
installed on this host is a hard build error, `provisioning [xlings] deps
failed` (`src/build/prepare.cppm:3462-3466`). The index's `qemu-user-aarch64`
declares `archs = {"x86_64"}`. A project that lists it therefore cannot be
built on an aarch64 or macOS host at all, not even with `mcpp build`. The
draft's "two keys that already exist" example is right for a CI matrix on one
host class and wrong as the general recommendation. The resolution rule of
section 4.4, which looks in the declared packages' `bin/` before `PATH`, is
kept because of a third fact: a bare name on `PATH` resolves to an xvm shim
that dispatches against the current subos, and both e2e 130 in CI and this
session (`python3` on `PATH` answering "not installed in this subos") show
that lookup failing with the package installed. Recommendation: keep the rule,
change the recommended shape in the documentation, state the consequence.

**D5. `runner = ""` is deferred, and the spelling when needed is `[]`.** The
draft's reason for `""` over `[]` does not survive inspection: both spellings
are hard errors on every released mcpp, so neither is "misread" by an older
version, and the draft's distinction was between two error messages. `[]` is
the empty value of the key's own type, as `sysroot = ""` is of its type. The
feature has no producer: no published package emits `mcpp:runner=` for a
hosted triple (the only emitters in the local registry are e2e fixtures 131
and 132, both freestanding). Recommendation: record the spelling, do not
implement until a dependency supplies a runner for a hosted target.

**D6. Scope beyond the issue.** Three items the design adds are listed so they
can be cut individually: the unknown-key sweep learning about array-valued
keys (issue ask 3, second half); `docs/05` moving `runner` out from under
"Bare metal"; and `docs/11` gaining the `mcpp test --message-format json`
record, which the machine-output contract page does not document today at
all, so the new `not_run` status would otherwise be a change to an
undocumented stream. Recommendation: include all three; the third is the one
most reasonably deferred to its own change.

**D7. `[xlings]` values can be given per host platform, in the form xlings
already defines.** Decided on review of D4 (2026-09-02). xlings' `.xlings.json`
resolves a `workspace` value that is an object keyed by platform against the
host it runs on (`src/core/xvm/db.cppm:375-421` in the xlings repository:
keys `linux`, `macosx`, `windows`, `default`; no match and no default means
the entry is absent on that host). mcpp's `[xlings.workspace]` parser accepted
only strings and dropped a table value in silence (`toml.cppm:1356-1358`), so
the form the section claims to mirror 1:1 was not mirrored. `deps` has no
xlings-side semantics (mcpp reads the list and calls `install_packages`), so
its per-platform form is mcpp's to define, and it takes the same one: an
entry is a string or a `{ <platform> = "<package>" }` table. Both keys are
resolved against the host when the manifest is loaded, which keeps every
downstream reader (the provisioning pass, its stamp, the build-program
hand-off) on a flat list. Platform keys are `linux`, `macos`, `windows` and
`default`, with `macosx` accepted as xlings' own spelling; an unknown key is a
hard error rather than a dropped entry. The axis is the host OS only; a
package that exists for the OS but not the architecture (`qemu-user-aarch64`
on aarch64 Linux) still fails provisioning loudly, and `--no-runner` remains
the answer on that host. Section 4.4's recommended CI form becomes:

```toml
[xlings]
deps = [{ linux = "qemu-user-aarch64" }]

[target.aarch64-linux-musl]
runner = ["qemu-aarch64-static"]
```

## 1. What the defect is

`[target.<triple>].runner` is parsed, type-checked and validated for every
triple a manifest names, and consulted for exactly one class of them.
`choose_runner` returns an empty template before reading either producer unless
the target is freestanding:

```cpp
// src/build/execute.cppm:435
RunnerChoice choose_runner(const BuildContext& ctx) {
    RunnerChoice c;
    auto ft = mcpp::toolchain::triple::parse(ctx.tc.targetTriple);
    if (!ft || !ft->is_freestanding()) return c;      // :438
    ...
    c.tmpl = ctx.manifest.buildConfig.runner;         // :445  build.mcpp channel
    if (auto it = ctx.manifest.targetOverrides.find(ctx.tc.targetTriple);
        it != ctx.manifest.targetOverrides.end() && !it->second.runner.empty()) {
        c.tmpl = it->second.runner;                   // :448  manifest channel
```

`is_freestanding()` is `os == "none"` (`modules/toolchain-model/src/triple.cppm:132`).
Both channels a runner can arrive by, the manifest key and a build program's
`mcpp:runner=`, are therefore bypassed before either is read, on every hosted
target. `mcpp run` gates on the same predicate at `execute.cppm:1272-1273` and
`mcpp test` at `:1861-1862`.

The manifest layer, by contrast, treats the key as load-bearing on every triple.
`runner = []` is a hard error naming the triple (`modules/manifest/src/toml.cppm:1910-1913`),
so the value's shape is checked where the value will never be read. Two smaller
inconsistencies accompany it. The unsupported-key warning for `[target.<triple>]`
lists `cxx_runtime, linkage, sysroot, toolchain` and omits `runner`; the sweep
skips arrays on purpose (`toml.cppm:1941-1946`, whose comment records that an
earlier version reported `runner` as "unsupported (ignored)" while honouring
it), so an array-valued typo is silent. And `docs/05-mcpp-toml.md` documents
the key only inside section 2.7.2, "Bare metal", while its own section 2.7.1
table says an exact triple "also carries `toolchain` / `linkage`" and not
`runner`.

Two failures compound the scope question and are independent of it.

1. **The two launch paths disagree about a spawn failure.** `run_exec`
   converts any `posix_spawnp` failure into a bare 127 with nothing printed
   (`modules/platform/src/process.cppm:566-567`). Its sibling `capture_exec`
   formats the same condition through `spawn_failure(argv.front(), sp)`
   (`:616-620`, helper at `:392`), and that text lands in the test's
   `runOutput`. `mcpp test` captures when there is more than one test or when
   `--message-format json` is passed, and streams otherwise
   (`execute.cppm:1713`). The reporter had exactly one test, so they saw the
   silent path; a second test file would have produced
   `posix_spawnp('…') failed (error 8): Exec format error` in the failures
   block. The symptom in #544 depends on the number of tests, which is the
   shape this repository records as "the loud failure has a silent twin".
2. **The bounded launcher states the invariant that this violates, and then
   defeats it.** On POSIX it returns `supported = false` when the child could
   not be spawned (`modules/platform/src/unix/bounded_process.cppm:227-230`),
   and its declaration says why: "'Could not spawn' and 'ran and failed' must
   not share an exit code, so the caller falls back rather than reporting a
   failure" (`:60-62`). Both callers fall back by spawning again:
   `run_exec_deadline` into `run_exec` (`process.cppm:746`) and
   `capture_exec_deadline` into `capture_exec` (`:857`). The first attempt's
   errno is discarded, the child is spawned twice, and on the untimed path the
   second attempt swallows what the first one knew.

## 2. Measured facts

The design rests on what the kernel reports, so the readings were taken before
the design was written and repeated for this revision. The probe spawns four
targets through `posix_spawnp` on the host used for this work (x86_64 Linux,
glibc, binfmt_misc populated by qemu-user):

| Case | `posix_spawn` result | Child ran |
|---|---|---|
| ELF with `e_machine = EM_AARCH64`, binfmt_misc registered | returns 0, child exits 255 | yes, under qemu |
| ELF with `e_machine = 0xffff`, no binfmt entry matches | returns 8, `ENOEXEC` | no |
| A program name absent from `PATH` | returns 2, `ENOENT` | no |
| A host binary | returns 0, child exits 0 | yes |

The artifact in the first row was synthesised by patching `e_machine` on a host
binary, so the 255 is qemu's own error code rather than a program's; that
coincidence is itself a measurement, and section 10 returns to it.

Three consequences follow, and each is load-bearing below.

**The host's ability to execute a foreign-ISA artifact is not a function of the
triple.** This machine registers several dozen architectures under
`/proc/sys/fs/binfmt_misc/`; `qemu-aarch64` carries flags `PO`. On it,
`mcpp run --target aarch64-linux-musl` starts successfully. On the machine that
reported #544 the same command yields `Exec format error`. The two hosts
disagree about the same target, and the deciding state is a kernel table that
neither mcpp nor the manifest can observe. The reverse asymmetry also holds: a
dynamically linked `aarch64-linux-musl` artifact need not run on an aarch64
glibc host. Equality of architecture is neither necessary nor sufficient.

**A failed spawn has no side effects.** glibc reports the exec failure through
the return value and the child never runs. The design below does not attempt
a second launch, but the property still matters: a spawn refused with
`ENOEXEC` leaves nothing behind, so a `mcpp test` invocation that stops after
the first refusal has not half-run anything.

**`ENOEXEC` and `ENOENT` are distinguishable at the point of failure, and
`ENOENT` is ambiguous.** `ENOEXEC` means the kernel refused to load the
artifact. `ENOENT` means the program named in `argv[0]` could not be started,
which covers three situations: it is not on the search path; it is a script
whose `#!` interpreter is missing; it is a dynamic executable whose ELF
interpreter is missing. Only the first is "not installed". The design
separates the first from the other two by doing its own lookup before
spawning (section 4.4), and reports the other two verbatim.

Four further facts were established for this revision, and each one changed
the design:

- **The index package is `qemu-user-aarch64`, and its program is
  `qemu-aarch64-static`** (`pkgs/q/qemu-user-aarch64.lua`: `programs =
  {"qemu-aarch64-static"}`, installed to `<payload>/bin/`, registered as an
  xvm shim of the same name). The draft's `deps = ["qemu-user"]` and
  `runner = ["qemu-aarch64"]` named a package and a program the index does
  not have. The package is `archs = {"x86_64"}` by design: on an aarch64 host
  an aarch64 binary just runs.
- **A bare name on `PATH` resolves to a shim that answers for the current
  subos, not for where the package is installed.** In this session, `python3`
  on `PATH` answered `python3 is not installed in this subos (_) — installed
  elsewhere 3.13.12`. e2e 130 records the same for `qemu-system-riscv64` in
  CI (`tests/e2e/130_freestanding_riscv_build_and_run.sh:44-51`) and works
  around it by resolving the emulator to an absolute path under
  `xpkgs/*-x-qemu-riscv/*/bin`.
- **`[xlings] deps` is unconditional and its provisioning failure is fatal.**
  The key has no `cfg()` or per-triple form (`toml.cppm:1350-1361`). A package
  xlings cannot resolve or install ends the build with `provisioning [xlings]
  deps failed: …` (`prepare.cppm:3462-3466`); that path was made to read the
  result in #531 precisely so that a declaration cannot look accepted and do
  nothing.
- **Tests run concurrently.** `run_tests_now` uses
  `min(runJobs, N)` workers when capturing (`execute.cppm:1711-1717`). A rule
  phrased as "the first failure decides for the rest" has to be written for
  workers that may already be past the check.

## 3. Principles

Three rules generate the whole design. They are stated first so that later
sections can be checked against them rather than argued individually.

**P1. mcpp does not predict whether the host can execute an artifact.** It
either does what the project declared, or it attempts execution and reports what
the kernel answered. No table maps a triple to an executability verdict.

**P2. Host-local facts are stated by the operator on that host.** The manifest
describes the project. Whether this machine can execute a given artifact, and
whether the runner the project named is wanted here, are facts about the
machine, and the operator on it is the only party that knows them. mcpp
provides a way to state them (`--no-runner`, section 7) and does not infer
them.

The draft's P2, "a configuration describing an environment this machine lacks
must not disable a capability this machine has", is withdrawn. It licensed a
guess, and the guess was wrong on the host it was measured on (D1).

**P3. Failing loudly outranks succeeding differently.** Where two defaults each
have a failure mode, the default is the one whose failure is an error message
rather than a program that runs and behaves incorrectly.

## 4. Structure

### 4.1 One read point, unchanged

`choose_runner` remains the single function both `mcpp run` and `mcpp test`
consult. The comment above it records why (`execute.cppm:420-425`): deriving the
answer twice is a shape this codebase has paid for in #233, #240, #242 and #344.
This design changes what the function decides, not how many places decide it.

The freestanding predicate moves out of the resolution path and into a single
remaining role: whether an absent runner is fatal before any spawn is
attempted. `RunnerChoice::freestanding` keeps that meaning; `RunnerChoice::tmpl`
is filled for every target that declares one.

### 4.2 Resolution order

For the resolved target, in order:

1. **A runner declared for this target.** The project's
   `[target.<triple>].runner` beats a dependency's `mcpp:runner=`, which is the
   existing precedence (`execute.cppm:446-450`) and the existing note when the
   consumer overrides (`:1286-1289`).
2. **No declaration.** Execute the artifact directly.

The per-triple key is already the scoping mechanism for "this runner belongs to
that cross target": a runner written under `[target.aarch64-linux-musl]` is not
found when the host target is resolved, because the lookup is
`targetOverrides.find(ctx.tc.targetTriple)` and the key is stored canonicalised
(`toml.cppm:1825-1830, :1960`). No new key is needed to express applicability
along the target axis.

What the key cannot express is the host axis, and this is the limit D3 and D4
respond to: the same triple is foreign on one host and native on another, and
`[target.<triple>]` reads the same on both.

The channel that has no triple scope is `BuildConfig::runner`
(`modules/manifest/src/types.cppm:458`), which a dependency's build program
writes. Its merge site says it "supplies a runner for this target"
(`prepare.cppm:7240-7256`) while the storage is per-build. One build resolves
one target, so the two coincide in practice; the exposure is a dependency that
should not be in the host target's graph emitting a runner. That is a
dependency-scoping question and is out of scope here, recorded in section 10.

### 4.3 Launch

```
--no-runner passed?
├── yes → spawn the artifact directly (declared runner ignored, one note)
└── no
    declared runner?
    ├── yes → resolve argv[0] (section 4.4)
    │   ├── not found on any search path → ERROR: names the program, the paths
    │   │                                   searched, the [xlings] deps hint; exit 2
    │   └── found → spawn
    │       ├── started            → done; its exit code is the verdict
    │       └── any spawn error    → ERROR: program and errno verbatim; exit 2
    └── no  → spawn the artifact directly
        ├── started            → done
        ├── ENOEXEC            → UNRUNNABLE: what the kernel said, the triple,
        │                        and the runner key to write; exit 2
        └── other spawn error  → ERROR: program and errno verbatim; exit 2
```

There is no second spawn on any branch. A runner that cannot be started is
reported, not worked around (D1); an artifact the kernel refuses is reported
with the key that would have changed the outcome. `EACCES` on either is a
permission problem, not an absence, and is reported as itself.

Freestanding keeps its stronger contract: with no runner declared, `mcpp run`
and `mcpp test` fail with `no_runner_message` before any spawn
(`src/freestanding/runner.cppm:57`). There is no direct-execution branch, and
its absence is provable rather than measured. The hosted UNRUNNABLE message is
a sibling of that one with different wording: it reports what the kernel
answered (`Exec format error`) rather than asserting why, because on a hosted
triple mcpp does not know whether the refusal is a foreign ISA or a file that
is not an executable at all, and the example it prints is a user-mode emulator
(`qemu-aarch64-static`) rather than a system one.

Exit codes. `mcpp run` today folds every non-zero result, including the
program's own exit status, into 1 (`execute.cppm:1319`); that fold is
pre-existing and not changed here, but "could not start" is distinguished from
"ran and failed" by exiting 2, the code the freestanding no-runner path already
uses. The distinction the bounded launcher's comment demands (section 1) is
thereby made at the caller rather than lost at the callee.

### 4.4 Resolving the runner's program

`BuildConfig::runner` exists because the value is machine-specific: the emulator
lives in a package payload whose path carries a home and a version, "so only a
`build.mcpp` can compute it" (`types.cppm:446-450`). That reasoning is one step
too strong. The engine already computes exactly this mapping for build
programs: `fillXpkgDirs` (`prepare.cppm:4549-4566`) walks the manifest's
`[xlings] deps`, resolves each to its payload directory through
`xlings::paths::xpkg_payload`, and hands the result down as `MCPP_XPKG_*`
environment variables that `xpkg_dir()` reads back (`src/build/hostprogram.cppm:322`).
A build program needs `xpkg_dir()` because it was the only caller with a reason
to ask, not because it is the only caller that can.

Therefore:

> The runner's `argv[0]`, when it is not an absolute path, is resolved by mcpp
> against the `bin/` directory of each payload this project declared under
> `[xlings] deps`, in declaration order, and then against `PATH`. If no
> candidate is an executable file, the runner is reported as not found before
> any spawn is attempted.

The rule earns its place for one reason, and it is not the one the draft gave.
A bare name on `PATH` reaches an xvm shim, and the shim answers for the current
subos rather than for the package (section 2). The payload's `bin/` is the
binary itself. Looking there first is what makes a declared package usable
from a runner without the project author writing a home-and-version path into
the manifest, which is the thing `BuildConfig::runner`'s comment says a static
manifest cannot do.

Doing the lookup in mcpp has a second effect: "not found anywhere" is decided
before `posix_spawnp`, so a spawn-time `ENOENT` can only mean the program was
found and its interpreter or loader was not. The two messages differ, and
neither guesses.

**The recommended shape, and its two forms.** The general form is one key and
a tool the operator has installed by whatever means, including
`xlings install qemu-user-aarch64`:

```toml
[target.aarch64-linux-musl]
runner = ["qemu-aarch64-static"]
```

The CI form adds the package declaration so that a fresh runner provisions the
emulator on first use, through the same pass, stamp and `--offline` refusal
that every other `[xlings] deps` entry already goes through
(`prepare.cppm:3242` onwards):

```toml
[xlings]
deps = ["qemu-user-aarch64"]

[target.aarch64-linux-musl]
runner = ["qemu-aarch64-static"]
```

The CI form has a consequence the documentation must state next to it:
`[xlings] deps` is provisioned on every host that builds the project, without
condition, and a package the host cannot install is a hard build error. With
`qemu-user-aarch64` being x86_64-only, the CI form makes the project
unbuildable on an aarch64 or macOS host until the line is removed. That is the
correct behaviour for the key (a declaration that silently does nothing was
#531) and the wrong shape for a project built on more than one host class. The
general form has no such consequence: a runner whose program is absent fails
only `mcpp run` and `mcpp test`, only on that host, with a message naming the
program, and `--no-runner` runs the artifact directly where the host can.

This keeps one declaration site for packages. `[xlings] deps`
(`types.cppm:763`, parsed at `toml.cppm:1351`) remains the only place that
answers "which packages does this project need". An attribute on `runner`
carrying a package reference was considered and rejected for this reason; see
section 9.

Scope is deliberately narrow: this changes how mcpp locates the runner it was
told about. It does not change `PATH` as seen by the program under test.
`mcpp test` already prepends the sandbox's `subos/default/bin` to the child
environment (`execute.cppm:1886-1897`) and `mcpp run` does not
(`:1309-1314`); that asymmetry predates this work and should be settled on its
own merits, not folded in here.

## 5. `runner = []`: deferred

There is currently no way to state "ignore the runner a dependency supplied and
execute this artifact directly". Omitting the key inherits the dependency's
value; `runner = []` is a hard error.

The feature is deferred because it has no producer. No published package emits
`mcpp:runner=` for a hosted triple; the only emitters in the local registry
are the fixtures of e2e 131 and 132, both freestanding. The parse-time error
for `[]` stays as it is until a dependency supplies a hosted runner.

The spelling is decided now so that the first producer does not reopen it:
`[]`, not `""`. `sysroot` established the shape (`toml.cppm:1877-1889`,
documented at `docs/05-mcpp-toml.md:1185`): an absent key and a present-and-empty
key are different answers, and the empty answer is spelled as the empty value
of the key's own type. Both `[]` and `""` are hard errors on every released
mcpp, so neither degrades better than the other on an older version; `""`
would add a second type to the key for no gain. Under a freestanding triple
`[]` stays rejected at parse time, because on such a target direct execution
is provably not available.

## 6. `mcpp test`: not-run is a reading, not a failure, and not a success

A cross-built test suite on a machine that cannot execute its artifacts has not
failed. Reporting `FAIL (exit 127)` states that the test ran and returned 127,
which is false, and is indistinguishable from a missing program. It has not
passed either, and an exit code that says it did would be read as one.

`TestResult::St` gains a fourth state alongside `Pass`, `CompileFail` and
`RunFail` (`execute.cppm:1529`): `NotRun`, carrying a reason. The rules:

- **One reason, N results.** An `ENOEXEC` on any test's artifact establishes
  that this host cannot execute artifacts of this target for this invocation;
  a runner that cannot be found or started establishes that no test can be
  run through it. Either sets an invocation-wide flag that workers check
  before each spawn. Workers already past the check may fail the same way;
  that is harmless (a refused spawn has no side effects) and each such result
  is `NotRun`, not `RunFail`. The reason is printed once, when it is first
  established, and appears again in the summary.
- **The count is always in the summary when non-zero**, at the same visual
  weight as failures, with the denominator, and in the wording e2e 178 already
  asserts for the workspace-timeout case: `test result: NOT RUN. 0 passed;
  0 failed; 5 not run (this host cannot execute aarch64-linux-musl artifacts:
  Exec format error); finished in 0.41s`. When failures and not-run tests
  coexist the prefix is `FAILED` and both counts are listed.
- **`--message-format json` carries `not_run` as its own status** next to
  `pass`, `compile_fail` and `run_fail` (`:1543-1545`), with `exit_code` 0 and a
  `reason` string; the summary record gains `not_run` and `not_run_reason`.
  The record's other fields keep their types.
- **The exit code is 2 whenever the not-run count is non-zero** (D2). It is
  the code the freestanding path already returns for the same situation, and
  it is distinct from 1, which continues to mean that a test ran and failed.

## 7. `--no-runner`

One flag, accepted by `mcpp run` and `mcpp test`: ignore any declared runner
and execute the artifact directly. It prints one note naming the runner it
ignored, so a transcript shows the deviation.

It exists for the host the manifest cannot describe (D3): a triple that is
native here although the project declares an emulator for it. It is also the
honest answer for a wrapper an operator does not want on this run
(`valgrind`, `sudo -E`, `ssh board`). Without it, that operator edits the
manifest, and the edit is host-shaped.

The draft's `--require-runner` is withdrawn. Its two roles were to make
`NotRun` fatal, which is now the default (D2), and to refuse the `ENOENT`
fallback, which no longer exists (D1). A per-declaration `required = true`
attribute is likewise moot.

## 8. The five axes

**Structure.** One read point is preserved. The freestanding predicate keeps a
single, smaller job. Package declaration stays in `[xlings] deps`; execution
stays in `runner`; the coupling between them is an engine resolution rule with
no representation in the schema, so no consumer of "the declared package set"
gains a second site to read. The launcher layers stop disagreeing about spawn
failures because the errno is carried up (`DeadlineRun` gains the spawn
errno; callers stop re-spawning), and the two `mcpp test` launch paths report
the same condition the same way.

**Stability.** Every decision is either a value the project wrote, a flag the
operator passed, or an answer the kernel gave. Nothing is inferred from host
state that mcpp models itself, which is the class of defect the N x N
cross-build work found nine instances of. There is no branch that runs a
program in a way other than the one declared.

**Cross-platform.** Linux is measured (section 2). The design reads the
platform's exec decision rather than reproducing it, so binfmt_misc, Rosetta,
WOW64 and ARM64EC need no cases in mcpp. Two legs are unmeasured and must not be
implemented from inference: macOS `posix_spawn` may report a wrong-architecture
Mach-O as `EBADARCH` rather than `ENOEXEC`, and on Windows `run_exec` still goes
through `std::system` (`process.cppm:571-576`), where the error is lost in the
shell before it can be typed. The residual `TODO(launcher-unify)` at
`process.cppm:549` names the prerequisite. Until both are measured, the Windows
leg reports the failure it can observe and does not claim to distinguish the
two situations, and the e2e criteria of section 11 are POSIX-gated for that
reason and no other.

**Compatibility.** The behavioural change needs no new manifest key, so a
manifest written for it loads on older mcpp and degrades correctly there: the
older version does not consult the runner and reports the failure it has. The
new flag is a CLI addition. A table form for `runner` was rejected partly on
this axis: `runner` is currently required to be an array (`toml.cppm:1896-1899`),
so a package shipping `runner = { ... }` does not lose a key on an older mcpp,
it fails to load at all, the failure mode recorded in #359.

Two behaviour changes affect existing manifests and are stated rather than
assumed harmless. A `runner` already declared on a hosted triple stops being
inert and starts being used; the documented population is small, because the key
is documented only under "Bare metal", and a runner that was inert and is now
missing becomes an error on that host rather than a silent no-op. And a
`mcpp test` run that reported `FAIL` for unrunnable artifacts now reports
`NotRun`; its exit code stays non-zero, so no CI job changes colour, but its
text and JSON change.

**Simplicity.** The common case is one key that already exists and a tool on
`PATH`. The mechanism a user must understand is one sentence: mcpp uses the
runner you declared, and tells you what the kernel said when there is none.

## 9. Rejected alternatives

**A host-capability oracle.** Deciding before spawning, from the triple plus
probes of binfmt_misc, Rosetta and similar. Rejected on measurement: this host
runs `aarch64-linux-musl` artifacts directly and the host that filed #544 does
not, so any triple-shaped verdict is wrong on one of them. Every entry in such a
table is a host-shaped branch. It would also be wrong in the other direction for
a dynamically linked artifact on a same-architecture host with a different C
library.

**`when = "cannot-execute"` as the default.** Consulting the runner only after
direct execution fails. It expresses "this runner is for cross targets"
directly, but as a default it inverts P3: on a host with binfmt registered,
direct execution starts, the declared runner is skipped, and any argument it
carried (a `-L` sysroot, a machine model) is silently dropped. That failure is a
program that runs and misbehaves; the alternative failure is an error message.

**Falling back to direct execution when the runner's program is missing**
(the draft's section 4.3). Rejected for the same reason as the previous entry,
which it reproduced with a warning attached: on a binfmt host the artifact
runs under a different interpreter with different arguments. It also required
a second spawn, and it read `ENOENT` as "absent" when the errno also covers a
present runner with a missing interpreter or loader. What it was for, a host
where the triple is native, is served by `--no-runner`, which states the fact
instead of guessing it.

**`runner = { command = [...], package = "xim:qemu-user-aarch64" }`.** Rejected
because it opens a second package-declaration site. Every consumer of "what
does this project need installed", the offline refusal message, the first-use
install list, any enumeration, would have to read both, and the ones that
forget do not fail; they answer a different question quietly. Section 4.4
obtains the same capability as a resolution rule with one declaration site and
no schema change. Its cost, that `[xlings] deps` is unconditional, is stated
there rather than hidden by a second site that would be conditional only by
accident.

**`runner = ""` as the override spelling.** Rejected in favour of `[]`; see
section 5.

**`--require-runner`.** Withdrawn; both of its roles are the default now. See
section 7.

**A relaxation flag that lets `mcpp test` exit 0 with tests not run.** Not
proposed. A matrix job that builds a target it cannot run should call
`mcpp build` for that target; a job that calls `mcpp test` has asked a
question and should not be told "yes" when the answer is "not established".
If a use appears, the flag is additive.

## 10. Open questions

1. **macOS.** Measure `posix_spawn` against a wrong-architecture Mach-O.
   `EBADARCH` and `ENOEXEC` must both map to UNRUNNABLE, and the mapping must be
   measured, not assumed. Half measured on 2026-09-02 (PR #545, macOS 14
   ARM64 CI): `posix_spawnp` on an executable file whose content is not a
   loadable format does not return `ENOEXEC`; it spawns the file through
   `/bin/sh`, as `execvp` does, and the shell exits non-zero with
   `spawn_error == 0`. The `ENOEXEC` unit test is therefore Linux-only, and
   on macOS the "this host cannot execute" typing is reachable only for a
   refusal the kernel reports as an error (`EBADARCH` on a real foreign
   Mach-O), which remains unmeasured. The implementation maps `EBADARCH` to
   UNRUNNABLE where the macro is defined; nothing asserts it yet.
2. **Windows.** Determine whether the launcher must be moved onto
   `CreateProcess` before the failure can be typed at all. Wine is not evidence
   for Windows.
3. **`run` and `test` disagree about the child's `PATH`** (`execute.cppm:1309-1314`
   versus `:1886-1897`). Deliberate or drift, to be answered separately.
4. **`BuildConfig::runner` has no triple scope.** Whether a dependency that
   should not be in a host target's graph can supply a runner for it is a
   dependency-scoping question.
5. **A runner's own failure is not typeable.** When qemu exits 255 because it
   could not start, mcpp cannot tell that from the program exiting 255. The
   measurement in section 2 shows this concretely. This is a documented limit,
   not a defect to fix by guessing.
6. **The manifest has no host axis.** `--no-runner` is a per-invocation
   statement; a developer on an aarch64 host types it every time. Whether the
   manifest should gain a host predicate, or whether a host-local
   configuration file should carry "this host executes `<triple>` directly",
   is the durable answer, and it is larger than this issue.
7. **`mcpp run` folds the program's exit status into 1** (`execute.cppm:1319`).
   `cargo run` propagates it. Whether mcpp should is unrelated to the runner
   and is noted because this work touches the line.

## 11. Test criteria

The criteria below are written to survive the two failure modes this repository
has recorded most often: an assertion that passes because it never ran, and an
assertion whose negative reading is also its silent reading.

**No `# requires:` guard on the new e2e.** Both CI shards lack llvm, and a
`# requires:` skip exits 0, so a criterion behind one never runs in CI. The
hosted-runner criteria therefore use a runner that is a shell script in the
project directory, recording `"$@"` to a file and then executing the artifact.
The script runs on every POSIX shard, needs no emulator, and lets the
assertion compare the exact argv mcpp launched against the artifact path.

**The unrunnable path must be exercised on any host, including one with
binfmt_misc.** The probe in section 2 supplies the technique: after
`mcpp build`, patch `e_machine` of the produced binary to `0xffff`, which
matches no binfmt entry and no native loader, so `posix_spawnp` returns
`ENOEXEC` deterministically. The criterion asserts, after the run, that the
artifact still carries the patched machine, so a rebuild between the patch
and the run is a loud failure of the test rather than a silent pass of the
host binary.

**Each criterion carries a denominator.** For `mcpp test`, assert the full
summary line including every count, not the presence of the words "not run".
For the runner path, assert the argv the script recorded equals the artifact
path, not that a message mentioning the runner appeared.

**Both `mcpp test` launch paths get the same criterion.** The single-test
invocation streams and the two-test invocation captures (section 1), and today
they report a spawn failure differently. Each criterion below that involves
`mcpp test` is run once with one test file and once with two, and the
assertion on the reason text is the same in both.

| Criterion | Setup | Assertion |
|---|---|---|
| Declared runner is used on a hosted target | script runner, host target | recorded argv equals the artifact path; exit code is the artifact's |
| Runner program not found | runner names an absent program | message names the program and the paths searched; exit 2; the artifact was not executed |
| Runner found under a declared payload, not on `PATH` | `[xlings] deps` names a package whose `bin/` holds the script; `PATH` does not | recorded argv proves the payload copy ran |
| No runner, unloadable artifact, `mcpp run` | `e_machine` patched | message contains `Exec format error`, the triple and the paste-able key; exit 2 |
| No runner, unloadable artifact, `mcpp test` | `e_machine` patched, one and two tests | `NOT RUN. 0 passed; 0 failed; N not run (…)` with N equal to the test count; exit 2; JSON records carry `not_run` |
| `--no-runner` | script runner declared | the script's record file is absent; the artifact ran; one note names the ignored runner |
| Freestanding contract unchanged | `os = none`, no runner | `no_runner_message`, exit 2, no spawn |
| Array-valued typo is reported | `runnerX = ["x"]` under `[target.<triple>]` | warning names `runnerX` and lists `runner` among supported keys |

**Three of these must fail before the fix and pass after.** The two
unrunnable cases produce exit 127 with no output today on the streaming path,
so a criterion asserting only a non-zero exit would pass against the unfixed
engine; the assertions are on the message text and the exit code together.
The declared-runner case fails today because the script is never invoked, so
its record file is absent, which is the assertion's negative reading and must
therefore be paired with the positive one (the artifact's own output appeared
through the script).

**A unit test states the invariant the layers disagree about.** `run_exec`
must not return 127 for a spawn failure without reporting it; `DeadlineRun`
must carry the spawn errno when `supported` is false; and neither
`run_exec_deadline` nor `capture_exec_deadline` may spawn a second time after
the bounded launcher reports a spawn failure. Section 1 shows these are one
defect seen from two sides; a test that covers only the untimed path leaves
the doubled spawn in place.

**What is not exercised.** Provisioning `qemu-user-aarch64` through
`[xlings] deps` is not an e2e criterion: it needs network, an x86_64 host,
and a registry, and the provisioning pass has its own coverage from #531. The
payload-`bin/` rule is exercised with a locally staged package directory
instead (third row).

## 12. Implementation surface

Listed so the change can be sized and split. Each line is one concern.

- `src/build/execute.cppm`: `choose_runner` reads both producers for every
  target; `mcpp run` and `mcpp test` take `--no-runner`; `TestResult::St::NotRun`,
  the invocation-wide flag, the summary line and the JSON record; exit codes.
- `modules/platform/src/process.cppm` and `unix/bounded_process.cppm`:
  `DeadlineRun` carries the spawn errno; `run_exec` reports the failure it
  drops today; callers stop re-spawning; the runner lookup helper.
- `src/freestanding/runner.cppm`: a hosted sibling of `no_runner_message`.
- `modules/manifest/src/toml.cppm`: the sweep learns a known-arrays list
  (`runner`) and prints both lists; nothing else in the parser changes.
- `docs/05-mcpp-toml.md` (2.7.1 gains `runner`; 2.7.2 keeps the bare-metal
  example; a hosted-cross subsection with the two forms and the `[xlings]
  deps` consequence), `docs/13-baremetal.md` (cross-reference),
  `docs/15-openkal-cross.md` (the example at :185 is bare-metal; add the
  hosted one), `docs/11-machine-output.md` (the `mcpp test` record, D6), and
  each one's `docs/zh/` twin, which CI enforces.
- `tests/e2e/`: one new script covering the table in section 11;
  `tests/unit/`: the launcher invariant.
