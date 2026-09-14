---
subject: triage
status: active
---

# A framework's CMake parity list: the twenty-one items of #634, read against the code and routed to where each one belongs

**Status:** active, revision 3. Revision 3 is the implementation's: the plan
and ledger are `2026-09-14-634-implementation-plan.md`, whose §1 states the
refinements adopted before and while implementing; the sections below carry
them where a decision's wording changed (§5.2, §5.3, §5.6). Revision 1 classified the items; revision 2 held every
decision to six properties (simple, low in new surface, observable,
cross-platform, stable for existing manifests, consistent with the rules its
neighbours already follow) and measured every premise revision 1 had left
unmeasured. §1 states the review and §2 lists what it changed.

Engine code was read at `b8d96844` (mcpp 2026.9.14.1, the version the issue
measured), mcpp-plugins at `9301832` (0.9.3), xim-pkgindex at `19e264e6`,
xlings at `59068d6`, mcpp-index at `f68512d`, HuxerUI at `5356991` (the head of
Sunrisepeak/HuxerUI#7). A statement marked *measured* names where it was run:
*local* is a Linux x86_64 host with the released mcpp 2026.9.14.1,
`gcc@16.1.0`, `llvm@22.1.8`, `xim:android-ndk@30.0.16248370` and mcpp:plugins
0.9.3 (probes in `2026-09-14-634-probes.sh`); *CI* is the temporary pull
request mcpp-community/mcpp#635, which carries only a measurement workflow
(`.github/verify-634/` on that branch) and ran on `macos-15`, `ubuntu-24.04`
(fresh homes, and an API 34 x86_64 emulator) and `windows-2022` against the
released 2026.9.14.1 (runs 34806507856, 34808499807, 34809272832 and 34809856621). A statement marked *read* names a file
and line and was not executed.

§0 is the ledger. §1 is the review and §2 its changes. §3 lists where the
measured state differs from the issue. §4 states the routing rule and the
rules the decisions share. §5 to §7 take the items by home. §8 is what the
project can do today, §9 the defects found beyond the issue, §10 the order and
tasks, §11 every reading with its source, §12 the self-review.

## 0. The ledger

"Home" is where the change belongs under the routing rule of §4: **engine**,
**official plugin** (mcpp-plugins), **project** (HuxerUI or Lib-Live2D),
**payload** (xim-pkgindex), **xlings**, **index** (mcpp-index).

| # | real | home | decision (§) | evidence |
|---|---|---|---|---|
| A1 | defect | engine | a matching conditional dependency declaration replaces the unconditional one on its rows; a package states its form on a row as `[target.<sel>.targets.<n>] kind` (§5.1) | measured local: the conditional `linkage` is dropped on the host row; no manifest among 509 declares one key in both tables; an unconditional `kind = "shared"` already selects the shared form |
| A2 | defect | engine; nine manifests | a `path`/`git` dependency's identity is its manifest's; a differing key warns and adopts it; two identities over one source are refused before scanning (§5.2) | measured local |
| A3 | defect | engine; bundle placement in the plugin (B1) | the Android and Mach-O rows read their closure with the PE row's function; Android stages into `lib/`, Mach-O beside the program; `walked` means complete (§5.3) | measured local (Android), CI (Mach-O) |
| A4 | defect | engine | an ELF shared library's default SONAME is its file name (§5.4) | measured local; bionic's documented requirement |
| A5 | gap | engine (one key); suites as packages | `[test] discover` globs; a suite built from several sources is a workspace member, as every divergent program already is (§5.5) | read |
| A6 | defect, and a question the defect was hiding | engine | the static C++ runtime is located by asking the driver for the effective target, so test programs on the Android rows are self-contained; every runner receives the artifact's runtime files as `MCPP_RUNTIME_FILES`, which `adb-run` transfers (§5.6) | measured CI (emulator), local |
| A7 | resolved by the ecosystem's existing surface | engine (one accessor); one recipe | the SubOS pkg-config view is complete on a fresh home; the engine names it with `mcpp::pkg_config_libdir()` (§5.7) | measured CI (fresh home), local |
| A8 | no engine change now | project | library resources already reach the application; a graph API waits for a need transitive deploy cannot answer (§5.8) | measured local |
| A9 | gap | engine | the engine states the target's platform floor as a fact and dependencies use the existing `version-floor` requirement, which refuses (§5.9) | measured local |
| A10 | gap | engine | `run`, `test` and `pack` declare `--toolchain` (§5.10) | measured local |
| X | observability gap revision 1 missed | engine | the resolved dependency graph is recorded in `resolution.json` and printed by `mcpp why deps` (§5.11) | measured local |
| B1 | real; needs one engine defect fixed (§9 item 11) | official plugin | `dist-apple` places staged dylibs in the bundle's framework directory; the rpath arrives at link time through `mcpp::link_flag` (§6.1) | measured local (`link_flag` reaches the link), CI (run 2 found the link-time rpath anchored to the package directory, an engine defect, and an ad-hoc signed bundle verifying; run 4 measured a bundle whose program carries the literal rpath from its link: it verifies, runs, and fails without the framework) |
| B2 | feature | official plugin | a `dmg` format of `dist-apple` (§6.2) | measured CI (create, verify, mount with the `Applications` link, detach) |
| B3 | small engine gap, plugin, payload | engine; official plugin; payload | `--format <f>` runs through the named runner `<f>` when one exists (§6.3) | measured CI (`--format app --runner app` exits 7 with the program's output and arguments; without a runner the bundle directory is executed and refused, exit 126) |
| B4 | feature | official plugin; payload | a `setup` format of `dist-wix` (§6.4) | measured CI: the archives link on both MSVC-ABI toolchains and a program calling `DutilInitialize` runs, exit 0, on both |
| B5 | two plugin defects, then a feature | official plugin; payload | fix the second-pack loss and the multi-ABI read, report refusals through `mcpp::warning`, then `aab` (§6.5) | measured local |
| B6 | feature | `rules-metal`: official plugin; `tools-esbuild`: project | promote the Metal rule; keep esbuild in the project (§6.6) | read |
| C1 | feature | payload | `macapp-run` execs the bundle executable (§7.1) | measured CI (the main bundle and its resources resolve when `Contents/MacOS/<exe>` is executed directly) |
| C2 | real | payload; plugins CI | a bundle whose executable does not load UIKit is installed and its installed executable spawned, which returns the application's status and kept its output every time; a UIKit application keeps `simctl launch`, whose status simctl does not report, and the runner says so (§7.2) | measured CI |
| C3 | already answered | payload (close-out) | anchor `mbanative.dll`, verify installs, add the `.wixext`, name the ABI (§7.3) | measured CI, and the payload reader's hashes |
| C4 | real, and usage-solvable today | engine (small); payload CI | a fresh home's `config.toml` override already serves a branch checkout; the engine makes the same key reach an existing home (§7.4) | measured CI |
| D1 | ecosystem data | index | after HuxerUI's release (§7.5) | read |

## 1. The review

Each decision was held to the same six questions. The table records the
answers; §2 lists the decisions the review changed.

| decision | new surface | observable through | rows measured | what stays byte-identical | consistent with |
|---|---|---|---|---|---|
| A1 replacement | none | the graph record (X): the declaring table | host (local; the issue measured Android) | every manifest that does not declare one key twice (509 of 509 scanned) | every other conditional scalar: last matching section wins |
| A1 per-row kind | one conditional sub-table, one key | the graph record: form and reason | host (local) | packages without the sub-table | `kind = "shared"` as a package constraint (`linkage_form.cppm:232-233`) |
| A2 | one warning | the warning; the graph record: key and identity | host | manifests whose keys match | SPEC-001 §1.2 (identity is what the content declares); the short-name check already refuses |
| A3 | additive manifest lines | `closure =` and `needs` lines | Android (Linux host), macOS | the ELF host tree; PE | `pe_closure`; PE's layout beside the program |
| A4 | none | `readelf -d` | host, Android | consumers' `DT_NEEDED` | Mach-O's default install name |
| A5 | one key | `mcpp test --list` | host | the default glob's names | `[build] sources` globs; the target model's rule for divergent programs |
| A6 static runtime | none | test programs' `DT_NEEDED` | Android emulator | the LLVM payload's archives (same answer by query) | `clang.cppm`'s driver query |
| A7 | one accessor | the accessor's value | fresh home | no environment default | the `toolchain_*()` accessor family |
| A9 | three fact names | the refusal names the fact and the key | Android (local) | no manifest key | `version-floor`; `requires_abi` refuses |
| A10 | three option declarations | the `Resolved` line | host | the side channel | `build --toolchain` |
| X | one JSON section, one `why` topic | itself | host | existing `resolution.json` fields | `resolution.json`'s own `package_json` |
| B3 | one lookup rule | the `Running` line names the runner | macOS | plain `run`, `test` | named runners (docs/41) |

## 2. What revision 2 changed, and why

1. **A1: key-wise refinement became whole replacement.** Revision 1 merged the
   keys a conditional table writes and unioned `features`; that needed the
   parser to record which keys were written. A scan of 509 manifests (mcpp's
   examples and tests, the plugins, HuxerUI, Lib-Live2D, the mcpplibs
   checkouts) found none that declares one dependency in both tables, so the
   richer rule has no user, and replacement is the rule every conditional
   scalar already follows.
2. **A1: `requires_linkage` became a per-row `kind`.** Everywhere else a
   `requires_*` key refuses (`requires_abi`, `version-floor`); a
   `requires_linkage` that *selects* would give one prefix two meanings. The
   per-row form of the existing constraint needs no resolver change, since
   `kind = "shared"` already selects the shared form under a default request
   (measured, §5.1).
3. **A3: Mach-O stages beside the program instead of changing the link.**
   Revision 1 added `@loader_path/../lib` to every Mach-O link with a shared
   dependency, which changes command lines and fingerprints for every such
   macOS build. The consumer already carries `@loader_path`, so a dylib staged
   beside the program loads with no link change; that is PE's layout.
   Measured on `macos-15`: the staged program runs, and fails without the staged dylib (§5.3).
4. **A5: named test targets and labels became one glob key.** Exclusive
   sources per target contradict the target model, which states that objects
   are compiled once and linked into every target and that divergent programs
   are workspace members (`types.cppm:175-180`). A multi-source suite is
   therefore a package; the one missing piece is pointing discovery away from
   a `tests/` another build system owns.
5. **A6: a defect replaced the question.** On the emulator no test loaded:
   every test program needed `libc++_shared.so`, because the engine's static
   libc++ lookup does not find the NDK's archives and the contract degrades
   (measured, §5.6). The deployed-file question was then measured with the
   runtime linked statically.
6. **A7: "the view is incomplete" was this machine's history.** On a fresh
   home the view resolves `gtk4` completely; the one absent `.pc` belongs to a
   recipe with no pkg-config handling (measured, §5.7).
7. **A8: the graph file is not built now.** The observability half moved to X,
   where it serves every resolution change in this record.
8. **A9: `requires_platform` with raising became engine facts with refusal.**
   The floor is fixed in the compiler's `--target` at toolchain resolution
   (`prepare.cppm:3670-3673`), before the dependency worklist starts
   (`:6499`); raising it from the graph would re-resolve the toolchain. A
   refusal reuses `version-floor`, which knows no meanings, and leaves the
   application's floor the application's decision.
9. **B6: the optional Apple SDK accessor was dropped.** A rule can ask
   `xcrun --sdk <sdk> --show-sdk-path`; no engine surface is needed.
10. **C4: no xlings change.** A fresh home's override serves a branch checkout
    and survives an index update (measured, §7.4). The engine's gap is that the
    same key in an existing home is ignored without a word.
11. **X was added**, because `mcpp why deps` reads only `mcpp.lock`'s text and
    `resolution.json` holds no graph (measured, §5.11), so three of this
    record's decisions would otherwise be observable only through warnings.
12. **New defects were measured**: two in `dist-apk` (§6.5); in the engine, the
    Android static runtime (§5.6), Mach-O loader tokens anchored in rpaths
    (§9 item 11, found by B1's measurement), the link-form degradation's
    missing reason (§5.1), and the version-floor message's wording (§5.9).

## 3. Where the measured state differs from the issue

1. **A1 is not Android-specific**, and the issue's proposed requirement form
   is weaker than what the engine already does for `kind = "shared"`.
2. **A2 is silent even without a duplicate**, and mcpp's own curriculum
   carries the shape six times.
3. **A3 is worse than reported**: the engine's own `--format dir` on the
   Android row ships an archive without its closure and says `walked`.
4. **A4 is wider than reported**: the Android application object lacks
   `DT_SONAME` too; the Mach-O half is already done.
5. **A6 cannot run at all today on the emulator**, for a reason the issue did
   not reach: the C++ runtime, not the data files.
6. **A7's 37 pins are unnecessary on a fresh home**: the view already resolves
   the GTK closure.
7. **A8's resources already reach the application.**
8. **A9 has a working form today** through a dependency's build program.
9. **A10 works today** through `MCPP_TOOLCHAIN`.
10. **B3 works today** through a named runner, and HuxerUI's `open -W` default
    runner also wraps a plain `mcpp run` (measured: a plain `mcpp run` handed `-W <bin>/rapp` to `open` and exited 0 for a program that exits 7).
11. **B5's premise does not hold**: `dist-apk` 0.9.3 refuses the multi-ABI
    stage, and a second pack drops the dependency's library (measured).
12. **C3 is already answered**, and the archives link under both MSVC-ABI
    toolchains once the system import libraries are named.
13. **C4 needs no xlings change**: the override works on a fresh home.

## 4. The routing rule, and the rules the decisions share

**The routing rule (given by the maintainer for this triage).** mcpp is a
general build engine and takes general capabilities only; special
functionality is built on the plugin system, a plugin a second project would
use belongs to the official plugins, a project-specific one to the project.
Each item gets one home, tried in this order, and its section says why the
cheaper homes do not suffice: usage; project plugin; official plugin;
ecosystem data (payload, runner program, xlings, index); engine, stated as the
general capability rather than the product feature. The rule filters
features, not defects.

**Rule 1. A declaration never disappears.** It is applied or reported (A1, A2,
the runner tokens of §9, C4's existing home).

**Rule 2. A closure is read, not run, and its manifest says what happened**
(A3; `pe_closure` is the complete instance).

**Rule 3. The engine asks the toolchain instead of guessing its layout** (A6's
archives; `clang.cppm:130-150` already asks `--print-file-name` for the same
reason), and it does not rewrite what belongs to the loader (the rpath tokens
of §9 item 11).

**Rule 4. Name the existing mechanism before adding one** (A7's view, A8's
deploy, A9's floor, A10's variable, B3's named runners, C3's payloads, C4's
override).

**Rule 5. A change to resolution leaves a record a test can read** (X): a
criterion reads state, not a warning's wording.

## 5. Engine items

### 5.1 A1: one identity, a different link form per row

**What the code does.** `merge_conditional_config` folds matching
`[target.<selector>]` sections before resolution: build inputs are appended
after the base, so the conditional rule wins (`prepare.cppm:395-398`); tools
replace the base entry (`:266-292`); scalars follow the last matching section
(`:428-438`). Dependencies do the opposite, `m.dependencies.insert(...)`
keeping the unconditional entry (`:458-463`), a rule dating from Phase 1b
whose record gives no reason
(`2026-06-29-manifest-environment-and-platform-design.md:279-287`) and which
#359 moved into the funnel every package's manifest passes through
(`df4f75e2`; `prepare.cppm:7227-7230`). A modifier-only table is TOML's nested
table: `huxerui.huxerui = { linkage = "shared" }` names no source
(`toml.cppm:1347-1353`), reaches `load_selector_dep_table` as the selector
`huxerui.huxerui.linkage` (`:1599-1629`), and the conditional tables are loaded
under the section string `"dependencies"` (`:3015-3024`), which is why the
warning names `[dependencies]`.

**Measured (local, `x86_64-linux-gnu`).** The first spelling exits 0 with no
`libfw.so` and no diagnostic; the second exits 2 with `[dependencies]
demo.fw.linkage = 'shared' is not a version RANGE` and `tried: (demo.fw,
linkage)`; two complementary predicates produce `bin/libfw.so` and `NEEDED
libfw.so` (the control). A dependency whose own target is `kind = "shared"`,
consumed by a root that writes no `linkage`, is linked shared; a root that
writes `linkage = "static"` gets `warning: demo.fw@0.1.0 is linked as a
shared library: the requested form is not available here`, a sentence that
does not name the package's own statement. A scan of 509 manifests found none
declaring one dependency key in both an unconditional and a conditional table.

**Decision.**

1. *Replacement.* A matching conditional declaration of a key replaces the
   unconditional declaration of that key on the rows its predicate selects;
   several matching sections apply in manifest order, last wins. No warning:
   this is what every conditional scalar does. The declaring table is recorded
   in the graph record (§5.11). The same rule applies to `dev-dependencies`,
   `build-dependencies` and `feature-deps`.
2. *The right words for the modifier-only table.* Conditional dependency
   tables are labelled with their full section name, separately from the
   lookup path `is_namespace_table` builds (`toml.cppm:1541-1546`). When a
   selector's last segment is a dependency option (`linkage`, `features`,
   `default-features`, `visibility`, `tools`, `host-module`, `reexport`,
   `backend`) and its value is a string, the warning at that one site says
   that the table names no source and gives the restated form, instead of the
   version-range warning. The grammar does not change, so a package named like
   an option remains addressable.
3. *A package states its form on a row.*
   `[target.'cfg(env = "android")'.targets.huxerui] kind = "shared"` is the
   per-row form of `[targets.huxerui] kind = "shared"`: merged in the first
   pass, accepted only for a target the package declares unconditionally, only
   with the library forms `lib` and `shared`. Resolution does not change: on a
   matching row the package is `declaredShared` and the shared form is
   selected; an explicit `linkage = "static"` on the root's edge gets the
   existing degradation, whose sentence now names the package's statement and,
   for a row, its selector. An engine that predates this sub-table skips it
   without a word (the `[target.<sel>]` sweep exempts every table,
   `toml.cppm:2756`), as `requires_abi` records for itself; a package relying
   on it states its engine floor, and the sweep learns the sub-table names the
   parser reads, so the next new sub-table is reported by engines from this
   one on.

HuxerUI's applications then write one unconditional line and the framework
writes the Android form once.

**Criteria.** The first spelling on the host row gives `bin/libfw.so` and
`NEEDED libfw.so`, on a row the predicate does not select a static link, and
the graph record names the conditional table. The modifier-only table exits
non-zero naming the conditional table and the restated form. A package with
`[target.'cfg(os = "linux")'.targets.fw] kind = "shared"` consumed by a root
that writes no `linkage` gives `bin/libfw.so` on Linux and a static link on a
row the selector does not match; with `linkage = "static"` on the root's edge
the degradation names the selector and `--strict` exits non-zero. Negative
direction: e2e 86, 195 and 328 are unchanged, and the manifests of the scan
build with identical command lines.

### 5.2 A2: a dependency key that names another identity than its manifest

**What the code does.** The worklist dedups by the identity the consumer's key
normalises to (`prepare.cppm:6531-6534`). After a `path` or `git` manifest is
loaded, only the short name is compared (`:7242-7260`), so a namespace
mismatch passes and the package's own identity and the resolver's key
disagree from then on; every reader that builds the qualified name from the
manifest (`:11315-11317`, `:5620-5630`) sees the manifest's answer. SPEC-001
takes identity from what the content declares (§1.2, §3.4).

**Measured (local).** Two edges to one directory, keyed `huxdemo.fw` and `fw`,
fail in the scanner naming one file twice; one edge keyed `fw` builds with no
diagnostic. Nine `path` edges among 46 scanned write a key the target manifest
does not declare: six in mcpp's `examples/04`, `08` and `12`, three in
HuxerUI's `huxerui-build-rules`, `huxerui-tools` and `huxerui-tests`.

**Decision.** A `path` or `git` dependency's identity is the one its manifest
declares. A key that normalises to another identity adopts the declared one
and warns, naming the requester, the key, its normalisation and the
declaration. Implemented by recording the identity resolved from each
canonical source, so a second key over the same directory or commit takes it
without the manifest being loaded again (revision 3: a key-to-identity alias
would also have captured a `version` dependency written with the same key); an
identity already resolved from a different source follows the #630 decision
table (`docs/05-dependencies.md:126-133`). Two identities over one canonical
source (possible only for manifests without a namespace, which keep taking the
key's) are refused before scanning, naming both. The scanner names the two
packages when one module has two providers. The nine manifests are corrected
with the change (mcpp's six in the same PR). Adoption rather than refusal:
nine builds that work today would break for a defect whose resolution is
known, and SPEC-001's reason against candidate search (index state
retargeting a dependency) does not apply to a source the manifest line fixes.

**Criteria.** The two-edge fixture builds, compiles `fw.cppm` once and warns
once with a sentence naming `fw`, `mcpplibs.fw` and `huxdemo.fw` (the domain
string is not rendered and is not asserted); the graph record shows one
package `huxdemo.fw` with both keys. Two keys `a.fw` and `b.fw` over a manifest
without a namespace are refused before scanning. Negative direction: matching
keys produce no warning.

### 5.3 A3: the staged closure on the Android and Mach-O rows

**What the code does.** `pack::run` dispatches the Android shared-object row,
PE and wasm before the closure walk (`pack.cppm:1359-1377`). PE reads its
closure: a breadth-first walk over `needed_names`, resolved against
`searchDirs`, which start at the artifact's `bin/` (`:999-1053`, `:497`). The
Android row copies the application object and the deployed files and returns
`ClosureResult{}`, whose `walked` defaults to `true` (`:1281-1337`, `:247`).
Mach-O programs reach `finish_without_closure` (`:1423-1438`) although the
reader landed in #630. Consumers of a graph-built dylib link with
`-Wl,-rpath,@loader_path` (`plan.cppm:583`), and dylibs are named
`@rpath/<file>` (`ninja_backend.cppm:288`).

**Measured.** Local, `mcpp pack --target x86_64-linux-android --format dir`:
exit 0, `Packed`, a tree holding only `lib/libapp.so`, whose `DT_NEEDED` names
`libfw.so` and `libc++_shared.so`, and a manifest `closure = walked`. The NDK
sysroot separates the two kinds of name: `usr/lib/<triple>/<api>/` holds the
platform stubs, `usr/lib/<triple>/libc++_shared.so` ships with the
application. CI, `macos-15`: the program in the build tree runs (exit 7) with `LC_RPATH @loader_path`, and the dylib is named `@rpath/libfw.dylib`; copied with its dylib into a fresh `stage/bin/` and run after the build tree was moved away, it exits 7; with the staged dylib removed it stops with `dyld: Library not loaded: @rpath/libfw.dylib` (exit 134). Today `mcpp pack --format dir` refuses with `cannot package the Mach-O program 'app' yet`.

**Decision.**

1. `pe_closure` generalises to one reader-driven function for PE, the Android
   rows and Mach-O, with a per-format rule for platform names: PE as today;
   Android, a name present as a stub in the payload sysroot's API-level
   directory; Mach-O, `/usr/lib/` and `/System/Library/`, with `@rpath`,
   `@loader_path` and `@executable_path` resolved by `resolve_macho_names`.
2. The Android row stages resolved libraries into `lib/` (per ABI on the
   several-triple route), `libc++_shared.so` included.
3. Mach-O stages resolved dylibs beside the program in `bin/`, where the
   existing `@loader_path` finds them: no link change, no load-command edit,
   no re-signing. Measured on `macos-15` (§11, C5).
4. `walked` is written only when every needed name resolved to a staged file
   or a platform name; otherwise `not-walked` with the names, and `dir` and
   `tar` refuse.
5. The stage manifest gains one additive line per needed name,
   `needs<TAB><name><TAB><staged path>`, `needs<TAB><name><TAB>platform` or
   `needs<TAB><name><TAB>unresolved` (revision 3: TAB-separated, because an
   install name or a directory can contain a space, and a third value so a
   provider reads a failure as state).

The ELF host row keeps `ldd_parse`; moving it onto the reader is a separate
change with its own byte-identical criterion. `dist-apk` then reads `lib/`
from the stage and deletes its own walk, which removes the defect of §6.5 by
construction.

**Criteria.** Android: the `dir` tree holds `lib/libapp.so`, `lib/libfw.so`
and `lib/libc++_shared.so`, and the manifest says `walked` with `libc.so`,
`libm.so`, `libdl.so` as platform; a needed library deleted after the build
makes `dir` refuse naming it. macOS: the staged program runs after the build
tree is moved, and fails with "Library not loaded" when the staged dylib is
removed. Negative direction: the ELF host tree is byte-identical and its
manifest differs only by `needs` lines.

### 5.4 A4: the default SONAME of an ELF shared library

**What the code does.** `shared_soname_flag` names `@rpath/<file>` on Mach-O
always, nothing on PE, and a SONAME on ELF only when one is declared
(`ninja_backend.cppm:274-290`); the ELF default was never decided (`93398f40`).

**Measured (local).** No `DT_SONAME` on `libfw.so` built through `linkage =
"shared"` on the host and Android rows, nor on the Android application object
`libapp.so`. bionic's `android-changes-for-ndk-developers.md` lists "Missing
SONAME (Enforced for API level >= 23)": each ELF shared object must carry one,
and the file name is used when it is missing.

**Decision.** An ELF shared library without a declared `soname` is linked with
`-Wl,-soname,<output file name>`. Consumers link with `-l<name>`
(`plan.cppm:582-584`) and already record that name.

**Criteria.** `SONAME lib<dep>.so` on the host and Android rows, and on the
application object. Negative direction: a consumer's dynamic section is
byte-identical, and a declared `soname` keeps its value and its alias.

### 5.5 A5: tests from several sources, and tests that live elsewhere

**What the code does.** The test set is the literal pattern `tests/**/*.cpp`,
one program per file (`test_targets.cppm:38`). `[targets]` has no test kind
(`toml.cppm:1111-1124`). The target model compiles objects once, links them
into every target, and sends a program that must diverge to a workspace
member (`types.cppm:175-180`).

**Decision.** `[test] discover = ["tests/**/*.cpp"]` is the default, a list of
globs in the vocabulary `[build] sources` already uses, `!` exclusions
included; `[]` disables discovery; a test's name is its path relative to its
glob's fixed prefix, so the default yields today's names. A suite compiled
from several sources is a package, as any divergent program is: a support
library carrying Catch2 and the include directories, one package per suite
with its own `[build] sources` and one `tests/main.cpp`, selected with
`mcpp test -p <member>`. No `[[test]]` table, no per-target exclusive sources,
no labels.

**Criteria.** `[test] discover = ["checks/**/*.cpp"]` runs `checks/a.cpp` and
not a failing `tests/b.cpp`; `discover = []` runs none. Negative direction: a
package without the key lists the same names as today.

### 5.6 A6: test programs on the emulator and the simulator

**What the code does.** `mcpp test` resolves its runner as `mcpp run` does
(`execute.cppm:643-711`, `2423`); a runner receives the artifact path only
(`:1691`); `adb-run` pushes that one file and runs it from `/`
(`xim-pkgindex:pkgs/a/android-platform-tools.lua:596-623`). The static libc++
of the self-contained contract is looked for only under the LLVM root's `lib/`
(`flags.cppm:1064-1079`, `1182-1183`), and a miss degrades the contract to
toolchain-coupled (`distribution.cppm:718-726`).

**Measured.** CI, API 34 emulator, run 1: `adb devices` lists
`emulator-5554`; all three tests fail with `CANNOT LINK EXECUTABLE
"/data/local/tmp/runs": library "libc++_shared.so" not found`, after the
engine warned `cxx_runtime: test target: this toolchain ships no
libc++.a/libc++abi.a; using toolchain-coupled`. Local: the same tests need
`libc++_shared.so`; with `-static-libstdc++` they need only `libm.so`,
`libdl.so` and `libc.so`. The drivers answer where their archives are for the
effective target: the NDK names its API-level `libc++.a` (a linker script,
`INPUT(-lc++_static -lc++abi)`) and `libc++abi.a`, the LLVM payload names
`lib/x86_64-unknown-linux-gnu/libc++.a`. CI run 2, with the runtime static:
`runs` passes; `reads_relative` fails with `open failed: /data/local/tmp/data/data.txt`, because `adb-run` pushed the executable and not its deployed file; `reads_host_path` fails with `open failed: <the build machine's path>`. CI, iOS simulator: all three tests pass through `simctl-run`, `reads_relative` (the deployed file beside the build-tree program) and `reads_host_path` (the build machine's path) included: the simulator reads the host's filesystem, so the runtime-files contract below serves device-like runners such as `adb-run` and changes nothing on the simulator.

**Decision.**

1. *The static runtime is located by asking the driver.* `find_archive` asks
   the compiler `--print-file-name=<archive>` with the effective target flag,
   the query `clang.cppm:130-150` already runs to find the std module, and
   falls back to today's directory search. The self-contained contract then
   holds on the Android rows for tests and programs; the shared-object role
   keeps `libc++_shared.so`, which `dist-apk` packages.
2. *Runners learn the artifact's runtime files.* Every runner, for `mcpp run` and `mcpp test`, receives `MCPP_RUNTIME_FILES`: the path of a file of `<destination relative to the artifact's directory><TAB><absolute source>` lines, taken from the plan's deployed files and the shared libraries the artifact links (revision 3: TAB-separated, the file always exists, and a test on a row whose dependency is shared needs that library on the device too). The argv grammar does not change and a runner that needs no files ignores the variable. `adb-run` pushes each file beside the pushed program and runs the program from that directory. A test locates its data relative to its own directory; a compiled-in build-machine path is the project's to change.

**Criteria.** On the emulator the fixture without any `ldflags` passes `runs`,
and its test programs name no `libc++_shared.so`; with `adb-run` reading
`MCPP_RUNTIME_FILES`, `reads_relative` passes too. Negative direction: the LLVM
payload's link line on the host row is unchanged, and on the iOS simulator,
whose runner ignores the variable, the three tests still pass.

### 5.7 A7: a payload's recipe closure and pkg-config

**What the code does.** `xpkg_dir` answers for the addresses the owning
manifest and its compiled-in host modules declare (`prepare.cppm:5720-5791`).
`gtk4` declares its dependencies as floors, relocates its `.pc` files and
declares them into `<subos>/usr/lib/pkgconfig` (`xim-pkgindex:
pkgs/g/gtk4.lua:60-84`, `110`, `132`). Payloads are provisioned into the global
registry so that its SubOS is the sysroot (`prepare.cppm:1508-1530`), but
`toolchain_sysroot()` is set only in the `Sysroot` link mode
(`prepare.cppm:1272-1275`), and the bundled LLVM takes `PayloadFirst`
(`linkmodel.cppm:469-476`).

**Measured.** CI, a fresh home declaring only `xim:gtk4` (xlings 2026.9.5.1,
50 payloads installed): the view holds 143 entries and
`PKG_CONFIG_LIBDIR=<view> pkg-config --cflags --libs gtk4` exits 0 with every
path under the payload store; of every `.pc` an installed payload ships, only
`libxml-2.0.pc` is absent from the view, and `libxml2.lua` has no pkg-config
handling. Local, a registry with older installs: the same command fails on
`zlib`, `x11` and five more; that is machine history, not a property of
provisioning.

**Decision.** `mcpp::pkg_config_libdir()` (`MCPP_PKG_CONFIG_LIBDIR`) returns
the registry SubOS's pkg-config directories, independent of the link mode; an
accessor, not an environment default, so a package using the host's
pkg-config (#493) is unaffected. `libxml2` declares its `.pc` files. No
`xpkg_dir` over a closure and no payload versions in `mcpp.lock`: the view
answers the need, and locking payloads is a decision about the payload plane
as a whole.

**Criteria.** A fresh home declaring only `xim:gtk4` runs `pkg-config
--cflags --libs gtk4` with the accessor's value and exits 0. Negative
direction: with the value replaced by an empty directory it exits non-zero.

### 5.8 A8: the resolved graph and package metadata, from a build program

**Measured (local).** Application, `liba`, `libb`, where `libb`'s build program
deploys `strings.txt`: the file is in the application's `bin/resources/libb/`
after `mcpp build` and in the `dir` pack.

**Decision.** No engine change now. A library's resources reach the
application; merging them into one package is a rule in HuxerUI's own rule
module, or a load-time merge. A build-program channel to the graph waits for a
need that transitive deploy and re-exported provisions cannot answer; when it
comes, it reads the record of §5.11.

### 5.9 A9: a platform floor a dependency needs

**What the code does.** `min_platform_version` reads the root manifest, then
the payload's `platform_floor`, then the NDK's metadata
(`prepare.cppm:1619-1670`), and the answer enters the compiler's `--target` at
toolchain resolution (`:3670-3673`), before the dependency worklist (`:6499`).
`version-floor` requirements are compared with stated facts before compiling
(`:7804-7849`); the module that parses them knows no meanings
(`version_floor.cppm`, header).

**Measured (local, Android row).** A dependency whose build program states
`mcpp::fact("demo.android-api", mcpp::min_platform_version())` and
`mcpp::floor("demo.android-api >= 23")`: a root stating 21 is refused before
compiling (`error: \`fw\` requires demo.android-api >= 23, and this machine has
21`), a root stating nothing likewise (the NDK's default is 21), a root stating
24 builds with `x86_64-unknown-linux-android24`, and the host row is silent.

**Decision.** The engine states the target's floor as a fact, in the
platform's words: `android.api-level`, `ios.deployment-target`,
`macos.deployment-target`. A dependency writes `[[runtime.requirements]] kind
= "version-floor" value = "android.api-level >= 23"`; a floor whose fact the
row does not state is silent, so no selector is needed. The refusal names the
manifest key that sets the fact, and the message says "this build targets"
where it says "this machine has". No raising, for the ordering reason above and
because an application's floor is a product decision.

**Criteria.** Android row: a dependency requiring 23 and a root stating 21, or
nothing, is refused naming `min_api_level`; a root stating 24 builds. Negative
direction: the requirement is silent on the host and iOS rows.

### 5.10 A10: `--toolchain` on `run`, `test` and `pack`

**What the code does.** The pre-parse loop publishes `--toolchain` as
`MCPP_TOOLCHAIN` for every command (`cli.cppm:195-199`); only `build` and `why`
declare the option (`:364`, `:538`).

**Measured (local).** `MCPP_TOOLCHAIN=llvm@22.1.8` makes `mcpp test` compile
with `Clang 22.1.8` and `mcpp pack` resolve `llvm@22.1.8`; the three commands
refuse `--toolchain`.

**Decision.** Declare the option on the three subcommands, not globally
(`why --toolchain` is a query with its own declaration).

**Criteria.** `mcpp test --toolchain llvm@22.1.8` builds with clang while the
default is gcc. Negative direction: without the flag the default resolves.

### 5.11 X: the resolved dependency graph, recorded

**What the code does.** `mcpp why deps` prints the lines of `mcpp.lock`
(`doctor.cppm:1089-1110`); `path` dependencies are not locked, so a project of
path dependencies shows "no mcpp.lock". `resolution.json` holds `toolchain`
and `runtime` (`prepare.cppm:12700-12870`).

**Measured (local).** For an application with one `path` dependency,
`mcpp why deps` prints `(no mcpp.lock — run mcpp build or mcpp update)` after a
build, and `resolution.json` has the keys `runtime`, `schema_version` and
`toolchain` only.

**Decision.** `resolution.json` gains `graph`: for each package the existing
`package_json` (identity, version, source provenance), the keys its requesters
wrote with the requester and the declaring table, and its link form with the
reason (`default`, `requested`, `package kind`, `row kind`). `mcpp why deps`
prints the same before the lock's lines. An additive field under the
machine-output guarantees (docs/50 §7).

**Criteria.** The A1, A2 and per-row-kind fixtures assert on `graph` rather
than on warnings. Negative direction: every existing `resolution.json` field
is unchanged.

## 6. Official plugin items

### 6.1 B1: `dist-apple` carries the graph's dylibs

**What the plugin does** (read). It registers `app` (`dist/apple.cppm:952`),
copies the launcher and maps the rest of `<stage>/bin/` into the resource
directory (`:734-777`); `<stage>/lib/` is never read; it signs only with an
identity (`:815-841`).

**Measured.** Local: `mcpp::link_flag("-Wl,-rpath,/m634/marker/Frameworks")`
emitted by a host module and called from the root's build program appears in
the root program's `RPATH`. CI, `macos-15`: the rpath a host module adds with `mcpp::link_flag("-Wl,-rpath,@executable_path/../Frameworks")` arrives in the binary as `<package dir>/@executable_path/../Frameworks`. `normalize_ldflag` anchors every relative rpath to the package root and exempts only a leading `$` (`flags.cppm:266-283`, with a copy for dependencies at `prepare.cppm:6141-6158`), so ELF's `$ORIGIN` survives and Mach-O's `@executable_path`, `@loader_path` and `@rpath` do not; a local probe shows the same on Linux. The bundle therefore cannot load its framework, exactly as the control built without the flag. An unsigned bundle fails `codesign --verify --deep --strict` (`code has no resources but signature indicates they must be present`); signed ad hoc, dylib first and bundle second, it verifies (`valid on disk`, `satisfies its Designated Requirement`). Run 3 tried to add the literal rpath to a copy after link, and the edit failed; run 4 recorded why and measured both remaining routes. `install_name_tool -add_rpath` refuses an mcpp-built program: `larger updated load commands do not fit (the program must be relinked, and you may need to use -headerpad or -headerpad_max_install_names)`. A program linked with `-rpath @executable_path/../Frameworks` and bundled with its dylib in `Contents/Frameworks` verifies after ad-hoc signing, runs (exit 7), and stops with `Library not loaded` without the framework (exit 134). An mcpp-built program linked with `-Wl,-headerpad_max_install_names` through `mcpp::link_flag` accepts the edit and then behaves the same after re-signing.

**Decision.** After §5.3, `dist-apple` copies the staged dylibs into
`Contents/Frameworks/` (macOS) or `Frameworks/` (iOS); the member emits
`mcpp::link_flag("-Wl,-rpath,@executable_path/../Frameworks")` on `*-macos`
(`@executable_path/Frameworks` on iOS), so no file is modified after link;
this needs the engine to treat `@executable_path`, `@loader_path` and `@rpath` in an rpath as loader tokens, as it already treats `$ORIGIN` (§9 item 11, task T3). The link-time route is chosen over an edit after link: the edit needs header padding at link time and a re-signature of the edited program, and the link-time route needs neither (both measured, run 4). Until an identity is given the member signs ad hoc, dylibs first and the bundle second.

### 6.2 B2: `dist-apple` produces a DMG

**Measured (CI, `macos-15`).** `hdiutil create -volname Demo -srcfolder <stage> -format UDZO` exits 0; `hdiutil verify` reports the checksum valid; the image attaches read-only with `Demo.app/Contents/MacOS/app` and an `Applications` link to `/Applications`, and detaches.

**Decision.** `dmg` is a second format of `dist-apple`: the `.app` action's
output and an `Applications` symlink in a staging directory, then one
`hdiutil create -format UDZO` action whose `.dmg` is the terminal artifact. No
engine change.

### 6.3 B3: the runner for a distributable

**What the engine does** (read). `mcpp run --format <f>` packs and then
prepares again without a pack format (`execute.cppm:1873-1885`); the default
runner slot is keyed by triple and shared with `mcpp test` (`:2423`); a named
slot is chosen by `--runner <name>` for a distributable as for a program
(`:1602-1627`).

**Measured (CI, `macos-15`).** With `[target.aarch64-macos.runners] app = ["/bin/sh", "-c", "exec …", "{}"]`, `mcpp run --format app --runner app -- extra` prints `1-2-3 argc=2` and exits 7; `mcpp run --format app` alone reports `…/RApp.app could not be started: Permission denied (error 13)` and exits 126; a plain `mcpp run` runs the program bare and exits 7; with `[target.aarch64-macos] runner = ["open", "-W"]` a plain `mcpp run` hands `-W …/bin/rapp` to `open` and exits with `open`'s 0.

**Decision.** *Engine:* `mcpp run --format <f>` without `--runner` uses the
named runner `<f>` when the graph or the manifest supplies one, and the
default runner otherwise; a distributable that is a directory and meets no
runner is refused before the spawn, naming the runner `<f>` that would reach
it, rather than executed and answered with `Permission denied`. *Official plugin:* `dist-apple` supplies
`mcpp::runner("app", "macapp-run")` on `*-macos` and declares the payload.
*Payload:* `macapp-run` (§7.1).

**Criteria.** A bundle whose program prints `1-2-3` and exits 7, with no runner
in the manifest: `mcpp run --format app` prints `1-2-3` and exits 7. Without
the member, `mcpp run --format app` is refused before the spawn, naming the
runner `app`. Negative direction: a plain `mcpp run` executes the program with
no runner line.

### 6.4 B4: `dist-wix` produces a Burn bundle

`dist-wix` produces `msi` from one `wix build` action (`dist/wix.cppm:584-601`,
`672`). The native SDK archives ship in `xim:wix` (§7.3) and link under both
MSVC-ABI toolchains (measured, §7.3). `setup` chains the MSI; the stock
bootstrapper needs `WixToolset.BootstrapperApplications.wixext`, which the
payload does not carry; a custom bootstrapper is the project's program,
reached as `${mcpp.target_file:<target>}`. No engine change.

### 6.5 B5: `dist-apk`'s two defects, then an Android App Bundle

**Measured (local, mcpp 2026.9.14.1, plugins 0.9.3, `tests/apk-consumer-shared`).**
A first `mcpp pack --target x86_64-linux-android --format apk` produces an APK
whose `lib/x86_64/` holds `libapk-consumer-dep.so`, the application object and
`libc++_shared.so`. A second, with no edit, exits 0 and its APK lacks
`libapk-consumer-dep.so`: the plan wipes the member's stage while the
`apk:needed` stamp lives outside it (`dist/apk.cppm:892-893`, `1164`), so the
walk does not run again. With `--target x86_64-linux-android --target
aarch64-linux-android` the engine stages `lib/x86_64/` and `lib/arm64-v8a/`,
and the pack exits 1 with the engine's `no action claimed --format 'apk'`: the
member reads `<stage>/lib/*.so` without descending (`:626-648`) and prints its
reason to `std::cerr`, which a build program's success does not surface.

**Decision.** Read the staged closure of §5.3 and delete the member's walk
(which removes the stamp); read `lib/<abi>/`; report every refusal through
`mcpp::warning`. Then `aab` (`aapt2 link --proto-format`, `bundletool
build-bundle` from a new `xim:bundletool`, `jarsigner`). No engine change.

**Criteria.** Two packs in a row carry the dependency's library both times;
two triples give one APK listing both ABIs; a member refusal prints its reason.

### 6.6 B6: `rules-metal` and `tools-esbuild`

Neither exists in the plugins; Lib-Live2D writes both in-project. `rules-metal`
is general (the Apple platforms' shader language, the counterpart of
`rules-spirv`, whose one-action-per-shader shape it copies,
`rules/spirv.cppm:751-813`) and belongs to the official plugins now;
`tools-esbuild` serves one project's Web bridge and stays there until a second
consumer. A rule locates an SDK with `xcrun --sdk <sdk> --show-sdk-path`.

## 7. Payload, xlings and index items

### 7.1 C1: a macOS app-bundle runner program

**Measured (CI, `macos-15`).** A program run as `N.app/Contents/MacOS/probe` reports `bundle=…/N.app` and `resource=…/N.app/Contents/Resources/greeting.txt` and exits 7; the same binary copied out of the bundle reports its own directory and `resource=none`.

**Decision.** `macapp-run <X.app> [args…]` in `apple-simulator-tools` (or a new
`apple-app-tools`): require `Contents/Info.plist` (exit 2 naming what is
missing), read `CFBundleExecutable`, `exec` `Contents/MacOS/<exe>` with the
arguments, so stdio and the status are the program's by construction.

### 7.2 C2: `simctl-run`'s output and status

**What the recipe does** (read). A bare executable goes through `simctl spawn`;
a `.app` through `simctl install` and `simctl launch --console-pty
--terminate-running-process … ; exit $?`, whose status the recipe marks
unmeasured (`pkgs/a/apple-simulator-tools.lua:200-236`). The plugins CI run
34770579266 lost the output twice in three attempts; its fixture exits 0 and
its diagnostic uses `cat -A`, which BSD `cat` refuses.

**Measured (CI, `macos-15`, 20 launches per cell).** Run 2, one install and then twenty launches per cell. For an application that exits at once, `--console-pty`, `--console` and `--stdout`/`--stderr` files each kept the marker 20 of 20 times; for one that sleeps a second first, 20, 19 and 20. Every one of the 120 launches returned 0 for an application that exits 7, and the application is absent from `launchctl list` once it exits, so neither simctl nor launchctl reports its status. Run 3: with an install before every launch, `--console-pty` kept the marker 19 of 20 times and returned 0 every time (the plugins CI's condition, reproduced); `simctl spawn` of the installed application's executable, whose path `simctl get_app_container` gives, kept the marker 20 of 20 times and returned the application's 7 every time; for an application that calls `abort()`, `simctl launch` returned 0 and `simctl spawn` returned 134 (`terminated with signal 6`).

**Decision.** For a bundle, `simctl-run` installs it and reads the executable's load
commands. When the executable does not load `UIKit.framework`, the program
asks `simctl get_app_container` for the installed path and runs `simctl spawn
<device> <container>/<CFBundleExecutable> <args>`: measured to return the
application's own status, a signal as a shell status, and its output every
time. When it loads UIKit, the application needs the launch path, so
`simctl launch --console-pty` stays, and the program prints that the status it
returns is simctl's, since no simctl spelling and no launchctl listing reports
the application's (measured). The rule reads the executable, not a guess about
its behaviour. The recipe's UNMEASURED notes are replaced with these readings.
The plugins CI fixture exits non-zero, and its diagnostic uses `od -c`. A
UIKit application under `spawn` is not measured here and is not proposed.

### 7.3 C3: WiX Burn native SDK payloads

Already shipped: `xim:wix` 5.0.2 installs `WixToolset.BootstrapperApplicationApi`
and `WixToolset.DUtil` beside `wix` under HuxerUI's own sha256 pins
(`pkgs/w/wix.lua:1-20`, `83-106`, `186-227`). **Measured (CI, `windows-2022`).**
Run 1 linked `dutil.lib` and `balutil.lib` under the default toolchain
(`lld-link`, MSVC ABI) and under `msvc@system`, and every unresolved symbol was
a Windows import (`MessageBoxA`, `RegOpenKeyExW`, `CoInitializeEx`), none a
WiX symbol; run 2, with the system import libraries named: both toolchains build the program and it runs with exit 0 (the default resolves the `x86_64-windows-msvc` row; `msvc@system` resolves MSVC 19.44.35228). A MinGW
link of the same archives fails (the payload reader's measurement: MSVC's
security-cookie and mangled `StringCch*` symbols). **Decision:** anchor
`runtimes/win-x64/native/mbanative.dll`, add a Windows install-verify job, add
the `.wixext`, and state in the recipe that the archives need an MSVC-ABI
link and the Windows import libraries.

### 7.4 C4: resolving an unmerged recipe from a consumer's CI

**What the pieces do** (read). xlings resolves `xim:` against the index named
`xim`; `--add-xpkg` lands under `local` (`xlings:src/core/xim/catalog.cpp:
397-426`). mcpp seeds the registry's `.xlings.json` from `config.toml
[index.repos.<name>]` only when that file does not exist (`src/config.cppm:
588-596`).

**Measured (CI, `ubuntu-24.04`).** A fresh home whose `config.toml` sets
`[index.repos.xim] url` to a checkout of xim-pkgindex with a recipe added on a
branch: the build installs `xim-x-m634-probe/0.0.1`; the registry's
`data/xim-pkgindex` is a symlink to the checkout and `.xlings.json` names it;
`mcpp index update` exits 0 and both stay; a version committed to the branch after the update (`0.0.2`) then resolves and installs, so the index follows the live checkout. A fresh home without the
override refuses: `package 'xim:m634-probe@0.0.1' not found in the synced index
(xim@artifact:19e264e, …)`. An existing home whose `config.toml` gains the same
table after its first build refuses the same way, and its `.xlings.json` never
names the checkout.

**Decision.** *Usage, today:* a consumer's CI creates a fresh home with the
override before its first command. *Engine:* `[index.repos.<name>]` in
`config.toml` reaches an existing registry too (reconciled on load, adding and
updating the entries it names), so the key never silently does nothing; a
payload installed from an overridden index names that source in the
provisioning line. *Payload CI:* xim-pkgindex's pull-request workflow builds a
consumer fixture through the override. *xlings:* nothing.

### 7.5 D1: six-row descriptors

`pkgs/h/huxerui.huxerui.lua` carries 0.3.0 on three rows; `huxerui.live2d` is
absent; `[package] platforms` accepts the other rows
(`docs/04-mcpp-toml.md:1273-1280`). It follows HuxerUI's release.

## 8. What the project can do today

| item | today |
|---|---|
| A1 | keep the two complementary predicates until §5.1 lands |
| A2 | write the namespaced key in `huxerui-build-rules`, `huxerui-tools` and `huxerui-tests` |
| A5 | one package per suite over a shared support library, selected with `mcpp test -p` |
| A6 | on the Android rows, `[target.<triple>.build] ldflags = ["-static-libstdc++"]` until §5.6 lands |
| A7 | declare the four direct payloads and point pkg-config at `<registry>/subos/default/usr/lib/pkgconfig`; the 37 pins are unnecessary |
| A8 | each library deploys its resource package under a common directory; the framework loads them all |
| A9 | the framework's build program states a fact from `mcpp::min_platform_version()` and a floor against it |
| A10 | `MCPP_TOOLCHAIN=llvm@22.1.8` on the clang job's steps |
| B3 | drop the `open -W` default runner; declare `[target.aarch64-macos.runners] app = [...]` and run `mcpp run --format app --runner app` (measured on `macos-15`: exit 7, and the output and arguments reach the terminal) |
| C4 | a fresh `MCPP_HOME` with `[index.repos.xim] url = "<checkout>"` in `config.toml` before the first command |

## 9. Defects found beyond the issue

1. The Android `dir` and `tar` archives omit their closure and say `walked`
   (measured, §5.3).
2. Test programs on the Android rows cannot load on a device: the static
   libc++ is not found and the contract degrades (measured, §5.6).
3. A second `dist-apk` pack drops the dependency's library (measured, §6.5).
4. `dist-apk` refuses the multi-ABI stage and its reason is not shown
   (measured, §6.5).
5. The link-form degradation for a package that declares `kind = "shared"`
   does not name that statement (measured, §5.1).
6. The version-floor refusal says "this machine has" for any fact (measured,
   §5.9).
7. `[index.repos.<name>]` added to an existing home is ignored without a word
   (measured, §7.4).
8. Two runner emitters in one build program become one argv
   (`modules/buildmcpp/src/directives.cppm:870`; the one-supplier check runs
   across dependencies only, `prepare.cppm:9285-9306`; read).
9. `libxml2.lua` declares no `.pc` into the view (measured, §5.7).
10. The plugins iOS CI step cannot record what it lost, nor measure a status
    (§7.2).
11. An rpath beginning with `@executable_path`, `@loader_path` or `@rpath`,
    given through `mcpp::link_flag` or `[build] ldflags`, is anchored to the
    package directory, because the normalisation exempts only a leading `$`
    (`flags.cppm:266-283`; its copy for dependencies, `prepare.cppm:6141-6158`).
    Measured on `macos-15` and on Linux (§6.1).

## 10. Order and tasks

| id | task | home | depends on |
|---|---|---|---|
| T1 | A1 replacement, labels and option warning; per-row `kind`; the degradation names its statement; the sweep's sub-table names | engine | - |
| T2 | A2 adoption, the canonical-source refusal, the scanner message; mcpp's six manifests | engine | - |
| T3 | A3 reader-driven closure for Android and Mach-O, staging, `walked`, `needs` lines; loader tokens (`$ORIGIN`, `@executable_path`, `@loader_path`, `@rpath`) exempt from rpath anchoring, in one function shared by the two copies | engine | - |
| T4 | A4 default SONAME | engine | - |
| T5 | A6 static runtime by driver query; `MCPP_RUNTIME_FILES` for every runner | engine | - |
| T6 | A10 option declarations | engine | - |
| T7 | X the graph record and `why deps` | engine | T1, T2 (criteria read it) |
| T8 | A9 platform facts and message wording; C4 index repos reach an existing home | engine | - |
| T9 | A5 `[test] discover`; A7 accessor; B3 format-named runner | engine | - |
| T10 | docs (`docs/04`, `05`, `06`, `08`, `09`, `10`, `30`, `50`) and zh mirrors; release; sandbox | engine | T1-T9 |
| P1 | `dist-apk`: staged closure, `lib/<abi>/`, `mcpp::warning` refusals | official plugin | T3 released |
| P2 | `dist-apple`: frameworks placement, link-time rpath, signing; `app` runner; `dmg` | official plugin | T3, T9 released; X1 |
| P3 | `aab`; `setup`; `rules-metal` | official plugin | P1; X3 |
| P4 | plugins CI: the iOS fixture exits non-zero; portable diagnostic | official plugin | - |
| X1 | `macapp-run` | payload | - |
| X2 | `simctl-run` per §7.2; `adb-run` transfers `MCPP_RUNTIME_FILES` | payload | T5 released |
| X3 | `xim:wix` close-out; `xim:bundletool`; `libxml2` pkg-config | payload | - |
| X4 | xim-pkgindex PR workflow through the index override | payload CI | T8 released (or a fresh home today) |
| H1 | HuxerUI: the A2 keys, `open -W`, `MCPP_TOOLCHAIN`, the four GTK payloads | project | - |
| I1 | D1 | index | upstream release |

T1 to T9 are independent engine changes and form one PR, each with its own
criterion; T7's fixtures are written against T1 and T2.

## 11. Readings

Every measurement this record uses, with its source. Local probes are in
`2026-09-14-634-probes.sh`; CI scripts are on the branch of mcpp#635.

| id | question | where | reading | decides |
|---|---|---|---|---|
| L1 | is a conditional `linkage` applied | local, host row | the second declaration: exit 0, no `libfw.so`, no diagnostic; the modifier-only table: exit 2 under the label `[dependencies]`, `tried: (demo.fw, linkage)`; two complementary predicates: `libfw.so` and `NEEDED libfw.so` | A1 |
| L2 | does any manifest declare one dependency key in both tables | local scan of 509 manifests | none | A1 replacement |
| L3 | what does `kind = "shared"` do to a default request | local | linked shared; an explicit `static` request warns without naming the package's statement | A1 per-row kind; §9 item 5 |
| L4 | is a key's namespace checked against the manifest | local | two edges: the scanner names one file twice; one edge: builds silently; 9 of 46 `path` edges disagree (6 in mcpp's examples, 3 in HuxerUI) | A2 |
| L5 | what does the Android `dir` pack stage | local | only `lib/libapp.so`, whose `DT_NEEDED` names `libfw.so` and `libc++_shared.so`; the manifest says `closure = walked` | A3 |
| L6 | does an ELF shared library carry a SONAME | local | none on the host `libfw.so`, the Android `libfw.so` or `libapp.so` | A4 |
| L7 | what do Android test programs need | local | `libc++_shared.so` by default; nothing but `libm.so`, `libdl.so`, `libc.so` with `-static-libstdc++`; the NDK and LLVM drivers name their static archives for the effective target | A6 |
| L8 | do a dependency's deployed files reach the application | local | yes, from two levels down, into `bin/` and the `dir` pack | A8 |
| L9 | can a dependency refuse a low platform floor today | local, Android row | a build-program fact and floor refuse 21 and the unset default, accept 24, and are silent on the host | A9 |
| L10 | does `MCPP_TOOLCHAIN` reach `test` and `pack` | local | yes (clang compiles the test; pack resolves `llvm@22.1.8`); `--toolchain` is refused by `run`, `test`, `pack` | A10 |
| L11 | what shows the resolved dependencies | local | `mcpp why deps` prints only `mcpp.lock` lines ("no mcpp.lock" for path dependencies); `resolution.json` has `runtime`, `schema_version`, `toolchain` | X |
| L12 | does a host module's `link_flag` reach the root's link | local | yes for an absolute rpath; an `@executable_path` rpath is anchored to the package directory | B1; §9 item 11 |
| L13 | does `dist-apk` 0.9.3 pack twice, and for two ABIs | local, `tests/apk-consumer-shared` | the second APK lacks `libapk-consumer-dep.so`; two ABIs exit 1 with `no action claimed --format 'apk'` | B5 |
| L14 | does this machine's registry view resolve `gtk4` | local | no (`zlib`, `x11` and five more absent); a history reading, superseded by C1 | A7 |
| C1 | does a fresh home's view resolve `gtk4` | CI `ubuntu-24.04`, runs 1 and 2 | 143 entries, `pkg-config --cflags --libs gtk4` exits 0; only `libxml-2.0.pc` of every shipped `.pc` is absent | A7 |
| C2 | does a `config.toml` index override serve a branch checkout | CI `ubuntu-24.04`, runs 1 and 2 | fresh home: `0.0.1` installs, the index is a symlink to the checkout, `index update` keeps it, `0.0.2` committed afterwards installs; no override: refused naming the address; existing home with the override added later: refused, `.xlings.json` never names the checkout | C4 |
| C3 | do the WiX `v14` archives link under mcpp's Windows toolchains | CI `windows-2022`, runs 1 and 2 | run 1: only Windows imports unresolved on both; run 2 with the import libraries: both build and the program exits 0 | C3, B4 |
| C4 | do test programs run on the emulator | CI API 34 emulator, runs 1 and 2 | run 1: `CANNOT LINK EXECUTABLE … libc++_shared.so not found` for every test; run 2 with `-static-libstdc++`: `runs` passes, `reads_relative` cannot open `/data/local/tmp/data/data.txt`, the host path is absent | A6 |
| C5 | does a Mach-O program run staged beside its dylib | CI `macos-15`, run 2 | exit 7; exit 134 with `Library not loaded` when the dylib is removed; `pack --format dir` refuses today | A3 |
| C6 | does a Frameworks bundle load and verify | CI `macos-15`, runs 2 to 4 | the `link_flag` rpath is anchored and the bundle cannot load; unsigned verification fails, ad-hoc signing verifies; with the literal rpath from the link (run 4): verifies, exit 7, exit 134 without the framework; an rpath edit of an unpadded mcpp build fails (`larger updated load commands do not fit`), of one linked with `-headerpad_max_install_names` succeeds | B1 |
| C7 | can the base system make and mount a DMG | CI `macos-15`, run 2 | create, verify, attach with the `Applications` link, detach: all exit 0 | B2 |
| C8 | does the main bundle resolve when its executable is run directly | CI `macos-15`, run 2 | yes, with its resource; a copy outside the bundle does not | C1 |
| C9 | which runner reaches a distributable | CI `macos-15`, run 2 | `--format app --runner app`: exit 7, `1-2-3 argc=2`; `--format app` alone: exit 126; plain `run`: exit 7; an `open -W` default runner receives a plain run's program and exits 0 | B3 |
| C10 | which `simctl launch` spelling keeps output and status | CI `macos-15`, run 2 | run 2: the marker in 119 of 120 launches (one loss, `--console`); every launch exits 0 for an application that exits 7; run 3: an install before each launch: `--console-pty` 19 of 20, exit 0; `simctl spawn` of the installed executable: 20 of 20, exit 7; an aborting application: `launch` 0, `spawn` 134 | C2 |
| C11 | do test programs run on the iOS simulator | CI `macos-15`, run 2 | all three pass, both file reads included | A6 |

Run 1's macOS readings were lost: its artifact upload hung after the measurements finished, and cancelling the job discarded its log; run 2 split the job and dropped the upload.

## 12. Self-review

- **What is measured.** Every decision's premise in §5 to §7 has a reading in
  §11, except where a section says *read*: A5's target-model citation, B4's
  and B6's plugin steps, D1, and §9 item 8. The CI readings measure the
  released 2026.9.14.1; the engine changes themselves are not implemented.
- **What was not taken.** A modifier-only dependency table (ambiguous with a
  namespace table); a selecting `requires_linkage` (a second meaning for
  `requires_*`); named test targets with exclusive sources and labels (the
  target model's rule); a closure `xpkg_dir` and payload versions in the lock;
  a graph API for build programs now; raising a floor from the graph; an xlings
  overlay; a Mach-O load-command editor, and an rpath edit after link for B1
  (it needs header padding and a re-signature, measured in run 4); `simctl
  spawn` for a UIKit application; an Apple SDK accessor.
- **Earlier decisions revisited.** #622's "the Android closure is the
  provider's" (§5.3); Phase 1b's `insert()` (§5.1); #630 §4's `LC_RPATH`
  question (§5.3); revision 1's six decisions listed in §2.
- **Negative directions.** Every criterion has one.
