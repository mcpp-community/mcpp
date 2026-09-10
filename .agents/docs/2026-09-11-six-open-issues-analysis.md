---
subject: triage
status: active
---

# Six open issues: what each one actually is, and what would answer it

Issues #564, #597, #599, #603, #604 and #606, read against the code and, where
a claim was checkable, measured rather than accepted. Every report is accurate
about its symptom; three of them are wrong about the cause, and two of those
three describe a defect that is smaller than the one that is present.

Line numbers are against `fix/staging-is-a-service-for-a-dispatched-format`
(`c16a5128`), which is #607 on top of #605.

## 0. What the six have in common

Three of them -- #606, #604, #599 -- are one class of defect: **a rule applied
to an object it was not written for.**

    #606   a keyword matcher written for source lines, applied to a comment line
    #604   a de-duplicator written for a one-token marker, applied to a two-token pair
    #599   a hub path written for the current tree, applied to a historical one

None of the three is a mistake in the rule. Each rule is correct about the
object it was written for, and each acquired a second object without acquiring
a second reading. That is why none of them was found by a test: a test written
alongside the rule tests the rule against the object its author had in mind.

Two of the three -- #606 and #599 -- have a second property in common, which
is why they lasted: **the wrong answer is not loud.** #606's reported form is
an error, but its unreported form corrupts the module graph in silence; #599's
check prints a note, and a note never turns a job red.

## 1. #606 -- the scanner reads inside block comments

### The report is right about the symptom and wrong about the cause

Reproduced verbatim at 2026.9.10.2:

    /*
      module (exe)
    */
    int main() { return 0; }

    error: scanner errors:
      src/main.cpp:2: '(exe)' is not a module name. ...

The report bisects this to "lands after 2026.9.7.1" and calls it a regression.
Measured: `git log -S` over `src/modgraph/scanner.cppm` returns **no commit at
all** that ever added block-comment state. The line loop
(`src/modgraph/scanner.cppm:739-751`) tracks exactly two things -- preprocessor
depth and multi-line raw strings -- and its comment filter is

    std::string_view strip_line_comment(std::string_view s) {
        auto p = s.find("//");
        ...
    }

which handles `//` and nothing else. What landed at 2026.9.9.1 (`7a4b8391`,
#594) is the `is_well_formed_module_name` refusal, and that refusal is
**correct**: a malformed name recorded into the graph becomes a BMI path that
nothing reports. The bisect therefore dates the moment the defect became
audible, not the moment it was introduced.

### The root cause is an argument written down as settled

The reasoning that admits the defect is in the source, stated as a completed
argument (`scanner.cppm:702-707`):

> Ordinary `"..."` strings are intentionally left as-is: the import/module
> matcher only fires on lines whose trimmed text *starts with* the keyword,
> which a string body can only do when it spans lines (i.e. a raw string).

The premise is sound and the enumeration is short by one. Two constructs can
put a keyword at the start of a line without it being code: a raw string, and a
**block comment**. The code handles the one the comment names.

This is also why the report's table looks arbitrary. `/* module (exe) */` on
one line is fine because the trimmed line starts with `/*`; the same text with
the opener on its own line is not, because then the trimmed line *is*
`module (exe)`.

### The unreported form is worse, and was measured

    /*
      export module y;
    */
    int main() { return 0; }

builds successfully, and the graph it generates says:

    build obj/main.o | gcm.cache/y.gcm : cxx_object .../src/main.cpp | obj/main.cpp.ddi.dd
      bmi_out = gcm.cache/y.gcm

A plain `main.cpp` is recorded as the **producer of module `y`**, and the BMI
it promises is never written -- `gcm.cache/` is empty after a successful build.
Add a file that legitimately imports `y` and the diagnostic is:

    y: error: failed to read compiled module: No such file or directory
    y: note: compiled module file is 'gcm.cache/y.gcm'
    y: note: imports must be built before being imported

The import was satisfied *from a comment*, so the real provider is never
searched for, and the message names an ordering problem that does not exist.
The scanner's own comment two arms above predicts this shape exactly: "a
recorded non-name propagates into the build graph as a BMI path and is reported
by nothing". It is a well-formed name here, so the guard that catches the
report's case does not fire.

`import foo;` and `module x` inside a block comment are the same defect at
lower cost: a false edge and a false implementation-unit identity.

### What answers it

Block-comment state in the line loop, in the same shape as the raw-string
state that is already there: a `bool in_block` carried across iterations, with
the stripping done before `strip_line_comment` so that `/* */ import x;` still
scans and `// /*` does not open a block. Nesting is not a C++ construct and
must not be implemented; `/*` inside a string literal is the one remaining
corner, and the existing raw-string pass already blanks the case that can span
lines.

The comment quoted above must be corrected in the same change. Leaving it
would leave the argument that produced the defect standing next to the code
that fixes it.

Criterion, and it has to be the silent form, because the loud one is a
side effect of a guard that could legitimately be relaxed:

* a `.cpp` whose only `export module y;` is inside a block comment generates a
  graph with **no** `gcm.cache/y.gcm` output and no `bmi_out`, and
* the four-line file from the report builds, and
* `/* */ import x;` on one line still records the import -- the strip must
  remove comment *text*, not the whole line.

The second and third are the pair that distinguishes a fix from a mute.

## 2. #604 -- a two-token switch loses its switch

### Cause, read rather than guessed

Host-module use flags are collected in `src/build/build_program.cppm:1259-1266`:

    for (auto& f : hm->useFlags) {
        // GCC's marker is just `-fmodules`, already present when the
        // bundled module was built; repeating it is harmless but noisy.
        if (std::find(moduleFlags.begin(), moduleFlags.end(), f)
            == moduleFlags.end())
            moduleFlags.push_back(f);
    }

The de-duplication is **per token**. What the three families put in `useFlags`
is not the same shape:

| family | `useFlags` | tokens |
|---|---|---|
| GCC | `-fmodules` | 1, idempotent |
| Clang | `-fmodule-file=<name>=<path>` | 1, unique per module |
| MSVC | `/reference`, `<name>=<path>` | **2, first one repeats** |

`bmi_reference_tokens` (`src/toolchain/hostflags.cppm:296`) splits at the
prefix's last space, so `" /reference huxerui.rules.sources="` becomes exactly
`{"/reference", "huxerui.rules.sources=<path>.ifc"}`. By the time an inner host
module is appended, `moduleFlags` already contains `/reference` -- put there by
the bundled `mcpp` module at `hostprogram.cppm:690`. The first token is found,
**skipped**, and only the pair's second half is appended:

    ... /reference mcpp=<mcpp.ifc> huxerui.rules.sources=<rules.sources.ifc> ...

`cl.exe` reads the orphan as a source file name, which is C1083 verbatim. Clang
is immune by construction: its form is one word and never equals an existing
element. The report's earlier LNK1104 at 2026.9.8.1 is the same orphan reaching
the link line instead.

### What answers it

Append verbatim. The comment states the whole reason the filter exists --
"repeating it is harmless but noisy" -- so it buys argv tidiness and pays with
a broken command line. If the noise is worth removing, de-duplicate the flag
list **as a sequence** (skip only when `hm->useFlags` already appears in order),
which is correct for all three shapes because it never splits a pair.

Criterion: a package with a host module that itself declares a host module,
built under `windows = "msvc@system"`, produces an argv in which **every**
`<name>=<path>` token is immediately preceded by `/reference`. Stated as a
count rather than a search, because the defect is a missing occurrence and a
grep for `/reference` finds the one that is there.

This is a unit-testable statement about flag assembly and should be one: the
end-to-end path needs a Windows runner with MSVC, and the property does not.

## 3. #603 -- the probe cannot be called as it stands

The report's diagnosis is right: `src/toolchain/clang.cppm:153-166` hardcodes
`importStdMinLevel = 23` for the MSVC-STL fallback while `msvc.cppm:1040-1042`
probes. Its suggested fix needs one correction.

`std_module_min_level(const Toolchain& tc)` (`msvc.cppm:934`) reads
**`tc.version`** and compares it against 19.38 -- the cl banner threshold for
microsoft/STL#3977. On the clang path `tc.version` is clang's version, so
calling the existing function there compares a clang version number against an
MSVC threshold: clang 20.x passes it by accident, clang 19.x fails it wrongly.
Both answers would be produced by asking the wrong object, which is the same
error the current hardcode makes, one step less visibly.

The version that is actually binding is discoverable, and from the file that
was already selected. `find_std_module_source()` (`msvc.cppm:427`) returns

    <VC>/Tools/MSVC/14.44.35207/modules/std.ixx

so the toolset version -- which is the STL's version -- is the parent's parent's
filename. Toolset `14.<N>` and cl banner `19.<N>` share `N` by MSVC convention,
so the existing `>= 38` predicate transfers unchanged.

### What answers it

A function that takes the std module source path and returns the level:

    int std_module_min_level_for_stl(const std::filesystem::path& stdIxx);

with 23 when the path yields no parseable `14.<minor>`. Both paths call it, and
the clang path stops being a special case. Preferring the *selected* `std.ixx`
over a fresh `find_msvc_tools_dir()` matters on a machine with two
installations: the answer must describe the STL that will be compiled, not
whichever one the search finds first.

Criterion: two unit tests over the path shape -- `14.44.35207` answers 20,
`14.37.x` answers 23 -- plus one that a path with no toolset component answers
23. The existing `msvc.cppm` behaviour is unchanged for a real cl, which is
worth asserting too, since that path currently passes for a reason
(`tc.version` is genuinely cl's there) that this change must not disturb.

## 4. #599 -- a check that has never run in CI

The report finds two defects. There are three, and the third subsumes the
concern the report raises about the second.

**(a) The hub path is stale, and the report's replacement is also wrong.**
`bench/matrix.json:136` names `modules/platform/src/platform.cppm` for the
pinned `mcpp-2026.8.11.3`. The report says the file "lives at
`src/platform.cppm` there". Measured: it is at
`bench/projects/mcpp/mcpp-2026.8.11.3/src/platform/platform.cppm`. Worth stating
because it is the same error one layer up -- a path written from memory of a
tree rather than read from it.

**(b) The `uninit` branch prints a note.** `tests/e2e/233_bench_matrix.sh:340-346`
prints `NOTE: hub/body existence NOT checked for ...` and does not fail. A note
is invisible in a green job.

**(c) There is no bench workflow.** `.github/workflows/` contains sixteen
files and no `bench.yml`; `grep -rn submodules .github/workflows/` matches only
three `--recurse-submodules` clones of *other* repositories. So the test's own
justification --

> The bench workflow checks submodules out and runs this test, so the
> assertion does execute on every change to the suite.

-- is false. The hub/body existence check has run in **zero** CI jobs since it
was written. It fires only on a developer machine that has run
`git submodule update --init`, which is where the report found it.

### What answers it

Three parts, and (c) first, because fixing (a) without it fixes one string and
leaves the mechanism that let it rot.

1. Make the check run where the submodules are. Either restore a bench
   workflow that checks them out, or -- cheaper and enough for this
   assertion -- add `submodules: true` to the checkout of whichever e2e shard
   runs `233`, since the check needs the trees and not the toolchains. The
   trees are pins; the cost is a shallow fetch.
2. Make the `uninit` branch fail when it is reached in CI and note when it is
   reached locally. The distinction is `CI=true`, which every runner sets. A
   developer without submodules must not be blocked; a runner without them is
   a mis-configured job, and that is the thing to report.
3. Resolve the hub against the tree rather than against one string. The pinned
   tree is historical by construction, so a single path is wrong for it the
   moment the layout moves -- which is what happened. A per-project hub keyed
   by pin is the minimal fix; resolving by basename within the tree is the one
   that survives the next move, at the cost of ambiguity when two files share a
   name. Recommend the per-project hub, and record in `matrix.json` that the
   path belongs to the pin and not to the repository.

Criterion: with the submodules absent, `233` fails when `CI=true` and passes
with a note otherwise; with them present, it passes -- and it fails if
`matrix.json`'s hub is edited to any path that tree does not contain. The last
clause is the one that says the check measures something.

## 5. #564 -- two dead keys that want opposite answers

Both claims verified. `defaultJobs` and `defaultBackend` each have exactly two
mentions in the tree -- a declaration (`src/config.cppm:96-97`) and a parse
(`:517-518`) -- and no reader. The generated template plants both
(`src/config.cppm:356-358`).

The report offers one resolution for the pair ("wire it, or drop it"). They
deserve different ones.

**`default_jobs` should be wired.** It names a property of the machine, and no
other key can hold it. `MCPP_JOBS` must be repeated on every invocation.
`[build] jobs` is per-package, and `[workspace.build]` refuses it
(`modules/manifest/src/toml.cppm:2652-2657`) -- correctly, because a workspace
is not a machine -- so a seven-member workspace would carry the number seven
times and commit a machine fact to the repository. The arithmetic in
`policy.cppm:131-136` is the argument: at 0.5-1.0 GB per module compile, ninja's
own default of 10 on an 8-core, 15 GiB machine swaps.

Wire it as a **parameter** to `resolve_jobs`, between the manifest and the
backend default:

    MCPP_JOBS  >  [build] jobs  >  global default_jobs  >  0 (say nothing)

A parameter rather than an import, because `resolve_jobs` deliberately depends
on nothing but the manifest (`policy.cppm:138-139`).

The second caller is the part to decide rather than inherit.
`src/build/execute.cppm:2179` uses `resolve_jobs` for test-runner concurrency,
where 0 falls back to `hardware_concurrency()` rather than to a backend
default. Someone who sets a machine-wide number almost certainly means it there
too -- a test runner at 10 concurrent processes has the same memory shape as a
compile at 10 -- so it should apply, and be documented as applying. What must
not happen is for it to apply silently: this is a second behaviour under one
key, and `docs/03-configuration.md` has to say so.

**`default_backend` should be removed.** `BackendKind` has `Ninja` and `Native`
(`src/build/backend.cppm:10`) and `src/build/` contains one backend
implementation. The key promises a choice that does not exist, and its default
value `"ninja"` makes it read as implemented. Removing it from the template and
the parser is the honest state; when a second backend ships, the key comes back
with a reader.

Criterion: an e2e that writes `default_jobs = 3` into a scratch `MCPP_HOME`,
builds, and reads `-j3` off the ninja argv -- and the same fixture with
`MCPP_JOBS=2` set, asserting `-j2`, so the precedence is measured and not just
the plumbing. Four e2e fixtures and one CI action carry a copy of the generated
`config.toml` and each needs the `default_backend` line dropped; a grep for the
key must return zero outside the changelog.

## 6. #597 -- most of it landed, and the rest is smaller than the report thinks

The report's four items, against the current branch:

**(1) triple parsing -- done.** `wasm32-emscripten` is a row in `kKnownTargets`
(`modules/toolchain-model/src/triple.cppm:589`), `parse()` accepts the
`emscripten` OS segment (`:951`), and `llvm_triple()` emits
`wasm32-unknown-emscripten` (`:172`).

**(2) the object format -- done, and it is why the axis exists.**
`ObjectFormat::Wasm` (`:58`, `:192`) was added as a third value across the
engine precisely because a wasm target has no row in a two-valued
ELF/Mach-O/PE mapping. `family()` answers `unix` for it (`:253`), because
Emscripten supplies a POSIX emulation.

**(3) toolchain resolution -- not a new compiler family.** The report reads
`emcc`/`em++` as a fourth driver alongside `llvm`/`gcc`/`msvc`. It is not:
`em++` *is* clang, with `--target=wasm32-emscripten` and its own sysroot baked
in, which is the shape the engine already serves. `src/toolchain/hostflags.cppm`
names it: "Every hosted cross this build tool could do was served by a payload
whose driver had exactly one target -- `x86_64-w64-mingw32-g++` needs no
`--target` because it has no choice." `CompilerId` does not need a fourth
value; the payload does, and `xim:emsdk` is published (xim-pkgindex #805).
What remains is the resolver accepting that payload for this triple, which is
the `kKnownTargets` row plus a toolchain layer entry.

**(4) link semantics -- reuses an existing channel.** The output shape is the
report's one genuine design item and it is smaller than it looks. `em++ -o
app.js` writes `app.js` **and** `app.wasm`. mcpp's link edge already carries
implicit outputs for exactly this -- `ninja_backend.cppm:2162-2174` attaches
Windows import libraries and PDBs to the link edge with ` | ` -- so the `.wasm`
is a sibling on the same edge, not a second target. Execution is the existing
`runner` key (`modules/buildmcpp/src/directives.cppm:280`, generalised beyond
bare metal in #544): `runner = ["node"]`.

`--preload-file` is the only item with no existing mechanism, and it is not
needed for a first tier: a program that reads no data files at run time is the
common case, and `mcpp pack` is where a data-file policy belongs when it is
needed.

### What answers it

The row's tier. `planned` is the honest value today and `verified` is
reachable, because unlike the Android and iOS rows in the same batch, every part
of the loop is on a Linux runner: `xim:emsdk` supplies the driver, `node`
supplies execution, and the artifact can be run and its output compared. The
work is a toolchain layer entry, the implicit `.wasm` output on the link edge,
`runner = ["node"]` in the row's defaults, and a CI lane that builds and runs
a program for the target.

Recording here rather than deferring: this is the same batch as #605/#607 and
the row already exists, so #597 closes when the row graduates, not with a
separate design.

## 7. Order, and what depends on what

Nothing here shares code with anything else here, so the order is by cost of
leaving it in place:

1. **#606** -- live, deterministic, and downstream. `mcpp-index`'s
   `mysql-connector-cpp` fails on every shard that contains it, and the silent
   form corrupts module graphs without a diagnostic. It is also the smallest
   fix of the six.
2. **#604**, then **#603** -- `msvc@system` is the documented way around #603,
   and #604 blocks it, so the pair has an order even though the fixes are
   independent. Both are small and both are unit-testable without a Windows
   runner.
3. **#599** -- a check that has never run is a check that cannot report the
   next stale path. Fixing (c) is what makes (a) stay fixed.
4. **#564** -- a promise the generated file makes and the engine does not keep.
5. **#597** -- graduating a row that already exists, in the batch that added it.

The first four are one release. #597 belongs to the platform batch.
