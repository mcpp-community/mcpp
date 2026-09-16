---
subject: triage
status: landed
---

# Four issues from a framework and an editor: #646, #647, #648 and #649, read against the engine and routed by home

**Status:** closed on 2026-09-16. Every row of this record is implemented and
published: mcpp 2026.9.16.1 (mcpp-community/mcpp#650 and #651),
mcpp-plugins 0.12.0 (#28), mcpplibs/mcpp-index#432 and #433,
openxlings/xlings#597 and openxlings/xim-pkgindex#845. The implementation, its
refinements, its readings and the sandbox verification are
`2026-09-16-646-649-implementation-plan.md`; its §9 is the closure and §1.10
states what the review before the release changed.

Two decisions in this record were revised while implementing. F2 is confirmed
rather than only probable, and the Mach-O default stays with a diagnostic
(D10, plan §0.1 row R5). E4.2's gate compares a restatement's source with the
declaration in effect, and the comparison is made on what the declarations
mean rather than on their bytes, because the byte comparison refused a
manifest 2026.9.15.2 builds (plan §1.10 item 19).

Engine code was read at `2fc7b5b0` (origin/main, after mcpp 2026.9.15.2).
mcpp-plugins, openxlings/xlings (`3cd8061`) and mcpplibs/mcpp-index were read
at their current heads. A statement marked *measured* was run on a Linux
x86_64 host with the released mcpp 2026.9.15.2, `gcc@16.1.0`, `llvm@22.1.8`
and `xim:android-ndk@30.0.16248370`; the probes are
`2026-09-16-646-649-probes.sh` (groups a to d), and §11 quotes their
`READING` lines. A statement marked *read* names a file and line and was not
executed. Nothing in this revision ran on macOS or Windows; §9 lists the
premises that need a temporary pull request before their decisions are taken.

The four issues hold 23 items. Measuring them found twelve defects that no
issue reports, two of which are more severe than anything the issues list: the
default llvm build of a program over a C++ shared library aborts on Linux
(F3a), and every plan of a project with a bare `compat.*` dependency starts a
network index refresh once the two-minute debounce has passed (T).

§0 is the ledger. §1 lists where the measured state differs from the issues,
§2 the defects beyond them. §3 states the rules the decisions share. §4 to §8
take the items by subject. §9 collects the decisions for the reviewer and the
measurements still owed, §10 the order of work, §11 the readings, §12 what
each project can do today, §13 the self-review.

## 0. The ledger

"Home" follows the routing rule of the #634 record: **usage** (a spelling or
an existing mechanism), **project**, **official plugin** (mcpp-plugins),
**ecosystem data** (xim-pkgindex, xlings, mcpp-index), **engine**. A defect
stays in the engine whatever its size; the rule filters features, not
defects. "Beyond" marks a finding no issue reports.

| # | real | general | home | decision (§) | evidence |
|---|---|---|---|---|---|
| F3a | defect, beyond | yes | engine | on ELF, every C++ image a plan loads into one process shares one C++ runtime; a program over a plan-built C++ shared library takes the shared-library contract (§4.1) | measured: llvm default aborts with `std::bad_cast`, exit 134; gcc default runs with 900 interposed libstdc++ symbols |
| F3b | defect (false report) | yes | engine | `STB_GNU_UNIQUE` is vague linkage; a duplicate whose two definitions come from one plan object is not a conflict (§4.2) | measured: 2 (llvm) and 8 (gcc) findings under a uniform contract; `--strict` exits 1 |
| F1 | defect | yes | engine | a static package reachable from exactly one shared image is linked into that image; one reachable from two images is refused with the `linkage = "shared"` remedy (§4.3) | measured: `-z defs` link fails, a foreign `dlopen` fails, ELF interposition hides both |
| F2 | probable defect | yes | engine, measurement first | measure on macos-15; the F3a rule per format decides the change (§4.4) | read: libc++ `typeinfo`; not measured on the payload origin |
| E10 | defect in the record, and a default to decide | yes | engine | step 1: the record and docs state the static CRT the row delivers, and a `cxx_runtime` the row cannot deliver is diagnosed; step 2 (own record, after windows-2022): whether the row adopts `/MD` (§4.5) | measured: the driver's `-defaultlib:libcmt`; read: no CRT flag outside the `msvc` dialect |
| E1 | gap | yes | engine | the root build program reads a graph file (`mcpp::graph_file()`): packages in dependency order with manifest directory, kind, features, link form and verbatim `[package.metadata]`; its digest joins the re-run key (§5.1) | measured: the program sees direct dependencies only; a metadata edit replays the cached run |
| E2 | feature | Apple rows | official plugin (`rules-swift`) | one package's Swift compiles through `action` objects, `link_flag` and a generated header; cross-package Swift modules wait for an engine interface channel (§5.2) | read; no `swiftc` on the host |
| E3 | defect | yes (macOS host, every non-Apple hosted row; latent on Windows) | engine | choose the link branch by the target's object format, in a pure function with a host-by-row unit test that runs on Linux (§5.3) | read line by line; the issue's CI log |
| E5 | defect | yes | engine; plugin workaround retires | strip the program on every row and every shared library the graph built; strip staged toolchain runtime copies; report what was done; expose the decision to build programs (§5.4) | measured: desktop `libdep.so`, Android program, `libdep.so` and `libc++_shared.so` unstripped under "stripped" |
| E9 | gap, and an inconsistency | yes | engine | `mcpp pack --message-format json` prints one `mcpp.pack` envelope; `pack` accepts `--release` and `--dev` with `build`'s precedence (§5.5) | measured: three `unknown option` refusals |
| E4.1 | defect | yes | engine | the forward validator accepts a key declared in any dependency table of the manifest, on any row (§6.1) | measured: warning and `--strict` exit 2 while the forward applies |
| E4.2 | documentation defect, and a silent drop beyond | yes | docs; engine (one check) | the docs say to restate the source; a restatement whose source differs is refused (§6.2) | measured: the restated form works; a different path is ignored under `--strict` |
| E4.3 | defect | yes | engine | one helper names a provider for a consumer; `dep_dir`, `dep_linkage` and `dep_bin` use it (§6.3) | measured: `qualified=[]` |
| E6 | defect | yes | engine | a package with no library target contributes nothing to a consumer's target graph; its programs come from the tool sub-build alone; package cycles are checked at resolution (§6.4) | measured: builds with `--cache=local`; the tool's own dependency is linked into the application |
| E7 | gap | yes | engine | a git dependency whose identity differs from the root manifest's is looked up among the repository root's `[workspace] members`; no `subdir` key (§6.5) | measured: member-only is refused; both keys drop the `tools` request |
| E8 | defect (narrower than stated), and a gap | yes | engine | a `dep/feature` token is a root forward and never a macro; a plain undeclared name without `[features]` keeps its documented meaning (§6.6) | measured: `-DMCPP_FEATURE_SPIKE_FW_INSTALLER` |
| T | defect, beyond | yes | engine; the xlings manifest | the refresh decision walks the resolver's deprecated bare-name rung before it calls a miss (§7.0) | measured on openxlings/xlings |
| A1 | gap | yes | engine | refusal code `offline-download-required` at every offline refusal site; envelope code `MCPP_OFFLINE_DOWNLOAD_REQUIRED` naming the first missing item (§7.1) | read: five prose-only sites |
| A2 | defect | yes | engine | the saved stdout descriptor is close-on-exec (Windows: not inheritable) (§7.2) | measured: a build program holds the caller's pipe as fd 3 |
| A3 | defect | yes | engine; xlings for its own connection timeout | xlings runs under the owned launcher; refresh has a total deadline, install an inactivity deadline (§7.3) | read |
| A4 | defect against docs/50 | yes | engine | a per-run record of observed effects; a launched network child records `network` (§7.4) | read |
| A5 | defect (docs and code disagree) | yes | engine | the three refreshes that bypass `mcpp.pm.index_refresh` go through `decide_for_miss` (§7.5) | read |
| A6 | ecosystem-data defect, beyond; then a default | yes | mcpp-index, then engine | the index artifact becomes byte-reproducible and is republished; then the default `artifact` becomes a region object (§7.6) | measured: GitCode's asset digest differs from the pointer |
| P1 | feature | yes | official plugin | `dist-apple` `options::omit_keys`, restricted to the defaulted keys (§8) | read |
| P2 | feature | yes | official plugin | `dist-web` `options::page`, default `index.html` (§8) | read |

The mcppls-side measures of #648 (offline by default, its own process group,
a cached last document) belong to the caller and are correct as stated (§7.7).

## 1. Where the measured state differs from the issues

1. **F3's 882 symbols are a split runtime, not the module initialiser.** The
   default ELF contracts give the program `self-contained` and the shared
   library `toolchain-coupled` (`resolution.json`, reading a). Under gcc the
   program's static libstdc++ is exported and interposes the library's
   `libstdc++.so.6` (900 conflicts, the run succeeds). Under llvm the same
   shape aborts when run (exit 134), even when the program itself does not
   import `std`. Only under a uniform contract do the initialiser findings the
   issue describes remain (§4.1, §4.2).
2. **F1 is described as intended by the code.** `symbol_provision.cppm:36-43`
   and the #519 design call the arrangement benign and state that PE and
   Mach-O "structurally do not have this problem". That holds only on ELF, and
   only for a consumer that linked the static package itself: `-z defs`
   refuses the library, and a program that did not link `x` cannot `dlopen`
   it (reading a, F1c and F1d).
3. **E10's record is false, and no value of `cxx_runtime` reaches the row.**
   The PE contract table treats clang on the MSVC ABI like `cl.exe` and
   records `host-coupled` (`distribution.cppm:615-686`), while the driver
   links `libcmt` (reading a, E10) because no CRT flag is emitted outside the
   `msvc` dialect (`flags.cppm:888-895`).
4. **E1: dependency build programs run dependents first**, in discovery order
   (`prepare.cppm:9551-9640`); `[package.metadata.*]` is accepted silently
   today, even under `--strict`; and the engine cannot resolve paths inside
   metadata it does not interpret.
5. **E3 is wider than Android.** The macOS host branch serves every non-Apple
   hosted row whose C library comes from a payload (wasm32-emscripten
   included, read), the Windows non-MSVC branch has the same shape
   (`flags.cppm:1568-1570`, latent: the NDK publishes no Windows archive), and
   no Linux test can see it because the branch is chosen by `if constexpr`
   and the Android e2e scripts are gated `# requires: gcc`.
6. **E5: the Android row strips nothing because it never reaches the strip
   step.** `run()` dispatches a shared-object program to
   `run_shared_program` (`pack.cppm:1733`), which does not call
   `strip_program` and ignores `--debug-symbols`. The word "stripped" is the
   decision, printed before the outcome (`pipeline.cppm:499-502`).
7. **E4.2: the restated form already works**, and docs/30 states it
   correctly (`docs/30:1387-1392`). The defect is docs/05 and its translation,
   plus a restatement with a different source that is ignored without a word.
8. **E6: the cycle is in the consumer's own target graph, not in a key.** The
   tool package's `[dependencies]` are walked into the consumer graph
   (`prepare.cppm:7680-7686`), so the same manifest builds with
   `--cache=local` (the only walk that detects cycles runs for the global
   cache, `:12790`), and a tool's own library dependency is linked into the
   application (reading c, E6c). Keying builds on features does not remove
   that edge.
9. **E7: the both-keys spelling drops the member's `tools` request silently**
   (`recordDependencyEdge` returns early on a second declaration,
   `prepare.cppm:6304-6313`).
10. **E8: accepting any plain name without a `[features]` table is
    documented** (docs/06:40-42, pure macro usage). The defect is that a
    `dep/feature` token, which can only be a forward, becomes the macro
    `MCPP_FEATURE_SPIKE_FW_INSTALLER`.
11. **#648: the refresh was started by a disagreement, not by a stale index**
    (§7.0). The pipe leak also reaches children started through
    `posix_spawn`, not only `popen` (§7.2). The CN mirror the issue asks for
    is deployed, and its artifact does not match its pointer (§7.6).
12. **A1 cannot name every missing item.** Planning stops at the first
    refusal; the diagnostic names the first (§7.1).
13. **A3: mcpp already owns its children.** The process group, the signal
    guard and the Windows job object were built for ninja; the xlings calls
    are the remaining exception (§7.3).

## 2. Defects beyond the issues

| # | defect | severity | § |
|---|---|---|---|
| F3a | the default ELF contracts put two C++ runtimes in one process; llvm aborts | a working-looking build that does not run | §4.1 |
| T | the refresh decision disagrees with the resolver on bare `compat.*` names; a network refresh every two minutes | every build of such a project, online | §7.0 |
| A6 | GitCode's index artifact digest differs from the pointer | the CN route falls back to GitHub | §7.6 |
| X1 | a second declaration of one dependency by one consumer loses its `tools`, `features`, `host-module` and `reexport` (`prepare.cppm:6304-6313`) | silent, under `--strict` | §6.5 |
| X2 | package-edge cycles are detected only by the global-cache key walk | the same manifest passes with `--cache=local` | §6.4 |
| X3 | a tool cycle is refused after four nested sub-builds, with a repeated prefix and no edge named (`tool_store.cppm:62`) | diagnostic | §6.4 |
| X4 | `[package]` accepts unknown keys silently, under `--strict` too; `[build]`, `[targets]` and `[test]` do not | a misspelt key is ignored | §5.1 |
| X5 | `run` gives `--release` precedence over `--profile`; `build` does the reverse (`cmd_build.cppm:477-479`, `:109-114`) | one command line, two profiles | §5.5 |
| X6 | a git dependency's compile banner prints `v` with no version (`execute.cppm:886-891`) | cosmetic | §6.5 |
| X7 | `mcpp why deps --features` is `unknown option` | a feature build's graph is visible only in `resolution.json` | §6.6 |
| X8 | a forward to a dependency declared for another row fails `--strict` | a portable manifest cannot be strict | §6.1 |
| X9 | the only qualified-`dep_bin` test uses the legacy dotted name (`tests/e2e/187:170`) | E4.3 was untested | §6.3 |

## 3. The rules the decisions share

The items are many; the rules they violate are few. Each decision below is an
application of one of these, and a reviewer who rejects a rule rejects its
decisions together.

- **R1. One process, one C++ runtime; one static package, one image.** A
  type's identity, a locale facet's id and a static package's state exist
  once per process only if one image defines them. F3a, F2 and E10 are the
  runtime instance; F1 is the general instance. #641's refusal
  `shared-library-cxx-runtime` is the precedent: it enforced the rule for a
  graph runtime and left the payload's defaults and ordinary static packages
  outside it.
- **R2. One question, one derivation.** Every "second copy" in this batch
  answers a question another function already answers: the refresh decision
  and the resolver (T), the refresh policy and three bypasses (A5), the
  forward validator and the injector (E4.1), the tool variable and
  `fillDepDirs` (E4.3), the host branch and the target format (E3), the CLI
  feature parser and the manifest forward (E8), `run` and `build` profile
  precedence (X5). The fix in each case deletes a derivation rather than
  correcting it, because a corrected copy drifts again.
- **R3. A child mcpp starts is owned, bounded, and inherits only what it
  needs.** The launcher that owns ninja and build programs already exists.
  A2 and A3 move the remaining children onto it and close the one descriptor
  that escapes it.
- **R4. What mcpp reports is what happened.** The envelope's `effects` (A4),
  its error codes (A1), pack's status line (E5), pack's artifacts (E9), the
  runtime record (E10) and the symbol-provision check (F3b) each state
  something the build did not do, or omit something it did. docs/50 already
  states the rule for `effects`; this batch applies it to the rest.
- **R5. The engine states facts; plugins build products.** A graph with
  metadata (E1), a strip decision (E5) and the existing `action` and
  `link_flag` channels (E2) are facts and mechanisms; merging resources,
  packaging an APK, compiling Swift and writing an Info.plist are products
  (E1's consumers, E2, P1, P2).
- **R6. Published data keeps loading on older clients.** A manifest shape
  that older clients refuse cannot be introduced to express something the
  current grammar already expresses (E4.2), and a new key that older clients
  ignore must not change what is built when ignored (E7's rejected `subdir`).
  E1's metadata table is admissible under this rule precisely because older
  clients already ignore it.

## 4. #646 and #649 E10: images, runtimes and placement

### 4.1 F3a: the default ELF contracts put two C++ runtimes in one process

**Measured (group a).** A program over a `kind = "shared"` C++ library, both
importing `std`, with `std::format` in the library:

```
F3 gcc  contracts in resolution.json: {"distributable": "self-contained", ..., "shared-library": "toolchain-coupled", ...}
F3 gcc  NEEDED program: [liblib.so] [libm.so.6] [libgcc_s.so.1] [libc.so.6]      NEEDED liblib.so: [libstdc++.so.6] ...
F3 gcc  run: lib-3 exit=0      bin/app conflicts=900
F3 llvm run: libc++abi: terminating due to uncaught exception of type std::bad_cast: std::bad_cast exit=134
F3 llvm-no-println run: ... std::bad_cast exit=134
F3 llvm-tc (cxx_runtime = "toolchain-coupled") run: lib-3 exit=0
F3 llvm-static-lib (kind = "lib") run: lib-3 exit=0
```

The lead reproduced the llvm abort independently, through `mcpp run` as well.

**Mechanism (read).** `default_contract` returns `toolchain-coupled` for an
ELF shared library since #414, which stopped shared libraries from embedding
libstdc++ and hijacking the program's runtime, and `self-contained` for an ELF
program. Neither default is wrong alone; together they place a static C++
runtime in the executable and a shared one in the library. The executable
exports the runtime symbols the library references, so the library binds
part of its runtime to the program's copy and part to its own. The `_Unwind_*`
symbols among the 46 llvm conflicts are the partial interposition already
recorded for the unwinder in 2026-08; the `bad_cast` is probably a locale
facet looked up by an id that exists twice, which was not isolated.

- **Kind:** defect, general to every ELF project whose program depends on a
  C++ shared library it builds. It is the ELF instance of F2.
- **Home:** engine (`distribution.cppm`).
- **Decision (R1).** When a plan's root image links a C++ shared library that
  the plan builds, the contracts of its programs and tests are derived from
  the shared-library contract (`toolchain-coupled`) instead of the
  per-role default. An explicit `cxx_runtime = "self-contained"` on the
  program in such a graph is refused before compiling, with a new refusal
  code beside `shared-library-cxx-runtime`, naming the library and the split.
  Unlike #641 M3, no refusal is needed for the default: a mechanism that
  delivers one runtime exists.
- **Cost.** A gcc program over a C++ shared library gains `NEEDED
  libstdc++.so.6`. The process already requires that file, through the
  library's own `NEEDED`, so no deployment gains a requirement; the change is
  where the program binds.
- **Scope.** The first criterion covers shared libraries the plan builds. A
  prebuilt C++ shared library from the store is the same class and is
  detectable from its `DT_NEEDED`; it is named here and not decided, because
  its detection runs after linking rather than at planning.
- **Criterion.** The llvm default shape runs (today exit 134); the gcc default
  shape reports no libstdc++ conflicts; `cxx_runtime = "self-contained"` on
  the program is refused with the code.

### 4.2 F3b: the symbol-provision check reports what the loader unifies by design

**Measured (group a).** Under a uniform contract the llvm shape reports two
conflicts (`_ZGIW3std`, `_ZGIW3stdW6compat`) and the gcc shape eight (the
initialiser and seven `STB_GNU_UNIQUE` objects such as
`__from_chars_alnum_to_val_table<false>::value`). Each finding is
`diag::degraded` (`ninja_backend.cppm:3283-3297`), so `--strict` exits 1 on
every leg. The initialiser in `std.o` is one global function whose body is a
return (gcc) or a frame push, pop and return (clang): no guard, no state.

- **Kind:** defect (a false report, R4), general.
- **Home:** engine (`symbol_provision.cppm`, `src/runtime/elf.cppm:742`).
- **Decision.** (1) `STB_GNU_UNIQUE` counts as vague linkage, as `STB_WEAK`
  already does. (2) A duplicate is not reported when both definitions come
  from one object of this plan linked into both images; the decision is by
  provenance, not by a `_ZGIW` name pattern, so a real duplicate with an
  initialiser-shaped name is still reported. Today that object set is
  `std.o` and `std.compat.o`.
- **Rejected: one owner of `std.o`.** An image without the definition cannot
  link on Mach-O or PE and cannot be loaded by a host that did not link the
  owner (F1c and F1d prove the same for any symbol); sibling shared libraries
  have no common owner; and the object holds nothing worth owning.
- **Criterion.** Under a uniform contract, no `build/symbol-provision` finding
  and `--strict` exits 0; a static zlib in the program over a shared `libz`
  still reports.

### 4.3 F1: a static dependency of a shared package goes into the program

**Read.** A dependency-owned shared unit receives its own package's objects
and its direct shared dependencies (`plan.cppm:1914-1929`); the root's units
receive every compile unit of every non-shared package
(`plan.cppm:1994-2000`, `2100-2115`).

**Measured (group a).**

```
F1a libfw.so inputs: obj/mcpplibs_fw/src/fw.o   undefined x_answer=1   program exports x_answer=1   run exit=0
F1c libfw.so relinked with -z defs: exit=1 undefined reference to `x_answer'
F1d foreign host dlopen(libfw.so, RTLD_NOW): exit=1 ... undefined symbol: x_answer
F1b root also depends on x: x.o linked into program=1 libfw.so=0; run counter=2
F1f libfw.so relinked with x.o as well: libfw defines x_counter=1; run counter=2
F1e root-owned shared image over x: libfw.so defines x_answer=1 undefined=0
```

F1c is the Linux form of the Mach-O `-undefined error` and PE link claims;
F1d is the Linux form of Android's `System.loadLibrary("fw")` before the
application, and of any consumer outside mcpp. F1f shows that ELF also hides
the state split a duplicate would cause elsewhere: the interposed copy wins
and the counter reads as shared.

- **Kind:** defect, general (every format but ELF-with-this-consumer).
- **Home:** engine (`make_plan`). Placement is plan logic; no cheaper home
  exists.
- **Decision (R1).** For each shared image, its static closure is the set of
  static packages reachable from its package without crossing another shared
  package; `directPackageDeps` (`plan.cppm:1800-1827`) already holds the
  edges. A static package in exactly one closure is linked into that image and
  removed from the root's object set. A static package in two or more closures
  (two images, or an image and the program) is the "one library, one provider,
  one form" conflict and is refused before compiling, with a refusal code
  naming the package, the images, and the remedy that `linkage_form` already
  implements: `linkage = "shared"` on that package, per dependency or as its
  default. Silent duplication is rejected because on Mach-O and PE each image
  would keep its own state, which F1f shows ELF would hide.
- **Consequence.** The comment at `symbol_provision.cppm:36-43` is rewritten;
  the arrangement it calls benign is removed.
- **Criterion.** On Linux, M3b's `libfw.so` links under `-Wl,-z,defs` and
  loads from a foreign host; F1b is refused naming `x` and builds after
  `linkage = "shared"` on `x`; on macos-15, M3b's dylib links (temporary
  pull request, §9.2).

### 4.4 F2: the payload's Mach-O shared default

**Read.** On macOS the deployment floor is never empty (`macos.cppm:91,
105-111`), so programs and dylibs alike are `self-contained` and embed
`-Wl,-load_hidden,libc++.a` and `libc++abi.a`
(`distribution.cppm:214-251, 572-593`); `toolchain-coupled` is refused on
Mach-O (#202). The payload's `include/c++/v1/typeinfo:146-187` states that on
arm64 Apple types are "considered different types" across linked image
boundaries under the non-unique RTTI bit, and x86_64 Apple compares unique
type information by address. The inference is that a `std::runtime_error`
thrown in a dylib is not caught by its class in the program, and that any
libc++ object compared by address, such as an `std::error_code` category,
compares unequal across images.

- **Kind:** probable defect, general; working builds depend on the default.
- **Home:** engine, after a measurement.
- **Decision.** No change before the measurement. The measurement (§9.2) runs
  the #641 M3 app/fw pair without `llvm.libcxx` on macos-15 against the
  released tarball, in three legs: the default; `cxx_runtime = "host-coupled"`
  project-wide (both images on `/usr/lib/libc++.1.dylib`); and the default
  program over a `{ shared = "host-coupled" }` library. The reading names
  whether `std::runtime_error`, the library's own exception type and
  `std::errc` comparisons survive the boundary. If the default leg splits,
  R1 applies per format: on Mach-O the only runtime shared across images is
  the system's, so a graph with a C++ dylib couples every image to it, subject
  to the floor, and an explicit `self-contained` program receives a
  diagnostic. Because working builds change, the first release warns, as the
  rule-E precedent did.
- **PE.** The `msvc` dialect defaults to `/MD` and shares one CRT across DLLs
  (`distribution.cppm:615-620`). The llvm row gives every image a static CRT
  (§4.5); MSVC exception handling matches catch types by decorated name
  (read), so the type split probably does not occur there, while per-image
  CRT state (`FILE*`, `errno`, locale, `atexit`) does. The windows-2022 leg of
  the same fixture answers it.

### 4.5 E10: the llvm row's runtime on `x86_64-windows-msvc`

**Measured (group a, the driver's decision is host-independent).**

```
E10 compile, no flag: dependent-lib= defines=
E10 link, no flag: -defaultlib:libcmt -defaultlib:oldnames
E10 -fms-runtime-lib=dll: compile --dependent-lib=msvcrt --dependent-lib=oldnames  link -defaultlib:libcmt -defaultlib:oldnames
```

**Read.** The CRT flag is emitted only for `isMsvcDialect`
(`flags.cppm:888-895`); the clang std module path never takes
`msvc_crt_flag` (`stdmod.cppm:330-346`), whose comment says non-MSVC dialects
yield `""` while `msvc_crt_flag(gnu, true)` returns `"-static"`
(`dialect.cppm:67, 134-135, 211`). The PE contract table records
`host-coupled` for the row (`distribution.cppm:615-686`), and runtime staging
is guarded on `CompilerId::MSVC` (`flags.cppm:1370`).

The issue's three questions, answered:

1. The static runtime is not a decision. It is clang's link-time default
   (`-defaultlib:libcmt`), and without `_DLL` the MSVC STL headers select the
   static C++ library, which matches the imports HuxerUI read. The record says
   otherwise.
2. No value selects the dynamic runtime. `host-coupled` and
   `toolchain-coupled` are recorded, not delivered, with no diagnostic.
3. Every llvm-row DLL carries its own CRT and STL, so the row is in F2's class
   for CRT state; whether exception identity also splits is the windows-2022
   measurement.

- **Kind:** defect (R4: the record and the artifact disagree), and a default
  that deserves a decision. General.
- **Home:** engine.
- **Decision, in two steps.** Step 1 changes no artifact: for clang on the
  MSVC ABI the record states `self-contained` unless a runtime flag is
  emitted; an explicit `host-coupled` or `toolchain-coupled` receives a
  degraded diagnostic saying the row does not deliver it; the `msvc_crt_flag`
  comment is corrected; docs/20 states the row's model. Step 2, in its own
  record after the windows-2022 measurement, decides whether the llvm row
  adopts the `cl.exe` model (`/MD` by default through `-fms-runtime-lib`, the
  same `msvc_wants_static_crt` derivation, the std module included, the
  redistributable staging extended to the row). Step 2 is CMake parity and
  changes every existing llvm-row artifact, which is why it is not taken here.
- **Criterion (step 1).** `resolution.json` for the default llvm row on
  windows-2022 records `self-contained`, and `cxx_runtime = "host-coupled"`
  prints the diagnostic.

## 5. #647 E1 to E3 and #649 E5, E9: what build programs see, and what pack does

### 5.1 E1: the resolved graph and package metadata

**Read.** The root build program runs at step L3
(`prepare.cppm:10710-10808`), after resolution, feature activation, every
dependency's build program and link-form resolution, so the whole graph is
known when it runs. `fillDepDirs` publishes only the consumer's visible
provisions: direct dependencies and reexports, under the bare and the
qualified name (`:6016-6055`). `resolution.json` receives a `graph` section
(`:13479-13505`, the #634 X decision) only at the end of `prepare`, and a
build program never sees it. A build program re-runs when its program, its
compiler, its contract environment (`contract_hash`,
`build_program.cppm:721-727, 971-972`) or a declared input changes; nothing
about a transitive package is in that key.

**Measured (group b).**

```
E1-root-env: SPIKE deps: MCPP_DEP_A_DIR MCPP_DEP_SPIKE_A_DIR MCPP_DEP_A_LINKAGE MCPP_DEP_SPIKE_A_LINKAGE
E1-program-order: SPIKE order a SPIKE order b
E1-metadata-edit: build.mcpp up to date (cached)x3 replayed=yes
E1-strict-metadata-table: exit=0 warnings=0
E1-unknown-package-key-strict: exit=0 mentions=0
```

- **Kind:** gap, general: the capability CMake target walks and
  `cargo metadata` provide.
- **Home:** engine. A plugin cannot enumerate transitive packages: registry
  and git roots are invisible to it, and `dep_dir` is scoped to provisions on
  purpose. #634 A8 deferred this "until a need transitive deploy cannot
  answer"; merging every library's resources into one package at build time
  is such a need, because deploy stages files and does not declare a build
  action over them.
- **Decision (R5).**
  1. The root build program receives `mcpp::graph_file()`
     (`MCPP_GRAPH_FILE`), an absolute path to a JSON document written before
     L3. It holds the per-package objects of `resolution.json`'s `graph`,
     extended with `manifest_dir`, active `features`, target kinds and the
     resolved link form, in topological order (dependencies first, ties in
     discovery order). A file rather than variables, because a graph with
     metadata exceeds what `MAX_ARG_STRLEN` and the Windows environment
     block allow; the precedent for a path a program reads is
     `pack_stage_dir()`. Root only, as `dep_linkage` is: the root holds the
     final facts. The JSON reader `mcpp-plugins/dist/apk.cppm:1027-1135`
     moves into a shared plugin module so rules read the file with one
     parser.
  2. `[package.metadata.<tool>]` is copied verbatim into that package's
     entry as `metadata`. The engine does not interpret it; a consumer
     resolves paths against `manifest_dir`. Older clients already ignore the
     table (X4), so publishing it breaks none of them (R6).
  3. The document's digest joins the contract environment, so a change of any
     graph package's identity, features or metadata re-runs the root program,
     and an edit to a package's sources does not.
  4. The open question (a dependency's program emitting values for its
     dependents) is deferred: manifest metadata serves both stated needs, and
     such a channel first requires dependency programs to run dependencies
     first, which they do not (E1-program-order).
- **X4.** When `[package]` gains a known-key check, `metadata` is in its list;
  the check itself is a separate, compatibility-sensitive change and warns
  before it refuses.
- **Criterion.** The issue's fixture: the root program prints `spike.b` after
  `spike.a` with `b`'s resolved metadata; editing `b`'s metadata changes the
  run, editing `b/src/b.cpp` does not.

### 5.2 E2: Swift sources

**Read.** The inferred glob has no `.swift`, and nothing in mcpp or
mcpp-plugins mentions it. `rules-metal` is the precedent for a language that
lives in a rule package: its feature declares
`device_extensions = [".metal"]` (`mcpp-plugins/mcpp.toml:91-95`), the rule
receives its sources through `mcpp::device_sources()` and locates tools with
`xcrun --sdk <sdk> --find`. The engine already offers what one package needs:
an `action` whose role `object` joins the image link, a `source` action with
companion outputs for a generated header, `link_flag` (LinkGlobal,
`directives.cppm:320-340`), and `include_dir` (PackagePrivate by design,
`:28-34`).

- **Kind:** feature, Apple rows.
- **Home:** official plugin, `rules-swift`. The issue's CI check (one
  package, `@_cdecl` exports, Swift calling C through a bridging header) needs
  no engine change: one whole-module `swiftc -emit-object -target <triple>`
  action, a generated `-Swift.h` through a `source` action and `include_dir`,
  and `link_flag` for the Swift library search paths and
  `-Wl,-rpath,/usr/lib/swift`.
- **What a plugin cannot do.** A Swift `import` of another package's module,
  and a C++ consumer in another package reading a generated header, both need
  an interface directory that dependents can see. Compile-interface directives
  are PackagePrivate on purpose, and dependency programs run dependents first
  (§5.1). That engine capability is named here and not designed; it is E1's
  deferred question in another form.
- **Unmeasured (macos-15).** Whether `ld64.lld` under `-fuse-ld=lld` honours
  the `LC_LINKER_OPTION` autolink entries of swiftc objects
  (`swiftCompatibility*`, `swiftCore`), and the `aarch64-ios-sim` link.

### 5.3 E3: an Android row does not link on a macOS host

**Read, line by line.** `crossTarget` is `--target=<llvm triple>`
(`flags.cppm:553-555`), and an own-sysroot row carries it only in
`link_toolchain_flags` (`:729-754`). The host branch is chosen by
`if constexpr`: Windows at `:1561`, macOS at `:1637`. The macOS branch
assembles `f.ld` from `full_static`, `b_flag`, `apple_cross_ld`, `macos_sdk`,
`version_min`, `-fuse-ld=lld` and the user and extra flags (`:1753-1756`), and
never appends `link_toolchain_flags`; `apple_cross_ld` is set only for an
Apple SDK row (`:1748-1752`); `macos_sdk` falls back to the host SDK
(`:1707-1711`); `version_min` is added whenever the row is not an Apple
cross (`:1683-1685`). An Android link therefore has no `--target`, which
selects `ld64.lld`, and carries the macOS `-isysroot` and
`-mmacosx-version-min`. The issue's cause is exact. Freestanding rows and
graph-supplied C-library rows replace the line after the host branch
(`:1841`, `:1958`) and are not affected.

- **Kind:** defect (R2: the host answers a question that belongs to the
  target), general.
- **Home:** engine.
- **Decision.** The link branch is chosen by host and target object format,
  not by host alone: the Apple SDK branch applies to Mach-O targets, the
  Windows `lld-link` branch to PE targets (MSVC keeps `link.exe`), and every
  other target takes the generic branch that consumes
  `link_toolchain_flags`. The choice is a pure function, for example
  `link_shape(host_os, triple, dialect)`, with a unit test over host by row
  that runs on Linux (macOS host and `x86_64-linux-android` gives the target
  driver; macOS host and `aarch64-ios-sim` gives the Apple SDK; Windows host
  and `x86_64-linux-android` gives the target driver). The generic branch is
  already compiled on every host, since a discarded `if constexpr` branch in
  non-template code is still checked.
- **Criterion.** The unit test above; on macos-15,
  `mcpp build --target x86_64-linux-android` produces a file `file` reports
  as ELF, and mcpp-plugins restores its macOS `dist-apk` steps.

### 5.4 E5: pack strips the program of some rows and nothing the graph built

**Measured (group b).**

```
E5-desktop-status: Packing hostapp v0.1.0 (vendored, stripped)
E5-desktop: bin/hostapp bytes=14280 symtab=0 debug=0
E5-desktop: lib/libdep.so bytes=16592 symtab=1 debug=7
E5-android-status: Packing app v0.1.0 (vendored, stripped)
E5-android: lib/libapp.so bytes=5904 symtab=1 debug=1
E5-android: lib/libc++_shared.so bytes=9091400 symtab=1 debug=6
E5-android: lib/libdep.so bytes=5496 symtab=1 debug=1
```

**Read.** `strip_program` strips the staged program with the `Executable`
shape (`pack.cppm:818-837`) and is called from the PE and ELF paths
(`:1503`, `:2079`); `bundle_libs` copies closure libraries, graph-built ones
included, unchanged; the Android row's dispatch skips the strip step entirely
(§1 item 6). The Mach-O row does not strip on purpose (`:1774`). The NDK's
`llvm-strip` is already resolved by `binutils_tool`
(`registry.cppm:1446-1450`).

- **Kind:** defect (a dropped row, and a status line that reports a decision
  as an outcome, R4), general.
- **Home:** engine; `dist-apk` 0.11.1's own strip becomes unnecessary.
- **Decision.**
  1. The program is stripped on every row that strips, inside
     `run_shared_program` too, per leg with that leg's tools, using the
     `SharedLibrary` shape when the program is a shared object.
  2. Every shared library this graph built is stripped in its staged copy
     with `--strip-unneeded`, which keeps `.dynsym`. `pack::Plan` receives the
     absolute outputs of `plan.linkUnits` whose kind is `SharedLibrary`. The
     comment's rule ("a bundled `.so` is somebody else's file") stays true for
     store and host files; the dh_strip rule it quotes, "a package strips
     what it built", covers these.
  3. A staged copy of the toolchain's runtime (`libc++_shared.so`) is stripped
     with `--strip-unneeded`. The comment's reason, not changing a shared
     payload's bytes, does not apply to a copy; Gradle strips it. `--no-strip`
     covers every case.
  4. The status line reports what was done: a row that stripped nothing does
     not print "stripped".
  5. Build programs read the decision through `mcpp::pack_strip()` and
     `mcpp::pack_debug_symbols_dir()`, in the contract environment beside
     `pack_format()` and `pack_stage_dir()` (`hostprogram.cppm:544-562`), for
     the AAR and Maven libraries `dist-apk` stages itself.
- **Criterion.** The issue's three checks; they fail today per the readings.

### 5.5 E9: a machine-readable pack report, and the profile switches

**Measured (group b).**

```
E9-pack --release: exit=2 [error: unknown option: --release]
E9-pack --dev: exit=2 [error: unknown option: --dev]
E9-pack --message-format json: exit=2 [error: unknown option: --message-format]
E9-human-lines-on-stdout: bytes=273
E9-run-precedence(--profile dev --release): Finished release
E9-build-precedence(--profile dev --release): Finished dev
```

**Read.** `PackOutcome` already holds absolute artifact paths and moves the
outputs another action consumed into `intermediate`
(`pipeline.cppm:34-46, 693-719`); only `shorten_path`'s form is printed.
docs/50 §3 reserves `--format json` for machine output, while `pack`'s
`--format` names the package format; `test --message-format json` is an NDJSON
event stream; `emit build-database` sends its narration to stderr.

- **Kind:** gap and inconsistency, general.
- **Home:** engine; only the engine holds `PackOutcome`.
- **Decision.** `mcpp pack --message-format json` prints one enveloped
  `mcpp.pack` document on stdout and sends every human line to stderr, as
  `emit build-database` does. `data.artifacts[]` holds
  `{path, type: "file" | "directory", format, targets[]}` with absolute paths;
  legs appear as `targets` of the artifact they produced, and intermediate
  outputs are omitted; `data.stage` holds the stage directory, manifest and
  closure. `--protocol-version` advertises the kind. docs/50 §3 gains one
  sentence: a command whose `--format` names its product asks for machine
  output with `--message-format json`. The shape follows the kind: a stream
  for `test`, whose events arrive over time, and one document for `pack`,
  which has one result. `pack` accepts `--release` and `--dev`; the pack
  default stays `release`.
- **X5.** `run` adopts `build`'s precedence (`--profile` over the
  shorthands), one derivation for the three commands (R2).
- **Criterion.** The issue's two checks, and `mcpp run --profile dev
  --release` finishing `dev`.

## 6. #647 E4 and #649 E6 to E8: features, tools and git sources

### 6.1 E4.1: a forward along a build-dependency edge is applied and reported as undeclared

**Read.** The validator looks the key up in `dependencies` and
`devDependencies` (`prepare.cppm:6677-6678`), while `injectForwards` applies
forwards to `buildDependencies` for the root (`:6718-6728`) and for
transitive packages (`:7690-7698`). Active `[feature-deps]` are merged into
`dependencies` before validation (`:6693`, `:7611`), and conditional tables
are folded into the same maps (`:497-506`).

**Measured (group c).**

```
E4.1-build: 2 forward warnings; tool kt built: 1; rc=0
E4.1-strict: error: feature 'kotlin' of 'fw' forwards to dependency 'spike.rules' ... rc=2
E4.1-control-without-feature: tool kt built: 0; rc=0
E4.1b-forward-to-other-row-dep: error: feature 'kotlin' of 'fw' forwards to dependency 'spike.win' ... rc=2
```

- **Kind:** defect (R2), general; X8 is its conditional-row twin.
- **Home:** engine.
- **Decision.** The validator asks the question the forward language
  defines: is the key declared in any dependency table of this manifest,
  on any row and under any feature? `conditionalConfigs` survives the merge
  (`types.cppm:1774`), so the check can read every row. A declared key that
  is not in the graph on this row does nothing and says nothing; a key
  declared nowhere keeps the #243 warning and its `--strict` error.
- **Criterion.** E4.1 and E4.1b build under `--strict` without a warning; a
  forward to a key declared nowhere still exits 2.

### 6.2 E4.2: `[feature-deps]` and a source-less `tools` entry

**Read.** docs/05 (`docs/05-dependencies.md:344-346`, `zh/05:304`) says a
feature-deps entry "may add `tools`" to a dependency declared elsewhere. The
parser tells a dependency table from a namespace table by the presence of a
source key (`toml.cppm:1383-1389`) and refuses an options-only table with a
message that prescribes restating the source (`:1400-1440`). The merge
already implements the intent of #359: `tools` and `features` are unioned,
`host-module` and `reexport` are or-ed, and identity fields are dropped
(`prepare.cppm:6603-6631`). docs/30 states the restated form.

**Measured (group c).**

```
E4.2-sourceless: error: ... must be a string, inline dep table, or nested table ... rc=2
E4.2-restated: SPIKE short=[~/.mcpp/build-cache/v1/tool/spike/installer@.../bin/installer] qualified=[]; rc=0
E4.2-restated-without-feature: SPIKE short=[] qualified=[]; rc=0
E4.2-restated-different-path: (--strict) SPIKE short=[.../installer...]; rc=0
```

- **Kind:** documentation defect; plus a silent drop (a restatement whose
  source differs from the declaration in effect is ignored).
- **Home:** docs; engine for the check.
- **Decision (R6).** The documentation changes, not the parser. A
  source-less table would change the parser's discriminator, and a published
  package that used it would fail to load on every older client, which is the
  hazard the parser's own comment describes (`toml.cppm:1370-1382`).
  Restating the source is already the grammar for conditional dependencies
  (#634 A1) and is what the refusal prescribes. `mergeActiveFeatureDeps`
  refuses a restatement whose path, git source or version differs from the
  declaration in effect on that row, after the conditional fold.
- **Criterion.** E4.2-restated-different-path exits 2 naming both sources;
  docs/05 and its translation say to restate the source.
- **Read, not measured.** Feature-deps merge into `dependencies`
  (`prepare.cppm:6609`), so a feature cannot scope a build-only dependency;
  restating a `[build-dependencies]` key under a feature probably moves it to
  the target axis. This is recorded for the implementation, not decided.

### 6.3 E4.3: the qualified `dep_bin` spelling is missing

**Read.** Tool variables are published at `prepare.cppm:9226-9233`, from
`manifest.package.name`, whose short form is taken after the last dot. For a
manifest with `namespace = "spike"` and `name = "installer"`, the name has no
dot, the qualified and short spellings coincide, and only
`MCPP_DEP_INSTALLER_BIN_*` is emitted (`:9272-9289`). #642 fixed the same
shape for `dep_dir` and `dep_linkage` in `fillDepDirs` (`:6036-6044`, through
`qualified_package_name`, `plan.cppm:435`); the tool site is a second
publication point that fix did not reach. The only test uses the legacy
dotted name (X9).

- **Kind:** defect (R2), general.
- **Home:** engine.
- **Decision.** One helper returns the names under which a provider is
  published to a consumer (qualified, canonical, and bare when bound), and
  both `fillDepDirs` and the tool record use it. `bareBindingsFor`
  (`:5838-5848`) reads `package.name` as well; two packages with one name in
  different namespaces probably collide on the bare variable (read), which
  the helper's test covers.
- **Criterion.** e2e 187 gains a `namespace =` plus `name =` package, and
  `dep_bin("myns.tp", ...)` answers.

### 6.4 E6: a feature-gated tool that depends on the package declaring it

**Read.** The error is raised by the build-cache key walk
(`prepare.cppm:12861-12930`), which runs only for the global cache
(`:12790`). The cycle is in the consumer's graph: `fw-installer` is merged
into `fw.dependencies` (`:6609`), the worklist walks the tool package's own
`[dependencies]` into the consumer's graph (`:7680-7686`), and `spike.fw`
meets the resolved `fw`, recording `fw-installer -> fw`
(`:7266-7270`). The tool itself is built by a sub-build "with its own
resolution" (`:8876-8881`) and succeeds.

**Measured (group c).**

```
E6-issue-fixture: SPIKE short=[.../fw-installer] qualified=[] | error: dependency cycle through package 'fw' while computing its build-cache key; rc=2
E6-tool-runs: fw-installer ran 42
E6-cache-local: rc=0; app runs: exit=0
E6c-tool-dependency-in-consumer: app link line: obj/main.o obj/spike_z/src/z.o obj/spike_fw/src/fw.o; app defines z symbol: 1; rc=0
E6b-tool-activates-declaring-feature: ... tool provisioning nested more than 4 levels deep — this is almost certainly a cycle.; rc=2
```

E6c is the wider defect: when the tool depends on a library `z`, `z.o` is
linked into the application, and the tool's `main.cpp` is compiled in the
consumer's build.

- **Kind:** defect, general.
- **Home:** engine.
- **Decision.** A package that declares no library target has nothing to link
  and contributes nothing to a consumer's target graph: its `[dependencies]`
  and `[build-dependencies]` are not walked into that graph and its sources
  are not compiled there. Its programs come from the tool sub-build, which
  resolves them on its own; that is the model the sub-build's comment already
  states, and the shape of Cargo's artifact dependencies and Bazel's exec
  configuration. A package with no `[targets]` (whose inferred library
  compiles into the consumer) keeps today's behaviour. This removes the cycle
  edge, the link leak and the tool's participation in the consumer's version
  resolution, with no change to any key.
- **Rejected: keying builds on (package, features, toolchain).** The tool
  store's key already carries features, host triple and compiler identity
  (`tool_store.cppm`), and splitting target-graph nodes by feature set would
  contradict one identity per graph with features unioned (the
  one-package-one-version rule).
- **X2 and X3.** Package-edge cycles are checked at resolution, in one place,
  with a message naming the edges, so `--cache=local` and the global cache
  agree. A repeated (package source, tool) in the tool chain is refused at
  its first repetition, naming the feature edge, instead of after four
  nested sub-builds.
- **Criterion.** The issue's fixture exits 0 with the default cache, `dep_bin`
  answers, and the tool prints `fw-installer ran 42`; E6c's application link
  line has no `spike_z`; E6b is refused once, naming the edge.
- **Blast radius to measure.** Before implementation, the manifests of the
  e2e corpus, the examples, mcpp-index and mcpp-plugins are scanned for a
  dependency on a package whose targets are all programs and which is not
  requested for `tools`; any such edge is a consumer that relied on the leak.
- **A modelling note, not a decision.** `tools` builds for the host.
  HuxerUI's installer interface is a program for the Windows target, so the
  channel serves it only when host and target coincide, which holds for a
  Windows host packaging a Windows setup.

### 6.5 E7: a git dependency resolves only the repository's root package

**Read.** A git dependency's root is the clone root
(`prepare.cppm:7376-7455`); its identity is url plus ref (`sourceRefOf`,
`:6736-6744`), so a second key over the same source takes the first key's
identity (#634 A2, `:6800-6829`). Declaring the member alone fails the name
check (`:7536-7549`), and membership is tested against the consumer's
workspace, not the repository's (`:7477-7483`).

**Measured (group c).**

```
E7-both-keys: 1 adoption warning(s) | SPIKE short=[] | Compiling spike.fw v | rc=0
E7-member-only: error: dependency 'spike.fw-installer' resolved to package 'fw' (mismatch with declared name 'fw-installer'); rc=2
E7-member-by-path: SPIKE short=[.../fw-installer...] | rc=0
EDUP-dependencies-and-build-dependencies: tool built: 0 | SPIKE short=[] | rc=0
```

- **Kind:** gap, general to multi-package repositories; X1 and X6 beside it.
- **Home:** engine.
- **Decision (R6).** A git dependency whose key names an identity other than
  the root manifest's is looked up among the repository root's
  `[workspace] members`, at the same commit. The source identity becomes
  (url, commit, member path); the member inherits the root's
  `[workspace.package]`; a member's `path` edge that stays inside the clone
  resolves as the same git source at that commit, so `spike.fw = { path =
  ".." }` inside the member and `spike.fw = { git = ... }` in the application
  name one package. The #634 A2 adoption applies only when no member declares
  the key's identity. Cargo resolves a git dependency by package name within
  the repository in the same way.
- **Rejected: a `subdir` key.** An older client ignores an unknown key and
  would build the root package, which is a silent wrong build, not a
  degradation; and a path is a second statement of identity that can disagree
  with the key.
- **X1.** A second declaration of one dependency by one consumer merges into
  the existing edge additively, as `mergeActiveFeatureDeps` already does,
  instead of returning early.
- **X6.** The banner prints the commit for a git source, or the manifest's
  version.
- **Criterion.** E7-member-only and the both-keys form build the member's tool
  with no identity warning; EDUP builds the tool.

### 6.6 E8: `--features` and a dependency's feature

**Read.** `parse_feature_request` splits on commas and spaces
(`prepare.cppm:650-661`). The unknown-name check applies only when the root
declares `[features]` (`:8531-8548`), and docs/06:40-42 documents that a
package without the table "accepts any request (pure macro usage)"; each name
becomes `-DMCPP_FEATURE_<NAME>` (`:8348`).

**Measured (group c).**

```
E8-dep-feature-no-table: SPIKE short=[] qualified=[] | tool built: 0; rc=0
E8-dep-feature-no-table-macro: -DMCPP_FEATURE_SPIKE_FW_INSTALLER
E8-unknown-no-table: rc=0; macros: -DMCPP_FEATURE_NOTHING_HERE
E8-dep-feature-with-table-strict: error: --features requests 'spike.fw/installer' which [features] does not declare; rc=2
E8-manifest-forward-control: SPIKE short=[.../fw-installer...] | tool built: 1; rc=0
```

- **Kind:** a defect narrower than the issue states (a token that can only be
  a forward becomes a macro), and a gap (Cargo accepts `--features
  dep/feature`).
- **Home:** engine. Today's usage-side answer is the root forward line of
  the control reading.
- **Decision (R2).** A token containing `/` is split with
  `split_feature_forward_token` and applied through `injectForwards` and
  `validateForwards` as a forward of the root, over the table set of §6.1.
  One that names no dependency is warned about, and refused under `--strict`,
  whether or not the root declares `[features]`; it never becomes a macro.
  Plain names keep their documented meaning. `pack`, `run` and `test` inherit
  the behaviour through the shared `overrides.features`.
- **X7.** `mcpp why deps` accepts `--features`, so the graph of a feature
  build is visible where the graph of a default build is.
- **Criterion.** E8-dep-feature-no-table builds the tool and `dep_bin`
  answers, with no `MCPP_FEATURE_SPIKE_FW_INSTALLER`; `--strict --features
  nope/x` exits 2.
- **Open.** Whether a forward, in a manifest or on the command line, may name
  a dependency by the qualified identity after A2 adoption as well as by its
  key.

## 7. #648: an editor that plans in the background

The issue's six asks are three defects of one kind (children that are not
owned or bounded), two contract gaps of the machine interface, and one
ecosystem-data item. The trigger of the field hang is a seventh finding that
the issue does not name, and it is the only one that sends a project to the
network on every build.

### 7.0 T: what started the refresh on openxlings/xlings

The issue establishes that an offline plan of the same project is complete
and byte-identical, so the refresh was not needed. It does not establish which
decision asked for it. Planning openxlings/xlings with `MCPP_OFFLINE=1 -v`
prints the refresh decision per dependency, and the decision for `ftxui` is
the suppressed kind, which `decide_for_dependency` assigns only after it has
decided to refresh (`index_refresh.cppm:237-280`). The same run then resolves
the dependency:

```
T decision ftxui@6.1.9: offline mode
T resolver warning: dependency 'ftxui' resolved to 'compat.ftxui' through the deprecated bare-name search; namespace omission means `mcpplibs` only.
```

The two functions answer one question with two ladders. The refresh decision
looks up the canonical coordinate `(mcpplibs, ftxui)`, which is conclusive
because `mcpplibs` is authoritative, and calls the miss a `DescriptorMiss`
(`index_refresh.cppm:221-249`; the reason is inferred, since the offline
opt-out overwrites it). The resolver, after the same miss, tries
`mcpp::pm::legacy_bare_candidates` for a version selector whose namespace was
omitted and reaches `compat.ftxui` (`prepare.cppm:5113-5160`); its comment
states the reach of that rung: every manifest written before it spells such
dependencies bare. The debounce is 120 seconds (`xlings.cppm:522`), so online
every plan or build of such a project that starts more than two minutes after
the last successful refresh runs a network `xlings update`, editor or not. An
editor that plans on every save meets it several times an hour; it made the
unbounded wait visible.

- **Kind:** defect (R2), general to every manifest with a bare `compat.*`
  dependency; also a usage issue in the xlings manifest, whose warning names
  the spelling.
- **Home:** engine; openxlings/xlings.
- **Decision.** The refresh decision consults `legacy_bare_candidates` under
  the resolver's condition (`spec.isVersion() && spec.namespaceOmitted`)
  before it calls a miss, so both walk one ladder for as long as the
  deprecated rung exists. The rung's warning announces its removal in 2026.9;
  the decision must not depend on that schedule. openxlings/xlings writes
  `compat.ftxui = "6.1.9"`.
- **Criterion.** A fixture whose dependency resolves only through the
  deprecated rung, planned offline with `-v`, prints no suppressed decision
  for it; planned online against a stub xlings that records its argv, it
  starts no `update`.

### 7.1 A1: an offline plan that needs a download has no code

**Read.** Every offline refusal is prose: the toolchain payload
(`prepare.cppm:3452-3475`), `[xlings]` provisioning (`:1541`), a dependency
download (`:5468-5474`), the fetcher's install (`package_fetcher.cppm:1114-1119`)
and a git dependency (`prepare.cppm:7308-7321`). `emit build-database` maps
every planning failure to `MCPP_BUILD_DATABASE_PLAN_FAILED`
(`cmd_build.cppm:375`). An absent index under `--offline` is not refused at
all: `update_index_unguarded` reports success (`xlings.cppm:1923-1926`) and the
resolver later fails on a missing descriptor.

- **Kind:** gap in the machine interface (R4), general.
- **Home:** engine; no client can separate the two failures without matching
  the message, which docs/50 forbids.
- **Decision.** One refusal code, `offline-download-required`, recorded in
  the per-run refusal sink (`src/build/refusal.cppm`, set immediately before
  the `return`) at each site above and at the absent-index case, with the
  subject's kind, name, version and index. `emit build-database` then uses
  `MCPP_OFFLINE_DOWNLOAD_REQUIRED` and one diagnostic naming the subject and
  the command (`mcpp build` without `--offline`, or `mcpp index update`);
  `mcpp why toolchain` reports the token. Planning stops at the first
  refusal, so the diagnostic names the first missing item. The issue asks for
  one per missing item; that would require resolution to continue past a
  refusal, which no other refusal does, and the client's remedy (one online
  build) is the same.
- **Criterion.** The issue's e2e case, and a home without an index planned
  offline yields the code.

### 7.2 A2: children inherit the caller's pipe

**Measured (group d).**

```
A2 reader stdin=pipe:[429071926]
A2 child fd 3 -> pipe:[429071926]
```

A build program started by `emit build-database` holds, as descriptor 3, the
pipe the caller reads. The leak is not specific to `popen`: the owned
launcher's `posix_spawnp` (`process.cppm:648-655`) passes it on too, because
the saved descriptor is created by `dup(1)` without close-on-exec
(`terminal.cppm:76-85`). A build program, hook or xlings process that outlives
mcpp keeps the caller from reading end-of-file.

- **Kind:** defect (R3), general to every caller that reads mcpp through a
  pipe.
- **Home:** engine (`mcpp.platform.terminal`).
- **Decision.** Save with `fcntl(1, F_DUPFD_CLOEXEC, 3)`; on Windows clear
  `HANDLE_FLAG_INHERIT` on the saved handle. The issue's second measure,
  closing every descriptor above 2 in each child, is not available through
  `posix_spawn` on musl, which the Linux release links, so it cannot be the
  rule; the rule is that a descriptor mcpp keeps open across a spawn is
  close-on-exec. The survey found one such descriptor: `FileLock` already
  opens with `O_CLOEXEC` (`fs.cppm:314`), and every other open or stream is
  scoped to a function that starts no child.
- **Criterion.** Reading A2 shows no descriptor naming the reader's pipe; with
  a build program that leaves a sleeping grandchild, a reader of the envelope
  sees end-of-file within one second of mcpp's exit.

### 7.3 A3: xlings runs unbounded and outside mcpp's ownership

**Read.** `run_exec` and the deadline runners give a child its own process
group, register the group with a signal guard that kills it when mcpp is
terminated, and use a job object on Windows (`process.cppm:631-667`,
`unix/bounded_process.cppm:414-470`); this was built after an orphaned ninja
outlived its sandbox. The xlings calls are the exception: `call`,
`update_index_unguarded`, `install_direct` and the bootstrap go through
`popen` or `std::system` (`xlings.cppm:1421, 1620, 1692-1724, 1955`), with no
deadline and no group. The refresh retries three times on a non-zero exit
(`:1950-1967`), which does nothing for a stall.

- **Kind:** defect (R3), general: a terminal build on a black-holed network
  hangs the same way.
- **Home:** engine for the bound; xlings for the missing connection timeout in
  `xlings update`, which the reporter files there.
- **Decision.**
  1. xlings invocations move to the owned launcher, argv-based, with the
     streaming sink the deadline runner already accepts. Ownership and the
     move happen together: a child in its own group without the guard would
     no longer receive a terminal's Ctrl-C, which would be a regression.
  2. The index refresh has a total deadline, `[index] refresh_timeout`
     (seconds). A timed-out refresh is a failed refresh, is not retried, and
     the existing rule then holds: warn, resolve from local data, fail only if
     the local data cannot answer (`prepare.cppm:4465-4472`).
  3. An install has an inactivity deadline. The interface protocol emits a
     heartbeat after five seconds without output
     (`xlings/src/interface.cpp:249-263`), so no line for the deadline means
     the xlings process is wedged, while a slow download that progresses is
     never killed. A stalled connection inside a live xlings is bounded by
     xlings' downloader.
  The default values are measured before they are fixed (§9.3).
- **Criterion.** The issue's two e2e cases with the stub named through
  `[xlings] binary`, and a Ctrl-C in a pseudo-terminal during a stubbed
  install that leaves no stub process.

### 7.4 A4: the envelope omits the network access that happened

docs/50 §2 defines the envelope's `effects` as "what running the command did",
and §4 defines the `--protocol-version` table as what a command may do. `emit
build-database` builds its list from constants and one observation
(`cmd_build.cppm:427-428`), so a run that refreshed the index reports no
`network`.

- **Kind:** defect against docs/50 (R4), general to enveloped commands.
- **Home:** engine.
- **Decision.** A per-run record of observed effects, written where a network
  child is launched (the index refresh, an install, a git remote operation,
  the bootstrap) and read by every enveloped command. An attempt that failed
  or timed out counts: the effect is the access. Under `--offline` those sites
  return before launching, so the record cannot contain `network`.
- **Criterion.** The issue's.

### 7.5 A5: `auto_refresh = false` does not govern three refreshes

**Read.** `mcpp.pm.index_refresh` exists so one function decides whether a
run touches the network for an index; its header records that five
independent derivations had drifted (#315). Three callers bypass it: the
fetcher's pre-install refresh (`package_fetcher.cppm:1127`, through
`ensure_official_package_index_fresh`, which checks neither the policy nor the
module's debounce), the fetcher's refresh before a retry (`:1205`), and the
first sync of a project's custom index (`prepare.cppm:4644-4662`). docs/05
states that `auto_refresh = false` means "never refresh the index
automatically".

- **Kind:** defect (R2), general.
- **Home:** engine.
- **Decision.** The three callers go through `decide_for_miss`, which already
  applies `--offline`, `auto_refresh` and the debounce. The issue's
  alternative, documenting that only `--offline` is complete, would keep the
  second derivation. A first sync is not exempted: the decision module already
  lets `auto_refresh` suppress an absent index, and a failure that names
  `mcpp index update` is the documented behaviour.
- **Criterion.** The issue's, and the custom-index case.

### 7.6 A6: the CN mirror exists, and its artifact does not match the pointer

**Measured (group d).** `publish_mcpp_index.sh` already publishes to
`xlings-res/mcpp-index` on GitHub and GitCode, and the GitCode pointer names
the same version as GitHub's. The asset behind it differs:

```
A6 pointer version=7649883 sha=78535f362b9e...
A6 github  sha=78535f362b9e... bytes=553573   first-entry=... 2026-09-16 04:41 ./
A6 gitcode sha=d89135a348c9... bytes=553557   first-entry=... 2026-09-16 03:00 ./
```

The trees are identical; the entries carry different modification times. The
script ran twice for one version, GitHub's upload replaced its asset with
`--clobber`, GitCode cannot replace an asset, and both pointers were
rewritten with the second digest. The tar invocation fixes names and owners
but not times (`publish_mcpp_index.sh:35`). A CN-routed client today would
reject GitCode's bytes and fall through to GitHub, the connection that hung.

- **Kind:** ecosystem-data defect first; then a small engine default.
- **Home:** mcpp-index (publish script), then engine (`config.cppm`).
- **Decision, in order.**
  1. The artifact becomes byte-reproducible (`--mtime` from the source
     commit, `gzip -n`), and a version whose GitCode asset exists with a
     different digest is published under a new name instead of keeping bytes
     that no pointer describes.
  2. The engine's default `artifact` becomes the region object
     `{"GLOBAL": ..., "CN": ...}` that xlings parses since #377, in the
     configuration template, the in-memory default and the existing
     `.xlings.json` text migration (#269).
- **Criterion.** Two runs of the script on one commit produce one digest, and
  group d's three digests agree; with `mirror = CN`, `mcpp index update`
  fetches no `github.com` URL in xlings' verbose download log and succeeds.

### 7.7 What belongs to the caller

mcppls running offline by default, owning its children through a process
group, bounding its read after mcpp exits and caching the last good document
are the caller's decisions and are correct independently of mcpp. The engine
items make them unnecessary as defences, not wrong.

## 8. Plugin items

- **P1, `dist-apple`.** `defaulted_plist_keys()` holds `UIDeviceFamily`,
  `LSRequiresIPhoneOS` and `NSHighResolutionCapable`
  (`mcpp-plugins/dist/apple.cppm:631-634`), and a project entry can only
  replace a default (`:700`, `:568-586`); a property list has no null to
  express removal. `options::omit_keys` lists defaulted keys to leave out;
  any other key is refused by name, as derived keys already are, and naming a
  key in both `info_plist` and `omit_keys` is refused.
- **P2, `dist-web`.** The page name is fixed in the `mcpp.dist.web.index`
  action (`dist/web.cppm:363-368`) and in `tests/web-consumer/check-web-plan.sh:42`.
  `options::page` names it, default `index.html`, a bare `*.html` name with no
  separator; the plan test gains a named-page leg.
- **`dist-apk`'s strip** (0.11.1) is retired once E5 is released and the
  plugin reads `mcpp::pack_strip()`.
- **`rules-swift`** is §5.2.

## 9. Decisions for the reviewer, and what is still owed

### 9.1 Decisions

| # | decision | alternative not taken | why |
|---|---|---|---|
| D1 | F3a: on ELF, programs over a plan-built C++ shared library take the shared-library contract | refuse the default shape | a mechanism that delivers one runtime exists; the process already requires the shared runtime |
| D2 | F1: a static package in two shared-image closures is refused with the `linkage = "shared"` remedy | link a copy into each image | each image would keep its own state on Mach-O and PE, and ELF would hide it (F1f) |
| D3 | E6: a package with no library target contributes nothing to a consumer's target graph | key builds by (package, features, toolchain) | the edge itself is wrong (E6c); feature-split nodes contradict one identity per graph |
| D4 | E1: a root-only graph file with verbatim metadata | a function family; dependency programs emitting values | one document serves both needs; dependency programs run dependents first |
| D5 | E5: staged toolchain runtime copies are stripped by default | leave them as the payload ships them | the reason for leaving store files alone does not apply to a copy; parity with Gradle |
| D6 | E9: `--message-format json` on `pack` prints one envelope | an NDJSON stream as on `test` | pack has one result; the flag names machine output, the kind names its shape |
| D7 | A3: a total deadline for the refresh and an inactivity deadline for installs, defaults measured | one total deadline for both | a total bound kills slow but progressing toolchain downloads |
| D8 | E4.2: the documentation changes | the parser accepts a source-less table | older clients could not load a package that used it |
| D9 | E7: members selected by identity | a `subdir` key | an older client ignoring `subdir` builds the wrong package silently |
| D10 | E10 and F2: step 1 corrects the record now; any change of default waits for its measurement and its own record | change the defaults here | working builds depend on both defaults |

### 9.2 Measurements owed, by platform

One temporary pull request carries a measurement workflow only, as #635 did.

| platform | fixture | reading | decides |
|---|---|---|---|
| macos-15 | #641 M3 app/fw without `llvm.libcxx`, three legs | exception and `errc` identity across the dylib boundary; `otool -L` | F2, D10 |
| macos-15 | #646 M3b | the dylib link with and without F1 placement | F1 criterion |
| macos-15 | an Android `kind = "app"` | `file` on the linked object | E3 criterion |
| macos-15 | a `@_cdecl` Swift function, C callback | the autolink entries under `ld64.lld`; `aarch64-ios-sim` | E2 |
| windows-2022 | the llvm row default and `-fms-runtime-lib=dll` spelled through flags | `llvm-objdump -p` imports; lld warnings | E10 step 2 |
| windows-2022 | the F2 fixture across an llvm-row DLL | exception identity, CRT state | F2 PE half |

### 9.3 Values owed

The A3 defaults: a cold `xlings update` of the default indexes from a
GitHub-hosted runner and from a CN host, on both mirrors, with the 95th
percentile of ten runs; the refresh deadline is a small multiple of it. The
install inactivity deadline is a multiple of the heartbeat interval.

## 10. Order of work

The engine items group into three batches by what they risk. Within a batch
the items are independent unless stated.

**Batch 1: defects whose fix does not change what a working build does when
it runs** (one release; Linux-testable except the E3 criterion). E5 changes
the bytes of packed libraries and nothing they do.

- R3: A2, A3.
- R2: T, A5, E4.1 with X8, E4.3, E8 with X7, X5.
- R4: A1, A4, F3b, E10 step 1, E5 (including the status line), E9.
- E3 (the pure function and its unit test; the macOS criterion rides the
  temporary pull request).
- X1, X3, X6; E4.2's refusal of a differing restatement; E4.2's
  documentation.

**Batch 2: decisions that change what some builds produce** (one release,
each with its own e2e and a CHANGELOG entry naming the change).

- F3a (D1), then F1 (D2), which share the image-closure computation.
- E6 (D3) with X2, after the blast-radius scan of §6.4.
- E7 (D9).
- E1 (D4).

**Batch 3: measured first, in their own records.** F2 and E10 step 2 (D10).

**Outside the engine.**

- mcpp-index: A6 step 1, before the engine's A6 step 2 (which joins batch 1
  once the digests agree).
- openxlings/xlings: `compat.ftxui`, now; it removes T's trigger for that
  project before any release.
- mcpp-plugins: P1, P2 now; `rules-swift` after its measurement; `dist-apk`'s
  strip retired after E5 is released.

The release chain follows the established order: mcpp release, mirror, index,
then the plugin pins that need the new engine surface (`pack_strip`,
`graph_file`).

## 11. Readings

Each block is quoted from the group's `READING` lines, with the home
directory written as `~` and scratch paths shortened.

**Group a (#646, E10).**

```
F1a M3b exit=0
F1a libfw.so inputs: obj/mcpplibs_fw/src/fw.o
F1a libfw.so undefined x_answer=1 program defines x_answer=1 program exports x_answer=1
F1a run exit=0
F1c libfw.so relinked with -z defs: exit=1 undefined reference to `x_answer'
F1d foreign host dlopen(libfw.so, RTLD_NOW): exit=1 output: dlopen failed: .../bin/libfw.so: undefined symbol: x_answer
F1b root also depends on x: exit=0
F1b x.o linked into: program=1 libfw.so=0
F1b run exit=0 counter=2
F1f libfw.so relinked with x.o: exit=0 libfw defines x_counter=1 run: counter=2 (1 shared counter => 2; split => 1)
F1e root-owned shared image over x: exit=0 libfw.so defines x_answer=1 undefined=0
F3 gcc run: lib-3 exit=0
F3 gcc NEEDED program: [liblib.so] [libm.so.6] [libgcc_s.so.1] [libc.so.6] [ld-linux-x86-64.so.2]
F3 gcc NEEDED liblib.so: [libstdc++.so.6] [libm.so.6] [libgcc_s.so.1] [libc.so.6]
F3 gcc contracts in resolution.json: {"distributable": "self-contained", "intermediate": "self-contained", "shared-library": "toolchain-coupled", "test": "self-contained"}
F3 gcc bin/app status=conflict exported=4571 conflicts=900 defined-in-std.o=1 first=['_ZGIW3std', '_ZGTtNKSt11logic_error4whatEv', ...]
F3 gcc --strict after touch exit=1 mentions=1
F3 llvm run: libc++abi: terminating due to uncaught exception of type std::bad_cast: std::bad_cast exit=134
F3 llvm NEEDED liblib.so: [libc++.so.1] [libc++abi.so.1] [libunwind.so.1] [libm.so.6] [libc.so.6]
F3 llvm bin/app status=conflict exported=145 conflicts=46 defined-in-std.o=1 first=['_Unwind_DeleteException', '_Unwind_ForcedUnwind', ...]
F3 llvm-static-lib run: lib-3 exit=0
F3 llvm-no-println run: libc++abi: terminating due to uncaught exception of type std::bad_cast: std::bad_cast exit=134
F3 llvm-tc run: lib-3 exit=0
F3 llvm-tc bin/app status=conflict exported=84 conflicts=2 defined-in-std.o=1 first=['_ZGIW3std', '_ZGIW3stdW6compat']
F3 llvm-tc --strict after touch exit=1 mentions=1
F3 gcc-tc run: lib-3 exit=0
F3 gcc-tc bin/app status=conflict exported=656 conflicts=8 defined-in-std.o=1 first=['_ZGIW3std', '_ZNSt8__detail31__from_chars_alnum_to_val_tableILb0EE5valueE', ...]
F3 gcc-tc --strict after touch exit=1 mentions=1
F3 std.o global definitions=1 strong=1 sample=['_ZGIW3std']   (gcc and llvm)
E10 compile, no flag: dependent-lib= defines=
E10 link, no flag: -defaultlib:libcmt -defaultlib:oldnames
E10 -fms-runtime-lib=dll: compile --dependent-lib=msvcrt --dependent-lib=oldnames  link -defaultlib:libcmt -defaultlib:oldnames
E10 -fms-runtime-lib=static: compile --dependent-lib=libcmt --dependent-lib=oldnames  link -defaultlib:libcmt -defaultlib:oldnames
```

The lead's independent run of the F3 llvm default shape:

```
F3A build exit=0
libc++abi: terminating due to uncaught exception of type std::bad_cast: std::bad_cast
F3A run exit=134
F3A mcpp-run exit=134
warning: app: 46 symbols in this image are also provided by a library it loads.
```

**Group b (E1, E5, E9).**

```
E1-strict-metadata-table: exit=0 warnings=0
E1-root-env: SPIKE deps: MCPP_DEP_A_DIR MCPP_DEP_SPIKE_A_DIR MCPP_DEP_A_LINKAGE MCPP_DEP_SPIKE_A_LINKAGE
E1-program-order: SPIKE order a SPIKE order b
E1-graph-record: [('mcpplibs.app@0.1.0', ['package','requested_by','root']), ('spike.a@0.1.0', ['link','package','requested_by','root']), ('spike.b@0.2.0', [...])]
E1-metadata-edit: build.mcpp up to date (cached)x3 replayed=yes
E1-unknown-package-key-strict: exit=0 mentions=0
E5-desktop-status: Packing hostapp v0.1.0 (vendored, stripped)
E5-desktop: bin/hostapp bytes=14280 symtab=0 debug=0
E5-desktop: lib/libdep.so bytes=16592 symtab=1 debug=7
E5-android-status: Packing app v0.1.0 (vendored, stripped)
E5-android: lib/libapp.so bytes=5904 symtab=1 debug=1
E5-android: lib/libc++_shared.so bytes=9091400 symtab=1 debug=6
E5-android: lib/libdep.so bytes=5496 symtab=1 debug=1
E9-pack --release: exit=2 [error: unknown option: --release]
E9-pack --dev: exit=2 [error: unknown option: --dev]
E9-pack --message-format json: exit=2 [error: unknown option: --message-format]
E9-human-lines-on-stdout: bytes=273
E9-run-precedence(--profile dev --release): Finished release
E9-build-precedence(--profile dev --release): Finished dev
E2-host: no swiftc on this host; E2 needs macos-15
```

**Group c (E4, E6, E7, E8).**

```
E4.1-build: 2 forward warnings; tool kt built: 1; rc=0
E4.1-strict: error: feature 'kotlin' of 'fw' forwards to dependency 'spike.rules' (as 'spike.rules/kotlin') which is not declared in ; rc=2
E4.1-control-without-feature: tool kt built: 0; rc=0
E4.1b-forward-to-other-row-dep: error: feature 'kotlin' of 'fw' forwards to dependency 'spike.win' (as 'spike.win/x') which is not declared in [dependencies] or [feature-de; rc=2
E4.2-sourceless: error: .../e42/app/mcpp.toml: error: [f; rc=2
E4.2-restated: SPIKE short=[~/.mcpp/build-cache/v1/tool/spike/installer@0.1.0+path.c728166a178c2a19/38e9d63bb5f6464d/bin/installer] qualified=[]; rc=0
E4.2-restated-without-feature: SPIKE short=[] qualified=[]; rc=0
E4.2-restated-different-path: warnings=1 tool-from:  SPIKE short=[~/.mcpp/build-cache/v1/tool/spike/installer@0.1.0+path.c728166a178c2a19/38e9d63bb5f6464d/bin/installer] qualified=[]; rc=0
E6-issue-fixture: SPIKE short=[~/.mcpp/build-cache/v1/tool/spike/fw-installer@0.1.0+path.2c270cde13410c30/46f3d4215af669cd/bin/fw-installer] qualified=[] | error: dependency cycle through package 'fw' while computing its build-cache key; rc=2
E6-tool-runs: fw-installer ran 42
E6-without-feature: ; rc=0
E6-cache-local: ; rc=0; app runs: exit=0
E6-cache-local-link:  obj/main.o obj/spike_fw/src/fw.o
E6b-tool-activates-declaring-feature: 4 host-tool lines | error: building host tool 'fw-installer:fw-installer' failed: (x4) ... tool provisioning nested more than 4 levels deep — this is almost certainly a cycle.; rc=2
E6d-build-dependency-tool-edge:  | error: building host tool 'fw-installer:fw-installer' failed: (x4) ... nested more than 4 levels deep ...; rc=2
E7-both-keys: 1 adoption warning(s) | SPIKE short=[] | Compiling spike.fw v | ; rc=0
E7-member-only: error: dependency 'spike.fw-installer' resolved to package 'fw' (mismatch with declared name 'fw-installer'); rc=2
E7-member-by-path: SPIKE short=[~/.mcpp/build-cache/v1/tool/spike/fw-installer@0.1.0+path.2db8369d57c7c650/47cd46c89a4debd7/bin/fw-installe | ; rc=0
EDUP-dependencies-and-build-dependencies: tool built: 0 | SPIKE short=[] | ; rc=0
E8-dep-feature-no-table: SPIKE short=[] qualified=[] | tool built: 0; rc=0
E8-dep-feature-no-table-macro: -DMCPP_FEATURE_SPIKE_FW_INSTALLER
E8-unknown-no-table: ; rc=0; macros: -DMCPP_FEATURE_NOTHING_HERE
E8-dep-feature-with-table: warning: --features requests 'spike.fw/installer' which [features] does not declare | tool built: 0; rc=0
E8-dep-feature-with-table-strict: error: --features requests 'spike.fw/installer' which [features] does not declare; rc=2
E8-manifest-forward-control: SPIKE short=[~/.mcpp/build-cache/v1/tool/spike/fw-installer@0.1.0+path.5020fbc16166960c/21a7db9275c93a63/bin/f | tool built: 1; rc=0
E6c-tool-dependency-in-consumer: app link line: obj/main.o obj/spike_z/src/z.o obj/spike_fw/src/fw.o; app defines z symbol: 1; rc=0
```

**Group d (#648).**

```
A2 reader stdin=pipe:[429071926]
A2 child fd 0 -> socket:[430147394]
A2 child fd 1 -> pipe:[430149989]
A2 child fd 2 -> pipe:[430149989]
A2 child fd 3 -> pipe:[429071926]
A2 child fd 4 -> .../a2/fds.txt
T exit=0
T decision ftxui@6.1.9: offline mode
T resolver warning: dependency 'ftxui' resolved to 'compat.ftxui' through the deprecated bare-name search; namespace omission means `mcpplibs` only. Write the exact package:
T resolver warning: dependency 'gtest' resolved to 'compat.gtest' through the deprecated bare-name search; namespace omission means `mcpplibs` only. Write the exact package:
A6 pointer version=7649883 sha=78535f362b9e4803e2036a5fd64730751074240001c262f204e07460dda6bc42
A6 github sha=78535f362b9e4803e2036a5fd64730751074240001c262f204e07460dda6bc42 bytes=553573
A6 gitcode sha=d89135a348c99c20baeba211f4bbaf2892b210fce0114ebee954880835c32a60 bytes=553557
A6 github first-entry=drwxr-xr-x 0/0 0 2026-09-16 04:41 ./
A6 gitcode first-entry=drwxr-xr-x 0/0 0 2026-09-16 03:00 ./
```

The T reading planned openxlings/xlings at `59068d6`, the local checkout; its
`[dependencies]` spell `ftxui = "6.1.9"` as the current head `3cd8061` does
(`mcpp.toml:92`). `gtest` resolves through the same rung and prints no
decision, because the refresh loop walks `[dependencies]` only
(`prepare.cppm:4455`) and `gtest` is a dev-dependency. The A6
digests are of the day's artifacts and change with the next publish.

## 12. What each project can do today

- **HuxerUI.** E4.2: restate the source in `[feature-deps]`. E6: keep the
  per-application installer package until D3 is released. E7: pin the tool by
  path within a checkout, or by a published version. E8: keep the root
  forward line. E1: keep hand-written library paths. E5: `dist-apk` 0.11.1
  strips. E3: build Android on Linux. E10: the specification's allowed
  difference stands.
- **mcppls.** Run offline by default, own the process group and bound the
  read after mcpp exits, as planned. Until A2 is released, a child that mcpp
  started in its own group can hold the pipe after the caller's group is
  killed, so the bounded read is the part that cannot be dropped.
- **openxlings/xlings.** Write `compat.ftxui = "6.1.9"`; the refresh every two
  minutes stops at once.

## 13. Self-review

1. **Unmeasured premises that carry decisions.** F2's split on the payload
   origin; E10's step 2; the Mach-O and PE halves of F1; E3's criterion; E2's
   autolink. Each is in §9.2, and no decision that changes an artifact is
   taken on them.
2. **T's reason is inferred.** The offline opt-out overwrites the reason the
   decision computed. The conclusion that the decision wanted a refresh does
   not depend on it (the suppressed reason is assigned only after
   `shouldRefresh` is set); which of `DescriptorMiss` or `IndexAbsent` fired
   does, and the bare-name warning of the same run makes the first the only
   consistent one. The criterion of §7.0 measures it directly.
3. **D3's blast radius is not yet known.** A consumer that depended on a
   program-only package's dependencies leaking into its link would break;
   §6.4 names the scan that precedes implementation.
4. **D1's scope stops at plan-built libraries.** A prebuilt C++ shared library
   from the store is the same class and is left undecided.
5. **A2's second measure was dismissed on musl's `posix_spawn`.** macOS offers
   `POSIX_SPAWN_CLOEXEC_DEFAULT`; the rule chosen is the one that holds on
   every host, and the platform-specific closing can be added where it
   exists without changing the rule.
6. **The readings are from one host.** Group b's Android leg used an NDK
   already installed on this host; groups a to d ran against a home shared by
   other processes, and no reading depends on an index refresh.
7. **Two forks' conclusions were checked against the code by the lead** before
   entering this record: E3's branch assembly, E5's dispatch, E1's program
   order, E4.3's publication site, E8's documentation and the X1 early return;
   and F3a was reproduced independently.
