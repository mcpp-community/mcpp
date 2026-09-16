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
  `2026-09-17-655-index-readings.py`): 9 differ, and all 9 are pieces of the CMake command line in
  `compat.mysql-connector-cpp.lua`'s install hook, not elements of a flag list.
  libarchive's `\"` and compat.lua's packed `-include` read the same under all
  three (§4 K6);
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
| E10 | CI green on every workflow of the pull request | E1-E9 | todo |

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

## 8. Residuals

- `ldflags`, `dialect_cxxflags` and `std-module-flags` keep their current
  meaning (documented in docs/04). Moving `ldflags` needs the link path's
  rendered text separated from manifest elements first (F1).
- `-B<binutils>` in the global compile flags is ninja-escaped but not quoted, so
  a registry path with a space would split; the databases now report that split
  faithfully.
- The detach-codegen edge on Windows runs `cmd.exe /c`, which interprets
  `<>|&^` outside its own quote counting; a word containing them is not covered
  by the round-trip criterion there.
