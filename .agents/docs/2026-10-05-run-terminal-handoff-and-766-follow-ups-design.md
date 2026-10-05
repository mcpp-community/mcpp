---
subject: design
status: landed
---

# `mcpp run` hands the terminal to the program, and the follow-ups of #761, #763 and #765 (#766)

- Date: 2026-10-05. Status: revision 3, implemented in mcpp 2026.10.5.1 (one pull request, §0.4).
- Base: `main` at `68e73108`, with #765 (`acd9578a`), #761 (`c313c024`) and #763 (`68e73108`) merged.
- Inputs:
  - A report that an interactive program cannot read its input under `mcpp run` or `mcpp run -q --release`.
  - Issue #766, which records the problems a combined review of #761, #763 and #765 found.
  - Measurements on Linux x86_64 with a binary built from the three PRs merged together (`16366aa3`).
    The Windows statements in Part I are inferred from the code and are marked as such.

**Revision 2.**
- Review on 2026-10-05 answered revision 1's four open questions (§0.1).
- A self-review against the code then corrected several points in this record. §0.3 lists them.

Reading order:
- Part I is the main design: `mcpp run` and the terminal.
- Part II designs each item of #766.
- Part III orders the work into pull requests.
- Part IV asks the questions that remain.

## 0. Decisions

### 0.1 Settled in review (2026-10-05)

| # | Question | Decision |
|---|---|---|
| Q1 | After an exec, mcpp can print nothing once the program ends. Is that acceptable? | Yes. |
| Q2 | Does `mcpp run` exec without a terminal too (CI, pipes)? | Yes: one path for both. |
| Q3 | `exports` beside source annotations on PE | A warning, not a refusal. |
| Q4 | A registry or git dependency's glob that leaves its package | Refused. |

### 0.2 Proposed, awaiting review

| # | Question | Proposal |
|---|---|---|
| D1 | How does `mcpp run` start the program on POSIX? | Replace mcpp with the program (`execve`) once the build is done (§1.6). Q1 and Q2 accept its consequences. |
| D2 | How does `mcpp run` start the program on Windows? | Start it without a new process group and, except for batch files, without `cmd.exe`. Keep it in a kill-on-close job, and let mcpp ignore Ctrl-C while it waits (§1.6). |
| D3 | The name of #763's key | `windows_auto_export`, and a SPEC-004 rule for platform-scoped keys (§2.1). |
| D4 | `exports` on PE | Patterns filter the discovered candidates. Beside annotations, a warning (Q3). Beside `windows_auto_export = false`, refused when planning an MSVC-ABI row. An empty export surface is an error only when a consumer in the plan links the DLL (§2.2). |
| D5 | Glob inputs | One directory walk shared with the source scanner: directory symlinks followed with a cycle guard, and the scanner's exclusions. A glob leaving its package (`../` or absolute) is honoured for the root, path dependencies and workspace members, and refused for registry and git dependencies (Q4). §2.5. |

### 0.3 What the self-review changed

| Where | Revision 1 | Revision 2, and why |
|---|---|---|
| §1.6 closing notices | not considered | `mcpp::ui::print_closing_notices()` runs after every command (`src/cli.cppm`, end of `run`). After an exec it would never run, so the notices are printed before "Running". |
| §1.6 locks | "audit `FileLock`" | Audited: `FileLock` opens with `O_CLOEXEC` and both users (`xlings.cppm`, `bmi_cache.cppm`) hold it in a local scope. Nothing leaks across an exec. |
| §1.6 threads and children | not stated | The build's threads (live progress, `--play-game`) are joined and `KeyInput` is destroyed before the exec. The group guard registry must be empty, so no child of mcpp survives into the program. |
| §1.3 R2 | "the status is the program's own" | Also a behaviour change: today a signal death becomes exit status `128+n` from mcpp. After the exec, a caller sees the signal itself; for example, Python's `subprocess` reports `-n`. That is what the direct run gives, and the release notes say so. |
| §1.6 Windows | `CreateProcessW` without `cmd.exe` | A runner that is a `.bat` / `.cmd` file still needs `cmd.exe`, with the existing quoting rule. Starting a batch file through `CreateProcess` directly is the "BatBadBut" argument-injection shape. The environment travels as an environment block, not a `set` prefix. The job's Ctrl-C guard is not registered for this launch. |
| §2.2 rule 4 | refused at manifest load | A manifest is cross-platform, and `exports` beside `windows_auto_export = false` is valid on ELF and Mach-O. The refusal moves to planning an MSVC-ABI row. |
| §2.2 rule 5 | an empty PE export surface is always an error | Resource-only DLLs and DLLs loaded only for `DllMain` are legitimate. The error applies only when a consumer in the plan links the DLL. |
| §2.5 symlinks | warn when the prefix crosses a symlink | The source scanner *follows* directory symlinks with a canonical-path cycle guard (`walk_tree`, `src/modgraph/scanner.cppm`). The comment in `glob_fingerprint` that claims "the same rule the source scan uses" is false today. The fingerprint adopts the scanner's walk instead of warning. |
| §2.5 exclusion | exclude the output directory by path | The scanner also excludes by *name* (`is_excluded_walk_dir`: `.mcpp`, `.git`, `target`, and submodule paths). Parity means adopting that rule, so `../assets/target/**` stays excluded. Changing it would change `sources` globs too, and is not proposed. |
| §2.5 absolute patterns | refused | `rerun_if_changed` (one file) already accepts absolute paths. Absolute globs follow the same rule as `../` instead: matched as absolute paths where escaping is allowed, refused where it is not. |

### 0.4 Revision 3: settled questions, and where the implementation departs

Review on 2026-10-05 accepted D1 to D5 and answered Part IV: closing notices
are printed before `Running` (IV.1), the change from `128+n` to a visible signal
death ships without a switch (IV.2), and glob inputs follow directory symlinks
(IV.3). The implementation then departed from revision 2 where the code showed
a smaller or a correct alternative:

| Where | Revision 2 | Implemented |
|---|---|---|
| §1.6 Windows | `CreateProcessW` on the program, an environment block, `cmd.exe` only for batch runners | The existing `cmd.exe` command line is kept: it is the one derivation of the quoting and the environment prefix every Windows launch uses, and the defect is the process group, not the shell. `winproc::run_foreground` starts it without `CREATE_NEW_PROCESS_GROUP`, in a kill-on-close job, and mcpp ignores Ctrl-C and Ctrl-Break through a handler of its own, which the child does not inherit. Replacing the shell remains possible and is not required by any requirement of §1.3. |
| §1.6 POSIX | resets every handled or ignored signal | mcpp ignores no signal (audited: no `SIG_IGN`, no signal mask), and `execve` resets handled ones, so the launcher restores the terminal guard and clears the group guard; a guarded group at that point is an internal error (exit 125). PATH is searched with mcpp's own PATH, as `posix_spawnp` did. |
| §1.6 docs | docs/08 states that a test does not read the terminal | Not stated: the test launcher inherits standard input, so the statement would be false. The test launcher is unchanged. |
| §2.2 rule 5 | `--required` when a unit of the plan consumes the import library | As designed; the backend decides it from the plan's units, so the plan carries no new field. |
| §2.3, §2.4 | two predicates | One predicate, `links_objects_of(package, owner)`, decides both a unit's objects and the packages whose shared dependencies it links, in the artifact and the member paths. |
| §2.5 | the fingerprint adopts the scanner's walk | The walk moved to `mcpp.modgraph.glob` (`walk_glob_tree`, `is_excluded_walk_dir`), which both the scanner and the fingerprint call; the submodule cache gained a lock, since build programs fingerprint on several threads. The build's output directory is excluded by name in addition. |
| §2.7 `coff-def` | one `llvm-nm` for all bitcode inputs | Each object's compiler and `llvm-nm` run concurrently, up to eight at a time, which keeps `read_nm_exports`' single-file grammar. |
| §2.7 PE executables | verify `<exe>.lib` / `.exp` on Windows | Settled from the code: `mcpp pack` stages the program by name and its runtime closure, never the contents of `bin/`, so such a file does not reach a package. The executable's own export table is documented in docs/04 as the program linking its package's objects. |
| §2.7 E2E 721 | a per-test bound in `run_all.sh` | The bound existed and needed GNU `timeout`, which the macOS runners lack; `tests/e2e/_timeout.py` supplies it there and ends the test's process tree. |

Tests: E2E 883 (new) drives `mcpp run` through a pseudo-terminal and fails
against a binary without the change (`STATUS hung`). E2E 880 gained the placed
static variant, which fails before the change with the undefined reference. E2E
881 gained cases I to L for `exports` on PE, and E2E 882 the absolute pattern,
the unwatchable warning and the refusal in a git dependency.

---

## Part I. `mcpp run` and the terminal

### 1.1 Symptom

```cpp
import std;
int main() {
    std::string line;
    while (std::cout << "> " << std::flush, std::getline(std::cin, line)) {
        if (line == "quit") break;
        std::cout << "read: " << line << std::endl;
    }
}
```

Measured on Linux, with a pseudo-terminal driving the program the way a user's terminal does:

| Invocation | Piped stdin | Terminal stdin | Ctrl-C (program with a `SIGINT` handler that exits 3) |
|---|---|---|---|
| the program directly | works | works | handler runs, prints, exit status 3 |
| `mcpp run` | works | **hangs** after `> `; typed text is echoed but never read | **handler never runs**; mcpp dies of `SIGINT` |
| `mcpp run -q --release` | works | **hangs** | same as above |

Piped stdin works, which is why no E2E has seen this: every E2E runs without a terminal.

### 1.2 Cause

`process::run_exec` (`modules/platform/src/process.cppm`) starts every child in a process group of
its own (`POSIX_SPAWN_SETPGROUP`). The rule came with #555 (`9da7163f`, 2026.9.4.3), so that a
`timeout`-terminated `mcpp` does not leave ninja and its compilers running. Both `mcpp run` paths
start the user's program through the same function: the fast path at `src/build/execute.cppm:2212`
and the prepared path at `:2404`.

On a terminal, the new group is a background group. `ps` while the program waits for input:

```
  PID   PPID   PGID    SID  TPGID STAT COMMAND
36047  36045  36047  36047  36047 Ss+  mcpp
36051  36047  36051  36047  36047 T    stdinrun
```

The program's group (36051) is not the terminal's foreground group (36047). Its first read is
answered with `SIGTTIN`, and it stops (`T`). Ctrl-C is delivered to the foreground group, that is to
mcpp alone. mcpp's signal guard then `SIGKILL`s the program's group, so the program's own handler
never runs.

The repository already states the constraint. `capture_with_deadline`'s `ownGroup` is opt-in
"because a child in a background group that reads the terminal is stopped by SIGTTIN, and the
uncaptured callers of this function hand the terminal to their child"
(`modules/platform/src/unix/bounded_process.cppm`). `run_exec` applies the group unconditionally,
and `mcpp run` is exactly such an uncaptured caller.

**Windows (inferred, not measured).** `run_exec` starts the child through `cmd.exe /d /s /c` with
`CREATE_NEW_PROCESS_GROUP` in a kill-on-close job. Console input is shared, so reading stdin
probably works. A process in a new group does not receive the console's Ctrl-C, however, so the same
"the program's handler never runs" applies. The implementation PR measures it (§1.7).

### 1.3 Requirements

| # | Requirement |
|---|---|
| R1 | The program owns the terminal as it would if started by the shell: line input, Ctrl-C, Ctrl-\\, and Ctrl-Z / `fg`. |
| R2 | The status a caller sees is the program's own, including death by a signal. This replaces today's `128+n` from mcpp (§0.3). |
| R3 | No process outlives mcpp (#555): `timeout mcpp run`, a closed terminal or a killed mcpp leaves nothing running. |
| R4 | A refused spawn is still classified and reported in the 125-127 band, with the ENOEXEC advice (#544). |
| R5 | Without a terminal (CI, pipes, `timeout`), output and exit status are unchanged, apart from R2. |

### 1.4 What other tools do

| Tool | How the program is started |
|---|---|
| a POSIX shell | `fork`; the child joins a new group and becomes the foreground group (`tcsetpgrp`); the shell waits with `WUNTRACED` and takes the terminal back. Full job control. |
| `cargo run` | POSIX: replaces itself with the program (`exec_replace`). Windows: spawns it in the same console and installs a Ctrl-C handler that ignores the event, then exits with the child's code. |
| `go run`, `npm run`, `system(3)` | spawn in the same process group; the parent ignores or relays `SIGINT` / `SIGQUIT` while waiting. |

None of them puts the program in a background group.

### 1.5 Options

| | A. Replace mcpp (`execve`) | B. Same group, parent relays signals | C. Own group, terminal handed over |
|---|---|---|---|
| R1 terminal, Ctrl-C | yes, by construction | yes; Ctrl-Z stops both | yes, if Ctrl-Z / `fg` is reimplemented |
| R2 status | yes, the caller waits on the program itself | needs re-raising the signal in mcpp | needs re-raising |
| R3 nothing outlives | yes: there is no parent left to kill | only if mcpp relays `SIGTERM` / `SIGHUP` to the child | yes, group guard kept |
| R4 spawn failure | yes: `execve` fails in mcpp, which can still classify the errno | yes | yes |
| Implementation | small, but mcpp must leave a clean process (§1.6) | small | `posix_spawn` cannot hand the terminal over in the child: glibc 2.35+ has `posix_spawn_file_actions_addtcsetpgrp_np`, macOS has nothing, so it needs `fork`; plus `WUNTRACED` stop/continue handling. It amounts to writing a job-control shell. |

C is rejected. B remains the fallback for a future path that must run code after the program, for
example a runner that needs cleanup. A is the design, as it is cargo's, and Q1 and Q2 accept its
consequences.

### 1.6 Design

**POSIX: `process::exec_program(argv, env) -> errno`.** It returns only when `execve` fails. Before
the call, mcpp leaves a process the program can own, in this order:

1. **Finish mcpp's own work.** Join the build's threads: the live progress line and `--play-game`.
   Destroy `KeyInput`. Every build-scoped child (ninja, `[hooks] during_build`) has been reaped; the
   group guard registry must be empty, and a non-empty registry is an internal error rather than a
   silent orphan.
2. **Print what would have come after.** Call `mcpp::ui::print_closing_notices()`. The notices then
   appear before "Running `…`" instead of after the program's output.
3. **Restore the terminal** (`unguard_terminal_mode`) and flush `stdout` / `stderr`.
4. **Reset signal state.** Restore `SIG_DFL` for every signal mcpp handled or ignored; `exec`
   resets handled signals but keeps ignored ones. Empty the signal mask.
5. **Descriptors.** Nothing to do: `FileLock` and every internal pipe are close-on-exec (§0.3).
   The implementation adds a debug assertion that only descriptors 0-2 survive.
6. **`execve`** with the merged environment (`merged_environ`). When a runner is selected, its argv
   is exec'd the same way.

On failure, the errno goes to the existing classification (`runner_lookup::classify`,
`unrunnable_message`, `launcher_status`), so R4 holds. Destructors and `atexit` handlers do not run
after a successful exec. Steps 1 and 2 are where anything mcpp does at exit must move, and the
implementation PR lists what it moved.

**Windows: `process::run_foreground(argv, env)`.**
- `CreateProcessW` on the program itself, with the merged environment as an environment block.
  A `.bat` / `.cmd` runner keeps going through `cmd.exe` with the existing quoting rule. Passing a
  batch file to `CreateProcess` directly is the "BatBadBut" argument-injection shape.
- No `CREATE_NEW_PROCESS_GROUP`, so the program receives Ctrl-C and Ctrl-Break.
- Keep the kill-on-close job: it makes R3 hold even when mcpp is killed outright. Do not register
  the job's Ctrl-C guard for this launch, because it would kill the program on the Ctrl-C the
  program is meant to handle.
- While waiting, mcpp's console control handler returns `TRUE` for Ctrl-C and Ctrl-Break, so mcpp
  survives to report the program's status. This is cargo's Windows behaviour.
- Closing notices are printed before the launch, as on POSIX, so that output order does not depend
  on the platform.
- Exit with the program's code.

**Scope.** Only the two `mcpp run` call sites change. `run_exec` keeps its group for every other
caller: the analyser actions in `src/cli.cppm:1257` run under ninja and do not own a terminal.
`mcpp test` keeps its deadline launcher; a test does not read the terminal, and docs/08 says so.

**Docs.** docs/09 (commands by scenario) states that `mcpp run` replaces itself with the program on
POSIX. It also states the R2 change: a signal death is visible as such. The release notes repeat
both.

### 1.7 Tests

A new E2E, `# requires: python3`, POSIX hosts only, drives `mcpp run` through `pty`. The pty driver
already written for this record is the template.

| Case | Expectation |
|---|---|
| line input under a terminal, fast path and prepared path (`run`, `run -q --release`) | `read: hello` then exit 0 |
| Ctrl-C with a program `SIGINT` handler | handler output, status 3 |
| a program killed by `SIGABRT` | the caller-visible status equals the direct run's |
| `timeout -s TERM 2 mcpp run` on a program that never exits | no process of the run remains |
| piped stdin, no terminal | unchanged |
| a runner (`[runners]` template) | the runner owns the terminal the same way |
| a closing notice raised during the build | printed before "Running" |

Windows: an E2E for piped stdin, the exit code and a `.cmd` runner with an argument containing
`&` and `"`. Ctrl-C is measured by hand and recorded in the PR, because a test cannot generate a
console Ctrl-C for a process in its own console without also interrupting itself.

---

## Part II. The follow-ups of #766

### 2.1 P0: `auto_export` becomes `windows_auto_export`, and the rule is written down

Every target key whose effect exists on one platform only already carries that platform's name:
`windows_subsystem`, `windows_entry`, `windows_code_page`. `auto_export`, added by #763, has an
effect only on MSVC-ABI PE shared libraries, but its name reads as cross-platform. SPEC-004 §5.2
forbids renaming a key once it is in a released descriptor, so the name is decided before the next
release.

- Add SPEC-004 §5.3: *A key whose effect exists on one platform or ABI carries that platform's
  prefix (`windows_`, …). A neutral name is reserved for a key with a meaning on every row; such a
  key either renders on every row or is refused where it cannot.*
- Rename in the TOML and xpkg readers, the unit tests, E2E 881 and docs/04 (en/zh). Because the key
  is unreleased, no alias is kept.
- Replace "(unreleased)" with the release's `(mcpp X.Y.Z+)`.

A cross-platform meaning, for example "`auto_export = false` means default-hidden visibility
everywhere", is a different feature with a compile-side effect. It is not proposed here.

### 2.2 P0: `exports` on PE, and the precedence of the export sources

Today `exports` has no effect on PE:
- `exports_flag` returns `""` for Windows.
- `coff-def` receives no patterns.
- The generated `obj/<t>.exports.gen` for Windows even contains an ELF version script that nothing
  reads.

The docs, before and after #763, say otherwise.

The rule for an MSVC-ABI DLL, which docs/04 states and SPEC-004 records:

1. **Annotations are authoritative.** If any input declares exports (`dllexport`, `/EXPORT:`
   directives, linker-option metadata), the DLL publishes exactly those. The `.def` is empty, as
   today. `exports` beside annotations cannot narrow them, because the linker reads object
   directives regardless of the `.def`. mcpp reports the combination as a **warning** that names an
   annotated object (Q3).
2. **Otherwise `exports` narrows discovery.** The candidates (COFF reader, or `llvm-nm` for bitcode)
   are filtered by the patterns with the ELF version script's glob semantics (`*`, `?`, `[…]`). The
   filtered list is the `.def`.
   - Patterns match the linker-level name: undecorated on i386, as `export_name` already spells it.
   - C names (`vk_icd*`) are portable across platforms. C++ patterns are not, because MSVC and
     Itanium mangle differently. docs/04 says so.
3. **Otherwise everything discovered is published**, as today.
4. **`windows_auto_export = false` with `exports` is refused when planning an MSVC-ABI row**, since
   there are no candidates to narrow. The message points to annotations. The manifest itself is
   valid: the same pair is meaningful on ELF and Mach-O.
5. **An empty export surface is an error only when a consumer in the plan links the DLL.**
   - An empty surface produces no import library, and a consumer would fail far from the cause.
   - The plan passes `--required` to that DLL's `coff-def` edge when a link unit of the plan
     consumes it.
   - `coff-def` then fails on an empty surface with no annotations. The message names the target
     and the three ways to export: annotations, `exports`, discovery.
   - Without a consumer, a resource-only or `DllMain`-only DLL stays valid.

Implementation:
- Write `obj/<t>.exports.gen` for Windows as one pattern per line, and pass it to `coff-def` as
  `--exports-file`.
- Add a Windows E2E that covers rules 1 (warning), 2, 4 and 5 (with and without a consumer).

### 2.3 P1: the shared dependencies of statics placed in the owner's own image (#761 follow-up)

Both #761 paths link the objects of static packages that `place_static_packages` assigned to the
owner's own image, but collect shared links only from packages outside `placedInImage`:
- workspace members: `src/build/plan.cppm` near line 3179;
- artifacts: near line 2937.

A static dependency's own shared dependency is then missing. This is measured: E2E 880 with
`common → support` instead of `dual → support` fails with `undefined reference to delta@t880_support()`.

Fix: in both loops, treat `staticsByImagePackage[owner]` as the owner:

```cpp
const auto ownImage = [&](std::size_t i) {
    auto it = staticsByImagePackage.find(ownerIndex);
    return it != staticsByImagePackage.end() && std::ranges::contains(it->second, i);
};
if (i == ownerIndex || ownImage(i) || (!sharedDepPackages.contains(q) && !placedInImage.contains(q)))
    append_direct_shared_deps(lu, i);
```

Then extend E2E 880 with the variant.

### 2.4 P1: consumers link statics that belong to another image

In the same variant, `client` links `common`'s objects directly and also `libdual_dll`, which
already contains them. This was measured as failing on `main` and with #761. The member and
artifact object loops must skip a `placedInImage` package unless its image is the unit's own (§2.3).
The root's loops already do. The image's shared library reaches the link through
`append_direct_shared_deps` of the package that depends on it. Placement is computed once for the
whole graph, from the plan's root, whose edges include every member. A static package that a member
also reaches directly is therefore a placement conflict, never placed, so skipping placed packages
cannot drop a direct dependency.

Test: a consumer of a shared package whose static dependency has an external shared dependency,
built and run on all three binary formats.

### 2.5 P1/P2: glob inputs: one walk, and a boundary (#765 follow-up)

**One walk shared with the source scanner.**
- `glob_fingerprint` (`modules/buildmcpp/src/directives.cppm`) walks with its own rules: it does not
  follow directory symlinks, and it excludes any directory *named* like the output directory.
- The source scanner (`walk_tree`, `src/modgraph/scanner.cppm`) follows directory symlinks
  ("vendored trees are often symlink farms") with a canonical-path cycle guard. It excludes
  directories named `.mcpp`, `.git` or `target`, and the repository's submodule paths
  (`is_excluded_walk_dir`).
- The fingerprint's comment claims the two are the same rule. They are not, and #765 carried the
  difference into its new prefix check.
- The fingerprint therefore adopts the scanner's walk. Directory symlinks, in the prefix and below
  it, are followed with the cycle guard. The scanner's exclusions apply unchanged.
- One known limitation is shared, not fixed: a directory named `target` is excluded wherever it
  is, so `../assets/target/**` matches nothing. The full-exclusion warning below makes that visible.
- #765's unit case `DirectorySymlinksAreNotFollowedThroughTheLiteralPrefix` is inverted accordingly.

**A glob that is excluded in full is reported.** If a component of the literal prefix is an
excluded directory (`target`, `.git`, `.mcpp`, a submodule), the glob can never match. mcpp warns once per build, naming the
pattern. For `.git/HEAD`-style needs, the warning names `rerun_if_changed`, which watches one file's
contents.

**Boundary (Q4).**
- A pattern that leaves its package, either `../…` or absolute, is honoured when the package is the
  root, a path dependency or a workspace member. Their surroundings are the user's tree.
- An absolute pattern is matched against absolute paths. Today it is walked but can never match,
  because matching is relative to the package root.
- In a registry or git dependency, the same pattern is **refused** when the directive is read, and
  the message names the package and the pattern. There, `../` resolves into the package store, whose
  contents depend on what else is installed.
- `mcpp pack` and publish warn about a pattern that leaves the package, since it would be refused
  once the package is consumed from a registry.
- docs/30 states the boundary, and advises a specific literal prefix: `../../**` walks a large tree
  on every fast-path check.

### 2.6 P2: LLVM IR text as an interface (#763)

Bitcode export intent is read from `clang -S -emit-llvm`. If the text format changes, annotations
stop being recognised and discovery publishes more than intended. Record in SPEC-009 (toolchain
specification) that a change of the Windows LLVM pin requires E2E 881 green on that pin. No code
change.

### 2.7 P3

- **`coff-def` with LTO.** One `llvm-nm` for all bitcode inputs (it accepts many files and prints a
  header per file), and the intent check run concurrently, bounded by the job count.
- **docs/04 or docs/07: an executable and its own DLL.** The executable links its package's objects
  (compile-once model, as Cargo's bin links the rlib). A host that must load its own DLL is two
  workspace members. A program that also loads a plugin built against that DLL holds two copies of
  its state.
- **A PE executable that exports annotated symbols.** Verify on Windows whether linking `dllexport`
  objects into an EXE writes `<exe>.lib` / `.exp` into `bin/`. If it does, `mcpp pack` stages only
  declared products, and docs/04 notes it.
- **SPEC-004 §1 and §6** cite "docs/05 Appendix A (Schema Ownership Principle)", which no longer
  exists. Point them to the current location or inline the rule.
- **E2E 721 on macOS** hung until the 20-minute step limit. Give it a per-test bound in `run_all.sh`
  so that one hang fails one test, not the shard and the coverage job behind it.

---

## Part III. Order of work

| PR | Content | Why this order |
|---|---|---|
| 1 | Part I: `mcpp run` owns the terminal (POSIX exec, Windows foreground), E2E, docs/09 | User-visible, independent, and the cause is known |
| 2 | §2.1: `windows_auto_export` and SPEC-004 §5.3 | Must land before the next release (§5.2) |
| 3 | §2.2: `exports` on PE, precedence, warning, refusal, `--required`, Windows E2E | Builds on 2 |
| 4 | §2.3 and §2.4: placement-aware member and artifact links, E2E 880 variants | Same loops; one review |
| 5 | §2.5: shared walk, full-exclusion warning, boundary and refusal, docs/30 | Independent |
| 6 | §2.6 and §2.7: spec notes, `coff-def` batching, docs, E2E 721 bound | Independent; can be split |

PRs 1, 2, 4 and 5 can proceed in parallel. Each closes its items in #766.

## Part IV. Open questions

1. §1.6 step 2: closing notices before "Running" (proposed), or dropped for `mcpp run`?
2. §0.3, R2: is the change from `128+n` to a visible signal death acceptable without a
   compatibility switch? The proposal: yes, because it matches the direct run, and the change is
   announced.
3. §2.5: following directory symlinks in glob inputs also changes which files existing patterns
   inside the package match. The proposal accepts that, for parity with `sources` globs. The
   alternative is to keep not following them and correct the false comment instead.
