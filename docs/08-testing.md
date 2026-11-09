# 08 — Testing

**Reader:** anyone with code that has to keep working.

**The question this chapter answers:** how tests are written and run, what mcpp
considers a test, and how to test something that does not run on this machine.

**Not here:** how a runner reaches a device — that is
[41 — Reaching a Device](41-devices.md) — and the schema of the machine-readable
stream, which is [50 — Machine-Readable Output](50-machine-output.md). This
chapter states which flag produces it and stops there.

Before: [05 — Dependencies and Resolution](05-dependencies.md) covers
`[dev-dependencies]`, which is how a test reaches a package the artifact does
not. After: [09 — Commands by Scenario](09-commands-by-scenario.md) is the
lookup for everything else.

## The definition of a test

Every `tests/**/*.cpp` is a test: mcpp compiles each one into its own program
and runs it. A test passes when its program exits zero.

```
myproject/
  mcpp.toml
  src/…
  tests/
    test_parse.cpp        one program
    unit/test_span.cpp    another
```

There is no framework and no registration. A test may use one — `[dev-dependencies]`
is how it reaches it — but the contract mcpp holds is the exit code, which is
also why a test written for another framework needs no adapter.

`mcpp new` scaffolds `tests/test_smoke.cpp`, a file of
[mtest](https://github.com/mcpp-community/mtest)'s cases with no `main`, and the
dev-dependency `mcpplibs.mtest` (2026.11.8.1+); `mcpp new <name> --template lib`
scaffolds a library and its test the same way ([01](01-getting-started.md)).

### Where tests are

`tests/**/*.cpp` is the default of one key:

```toml
[test]
discover = ["checks/**/*.cpp", "!checks/fixtures/**"]
```

`discover` takes globs in the vocabulary of `[build] sources`: every file a
glob matches is one test program, and a glob beginning with `!` removes the
files it matches from the set, whichever glob found them. A test's name is its
path relative to the fixed directory of the first glob that matched it, without
the extension, so the default names `tests/unit/test_span.cpp` `unit/test_span`.
`discover = []` discovers no test. Two files whose names coincide are refused,
naming both.

`[test] programs = "directory"` (2026.11.8.1+) makes the files of one directory
below a glob's fixed directory one program, named by the directory
(`tests/unit/*.cpp` is the program `unit`): each file is a translation unit of
it, its cases are all of theirs, and it begins at the file that defines `main`
when one does, else as a file that defines none does. Two files of one directory
that define `main` are refused, naming both; a file directly in the fixed
directory stays a program of its own. A test's name is the path of its program,
so a name that is another's directory (`unit` and `unit/own`) is refused,
naming both (2026.11.9.1+). A program of many files is linked once
where a program per file is linked once per file, which is what a suite of many
small files spends its build time on.

A program that speaks the test protocol runs as **one process per file of its
cases** (2026.11.9.1+): mcpp lists the cases of a program of several files, and
each file's run selects its own (P4) and is scheduled across the workers as a program is, named
`<program>/<file>` -- the isolation and the parallelism of a program per file,
with one link. `[test] split = "case"` gives each case its own process, and
`"none"` runs the program once. A pattern names a program or a part of one as
the run names it: `mcpp test unit/test_home` (or `home`) runs that file's part.

A test program keeps what it reaches (2026.11.9.1+): units are compiled with one
section per function and per object, and a test links with `--gc-sections`
(`-dead_strip` on Mach-O), so a test of one module does not carry its package's
every function (mcpp's own unit tests: 32 MB each to 14 MB). Static
constructors are the collection's roots. `[test] link = "objects"` keeps every
object whole.

A run is kept in the form of the mtest store (2026.11.9.1+): `target/mtest/`
holds `runs/<id>/run.json`, a line per process in `programs.ndjson` (its
status, duration, start, worker and selected cases) and a line per case in
`cases.ndjson`, and `index.ndjson` lists the runs. `mtest store import --from
target/mtest` takes them into mtest's store, and an `mtest.toml` whose
`[project] store` is `target/mtest` reads them where they are; `mtest report`,
`analyze` and `graph` then read them as mtest's own runs (the Markdown report,
the slowest cases, the critical path and the schedule). In CI, `commit` and
`group` are `GITHUB_SHA` and `GITHUB_RUN_ID`. `[test] store = "<dir>"` names
another directory, relative to the project; `"none"` keeps nothing.

A suite compiled from several sources of its own is also a package: a workspace
member whose `[build] sources` carries the suite and whose one test program
drives it, selected with `mcpp test -p <member>`.

### Where a test program begins

A test framework that supplies `main` declares it as an entry target
(`kind = "entry"`, docs/04 §2.2), and the package names it once for every test:

```toml
[dev-dependencies]
fw = "0.1"

[test]
main = { from = "fw/main" }
```

Each test file is then an ordinary translation unit of its program and defines
no `main`. The package's own name selects its own entry, which is how a framework
tests its entry with the callbacks a test file defines. On a freestanding target,
`[test] start` states the symbol every test image is entered at.

An entry may offer itself with `test-main = true` (2026.11.8.1+): a test file that
defines no `main`, of a package that states no `[test] main`, then begins at it,
and a file that defines `main` keeps its own. mtest's entry and its GoogleTest
adapter's offer themselves, so a test needs only the dev-dependency. The choice
is the file's: an offer whose package provides modules is this test's when the
file imports one of them and is set aside when it imports none, so in one
package a file of mtest's cases begins at mtest's entry and a file of
GoogleTest's at GoogleTest's. A dependency's source that defines `main`
(`gtest_main.cc`) begins a test as it always has. Where two beginnings remain
for one test, the build is refused, naming them, and `[test] main` chooses.

## Running them

```bash
mcpp test                 # build and run every test
mcpp test parse           # only those whose name matches
mcpp test --list          # what would run, without building or running it
mcpp test -- --verbose    # everything after `--` goes to each test binary
```

Tests build with the same axes as `mcpp build`, so a test runs against the
configuration it is meant to check rather than against the default one:

| flag | the set it selects |
|---|---|
| `--profile <name>` | `dev` (default), `release`, `dist`, or a `[profile.*]` the manifest declares |
| `--features <list>` | the feature set for the test build |
| `--target <triple>` | a target other than the host |
| `--accel <spec>` / `--no-accel` | the device backends the build targets |
| `--cap <list>` | pin a capability provider |
| `--toolchain <spec>` | the toolchain for this invocation, e.g. `llvm@23.1.3` |

`--timeout <secs>` kills a test still running (default 300; `0` disables it) and
`--build-timeout <secs>` bounds the compile. A test that hangs is reported as a
failure with its own name, not as a job that stopped.

The report is the command's result and is written to standard output: each
test's verdict, the output of a test that failed, the `test result` line and, for
`--workspace`, the `workspace result` line. The build's steps (`Compiling`,
`Running`) are narration and are written to standard error, so `mcpp test > report.txt`
collects the report, and `mcpp test 2>&1 | tee test.log` collects both
([09 — Commands by scenario](09-commands-by-scenario.md#output-streams)).

### The tests a change affects

`mcpp test --since <commit>` and `--changed <path>` (2026.11.8.1+) build and run
the tests a change affects, read from the plan before anything is built:

- a test is affected by a change of a file it is built from: its own files, the
  module units it imports and the units those import in turn, and every unit
  of its link that belongs to no module;
- a change of a manifest, a lock, a build program, a header, or a source the
  plan does not hold affects every test;
- another file affects the tests whose own files name it (a fixture), and no
  other test.

`--since` reads the change from git: the paths committed since the commit,
changed in the working tree, and new and not ignored. `Affected N of M tests`
states the selection, with what each changed path decided; under
`--message-format json` an `affected` record lists each test with its reason.
The selection narrows what a pattern or `-p` names and adds nothing to it.

**A test finds the package's programs by name** (2026.10.27.1+). `mcpp test` builds
the package's programs first, as `mcpp build` does, and gives each test
`MCPP_BIN_<NAME>`: the path of program `<name>`, upper-cased, every character
other than a letter or a digit written `_` (`MCPP_BIN_MY_TOOL` for `my-tool`).
A test that runs the program it tests reads it from there.

**A platform package is tested on its targets** (2026.10.27.1+). `mcpp test` in a
package with a `[platform]` section, with no `--target`, builds the tests for
each of the platform's `targets` with the package itself as the platform. The
targets its runner can run on this machine are run; the others are built and
reported as not run ([35](35-platforms.md)).

### In a workspace

```bash
mcpp test -p core -p http              # the tests of two members
mcpp test --workspace --exclude legacy # every member's but legacy's
```

`-p` may be repeated, and `--exclude` leaves members out of `--workspace` or of
a virtual root's whole-workspace selection; both are resolved as
[07 — Workspaces](07-workspace.md#53-the--p---package-option) states. A test over
several members plans them together and builds once: the members of one
configuration are one build graph, so a package they share is compiled once,
and its build program runs once. Then each member's tests run, in
`[workspace] members` order, each with that member's own runtime directories.
The command continues past a member that fails, whether a test, its package's
build or its plan failed, reports each member, and exits non-zero if any did.
Discovery is scoped per member, so two members may each have a `tests/main.cpp`.

## Tests that reach packages the artifact does not

```toml
[dev-dependencies]
counters = { path = "../counters" }
```

A `[dev-dependencies]` entry is resolved for the test build and for nothing
else: it is not in the artifact, and a consumer of the package never sees it.
That is the difference between a test's dependency and the package's own, and it
is why a test framework does not become part of what a library ships.

A dev-dependency reaches the test units and nothing else of the package
(2026.11.8.1+). Its include directories and interface definitions are given to
the files under `tests/`; the package's own sources are compiled under
`mcpp test` with the commands `mcpp build` gives them, so the code under test is
the code that ships, and `mcpp build` and `mcpp test` share one build directory
and one compile database. A kernel interface that only a dev-dependency reaches
(`mcpplibs.mtest` over `mcpplibs.os` reaches openkal's) is given to the
dev-dependencies and the test units alone: they see `__OPENKAL__` and openkal's
headers, and the package's own sources do not. A C library or a C++ runtime that
a dev-dependency supplies is a property of every object a test links, and it
remains the configuration's.

[`examples/11-features`](../examples/11-features/) declares one and uses it.

## Testing on a target this machine cannot run

A test for a cross target or a bare-metal board is compiled for that target and
executed through a **runner** — an argv a board-support package supplies, which
mcpp performs with the test binary appended.

```bash
mcpp test --target thumbv7em-none-eabihf     # built for the board, run through its runner
mcpp test --no-runner                        # ignore the runner and execute directly
mcpp test --target aarch64-macos --no-run    # build the tests for the target and stop
```

Nothing about the test changes. The same `tests/**/*.cpp` compiles for the
device, and the verdict is still the exit code — which is why a semihosting exit
or a QEMU exit code is what a bare-metal runner is chosen to produce.

`--no-runner` exists for a host that can execute the binaries natively and
should not pay for an emulator.

`--runner <name>` (2026.11.7.1+) selects a named runner instead of the default,
using the same declarations and argument substitution as `mcpp run --runner`.
It also applies to `--print-launch` and tests read from an archive. An absent
runner name is refused; `--runner` and `--no-runner` cannot be combined.

For a platform that declares `debuggee`, this builds one test and prints the
command a debugger starts, without starting the test:

```bash
mcpp test unit/math --runner debuggee --print-launch
```

`--no-run` makes the narrower claim, and it has to be asked for. Without it, a
target this host can neither execute nor reach through a runner leaves every
test not run and the command exits 2: mcpp did not establish whether the tests
pass, and reporting that as success is the false reading this repository has
recorded most often. But 2 is also what a broken runner returns, so a caller
that wanted only the build could not tell the two apart. Under `--no-run` every
selected test is compiled and linked for the target, none is executed, and the
result says so:

```
test result ok. 0 passed; 0 failed; 2 built, not run
```

A test that does not compile is still a failure, and `--no-run` with
`--no-runner` is refused rather than resolved: one says to run the binaries
without the declared runner, the other says not to run them.

A test program carries the files it reads beside it: the runner receives
`MCPP_RUNTIME_FILES`, the list of its deployed files and the shared libraries it
loads, and a runner that moves the program to a device copies them with it. A
test locates such a file relative to its own directory. On the Android rows a
test program links the C++ runtime statically unless `cxx_runtime` says
otherwise, so it needs no `libc++_shared.so` on the device.

The runner itself, named runners, and what a board declares are
[41 — Reaching a Device](41-devices.md).

### Built once, run elsewhere (mcpp 2026.10.30.1+)

An ok image is the same bytes on every system, and a test built for
`x86_64-windows-gnu` on Linux runs on Windows. `--archive` separates the two
halves:

```bash
mcpp test --target openkal --no-run --archive tests.tar.gz   # build every leg's tests, write them
mcpp test --archive tests.tar.gz                             # in a checkout: run what this machine reaches
```

The archive holds each leg's test programs and `mcpp-tests.json`, which names
each test's leg, arguments, working directory and the test protocol it
implements. Reading it builds nothing; the tests run in the checkout, through
this machine's runner or system, by `mcpp test`'s own runner: its cases, a
process per file of them, the workers and the store (2026.11.9.1+). A failed test exits 1, and so does an archive of which
no test ran. Legs this machine does not reach are reported as not run
([SPEC-015 §4](specs/target-sets.md)). The archive is a tar compressed as its
name states: `.tar.gz` (or `.tgz`), `.tar.zst`, `.tar.xz` or `.tar`; another
name is refused before anything is built.

## Tests that cannot run beside each other

`mcpp test` runs test programs on a worker pool. One board on one probe, one
GPU, one serial port or a single-seat licence admits one user at a time, and two
workers reaching for it interleave rather than fail.

The package that owns the resource states this about itself, and `mcpp test`
then serialises those tests. A project never has to remember `-j1`.

## Cases: the test protocol

A test program may consist of cases. mcpp does not provide a framework for
them; it implements the runner's side of a protocol that frameworks implement:
the [mtest test protocol](https://github.com/mcpp-community/mtest/blob/main/spec/protocol.md),
version 1 (mcpp 2026.11.8.1+). A program implements it when its entry states it,

```toml
[targets.main]
kind          = "entry"
main          = "src/main.cpp"
test-protocol = 1
```

or when its file carries the marker `mtest-protocol=1`, as a program built
with mtest's libraries does; of a test that keeps what it reaches, an object it
was linked from carrying the marker counts as well, so a marker nothing
references (mtest before 0.5.1) is not lost at the link. A program that carries the marker and writes no
`program` event spoke no protocol: a file of plain `main` linked beside a
framework's library is one case, judged by its exit status, run and listed. mcpp then gives the program the protocol's
variables (`MTEST_PROTOCOL`, `MTEST_MODE`, `MTEST_REPORT`, `MTEST_TMPDIR`, and
`MTEST_SELECT` and `MTEST_SEED` when they apply), reads the report it writes,
and states each case's result:

```bash
mcpp test                       # each case's result in the report and the record
mcpp test --case 'fs/*'         # of each such program, the cases a pattern matches
mcpp test --cases               # each program's cases, running none
mcpp test --seed 42             # the seed of anything random in the cases
```

- The program's exit status still decides its verdict. A report whose cases
  failed fails the program though it exited zero; a case that began and did
  not end failed, with the program's exit status as its reason.
- A failing program's report names each failed case with the place of its
  first failure: `case fs/read failed at tests/fs.cpp:12: ...`.
- A pattern of `--case` matches with `*` and `?`; a name without either
  matches itself and the cases below it (`fs` matches `fs/read`). A case that
  a listed case declares is reported when it runs and need not be listed
  (protocol P3.4; mtest's `UTest fs{"fs", [] { "read"_test = ...; }}` lists
  `fs`): a name below a listed case selects that one case, and a pattern below
  it runs the listed case whole. Under `--case`, a program with no matching
  case, and every program that implements no protocol, does not run.
- A program that implements no protocol is one case, judged by its exit status,
  and receives none of the variables, even when mcpp's own environment has
  them.

The records of `--message-format json` carry the protocol, the cases and their
failures ([50](50-machine-output.md)); `--print-launch` states the protocol
for a debugger that starts the program.

## Writing a test

```bash
mcpp test --guide            # the frameworks, their guides, and the commands for cases
```

A framework states its guide to writing tests on its entry, `test-guide = "<file>"`
(2026.11.8.1+; the protocol's P2.5): a file of its package, read from the installed
package and of its version. `mcpp test --guide` names each test framework of the
package with its guide and repository, and the commands that list, select and
explain cases; with none, it states the contract above and how to take mtest.
A failed run ends with `how these tests are written: mcpp test --guide (...)`,
and a record of `--message-format json` carries `guide`. A person or an agent
about to write a test, or reading why one failed, starts there.

## Reporting to a program

```bash
mcpp test --message-format json
```

One NDJSON record per test, for a CI job or an editor. The schema and its
version are [50 — Machine-Readable Output](50-machine-output.md); what belongs
here is only that the flag exists and that the human format is the default.

```bash
mcpp test unit/math --print-launch -- --gtest_filter=Math.Adds
```

Builds the one test the pattern names and prints how `mcpp test` would start it
(the `mcpp.launch` envelope, mcpp 2026.10.31.2+), for a debugger to start: the
runner, the environment the run adds and the arguments after `--` are in it.

## Current limitations

- A test is one `.cpp` producing one program. The cases inside it are the
  program's: mcpp lists and selects them only for a program that implements
  the test protocol; for any other, a framework's selection happens inside the
  program, through arguments after `--`.
- `mcpp test --list` over a manifest that does not load lists
  `tests/**/*.cpp`, not the `[test] discover` set it cannot read.
- `--build-timeout` is POSIX-only.
- `--workspace-timeout` bounds the runs of a `--workspace` fan-out and reports
  what did run; it does not attribute the timeout to a member. It is checked
  before each member's tests start, so it cannot interrupt the build the
  members share, which `--build-timeout` bounds.
