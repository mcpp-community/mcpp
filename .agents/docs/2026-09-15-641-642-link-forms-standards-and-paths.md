---
subject: triage
status: landed
---

# Link forms, standard levels and a path limit: the asks of #641 and #642, read against the code

**Status:** landed on 2026-09-15 as mcpp 2026.9.15.2 (mcpp-community/mcpp#644),
with `llvm.libcxx` 22.1.8.3 and `openkal-llvm-runtime` 0.9.7 in the index
(mcpplibs/mcpp-index#428). The recommendations D1 to D4 were adopted; D4's
Windows measurement became e2e 698 leg C on the pull request's own windows-2022
run instead of a separate pull request. Where the landed form differs from this
revision, `2026-09-15-641-642-implementation-plan.md` §1.9 records it: the
dependency spelling `namespace.name` is published for `dep_dir` and
`dep_linkage`; on Mach-O a shared library over a graph runtime exports what its
sources mark with default visibility; the M3 remedy follows whether the package
constrains its own form. The body below is revision 1, unchanged.

Engine code was read at `69fae268` (origin/main, mcpp 2026.9.15.1). The
issues measured 2026.9.14.2 and 2026.9.14.3; every probe below reproduces its
item on 2026.9.15.1, so nothing released in between addressed them. A
statement marked *measured* was run on a Linux x86_64 host with the released
mcpp 2026.9.15.1, `llvm@22.1.8`, `gcc@16.1.0` and `llvm.libcxx` 22.1.8.1
(22.1.8.2 differs from it only in `platforms`, CI, README and an example;
the sources are the same). The probes are `2026-09-15-641-642-probes.sh`; each
reading is quoted from its `READING` lines in §6. A statement marked *read*
names a file and line and was not executed. Nothing in this revision ran on
macOS or Windows; §4 names the two premises that need a CI measurement, one of
which (641.3) holds its decision until it is taken.

§0 is the ledger and §1 lists where the measured state differs from the
issues. §2 states why every item is an engine item. §3 takes the items one by
one. §4 collects the decisions that need the reviewer, §5 the order of work,
§6 the readings, §7 the self-review.

## 0. The ledger

| # | real | home | decision (§) | evidence |
|---|---|---|---|---|
| 641.1 | defect, with a twin the issue did not report | engine | the vocabulary fallback matches a pin by the payload it names, not by the normalised family (§3.1) | measured: `llvm@30.0.16248370` on a fresh home; `compiler=emsdk` and `compiler=android-ndk` refused with "no target row pins one" |
| 641.2 | engine gap; both C++-layer packages write a key the engine does not read | engine; two package manifests | a package that provides the C++ layer compiles its implementation units at its own stated standard and its module units at the graph's; the packages state `[package] standard` (§3.2) | measured: the c++20 failure on Linux; per-unit levels emulated, built and run; `cxx_standard` reported as unsupported |
| 641.5 | defect on every format; the fix the issue proposes has a measured semantic cost | engine | refuse before compiling by default; link a private copy of the graph's runtime into a shared image only under the explicit `cxx_runtime = { shared = "self-contained" }` (§3.3, D1) | measured: `ld.lld` refuses the program link on Linux; with a private runtime copy per image, `std::runtime_error` thrown in the library is not caught by type; read: libc++ documents the same split on arm64 Apple |
| F1 | defect beyond the issues | engine, design needed | recorded, not decided (§3.3.5) | measured: a shared package over a static one works on ELF only by binding to the executable; read: Mach-O, PE and Android cannot |
| F2 | probable defect beyond the issues | engine, measurement needed | the payload's default Mach-O shared contract embeds a private hidden libc++ in each dylib and has the same property; measured on macos-15 before any decision (§3.3.3) | inferred from libc++'s comparison rules and M5's mechanism; not measured |
| 642.E1 | gap | engine | `linkage = "static" \| "shared"` in `[targets.<n>]` and its row form states the package's default form; an explicit consumer statement overrides it (§3.4, D2) | read |
| 642.E2 | gap, and a reordering the issue did not see | engine | resolve the dependencies' link forms once, before the root's build program; expose `MCPP_DEP_<NAME>_LINKAGE` and `mcpp::dep_linkage` to that program (§3.5, D3) | read: the form is decided after the scan, which runs after the root's program |
| 641.4 | defect (a missing option) | engine | `mcpp pack --features` reaches every build pass `pack` performs (§3.6) | measured: `error: unknown option: --features` |
| 641.3 | defect; its cause is inferred by the reporter and not isolated | engine | measure on windows-2022 first; mcpp's own opens become extended-length, the tool store's prefix is shortened, and an escaping source is addressed relative to its owner (§3.7, D4) | the issue's log; arithmetic: 181 of the 271 characters precede `obj/` |

## 1. Where the measured state differs from the issues

1. **641.1 is not a property of macOS or of CI caches.** Any home without an
   installed llvm reproduces it, Linux included (M1). The same comparison
   also fails in the opposite direction: a requirement on `emsdk` or
   `android-ndk` finds no pin at all, and the refusal says that no target row
   pins one, which is false for both (M1).
2. **641.2: neither C++-layer package declares a standard.** `llvm.libcxx`
   and `openkal-llvm-runtime` (0.1.0 to 0.1.2) write `[build] cxx_standard =
   "c++23"`, a key the engine has never read (`git log -S cxx_standard` over
   `src/` and `modules/` is empty; `kKnownBuildKeys`, `toml.cppm:2157-2171`).
   At the package's own root the engine prints `[build] has unsupported key
   'cxx_standard' (ignored)` (M6); as a dependency it prints nothing. The
   "declares a higher standard" diagnostic therefore had nothing to report
   even before its store-root gate. The failure is also not specific to Apple
   rows (M2).
3. **641.3: the object address is not the dominant term.** Of the 271
   characters, 181 precede `obj/` (the tool-store scratch and the sub-build's
   `target/<triple>/<fingerprint>/`). An address relative to the owning
   package would still total 262, above the 259 a non-extended Win32 path
   allows. The "tool store layout" half of the issue's expectation is the
   half that matters for the reported case.
4. **641.5 is not specific to Mach-O.** On Linux the shared library links,
   because ELF permits undefined references, and `ld.lld` then refuses the
   program: `non-exported symbol 'std::runtime_error::~runtime_error()' ...
   is referenced by DSO 'bin/libfw.so'` (M3). The runtime package compiles
   with hidden visibility, so no image can resolve against another's copy.
   Linking the package into the library "the way the program does" builds and
   runs, and a standard exception thrown in the library is then not caught by
   type in the program (M5). libc++ states the same outcome for arm64 Apple
   platforms, where type information of a hidden type is unique per image and
   "across linked image boundaries, such types are thus considered different
   types" (`include/typeinfo`, the `NonUniqueARMRTTIBit` notes).
5. **642.E2 is not only an accessor.** The resolved form does not exist when a
   build program runs: link forms are resolved after the module scan
   (`prepare.cppm:11730-11790`), and the scan runs after the root's build
   program (`:10623`, `:10859`). Exposing the value requires the decision to
   move, once, ahead of the program that reads it.
6. **F1.** A dependency's shared library is linked from its own package's
   objects only. The same assembly that loses the C++ runtime in 641.5 loses
   every static dependency of a shared package; ELF hides this by binding the
   library to the executable's copy (M3b).

## 2. Routing

The rule is the one recorded for #634: usage side, project plugin, official
plugin, ecosystem data, and the engine only for a general capability or a
defect. Every item here lands in the engine, for these reasons.

- **641.1, 641.3, 641.4, 641.5** are defects in engine behaviour; the rule
  filters features, not defects.
- **641.2** has no usage path: `-std=` in `[build] cxxflags` is refused
  (`toml.cppm:2203-2208`), and the conditional spelling M2b used to emulate
  per-unit levels is an evasion of that refusal, not a supported spelling.
  The one-standard rule is the engine's. The declaration half belongs to the
  two packages.
- **642.E1** changes the admissible set and the default of `linkage_form`,
  which no plugin can influence.
- **642.E2** is a build program's view of a graph decision, the same kind of
  engine API as `dep_dir`. Two cheaper homes were considered and rejected: the
  project's rule re-reading the manifest is a second derivation of a decision
  with five inputs (edge `linkage`, `dependency_linkage`, profiles, the row
  replacement of #634 A1, admissibility); a pack-time step in `dist-apk`
  deriving the Java constant from the staged closure would put one
  framework's loading convention into a general plugin. A general need
  exists beside the framework's: a consumer of a shared library on PE
  generates `dllimport` definitions from the same answer.

## 3. The items

### 3.1 #641 item 1: a required family resolves to another payload's version

**Code.** `resolve_required_family` (`prepare.cppm:7733-7790`) consults the
installed versions of the family's payload, then the pins of
`known_targets()`, and keeps a pin when `family_name(p->family) == family`
(`:7761`). `android-ndk@30.0.16248370` (`triple.cppm:670`, `:709`) and
`emsdk@6.0.9` (`:844`) normalise to `Family::Llvm` with `payloadName` set
(`registry.cppm:66-96`), so the candidates for `llvm` are 22.1.8,
30.0.16248370 and 6.0.9, and the maximum is the NDK's. For `emsdk` and
`android-ndk` the requested string never equals `llvm`, so no pin matches.
The installed-version source is correct: it asks for
`to_xim_package(*spec).ximName`.

**Measured.** M1: fresh home, `requires = ["mcpp:compiler=llvm"]`:
`toolchain 'llvm@30.0.16248370'`, exit 2. With `compiler=emsdk` and
`compiler=android-ndk`: exit 2, "none is installed, and no target row pins
one".

**Decision.** A pin matches when the payload it names equals the payload the
requirement names (`to_xim_package(pin).ximName ==
to_xim_package(requested).ximName`), which is the comparison the first source
already makes. The function leaves the lambda for the toolchain module, beside
`resolve_version_match`, so that it is tested against the real row table. The
highest matching pin still wins; the comment at `:7727-7731` is right that the
version is a property of the package and not of the row being built.

**Criterion.** A unit test over `known_targets()`: `llvm` resolves to
`llvm@22.1.8`, `emsdk` to `emsdk@6.0.9`, `android-ndk` to
`android-ndk@30.0.16248370`, `gcc` to `gcc@16.1.0`. Reverting the comparison
to the family name turns the first three red with M1's readings. At release,
M1 on a fresh home reads `llvm@22.1.8`.

**Stable.** Every home with the family installed; every fresh-home answer for
`gcc`.

### 3.2 #641 item 2: `llvm.libcxx` cannot serve a c++20 graph

**Code.** A module graph has one standard (`docs/07-workspace.md` §4.2): the
root's level is imposed on every package, and a higher declaration is
reported only for a package that declared one and lies outside the store roots
(`prepare.cppm:10884-10935`). As §1 item 2 states, the two C++-layer packages
declare nothing, and a package cannot place a level on some of its units.
Every Apple fixture builds only because the default level is c++23.

**Measured.** M2: a c++20 root over `llvm.libcxx` fails on Linux with six
`no member named 'resize_and_overwrite'` and one `no template named
'bad_expected_access'`. M2b: the same root, with the package's implementation
units at `-std=c++23` while the graph's flags, the std module and `main.cpp`
stay at `-std=c++20`, builds; a program using `std::unordered_map`,
`std::format` and a caught `std::runtime_error` prints `1-2` and exits 0.

**Why the arrangement is safe for this kind of package and not in general.**
The one-standard rule protects BMIs. An implementation unit neither provides
nor imports a module, so no BMI crosses it, and the std module is still
compiled at the graph's level (M2b). A standard library is, in addition, the
one kind of package built at its own level and consumed at every other:
upstream compiles libc++ at C++23 and supports consumers from C++03 on. An
ordinary library has no such contract: a header whose declarations depend on
`__cplusplus` makes the library's objects and its consumer's disagree without
a diagnostic. Descriptor-generated declarations would also move: the measurement
recorded at `prepare.cppm:10901-10906` found a declared language in 782 of 782
descriptors with an mcpp segment, 756 of them C libraries carrying a
boilerplate "c++23", so a general rule would change the command lines of every
c++20 root.

**Decision.**

1. *Engine.* A package that provides the C++ layer, by the predicate the std
   module adoption already uses (`hosted-standard-library` or
   `mcpp:c++-abi=<impl>`, `prepare.cppm:11043-11052`), and states
   `[package] standard`, compiles its implementation units at exactly that
   level. Units that provide or import a module, `std` and `std.compat`
   included, stay at the graph's level. *Exactly* rather than the maximum of
   the two: a c++26 graph compiles libc++'s sources at c++23, which is
   upstream's configuration. The level is carried on the compile unit and
   spelled once by the dialect layer, so the compile edge, the scan edge and
   the build database (#636) read one value; it reaches the package's cache
   key through the manifest statement. A provider that states nothing is
   compiled as today.
2. *Packages.* `llvm.libcxx` 22.1.8.3 and the next `openkal-llvm-runtime`
   replace `[build] cxx_standard` with `[package] standard = "c++23"`. An older
   engine ignores a dependency's standard and reads it at the package's own
   root as c++23, which its CI builds today, so the two releases may land in
   either order.
3. *Not adopted.* The general form (every package at the maximum of the two
   levels), for the reasons above; and a refusal before compiling, which is
   unnecessary for the packages the engine change serves. For ordinary
   packages the existing degraded diagnostic stands.

**Criterion.** A Linux e2e in the shape of 663: a c++20 root over a path copy
of `llvm.libcxx` that states `standard = "c++23"` builds and runs; the build
database names `-std=c++23` for `libcxx/src/new.cpp` and `-std=c++20` for
`main.cpp` and the std module. The same copy under a c++26 root compiles
`new.cpp` at c++23. Without the statement, every compile line is byte-identical
to 2026.9.15.1's. A path dependency that is not a C++-layer provider and states
c++23 in a c++20 graph keeps the graph's level and the warning. With the engine
change removed, the first leg fails with M2's errors. After the package
revision, the issue's check applies: the Apple fixtures over `llvm.libcxx`
build once more at c++20.

### 3.3 #641 item 5: a shared dependency in a graph whose C++ runtime is a package

#### 3.3.1 What the code does

- A dependency's shared library is linked from that package's objects and its
  direct shared dependencies (`plan.cppm:1793-1808`). The objects of every
  other package are linked into the program (`plan.cppm:1986-1997`). The C++
  runtime package is one of those other packages.
- `dist::resolve` returns `-nostdlib++` for every role when the C++ layer comes
  from the graph (`distribution.cppm:481-485`), before the per-format table
  is consulted, so the shared library receives no C++ runtime at all.
- `llvm.libcxx` compiles with `-fvisibility=hidden` and its visibility
  annotations disabled (its manifest: a second libc++ in one process is "kept
  apart by visibility, not by name"), so its definitions in one image cannot
  satisfy references from another.

#### 3.3.2 What was measured

- M3: `bin/libfw.so` is linked from `fw.m.o fw.o std.o std.compat.o` with
  `-nostdlib++` and carries 19 undefined `std::__1` references; the program's
  link is refused by `ld.lld` (§1 item 4).
- M5: the library relinked with its own copy of the package's objects. Both
  images link and the program runs; a `std::string` returned by the library is
  printed by the program; the library's own `fw_error` is caught by type; a
  `std::runtime_error` thrown in the library is **not** caught by
  `catch (const std::runtime_error&)` and reaches `catch (...)`. Each image
  holds its own hidden type information for libc++'s classes, and the result is
  the one a comparison by address produces.

#### 3.3.3 What libc++ and the engine already say about private copies

libc++ picks its type-information comparison per platform
(`include/typeinfo`, read in the 22.1.8.1 package): by address on ELF and on
x86_64 Apple, by name on COFF, and on arm64 Apple by address for a type whose
type information is hidden, which is "considered different" across linked
images. `llvm.libcxx` compiles its classes hidden. A private copy of the
runtime in each image therefore splits the identity of the library's own
classes (`std::runtime_error`, `std::bad_alloc`, `std::system_error`) on every
row the package serves, and M5 is that split measured on one of them.

`default_contract(SharedLibrary, format)` (`distribution.cppm:214-251`)
couples an ELF shared library to one shared C++ runtime (ToolchainCoupled) and
gives Mach-O and PE shared libraries a private hidden copy (SelfContained;
`-Wl,-load_hidden` on Mach-O, PR #117). On ELF,
`cxx_runtime = { shared = "self-contained" }` selects the private copy
explicitly and is guarded rather than refused (`:443`).

**F2.** The Mach-O default embeds the payload's `libc++.a` hidden in every
dylib. The type information of an exception class defined in libc++'s sources
(`std::runtime_error` has its key function there) is a strong definition
compiled with default visibility, so it is marked unique, and `-load_hidden`
then gives each image its own unique copy: by the rule above, such an exception
thrown in the dylib is not caught by type in the program, on arm64 Apple as
well. This is inferred from libc++'s rules and M5's mechanism, not measured; it
touches builds that work today, so it is measured on macos-15 and decided in
its own record.

#### 3.3.4 Options and decision (D1)

- **A. A private copy in every image by default.** It links on every format
  and splits the identity of the runtime's classes on every row the package
  serves, silently.
- **B. One shared copy.** The provider resolves to its shared form whenever
  the graph holds a C++ shared image, and every C++ image links it. The
  semantics are right; it is not implementable now, because the provider
  compiles hidden unconditionally (its shared form would export nothing) and a
  package cannot condition its flags on its own resolved form. An iOS bundle
  would also carry a libc++ dylib. It is the direction for a later record.
- **C. Refusal by default, private copy on request (recommended).** A shared
  C++ image in a graph whose C++ runtime is a package is refused before
  compiling, naming the shared library, the provider and the two remedies:
  link the dependency static (E1 gives the consumer that statement), or write
  `cxx_runtime = { shared = "self-contained" }`, whose consequence the message
  states. Under that key, every shared image links the provider's objects as
  the program does, on every format. No key is added; the explicit key already
  means "a private runtime in each shared library" for the payload's runtime.
  Every build this changes fails today.
- **Parity with the payload's per-format defaults** was considered: it would
  make the private copy the Mach-O default for the graph's runtime, copying F2
  into a path that nothing depends on yet. The two origins may differ until F2
  is decided; the difference is recorded here rather than inherited.

The decision is C in two steps: the refusal first, then the private copy under
the explicit key.

**Criterion.** Step 1, Linux e2e: a root over `llvm.libcxx` with a
`kind = "shared"` path dependency is refused before compiling, and the message
names the dependency, the provider, `linkage = "static"` and `cxx_runtime`;
with `linkage = "static"` it builds and runs. Step 2, Linux e2e with the
explicit key: `libfw.so` has no undefined `std::__1` reference and the program
runs; macOS, the issue's check under the key: `otool -L` on the dylib names no
libc++ dylib and the program runs. The M5 probe is kept as a reading on both
hosts, for the payload's runtime as well, which is F2's measurement.

#### 3.3.5 F1: a shared package over a static package

M3b: `libfw.so` has an undefined `x_answer`, the program exports it, and the
program runs, because ELF lets the library bind to the executable. A Mach-O
dylib (`-undefined error` by default) and a PE DLL (every import resolved at
link time) cannot link the same manifest; on Android the Java host loads
`lib<fw>.so` before `lib<app>.so` and bionic binds immediately, so the ELF
binding fails there as well (read, not measured). Placement is a design
question beyond these issues: a static package reachable only through a shared
image belongs in that image, and one reachable from both the image and the
program is the "one library, one provider, one form" conflict that
`linkage_form` exists to decide. A separate record should decide it. The C++
runtime of §3.3 is the member of this class that every C++ image reaches, which
is why it is decided by the runtime contract rather than by placement.

### 3.4 #642 E1: a per-row form that a consumer may override

**Code.** `linkage_form` has three layers: the admissible set, the request and
the resolution (`linkage_form.cppm`). `kind = "shared"`, unconditional or per
row (`toml.cppm:2988-3027`, merged at `prepare.cppm:511-518`), is read as a
constraint: `declaredShared` removes the static form from the admissible set
(`linkage_form.cppm:258-265`), and an explicit `linkage = "static"` becomes a
degraded record that `--strict` refuses (`:285-330`). The request defaults to
static, and a package has no statement for a preferred form.

**Decision.**

- A new key, `linkage = "static" | "shared"`, in `[targets.<n>]` and in
  `[target.<sel>.targets.<n>]`, for library targets only. It states the
  package's default for the question the consumer's `linkage` answers; `kind`
  continues to state a constraint. Within one table the two keys exclude each
  other, since a constraint has no default. Across tables the last matching
  statement replaces the earlier one, as `kind` rows already do (#634 A1).
- Precedence: the root's edge `linkage`, then the root's `[build]
  dependency_linkage` when written, then the package's `linkage`, then static.
  An explicit statement that differs from the package's default is honoured and
  reported by one information line naming both statements; it is not a
  degradation, so `--strict` accepts it. The resolution record's reason is
  `requested` or `package-default`.
- The spelling (D2). `linkage_form` names the concept after the key the
  consumer writes, "so one concept has one word in the manifest, in the code
  and in the diagnostics"; `kind = { default = "shared" }` would give `kind`
  two grammars. The compatibility of the two spellings is the same: engines
  from 2026.9.14.2 to 2026.9.15.1 refuse a row table without a string `kind`,
  older engines skip row tables, and a package using either states its engine
  floor.

**Criterion.** A Linux e2e in the shape of 678, a package with
`[targets.fw] linkage = "shared"`: a silent consumer gets `bin/libfw.so`,
`NEEDED libfw.so` and the reason `package-default`; `linkage = "static"` gives
no `libfw.so`, the information line, and exit 0 under `--strict`;
`[build] dependency_linkage = "static"` gives the static form. The row form on
the Android row, in the shape of 667: the staged closure holds `lib<fw>.so` for
the silent consumer and not for the explicit one. Negative: a package with
`kind = "shared"` keeps today's refusal and `--strict` failure.

### 3.5 #642 E2: a build program reads a dependency's resolved form

**Code.** The dependency programs run in discovery order (`prepare.cppm:9509`),
the root's program runs next (`:10623`), and the module scan follows
(`:10859`). Link forms are resolved after the scan because `hasSources` reads
the scanned units (`:11730-11743`); the answer is applied there (the library
targets are flipped, the diagnostics emitted) and recorded (`:13282`). Of the
inputs, the requests come from the root manifest, which no directive changes;
the target facts are known early; a dependency's facts are final once its own
program has run, since that program can add the `-L` that decides
`carriesForeignLinkInputs`, and its sources are known by then. For the root's
program every dependency program has run. For a dependency's program, the
packages discovered after it have not.

**Decision.**

1. The resolution is split into a computation and its application. The
   computation runs once, after the dependency programs and before the root's
   program; `hasSources` comes from the scanner's own per-package source
   enumeration, factored out of `scan_packages` so that the scan and the
   resolution read one function. The application stays where it is and reads
   the stored answers.
2. The root's program, and every rule module running inside it, receives
   `MCPP_DEP_<NAME>_LINKAGE=static|shared` for each dependency with a library
   form, under the names and sanitiser of `MCPP_DEP_<NAME>_DIR`, and the bundled
   module gains `mcpp::dep_linkage(name)`, shaped like `dep_dir` and empty for
   an unknown dependency or one without a library form.
3. A dependency's program does not receive it (D3). Only the root decides link
   forms, and a dependency's program runs before packages whose programs supply
   inputs to the answer. The helper returns empty there, and the documentation
   says why.
4. The variables join the contract hash, so a changed form re-runs the root's
   program. Every root program with dependencies re-runs once after the
   upgrade.

**Criterion.** The root program prints `dep_linkage("fw")` under the E1
consumers: `shared` and `static`, matching the presence of `bin/libfw.so`.
Switching the consumer's `linkage` re-runs the program without an edit to
`build.mcpp`. A dependency whose only sources are generated by its own program
and which is requested shared reads `shared`, which a computation placed before
the dependency programs would get wrong. A dependency without sources requested
shared reads `static`, the admissible answer, not the request.

### 3.6 #641 item 4: `mcpp pack --features`

**Code.** `build`, `run` and `test` register `--features`
(`cli.cppm:382`, `:448`, `:512`); `pack` (`:584-627`) does not. The pack
pipeline constructs `BuildOverrides` at three sites
(`pack/pipeline.cppm:119`, `:216`; `pack/library_pipeline.cppm:133`), and the
dispatched format's second pass reuses the one at `:216`.

**Measured.** M4: `error: unknown option: --features`, exit 2.

**Decision.** Register the option; carry it in `pack::Options`; set
`features` at each of the three sites.

**Criterion.** The issue's e2e: a root with a feature-gated `tools = [...]`
dependency and a build program that requires `dep_bin` under `--format <f>`;
`mcpp pack --format <f> --features <gate>` succeeds, and `mcpp build` without
the feature builds no tool. One assertion per construction site (the Android
extra legs and a library pack included), so that the denominator is three.

### 3.7 #641 item 3: a tool build's paths exceed the Windows limit

**Code.** `owner_of` (`plan.cppm:1363`) assigns a source to the longest
containing package root; the object address is
`obj/<declaring package>/<mirror of the path relative to the declaring
package>` (`:1453-1474`), with `..` spelled `__up` (`:1422`). A source under a
dependency's root that the declaring package's program adds is mirrored with
one `__up` per level between the two roots. `clang-scan-deps` writes the `.ddi`
itself (`-o $out`, `ninja_backend.cppm:1495`); `mcpp dyndep` reads it with
`std::ifstream` on the relative path (`modules/dyndep/src/dyndep.cppm:337`).
No extended-length path helper exists in the tree, and `mcpp.exe` carries no
application manifest. The tool store's entry is
`<cache>/tool/<index>/<pkg>@<ver>/<key>/` (`tool_store.cppm:123`), and the
sub-build runs in a `build-<hash>` scratch beneath it with its own
`target/<triple>/<fingerprint>/`.

**Arithmetic, from the issue's log.** Scratch 133, target directory 48, object
address 90: 271 characters. mcpp owns about 155 of the first 181. The address
relative to the owning package (`obj/mcpplibs_installer/<dependency>/platform/
windows/windows_installer.cpp.ddi`) is 81 characters, for 262 in total.

**Not isolated.** The log shows that `mcpp dyndep` cannot open the file. It
does not show which of ninja 1.12.1, the compiler, `lld-link` and mcpp fail at
which length, nor whether the runner enables `LongPathsEnabled`; the scan's
success is consistent with LLVM widening long paths itself, and that is an
inference.

**Decision (D4).** Measure first: a temporary pull request on windows-2022
with a synthetic tool build whose paths cross 260 characters by a fixed margin,
reading each component and the registry value. The three changes below are
made whatever the result, and the measurement decides which of them is the fix
and which is margin.

1. Every path mcpp itself opens under a build directory goes through one
   platform helper that makes it absolute and extended-length on Windows. The
   call sites are enumerated by searching for file opens on build-tree paths,
   not sampled.
2. The tool store's sub-build runs directly in a short key-named scratch, and
   its output directory is that scratch rather than a nested
   `target/<triple>/<fingerprint>/`; the entry metadata already records the
   package, version and source. The prefix is the dominant term and the one no
   other tool can compensate for.
3. A source outside its declaring package is addressed relative to the package
   that owns it, and a source outside every package by a hashed directory, so
   an address no longer grows with the distance between two roots, on every
   platform.

If ninja fails too, (2) is the fix and (1) is defence; if only mcpp fails, (1)
is the fix and (2) buys margin.

**Criterion.** The issue's Windows e2e (a tool package whose program adds a
source under its dependency's root, built through `tools = [...]`). A unit test
that an escaping source's address contains no `__up` and does not lengthen when
the declaring package moves one directory deeper. The scratch path of the
issue's tool, measured from the home, stated as a number in the test.

## 4. Decisions for the reviewer

- **D1 (641.5).** Refusal by default with the private copy under the existing
  explicit key (C); the private copy by default (A); or parity with the
  payload's per-format defaults, which makes the private copy the Mach-O
  default. Recommended: C, with one shared copy (B) left to a later record and
  F2 measured before the payload's Mach-O default is touched.
- **D2 (E1).** The package's default is spelled `linkage`, beside `kind`, or
  `kind = { default = ... }`. Recommended: `linkage`.
- **D3 (E2).** The resolved form is exposed to the root's build program only,
  or also to dependency programs after reordering them topologically.
  Recommended: the root only; reordering the dependency programs changes the
  order every existing graph runs its programs in, for a reader nobody has
  asked for.
- **D4 (641.3).** Measure on windows-2022 before implementing, or implement
  the three changes and measure afterwards. Recommended: measure first; the
  measurement is one temporary pull request.

Two premises need CI rather than this host: the Windows component readings of
§3.7, and M5 on macos-15 for both runtime origins (§3.3.3, F2).

## 5. Order of work

One engine release, in this order; each step is independently testable.

1. 641.1 with its twin, and 641.4 (small, no interaction).
2. 641.5 step 1, the refusal.
3. 642.E1, then 642.E2, which reads E1's resolution.
4. 641.2 in the engine; `llvm.libcxx` 22.1.8.3 and the `openkal-llvm-runtime`
   patch with `[package] standard`, in either order, each followed by its
   index entry.
5. 641.5 step 2, the private copy under the explicit key.
6. 641.3, after the Windows measurement.

The framework adopts E1 and E2 after the release. F1 and F2 wait for their own
records; F2's macos-15 measurement can run in the same temporary pull request
as §3.7's Windows measurement.

## 6. Readings

All from `2026-09-15-641-642-probes.sh`, Linux x86_64, mcpp 2026.9.15.1.

| id | item | reading |
|---|---|---|
| M1 | 641.1 | `exit=2 spec=toolchain 'llvm@30.0.16248370'` (fresh home) |
| M1 | 641.1 twin | `emsdk exit=2 none is installed, and no target row pins one`; the same for `android-ndk` |
| M2 | 641.2 | `exit=1 resize_and_overwrite errors=6 bad_expected_access errors=1` (root at c++20) |
| M2b | 641.2 | `exit=0`; graph `-std=c++20`; `new.cpp` unit `-std=c++23`; `run: out=1-2 exit=0` |
| M6 | 641.2 | `[build] has unsupported key 'cxx_standard' (ignored)` |
| M3 | 641.5 | `exit=1 non-exported symbol 'std::runtime_error::~runtime_error()' in '.../stdlib_stdexcept.o' is referenced by DSO 'bin/libfw.so'`; libfw.so inputs `fw.m.o fw.o std.o std.compat.o`, `unit_ldflags = -nostdlib++ ...`; 19 undefined `std::__1` references |
| M5 | 641.5 | both links `exit=0`; `run exit=0 output: fw-3 \| runtime_error NOT matched by type \| fw_error caught by type` |
| M3b | F1 | `exit=0`; libfw.so undefined `x_answer`=1, program exports `x_answer`=1; `run exit=0` |
| M4 | 641.4 | `pack exit=2 error: unknown option: --features` |

## 7. Self-review

- **What is read and what is measured.** E1 and E2 are read. The timing claim
  of E2 rests on line order in one function; the implementation must confirm
  that nothing between the root's program and the scan changes a dependency's
  facts, and the criterion with a generated-only dependency is the test of the
  placement.
- **M2b measures an arrangement, not an implementation.** It placed `-std=c++23`
  through a conditional table the `-std=` refusal does not cover. It shows
  that the levels can differ; it does not show that the engine's scan edge and
  build database will agree with the compile edge, which is what the criterion
  of §3.2 asks.
- **The first draft of 641.5 recommended parity with the payload's per-format
  defaults**, and left arm64 Apple as possibly exempt. Reading libc++'s
  comparison rules removed the exemption: hidden type information is unique
  per image there too. The recommendation changed to a refusal by default, and
  the same reading produced F2, which concerns builds that work today and is
  therefore not decided from a reading.
- **641.3's decision is staged on a measurement** because the arithmetic
  already refuted the obvious fix (a shorter object address alone totals 262).
- **Scope held.** F1 and F2 are recorded and not designed. The general per-unit
  standard (every package) was considered and not adopted; its hazard is stated
  so that a later request meets the reason rather than the gap.
- **Stability.** 641.1 changes answers only where they were wrong; 641.2 is
  byte-identical until a provider states a level; 641.5 changes only builds
  that fail today; E1 adds a key; E2 re-runs each root program once; 641.4 adds
  an option; 641.3 moves object and tool-store paths, which costs one rebuild.
