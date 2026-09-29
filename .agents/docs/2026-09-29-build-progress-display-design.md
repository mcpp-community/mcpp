---
subject: design
status: landed
---

# Build progress: each step's line states its outcome, and one status line states the build

- Status: landed (revision 2). Implemented in #742, released as 2026.9.29.5
  (the parts listed in section 6 of
  `2026-09-30-build-output-refinement-design.md` are replaced by revision 3,
  #743, 2026.9.30.1)
- Date: 2026-09-29
- Origin: the cross-verification of #742 on the validation project (run
  36562019799). `mcpp build --workspace` printed its last `Compiling` line at
  11:34:50 and `Finished` at 12:14:58; nothing was printed in between. The
  discussion that followed settled the requirements in section 2.

Revision 2 replaces the criterion "every step of the build is planned", which
a measurement refuted (section 3.1), with a completion rule that holds in
every build. It drops the per-package fraction and the `stopped` state, which
depended on that criterion, and states how a step is attributed to its
package (section 6.3) and how lines that bypass the renderer are handled
(section 5.3).

## 0. Scope

This document covers the human output of the commands that build (`build`,
`run`, `test`, `pack`, and the planning part of `emit build-database`), from
the first line to `Finished`. It does not change machine output
(`--message-format json`), the result lines of `mcpp test`, or the `Packed`
report of `mcpp pack` (revised in 2026.9.29.5).

## 1. The problem, measured

1. **The ninja phase prints nothing.** ninja runs with `--quiet`, and mcpp
   collects its output whole (`capture_exec`, `capture_exec_deadline`) and
   examines it after ninja exits. In the run above the vcpkg binary cache was
   cold, so the `prepare` actions of deps-vcpkg built ports from source for
   most of the 40 minutes 8 seconds, and the log gives no indication of it.
2. **A failure is reported late.** A failed step is printed after ninja exits,
   that is, after every step already running has finished. A compile error
   beside a running 20-minute vcpkg install is shown 20 minutes after it
   occurred.
3. **`Compiling <package>` states an intention, not an outcome.** The line is
   printed for the root's direct dependencies before ninja starts, whether or
   not they have anything to compile. Transitive dependencies are not named at
   all, and nothing later states what happened to any of them.
4. **A build program takes two lines and states no time**:
   `build.mcpp compiling X` and `build.mcpp running X`, or
   `build.mcpp up to date X (cached)`.
5. **`Finished … in X` counts only ninja.** The validation project's step took
   1124 s while `Finished` said 1020.65 s (the acceptance report of
   2026.9.29.4).
6. **Terminal detection answers "not a terminal" on macOS and Windows.**
   `mcpp.platform.terminal::is_tty` is compiled only under `__unix__`, which
   Apple's compilers do not define and Windows does not have. The one live
   display mcpp has, the download bar, is therefore drawn only on Linux, and
   colours are off on the other two platforms.

## 2. Requirements

These were settled in the discussion.

- **R1** Each step's line carries the step's state. On a terminal the line is
  updated in place while the step runs. In a log it is written once, when the
  outcome is known.
- **R2** One status line summarises the build, for example
  `Building 612/1203 · 14:32 · gpp.gui: vcpkg install 6:10`. It is always the
  last line, separated from the step lines by one blank line, and not aligned
  with them.
- **R3** `Finished` is separated from the step lines by one blank line and
  states the total time and how it was spent.
- **R4** The default output names the packages the user asked to build. The
  packages they depend on are folded into one line.
- **R5** `-v` or `--verbose` shows everything: every package, every step as
  ninja reports it (commands and their output), and how long each build
  program took to compile and to run.
- **R6** The states are named `done`, `ran`, `cached` and `waiting`, with
  `failed` and `fresh` for the cases the discussion did not cover
  (section 4.2).
- **R7** No line states something mcpp does not know. This is the codebase's
  standing rule: the word `Cached` was once printed for three months while
  ninja recompiled every unit behind it.

## 3. What mcpp can know, and when

| Fact | Source | Known | Exact |
|---|---|---|---|
| a download, a provisioning entry, a build program: start, end, outcome | mcpp runs them | as they happen | yes |
| steps finished, steps planned so far | ninja's status line (`NINJA_STATUS`: `%f`, `%t`, `%e`) | when each step finishes | yes |
| which step finished, when it started and ended | `.ninja_log`, written and flushed right after the status line (ninja 1.12.1, `build_log.cc:170`) | a moment after each step finishes | yes |
| a step's output, a failure | ninja's standard output, after the step's status line | when the step finishes | yes |
| which step is running | not reported by ninja through a pipe | not known | no |
| which steps ninja will run | not reported; `%t` counts them and changes during the build | not known | no |

**ninja through a pipe reports a step only when it finishes.** ninja prints a
status line when a step starts only on a smart terminal or for the console
pool (`StatusPrinter::BuildEdgeStarted`); mcpp reads ninja through a pipe.
Measured with ninja 1.12.1 and a two-second step: the step's line appeared
two seconds after it started, together with its output. `check` and
`prepare` actions are the exception: they run through the engine's wrapper
(`mcpp __action`, `mcpp __action-stamp`) on every platform, so the wrapper can
report their start (section 6.4). These are the roles SPEC-007 assigns to
analysers and to external builds, the steps that run for minutes.

### 3.1 Measured on a first build

A probe (a root package, a path dependency with two module units, `import
std`, clang 22, ninja 1.12.1) built from an empty directory:

- `%t` read 12 on the first seven status lines and 13 from the eighth on:
  loading a dyndep file added the step that compiles `std.pcm`;
- the graph holds 14 steps, and the build ran 13: `std.compat.pcm` is never
  built unless something imports it;
- `std.o` and `std.compat.o` ended in the same millisecond; the log tells
  them apart by their command hashes.

A criterion of the form "`%t` equals the graph's number of steps, so every
step runs" is therefore never met, and a package's planned number of steps
is not knowable from `%t`.

### 3.2 The rule that follows

**A package is complete when every step the graph assigns to it has finished
in this build.** The rule needs no knowledge of ninja's plan:

- in a first build it is met as the package's last step finishes;
- in an incremental build a package rarely re-runs every step, and its
  outcome is then stated when ninja exits;
- a step the graph assigns to a package and that is never built (the
  standard library's `std.compat` module in the probe) keeps the package open
  until ninja exits. The rule can only state completion late, never early.

A package is never shown as `waiting`, because mcpp cannot tell a package
whose first step is running from one whose steps have not started. `waiting`
is stated only where mcpp itself decides the order: build programs, which
mcpp runs one after another.

## 4. The output

### 4.1 Two layers

The output has two layers:

- **the log**: lines that do not change once they are written;
- **the status line**: always the last line. On a terminal it is rewritten
  in place. In a log it is repeated when the log has been silent (section 5).

A step line belongs to the log once its outcome is known. On a terminal it is
shown between the log and the status line while it runs.

### 4.2 Step lines

A step line has the shape

    <verb, right-aligned in 12 columns> <subject>  <state>

The state begins two columns after the longest subject of its block, and at
column 56 at most; a longer subject is followed by two spaces. A block is the
set of lines of one kind: the build programs, the packages.

| Verb | Subject | Live states (terminal) | Final states |
|---|---|---|---|
| `Downloading` | the package | the existing bar | unchanged: `done, 211.9 MB in 5.0s` |
| `build.mcpp` | the package | `waiting`, `compiling 0:02`, `running 0:05` | `ran 7.52s`, `cached`, `failed` |
| `Compiling` | the package: `gpp.core (GalTranslPP)`, `fmt v11.0.2`, `x (path)`, `y (git tag v1)` | `61 steps` | `done 3m12s`, `cached 12 units`, `failed`, `12 steps`, `fresh` |
| `Compiling` | `23 dependencies` | `412 steps` | `done 1m12s`, `done 1m12s · 5 cached` |

The meaning of each state:

- `done <span>`: every step of the package that ran succeeded, and the
  package is complete (section 3.2) or the build has ended. The span runs
  from the start of its first step to the end of its last, both read from
  ninja's log.
- `ran <time>`: a build program ran. Its compile time is included. Verbose
  output states the two parts: `compiled 3.21s · ran 7.52s`.
- `cached`: the result came from a cache: a build program's result, or a
  dependency whose units were staged from the global BMI cache
  (`cached 12 units`, the number the `Cached` line states today).
- `waiting`: a build program that mcpp has scheduled and not started.
- `failed`: a step of the package failed.
- `12 steps`: the steps of the package that ran. On a terminal it counts up
  while the package is open. After a failed build it is the final state of a
  package that did not complete, because whether it had steps left is not
  known (R7).
- `fresh`: the package had nothing to do. Shown only with `--verbose`.

The verb stays `Compiling` after the step is finished: the verb names the kind
of step, and the state names its outcome. The download line keeps its
wording: it is already one line with its outcome, and matching it to the
state column would change a line that has no defect.

### 4.3 Folding (R4)

The **requested** packages are the root package of a single-package build, or
the selected members of a workspace. Every other package is a
**dependency**: index packages, path dependencies that are not selected, git
dependencies, and the standard library module that `import std` compiles.

- The default output lists each requested package that has work. The
  dependencies with work are folded into one line, `Compiling 23 dependencies`,
  which is final when every one of them is complete, or when ninja exits.
- A dependency that fails is always listed by name.
- A package with nothing to do is not listed. Before this change the root's
  direct dependencies were announced as `Compiling` although nothing compiled.
- Build programs follow the same rule: the programs of requested packages are
  listed, and those of dependencies are folded into `build.mcpp 3 dependencies`.
- `--verbose` lists every package, including those with nothing to do
  (`fresh`), and the per-dependency `Cached … (N units)` facts.

### 4.4 The status line (R2)

    <phase>[ n/m] · <elapsed>[ · <current>]

- The phases are `Resolving`, `Running build programs`, `Building f/t`,
  `Stopping` (a step failed and ninja waits for the steps still running) and
  `Checking` (the validations after ninja).
- `<elapsed>` is the time since the command started, as `m:ss` or `h:mm:ss`.
  It therefore ends equal to the total that `Finished` states.
- `<current>` is the longest-running step that mcpp knows to be running, with
  its own time: a build program, or a `check` or `prepare` action. It is
  omitted when no such step runs.
- The line starts at column 0, is not aligned with the step lines, and is
  truncated to the width of the terminal.

### 4.5 `Finished` (R3)

    Finished fast-release [unoptimized + debuginfo] in 41m53s · plan 1m13s · programs 32s · build 40m08s · longest gpp.gui: vcpkg install 22m10s

- The time is the whole command's. Until now it was ninja's alone.
- `plan` (resolution, provisioning, planning), `programs` (build programs)
  and `build` (ninja and the validations after it) are listed when the command
  took at least ten seconds and more than one of them took time. Their sum is
  the total. A shorter command has no wait to explain.
- `longest` names the longest single step when the build phase took at least
  ten seconds and that step took at least a quarter of it. Below that, time
  is spread over many steps, and naming one would misdirect the reader.
- `Finished` ends the report: the region is erased before it and is not drawn
  again below it.
- One format for every duration: `0.84s` and `12.34s` below a minute,
  `3m12s` below an hour, `1h02m` above it. The status line uses a clock:
  `14:32`, `1:02:10`.

### 4.6 Examples

The numbers below are those of run 36562019799 where the run's log states
them. The per-package spans and the vcpkg time are illustrative.

**A terminal, during the build** (a first build):

```
   Workspace building 5 members: gpp.cli, gpp.core, gpp.gui, gpp.updater, gpp.version
    Resolved llvm@22.1.8 → @mcpp/registry/data/xpkgs/xim-x-llvm/22.1.8/bin/clang++.exe
  build.mcpp gpp.core                     ran 16.00s
  build.mcpp gpp.cli                      ran 4.60s
  build.mcpp gpp.updater                  ran 4.60s
  build.mcpp gpp.gui                      ran 6.70s
   Compiling gpp.version (GPPVersion)     done 2.10s
   Compiling gpp.core (GalTranslPP)       done 3m12s
   Compiling gpp.cli (GPPCLI)             done 41.20s
   Compiling gpp.gui (GPPGUI)             61 steps
   Compiling 23 dependencies              412 steps

Building 612/1203 · 14:32 · gpp.gui: vcpkg install 6:10
```

The first nine lines are in the log. The two `Compiling` lines without a
final state and the status line are redrawn in place. gpp.updater has no
finished step yet and is not shown.

**A log (CI), the same build**:

```
   Workspace building 5 members: gpp.cli, gpp.core, gpp.gui, gpp.updater, gpp.version
    Resolved llvm@22.1.8 → @mcpp/registry/data/xpkgs/xim-x-llvm/22.1.8/bin/clang++.exe
  build.mcpp gpp.core                     ran 16.00s
  build.mcpp gpp.cli                      ran 4.60s
  build.mcpp gpp.updater                  ran 4.60s
  build.mcpp gpp.gui                      ran 6.70s
   Compiling gpp.version (GPPVersion)     done 2.10s
Building 214/1203 · 2:32 · gpp.gui: vcpkg install 0:52
Building 388/1203 · 3:32 · gpp.gui: vcpkg install 1:52
   Compiling gpp.core (GalTranslPP)       done 3m12s
   Compiling gpp.cli (GPPCLI)             done 41.20s
Building 612/1203 · 5:02 · gpp.gui: vcpkg install 3:22
…
   Compiling gpp.updater (Updater)        done 12.40s
   Compiling 23 dependencies              done 6m20s
   Compiling gpp.gui (GPPGUI)             done 38m05s

    Finished fast-release [unoptimized + debuginfo] in 41m53s · plan 1m13s · programs 32s · build 40m08s · longest gpp.gui: vcpkg install 22m10s
```

**An incremental build** (one source of gpp.gui edited):

```
   Compiling gpp.gui (GPPGUI)             done 4.12s

    Finished fast-release [unoptimized + debuginfo] in 4.31s
```

**A failure**:

```
   Compiling bad v0.1.0 (.)               failed
error: build failed
src/main.cpp:1:2: error: #error MARKER
```

The three lines are printed when the step fails, not when ninja exits. While
ninja waits for the steps still running, the status line reads
`Stopping · 0:03 · gpp.gui: vcpkg install 6:10`. The advice blocks that read
the whole output (link failure, C library isolation) follow after ninja exits.

**`--verbose`**: the lines above, every dependency on its own line, and above
them each step as ninja reports it, `[f/t] <command>` followed by its output,
printed when the step finishes rather than after ninja exits.

## 5. The two media

### 5.1 A terminal

The region is the part of the screen below the log: the live lines, one
blank line, and the status line. The live lines are the download bars, the
build programs not yet final, and the packages still open.

- Every other line mcpp writes goes above the region: the region is erased,
  the line is written, and the region is redrawn, in one write. This covers
  warnings, errors, notes and the step lines that become final.
- The region holds at most `min(10, rows − 3)` live lines. The rest are
  summarised as `… 12 more`.
- Every line of the region is truncated to the width minus one column, in
  display columns (colour sequences count zero, an East Asian wide character
  two), so that no line wraps and the cursor arithmetic holds after a resize.
- The region is drawn at most ten times a second when events arrive, and once
  a second otherwise, so that the clocks advance.
- When the command ends, the region is erased, and the final step lines and
  `Finished` are written to the log.

Whether the terminal medium is used is decided when the region opens, from
the descriptor the region writes to (section 8).

### 5.2 A log

Only final lines are written. The status line is written, without a blank
line, when the log has been silent for 60 seconds, so that a long step shows
what is happening at least once a minute. `Finished` is preceded by a blank
line, as on a terminal. A command that wrote no line before `Finished`
writes no blank line.

### 5.3 Lines that do not come from the renderer

The region is drawn relative to the cursor, so a line written to the terminal
by anything else while the region is on screen lands inside it. Three
measures keep that from happening:

- every line mcpp writes on the build path goes through `mcpp.ui`: the
  command handlers' `std::println(stderr, "error: …")`, the failure report's
  `fputs`, and the lines of `mcpp.log`'s verbose channel, which `mcpp.ui`
  receives through a sink it installs;
- a child that is given the terminal (a `[hooks]` command, the program
  `mcpp run` starts) runs with the region erased, and the region is redrawn
  after it returns;
- a command with a `[hooks] during_build` command that writes to the
  terminal while the build runs uses the log medium.

### 5.4 Verbosity

`--verbose` writes ninja's lines as they arrive, above the region, in either
medium. `--quiet` and `--message-format json` write nothing, and none of the
readers of section 6 runs.

## 6. Sources

### 6.1 ninja's standard output

- ninja is started with `NINJA_STATUS="\x1b[0m@@mcpp %f %t %e@@ "` and
  `CLICOLOR_FORCE=0` in its own environment and without `--quiet`, and its
  output is read line by line as it arrives. The marker begins with an escape
  sequence because a step's command inherits ninja's environment: a ninja it
  runs (a `prepare` action's CMake or vcpkg build) prints the same marker, and
  the outer ninja, whose output is not a terminal, strips escape sequences
  from a command's output before relaying it while printing its own status
  line as it is. Only the outer ninja's lines keep the escape sequence. The bounded launcher already delivers lines to a callback
  (`dispatch_bounded`, used by `run_streaming_bounded`). Its contract gains
  one case: with a callback and no bound, the child runs unbounded.
- A line is one of: a status line (it starts with the marker), `FAILED: `
  followed by the step's outputs, a line of ninja's own (`ninja: …`), or
  output of the step whose status line came last.
- The default output discards the output of a successful step, as it does
  today, and filters a failed step's block with `filter_ninja_output` and
  prints it at once. `--verbose` prints every line, with the marker replaced
  by `[f/t] `.
- The staged-cache pass (`_mcpp_staged_cache`), which copies what the global
  cache supplies before the main pass, runs as before and is not read: a
  dependency the cache supplied whole is stated as `cached` from the plan
  (`BuildPlan::packages`), not from its copies.

### 6.2 ninja's log

ninja appends one entry per output of each successful step: start, end,
modification time, output path and command hash. The entries of one step
share start, end and hash, and the entries of a run follow the order of its
status lines.

- The log alone states which step finished, when it started and when it
  ended. The status lines give the counts and pace the reading.
- The entries of this run are those after the offset the file had when ninja
  started. ninja rewrites the file when it recompacts it, which it does while
  loading, before the first step; the reader then sees a different file, or a
  shorter one, and finds this run's entries by their end times, which equal
  the `%e` of the status lines (`Builder::FinishCommand` hands one value to
  both).
- The log is read after each status line and on each tick, from the last
  position read. A read can land between two entries of one step (ninja
  flushes each), so a step is counted by its identity in the step record,
  once, however many reads its entries take.
- A log that cannot be read or matched gives no durations and no attribution.
  The counts still come from the status lines. The display loses detail; it
  does not state anything false.

### 6.3 The step record

The function that writes `build.ninja` writes one more file beside it,
`steps.tsv`:

- per package: its qualified name, whether it is requested or a dependency,
  its subject as its line shows it, the number of units it takes from the
  global cache, and the number of steps the graph assigns to it;
- per output of every step: the package the step belongs to, and the step
  itself (the outputs of one step share its number);
- per `check` or `prepare` action: its label (the action's description, or
  its id).

**Attribution.** The emitter knows, at each build statement it writes, which
package's unit, link, action or staged file the statement is for. It records
that package with the statement's outputs as it appends the statement: the
compile unit's package, the link unit's (`LinkUnit::package`: the member's,
the root's, or the dependency's whose library or program it is), the resource
script's (`ResourceUnit::package`), the action's declaring package, `std` for
the standard library's module, and none for the steps that belong to the build
as a whole (the macOS `ios_base` shim, the files placed beside the programs,
and `std.compat`, which is built only when something imports it and would
otherwise keep `std` open).

**Naming.** The plan names each package (`BuildPlan::packages`) as the edge
from the root, or from its first requester, spells it: the dependency key,
then `vX.Y.Z`, `(path)`, `(git <kind> <ref>)`, or a member's directory. The
root is `name vX.Y.Z (.)`. Transitive dependencies are named too; before this
change they were not announced at all.

The full path and the fast path read the same file; the fast path reads it
lazily, when ninja reports its first step. The record changes nothing in
`build.ninja`.

### 6.4 The start of a `check` or `prepare` action

ninja is started with `MCPP_ACTION_STARTS=<build directory>/.mcpp-action-starts`,
truncated before each run. When the variable is set, `mcpp __action` and
`mcpp __action-stamp` append one line, `<first stamp>\t<start time>`, before
running the command. The first stamp is the step's first output, so the step
record gives the action's package and label.

- The start is not passed on the command line, because an action's command
  line is ninja's key for re-running it: the positional `__action-stamp` form
  exists to keep that key unchanged, byte for byte. The environment is not
  part of the key, so an upgrade re-runs no action.
- Every mcpp that runs ninja sets the variable for that ninja, or empties it
  when it reads no progress (the staged-cache pass, `--quiet`, machine
  output). The wrapper empties it, and resets `NINJA_STATUS` to ninja's
  default, for the command it runs, unless the action declares either. A
  nested build therefore reports to its own file or to none.
- One line is one write, in append mode (`O_APPEND`, `FILE_APPEND_DATA`), so
  the lines of actions running at the same time do not interleave.
- An action is running from its start line until its step appears in the log
  or fails.

### 6.5 mcpp's own steps

Downloads, build programs and the phase boundaries are reported by the code
that performs them.

## 7. Architecture

```
  mcpp's own steps ──────────────┐
  ninja stdout ─► stream reader ─┤
  .ninja_log   ─► log reader ────┼─► mcpp.build.progress (model) ─► mcpp.ui region ─► terminal / log
  action starts ► start reader ──┤        ▲
  steps.tsv    ──────────────────┘        └─ clock (injected; steady_clock in use)
```

- **`mcpp.platform.terminal`**: whether a descriptor is a terminal that can
  move the cursor, its width and height, enabling virtual terminal
  processing on Windows, and writing UTF-8 text to a Windows console.
- **`mcpp.ui`** (existing): every human line. It gains the region, and
  `ProgressBar` becomes a live line of it. That realises the rule its header
  already states, one renderer with two output modes, for every live element
  rather than for one bar.
- **`mcpp.build.progress`** (new): the model (phases, packages, steps,
  running actions, counts, spans), the formatting of every line from it, and
  the three readers of section 6, each of which turns one file format into
  model events. It performs no terminal I/O, and its clock is injected, so
  every rule of section 4 is testable without a terminal.
- **The backend** writes `steps.tsv` from every plan, and, given a handle to
  the model's record of its build through `BuildOptions`, runs ninja through
  the readers.
- **`mcpp.log` and the terminal.** `mcpp.log` stays a leaf that depends on
  `std` alone: it states records (a tag, a message, a time), writes the log
  file, and spells a verbose record once (`verbose_line`). The terminal
  belongs to `mcpp.ui`, which installs a sink for verbose records at start-up
  and writes them through the same writer as every other line, so a verbose
  line lands above the region. Without the sink (a unit test, a tool that
  does not use `mcpp.ui`) the record goes to stderr as before. The model
  writes its events to the log file (`progress` records at `info`): the
  build programs, each build's step counts and outcome, so the file holds a
  trace of what the report showed.
- **Threads.** The model is guarded by one mutex. The configuration groups of
  a workspace build on their own threads (workspace design §6) and feed the
  same model, so their counts are summed in one status line. One ticker
  thread draws the region, or writes the heartbeat in a log. The ticker
  takes the model's frame without holding the renderer's lock, and events
  signal the ticker without taking it, so the two locks are never held
  together.
- **Lifetime.** The region belongs to the command. Closing it erases the
  region before `mcpp run` starts the program and before the command returns.

## 8. Platforms

| Platform | Terminal | Cursor and colour | Text | Width |
|---|---|---|---|---|
| Linux | `isatty` (unchanged) | ANSI | UTF-8 | `TIOCGWINSZ`, `COLUMNS`, 80 |
| macOS | `isatty` (today: always false) | ANSI | UTF-8 | `TIOCGWINSZ` |
| Windows console | `GetConsoleMode` | `ENABLE_VIRTUAL_TERMINAL_PROCESSING`; if refused, the log medium without colour | UTF-16 through `WriteConsoleW` | `GetConsoleScreenBufferInfo` |
| Windows, mintty or a pipe | not a console | the log medium | bytes, as today | none |

- `TERM=dumb`, or a terminal narrower than 20 columns or lower than 5 rows,
  uses the log medium.
- On Windows the lines of `mcpp.ui` are written as UTF-16 when the
  descriptor is a console, so `·`, `→`, `…` and non-ASCII paths appear as
  written whatever the console's code page. Today the console decodes mcpp's
  UTF-8 bytes in its own code page. The standard output buffer is flushed
  before each such write.
- ninja reports steps only when they finish, through a pipe, on all three
  platforms (the same `StatusPrinter` code), and flushes its log after each
  entry on all three.
- An interrupted command leaves its last frame on the screen; the next line
  the shell prints starts after it.

## 9. Cases

| Case | Behaviour |
|---|---|
| first build | packages final as they complete (section 3.2) |
| incremental build | step counts; packages final when ninja exits; packages with nothing to do not listed |
| nothing to do (fast path) | no step line, no blank line, `Finished` |
| a failed step | its package `failed`, `error: build failed`, its diagnostics, at once; `Stopping` until ninja exits; then the other packages (`done`, or `N steps`) and the advice |
| several failed steps (`-k 0`, or failures at the same time) | each failed step's diagnostics once; `error: build failed` once |
| the build timeout | the existing message |
| Ctrl-C | the last frame stays on the screen |
| several configurations | one status line; each package line carries the configuration's directory name when there is more than one, as SPEC-005 v1.6 names the database's sets; one `Finished` |
| the staged-cache pass | runs before the main pass, unread; cached dependencies are stated from the plan |
| `mcpp run` | the region is closed before `Running`; the program owns the terminal |
| `mcpp test` | the report covers planning, build programs and the package's own build (Phase A); it is closed before the tests' own builds and runs, whose lines are unchanged |
| `mcpp pack` | the status line during its build; `Packing` and `Packed` unchanged |
| `emit build-database` | the region opens inside the standard-output redirection and is decided on that descriptor, so the document on standard output stays clean |
| `--message-format json`, `--quiet` | nothing written; no reader runs |
| `--verbose`, `MCPP_VERBOSE=1` | section 5.4 |
| a `[hooks]` command | runs with the region erased (section 5.3) |
| a nested mcpp (in a build program or an action) | it reports to its own start file (section 6.4) |
| an older ninja from `PATH` | status lines as above; if its log is not flushed per entry, attribution waits until ninja exits; log formats v5 and v6 are read, and any other gives no durations |
| the sandbox (`xlings subos … --sandbox`) | a log medium; nothing depends on the host |
| a terminal resized during the build | width and height are read at each frame |

## 10. Compatibility and upgrade

- **No command line changes.** The commands in `build.ninja` are unchanged,
  so upgrading re-runs no step. The step record is a new file beside
  `build.ninja`, and the descriptions in `build.ninja` are unchanged.
- **The human output changes.** It is not an interface; the machine output
  is (docs/50), and it is unchanged. The changes are:
  - `build.mcpp compiling/running/up to date X (cached)` becomes one line,
    `build.mcpp X  ran …` or `cached`;
  - a package with nothing to do is no longer announced;
  - dependencies are folded unless `--verbose` is given, including the
    `Cached X (N units)` lines;
  - `Finished` states the whole command's time and its distribution.
- **mcpp's own tests.** The e2e tests that match these lines change with
  them:
  - `build.mcpp running` or `build.mcpp compiling` (110, 143, 144, 186, 189,
    194, 325, 839);
  - `up to date … (cached)` (139, 186, 651, 779, 826);
  - a dependency's `Compiling` or `Cached` line (19, 40, 49, 53, 82, 135, 163,
    172, 196, 212, 713, 839 B5), which add `--verbose`. Test 713 asserts the
    absence of a line, and would pass vacuously once dependencies are folded,
    so it changes in the same commit;
  - the tests that read any `Compiling` line as "the full path ran" (218, 322,
    324, 612, 638, 781, 821). Under this change the line means "a step of the
    package ran", on either path. 322, 612 and 638 meant that, and are
    unchanged; 218, 324, 781 and 821 meant "the build planned", and now ask
    whether `build.ninja` is newer than a marker written before the build:
    the full path writes it or moves its time, and the fast path does not;
  - test 48 keeps its ordering: the package line comes before
    `error: build failed`, and the diagnostic appears once.
- **The ecosystem.**
  - mcpp-plugins' CI asserts `! grep -q 'build.mcpp running'` in
    `check-deps-and-qt.sh` (the qt import-only case). After this change that
    assertion passes whether or not a program ran. It changes in the same
    pull request that moves the plugins' CI to this release, to a check that
    does not depend on wording.
  - mcpp-index's `tests/openkal/compat.py` reads `Running bin/`, which does
    not change.
  - The validation project reads no build line.
- **Upgrade without notice.** A build directory written by an earlier version
  has no step record. Its first build under this version writes one, because
  a version change regenerates `build.ninja`. A fast path that finds no
  record shows counts without attribution.

## 11. Tests

- **Unit tests of the model and the formatting**, with an injected clock:
  - each state of section 4.2, and the completion rule of section 3.2,
    including a package with a step that never runs;
  - folding, and the named failed dependency;
  - the state column and truncation, with CJK subjects and colour sequences;
  - the duration and clock formats;
  - the heartbeat after 60 silent seconds, and none while lines are written;
  - `longest`, at and below its thresholds.
- **Unit tests of the readers**:
  - status lines, `FAILED:` blocks, ninja's own lines and output, in both
    modes;
  - the log: entries of several outputs, two steps ending in one
    millisecond, failed steps (no entry), a recompacted file, formats v5 and
    v6, an unreadable file;
  - the start file: interleaved lines, an unknown stamp.
- **A unit test of attribution**: every step of an emitted plan has an owner
  or is build-wide (section 6.3).
- **Unit tests of the region**: the byte stream for a scripted sequence (a
  line written above the region, the region growing and shrinking, closing).
- **e2e, the log medium, every platform**:
  - a workspace with a dependency: members listed, dependencies folded, the
    blank line and `Finished` with its phases;
  - `--verbose` lists every package and each step;
  - a failure is reported before a concurrently running slow `prepare`
    action ends;
  - build program lines `ran` and `cached`;
  - an incremental build lists only the package that did work.
- **e2e, the terminal medium, Linux and macOS**: through a pseudo-terminal
  (Python's `pty`), a `prepare` action that runs for three seconds is named in
  the status line, and the final screen holds the log lines and `Finished`.
- **Cross-verification**: the validation project's CI log shows the heartbeat
  during the ninja phase and the package lines with their spans.

## 12. Tasks and order

All of them land in #742, with 2026.9.29.5.

| Task | Content | Depends on |
|---|---|---|
| T1 | `mcpp.platform.terminal`: terminal detection on macOS and Windows, virtual terminal processing, width and height, console UTF-16 output | none |
| T2 | the region in `mcpp.ui`; `ProgressBar` rebuilt on it; the sink for `mcpp.log`; the direct writes on the build path routed through `mcpp.ui` | T1 |
| T3 | the launcher: unbounded runs with a line callback | none |
| T4 | attribution in the emitter and `steps.tsv` | none |
| T5 | `mcpp.build.progress`: model, formatting, readers; the start line in `__action` and `__action-stamp` | T3, T4 |
| T6 | integration: phases, build programs, package lines, failure reporting, `Finished`, the fast paths, several configurations, `run`, `test`, `pack`, `emit` | T2, T5 |
| T7 | tests (section 11), and the e2e tests that change with the wording | T6 |
| T8 | docs/09 (the verbose flag and the output), docs/30 (build program lines), the pages whose examples show the output (docs/00, 21, 30, 40, in English and Chinese), CHANGELOG | T6 |
| T9 | cross-verification with the validation project, together with #742's CI | T7 |
| T10 | after the release, mcpp-plugins' CI check (section 10) with its pin bump | release |

## 13. Alternatives not taken

- **Running ninja under a pseudo-terminal**, so that it prints steps as they
  start. On Windows this needs ConPTY, which rewrites the output stream, and
  ninja's smart-terminal output overwrites and elides its own lines. The
  start of a compile is not worth that.
- **Wrapping every step in the engine** to report its start. That changes
  every command line, and ninja re-runs every step whose command line
  changed.
- **`ninja -n` to learn the plan.** It cannot see what `restat` and `dyndep`
  change during the build, and it loads the graph a second time.
- **Comparing `%t` with the graph** to decide that every step runs. Refuted
  by measurement (section 3.1).
- **One completion step per package in the graph.** It would put a display
  concern into the build graph, and a build with explicit goals (`mcpp test`,
  `mcpp pack`) would not reach it.
- **Reserving the bottom rows with a scrolling region (`DECSTBM`)**, which
  makes the status line immune to writes from other processes. A process
  that is killed leaves the terminal's scrolling region set, and the rows
  must be re-reserved on every resize. Section 5.3 keeps writes out of the
  region instead.
- **An estimated time of completion** (ninja's `%W`). It is a prediction from
  earlier runs, not a fact about this one (R7).
- **Printing the warnings of successful steps by default.** This is a separate
  decision; the default output discards them as before, and `--verbose`
  prints them, now as they arrive.

## 14. Self-review

- **Architecture.** Four sources feed one model, and one renderer draws it.
  Each source is a reader of one format. The model does no terminal I/O. The
  renderer knows nothing about builds. The engine's graph and commands are
  unchanged.
- **Stability.** Every exact statement rests on a mechanism ninja documents
  (`NINJA_STATUS`, the log format) or one the engine owns (the wrapper, the
  step record). Completion is stated by a rule that can be late and cannot be
  early. Where a source fails, the display loses detail and states nothing
  false.
- **Simplicity.** One rule for step lines (a line becomes final when its
  outcome is known), one completion rule, one status line, one duration
  format. No new option and no new environment variable for the user.
- **User experience.** Something is visible every second on a terminal and
  every minute in a log. A failure is reported when it occurs. The default
  output shows what was asked for and what it cost; `--verbose` shows the
  rest.
- **Compatibility.** Machine output, commands and build directories are
  unchanged. The changes to human output are listed, and so are the tests and
  the one ecosystem check that read it.
- **Platforms.** The live display reaches macOS and Windows for the first
  time, and the Windows console shows non-ASCII text correctly for the first
  time.
- **Consistency.** Build programs and packages use the same line shape and
  the same states. The status line has the same form in every phase.
- **Upgrade.** Nothing is re-run and no directory is invalidated. A missing
  step record degrades the fast path's display for one build.
- **Coverage.** The rules are unit-tested with an injected clock, both media
  are tested end to end, and the terminal medium is tested through a
  pseudo-terminal on Linux and macOS.
- **Known limit.** A single long compile is not named: ninja does not report
  compiles when they start. It shows as a package whose count does not
  advance, under a status line whose clock does.
