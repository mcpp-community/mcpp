// mcpp.manifest:types — shared manifest data model.
//
// Everything both descriptor formats (mcpp.toml, xpkg .lua) synthesize
// into, plus format-agnostic helpers. No parsing lives here.

export module mcpp.manifest.types;

import std;
import mcpp.pm.dep_spec;     // M5.x pm/ subsystem refactor: DependencySpec lives here
import mcpp.pm.compat;       // Legacy dependency-key compatibility helpers
import mcpp.pm.index_spec;   // IndexSpec for [indices] section
import mcpp.platform;

export namespace mcpp::manifest {


// PR-R1 transitional: the dependency data model has moved into
// `mcpp.pm.dep_spec`. The aliases below keep `mcpp::manifest::DependencySpec`
// and `mcpp::manifest::kDefaultNamespace` available as before so existing
// callers (`cli.cppm`, `fetcher.cppm`, ...) compile unchanged. A later
// refactor PR will migrate call sites to reference `mcpp::pm::` directly
// and these aliases can disappear.
using DependencySpec = mcpp::pm::DependencySpec;
inline constexpr auto kDefaultNamespace = mcpp::pm::kDefaultNamespace;
inline constexpr auto kCompatNamespace  = mcpp::pm::kCompatNamespace;

struct CppStandardConfig {
    std::string                 canonical = "c++23";
    std::string                 flag = "-std=c++23";
    int                         level = 23;
    bool                        gnuDialect = false;
    // standard = "c++fly": latest level + all experimental gates the
    // resolved toolchain supports (toolchain/cppfly.cppm owns the mapping).
    bool                        experimental = false;
};

struct Package {
    std::string                 name;
    std::string                 namespace_;    // xpkg V1 namespace field (0.0.6+); empty = infer from name
    std::string                 version;
    std::string                 standard   = "c++23";   // C++ standard (M5.0: moved from [language])
    // DID THE AUTHOR WRITE `standard`, or is this the default?
    //
    // The two are the same bytes in `standard` and they need opposite
    // treatment. `[workspace.package] standard` means "use the workspace value
    // where the member did not say", which is unanswerable without this bit:
    // inheriting over a member that deliberately pinned c++23 and inheriting
    // into one that said nothing are the same operation otherwise.
    //
    // The cpp20 design doc's §9-Q3 recorded this as the precondition for a
    // dependency floor check and declined to add it while nothing consumed it.
    // Workspace inheritance is that consumer, and the floor check is now its
    // second one.
    //
    // MUST BE SET BY BOTH PARSE PATHS. `toml.cppm` reads an author's
    // `mcpp.toml`; `xpkg.cppm` synthesises a manifest from an index descriptor
    // and writes `"c++23"` unconditionally before reading `language`. Setting it
    // in one place only would be the same decision derived twice, which is what
    // §9-Q3 warned about.
    //
    // NOTE FOR THE FLOOR CHECK: an index descriptor declaring `language` sets
    // this too, and measured over the local registry every descriptor with an
    // mcpp segment does (782 of 782, 756 of them C libraries carrying a
    // boilerplate "c++23"). Declaredness alone therefore does NOT mean "the
    // author asked for it" outside author-owned manifests — see the scope gate
    // in prepare.cppm.
    bool                        standardDeclared = false;
    std::string                 description;
    std::string                 license;
    std::vector<std::string>    authors;
    std::string                 repo;
    std::vector<std::string>    platforms;     // declared supported platforms (CI matrix hint)
    // Accelerator backends this package supports, declared in the same spirit
    // as `platforms`: a statement of intent and a CI-matrix hint, not a gate.
    //
    // The pair is deliberate and the two must not be confused. `accelerators`
    // is what a SOURCE package says it can be built for; `accel` on a built
    // artifact is what that binary actually carries. A declaration is written
    // by hand and can be aspirational; the artifact field is measured from the
    // build and is what a consumer is refused against.
    std::vector<std::string>    accelerators;
    // Resolution source carried into machine-readable runtime provenance.
    // Version dependencies use `index+<name>@<snapshot>`; path/git packages
    // use their corresponding immutable-or-local source spelling.  Parsing a
    // standalone manifest leaves this empty; prepare_build fills it once the
    // resolver knows which index/source actually answered.
    std::string                 sourceProvenance;
};

struct Language {
    std::string                 standard   = "c++23";
    bool                        modules    = true;
    bool                        importStd = true;
};

// Author-asserted scan result for one source glob (scan_overrides).
// Files matched by the glob bypass the M1 text scan entirely; the declared
// (provides, imports) enter the module graph directly. Sound because the
// declaration is verified against the compiler's own P1689 (.ddi) output
// at build time — assertion + verification instead of computation.
// Design: .agents/docs/2026-07-08-scanner-backend-abstraction-design.md §3-pre.
struct ScanOverride {
    std::vector<std::string> provides;   // module logical names the file exports
    std::vector<std::string> imports;    // module logical names the file imports
};

struct Modules {
    std::vector<std::string>    sources;        // glob patterns
    std::vector<std::string>    exports_;       // declared module names (optional)
    bool                        strict = false;
    // glob → declared scan result; every glob must match ≥1 source file.
    std::map<std::string, ScanOverride> scanOverrides;
};

struct Target {
    std::string                 name;
    enum Kind { Library, Binary, SharedLibrary, TestBinary } kind;
    std::string                 main;           // for binary / test
    std::string                 soname;         // ABI name for shared libraries, e.g. libfoo.so.1
    // Per-target compile flags. SCOPE: applied ONLY to this target's exclusive
    // entry source (its `main`) — never to shared module/impl objects, which are
    // compiled once and linked into every target (the build's compile-once model;
    // see src/build/plan.cppm). `defines` are sugar desugared to `-D<x>` at plan
    // time and applied to both the C and C++ entry compile. Use these for flags
    // that are private to a binary's own entry (e.g. `-DBUILD_SERVER=1`,
    // `-Wno-deprecated`); for divergence that must reach shared code, use a
    // workspace member or a [features] knob instead.
    std::vector<std::string>    cflags;
    std::vector<std::string>    cxxflags;
    std::vector<std::string>    defines;
    // Build gate: this target is emitted ONLY when every listed feature is
    // active in the current build (otherwise it is silently skipped). Gate
    // only — it does not activate features (use --features / [features].default).
    std::vector<std::string>    requiredFeatures;
};

// `DependencySpec` and `kDefaultNamespace` have moved to mcpp.pm.dep_spec.
// Aliases at the top of this file keep `mcpp::manifest::DependencySpec`
// resolvable for unchanged call sites.

// `[toolchain]` section per docs/21-toolchain-and-tools.md
//   linux   = "gcc@15.1.0"
//   macos   = "llvm@20"
//   windows = "msvc@system"
//   default = "gcc@15.1.0"   (used when current platform isn't listed)
struct Toolchain {
    std::map<std::string, std::string> byPlatform;   // platform -> "pkg@ver"

    // Returns the toolchain spec for a platform, falling back to "default".
    std::optional<std::string> for_platform(std::string_view platform) const {
        if (auto it = byPlatform.find(std::string(platform)); it != byPlatform.end()) {
            return it->second;
        }
        if (auto it = byPlatform.find("default"); it != byPlatform.end()) {
            return it->second;
        }
        return std::nullopt;
    }
};

// `[build] flags = [{ glob = "...", ... }]` — per-glob compile flags (G4).
// A VECTOR, not a map: declaration order is application order (a later
// entry's flags land later on the command line, so GNU "last flag wins"
// gives it precedence over an earlier, broader glob). Private build flags —
// they never enter usage requirements / never propagate to consumers.
struct GlobFlags {
    std::string              glob;      // matched against package-root-relative paths
    std::vector<std::string> cflags;    // C units (.c/.m)
    std::vector<std::string> cxxflags;  // C++ units (.cpp/.cc/.cxx/.cppm)
    std::vector<std::string> asmflags;  // assembly units (.S/.s via cc, .asm via nasm)
    std::vector<std::string> defines;   // desugars to -D on every matched unit kind
    // #253: non-empty when this entry came from `features.<name>.flags` and was
    // folded in at feature activation (prepare_build). Diagnostic context only
    // (names the owning feature in the zero-hit warning); deliberately NOT part
    // of the fingerprint — the active feature set is already fingerprinted via
    // the -DMCPP_FEATURE_* cflags.
    std::string              featureOrigin;
};

// The additive build inputs — the ONLY things any conditional axis may
// contribute (#258).
//
// mcpp has two conditional axes: `[target.'cfg(...)']` (platform) and
// `[features.<name>]`. Each used to hand-pick which build fields it could
// carry, and they picked DIFFERENT subsets — cfg took cflags/cxxflags/
// ldflags/sources, features took sources/defines/flags. "Which build inputs
// may be contributed conditionally" was being decided twice, differently,
// which is exactly the architectural debt the batch ledger warns about.
//
// Membership here is the answer, and it is a type rather than a hand-kept
// list, so it cannot drift from the struct it describes. Two properties
// qualify a field:
//
//   1. APPENDING is its merge semantics. Scalars (linkage, cStandard, the
//      profile knobs) would need last-wins override semantics — a different
//      operation, and a separate design.
//   2. It is consumed AFTER the conditional merge point. `target` and
//      `linkage` are consumed BEFORE it — indeed `target` SELECTS the triple
//      the cfg predicate is evaluated against, so conditioning it is
//      circular by construction.
//
// Two deliberate non-members worth naming, because their exclusion is about
// category rather than mechanics:
//   • generatedFiles is a side-effecting materialization ACTION, not an
//     input (and root/dep materialize on opposite sides of the merge — see
//     the design doc; that ordering bug is tracked separately).
//   • featureDefines are INTERFACE contributions that propagate along Public
//     edges, not private build inputs. Per-glob `defines` (GlobFlags::defines
//     below) are private and per-TU, so those DO belong here.
struct BuildInputs {
    std::vector<std::string>           sources;        // glob patterns
    std::vector<std::string>           cflags;
    std::vector<std::string>           cxxflags;
    std::vector<std::string>           ldflags;
    // #296: package-level preprocessor macros. Unlike per-target `defines`
    // (which only reach the binary's own entry TU), these reach EVERY TU in
    // the package — module interface units included — so they participate in
    // the P1689 module scan, which is what makes a macro-guarded `import`
    // resolvable. Desugared to `-D<x>` on both the C and C++ channels
    // (fold_build_defines_into_flags in prepare.cppm) after the conditional
    // merge and before the manifest is snapshotted into packages[] /
    // fingerprinted. A member HERE rather than on BuildConfig so the cfg axis
    // can carry it: `[target.'cfg(windows)'.build] defines = [...]` must work,
    // and membership of this type is what guarantees it (see above).
    std::vector<std::string>           defines;
    std::vector<GlobFlags>             globFlags;      // flags = [...] (ordered)
    std::vector<std::filesystem::path> includeDirs;    // relative to package root
    // #249: emitted as -idirafter (searched after the toolchain's system dirs)
    std::vector<std::filesystem::path> includeDirsAfter;
    // ⭐⭐ WHICH OF `includeDirs` A CONSUMER MUST NOT RECEIVE.
    //
    // `publicUsage` has always taken `privateBuild`'s include directories
    // ENTIRE, so a package is built from exactly the set it publishes. For
    // almost every package those are the same set. For one that vendors a
    // library with an internal header overlay they are not, and the difference
    // reaches every consumer.
    //
    // `mcpplibs/openkal-musl` states the case in its own source
    // (`port/include/features.h`), having found it three times:
    //
    //   ⓘ THIS IS THE SECOND-BEST REMEDY. The first would be for a package to
    //   distinguish the directories it is BUILT FROM from the directories it
    //   PUBLISHES. Measured 2026-08-22: mcpp cannot express it.
    //
    // musl's build reaches its own declarations through `src/include`, whose
    // headers define `hidden`, `weak` and `weak_alias` — names that mean
    // something only to musl's own sources. Publishing that directory hands
    // those macros to every consumer, and which consumer breaks on which name
    // was discovered one at a time: a C++ one on `restrict`, then on linkage;
    // a C one (compiler-rt) on `weak`, which it writes itself.
    //
    // ⚠️ A SUBSET OF `includeDirs`, NOT A SECOND LIST, AND THE REASON IS ORDER.
    // The relative order of the two kinds is load-bearing: moving musl's
    // internal directories after the public ones makes musl's OWN build find
    // the public `<features.h>` first and fail with `unknown type name hidden`
    // (measured, same file). Two arrays in TOML cannot express one order, so
    // `includeDirs` stays the single ordered list and this one says which of
    // its entries stop at the package boundary. An entry here that is not in
    // `includeDirs` withholds nothing, and is reported as such rather than
    // passing in silence.
    std::vector<std::filesystem::path> privateIncludeDirs;
    // What the `std` module source of a package that IS a standard library
    // needs on its command line.
    //
    // A MEMBER OF THIS TYPE AND NOT OF THE MANIFEST, for the same reason
    // `defines` is: membership here is what makes the cfg axis carry it. A
    // package supplying one C++ runtime over SEVERAL C libraries needs
    // different flags per C library — `-D_GNU_SOURCE` is right for musl and
    // glibc and wrong for picolibc — and while this lived beside the package's
    // identity there was no spelling for that difference.
    std::vector<std::string>           stdModuleFlags;
};

// The single additive merge. Every conditional axis folds through this, so
// "how does a contribution combine with the base" has one answer: append, in
// declaration order, which gives later entries GNU last-wins precedence.
inline void append(BuildInputs& dst, const BuildInputs& src) {
    dst.sources.insert(dst.sources.end(), src.sources.begin(), src.sources.end());
    dst.cflags.insert(dst.cflags.end(), src.cflags.begin(), src.cflags.end());
    dst.cxxflags.insert(dst.cxxflags.end(), src.cxxflags.begin(), src.cxxflags.end());
    dst.ldflags.insert(dst.ldflags.end(), src.ldflags.begin(), src.ldflags.end());
    dst.defines.insert(dst.defines.end(), src.defines.begin(), src.defines.end());
    dst.globFlags.insert(dst.globFlags.end(),
                         src.globFlags.begin(), src.globFlags.end());
    dst.includeDirs.insert(dst.includeDirs.end(),
                           src.includeDirs.begin(), src.includeDirs.end());
    dst.includeDirsAfter.insert(dst.includeDirsAfter.end(),
                                src.includeDirsAfter.begin(),
                                src.includeDirsAfter.end());
    dst.privateIncludeDirs.insert(dst.privateIncludeDirs.end(),
                                  src.privateIncludeDirs.begin(),
                                  src.privateIncludeDirs.end());
    dst.stdModuleFlags.insert(dst.stdModuleFlags.end(),
                              src.stdModuleFlags.begin(),
                              src.stdModuleFlags.end());
}

// A build-graph node declared by a build program (`mcpp:action=`).
//
// The architectural point (see
// .agents/docs/2026-08-05-build-mcpp-extensibility-architecture.md §3.1):
// build.mcpp answers "what does this build look like" — CONFIGURATION — and is
// a bad place to do WORK. Generating sources, linting, signing and packaging
// are work: they want to be incremental, parallel and attributable, which a
// once-per-prepare program can never be. So instead of DOING the work, the
// program DECLARES it, and it becomes an edge in the build graph.
//
// One primitive, four wirings. `role` is not four mechanisms — it is where
// the same edge's outputs attach:
//
//   Source   — outputs join the compile set (protoc, a transpiler)
//   Check    — outputs are a stamp; nothing consumes them (clang-tidy, a
//              format or ABI check). Runs alongside compilation by default,
//              because serialising every compile behind a linter is a cost
//              nobody accepts and "the build still fails" is just as true.
//   Object   — outputs join the LINK set (a resource compiler, objcopy
//              embedding a blob, a generated .def, a pre-built .o)
//   Artifact — inputs are link outputs (codesign, packaging, size budgets)
//
// `Object` completes the table (mcpp#365). The other three attach to the
// compile inputs, to nothing, and to the link OUTPUTS — leaving the link
// INPUTS, the one attachment point a build graph obviously has, inexpressible.
// The consequence was not theoretical: a Windows resource could only reach the
// linker by naming a pre-built `.res` in `[build].ldflags`, where it is a flat
// string in the link command rather than a file in the graph — so editing the
// icon produced "ninja: no work to do". A missing attachment point does not
// stop people; it makes them route around the graph.
//
// INV-D, the constraint that makes this expressible at all: the declaration
// must name its OUTPUT FILES, not merely promise some. mcpp fixes the source
// set, the fingerprint, compile_commands.json and the module topo order during
// prepare, and all of them need to know which files exist. Content may arrive
// later; names may not.
struct BuildAction {
    enum class Role { Source, Check, Object, Artifact };

    std::string                        id;        // diagnostics + edge naming
    // Which package's `build.mcpp` declared this. Filled by the engine when
    // actions are collected into the plan, NOT by the build program — the
    // program does not know, and the engine already does.
    //
    // Load-bearing, not bookkeeping: the ordering edge an action needs is
    // scoped to the declaring package, because `include_dir` colours only that
    // package's own translation units. A build-wide ordering would express a
    // dependency that does not exist and put it on the critical path of a
    // build whose wall clock is dominated by one. Spelled the same way
    // `CompileUnit::packageName` is (`qualified_package_name`), because the
    // two are matched against each other.
    std::string                        packageName;
    Role                               role = Role::Source;
    std::vector<std::string>           inputs;    // absolute or package-relative
    std::vector<std::string>           outputs;   // ditto; declared, see INV-D
    // Object only: which link units receive the outputs. Empty = every LINKED
    // IMAGE of the declaring package — binary, shared library AND test binary.
    // Test binaries are in the default set because they link the same library
    // code: leaving them out made `mcpp build` pass and `mcpp test` fail with
    // `undefined symbol` on the very symbol the action exists to provide. It is
    // also the only workable default, because test link units are DISCOVERED
    // from tests/*.cpp — their names are not in mcpp.toml, and a build.mcpp that
    // spells one stops building under plain `mcpp build`, where it does not
    // exist. (`[resources]` deliberately excludes them: an icon belongs to what
    // the project ships, not to a test runner.)
    //
    // Artifact infers its target from `${mcpp.target_file:NAME}` appearing in
    // its inputs; Object cannot, because it runs BEFORE the link and so has no
    // link output to name. Naming the targets is the only honest option, and
    // EVERY unknown name is an error — including one alongside a name that did
    // match, which is the shape a typo actually takes.
    std::vector<std::string>           targets;
    std::vector<std::string>           command;   // argv; NOT a shell string
    // Serialised module facts for a generated OUTPUT, when it is a module
    // interface. Same "declare instead of discover" trade `[modules].scan_overrides`
    // already makes — and the reason a generated `.cppm` does not need its
    // content to exist during prepare.
    std::vector<std::string>           provides;
    std::vector<std::string>           imports;
    // Check only: make compilation wait for this to pass. Off by default.
    bool                               blocking = false;
    std::string                        description;
};

// `[resources]` — metadata and assets compiled INTO the produced artifact
// (mcpp#365).
//
// SCOPE. Today only PE targets consume this: `icon` becomes RT_GROUP_ICON and
// the version fields become an RT_VERSION resource. On ELF/Mach-O the whole
// section is INAPPLICABLE — not degraded, not skipped-with-a-warning: there is
// no consumer, the build is byte-identical, and nothing is said. That is why
// the section is spelled `[resources]` and not `[windows]`, and why it does not
// need (or accept) a `cfg(windows)` predicate: an icon is a cross-platform
// CONCEPT — only the file format and the embedding mechanism are per-OS — so a
// future macOS `.icns` / Linux `.desktop` consumer extends THIS section instead
// of splitting the axis three ways. It also could not live in the conditional
// channel: `[target.'cfg(...)'.build]` carries BuildInputs and nothing else.
//
// A DECLARED FILE THAT DOES NOT EXIST IS AN ERROR, deliberately, and this is a
// documented deviation from what #365 asked for. Every other declared input in
// mcpp behaves this way (`main = "..."` must match exactly one file,
// scan_overrides globs must match ≥1, a missing nasm is fatal), and "missing →
// silently skip" would institutionalise the very failure this feature exists to
// fix: a release binary shipping with no icon and no version metadata, with
// nothing in the build output saying so. Not wanting an icon is already
// expressible — delete the line.
struct ResourceVersionInfo {
    std::string company;            // default: [package].authors[0]
    std::string product;            // default: [package].name
    std::string description;        // default: [package].description
    std::string copyright;          // default: synthesised from authors/license
    std::string originalFilename;   // default: the produced file name
    std::string internalName;       // default: [package].name

    bool empty() const {
        return company.empty() && product.empty() && description.empty()
            && copyright.empty() && originalFilename.empty() && internalName.empty();
    }
};

struct Resources {
    std::filesystem::path              icon;         // e.g. "assets/app.ico"
    std::vector<std::filesystem::path> files;        // author-written .rc sources
    // Escape hatch for the .rc input scanner: a file name reached through a
    // macro (`1 ICON APP_ICON`) is invisible to it. mcpp names what it could not
    // resolve and points here — same "declare when discovery is not enough"
    // trade as [modules].scan_overrides.
    std::vector<std::filesystem::path> extraInputs;
    // Unset = the default rule: synthesise a version resource unless the author
    // supplied their own .rc (in which case they own the resource ID space).
    std::optional<bool>                versionInfo;
    ResourceVersionInfo                info;

    bool declared() const {
        return !icon.empty() || !files.empty() || versionInfo.has_value()
            || !info.empty() || !extraInputs.empty();
    }
    // The 3-row rule from the design doc, in one place.
    bool synthesize_version_info() const {
        if (versionInfo.has_value()) return *versionInfo;
        return files.empty();
    }
};

// `[build]` section — tunables for the build backend.
//
// M5.0: now also carries `sources` (moved from [modules]) and `include_dirs`
// (new). Defaults are injected by load() after parse if these are empty.
//
// Inherits the additive inputs rather than nesting them: `buildConfig.cflags`
// is read in ~150 places, and a BuildConfig genuinely IS a set of build
// inputs plus the selection axis and resolved policy scalars.
// One named way of reaching the artefact.
//
// ⚠️ `longLived` IS DECLARED, NOT DERIVED FROM THE NAME. Whether the process
// ends is the one thing the engine must act on and no argv can express:
// `openocd -c "program … exit"` terminates and `openocd -c "init"` does not,
// spelled alike up to the argument the package chose. Deriving it from a name
// would work only for names the engine knows — the coupling this removes.
//
//   false  runs to completion; the exit code is the verdict
//   true   no natural end; the operator ends it, and a non-zero status
//          afterwards is that rather than a failure
struct NamedRunner {
    std::vector<std::string> argv;
    bool                     longLived = false;
};

struct BuildConfig : BuildInputs {
    // How `mcpp run` / `mcpp test` execute an artifact this host cannot run,
    // as an argv template (the artifact path is appended, or substituted for
    // `{}`).
    //
    // On BuildConfig rather than only in `[target.<triple>].runner` because
    // the value is MACHINE-SPECIFIC: the emulator lives in a package payload
    // whose path carries a home and a version, so only a `build.mcpp` can
    // compute it — and a build program writes into BuildConfig. A
    // board-support package emitting `mcpp:runner=` is the intended producer;
    // the manifest key remains the consumer's override.
    //
    // ⚠️ EXACTLY ONE provider among the dependencies. Two board-support
    // packages both claiming to know how to run the artifact is a
    // configuration error, not something to merge: appending would produce an
    // argv that is neither one's, and it would fail at exec time with no
    // indication of which package contributed which token.
    std::vector<std::string>            runner;
    // ⭐⭐ NAMED WAYS OF REACHING THE ARTEFACT, AND THE ENGINE KNOWS NONE OF
    // THEIR NAMES.
    //
    // `runner` above is how the artefact is EXECUTED. Writing it to a device,
    // watching what it prints, starting a debug server, deploying it, serving
    // it — every one of those is the same shape: an argv the PACKAGE knows and
    // a tool performs, with the artefact appended or substituted for `{}`. The
    // only thing distinguishing them is a name.
    //
    // ⚠️ SO THE NAME IS DATA, NOT VOCABULARY. An earlier version of this gave
    // `flash`, `monitor` and `debug` their own members, enum values, TOML keys
    // and subcommands — four instances of one idea, where a fifth would have
    // touched nine places. Worse, it put EMBEDDED vocabulary into the engine:
    // a web package could not add `serve`, nor a cluster package `submit`,
    // without an engine release. A map has neither problem, and every domain
    // gets the same flow.
    std::map<std::string, NamedRunner> namedRunners;
    bool                                runExclusive = false;

    // Was `sources` WRITTEN, as opposed to merely being empty?
    //
    // Presence is semantic here for the same reason it is on
    // `XlingsConfig::subosDeclared`: an absent key selects the default glob,
    // while an explicit `sources = []` selects "compile nothing". A container
    // alone cannot tell those apart, and until this flag existed it did not:
    // `sources = []` and deleting the line produced byte-identical build
    // graphs, so an author had NO spelling for "nothing".
    //
    // A binary distribution package needs that spelling. It ships prebuilt
    // artifacts and, in the header-only shape, no compilable source at all —
    // yet any file left under `src/` would be swept up by the default glob and
    // compiled into the consumer's build, where it can collide with the very
    // symbols the prebuilt library already defines.
    //
    // Deliberately on BuildConfig and not on BuildInputs: the conditional axis
    // (`[target.'cfg(...)'.build]`) only ever APPENDS sources, so "declared
    // empty" has no meaning there — it is the same as contributing nothing.
    bool sourcesDeclared = false;
    // `[build] jobs` — how many compiles to run at once. A decimal count,
    // "auto", or empty (the default) meaning "let the backend decide".
    //
    // Kept as TEXT rather than a number so that "auto" survives into the build
    // that actually runs: resolving it at parse time would freeze one machine's
    // core count into a value that then travels with the manifest.
    std::string jobs;
    // `[build] bmi_schedule` — when the BMI becomes visible to importers:
    // "auto" (default), "on", "off".
    //
    // "on" publishes each module's BMI as soon as it exists and moves code
    // generation onto a separate edge, so downstream units stop waiting for
    // work they do not need. The per-compiler strategy that implements it
    // (`detach-codegen` for gcc, `two-phase` for clang) is chosen by
    // mcpp.build.schedule::decide and reported by `mcpp build --verbose`.
    //
    // NAMED FOR WHAT IT SCHEDULES. It was `schedule`, which said only that
    // something was being scheduled — and disagreed with its own environment
    // override, `MCPP_BMI_SCHEDULE`. The two spellings now match.
    //
    // Text for the same reason `jobs` is: the meaning of "auto" depends on the
    // compiler doing the build, and resolving it at parse time would freeze one
    // machine's answer into a manifest that travels.
    std::string bmiSchedule;
    // feature name → extra source globs gated by that feature. A glob listed
    // here is EXCLUDED from the default build and only compiled/linked when the
    // feature is active for this package (resolved in prepare_build). Lets a
    // dependency expose an optional component (e.g. gtest's gtest_main.cc behind
    // the "main" feature) without it being linked by default — see
    // .agents/docs/2026-06-25-gtest-main-feature-and-add-dev-design.md.
    std::map<std::string, std::vector<std::string>> featureSources;
    // `[build] sources` entries written as a table:
    //
    //     sources = ["src/**/*.cppm", { glob = "src/kernels/**/*.cu", accel = "cuda12.9+{sm_89}" }]
    //
    // The glob ALSO appears in `sources`, so every reader that walks the plain
    // list sees it; this carries the constraint that decides whether it
    // applies to a given build. Resolved in prepare_build: the glob must match
    // at least one file (an empty match is a typo, not a no-op); when the
    // build asks for no accelerator the glob is excluded, which is how
    // `--no-accel` yields the CPU-only variant of a project; and when it does
    // ask for one, the constraint must lie within what the build targets, or
    // the build is refused naming both. Device-kind files the effective source
    // set matches are handed to the package's build program
    // (MCPP_DEVICE_SOURCES) rather than compiled by the engine, which has no
    // rule for them: that is the rule package's business.
    struct SourceConstraint {
        std::string glob;
        std::string accel;    // wire form, the mcpp.pack.abi_tag grammar
    };
    std::vector<SourceConstraint> sourceConstraints;
    // feature name → package-owned preprocessor defines (e.g. "-DEIGEN_USE_BLAS").
    // Feature System v2 Stage 1: when the feature is active these are appended to
    // the package's compile flags alongside the automatic -DMCPP_FEATURE_<NAME>
    // (resolved in prepare_build). Restricted by convention to the package's own
    // namespaced macros — features do NOT inject free-form cflags/ldflags, which
    // would break feature-union composition. See
    // .agents/docs/2026-06-29-feature-capability-model-design.md.
    std::map<std::string, std::vector<std::string>> featureDefines;
    // #253: feature name → per-glob compile flags gated by that feature. Same
    // ordered GlobFlags model as `globFlags` below; when the feature is active
    // the entries are appended AFTER the base globFlags (prepare_build), so a
    // feature rule wins over a broader base rule via "last flag wins". Lets a
    // feature's group-specific flags co-locate with its sources (e.g. opencv
    // dnn's mlas defines) instead of living as base rules whose globs go dead
    // on feature-off builds. Private per-TU flags — never propagate (contrast
    // featureDefines above, which are interface switches).
    std::map<std::string, std::vector<GlobFlags>> featureFlags;
    // [build] module_extensions — extra file extensions this package's module
    // INTERFACES use, on top of the built-in `.cppm`. Additive and opt-in:
    // `.ccm` / `.cxxm` / `.ixx` are NOT built in, because widening the
    // built-in set also widens the default source glob, which would make a
    // published package with a vendored MSVC-only `.ixx` start compiling it on
    // the next mcpp upgrade — a break its author cannot fix.
    //
    // Consumed through mcpp.source_kind (never read raw): the table it builds
    // decides the graph shape, so this vector is part of the fingerprint.
    // Scoped to the declaring package — a dependency is classified by its own
    // manifest, never by its consumer's.
    std::vector<std::string>            moduleExtensions;
    // [build] accel — which accelerator backends and device architectures this
    // build targets, in the wire form mcpp.pack.abi_tag reads. Empty means the
    // build asks for none, and then every prebuilt artifact satisfies it
    // vacuously; ordering in the descriptor is what makes the CPU variant win.
    std::string                         accel;
    // [build] build_program_timeout — seconds this package's build.mcpp may
    // run before mcpp kills it. 0 = no limit; nullopt = use the built-in 600.
    //
    // `optional` is load-bearing, not style. With a plain `int` the default
    // value would have to be 0, which MEANS "no limit" — so every project that
    // never mentions the key would silently lose its run bound.
    //
    // Deliberately NOT part of the fingerprint: it changes no edge in the
    // graph, and folding it in would make raising a timeout rebuild the whole
    // project — the opposite of what someone raising a timeout wants.
    std::optional<int>                  buildProgramTimeoutSecs;
    std::map<std::filesystem::path, std::string> generatedFiles; // Form B package-owned support files
    // Build-graph nodes declared by this package's build program
    // (`mcpp:action=`). Empty for every package that does not use one, so an
    // ordinary build is untouched.
    std::vector<BuildAction>            actions;
    bool                                staticStdlib = true;
    // #336 — the C++ runtime DISTRIBUTION contract: what the artifact promises
    // about the machine that runs it ("self-contained" | "toolchain-coupled" |
    // "host-coupled"). Empty = unset, in which case `staticStdlib` supplies it
    // (true → self-contained, false → host-coupled), which is exactly what that
    // flag has always been documented to mean.
    //
    // Why a separate field rather than widening the bool: the bool spells a
    // MECHANISM ("statically link the stdlib") and expanded into three
    // different per-platform meanings — including a silent no-op on
    // Linux/libc++, where it produced a toolchain-coupled artifact while
    // claiming to be static. The contract spells the INTENT, and
    // build/distribution.cppm maps intent to mechanism in one total function.
    std::string                         cxxRuntime;
    // Per-role override for test binaries. Empty = follow `cxxRuntime`.
    // Tests are the one role whose contract legitimately diverges: they never
    // leave the build machine, so "link the host's runtime" is a defensible
    // choice there and an indefensible one for a shipped artifact.
    std::string                         cxxRuntimeTests;
    // Per-role override for shared libraries. Empty = the role default, which
    // on ELF is toolchain-coupled (see `dist::default_contract`): a .so that
    // embeds its own libstdc++ exports it into the process's single global
    // symbol namespace and becomes the executable's C++ runtime by accident.
    // Setting this to "self-contained" is supported and additionally emits
    // `--exclude-libs` so the escape hatch cannot re-open that.
    std::string                         cxxRuntimeShared;
    // "" (default = dynamic), "static", "dynamic" — chosen at resolve
    // time from --static / --target / [target.<triple>].linkage. Wired
    // through to ninja backend as the `-static` link flag.
    std::string                         linkage;
    // [build] target = "<triple>" — the project's default build target
    // (≙ cargo's build.target). Used when no --target flag is passed;
    // "default to fully-static musl" belongs here, not in a toolchain name
    // (static output is a product property, not a compiler-family property).
    std::string                         target;
    // M5.x C-language support: `cStandard` controls -std= for the C compile
    // rule (.c files); empty → backend default ("c11" today). The cflags /
    // cxxflags / ldflags vectors themselves live in BuildInputs above.
    // Dialect-class C++ flags: flags that change what the standard library's
    // headers DECLARE or participate in module dialect checks (issue #210's
    // -freflection: libstdc++'s <meta> is gated on __cpp_impl_reflection).
    // These are module-graph-global — they ride -std='s channels (global
    // cxxflags for every TU incl. deps, the std/std.compat BMI prebuild,
    // scan commands). Populated from [build] dialect_cxxflags plus
    // auto-promotion of known flags found in [build] cxxflags
    // (see dialect_flags()).
    std::vector<std::string>           dialectCxxflags;
    std::string                         cStandard;
    // Escape hatch for the hermetic link check: a sandbox toolchain whose
    // CRT/loader resolve OUTSIDE the sandbox is a hard error by default
    // (silent host contamination, or issue #195's bare-CRT link failure);
    // set true to deliberately link against host libraries.
    // MCPP_ALLOW_HOST_LIBS=1 is the per-invocation equivalent.
    bool                                allowHostLibs = false;
    // macOS minimum supported OS version for produced binaries
    // (LC_BUILD_VERSION minos), e.g. "14.0". Mirrors the ecosystem
    // conventions around deployment targets (the MACOSX_DEPLOYMENT_TARGET
    // env var that cargo/rustc/cc honor; SwiftPM's `platforms:` manifest
    // field; CMAKE_OSX_DEPLOYMENT_TARGET). Precedence: the env var (an
    // explicit per-invocation override) wins over this manifest default;
    // empty + no env = toolchain/SDK default. No effect off macOS.
    std::string                         macosDeploymentTarget;
    // Resolved build-profile knobs (from [profile.<name>] + built-in defaults).
    std::string                         optLevel = "2";  // -O level
    bool                                debug    = false; // -g
    bool                                lto      = false; // -flto
    bool                                strip    = false; // link -s
    // `[build].default-profile` (alias: `profile`) — the project's DEFAULT
    // profile when no --profile/--dev/--release is passed. The global convention
    // default stays "release"; this lets a project opt its plain `mcpp build`
    // into e.g. "dev" without typing --profile. Precedence: --profile/--dev/
    // --release flag > [build].default-profile > "release". NOTE (distribution
    // footgun): a project that defaults to dev should pass `--profile release`
    // when producing a distributable (a pack-time release guard is a follow-up).
    std::string                         defaultProfile;
    // `[build] dependency_linkage` — "static" (default) | "shared" (#519).
    //
    // How this build wants its DEPENDENCIES to arrive: merged into the images
    // that use them, or as separate shared libraries beside them. A separate
    // axis from `[target.<triple>].linkage`, which answers the same-sounding
    // question about the C LIBRARY — and they are not independent, because a
    // statically linked image cannot load a shared object at all
    // (mcpp.build.linkage_form).
    //
    // A SCALAR, so it is deliberately absent from the `cfg(...)` channel:
    // that channel appends, and this needs last-wins. Overridable per profile
    // and per dependency edge. Empty = "static", which is byte-for-byte what
    // mcpp did before the key existed.
    std::string                         dependencyLinkage;
    // `[build] cache` — "global" (default) | "local" | "off". Project-level
    // default for the global dependency cache; --cache and MCPP_BUILD_CACHE
    // both override it. Validated in prepare_build (unknown value: warning, or
    // error under --strict) rather than here, so parsing a manifest never
    // depends on the build-mode vocabulary.
    std::string                         cacheMode;
};

// Canonical package identity used by runtime requirements/artifacts.  A short
// name is never sufficient here: two indices may legitimately contain the
// same name, and provenance must still be attributable after the build.
struct PackageId {
    std::string namespace_;
    std::string name;
    std::string version;
    std::string sourceProvenance;

    std::string canonical() const {
        std::string out;
        if (!namespace_.empty()) {
            out += namespace_;
            out += '.';
        }
        out += name;
        if (!version.empty()) {
            out += '@';
            out += version;
        }
        return out;
    }

    auto operator<=>(const PackageId&) const = default;
};

inline PackageId package_id(const Package& package) {
    PackageId out;
    out.namespace_ = package.namespace_.empty()
        ? std::string(kDefaultNamespace) : package.namespace_;
    out.name = package.name;
    const auto prefix = out.namespace_ + ".";
    if (out.name.starts_with(prefix) && out.name.size() > prefix.size())
        out.name.erase(0, prefix.size());
    out.version = package.version;
    out.sourceProvenance = package.sourceProvenance;
    return out;
}

// Provider-neutral runtime facts.  `requester`/`provider` are resolver-owned:
// descriptors declare the generic fact, then BuildPlan stamps the exact
// PackageId that supplied it.  This prevents a package from spoofing another
// package's identity and keeps same-short-name providers distinguishable.
struct RuntimeRequirement {
    std::string kind;
    std::string value;
    std::string phase = "run";       // link | run
    // How the loader finds whatever satisfies this, e.g. "rpath-of-dispatch",
    // "json-dir", "glvnd-dispatch". DECLARED, never inferred by mcpp: the
    // mechanism is a property of the provider's ecosystem, and inferring it
    // from the capability name would put provider-specific knowledge in mcpp
    // (`test_runtime_contract` gates exactly that).
    //
    // It earns its place because the mechanisms are not interchangeable: an
    // EGL vendor is found through a JSON file whose library_path is ABSOLUTE,
    // while GLX is found through the dispatch library's own DT_RPATH — so
    // "copy the directory across" satisfies one and not the other. Empty means
    // "not declared", which is reported as unknown rather than guessed.
    std::string discovery;
    PackageId   requester;
    bool        required = true;
};

struct RuntimeArtifact {
    std::string           role;
    PackageId             provider;
    std::filesystem::path path;
    std::string           provenance;
    std::string           abi;
    // What device code this artifact carries, in the wire form
    // mcpp.pack.abi_tag reads: `cuda12.8+{sm_80,sm_90f} ptx>=90`.
    //
    // A separate field rather than a segment of `abi`, because an architecture
    // list is a set and the tag is a dash-joined string whose triple already
    // carries a variable number of dashes. Empty means the artifact carries no
    // device code, which constrains nothing.
    std::string           accel;
    std::string           digest;
    std::string           hostFingerprint;
};

// Platform-neutral link intent.  Platform spelling belongs to flags.cppm;
// notably runtimeSearchDirs are not link-library search paths.
struct LinkIntent {
    std::vector<std::string>           libraries;
    std::vector<std::filesystem::path> linkLibraryDirs;
    std::vector<std::filesystem::path> transitiveNeededDirs;
    std::vector<std::filesystem::path> runtimeSearchDirs;
    std::vector<std::string>           frameworks;
    std::vector<std::filesystem::path> deployFiles;
};

// `[runtime]` — requirements needed when linking/launching built binaries.
struct RuntimeConfig {
    std::vector<std::filesystem::path> libraryDirs;   // relative to package root
    std::vector<std::string>           dlopenLibs;    // runtime-loaded sonames
    std::vector<std::string>           capabilities;  // host/system capabilities REQUIRED
    // Capabilities this package explicitly FULFILS. Only this field creates a
    // descriptor-owned provider fact; legacy `capabilities` is a requirement
    // and can never promote its requester into a provider.
    std::vector<std::string>           provides;
    // [runtime.<capability>] provider = "<pkg>" — explicit provider selection
    // (the three-tier knob: default/auto → explicit override).
    std::map<std::string, std::string> providerOverrides;
    // New structured contract.  The four legacy vectors above remain readable
    // for one compatibility train and are normalized by BuildPlan.
    std::vector<RuntimeRequirement> requirements;
    std::vector<RuntimeArtifact>    artifacts;
    LinkIntent                      linkIntent;
};

// `[xlings]` — mcpp's manifest surface for xlings' LOCAL PROJECT MECHANISM.
//
// Not a schema of mcpp's own: it is what a project writes into the project
// `.xlings.json` that gives a directory its own environment, and mcpp
// materializes it into `<proj>/.mcpp/.xlings.json` verbatim.
//
// `[xlings.workspace]` is the one table an author writes: an entry names a
// package and the version the project uses it at. mcpp both provisions it and
// materializes it as a resolution pin, which are the file's two fields and
// xlings' two consumers.
//
// `deps` is the pre-2026.9.3 spelling of the same statement. It is still
// honoured and is reported, because refusing it would reach a DEPENDENCY's
// manifest that a consumer pinning that package cannot edit.
//
// `envs` is gone: it was materialized here and read by nothing, while the
// documentation described an effect it did not have.
//
// See .agents/docs/2026-09-03-xlings-workspace-as-the-one-table.md, and
// .agents/docs/2026-06-29-manifest-environment-and-platform-design.md (L-1).

// WHEN an entry's package is needed, which is the axis package dependencies
// have had since the beginning (`[dependencies]` / `[build-dependencies]` /
// `[dev-dependencies]`) and tools did not.
//
// ⭐ `always` IS WHAT AN ENTRY WITHOUT `when` GETS, AND IT IS TODAY'S
// BEHAVIOUR EXACTLY. Narrowing is an optional action, never a question an
// author has to answer, so no manifest needs to change.
//
// ⚠️ `dev` IS THE ONLY TIER THAT DOES NOT PROPAGATE. It means "when the package
// that declared this is itself being developed", so a dependency's `dev` entry
// is never installed for a consumer. The other three reach a consumer, because
// a board-support package that knows which emulator runs its machine is
// precisely the thing that should say so once.
enum class ToolWhen {
    Always,   // no `when`: provisioned by every verb that builds
    Build,    // `mcpp build` onwards — the same as Always today, named
    Run,      // only when the artefact is to be executed (`run`, `test`)
    Dev       // only for the package that declared it, as the root
};

inline std::string_view to_string(ToolWhen w) {
    switch (w) {
        case ToolWhen::Build: return "build";
        case ToolWhen::Run:   return "run";
        case ToolWhen::Dev:   return "dev";
        default:              return "always";
    }
}

struct XlingsConfig {
    // The install addresses `[xlings.workspace]` asks for, resolved for THIS
    // host: `[<ns>:]<target>[@<version>]`. Derived rather than authored — the
    // provisioning pass, `fillXpkgDirs`, `xlingsDepBinDirs` and the `deps`
    // array of the materialised `.xlings.json` all read this, and none of them
    // has to know that a namespace may be written on either half of an entry.
    std::vector<std::string>           deps;
    // The resolution layer, resolved for THIS host: target → `[<ns>:]<version>`,
    // empty when the entry asked only for presence. Materialised as the file's
    // `workspace` object, which is a layer xlings merges over the machine's.
    std::map<std::string, std::string> workspace;
    // The same declaration for every platform, resolved but not collapsed:
    // platform → the install addresses that apply there. Only the descriptor
    // emitter reads it, because `xpm.<platform>.deps` needs all three at once
    // and the host resolution above has already discarded two.
    //
    // ⚠️ A VECTOR, NOT A NESTED MAP, AND NOT A STYLE CHOICE. Spelling this
    // `map<string, map<string, string>>` compiles the module and then produces
    // a TRUNCATED BMI under GCC 16: consumers fail with
    // `failed to read compiled module cluster N: Bad file data` and
    // `failed to load pendings for 'std::map'`, pointing at an unrelated file.
    // Measured while implementing this. The emitter needs the addresses and
    // never the keys, so the inner map bought nothing.
    std::map<std::string, std::vector<std::string>> workspaceByPlatform;
    // address → tier, for every address in `deps`. An entry that wrote no
    // `when` is absent here and reads as `Always` through `when_of` below.
    //
    // ⚠️ BESIDE `deps` RATHER THAN INSIDE IT. `deps` is materialised into
    // `.xlings.json` verbatim, and that file has no such axis — folding the
    // tier into the address would put a word xlings does not parse into a
    // field xlings reads.
    std::map<std::string, ToolWhen>    depWhen;
    // `[feature-xlings.<feature>]` — the same table, gated on a feature of the
    // package that declared it. feature → the install addresses it adds,
    // resolved for THIS host. A consumer that never activates the feature
    // never downloads them.
    std::map<std::string, std::vector<std::string>> featureDeps;
    // The pins those feature-gated entries carry, on the same axis as
    // `workspace`: address → `[<ns>:]<version>`. Merged into the materialised
    // `workspace` object only when the feature is active.
    std::map<std::string, std::string> featurePins;
    std::string                        subos;      // → .xlings.json "subos"
    // Presence is semantic: an absent key selects McppDefault, while an
    // explicitly written `subos = "default"` selects NamedSubos("default").
    // A string alone cannot distinguish absence from an invalid empty value.
    bool                               subosDeclared = false;

    ToolWhen when_of(std::string_view address) const {
        auto it = depWhen.find(std::string(address));
        return it == depWhen.end() ? ToolWhen::Always : it->second;
    }

    bool empty() const {
        return deps.empty() && workspace.empty() && featureDeps.empty()
            && !subosDeclared;
    }
};

// `[target.<triple>]` — per-target overrides.
// Picked up when caller passes --target <triple> to build/run/test.
struct TargetEntry {
    std::string                         toolchain;     // e.g. "gcc@15.1.0-musl"; empty = inherit [toolchain]
    std::string                         linkage;       // "static" | "dynamic" | "" (= auto by libc)
    // How `mcpp run` executes an artifact for this target when the artifact
    // cannot run on this machine (a freestanding image: wrong ISA, no loader).
    // A TEMPLATE and never a default — which emulator, which machine model and
    // which firmware mode are board facts, and an engine that guesses one is an
    // engine a different board has to fight. The artifact path is appended, or
    // substituted for `{}` when the template contains it.
    std::vector<std::string>            runner;
    // The project's override for a NAMED runner, on the same axis as `runner`
    // and with the same precedence: what the author of THIS project wrote beats
    // what a dependency supplied, and the override is reported. Written as
    // `[target.<triple>.runners]`, one key per name.
    std::map<std::string, std::vector<std::string>> namedRunners;

    // #336 — per-target C++ runtime contract, same vocabulary as
    // [build].cxx_runtime and overriding it for this triple. It lives HERE,
    // beside `linkage`, rather than in the `cfg(...)` conditional channel:
    // both describe what the produced artifact depends on at run time, both
    // are resolved before the build inputs are merged, and the conditional
    // channel deliberately carries build INPUTS and nothing else
    // (ConditionalConfig). One axis, one scoping rule.
    std::string                         cxxRuntime;
    // The target's C library, overriding the `sysroot` column of the target
    // table for this triple. Same axis as `toolchain` overriding `pin`: one
    // names the compiler the target resolves, the other names the C library,
    // and both were engine-only until a project had a reason to disagree.
    //
    // ⚠️ TWO MEMBERS AND NOT AN `std::optional<std::string>`, AND THE REASON IS
    // NOT STYLE.
    //
    // ABSENT and EMPTY are different answers — absent inherits the target row,
    // `sysroot = ""` is the ZERO-LIBC tier — so a plain string alone cannot
    // carry the distinction. An optional can, and was the first version.
    //
    // But an `std::optional<std::string>` DATA MEMBER of an exported struct
    // forces this module's interface to materialise that specialisation's
    // special-member machinery, and under clang with the MSVC standard library
    // that broke every downstream translation unit constructing one:
    //
    //     MSVC\include\optional:307: error: no matching constructor for
    //     initialization of '_SMF_control<_Optional_construct_base<basic_string…
    //
    // The errors named test files the change never touched — the signature of a
    // std type in a newly-exported interface poisoning the importers' module
    // files rather than failing where it was written. `std::optional<std::string>`
    // already appeared in this module as a RETURN type without incident; a
    // member is what forces the instantiation.
    //
    // Two plain members carry the same information and instantiate nothing.
    std::string                         sysroot;
    bool                                sysrootDeclared = false;
    // ⚠️ NO per-role field here. There used to be a `cxxRuntimeTests`, and it was
    // parsed nowhere and applied nowhere — a configuration key that looked
    // available and did nothing (#418). The per-target channel carries the
    // SCALAR contract only; `[build].cxx_runtime`'s table form already covers
    // the role split, and an unsupported key in `[target.<triple>]` is now
    // reported rather than dropped.
};

// `[target.'cfg(...)'.build]` — platform-conditional build flags (L1). The
// predicate is the raw `[target.<predicate>]` key (e.g. `cfg(windows)`,
// `cfg(all(linux, not(arch="aarch64")))`, or a bare triple). It is stored
// DEFERRED here because manifest parsing is target-agnostic; prepare_build
// evaluates it against the RESOLVED target (host triple for a native build,
// the --target triple for a cross build) and merges matching flags into
// buildConfig. See .agents/docs/2026-06-29-manifest-environment-and-platform-design.md.
struct ConditionalConfig {
    std::string                         predicate;     // the [target.<predicate>] key
    // Everything `[target.<pred>.build]` may contribute, and nothing else.
    //
    // Previously four hand-listed vectors, which is why per-glob `flags` was
    // inexpressible here while the xpkg descriptor's `mcpp.<os>` sections
    // supported it (#258): the conditional reader maintained its own subset
    // of [build]'s keys and nobody noticed it had fallen behind. Carrying the
    // BuildInputs type instead means the set cannot drift, and a key outside
    // it — `linkage`, `target`, a profile knob — is simply not a member, so
    // it cannot silently parse into a field nothing downstream reads.
    //
    // Conditional source globs (G1b) live in `inputs.sources`: appended to
    // [build].sources when the predicate matches the resolved target — the
    // declarative gate for arch-specific code (x86 .asm on x86 targets only).
    // `!`-exclusion globs work there too (the scanner handles positive+
    // negative sets).
    BuildInputs                         inputs;
    // `[target.<sel>.runtime]` — the DIALECT-NEUTRAL half of a link line.
    //
    // `inputs.ldflags` above is spelled the GNU way and native `cl.exe` rejects
    // `-L`. These two keys say the same thing without committing to a spelling,
    // and `render_link_intent_flags` renders them as `/LIBPATH:` + `<n>.lib` or
    // `-L` + `-l<n>` depending on the target. A generated package carries BOTH,
    // because an older mcpp reads only the first — see the note where they are
    // merged for why the newer client must then IGNORE the ldflags rather than
    // add to them.
    std::vector<std::filesystem::path>  linkLibraryDirs;
    std::vector<std::string>            libraries;
    // Conditional dependencies (Phase 1b): merged into the corresponding
    // manifest maps in prepare_build when the predicate matches the resolved
    // target — before dependency resolution, so they resolve like any dep.
    std::map<std::string, DependencySpec> dependencies;
    std::map<std::string, DependencySpec> devDependencies;
    std::map<std::string, DependencySpec> buildDependencies;
    // #359: `[target.<sel>.feature-deps.<feature>]`. The conditional channel
    // carried three of the four dependency maps and silently lacked the
    // fourth, which is the exact failure this struct's `BuildInputs` comment
    // above describes for #258 — the conditional reader kept its own subset of
    // the keys and fell behind without anyone noticing.
    //
    // It is load-bearing for build-time provisions: a library that declares a
    // host tool behind a feature (`grpc`'s `codegen` pulling protoc) has no
    // other way to say "not on this platform", and an unconditional
    // declaration turns an unsupported platform into a hard error raised from
    // inside the LIBRARY's manifest, which its user cannot work around.
    std::map<std::string, std::map<std::string, DependencySpec>> featureDeps;
};

// `[lib]` — library "root" interface convention.
//
// Convention-over-configuration: a library package's primary module
// interface lives at `src/<package-tail>.cppm`, where `<package-tail>` is
// the last dotted segment of `[package].name` (e.g. `mcpplibs.tinyhttps`
// → `src/tinyhttps.cppm`). That file declares `export module
// <full-package-name>;` and re-exports the public partitions. The lib
// root then drives:
//   * `[modules].exports` default (the lib root's module = the only
//     externally-visible base module),
//   * `mcpp publish` xpkg generation (consumer just `import <name>;`),
//   * downstream tooling (docs / explain) entry point.
//
// Override the convention with `[lib].path = "src/foo.cppm"` (cargo-style)
// — the file must still `export module <package-name>;` (no partition).
//
// Lib-root is only meaningful for projects that ship a `kind = "lib"`
// target. Pure-binary projects (mcpp itself, scaffolded `mcpp new`)
// don't trigger any lib-root checks.
struct LibConfig {
    std::filesystem::path               path;          // explicit override; empty = use convention
};

// `[pack]` — `mcpp pack` configuration. See docs/35-pack-design.md.
//
// `default_mode` picks the bundling strategy when the user runs bare
// `mcpp pack` (no `--mode` flag):
//   "static"          — full musl static, no PT_INTERP / RUNPATH
//   "bundle-project"  — bundle only project's third-party .so (default)
//   "bundle-all"      — bundle every dynamic dep including libc / libstdc++
struct PackConfig {
    std::string                         defaultMode;   // empty → "bundle-project"
    // ⚠️ THERE IS DELIBERATELY NO `[pack] profile`. Which profile `mcpp pack`
    // builds with is `--profile` > `[build] default-profile` > "release" —
    // packaging only changes the LAST step (from "dev"), because a fourth
    // precedence level would have to be resolved before `prepare_build` runs
    // and this manifest is what `prepare_build` produces.
    // Strip the SHIPPED artifacts (not a link-time `-s`; see mcpp.pack.strip).
    // Tri-state: unset = the default (strip), which is what a published binary
    // wants. `false` ships the artifact exactly as built.
    std::optional<bool>                 strip;
    // Where the separated `*.debug` files go, package-root-relative or
    // absolute. Empty = do not separate, which is the default: most publishers
    // do not ship a debug package, and writing one by default would double the
    // output of every `mcpp pack`.
    std::string                         debugSymbols;
    std::vector<std::string>            include;       // extra files/globs to ship
    std::vector<std::string>            exclude;       // patterns to drop from include
    // Mode C overrides — let the user expand or contract the PEP 600
    // skip list when their target distros differ from the default
    // assumption ("modern desktop Linux").
    std::vector<std::string>            alsoSkip;      // libs to ALSO skip on top of PEP 600
    std::vector<std::string>            forceBundle;   // libs to bundle even if PEP 600 says skip
};

// `[workspace]` — multi-package workspace support (0.0.11+).
//
// A workspace root mcpp.toml declares member packages. Members share
// a unified lock file, target directory, and can inherit dependency
// versions via `.workspace = true`.
//
// Virtual workspace (no [package]): pure management node.
// Rooted workspace ([package] + [workspace]): root is also a package.
// THE MERGE DISCIPLINE, STATED ONCE.
//
// Four keys were inherited before these tables existed — `[toolchain]`,
// `[target.<triple>]`, `[indices]` and `[workspace.dependencies]` — under three
// different rules, none of them written down. That is how the fifth key gets
// whichever rule its author happened to read. The rule for everything here:
//
//   SCALARS   the member wins when it DECLARED the key; otherwise the
//             workspace value applies. "Declared" is a fact the parser
//             records, not a comparison against a default — see
//             Package::standardDeclared for why the difference is load-bearing.
//
//   VECTORS   append, workspace first. A member adds to the shared set rather
//             than restating it, and the workspace flag comes first so a
//             member's flag can override it on the command line where later
//             wins. A member that needs to NOT have a workspace flag is a
//             signal the flag was declared at the wrong altitude.
//
//   DEPENDENCIES keep their explicit `x.workspace = true` opt-in, and that
//             exception is deliberate rather than historical: a dependency is
//             an EDGE in the resolution graph, and inheriting edges implicitly
//             would change what a member resolves without the member's manifest
//             naming it.
//
// Implicit-if-absent rather than cargo's per-key `x.workspace = true` for
// package and build keys, because the drift this exists to remove is a member
// that FORGOT to opt in. Making inheritance the default makes drift the thing
// you have to ask for.
//
// NOT EVERY KEY IS INHERITABLE. `[build] allow_host_libs` turns a correctness
// gate off; a workspace root setting it once would disable that gate for every
// member, including members added later by someone who never read the root
// manifest. Keys that describe HOW TO BUILD are inheritable; keys that describe
// WHICH SAFETY CHECK NOT TO RUN stay with the package whose artifact it is.
struct WorkspaceInherited {
    // `[workspace.package]` — the subset that is meaningful to share. `name` is
    // deliberately absent: two members cannot have one name, and a workspace
    // that could set it would be describing a single package.
    std::string              standard;
    bool                     standardDeclared = false;
    std::string              version;
    std::string              license;
    std::string              description;
    std::string              repo;
    std::vector<std::string> authors;
    // `[workspace.build]` — the INHERITABLE SUBSET of `[build]`, and the subset
    // is a stated list rather than "whatever [build] happens to carry". A key
    // that is not in it is refused at parse time with the reason, because
    // silently ignoring a key in a table whose whole purpose is propagation is
    // the failure this table exists to remove.
    BuildConfig              build;
    bool                     buildPresent = false;

    // THERE IS NO `[workspace.target.<triple>]`, DELIBERATELY.
    //
    // A plain `[target.<triple>]` block in the workspace root manifest is
    // ALREADY inherited by every member, per triple, member-wins — that
    // predates these tables. Adding a second spelling for a capability that
    // exists would be surface with no function, and two spellings of one rule
    // is how the two acquire different behaviour later. Documented in docs/05
    // rather than implemented here.
};

struct WorkspaceConfig {
    std::vector<std::string>                            members;       // relative paths to member dirs
    std::vector<std::string>                            exclude;       // paths to exclude
    std::map<std::string, DependencySpec>               dependencies;  // [workspace.dependencies]
    WorkspaceInherited                                  inherited;     // [workspace.package|build|target.*]
    bool                                                present = false;
};

// `[hooks]` — project build lifecycle commands (#496).
//
// The commands are host-shell strings written by the project author, run by
// `mcpp build` around the build it performs. See docs/05-mcpp-toml.md §2.16.
//
// ⚠️ ONLY THE ROOT PROJECT'S HOOKS ARE EVER RUN. Every manifest mcpp parses
// carries this field, including a DEPENDENCY's — and `mcpp build` reaches the
// invoker (mcpp.hooks) with the root project's manifest alone. A dependency
// that declares hooks is inert by construction, which is the only reason
// `mcpp add` of a third-party package does not become "run their shell
// command on my next build". Anything that adds a second call site inherits
// that responsibility.
// One event's command. Spelled either as a bare string or as a table — the
// same string-or-table shape `[dependencies]` and `[resources].version-info`
// already use, so it adds no parsing semantics.
struct HookCommand {
    std::string cmd;
    // Per-event override of the table's `timeout_seconds`. 0 = inherit. Only
    // meaningful for a self-closing interval; see below.
    int         timeoutSeconds = 0;
    // Restart the command if it exits before its interval closes. Only
    // `during_build` has an interval that can outlast a run, so this is
    // REJECTED on the other events rather than accepted and ignored.
    bool        loop           = false;

    bool empty() const { return cmd.empty(); }
};

// A hook is a command mcpp OWNS FOR AN INTERVAL; the event names the interval.
//
//   build_start / build_finished / build_failed   opens at the event,
//                                                 closes when the command exits
//   during_build                                  opens before the build,
//                                                 closes after it
//
// The first three are SELF-CLOSING, and "synchronous" is not a separate mode —
// it is what an interval closed by the command itself looks like. Everything
// that reads as a special case for `during_build` falls out of that one
// difference instead of being declared: `timeout_seconds` bounds one run and
// so does not apply where the build already bounds it; `loop` restarts a
// command that ended before its interval did, which a self-closing interval
// makes impossible.
//
// See .agents/docs/2026-08-30-project-build-hooks-owned-intervals.md.
struct Hooks {
    HookCommand buildStart;
    HookCommand buildFailed;
    HookCommand buildFinished;
    HookCommand duringBuild;

    int         timeoutSeconds = 10;   // default for one run of a hook command
    bool        enabled        = true; // whole table

    // ⚠️ EXPERIMENTAL: FALSE, AND CURRENTLY THE ONLY VALUE.
    //
    // The key means "a hook failure fails the build". While `[hooks]` is
    // experimental it does not get to decide that: a hook that fails is
    // reported as a warning and the build keeps whatever result it earned on
    // its own. `side_effect = true` is REJECTED by the parser rather than
    // accepted and ignored — a project that believes its build is gated on a
    // notifier, and is not, has been told something false.
    //
    // The mechanism below it is intact and is what the key will switch on when
    // the feature graduates; the parser check is the whole of the gate, so
    // removing it is the whole of the change.
    bool        sideEffect     = false;

    // "This project has work for `mcpp build` to do." Distinct from `enabled`:
    // a table that only sets policy keys declares no command, and must leave
    // the build path it would otherwise divert (the fast path) untouched.
    bool active() const {
        return enabled && !(buildStart.empty() && buildFailed.empty()
                            && buildFinished.empty() && duringBuild.empty());
    }

    // The bound on one run of `c`. The per-event value wins; 0 means it was
    // not given, which is what makes "inherit" expressible at all.
    int timeout_for(const HookCommand& c) const {
        return c.timeoutSeconds > 0 ? c.timeoutSeconds : timeoutSeconds;
    }
};

// [profile.<name>] — bundled build settings (opt level, debug, lto, strip).
struct Profile {
    std::string optLevel = "2";
    bool        debug    = false;
    bool        lto      = false;
    bool        strip    = false;
    // `dependency_linkage`, per profile (#519).
    //
    // OPTIONAL, and that is load-bearing rather than stylistic: resolving a
    // profile REPLACES the whole struct with the declared one, so a plain
    // value would make `[profile.dev] opt = 0` silently reset a
    // `[build] dependency_linkage = "shared"` back to the field default.
    // Absent means "whatever [build] said".
    std::optional<std::string> dependencyLinkage;
    // Passthrough escape hatch (fixed keys, open values — I6 completeness):
    std::vector<std::string> cflags;
    std::vector<std::string> cxxflags;
    std::vector<std::string> ldflags;
};

struct Manifest {
    std::filesystem::path       sourcePath;    // mcpp.toml's filesystem path

    // Unknown top-level keys silently skipped while synthesizing from an
    // xpkg mcpp segment — surfaced as warnings by `mcpp xpkg parse` so
    // schema evolution is loud in lint instead of invisible.
    std::vector<std::string>    xpkgUnknownKeys;

    // ⚠️ CAPABILITY NAMES INSIDE THE RESERVED `mcpp:` PREFIX THAT THIS ENGINE
    // DOES NOT KNOW, AND WHY THEY ARE RECORDED RATHER THAN REFUSED HERE.
    //
    // The reserved prefix is a closed set so that a misspelled layer name is an
    // error instead of a silently disabled behaviour. Refusing at PARSE time
    // made the set closed in a second, unintended sense: a package declaring a
    // layer added after the reader was released failed to load AT ALL, so the
    // vocabulary could never be extended by a published package.
    //
    // Measured 2026-08-24, `openkal-llvm-runtime` declaring the newly named
    // compiler-runtime layer, read by the release before it:
    //
    //   error: dependency 'openkal-llvm-runtime': mcpp.toml: error:
    //          `provides = ["mcpp:compiler-runtime=compiler-rt"]` names no
    //          capability mcpp knows.
    //
    // Whose manifest it is decides the answer. A name in the ROOT project's own
    // manifest is the author's to fix and they are looking at the build — an
    // error. A name in a DEPENDENCY's manifest was written against a newer
    // engine, and the correct response is to ignore the layer and say so, which
    // is what this engine already does for every other unknown key.
    std::vector<std::string>    unknownCapabilities;

    Package                     package;
    Language                    language;
    Modules                     modules;
    std::vector<Target>         targets;

    // version-string keyed dependencies (M2 short form only).
    std::map<std::string, DependencySpec> dependencies;
    std::map<std::string, DependencySpec> devDependencies;
    std::map<std::string, DependencySpec> buildDependencies;   // host-side tools (M5+ behavior)

    Toolchain                   toolchain;     // optional; empty == fallback
    BuildConfig                 buildConfig;
    Resources                   resources;          // [resources] (mcpp#365)
    RuntimeConfig               runtimeConfig;
    XlingsConfig                xlings;             // [xlings] build environment (L-1)
    Hooks                       hooks;              // [hooks] lifecycle commands (#496)
    std::vector<ConditionalConfig> conditionalConfigs;  // [target.'cfg(...)'.build], deferred
    std::map<std::string, Profile> profiles;   // [profile.<name>]
    // [features] — feature name → implied features ("default" = default set).
    std::map<std::string, std::vector<std::string>> featuresMap;

    // Feature System v2 Stage 3 — capabilities (provides/requires). A capability
    // is just a shared string. A package satisfies one via package-level
    // `provides` or via a feature's `provides`; a feature `requires` an abstract
    // capability instead of a concrete package, and the resolver binds exactly
    // one provider from the graph. See
    // .agents/docs/2026-06-29-feature-capability-model-design.md.
    std::vector<std::string>                        provides;        // package-level
    // [package] requires — the symmetric half of `provides`, and the only way
    // the layering rule can be enforced without a product name in the engine.
    //
    // A C++ runtime built from libc++'s sources is compiled, and its module,
    // by clang; gcc cannot consume it. That fact belongs to the package:
    //
    //     requires = ["mcpp:compiler=llvm"]
    //
    // The engine then checks a relation it can state generically — the named
    // layer must resolve to the named implementation — and reports a mismatch
    // by naming both, which a hardcoded table could not do for a family it had
    // never heard of.
    //
    // ⚠️ Names outside the `mcpp:` prefix are the feature system's and pass
    // through untouched, exactly as they do in `provides`.
    // ⚠️ The spelling is `requires_` because `requires` is a keyword.
    std::vector<std::string>                        requires_;
    // [package] exclusive — the capabilities this package claims it is the ONLY
    // provider of.
    //
    // WHY THIS IS DECLARED RATHER THAN INFERRED
    //
    // Two packages providing one capability is usually fine and sometimes the
    // point: `compat.openblas` and an MKL package both provide `blas`, and a
    // build that links one program against each is legitimate. So the engine
    // cannot refuse duplicate providers as a rule — the rule would break a case
    // this project documents.
    //
    // What it also cannot do is detect the case that is NOT fine. Two
    // implementations of one accelerator interface define the same symbols, and
    // the link then resolves every call to whichever archive the linker reached
    // first. Seeing that requires the object files, which do not exist when
    // capabilities are bound.
    //
    // So the package says it. An author who knows their library defines
    // `cublasCreate` writes:
    //
    //     provides  = ["gpu-blas"]
    //     exclusive = ["gpu-blas"]
    //
    // and two such packages in one graph are refused at binding time, naming
    // both — instead of at link time naming a symbol, or at run time naming
    // nothing at all.
    //
    // ⚠️ An entry that is not also in `provides` (or a feature's `provides`) is
    // a typo and is reported: claiming exclusivity over something you do not
    // provide cannot be acted on.
    std::vector<std::string>                        exclusive;
    // [package] std-module / std-module-flags — a package that IS a standard
    // library says where its `std' module source is and what that source needs
    // to compile. The build tool otherwise asks the COMPILER where std.cppm is
    // (`-print-library-module-manifest-path'), which is right whenever the
    // standard library is the compiler's own and wrong when it is a package's:
    // that source was configured for a target the compiler knows nothing about,
    // and its include path and its __config_site are the package's.
    //
    // Both are read only from a package that also provides the capability the
    // std-module gate reads; a package that says one without the other is
    // saying something about a library it does not supply.
    std::string                                     stdModule;       // relative path
    // The second module over the SAME library. A package that names
    // `std-module` and not this one offers `import std;` and not
    // `import std.compat;` — a complete answer, and better than silently
    // pairing its own `std` with the toolchain's `std.compat`.
    std::string                                     stdCompatModule;
    // (`std-module-flags` lives on BuildInputs so the cfg axis can carry it —
    //  see the member there.)
    std::map<std::string, std::vector<std::string>> featureProvides; // feature → caps
    std::map<std::string, std::vector<std::string>> featureRequires; // feature → caps
    // Feature System v2 Stage 2a — dependencies activated by a feature. A dep
    // declared ONLY here is optional: pulled into the resolution worklist only
    // when its feature is active (root --features or a dep spec's features=[...]).
    // Each value is a full DependencySpec, so a feature-dep may itself request
    // features. See .agents/docs/2026-06-29-feature-optional-dependencies-s2-design.md.
    std::map<std::string, std::map<std::string, DependencySpec>> featureDeps;
    // Feature System v2 #243 — dep/feat forwarding (Cargo parity). When this
    // package's feature <featureName> is active, each <depFeature> is injected
    // into the request set for dependency <depStableKey>, so a feature can open
    // a feature OF a dependency (e.g. an opencv module package's `dnn` feature
    // forwarding `compat.opencv/dnn`). <depStableKey> shares the keyspace of
    // `featuresMap`'s siblings `dependencies` / `featureDeps` (the raw selector
    // string == resolve_dependency_selector(...).stableMapKey). Forwarding is
    // additive: it only opens more of a dep's features, never pulls the dep in
    // (that is featureDeps' job) and is unaffected by `default-features`.
    // featureName → [(depStableKey, depFeature)].
    std::map<std::string, std::vector<std::pair<std::string, std::string>>>
        featureForwards;
    // Root-only: [capabilities] cap = "provider" pins (also fed by --cap).
    std::map<std::string, std::string>              capabilityPins;
    // #355 `[tools.overrides]` — "<package>:<tool>" → absolute path to an
    // existing host binary, which mcpp uses INSTEAD of building the tool.
    //
    // The escape hatch every comparable system provides (vcpkg's
    // VCPKG_HOST_TRIPLET, CMake's LLVM_NATIVE_TOOL_DIR, Qt's QT_HOST_PATH,
    // Cargo's `target = "target"`). Without one, a user whose tool cannot be
    // built from source — or who already has the right binary — has no way
    // forward at all.
    //
    // Deliberately NOT part of the tool store key: an override is an escape
    // hatch, not a reproducible input. `mcpp doctor` reports the ones in
    // effect so a build that silently used one is still explainable.
    std::map<std::string, std::string>              toolOverrides;

    // [target.<triple>] tables — empty if user didn't declare any.
    std::map<std::string, TargetEntry> targetOverrides;

    // [pack] — `mcpp pack` config (see docs/35-pack-design.md).
    PackConfig                         packConfig;

    // [lib] — library root interface convention (M5.x+).
    LibConfig                          lib;

    // [workspace] — multi-package workspace.
    WorkspaceConfig                    workspace;

    // [indices] — custom package index repositories (index-name → IndexSpec).
    std::map<std::string, mcpp::pm::IndexSpec> indices;

    // M5.0: post-parse computed/inferred state
    CppStandardConfig           cppStandard;
    bool                        usesModules    = true;   // refined by scanner
    bool                        usesImportStd  = true;   // refined by scanner
    std::vector<std::string>    inferredNotes;           // for `Inferred ...` banner

    // Non-fatal schema warnings collected during parse (e.g. unsupported keys
    // under [targets.<name>]). The caller (prepare_build) prints these and, under
    // --strict, escalates them to errors — mirroring the feature/platform path.
    std::vector<std::string>    schemaWarnings;
};

struct ManifestError {
    std::string                 message;
    std::filesystem::path       file;
    std::size_t                 line   = 0;
    std::size_t                 column = 0;

    std::string format() const {
        if (line)
            return std::format("{}:{}:{}: error: {}", file.string(), line, column, message);
        return std::format("{}: error: {}", file.string(), message);
    }
};

std::expected<CppStandardConfig, std::string> normalize_cpp_standard(std::string_view raw);

// A CppStandardConfig::level back as the value users write ("c++23"). The
// inverse of the whitelist above, so it lives next to it instead of becoming a
// second table somewhere in the build layer.
std::string cpp_standard_level_name(int level);

// The module-graph-global dialect flag set: explicit [build] dialect_cxxflags
// plus KNOWN dialect-class flags auto-promoted out of [build] cxxflags
// (they also stay per-unit there — duplication is harmless and keeps the
// mechanism explainable). Deduplicated, declaration order preserved.
std::vector<std::string> dialect_flags(const BuildConfig& bc);

// True when `flag` belongs to the known dialect-class list (changes what
// libstdc++/libc++ headers declare, or participates in BMI dialect checks).
bool is_dialect_flag(std::string_view flag);

// True when `flag` changes the language dialect the standard library BMI is
// compiled with, but is deliberately NOT auto-promoted into the graph-global
// set (`-fno-exceptions`, `-fno-rtti`, and their MSVC spellings). See the
// implementation for why the list is split in two rather than merged.
//
// Disjoint from `is_dialect_flag` by construction: a flag is promoted or it is
// recognised-and-refused, never both, so a caller cannot double-count one.
bool is_unpromoted_dialect_flag(std::string_view flag);

// The dialect-class flags present in `flags` that will NOT reach the `import
// std` BMI prebuild — the exact set that makes an importing TU fail with
// "language dialect differs". Empty when there is nothing to say.
//
// `flags` is the EFFECTIVE per-unit set, not `[build] cxxflags`: the same flag
// arrives from `[profile.<name>] cxxflags`, from `[target.<triple>.build]`, and
// from a `cfg(...)` block, and all three reach the compile line while none
// reaches the prebuild. A check that reads one table is silent on three
// spellings of one mistake.
std::vector<std::string> dialect_flags_missing_from_prebuild(
    std::span<const std::string> flags, std::span<const std::string> prebuild);

// The lib root's CONVENTIONAL name: `src/<package-tail>.cppm`, or `[lib] path`
// when the manifest states one. It does not touch the filesystem, so it is the
// right answer for a diagnostic or a validator's expectation and the wrong one
// for "which file is actually there" — `mcpp.manifest.toml` owns the probing
// form, because probing needs the extension table and this module deliberately
// does not import it (see the note there).
std::filesystem::path resolve_lib_root_path(const Manifest& manifest);

// True if the manifest declares at least one `kind = "lib"` target.
// Lib-root convention only applies when this returns true.
bool has_lib_target(const Manifest& manifest);



} // namespace mcpp::manifest

// ── helpers shared by the toml and xpkg parsers (exported: separate
//    modules need them reachable; they live in the manifest namespace) ──
export namespace mcpp::manifest {

bool starts_with_std_flag(std::string_view flag) {
    return flag == "-std" || flag.starts_with("-std=");
}

bool is_basename(std::string_view value) {
    return !value.empty()
        && value.find('/') == std::string_view::npos
        && value.find('\\') == std::string_view::npos;
}

std::optional<std::string> validate_target_soname(const Target& t,
                                                  std::string_view targetPath) {
    if (t.soname.empty()) return std::nullopt;
    // A LIBRARY may declare one, whatever form it is built in.
    //
    // This used to require `kind = "shared"`, which read as tidiness and was
    // in fact a constraint on the ecosystem: a `soname` is the name a library
    // is FOUND BY, and it is the only thing that lets mcpp's build of a
    // package and a third party's copy of the same library resolve to one
    // file rather than two. A package cannot state that unless it can write
    // the name down while still being consumed as a static library — which is
    // the normal case (mcpp-index: 84 `kind = "lib"` against 12 `"shared"`).
    //
    // ⚠️ RELAXED RATHER THAN MOVED: the old spelling made the whole manifest
    // FAIL TO LOAD, in both parsers. Any descriptor that starts writing this
    // key is therefore unreadable by every mcpp released before this change,
    // so the ecosystem-side rollout is gated on the index's floor moving —
    // see .agents/docs/2026-08-28-issue519-dependency-linkage-form.md §11.3.
    // The engine accepting it is what makes that gate start counting down.
    if (t.kind != Target::Library && t.kind != Target::SharedLibrary) {
        return std::format("{}soname is only valid for library targets", targetPath);
    }
    if (!is_basename(t.soname)) {
        return std::format("{}soname must be a library basename, got '{}'",
                           targetPath, t.soname);
    }
    return std::nullopt;
}


bool is_dialect_flag(std::string_view flag) {
    // Deliberately conservative first list (design doc §1.3a):
    // -fno-exceptions / -fno-rtti stay per-unit — see
    // `is_unpromoted_dialect_flag` below for why, and for what now happens
    // instead of silence.
    static constexpr std::string_view exact[] = {
        "-freflection",  "-fno-reflection",   // P2996 (GCC 16+)
        "-fcontracts",   "-fno-contracts",    // P2900
        "-fchar8_t",     "-fno-char8_t",
    };
    for (auto e : exact)
        if (flag == e) return true;
    // libstdc++ dual-ABI switch changes declared symbols/types wholesale.
    if (flag.starts_with("-D_GLIBCXX_USE_CXX11_ABI=")) return true;
    return false;
}

bool is_unpromoted_dialect_flag(std::string_view flag) {
    // THE SECOND TIER, AND WHY THE LIST IS SPLIT AT ALL.
    //
    // A flag is AUTO-PROMOTED (the list above) when a graph that mixes it is
    // ill-formed anyway: `-freflection`, `-fchar8_t` and the libstdc++ dual-ABI
    // macro change what the standard library headers DECLARE, so no dependency
    // can hold a coherent different opinion and promoting is the only outcome
    // that can work.
    //
    // A flag is NOT auto-promoted when a dependency can legitimately disagree.
    // `-fno-exceptions` and `-fno-rtti` remove a language facility the
    // dependency may use, and the consumer cannot make that decision on its
    // behalf: promoting them would compile every dependency without exceptions
    // because the root package asked for it, and the failure would land in
    // source the user does not own.
    //
    // WHAT WAS MISSING WAS THE THIRD OPTION. Left in `cxxflags`, these flags
    // reach every TU but not the `import std` BMI prebuild, so the compiler
    // refuses the BMI it was handed:
    //
    //   std: error: language dialect differs 'C++23', expected
    //               'C++23/no-exceptions'
    //
    // Recognising them here does not promote them. It lets the build refuse
    // BEFORE compiling, naming `dialect_cxxflags` — the key that does apply to
    // the prebuild — instead of leaving the user with a compiler message about
    // a file mcpp generated.
    static constexpr std::string_view exact[] = {
        "-fno-exceptions", "-fexceptions",
        "-fno-rtti",       "-frtti",
        // The MSVC spellings of the same two axes. `cl` bakes the choice into
        // the module the same way it bakes `_MSVC_MT`/`_MSVC_MD` in — which
        // `stdmod::ensure_built` already threads through for the CRT — so the
        // mismatch class exists there too. A GNU-only list would make this
        // check silent on one of the three supported toolchains, and silence
        // is what it exists to remove.
        "/EHsc", "/EHs-c-", "/EHa", "/EHac", "/GR", "/GR-",
    };
    for (auto e : exact)
        if (flag == e) return true;
    return false;
}

std::vector<std::string> dialect_flags_missing_from_prebuild(
    std::span<const std::string> flags, std::span<const std::string> prebuild) {
    std::vector<std::string> out;
    for (auto const& f : flags) {
        // An auto-promoted flag is already in `prebuild` by construction, so
        // asking the membership question covers both tiers with one rule
        // rather than special-casing the promoted list here — and it stays
        // correct on the day a flag moves from one tier to the other.
        if (!is_dialect_flag(f) && !is_unpromoted_dialect_flag(f)) continue;
        if (std::find(prebuild.begin(), prebuild.end(), f) != prebuild.end())
            continue;
        if (std::find(out.begin(), out.end(), f) == out.end())
            out.push_back(f);
    }
    return out;
}

std::vector<std::string> dialect_flags(const BuildConfig& bc) {
    std::vector<std::string> out;
    auto add = [&](const std::string& f) {
        if (std::find(out.begin(), out.end(), f) == out.end())
            out.push_back(f);
    };
    for (auto& f : bc.dialectCxxflags) add(f);
    for (auto& f : bc.cxxflags)
        if (is_dialect_flag(f)) add(f);
    return out;
}

std::expected<CppStandardConfig, std::string> normalize_cpp_standard(std::string_view raw) {
    auto trim_copy = [](std::string_view input) {
        std::size_t begin = 0;
        while (begin < input.size()
               && std::isspace(static_cast<unsigned char>(input[begin]))) {
            ++begin;
        }
        std::size_t end = input.size();
        while (end > begin
               && std::isspace(static_cast<unsigned char>(input[end - 1]))) {
            --end;
        }
        return std::string(input.substr(begin, end - begin));
    };

    std::string s = trim_copy(raw);
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    CppStandardConfig out;
    // C++20 is the floor: named modules are a C++20 feature, so mcpp's whole
    // build model does not exist below it. `import std;` still works here on
    // every toolchain mcpp ships (GCC >= 15, Clang >= 17 + libc++, MSVC STL
    // since STL#3977) — it is a C++23 *library* feature that all three
    // implementations also provide in C++20 mode. See
    // .agents/docs/2026-07-31-cpp20-standard-support-design.md.
    if (s == "c++20" || s == "c++2a") {
        out.canonical = "c++20";
        out.flag = "-std=c++20";
        out.level = 20;
        out.gnuDialect = false;
        return out;
    }
    if (s == "gnu++20" || s == "gnu++2a") {
        out.canonical = "gnu++20";
        out.flag = "-std=gnu++20";
        out.level = 20;
        out.gnuDialect = true;
        return out;
    }
    if (s.empty() || s == "c++23" || s == "c++2b") {
        out.canonical = "c++23";
        out.flag = "-std=c++23";
        out.level = 23;
        out.gnuDialect = false;
        return out;
    }
    if (s == "gnu++23" || s == "gnu++2b") {
        out.canonical = "gnu++23";
        out.flag = "-std=gnu++23";
        out.level = 23;
        out.gnuDialect = true;
        return out;
    }
    if (s == "c++26" || s == "c++2c") {
        out.canonical = "c++26";
        out.flag = "-std=c++26";
        out.level = 26;
        out.gnuDialect = false;
        return out;
    }
    if (s == "gnu++26" || s == "gnu++2c") {
        out.canonical = "gnu++26";
        out.flag = "-std=gnu++26";
        out.level = 26;
        out.gnuDialect = true;
        return out;
    }
    if (s == "c++latest") {
        out.canonical = "c++latest";
        out.flag = "-std=c++26";
        out.level = 999;
        out.gnuDialect = false;
        return out;
    }
    if (s == "c++fly") {
        out.canonical = "c++fly";
        out.flag = "-std=c++26";      // static GNU fallback; the real spelling
        out.level = 1000;             // comes from cppfly::std_flag (per-toolchain
        out.gnuDialect = false;       // latest, > c++latest's 999)
        out.experimental = true;
        return out;
    }

    return std::unexpected(std::format(
        "unsupported C++ standard '{}'; expected c++20, c++23, c++26, c++2a, c++2c, "
        "gnu++20, gnu++23, gnu++26, c++latest, or c++fly",
        raw));
}

std::string cpp_standard_level_name(int level) {
    switch (level) {
        case 20: return "c++20";
        case 23: return "c++23";
        case 26: return "c++26";
        case 999: return "c++latest";
        case 1000: return "c++fly";
        default: return std::format("c++{}", level);
    }
}

bool has_lib_target(const Manifest& manifest) {
    for (auto& t : manifest.targets) {
        if (t.kind == Target::Library || t.kind == Target::SharedLibrary) {
            return true;
        }
    }
    return false;
}

std::filesystem::path resolve_lib_root_path(const Manifest& manifest) {
    if (!manifest.lib.path.empty()) return manifest.lib.path;
    std::string tail = manifest.package.name;
    if (auto p = tail.rfind('.'); p != std::string::npos) tail = tail.substr(p + 1);
    return std::filesystem::path("src") / (tail + ".cppm");
}




} // namespace mcpp::manifest
