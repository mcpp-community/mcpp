---
subject: triage
status: landed
---

# #655: the `arguments` array is not the command the build runs

**Status:** landed in mcpp 2026.9.17.1 (#657); the closure is `2026-09-17-655-implementation-plan.md` §7.

**Revision 2026-09-17 (before implementation).** §3 proposed that the
database adopt each host's own reading (C1) and that defines be tagged at
ingestion (C2). The maintainer asked for the option with the best cross-platform
compatibility, so the adopted design replaces C1: an element of `cflags`,
`cxxflags` and `asmflags` has one host-independent syntax, the build quotes each
word for its host, and the databases list the words without reading any command
line back. C2 is kept in a simpler form (a define enters the list already
spelled as one word). §3 below is the proposal as written; the adopted design,
its criteria and the findings made while implementing are
`2026-09-17-655-implementation-plan.md`.

**Issue:** mcpp-community/mcpp#655, "emit build-database: shell-escaped quotes
inside the `arguments` array break consumers".

Engine code was read at `51aeee0e` (origin/main, mcpp 2026.9.16.2). A statement
marked *measured* was run on Linux x86_64 with the released mcpp 2026.9.16.2 and
`gcc@16.1.0`; the probe is `2026-09-17-655-probe.sh`. A statement marked *read*
names a file and line and was not executed. Nothing here ran on macOS or Windows.

## 1. Verdict

The report is a real engine defect, and it is not a documentation question.
SPEC-005 R3.6 and R3.7 state that a unit's `arguments` is its compile command
and that it is the same record `compile_commands.json` lists; the JSON
Compilation Database and S1 both define `arguments` as an argument vector that
is executed without a shell. Consumers should not unescape defensively: after
the fix below, a defensive unescape would corrupt a define whose value really
contains a backslash.

The issue describes one case. The defect is wider. The probe compares, for five
macros, the value the compiler receives when ninja runs the build with the value
it receives when the database's `arguments` is executed directly (*measured*):

| Declared in `mcpp.toml` | Build (ninja, `/bin/sh -c`) | `arguments` executed | Agree |
|---|---|---|---|
| `cflags = ["-DESC=\\\"esc.h\\\""]` (the libarchive form) | `ESC "esc.h"` | `ESC \"esc.h\"` | no, #655 |
| `cflags = ["-DMID=\"mid\""]` | `MID mid` | `MID "mid"` | no |
| `cflags = ["-DSQ='sq'"]` | `SQ sq` | `SQ 'sq'` | no |
| `defines = ["DEF=\"def\""]` | `DEF def` | `DEF "def"` | no, and the build is the wrong one |
| `defines = ["SPACE=\"a b\""]` | `SPACE "a b"` | `SPACE "a` plus a stray argument `b"`; the compiler exits 1 | no |

Only the database's reading of `SPACE` stops the compiler. The other four
disagreements compile silently with a different macro value, which is the worse
form for a language server.

## 2. Cause

### 2.1 Two word splitters answer one question, and the per-unit path uses the wrong one

The ninja rule is `command = $cc $cflags $unit_cflags ...`
(`src/build/ninja_backend.cppm:1246`), and ninja runs it with `/bin/sh -c` on
POSIX hosts and with `CreateProcess` on Windows. Every flag string in the engine
is therefore text in the host's command-line syntax. The database has to turn
that text back into words by the same rules the host applies.

The engine contains two splitters for that purpose (*read*):

- `mcpp::build::split_flags` (`src/build/compile_commands.cppm:133`) is used for
  every translation unit in both databases. It undoes ninja's `$` escapes and
  removes a quote only when the quote opens a token. It does not implement
  backslash escapes, it treats a quote in the middle of a token as data, and it
  has one rule for both hosts. The unit test
  `CompileCommandsArgs.InnerQuotesAreNotStripped`
  (`tests/unit/test_compile_commands.cpp:307`) asserts the second of these
  choices, so the divergence in rows MID and SQ is pinned by a test rather than
  merely untested.
- `split_command_words` (`src/build/build_database.cppm:309`), added in #636 for
  the standard library units (R3.11), implements POSIX `sh` word splitting
  (backslash outside quotes, the four escapable characters inside double quotes,
  quotes anywhere in a word) and, separately, the MSVCRT rules that
  `CreateProcess` programs use.

The second splitter is the correct one, and it already exists. The per-unit path
was never moved onto it.

### 2.2 The database joins package flags differently from the build

`package_flag_args` (`src/build/compile_commands.cppm:211`) joins
`packageCflags`/`packageCxxflags` with bare spaces. The build joins the same
vector with `join_flags` (`src/build/ninja_backend.cppm:198`), which
shell-quotes a `-D` element that contains a space (mcpp#234). The database
therefore parses a string the build never wrote, which is why `SPACE` is cut in
two even though its quotes open the token.

### 2.3 The silent twin: `defines` values lose their quotes in the build

`join_flags` quotes a define only when it contains a space. The `defines` key is
documented as `name` or `name=value` (`docs/04-mcpp-toml.md:333`), that is a
value and not shell text, but `DEF="def"` reaches `sh` unquoted and the compiler
receives `-DDEF=def` (*measured*). This is a build defect that the database
defect was hiding: once the database agrees with the build, it will faithfully
report the wrong value.

The quoting cannot simply be widened to every `-D` element, because
`scanner.cppm:1209` pushes `defines` into the same vector as raw `cflags`, and a
raw `cflags` element such as libarchive's `-DPLATFORM_CONFIG_H=\"...\"` is shell
text that must not be quoted again. The origin of the element is lost at
ingestion.

## 3. Proposed changes

### C1. One splitter, one rendering (fixes #655 and rows MID, SQ, SPACE)

1. Move `split_command_words` out of `build_database.cppm` into
   `compile_commands.cppm` (or `flags.cppm`) as the single host-command-line
   splitter, and make `split_flags` "undo ninja `$` escapes, then
   `split_command_words(s, is_windows)`". The ninja unescape must come first, as
   it does today, because `escape_ninja_chars` runs before `shell_quote_arg`.
2. Make `package_flag_args` call `join_flags` (export it from `ninja_backend` or
   move it beside `shell_quote_arg` in `flags.cppm`), so the database parses the
   byte string the build writes.
3. Replace `InnerQuotesAreNotStripped` with a test stating the opposite: a
   mid-token quote is quoting, because `sh` treats it so.

No manifest or index change is needed. The libarchive recipe
(`mcpp-index/pkgs/c/compat.libarchive.lua:48`) is correct shell text for the
contract the build already implements.

### C2. `defines` are values, rendered by the engine (fixes row DEF)

Keep the origin of a define through ingestion: either a separate
`packageDefines` vector on the unit, or the element pushed already rendered with
`shell_quote_arg`. `join_flags` then quotes every define that contains any
character in `shell_quote_arg`'s trigger set, not only a space, and never
touches a raw `cflags` element.

This changes the build's output for a define whose value contains a quote,
backslash, `$` or similar. No package in `mcpp-community/mcpp-index` declares
such a `defines` entry (*read*, grep over `pkgs/`), so the change is visible
only to a project that relied on the value being shell text. It should be named
in the release notes.

### C3. State the flag contract in the documentation

`docs/04-mcpp-toml.md` (and its `zh` copy) should state that a `cflags`,
`cxxflags` or `ldflags` element is command-line text in the host's syntax and may
hold several words (`-include foo.h`), whereas a `defines` element is one value
that mcpp quotes. SPEC-005 R3.6 should add one sentence: `arguments` holds the
words the host passes to the compiler, after the host's quoting is removed.

A further change, holding flags as argument vectors from ingestion to the ninja
writer so that no string is ever parsed back, would remove the splitter
altogether. It is not proposed here: the index publishes flag elements that pack
several words, so it needs a manifest contract change and a migration, and C1
already makes the database agree with the build by construction.

## 4. Criteria

- **K1 (differential, the one that decides C1).** For a table of flag strings,
  including every row of §1, a path with a space, `$ORIGIN`, `-include x.h`, a
  backslash that precedes no quote, and an empty quoted argument, the words
  `split_flags` returns equal the argv that `/bin/sh -c 'printf "%s\0" <string>'`
  prints. On Windows the reference is a helper that prints its own `argv`.
  The oracle is the host, not an expectation written by hand.
- **K2 (end to end).** `2026-09-17-655-probe.sh` prints identical `READING
  build` and `READING database` lines for all five macros, and the database's
  exec returns 0.
- **K3 (row DEF).** `READING build: #define DEF "def"`. Before C2 it reads
  `DEF def`.
- **K4 (revert).** Revert C1 alone and K1 and K2 turn red; revert C2 alone and
  K3 turns red. A criterion that stays green without its change measures nothing.
- **K5 (consumer).** The reporter's case: `openxlings/xlings` with the released
  build produces `-DPLATFORM_CONFIG_H="mcpp_libarchive_config.h"` in both
  databases, and mcpp-language-server indexes libarchive without the
  `expected "FILENAME"` error.

## 5. Open points for review

1. Windows was not measured. `split_command_words(…, windows=true)` follows the
   MSVCRT rules; whether every Windows compile edge is a plain `CreateProcess`
   (no `cmd /c` wrapper, which would add cmd.exe's own parsing) should be read
   from `ninja_backend.cppm` before K1's Windows leg is written.
2. A raw `cflags` element with single quotes (`-DSQ='sq'`) already yields a
   different macro on Linux (`sq`) and on Windows (`'sq'`), because the flag is
   host command-line text. C1 makes the database report each host faithfully;
   it does not make the manifest portable. C3 documents that; whether to
   normalise raw flags to one syntax is a separate decision.
3. The reply to the reporter: `arguments` is exec-ready, the escaped form is an
   engine defect, and consumers should not unescape.
