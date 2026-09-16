---
subject: triage
status: active
---

# #655 implemented: one reading of a compile-flag element, the plan, and the ledger

This record turns `2026-09-17-655-arguments-are-not-the-command-the-build-runs.md`
("the triage record") into work. The triage record measured that the compile
databases list arguments the compiler never receives, in five spellings, and
that one of the five is a defect of the build itself. Its §3 proposed that the
databases adopt each host's reading. The maintainer asked for the design with
the best cross-platform compatibility; §1 states the design adopted instead and
why it is that design, §2 reviews it from the angles the change touches, §3 is
the ledger, and §5 records what implementing it found.

Base commit: mcpp `51aeee0e` (2026.9.16.2). The engine release is 2026.9.17.1.

## 1. The design

### 1.1 What was measured

The temporary pull request mcpp-community/mcpp#656 ran the released engine
(2026.9.16.1, whose flag handling equals 2026.9.16.2) over fourteen spellings,
one project each, on Linux, macOS and Windows runners (run 35137218111). The
value each macro received (`<08>` is the byte a stringized `\b` becomes):

| element | Linux | Windows |
|---|---|---|
| `-DV=\"esc.h\"` | `"esc.h"` | `"esc.h"` |
| `-DV="mid"` | `mid` | `mid` |
| `-DV='sq'` | `sq` | `'sq'` |
| `-DV='a b'` | `'a b'` | `'a b'` |
| `-DV=a\b` | `ab` | `a<08>` (backslash kept) |
| `-DV=a\\b` | `a<08>` | `a\b` (both kept) |
| `-DV=a$b` | `a` | `a` |
| `defines = ["V='c'"]` | `c` | `'c'` |
| `defines = ["V=\"def\""]` | `def` | `def` |

Every database listed the element text unread (`-DV=\"esc.h\"`, `-DV='a`,
`b'`). The macOS job failed at the link of every probe project under the store
binary, which is a defect of the probe's environment, not a reading; macOS runs
POSIX `sh` under ninja as Linux does, and e2e 736 on the macOS runner of the
engine pull request is its measurement.

Two facts follow. The meaning of an element depended on the host: five of the
fourteen spellings gave different macros on Linux and Windows. And no reading
of the text back can make the databases agree with the build on both hosts
while the element itself means two things.

### 1.2 The adopted design

An element of `cflags`, `cxxflags` and `asmflags` has one meaning on every host
(SPEC-004 §8, `mcpp.manifest.flag_words`):

- the POSIX shell's word syntax without expansions: blanks separate words,
  `'...'` is literal, `"..."` honours `\"` and `\\`, quoted and unquoted pieces
  concatenate;
- outside quotes a backslash escapes only a blank, a quote or a backslash, so a
  Windows path keeps its backslashes and pkg-config's `\ ` is a space;
- `$` and the shell operators are ordinary characters;
- a `defines` entry is one value, spelled into the list with `flag_element` so
  that it reads back as exactly one word.

The ninja writer quotes each word for its host (`ninja_command_word`:
`shell_quote_arg`, then `$` doubled), and the databases list the words
(`package_flag_args`). Nothing in the unit path reads a command line back. The
host readers remain only for text the engine renders itself (the global flag
strings, the std module command), where `split_flags` undoes ninja's escapes and
then applies `host_command_words`.

### 1.3 Why this design and not the triage record's C1

C1 makes the databases faithful to a build whose meaning differs by host. It
fixes the reported symptom and keeps `-DV='sq'` meaning two macros. The adopted
design removes the host dependence at the manifest, so the same manifest builds
the same program on Linux, macOS and Windows, and the databases are equal to the
build by construction rather than by a second parser agreeing with the first.

The compatibility cost is bounded by measurement rather than by argument:

- every string literal of the published index that starts like a flag was
  read under the new syntax and under both hosts' previous readings
  (mcpplibs/mcpp-index `c176883`, 285 literals,
  `2026-09-17-655-index-readings.py`): before the `-D` exception (§5.2) 9
  differed, all pieces of the CMake command line in
  `compat.mysql-connector-cpp.lua`'s install hook rather than elements of a flag
  list; with the exception, 0 differ. libarchive's `\"` and compat.lua's packed
  `-include` read the same under all three (§4 K6);
- an element whose reading does change is named once, with both readings, on the
  first plan after the upgrade (§2.5).

## 2. Review from the angles the change touches

### 2.1 Architecture

One module owns the meaning (`modules/manifest/src/flag_words.cppm`), below the
build layer, so the manifest parser, the scanner, the build-program directives,
the planner, the ninja writer and both databases read the same function. The two
duplicate splitters that existed before (`compile_commands::split_flags` and
`build_database::split_command_words`) are reduced to one host reader in the same
module. The link path is deliberately not moved: its elements are rendered text,
not manifest elements (§5 F1).

### 2.2 Stability

A plain word (no blank, quote or backslash) is written byte-identically, so the
build.ninja of a project without such elements does not change beyond the
version-driven fingerprint. The round-trip properties are the invariants the
engine relies on, and each is a test: `flag_words(flag_element(w)) == {w}`
(2000 generated words), `split_flags(ninja_command_word(w)) == {w}` (1000 words,
on each CI host), and `/bin/sh` receiving `shell_quote_arg(w)` as `w`.

### 2.3 Simplicity

The syntax is the shell's, minus expansions, with one narrowing (backslash).
`join_flags` for compile lists is one line; the defines channel needs no tag,
only a spelling. The diagnostic's model of the previous reading is 25 lines and
disappears when the model is no longer needed.

### 2.4 Cross-platform consistency

The e2e criterion (§4 K2) asserts the same macro values and the same database
words on Linux, macOS and Windows, which the old build could not satisfy
(§1.1). The Windows quoting was corrected at the same time: `shell_quote_arg`
doubles the backslashes before a quote and before the closing quote, so a word
ending in `\` no longer escapes the quote that ends it.

### 2.5 Upgrade

The fingerprint names the mcpp version and every flag, so the first plan after
an upgrade writes a new output directory. The notes collected while manifests
load are released only when that directory has no `build.ninja` yet: once after
an upgrade, after an edit of a flag, and in a fresh checkout. A build that
repeats the plan says nothing, so a manifest already spelled for the new reading
is not warned about on every run (measured: three repeated builds, zero notes;
an edit of a define, one note).

### 2.6 Test coverage

| layer | test | what fails without the change |
|---|---|---|
| syntax | `modules/manifest/tests/test_flag_words.cpp` (6) | the module does not exist |
| edge == database | `CompileCommandsArgs.TheEdgeAndTheDatabaseListTheSameWords` | words listed as text |
| host quoting | `CompileCommandsArgs.AWordOnAnEdgeReadsBackAsTheWord` | the empty word vanished; a trailing `\` on Windows |
| host itself | `CompileCommandsArgs.ShellReceivesTheQuotedWordsAsWritten` | model and `/bin/sh` disagree |
| unit record | `CompileCommandsEmit.AUnitListsTheWordsOfItsFlagList` | text listed |
| end to end | e2e 736 | fails on 2026.9.16.2: `V_DOL=[a]`, `V_A=[1 -DV_B=2]` |

## 3. The ledger

Status is `todo`, `branch` (implemented on the pull request's branch, with its
reading), `done` (merged or published, with its reading) or `dropped` (with the
reason).

### 3.1 Engine: mcpp-community/mcpp, one pull request (`fix/655-flag-words`)

| id | task | depends on | status |
|---|---|---|---|
| E1 | `mcpp.manifest.flag_words`: `flag_words`, `flag_element`, `host_command_words` | - | branch: `FlagWords.*` 6/6 |
| E2 | every define push site spells one word (`[build]`, globs, targets, features, `mcpp:cfg=`) | E1 | branch: e2e 736 `V_DEF=["def"]` |
| E3 | compile edges write words quoted for the host; link edges keep the rendered text | E1 | branch: e2e 307, 615, 736 |
| E4 | databases list words; `split_flags` = ninja unescape + host reader; GAS units list their edge's list | E1 | branch: `CompileCommandsArgs.*`, `BuildDatabase.*` |
| E5 | include normalisation, the `-std` guard, dialect promotion and the std module's target-side flags read words | E1 | branch: build and e2e subset |
| E6 | Windows `shell_quote_arg` follows the MSVCRT backslash rule | - | branch: round trip on the Windows runner |
| E7 | `build/flag-words` note on the first plan | E1 | branch: e2e 736 D and E |
| E8 | SPEC-004 §8, SPEC-005 R3.7, docs/04 and docs/30 with their Chinese mirrors, CHANGELOG | E1-E7 | branch |
| E9 | version 2026.9.17.1 | - | branch |
| E10 | CI green on every workflow of the pull request | E1-E9, E11 | branch: `e90674fb` 12/12 runs green (macOS 27 unit 121/121, macOS 27 e2e 182 passed 0 failed, Windows unit and e2e) |
| E11 | macOS 27 legs and the macOS 27 std module fix (maintainer request, 2026-09-17; §5.1, §5.2): `ci-macos` and `ci-macos-e2e` run on `macos-15` and macOS 27; `ci-fresh-install` runs its xlings and Homebrew channels on `macos-14` and macOS 27; each macOS 27 leg asserts `sw_vers` major 27 | - | branch |

### 3.2 Release and ecosystem, in order

| id | task | depends on | status |
|---|---|---|---|
| R1 | merge the engine pull request; verify the `origin/main` run | E10 | todo |
| R2 | dispatch `release.yml`; upload each archive to GitCode with the local `gtc` as it appears | R1 | todo |
| R3 | merge the xim-pkgindex bump; read the index artifact, not git | R2 | todo |
| R4 | bootstrap pin 2026.9.17.1 (pull request) | R3 | todo |
| R5 | sandbox verification with CN mirrors (§6) | R3 | todo |
| R6 | reply on #655; close #656 | R5 | todo |

No change is needed in mcpplibs/mcpp-index (§4 K6), in mcpp-plugins (no flag
list) or in openxlings/xlings (its manifest pins an older engine and carries no
flag element; its build database is verified in R5).

## 4. Criteria

- **K1** Syntax table and round trip (`FlagWords.*`).
- **K2** e2e 736 green on Linux, macOS and Windows with the same assertions.
- **K3** The same e2e fails on 2026.9.16.2 (measured locally: `V_DOL=[a]`,
  `V_A=[1 -DV_B=2]`, `V_B=[V_B]`).
- **K4** The issue's probe reads identical build and database values for all
  five macros, and the database's arguments execute with status 0 (measured
  locally with the branch binary).
- **K5** The e2e subset that touches flags or databases (80 scripts) passes.
- **K6** No flag-list element of the published index reads differently (static
  reading above), and building libarchive and lua consumers in the sandbox
  produces no `build/flag-words` note (§6).
- **K7** In the sandbox, `openxlings/xlings`'s build database lists
  `-DPLATFORM_CONFIG_H="mcpp_libarchive_config.h"` and one libarchive unit's
  arguments execute with `-fsyntax-only`.

## 5. Findings while implementing

- **F1 The link path carries rendered text.** The first implementation routed
  every `join_flags` caller through the word reading. Two callers pass link
  units' flags, which the engine writes already quoted and escaped
  (`-Wl,-rpath,'$$ORIGIN'`); the edge became `'-Wl,-rpath,$$$$ORIGIN'` and e2e
  307 and 615 failed with `libwrap.so not found` at the closure check, while
  2026.9.16.2 passed both. The compile and link joins are now two functions.
- **F2 The database listed the wrong list for a GAS unit.** Its edge carries the
  `-D/-U/-I` subset and asmflags, the database listed all C flags. Both now read
  `unit_asm_flags`.
- **F3 An empty word vanished on the edge.** `shell_quote_arg("")` is empty by
  design for its other callers; `ninja_command_word` writes `''` or `""`.
- **F4 An unquoted `$ ` splits.** A unit test asserted that `-Idir$ with$ space`
  is one argument. ninja turns `$ ` into a space before the host reads the line,
  so it is three; the engine quotes every word it writes, and the test now
  states the host's reading.
- **F5 A note that repeats on every build is noise.** The first version warned on
  each plan; §2.5 is the rule adopted.

## 6. Sandbox verification

Recorded after the release in §7.

## 7. Readings and closure

Recorded as each row closes.

## 5.1 macOS 27 on GitHub-hosted runners

GitHub publishes no `macos-27` label. The runner-images README (read on
2026-09-17) lists macOS 26 as the newest GA image (`macos-26`, `macos-latest`)
and macOS 14 as deprecated (unsupported from 2026-11-02). macOS 27 is served as
the preview label `xcode-27`, whose base OS moved from macOS 26 to macOS 27.0 on
2026-09-16 (actions/runner-images#14404), with a warning that capacity may
queue. A temporary pull request (#658) measured which labels are served; its
first run, naming `macos-27`, `macos-27-arm64`, `macos-26` and `macos-latest`,
was cancelled while every job was still queued behind this repository's runner
limit, and the second names `xcode-27` and reads
`label=xcode-27 27.0 build=26A5406e arch=arm64 xcode=Xcode 27.0 clang=Apple clang
version 21.0.0` with `ImageOS=macos27` (run on 2026-09-16T19:16Z). Because the label names an Xcode and its
base OS has changed once, every macOS 27 leg asserts that `sw_vers` reports 27.

## 5.2 The first macOS 27 run: the std module does not build

Both macOS 27 legs failed on their first run (runs 35139712291 and
35139712295): the std module precompile with llvm 22.1.8 stops at
`<complex>:1012: use of undeclared identifier 'INFINITY'` (14 errors), in the
raw clang step and in mcpp's own std module build. The maintainer asked for the
fix in the same pull request, with the host kept to the minimum. Three probe
rounds on the `xcode-27` image (#659) read:

- The 27.0 SDK's `<math.h>` defines `INFINITY` and `NAN` itself only when
  `__has_feature(modules)` is false; with modules on it includes `<float.h>`
  with `__need_infinity_nan` and expects the compiler's header to supply them.
- A module interface unit has `__has_feature(modules)` true; a plain unit does
  not.
- `-std=c++23` and `-std=c++26` fail; `-std=gnu++23`, `-U__STRICT_ANSI__`, the
  26.5 SDK, `-D__need_infinity_nan`, and `-DINFINITY=HUGE_VALF
  -DNAN=__builtin_nanf("0x7fc00000")` build. `-fbuiltin-headers-in-system-modules`
  and `-fno-implicit-module-maps` do not.
- The CLT SDK and the Xcode SDK on the image are the same 27.0 SDK, so the
  sysroot choice is not the cause.

Adopted: `apple_float_macro_words` (hostflags.cppm) states the two macros with
the SDK's own GNU-mode spellings for clang on an Apple target. The alternatives
were rejected on their effect beyond the defect: `gnu++23` changes the language
dialect of every unit, `-U__STRICT_ANSI__` exposes non-standard declarations of
the C library, and `-D__need_infinity_nan` makes the first inclusion of
`<float.h>` supply only the two macros, so a unit that includes it once loses
`FLT_MAX` (clang's `float.h` lines 15-46). The identical spellings make the
redefinition in the SDK's non-module path silent; plain C and C++ units
including `<math.h>`, `<cmath>`, `<float.h>` and `<cfloat>` build with
`-Wall -Werror` on the 27.0 SDK. The value holds quotes and parentheses, so the
decision is a list of words and each reader quotes it: the ninja compile flags
(`ninja_command_word`), the std module command (single quotes), the
graph-supplied std module flags (`shq`) and a build program's argv (none). The
decision reads the target triple only; it reads nothing from the host.

The same round found two host-shaped test defects on the Windows runner of the
first push, both in this pull request's own code: `normalize_include_flags`
wrote every word back through `flag_element`, which quoted any backslash, so
Windows paths came back single-quoted; and two `split_flags` tests assumed
POSIX quoting on every host. `flag_element` now quotes a backslash only where
the syntax would read it as an escape, an element whose words did not change
keeps its spelling, and the POSIX-quoting tests are POSIX-only with a Windows
counterpart.

It also found a compatibility defect of the syntax itself: `NinjaBackend.
QuotesFlagValueWithSpace` (mcpp#234) failed, because `-DT=long long` read as
two words. Every release since #234 passed such an element as one argument, so
the syntax gained one exception (SPEC-004 §8 rule 8): an element that begins
with `-D` or `/D` and contains a space is one word, verbatim.

The second macOS 27 run (`10224fb8`) confirmed the fix at the compiler: the raw
clang steps print `C++23 import std works on macOS via xlings LLVM!`. It then
failed where the job builds mcpp with the bootstrap binary, 2026.9.16.1, which
predates the fix and so cannot build its std module on macOS 27. The
`setup-macos-llvm` action wraps a bootstrap older than 2026.9.17.1 on macOS 27
or later: the wrapper passes the same two words to clang through
`CCC_OVERRIDE_OPTIONS` (quiet form, measured on clang 22.1.8: both definitions
arrive exactly and nothing is written to stderr). The binary the job builds
runs unwrapped, so the unit tests, the second self-host build and the e2e suite
measure the engine's own fix. The wrapper retires when the bootstrap pin
reaches 2026.9.17.1. (Corrected in review: an earlier sentence here said e2e 252
keeps its real old-client leg through the wrapper. It does not, and did not
before this pull request on any macOS leg: `MCPP_BOOT` is the xlings shim,
which answers no `--version` inside the test's temporary directory, so the test
prints its NOTE and runs only its static half. Recorded in §8.)

## 8. Residuals

- e2e 252's real old-client leg does not run on the macOS legs (main included):
  `MCPP_BOOT` is the xlings shim, not a store binary. Pointing it at
  `~/.xlings/data/xpkgs/xim-x-mcpp/<version>/bin/mcpp` would make it run.
- Dialect promotion now reads words, so a packed element such as
  `"-fno-exceptions -fno-rtti"` is promoted into the std module's dialect set;
  the compiler already received both flags, so the std BMI now matches its
  importers where it did not.
- `ldflags`, `dialect_cxxflags` and `std-module-flags` keep their current
  meaning (documented in docs/04). Moving `ldflags` needs the link path's
  rendered text separated from manifest elements first (F1).
- `-B<binutils>` in the global compile flags is ninja-escaped but not quoted, so
  a registry path with a space would split; the databases now report that split
  faithfully.
- The detach-codegen edge on Windows runs `cmd.exe /c`, which interprets
  `<>|&^` outside its own quote counting; a word containing them is not covered
  by the round-trip criterion there.
