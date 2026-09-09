---
subject: modules
status: active
---

# Two answers and two silences: the scanner's second grammar, and the manifest keys nothing reads

A consumer project pinned at mcpp 2026.8.17.1 reported five obstacles to
upgrading. This record judges each against two questions, and only those two:

1. **Does it violate a contract mcpp already holds?** A defect is a place where
   mcpp contradicts something it has already decided — in code, in a comment
   that states a rule, or in `docs/`. A gap that mcpp never promised to fill is
   a feature request, and belongs in a different record.
2. **Does the repair fit the semantics already in the tree?** A repair that
   introduces a second answer to a question mcpp already answers is not a
   repair, however green it measures.

Three reports survive both questions as defects. Two are refuted in part: one
described correct behaviour as a bug, and one asks mcpp to police a key it
deliberately does not interpret. The defects that survive are not five
independent bugs — they are **four mechanisms**, and two of them are instances
of failure modes this codebase has already named in its own comments.

## 1. What was measured

Host: Linux 6.8, x86_64. `gcc 16.1.0` and `clang 22.1.8`, both from the xlings
store. mcpp `2026.9.8.1` (released binary) and `2026.8.17.1` (the consumer's
pin). The working tree is `origin/main`, whose `MCPP_VERSION` is `2026.9.8.1` —
so the source read here and the binary measured here are the same version.

| # | Construct | 2026.8.17.1 | 2026.9.8.1 | Compiler alone |
|---|---|---|---|---|
| 1 | `module : private;`, clang | builds, links, runs (exit 0) | `error: scanner errors` (exit 2) | clang: compiles, emits `.pcm` and `.o` |
| 1b | `module : private;`, gcc | `error: module already declared` | same scanner error | gcc: `sorry, unimplemented: private module fragment` |
| 2 | UTF-8 BOM on a `.cppm` | — | `module 'hello' not found` at the *consumer* | clang: compiles the BOM'd file without complaint |
| 3 | implementation unit in a `.cppm` | — | **gcc: builds. clang: hard error.** | — |
| 4 | `[target.linux.runtime] libraries` alone | — | silently dropped | — |
| 4b | the same, plus one unrelated `defines` entry | — | applied (`-ldl` present) | — |
| 5 | unknown key in `[runtime]` | — | silent | — |

Rows 1 and 4b are the load-bearing ones. Row 1 is a clean A/B across the
regression boundary: same source, same clang, same machine, opposite outcomes.
Row 4b differs from row 4 in exactly one dimension that has nothing to do with
libraries, which is what makes it evidence about the gate rather than about the
predicate.

## 2. Mechanism A — a second grammar that disagrees with the normative one

`scan_file` (`src/modgraph/scanner.cppm:577`) is a hand-written parser for a
fragment of the C++ grammar. It exists because the compiler's own P1689 scan is
slower and, for clang, needs an external tool; `MCPP_SCANNER=p1689` selects the
normative path and is **opt-in**, so the hand-written scanner is what every
default build uses. Where the two disagree, the compiler is right by
construction. Two disagreements were measured.

### 2.1 The private module fragment (regression, `df7a443d`, #433)

`is_module_name_char` (`scanner.cppm:194`) admits `:` so that `M:part` scans as
one token. The `module` branch therefore reads `module : private;` as a
declaration whose "name" is `:`, and `scanner.cppm:732` tests
`name.find(':') != npos` to decide "this is an implementation partition":

```
module M;          implementation unit      → requires M
module M:part;     implementation partition → provides M:part
module : private;  private module fragment  → provides and requires nothing
```

The third production is not a name with a colon in it. The predicate "contains
a colon" collapses it into the second, A private module fragment may only appear in
a primary interface unit, so wherever that unit's `export module` line was seen
`u.provides` is set and the branch reports `file already provides module 'X';
cannot also provide ':'`. That sentence is false about the source it names.

Where the interface line was *not* seen — §2.2 gives one way that happens — the
same branch takes its other arm, raises nothing at all, and records the file as
providing a module literally named `:`. Measured on the compound input (a BOM'd
interface that also has a private fragment), the generated graph acquires an
edge for `pcm.cache/-.pcm`: a BMI for a module whose name is punctuation.
Neither arm of that branch is a name.

`git log -L` attributes the branch to `df7a443d` (#433), first released in
`v2026.8.18.1`. Before it, the `else` read `if (!u.provides)`, and since
`u.provides` was set, the line fell through and was ignored — correct behaviour
by accident. The commit fixed a real defect (implementation partitions recorded
as requiring themselves) and took this one with it.

**Verdict: defect.** `module : private;` is standard C++20, clang implements it,
and mcpp refuses a file the toolchain mcpp itself resolved would accept. On gcc
the code cannot build either way, but that is the compiler's sentence to pass,
and it passes it clearly.

**Repair.** Recognise the production before the partition test and record
nothing from it. Then, so the class is closed rather than the instance:
**never record a module identity that is not a well-formed module name.** `":"`
and `":private"` are not. Today any mis-parse of this line silently becomes a
module named after punctuation; with the check, the next one is a diagnostic.

mcpp must not additionally warn that gcc lacks the feature. The compiler says
so, and a second answer is what this record exists to avoid.

### 2.2 The UTF-8 BOM

`scan_file` opens an `ifstream` (`scanner.cppm:581`) and reads lines
(`:656`). Nothing consumes a byte-order mark, and `trim` uses `std::isspace`,
which is false for `0xEF 0xBB 0xBF`. The first line of a BOM'd file is therefore
`\xEF\xBB\xBFexport module hello;`, which matches neither `export` nor `module`,
so the declaration is not seen. Every normative parser skips the mark; clang
compiled the same file without comment.

MSVC writes UTF-8 with BOM by default, so this is ordinary input, not a corner.

**Verdict: defect**, and of a specific kind — mcpp's second parser diverging
from the parser whose answer is definitive.

**Repair.** Consume the mark once, where bytes become lines, not at call sites.
Refuse a UTF-16/32 mark by name rather than misparsing it, which is the
"no silent acceptance" rule this codebase applies elsewhere.

*Listed separately so it can be dropped without dropping the above:* a BOM on
`mcpp.toml` produces `1:1: error: expected key`. That is honest — it fails, and
it fails at the right place — but the message does not name the cause, and the
file is one MSVC users commonly author. This is diagnostic quality, not
correctness, and it carries its own criterion below precisely so that shipping
the scanner fix cannot quietly retire it.

## 3. Mechanism B — two answers to "is this a module interface"

This is the mechanism the other three symptoms share, and mcpp has already
written the rule down. `docs/04-mcpp-toml.md` §2.3:

> the scanner reads `export module` and gives the edge a BMI while the
> classifier says the file has no role, and what the author sees is
> `undefined reference` to a module-mangled symbol.

`scanner.cppm:616` enforces that rule in one direction: classifier says `Other`,
scanner found a module → refused, with a diagnostic that names the file, the
extension and the key to add. Its comment explains the choice: mcpp guessing
what an unknown extension "must have meant" would be *a second answer to the
same question — the very thing that produced this defect.*

The opposite direction is unguarded:

| | scanner: provides | scanner: provides nothing |
|---|---|---|
| **classifier: ModuleInterface** | normal | **unguarded** |
| **classifier: Other** | refused at `scanner.cppm:616` | normal |

The unguarded cell is reachable, and the two readers disagree by construction:

- `pick_rule` (`ninja_backend.cppm:1541`) switches on `cu.kind` — the
  **classifier's** answer, derived from the extension — and returns
  `cxx_module` for every `ModuleInterface`.
- `bmi_out` is bound only `if (cu.providesModule)` — the **scanner's** answer,
  derived from the content (`ninja_backend.cppm:1837`, `:1934`, `:2009`).

Three symptoms follow.

**Symptom 1 — an empty flag.** `ninja_backend.cppm:811` builds the flag
unconditionally for any toolchain with `needsExplicitModuleOutput` (clang and
MSVC; not gcc):

```
rule cxx_module
  command = if [ -n "$bmi_out" ] && [ -f "$bmi_out" ]; then cp -p ...; fi
            && $cxx ... -fmodule-output=$bmi_out -x c++-module -c $in -o $out
            && if [ -n "$bmi_out" ] && ...; fi
```

Both guards wrap the backup and the restore. The flag between them has none.
The comment three lines above the code says "If `$bmi_out` is empty (no module
provided), we just compile normally" — a protection that is not there.
Measured: an edge with no `bmi_out` binding, `clang++ -fmodule-output=` exits 0,
writes the object, and writes **no BMI**. Zero `.pcm` existed anywhere under
`target/` after the BOM build; the failure then surfaced at `main.cpp` as
`module 'hello' not found`.

**Symptom 2 — a project that builds on gcc and fails on clang.** A `.cppm`
holding an implementation unit (`module foo;`) is legal C++ and the scanner
parses it correctly — it records `requires foo` and provides nothing. The
classifier still says `ModuleInterface`, so the edge takes `cxx_module`, whose
clang spelling is `-x c++-module`. Clang is thereby told the file is an
interface and reports `missing 'export' specifier in module declaration while
building module interface`. The gcc rule spells the same edge `-x c++`, so gcc
reads the file's own declaration and the identical project builds. This is not
the BOM case; the input is well-formed, and the split is mcpp's.

**Symptom 3 — the BOM's downstream half**, which §2.2 already covers.

**Verdict: defect**, against a rule stated in mcpp's own documentation and
already enforced in the mirror direction.

**Repair, and the decision it requires.** What does a module-interface
extension declare? Two readings are available and mcpp has never chosen between
them in the backend:

- **A. The extension says how to *scan*; the content says what the file *is*.**
  Then the edge must be built from the scan result: a unit that provides a
  module gets `cxx_module` with a bound `bmi_out`; a unit that provides nothing
  does not. The empty flag becomes unreachable, and symptom 2 disappears because
  both compilers then receive the same instruction.

  **Routing to `cxx_object` is not sufficient, and this was measured.** Clang's
  driver infers `c++-module` from the `.cppm` extension on its own, and
  `cxx_object` carries no `-x` flag at all — so an implementation unit sent
  there fails with the identical `missing 'export' specifier` error. What the
  edge must carry is an **explicit `-x` spelling chosen from the scan result**:
  `-x c++-module` where the unit provides a module, `-x c++` where it does not.
  Measured: `clang++ -x c++ -c foo_impl.cppm` compiles and writes the object;
  the same command without `-x` does not. The gcc rule already spells `-x c++`
  unconditionally, which is the whole reason gcc was never affected — the
  portability split in symptom 2 is one flag wide.
- **B. The extension says the file *is* an interface.** Then a `.cppm` whose
  scan finds no module declaration is refused at scan time — the mirror of
  `scanner.cppm:616`.

**A is recommended**, because it is what `scanner.cppm:616`'s own reasoning
implies: an extension cannot know whether a given file is an interface or an
implementation unit, only the content can, and the scanner's answer is *read*
rather than guessed. B is not excluded by A and may be added as hardening, but
it is a separate decision with a separate criterion, because after A the only
input that reaches it is a genuinely malformed file.

**What the repair must not be.** Wrapping the flag in `[ -n "$bmi_out" ]` makes
the command line well-formed and leaves the graph wrong: the file is still
compiled as a module that provides nothing, and the consumer still fails at a
distance. It would also hide the next instance of the mismatch. The invariant
worth adding instead is the assertion that a `cxx_module` edge always binds
`bmi_out` — a branch that should not happen, given a line that prints.

## 4. Mechanism C — a declared key that reaches no decision

`[target.<pred>.runtime]` **is implemented**, contrary to the report: parsed at
`toml.cppm:2598` into `cc.libraries` / `cc.linkLibraryDirs`, applied at
`prepare.cppm:418`. It works — but only when something *else* under the same
predicate is also present.

`toml.cppm:2865` decides whether to record the `ConditionalConfig` at all:

```cpp
if (!cc.inputs.cflags.empty() || !cc.inputs.cxxflags.empty()
    || !cc.inputs.ldflags.empty() || !cc.inputs.sources.empty()
    ...
    || !cc.xlings.deps.empty() || !cc.xlings.featureDeps.empty())
    m.conditionalConfigs.push_back(std::move(cc));
```

Fourteen fields are named. `cc.libraries` and `cc.linkLibraryDirs` are not among
them. A predicate carrying only a `runtime` table therefore produces a config
that is parsed, populated, and discarded. Adding one unrelated `defines` entry
under the same predicate makes `-ldl` appear.

**This is the third instance of a failure mode the struct documents on itself.**
`ConditionalConfig` in `modules/manifest/src/types.cppm` carries two comments:

- #258 — "the conditional reader maintained its own subset of `[build]`'s keys
  and nobody noticed it had fallen behind." Repaired **structurally**, by
  carrying the whole `BuildInputs` type: "the set cannot drift."
- #359 — "the conditional channel carried three of the four dependency maps and
  silently lacked the fourth, which is the exact failure this struct's
  `BuildInputs` comment above describes for #258." Repaired **locally**, by
  adding the fourth.

The emptiness gate is a third hand-maintained list over the same struct, and
`xpkg.cppm:1530` is a fourth. Reading the xpkg loop, it fills nine `cc.inputs`
fields and its gate names eight: `privateIncludeDirs` is filled and unlisted, so
an xpkg conditional block carrying only private include directories is dropped
the same way. *(Found by reading; not measured end-to-end — it is stated here so
it is checked, not so it is believed.)*

**Why no test caught it.** The only e2e for the neutral link intent,
`tests/e2e/262_pack_consumed_by_native_cl.sh`, is `# requires: msvc` and does
not run on the Linux shards. It would not have caught it if it did: its fixture
is a *generated distribution package*, which by design carries both `ldflags`
and `libraries` — so `cc.inputs.ldflags` is non-empty, the gate passes, and the
bug is unreachable in the one shape the test builds.

**Verdict: defect.** Not a missing feature — a shipped feature that a fourth
reader discards.

**Repair.** Take #258's medicine rather than #359's: give `ConditionalConfig` an
`empty()` member defined **beside the fields**, and have both gates call it.
Adding a field then forces the question where the field is written, instead of
in two files that do not mention each other. Pushing unconditionally is simpler
and probably harmless, but it changes what `merge_conditional_config`'s two
disjoint passes iterate over, so it is the larger change and is not recommended
without its own measurement.

## 5. Mechanism D — a key that is never read

`toml.cppm:2542` states the contract:

> Unsupported scalar and array keys are REPORTED, not dropped. `[targets.<name>]`
> has done this since #249; this table did not, so a key that looks plausible —
> `cxx_runtime_tests` was the real one — was accepted in silence and had no
> effect (#418).

Measured coverage of that contract:

| Table | Unknown key |
|---|---|
| `[build]` | reported |
| `[target.<pred>]` | reported |
| `[target.<pred>.build]` | reported |
| `[runtime]` | **silent** |
| `[target.<pred>.runtime]` | **silent** |
| `[package]` | **silent** |

`[runtime]` reads ten keys individually (`toml.cppm:2001`–`2026`) with no sweep;
the `[target.<pred>]` sweep skips tables by design (`if (value.is_table())
continue;`), which is correct for the conditional channel but leaves the keys
*inside* `[target.<pred>.runtime]` swept by nobody.

**Verdict: defect** for the two runtime tables — a stated contract enforced on
three tables and not on two others that are read the same way. The key sets are
small and closed, so the existing sweep pattern applies directly.

`[package]` is listed but **not** bundled: whether package metadata stays open
for forward compatibility is a policy question, not an oversight, and deciding
it inside a scanner fix is how a requirement disappears.

## 6. What is not a defect

Two reports do not survive, and saying so is part of the plan.

**`[target.windows.runtime]` ignored on a Linux host is correct.** The predicate
is false; the table does not apply. The report read a correctly-evaluated
predicate as a silent drop. The genuine defect nearby is §4, which the report
reached by the wrong route — its conclusion "the neutral form cannot be written
per platform" is false, and the true statement is narrower and stranger: it can,
unless it is the only thing you write.

**mcpp has no detector for GNU-spelled `ldflags` reaching MSVC, and should not
grow one. DECIDED, and recorded here so the question is not reopened by the
next reader who meets an `LNK4044`.** `ldflags` is a documented raw escape
hatch; its contents are the author's spelling, passed through. `[runtime] libraries` is the neutral form
that exists precisely so the spelling need not be committed, and it renders
`user32.lib` or `-luser32` per dialect. A linter that inspected `ldflags` for
GNU syntax would be mcpp giving a second answer to a question the author already
answered — the exact move `scanner.cppm:616` refuses. The remaining work belongs to
`mcpp-index`, not here, and both halves of that sentence were checked before it
was left standing.

`mcpplibs/mcpp-index` `pkgs/c/compat.glfw.lua:125` does ship
`ldflags = { "-lgdi32" }` under its `windows` section, so the report is accurate.

**And the package can fix itself without any change to mcpp**, which is the half
worth recording because the opposite conclusion is easy to reach. An xpkg
descriptor has TWO conditional channels, and only one of them lacks the neutral
form. `target_cfg["cfg(...)"]` accepts `cflags`/`cxxflags`/`ldflags`/`sources`/
`defines`/`flags`/the include-dir keys and nothing else — an unknown sub-key
there is a hard error — so it cannot express a per-target `libraries`. But the
`mcpp.<platform>` sections can: they already parse a nested `runtime` table with
`libraries` and `link_library_dirs`, and mcpp splices only the matching
platform's body before parsing, on the TARGET axis rather than the host's
(`synthesize_from_xpkg_lua`, axis-typed by #254). So

```lua
windows = { runtime = { libraries = { "gdi32" } } }
```

is available today and renders as `gdi32.lib` for a native `cl.exe` consumer and
`-lgdi32` for a GNU one.

Adding `runtime` to `target_cfg` as well would therefore be a change with no
demonstrated need behind it, and it is deliberately NOT made here. The package
change is left for `mcpp-index` to make on its own schedule, because dropping
the `ldflags` line has a compatibility question attached — how old a client may
still read the descriptor — that belongs with the index and not with this
record.

**`[scan_overrides]` and `[xlings.workspace]` are capabilities, not defects.**
They are out of scope for this record.

## 7. Repair order and criteria

Ordering. §2.1 and §2.2 are independent of everything else and are what unblock
the consumer's upgrade. §3 is the structural repair and removes the mechanism
behind §2.2's downstream half; it should land after §2.2 so that the BOM case
is fixed at its cause rather than absorbed by the classifier change. §4 and §5
are manifest-side and independent of all of the above.

Criteria. Each must fail before the change and pass after; several past
regressions here passed because the assertion could not distinguish the two
worlds.

1. **Private module fragment.** A fixture with `module : private;` in a primary
   interface, built with clang, links and **runs**, asserting the program's exit
   status — not merely that the scanner is quiet. Both spellings (`module :
   private;` and `module :private;`). Denominator: the fixture must be listed by
   the e2e index, and the test must not carry a `# requires:` that the default
   shards do not satisfy — the lesson of `262`.
2. **Malformed identity.** A unit test asserting that the scanner records no
   module whose name is not a well-formed module name, driven by the token, so
   that it fails if the §2.1 fix is reverted while the fixture still passes.
3. **BOM.** A `.cppm` whose bytes begin `EF BB BF`, built and run. The mark is
   WRITTEN BY THE TEST rather than committed, which is the opposite of what this
   record first proposed: a committed fixture carrying a BOM is exactly the kind
   of file an editor, a linter or a checkout filter normalises without saying so,
   and the test would then pass while asserting nothing. Writing the bytes in the
   script cannot be disarmed that way — and the script still verifies that the
   three bytes are there before relying on them.
4. **BOM on `mcpp.toml`** — its own assertion on the message text, so it cannot
   be retired by 3 shipping.
5. **Classifier/scanner.** Two assertions, because one does not imply the other:
   (a) no `cxx_module` edge in any generated `build.ninja` lacks a `bmi_out`
   binding — asserted over the edges, with the count of edges examined printed,
   so an empty denominator is visible; (b) a project with an implementation unit
   in a `.cppm` builds and runs **on gcc and on clang**, since one compiler alone
   has no information about this defect. (b) must assert the program's exit
   status: a criterion that stops at "the compile succeeded" passes today on gcc
   and would have reported this defect as absent.
6. **Conditional runtime gate.** The minimal pair from row 4/4b: a predicate
   carrying *only* a `runtime` table applies, and the control differing by one
   unrelated field still applies. The negative leg is the load-bearing one.
7. **Unknown runtime keys.** Assert on `unsupported key` and the offending key
   name. Not on the substring `unknown` — `x86_64-unknown-linux-gnu` contains it,
   which cost one false reading while this record was being written.

## 8. What was implemented

Landed in mcpp 2026.9.9.1, one change per mechanism.

| Mechanism | Change | Site |
|---|---|---|
| A.1 private fragment | the production is recognised before the partition test | `scanner.cppm` |
| A.1 identity | an identity that is not a well-formed module name is refused | `scanner.cppm` |
| A.2 BOM | a UTF-8 mark is consumed where bytes become lines; UTF-16/32 refused by name | `scanner.cppm` |
| A.2 BOM | the same rule for every TOML document mcpp reads | `libs/toml.cppm` |
| B | `module_lang` and `module_output` become PER-EDGE, bound from `providesModule` | `ninja_backend.cppm` |
| B | `moduleImplLangFlag` — the spelling for a module-extension file that is not an interface | `toolchain-model/model.cppm` |
| C | `BuildInputs::empty()` and `ConditionalConfig::empty()`, replacing two hand-written gates | `manifest/types.cppm`, `toml.cppm`, `xpkg.cppm` |
| D | unknown-key sweeps for `[runtime]` and `[target.<pred>.runtime]` | `manifest/toml.cppm` |

Reading B's row against §3: the recommendation there was A, and the emitter now
implements it in the form the measurement forced — the rule states neither flag
and every module edge states its own. `pick_rule` is unchanged, and deliberately:
an extension can answer "which rule shape", because every module-extension file
needs that rule's depfile and BMI-preservation machinery. It cannot answer "is
this an interface". Splitting the question that way is what makes the empty-flag
state unreachable rather than merely unlikely.

`ConditionalConfig::empty()` composes rather than enumerating: it calls
`BuildInputs::empty()` and `XlingsConfig::empty()`, the latter of which already
existed. That is #258's medicine applied one level further out, and it closed a
fourth instance found while writing it — `xpkg.cppm`'s gate omitted
`privateIncludeDirs`, which its own loop fills.

### Measurements after

Each row was measured before the change and after, on the same machine, with
gcc 16.1.0 and clang 22.1.8.

| Construct | 2026.9.8.1 | 2026.9.9.1 |
|---|---|---|
| `module : private;`, clang | scanner error, exit 2 | builds, runs, exit 0 |
| `module :private;`, clang | scanner error, exit 2 | builds, runs, exit 0 |
| BOM on a `.cppm` | `module 'hello' not found` at the consumer | builds, runs, exit 0 |
| BOM + private fragment | graph gains `pcm.cache/-.pcm` | builds, runs; no such edge |
| implementation unit in a `.cppm`, clang | `missing 'export' specifier` | builds, runs, exit 0 |
| the same, gcc | builds | builds |
| `[target.linux.runtime]` alone | dropped | applied |
| unknown key in `[runtime]` | silent | reported |
| BOM on `mcpp.toml` | `1:1: expected key` | builds |

The criteria were then run against the implementation REVERTED, because a test
that passes in both worlds measures nothing. Ten of the twelve new unit tests
fail on the reverted tree and all four new e2e tests fail against the released
2026.9.8.1 binary. The two that stay green are the negative controls —
`WellFormedNamesSurviveTheIdentityGuard` and
`CorrectlySpelledRuntimeKeysAreSilent` — which exist to fail if the new refusals
are too broad, and are supposed to pass in both.

`ConditionalConfig::empty()` replaced a gate in the descriptor reader as well
as in the manifest reader, and a descriptor is read by every consumer of the
index rather than by one project. All 228 descriptors in `mcpplibs/mcpp-index`
were parsed with both binaries and compared: 228 identical, 0 differing, 0
errors on either side. That is the denominator the change is safe against, and
it is stated because "no published package changed behaviour" is otherwise a
claim with nothing behind it.

One denominator is worth recording because it is empty where a reader would
expect it not to be: mcpp's own build emits **zero** `cxx_module` edges, since
every module unit in it takes the two-phase split path (`cxx_precompile` /
`cxx_module_object`, both of which are reached only after `providesModule` has
been tested). Self-hosting therefore does not exercise mechanism B at all, and
the e2e asserts a minimum edge count for exactly that reason.

## 9. What would refute this

The §3 recommendation rests on the claim that an extension cannot decide what a
file is. If mcpp intends `module_extensions` to mean "these files are
interfaces, and an implementation unit among them is an authoring error", then
reading B is right, symptom 2 is user error, and the repair is a diagnostic
rather than a rule change. That decision is the maintainer's; the measurement
that forces it is row 3, where the same input builds on one compiler and not the
other. Whichever reading is chosen, mcpp must hold **one** of them — today it
holds neither, and which one a build gets is decided by the compiler.
