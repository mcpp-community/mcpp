// mcpp.pack — bundle a built binary into a self-contained release archive.
//
// TWO OUTPUT FAMILIES, ONE PIPELINE. An ELF artifact becomes a `.tar.gz` whose
// libraries live in `lib/` and are reached through a rewritten RUNPATH; a PE
// artifact becomes a `.zip` whose DLLs sit BESIDE the .exe, because that is
// where the Win32 loader looks and PE has no rpath to rewrite. A Mach-O program
// takes PE's placement inside the ELF family's archive: its dylibs sit beside
// it in `bin/`, where the `@loader_path` rpath it was linked with finds them
// (#634 A3). Same contract, same modes, different mechanism — see
// .agents/docs/2026-08-16-windows-toolchain-three-axes-design.md §4.
//
// The PE path runs on ANY host. That is not a portability nicety: the ELF
// path derives its closure by running the artifact under
// LD_TRACE_LOADED_OBJECTS, which is why it can cross neither an OS nor an
// architecture, and the `#if defined(_WIN32)` refusal that used to sit at the
// top of `run()` was that limitation surfacing rather than a missing branch.
// mcpp.pack.binfmt reads the import table instead, so a Linux box packaging a
// Windows build is simply what happens when nothing has to be executed.
//
// See docs/10-pack-and-release.md for the full design. Three modes:
//   Static          full musl static, no PT_INTERP / RUNPATH
//   BundleProject   bundle only the project's third-party .so (default)
//   BundleAll       bundle every dynamic dep incl. libc / libstdc++ / ld
//
// Layout produced under `target/dist/<name>-<version>[-<mode>]/`:
//   bin/<name>                 main executable
//   lib/*.so*                  bundled .so (BundleProject / BundleAll only)
//   share/...                  extra files declared in [pack].include
//   README.md / LICENSE        copied from project root if present
//   share/licenses/*           bundled .so LICENSE files (BundleAll only)
//
// Tarball name: `<name>-<version>-<triple>[-<mode>].tar.gz`
//   ⤷ mode suffix omitted for the default (`bundle-project`).

module;

export module mcpp.pack;

import std;
import mcpp.build.loader_contract;
import mcpp.build.stage;      // place_runtime_dlls publishes through the staging primitive
import mcpp.build.runtime_placement;   // and decides the C++ runtime's names by the plan's rule
import mcpp.config;
import mcpp.pack.binfmt;
import mcpp.pack.host_requirements;
import mcpp.pack.relocate;
import mcpp.pack.stage_tree;
import mcpp.pack.strip;
import mcpp.pack.zip;
import mcpp.pack.compress;
import mcpp.pack.distribution;
import mcpp.modgraph.glob;     // [pack] include / exclude
import mcpplibs.os.platform;
import mcpp.toolchain.platform;
import mcpp.invocation;
import mcpp.build.command_line;
import mcpp.run;
import mcpp.provider;
import mcpp.provider.select;
import mcpp.manifest;
import mcpp.project;              // the effective manifest a build reads
import mcpp.build.prepare_inputs;  // cfgpred: the target's conditional tables
import mcpp.toolchain.triple;
import mcpp.toolchain.base;
import mcpp.ui;
import mcpp.runtime.elf;
import mcpp.platform;

export namespace mcpp::pack {

enum class Mode { None, Static, BundleProject, BundleAll };

// WHAT SHAPE THE OUTPUT TAKES, and the third value is the one that is not a
// shape the engine knows.
//
// `tar` and `dir` answer the same question `msi` and `appimage` answer, so they
// belong on one axis -- which is why this is a wider set of values for one flag
// rather than a second flag. `Dispatched` carries a name the engine has never
// heard: `mcpp pack --format appimage` finds the provider among the resolved
// dependencies and hands it the staged tree, exactly as `--target` reaches a
// triple the engine did not have to know individually.
//
// The engine keeps `Tar` and `Dir` because an archive that extracts and runs is
// universal in the only sense that matters here: it needs no knowledge of
// anyone else's release. dpkg's control fields, AppImage's runtime, WiX's
// schema and Apple's notarisation each couple an mcpp release to a release mcpp
// does not control.
enum class Format { Tar, Dir, Zip, SingleFile, Dispatched };

// THE LINKAGE AXIS (SPEC-014 §1): a property of the program -- whether the
// libraries of its graph are in it or beside it -- which a pack states for the
// build it makes (`--linkage`, `[pack] linkage`). Unstated: as the project
// builds it.
enum class Linkage { Unstated, Static, Shared };

// The container an archive format writes, decided once in `make_plan`: a tar
// (compressed as `Options::compression` says), a zip, or none (a directory, a
// single file, a dispatched format).
enum class ArchiveKind { None, Tar, Zip };

// One more architecture of a tree (design 2026-10-29 §4.2): a leg the
// command named other than the one the pack built first, built by the
// pipeline before staging. `artifact` is that leg's program image; the images
// it names are found beside it, as for the first leg.
struct LoadedLeg {
    std::string           triple;   // canonical
    std::string           arch;     // the directory under `lib/`
    std::filesystem::path artifact;
};

// What happens when a FORMAT's dependency closure cannot be resolved on this
// host, as a function of the format alone — never of the reason (a Mach-O
// program whose loader ignores `LD_TRACE_LOADED_OBJECTS`, or a Windows host
// that cannot execute the artifact at all; see the two call sites in `run`).
//
// `Tar` and `Dir` are the archive: the staged tree IS the product, so an
// unavailable closure is the command failing, exactly as before this type
// existed. A dispatched format receives the staged tree regardless, because
// the provider may not need a closure at all — `dist-apk` reads
// `${mcpp.target_file:<name>}` and never looks at the tree — so the outcome
// there is "staged, closure not walked", never a refusal. A pure function of
// one enum value, so it is tested without building a Plan.
enum class ClosureUnavailableOutcome { CommandFails, StageWithoutClosure };

ClosureUnavailableOutcome closure_unavailable_outcome(Format format) {
    return format == Format::Dispatched
        ? ClosureUnavailableOutcome::StageWithoutClosure
        : ClosureUnavailableOutcome::CommandFails;
}

struct Options {
    Mode                            mode         = Mode::BundleProject;
    // The three axes (SPEC-014 §1) beyond `mode` and `format`: the linkage
    // the pack builds with, whether the format was stated (a format left to
    // the target is a zip for a Windows program and a tar elsewhere), and the
    // container's compression.
    Linkage                         linkage      = Linkage::Unstated;
    bool                            formatStated = false;
    mcpp::pack::compress::Choice    compression;
    bool                            compressionFromFlag = false;   // --compress, not [pack]
    // THE TREE OF A PLATFORM WITH A RUNNER (design 2026-10-29 §4.2, §4.3):
    // `tree` is set by the pipeline for an image of such a platform, whatever
    // the number of legs -- `bin/<name>`, `bin/<name>.exe`, `lib/<arch>/` --
    // and `universalName` (the platform's name, which names the package
    // instead of a triple) when the tree holds more than one leg. `systems`:
    // the runners a self-contained package carries, or the systems a single
    // file is made for, each `<os>`, `<os>-<arch>` or `all` (`--systems`;
    // empty: this machine's).
    bool                            tree = false;
    std::string                     universalName;
    std::vector<std::string>        systems;
    bool                            systemsFromManifest = false;   // `[pack] systems`, not --systems
    // The legs a pack of several targets stages into one tree (design
    // 2026-10-29 §4.2): the command's targets, a set expanded.
    std::vector<std::string>        legTargets;
    // `[pack] sign`: run on each program, image, library and loader of the
    // staged tree before the container is written, `{}` the file.
    std::vector<std::string>        sign;
    // The build stated its target base (design 2026-10-23 §4.11): the base it
    // was built against becomes the C library floor the package states.
    bool                            baseStated   = false;
    Format                          format       = Format::Tar;
    // The `--format` value when `format == Dispatched`. Empty otherwise.
    //
    // Not validated by the CLI, and deliberately: the set of valid values is a
    // property of the RESOLVED GRAPH, so the refusal has to wait until build
    // programs have declared what they provide. It arrives before anything is
    // compiled, which is the earliest point at which it can be exact.
    std::string                     formatName;
    // `--pkg-format` naming several containers (design 2026-10-29 §4.6):
    // every format, in order, the first being `format`/`formatName`; each
    // after the first is written from the same staged tree. Empty: one.
    std::vector<std::string>        formats;
    std::filesystem::path           output;        // empty = derive from manifest
    // `-o <directory>/` (design 2026-10-29 §4.2, §4.6): every file the pack
    // writes is written below it under its default name.
    std::filesystem::path           outputDir;
    std::string                     targetTriple;  // empty = host
    // Where a dependency NAME may be resolved to a file.
    //
    // Only used where the closure is read STATICALLY (PE, the Android row): on
    // the ELF host row the loader hands back resolved paths and no search is
    // performed here, and a Mach-O name carries its own search rule. Deliberately
    // never the target's own system directories — a DLL that resolves only
    // there is the target's to provide, and copying one is a broken program
    // rather than a heavier one.
    std::vector<std::filesystem::path> depSearchDirs;
    // The TOOLCHAIN's own runtime directory: libstdc++'s for gcc, the MSVC
    // toolset's `VC\Redist\MSVC\<v>\<arch>\Microsoft.VC*.CRT\` for cl.
    // Searched ONLY under the toolchain-coupled contract — see make_plan.
    std::vector<std::filesystem::path> toolchainRuntimeDirs;
    // #634 A3: the Android row's closure is read against the directories the
    // row's own driver links from (`-print-search-dirs`), and a name found in
    // `platformLibraryDirs` -- the directory the driver finds bionic's
    // `libc.so` in, which is the API level's stub directory -- is the
    // device's. Both are empty on every other row.
    std::vector<std::filesystem::path> toolchainLibraryDirs;
    std::vector<std::filesystem::path> platformLibraryDirs;
    // What the build placed relative to the executable, from
    // `runtime.deploy_files` and `runtime.deploy` (#615), as paths relative to
    // the executable's directory. Each is staged at the same relative path
    // beside the packed executable, in every mode: these are files a program
    // opens, not libraries a closure decides about.
    std::vector<std::filesystem::path> runtimeFiles;
    // Does the RESOLVED C++ runtime contract require the toolchain's own
    // runtime to travel WITH the artifact — i.e. `cxx_runtime =
    // "toolchain-coupled"`?
    //
    // `pack` used to be unable to see the contract at all (design §4.3): it
    // reached compile and link FLAGS and stopped there, so the step that
    // decides which files actually travel had no idea what had been promised.
    // On ELF the `ldd` closure happened to agree with it; on PE nothing did.
    //
    // A BOOL rather than `dist::Contract`, deliberately. Only one of the
    // three values changes anything here, so the enum would be three states
    // where the decision has two — and carrying it across this module
    // boundary crashed the clang 20.1.7 frontend outright (see run_pe). The
    // caller resolves the contract; this is the one bit of it that packaging
    // acts on.
    bool                            carryToolchainRuntime = false;
    // Names the host provides under the contract, whatever directory offers a
    // copy: the MSVC C++ runtime when it is not carried (host-coupled, or an
    // explicit `--mode system` over a defaulted contract). The closure states
    // them as the target's and never stages them. Compared without case, as
    // the PE loader compares them: an MSVC-linked image imports
    // `VCRUNTIME140.dll`.
    std::vector<std::string>        hostProvidedLibs;

    // ── how the shipped artifact is BUILT and what travels inside it ──
    //
    // `profile` is the `--profile` override only. The default is not spelled
    // here: `mcpp pack` passes `BuildOverrides::profile_fallback = "release"`,
    // so `[build] default-profile` still decides when it is set, and the
    // precedence rule stays in `resolve_profile_name` where the rest of mcpp
    // reads it.
    std::string                     profile;
    // Tri-state on purpose. `nullopt` = "nobody said", which is what lets
    // `[pack] strip` be consulted at all — a plain `bool` defaulted to true
    // would make the manifest key unreachable from the CLI's point of view.
    std::optional<bool>             strip;
    // `--debug-symbols <dir>`: where the separated `*.debug` files go. Empty =
    // do not separate.
    std::filesystem::path           debugSymbols;
    // `--features <LIST>`: root-package features for every build pass the
    // pack performs -- each leg, and both passes of a dispatched format. It is
    // the value `mcpp build --features` takes, carried as that string so the
    // parser that reads it stays the build's own (#641).
    std::string                     features;
};

// The strip decision for this run: `--strip`/`--no-strip` > `[pack] strip` >
// stripped.
//
// Spelled once because both packers ask it and a distribution that strips its
// libraries but not its programs is a distribution whose rule nobody can state.
bool resolve_strip(const Options& opts, const mcpp::manifest::PackConfig& cfg);

// Where the separated debug files go, absolute. Empty = do not separate.
// A manifest-relative path is resolved against the project root, like every
// other path a manifest names.
std::filesystem::path resolve_debug_dir(const Options& opts,
                                        const mcpp::manifest::PackConfig& cfg,
                                        const std::filesystem::path& projectRoot);

// #630 A9: one more leg of a several-triple application pack, built by
// `pack::pipeline::build_extra_android_legs`: the Android ABI it is staged
// under and the artifact that triple produced. #634 A3 adds where that leg's
// closure is read from, because each triple resolves its libraries against
// its own directories (see `Options::toolchainLibraryDirs`).
struct SharedLeg {
    std::string                        abi;
    // The leg's canonical triple, for the pack report (#649 E9).
    std::string                        triple;
    std::filesystem::path              artifact;
    // #649 E5: the shared libraries THIS leg's graph built. See
    // `Plan::graphSharedLibraries`.
    std::vector<std::filesystem::path> graphSharedLibraries;
    std::vector<std::filesystem::path> searchDirs;
    std::vector<std::filesystem::path> platformDirs;
};

// Resolved plan — all paths absolute, all decisions baked in.
struct Plan {
    Options                              opts;
    std::filesystem::path                projectRoot;
    std::filesystem::path                builtBinary;       // mcpp build artefact
    std::string                          binaryName;        // basename(builtBinary)
    std::filesystem::path                stagingRoot;       // target/dist/<root-dir>/
    std::filesystem::path                archivePath;       // …/<name>.tar.gz | .zip
    ArchiveKind                          archive = ArchiveKind::None;
    // The linkage the package's `mcpp-package.json` states: `static`,
    // `shared`, or `declared` (each library as it declares itself). Set by the
    // caller from the build.
    std::string                          linkageName = "declared";
    // The other legs of a tree or a single file (SPEC-014 §3), set by the caller
    // after `make_plan` as `extraSharedLegs` is.
    std::vector<LoadedLeg>               loadedLegs;
    std::string                          packageName;
    std::string                          packageVersion;
    std::string                          triple;            // e.g. "x86_64-linux-musl"
    // From manifest [pack]
    std::vector<std::string>             includeGlobs;
    std::vector<std::string>             excludeGlobs;
    std::vector<std::string>             alsoSkipLibs;
    std::vector<std::string>             hostProvidedLibs;   // Options::hostProvidedLibs
    std::vector<std::string>             forceBundleLibs;
    // What the TARGET machine must provide. Derived once, in make_plan, from
    // the same predicate `mcpp publish` uses — see mcpp.pack.host_requirements.
    std::vector<HostRequirement>         hostRequirements;
    // Is the artifact a PE? Read from the FILE, not inferred from the triple —
    // the file is the thing being packaged, and a triple is a request.
    bool                                 targetIsPe = false;
    // Is the artifact a wasm32-emscripten launcher? Read from the TRIPLE, not
    // the file: the packed file is `bin/<name>.js`, plain JavaScript text
    // carrying none of ELF/PE/Mach-O's magic, so `binfmt::identify` cannot
    // answer this the way it answers `targetIsPe`. The triple is what named
    // the file `.js` in the first place (see `artifact_naming`), so it is the
    // one fact this format IS recorded under.
    bool                                 targetIsWasm = false;
    // Is the artifact one a loader runs (an ok image, `<arch>-openkal`)? From
    // the triple, as for wasm: the file is an ELF shared object that no loader
    // of this machine runs, and its closure is the images it names.
    bool                                 targetIsLoaded = false;
    // The target's platform (SPEC-013): the carrier a self-contained pack of
    // a loaded artifact is built over (§4.7).
    mcpp::platform::Resolved             platform;
    // #622 A3/A10: is `builtBinary` an `Application` target whose link form
    // on THIS row is a shared object (`*-linux-android`,
    // `toolchain::triple::application_form`)? Read from the CALLER, which
    // already asked the manifest which target this file belongs to while
    // choosing it (`pipeline.cppm`'s program-selection loop) -- `make_plan`
    // has only the file and would have to re-derive the same answer from the
    // triple and the manifest a second time, which is the shape a dep
    // fingerprint gap in this codebase's own history warns against. A shared
    // object is not runnable here (an Android object names
    // `/system/bin/linker64` as its interpreter, which this host does not
    // have), so it is staged like a dependency .so -- under `lib/`, with no
    // dependency closure attempted -- rather than through the ELF closure
    // walk below, which asks the file to name its own needs by executing it.
    bool                                 programIsSharedObject = false;
    // #630 A9: the OTHER triples this `app` was packed for, each already
    // built by the caller (`pack::pipeline::build_extra_android_legs`). Set
    // AFTER `make_plan`, the way `strip`/`debugDir` are: what the request came
    // out as once more than one `--target` was resolved, which `make_plan`
    // itself has no way to know from a single triple. Empty means "an ordinary
    // single-triple pack", which stages the flat `lib/` layout.
    std::vector<SharedLeg>               extraSharedLegs;
    // The search set the PE and Android closures resolve names against, after
    // the contract has had its say (see make_plan).
    std::vector<std::filesystem::path>   searchDirs;
    // ── debug information: the RESOLVED decision, not the request ─────
    //
    // On the Plan rather than in Options because `Options` is what the user
    // asked for and this is what that came out as once the manifest and the
    // toolchain had their say. The library packer keeps the same split.
    bool                                 strip = true;
    std::filesystem::path                debugDir;   // absolute; empty = discard
    mcpp::pack::StripTools               stripTools;
    // #649 E5: WHAT THIS GRAPH BUILT, absolute: every `SharedLibrary` link
    // unit of the plan, the dependencies' shared images included. A staged
    // copy of one of these is stripped as the program is, because "a package
    // strips what it built" (the dh_strip rule `strip_program` quotes) covers
    // them: they were compiled from source by this build. Set after
    // `make_plan`, by the caller that holds the build plan.
    std::vector<std::filesystem::path>   graphSharedLibraries;
};

// Does packing `plan` strip anything? The strip decision, AND a format whose
// debug information is inside the image. A Mach-O or MSVC PE row keeps its
// debug information beside the image, so the answer there is no whatever
// was requested; the `Packing` line states this, not the request.
bool strips_on_this_row(const Plan& plan);

struct Error { std::string message; };

// What step 4 of `run` (resolving the dependency closure) produced.
//
// `walked = false` reaches a caller only for `Format::Dispatched` —
// `closure_unavailable_outcome` turns the same condition into an `Error` for
// `Tar` and `Dir`, so a provider is the only reader that ever sees `false`
// here. `reason` is populated exactly when `!walked`, and is the same text a
// hard refusal used to carry — moved from "before staging" to "step 4's
// outcome", per the design record's decision (§3 of
// 2026-09-13-630-what-a-framework-still-hits-in-the-engine.md).
//
// `needs` (#634 A3) is every name the closure resolved or failed to, for the
// stage manifest's `needs` lines; it is empty when no closure was read (a mode
// that bundles nothing, the wasm launcher). `walked` is true only when no
// entry of it is unresolved.
struct ClosureResult {
    bool                     walked = true;
    std::string              reason;
    std::vector<ClosureNeed> needs;
};

// ─── the closure, read from the files (#634 A3) ───────────────────────────
//
// THREE ROWS, ONE READER. PE, the Android shared-object row and Mach-O have
// their closure read from the files rather than traced by a loader, and they
// differ in two answers only: which names the target itself provides, and
// where a member is staged relative to the object that needs it. Those two
// answers are the rule; the walk is shared, and so are the refusal and the
// `needs` lines that the stagers in `run` derive from its result.
//
//   Pe       a name `binfmt::is_system_lib` knows (unless `[pack]
//            force_bundle` names it), or one found in no search directory,
//            is the target's -- as before this reader existed: a Windows
//            component has no file to find on another host, and only the
//            system directory holds it on a Windows one.
//   Android  a name found in a platform directory (the API level's stub
//            directory the driver links against) is the device's; any other
//            name must resolve in a search directory, or the closure is
//            incomplete.
//   MachO    a name under `/usr/lib/` or `/System/Library/` is the OS's.
//            Members are staged beside the program, so a name is a member only
//            when the loader resolves it there: `@executable_path/<file>`,
//            `@loader_path/<file>`, or `@rpath/<file>` when an rpath of the
//            image that needs it, or of the program, is exactly `@loader_path`
//            or `@executable_path`. Any other name -- an absolute install name
//            outside the OS's roots is read from that path on the target,
//            whatever the tree carries -- leaves the closure incomplete.
enum class ClosureRule { Pe, Android, MachO };

struct ClosureReadInput {
    std::filesystem::path              object;        // the program or application object
    ClosureRule                        rule = ClosureRule::Pe;
    std::vector<std::filesystem::path> searchDirs;    // Pe, Android
    std::vector<std::filesystem::path> platformDirs;  // Android
    std::vector<std::string>           forceBundle;   // Pe: overrides the system list
    std::string                        arch;          // MachO: the slice of a fat file
    // Pe: names never resolved in `searchDirs.front()`. `place_runtime_dlls`
    // passes the copies an earlier placement put beside the program, so that
    // each resolves again where it came from instead of shadowing a newer
    // version there. `mcpp pack` leaves it empty.
    std::vector<std::string>           notInFirstDir;
};

// A name that resolved to a file, and where that file is staged: `dest` is
// relative to the directory the object itself is staged in.
struct ClosureMember {
    std::string           name;
    std::filesystem::path source;
    std::filesystem::path dest;
};

struct ClosureUnresolved {
    std::string name;
    std::string why;
};

struct ClosureRead {
    std::vector<ClosureMember>     members;     // sorted by `dest`
    std::vector<std::string>       platform;    // sorted
    std::vector<ClosureUnresolved> unresolved;  // sorted by name
};

// Read `in.object`'s closure under `in.rule`, transitively. Runs nothing; the
// only file-system access is reading the objects and asking whether a file
// exists.
ClosureRead read_closure(const ClosureReadInput& in);

// WHAT A WINDOWS PROGRAM NEEDS BESIDE IT AFTER ITS LINK (SPEC-007 R4.3).
//
// A PE image has no run path: the loader searches the program's directory,
// the system directories and `PATH`, and nothing the program carries. A DLL in
// a runtime search directory therefore serves `mcpp run`, which puts those
// directories on `PATH`, and `mcpp pack`, which stages the closure; a program
// started by hand from the build directory does not find it. The platform's
// own answer is placement: vcpkg's integration copies a program's imported
// DLLs beside it after the link, and CMake names the same set
// `$<TARGET_RUNTIME_DLLS>`.
//
// This is that placement, as one engine mechanism that names no tool. The
// closure is `mcpp pack`'s (`read_closure` under `ClosureRule::Pe`, with its
// system rule), searched in the program's own directory first and then in the
// runtime search directories, in their order. Every resolved DLL outside the
// program's directory is published beside the program through the staging
// primitive, which writes only when the bytes differ. `sources` lists what was
// resolved, for the edge's depfile, so that a DLL replaced in its directory is
// placed again on the next build; `names` lists the DLLs placed, which the
// caller records and passes back as `placedBefore` on the next run, so that a
// copy this function put beside the program is resolved again from its
// directory rather than taken for a file of the program's own; `notes` names
// each DLL that more than one directory offers, with the one the search order
// chose; `warnings` names a DLL this function did NOT place because
// `placedByOthers` already claims that name, together with the difference
// found (SPEC-007 R4.2/R4.3: one destination, one writer, #723).
struct RuntimeDllPlacement {
    std::vector<std::filesystem::path> sources;
    std::vector<std::string>           names;
    std::vector<std::string>           notes;
    std::vector<std::string>           warnings;
};
// `placedByOthers` names the DLLs another writer already put directly beside
// `program` (a declared deploy, or the toolchain's staged runtime; the caller
// determines them from the directory, see cmd_place_dlls): a name in it is
// never written here. When the resolved import differs from what is already
// there, the difference is reported in `warnings` rather than silently kept
// or silently overwritten.
//
// THE MSVC C++ RUNTIME'S NAMES FOLLOW THE CONTRACT, NOT THE SEARCH ORDER
// (mcpp.build.runtime_placement). `crtRule` is the rule the plan's resolver
// applied: under "system" (host-coupled) no copy of the runtime is placed;
// under "carry" the plan already placed the chosen set, and a dependency's
// different copy is the plan's packaging-fault note, not a warning per link;
// a runtime name the plan did not see -- a directory a `prepare` action filled
// -- is decided by the same resolver between `toolsetCrtDir`'s set and that
// directory's.
struct RuntimeCrtRule {
    std::string           policy = "not-applicable";   // carry | system | static | not-applicable
    std::filesystem::path toolsetCrtDir;
};
std::expected<RuntimeDllPlacement, Error>
place_runtime_dlls(const std::filesystem::path& program,
                   const std::vector<std::filesystem::path>& searchDirs,
                   const std::vector<std::string>& placedBefore = {},
                   const std::vector<std::string>& placedByOthers = {},
                   const RuntimeCrtRule& crtRule = {});

// Build a Plan from already-resolved inputs. Caller is expected to have
// already run `mcpp build` (or equivalent) and pass the resulting
// binary path in.
std::expected<Plan, Error>
make_plan(const mcpp::manifest::Manifest& manifest,
          const mcpp::config::GlobalConfig& cfg,
          const Options& opts,
          const std::filesystem::path& builtBinary,
          const std::filesystem::path& projectRoot,
          std::string_view triple,
          // The RESOLVED run-time requirements, from BuildPlan. Not the root
          // manifest's: an application almost never declares a host capability
          // itself, it depends on something that does.
          std::span<const mcpp::manifest::RuntimeRequirement> resolvedRequirements = {},
          // #622 A3/A10: see the field comment on `Plan::programIsSharedObject`.
          bool programIsSharedObject = false);

// Execute the plan: copies binary + .so + extra files, runs patchelf,
// writes the final tarball or directory. The tree is staged (the program,
// then the declared runtime files) before the dependency closure is
// resolved, so a format whose closure mechanism is unavailable here still
// gets a tree — see `ClosureResult` and `closure_unavailable_outcome`.
std::expected<ClosureResult, Error>
run(const Plan& plan, const mcpp::config::GlobalConfig& cfg);

// THE SINGLE FILE (SPEC-014 §5.2), made from the tree `run` staged: for an
// image, the platform's carrier carrying it (or, when it names shared images,
// a stored zip of all of them aligned for mapping), one file per system of
// `Options::systems`; for a native program, the engine's launcher carrying the
// tree as a zstd frame. Returns the files written.
std::expected<std::vector<std::filesystem::path>, Error> single_file(const Plan& plan);

// The row a program of `system` (linux | macos | windows) and `arch` is built
// on -- an image's single file, a self-contained tree's runner -- when this mcpp
// builds no program for it: a row its table states as planned (`x86_64-macos`)
// or does not state (`aarch64-windows-gnu`). Empty when it builds one.
std::string unbuilt_row(std::string_view system, std::string_view arch);

// The systems `--systems` names for an architecture (design 2026-10-29
// §4.1): an `<os>`, an `<os>-<arch>` of that architecture, or every system
// for `all`; this machine's system when it names none.
std::vector<std::string> systems_for(const std::vector<std::string>& stated, std::string_view arch);

// The single files of an image (design 2026-10-29 §4.5): one per leg and
// system of the leg's architecture, the legs in the plan's order. `skipped`
// is why a row `--systems all` reaches is made nowhere (this mcpp builds no
// carrier for it); empty for a row that is made.
struct SingleFileRow { std::string system, arch, skipped; };
std::vector<SingleFileRow> single_file_rows(const Plan& plan);

// `-o` names a directory (design 2026-10-29 §4.2, §4.5): it ends in a
// separator or is an existing directory. A pack writing more than one file
// writes each under it with its default name.
bool output_names_directory(const std::filesystem::path& output);

// The `<os>-<arch>` of the loaders a package carries (`systems` of its
// `mcpp-package.json` and of the `mcpp.pack` envelope, SPEC-014 §4.5): a
// self-contained tree's under `libexec/<runner>/`, and for a
// self-contained package of one image its carrier, which is the program -- of
// this machine's system and the image's architecture. `mcpp emit xpkg` reads
// it as the system the carrier runs on (§8).
std::vector<std::string> carried_systems(const Plan& plan);

// THE PLATFORM'S RUNNER FOR ANOTHER ARCHITECTURE (SPEC-013 §10, §11): the
// carrier built with nothing carried, for this machine's system and `arch`,
// named `name` -- the program a self-contained tree carries under
// `libexec/<name>/<os>-<arch>/`, and the one `mcpp run` and `mcpp test` hand an
// execution host of that architecture. Built under
// `<projectRoot>/target/carrier-pack/<name>-<os>-<arch>/`; returns the program.
// Refused when the platform states no carrier or this mcpp builds no program
// for that system and architecture. `build = false` builds nothing and returns
// where the program is placed (what `mcpp self env` reports).
std::expected<std::filesystem::path, Error>
runner_program(const mcpp::platform::Resolved& platform, const std::filesystem::path& projectRoot,
               std::string_view name, std::string_view arch, bool build = true);

// The tree made a package: signed and described (`mcpp-package.json`). `run`
// does it before writing a container; a dispatched format whose tree was
// staged without its closure is finalized by the caller.
std::expected<void, Error> finalize_staged(const Plan& plan);

// One more engine container (`tar` or `zip`) of a tree already staged and
// finalized, under its default name beside the plan's own (design 2026-10-29
// §4.6): a `--pkg-format` list writes each container from one tree.
std::expected<std::filesystem::path, Error> write_another_container(const Plan& plan, ArchiveKind kind);

// Helpers used by cli.cppm to render mode names + parse `--mode`.
// Canonical name shown in `--help`/diagnostics (renamed for legibility).
std::string_view mode_cli_name(Mode m);
// FROZEN wire-format suffix for tarball filenames. Never rename these —
// install.sh / download URLs depend on them. "" means "no suffix" (default).
std::string_view mode_tarball_suffix(Mode m);
std::optional<Mode> parse_mode(std::string_view s);

} // namespace mcpp::pack

namespace mcpp::pack {

std::string_view mode_cli_name(Mode m) {
    switch (m) {
        case Mode::None:          return "system";
        case Mode::Static:        return "static";
        case Mode::BundleProject: return "vendored";
        case Mode::BundleAll:     return "self-contained";
    }
    return "?";
}

std::string_view mode_tarball_suffix(Mode m) {
    switch (m) {
        case Mode::None:          return "system";      // brand-new mode
        case Mode::Static:        return "static";      // frozen
        case Mode::BundleProject: return "";            // frozen: default → no suffix
        case Mode::BundleAll:     return "bundle-all";  // frozen
    }
    return "";
}

std::optional<Mode> parse_mode(std::string_view s) {
    // Canonical names.
    if (s == "system")         return Mode::None;
    if (s == "vendored")       return Mode::BundleProject;
    if (s == "self-contained") return Mode::BundleAll;
    if (s == "static")         return Mode::Static;
    // Permanent back-compat aliases (old names — keep forever).
    if (s == "bundle-project") return Mode::BundleProject;
    if (s == "bundle-all")     return Mode::BundleAll;
    return std::nullopt;
}

namespace detail {

// Helpers below are kept in a NAMED (non-exported) sub-namespace rather
// than an anonymous one. Anonymous namespaces inside a module become
// TU-local; types declared there can't appear in the signature of any
// function that's referenced from non-anonymous code. GCC 15 + musl's
// libstdc++ flags the resulting `std::vector<TU-local>` /
// `std::expected<…>` instantiations as "exposes TU-local entity"
// errors. Naming the namespace gives every helper module linkage and
// sidesteps the rule entirely.

// Default archive name: `<name>-<version>-<triple>[-<mode>].<ext>`.
// Mode suffix only for non-default modes so adjacent builds of different
// modes don't stomp each other in target/dist/.
//
// The EXTENSION follows the artifact, not the host: a Windows package is a
// `.zip` whoever built it, and a `.tar.gz` full of DLLs is a package most
// Windows users cannot open without installing something first.
std::string default_archive_name(std::string_view name, std::string_view version,
                                 std::string_view triple, Mode mode, ArchiveKind kind,
                                 mcpp::pack::compress::Algorithm compression)
{
    std::string_view ext = kind == ArchiveKind::Zip ? std::string_view(".zip")
                         : mcpp::pack::compress::tar_extension(compression);
    auto sfx = mode_tarball_suffix(mode);
    if (sfx.empty())
        return std::format("{}-{}-{}{}", name, version, triple, ext);
    return std::format("{}-{}-{}-{}{}", name, version, triple, sfx, ext);
}

// Strip the archive suffix to get the canonical wrapper-directory name. The
// result names both the disk staging dir and the top-level entry inside the
// archive — keeping the two in lock-step makes click-to-extract behave the
// way users expect (and on Windows, Explorer's "extract here" too).
std::string wrapper_dirname_from_archive(const std::filesystem::path& archive) {
    return mcpp::pack::distribution::archive_directory(archive);
}

} // namespace detail

bool strips_on_this_row(const Plan& plan) {
    // wasm32-emscripten is packed by `run_wasm`, which has no strip step: the
    // launcher is JavaScript and the module is one static image.
    return plan.strip && plan.stripTools.inBandDebugInfo && !plan.targetIsWasm;
}

bool resolve_strip(const Options& opts, const mcpp::manifest::PackConfig& cfg) {
    if (opts.strip) return *opts.strip;
    if (cfg.strip)  return *cfg.strip;
    return true;
}

std::filesystem::path resolve_debug_dir(const Options& opts,
                                        const mcpp::manifest::PackConfig& cfg,
                                        const std::filesystem::path& projectRoot)
{
    auto raw = !opts.debugSymbols.empty()
        ? opts.debugSymbols
        : std::filesystem::path(cfg.debugSymbols);
    if (raw.empty()) return {};
    return raw.is_absolute() ? raw : projectRoot / raw;
}

std::expected<Plan, Error>
make_plan(const mcpp::manifest::Manifest& manifest,
          const mcpp::config::GlobalConfig& /*cfg*/,
          const Options& opts,
          const std::filesystem::path& builtBinary,
          const std::filesystem::path& projectRoot,
          std::string_view triple,
          std::span<const mcpp::manifest::RuntimeRequirement> resolvedRequirements,
          bool programIsSharedObject)
{
    Plan p;
    p.opts            = opts;
    p.projectRoot     = projectRoot;
    p.builtBinary     = builtBinary;
    p.binaryName      = builtBinary.filename().string();
    p.programIsSharedObject = programIsSharedObject;
    p.packageName     = manifest.package.name;
    p.packageVersion  = manifest.package.version;
    p.triple          = std::string(triple);
    // Resolved graph first; the manifest's own declarations are the fallback
    // for callers that have no plan (and remain covered by the legacy vector).
    p.hostRequirements = resolvedRequirements.empty()
        ? host_requirements_of(manifest.runtimeConfig)
        : host_requirements_of(resolvedRequirements,
                               manifest.runtimeConfig.capabilities);

    // A MODE THAT CARRIES ITS OWN libc CANNOT CONSUME A HOST CAPABILITY.
    //
    // `static` has no libc to share and `self-contained` brings its own, and
    // for a library the target must supply the consequence is identical: that
    // .so arrives with its own requirements on the HOST's libc, and the
    // process does not have that libc. The proprietary graphics stacks are the
    // everyday case — they cannot be bundled (kernel lockstep, redistribution
    // terms), so they are always the host's, and a self-contained bundle meets
    // them with the wrong loader. Measured in both directions as mcpp#392 /
    // mcpp#401: a private glibc meeting host-loaded objects dies during
    // relocation, before main.
    //
    // Today both modes link and then fail at run time, or silently fall back
    // to software rendering — worse than not building. The predicate is
    // DECLARED data, not a list of driver names mcpp would have to maintain
    // and would get wrong.
    if (!p.hostRequirements.empty()
        && (opts.mode == Mode::Static || opts.mode == Mode::BundleAll)) {
        std::string names;
        for (auto const& req : p.hostRequirements) {
            if (!names.empty()) names += ", ";
            names += req.capability;
        }
        return std::unexpected(Error{std::format(
            "--mode {} cannot be used by a program that needs the host to "
            "provide {}.\n"
            "  That capability is satisfied at run time by a library on the "
            "TARGET machine, and it\n"
            "  arrives with its own requirements on the target's libc — which "
            "a bundle carrying its\n"
            "  own libc does not have. The result links and then fails at "
            "startup, or silently\n"
            "  degrades (mcpp#392, mcpp#401).\n"
            "  use: --mode vendored — third-party .so travel with the "
            "artifact; libc and the\n"
            "       capability above both come from the host.",
            mode_cli_name(opts.mode), names)});
    }

    // WHAT THE FILE IS, not what the triple asked for. `--target` states an
    // intention; the artifact on disk is the thing being packaged, and when
    // the two disagree it is the file that has to be believed. (They can
    // disagree for real: `[pack] default_mode = "static"` re-prepares the
    // build with a different target after the first one has already run.)
    p.targetIsPe =
        mcpp::pack::binfmt::identify(builtBinary).format
            == mcpp::pack::binfmt::Format::Pe;
    // The triple, not the file — see the field comment on `targetIsWasm`.
    if (auto t = mcpp::toolchain::triple::parse(p.triple)) {
        p.targetIsWasm =
            t->object_format() == mcpp::toolchain::triple::ObjectFormat::Wasm;
        p.targetIsLoaded = t->loader_supplies_os();
    }

    // THE CONTRACT REACHES PACKAGING. Until now it stopped at the compile and
    // link flags, so the step that decides which files travel could not see
    // what the artifact had promised (design §4.3).
    //
    //   toolchain-coupled  the toolchain's own runtime travels WITH the
    //                      artifact — so its directory joins the search set.
    //   host-coupled       the target provides it — so that directory stays
    //                      OUT, and a vcruntime140.dll sitting in the
    //                      toolset is not silently swept into the package.
    //   self-contained     there is nothing to carry.
    //
    // A mode that bundles NOTHING cannot honour a contract that requires
    // files to travel. `pack` already refuses the mirror-image contradiction
    // (a bundle carrying its own libc cannot consume a host capability), and
    // this grows from the same root: a contract with no executor is a promise
    // the build prints and the package quietly drops.
    const bool modeBundlesNothing =
        opts.mode == Mode::None || opts.mode == Mode::Static;
    if (opts.carryToolchainRuntime && modeBundlesNothing) {
        return std::unexpected(Error{std::format(
            "cxx_runtime = \"toolchain-coupled\" and --mode {} contradict each "
            "other.\n"
            "  The contract says the toolchain's C++ runtime travels WITH this "
            "artifact; --mode {}\n"
            "  bundles nothing, so it would ship a program that cannot start "
            "anywhere the toolchain\n"
            "  is not already installed.\n"
            "  use: --mode vendored (carry it), or cxx_runtime = "
            "\"host-coupled\" / \"self-contained\"\n"
            "       in [build] if the target is expected to provide the runtime "
            "itself.",
            mode_cli_name(opts.mode), mode_cli_name(opts.mode))});
    }

    // The artifact's own directory is always searched: whatever the build
    // staged beside it (the toolchain-coupled CRT, a dependency's DLL) is by
    // definition part of what it runs with.
    p.searchDirs.push_back(builtBinary.parent_path());
    for (auto const& d : opts.depSearchDirs) p.searchDirs.push_back(d);
    if (opts.carryToolchainRuntime)
        for (auto const& d : opts.toolchainRuntimeDirs) p.searchDirs.push_back(d);

    // THE CONTAINER. A format left unstated follows the artifact -- a zip for
    // a Windows program, which its users open without installing anything, a
    // tar elsewhere; `tar` and `zip` stated are what they say (SPEC-014 §1).
    p.archive = opts.format == Format::Zip ? ArchiveKind::Zip
              : opts.format != Format::Tar ? ArchiveKind::None
              : (p.targetIsPe && !opts.formatStated) ? ArchiveKind::Zip
              : ArchiveKind::Tar;
    // A zip deflates or stores: a zstd or xz the format left to the artifact
    // cannot honour is refused when the command stated it, and the project's
    // default leaves it aside (SPEC-014 §5.1).
    if (p.archive == ArchiveKind::Zip
        && (opts.compression.algorithm == mcpp::pack::compress::Algorithm::Zstd
            || opts.compression.algorithm == mcpp::pack::compress::Algorithm::Xz)) {
        if (opts.compressionFromFlag)
            return std::unexpected(Error{std::format(
                "{} is packed as a zip (a Windows program's archive), whose members are deflated "
                "or stored; --compress {} is a tar's.\n  use: --pkg-format tar --compress {}, or "
                "--compress gzip | none", p.binaryName,
                mcpp::pack::compress::name(opts.compression.algorithm),
                mcpp::pack::compress::name(opts.compression.algorithm))});
        p.opts.compression = {};
    }
    // A tree of several legs is named after its platform, not a triple: it is
    // the same package on every system and architecture of the platform.
    const std::string nameTriple = opts.universalName.empty() ? p.triple : opts.universalName;
    auto distDir = projectRoot / "target" / "dist";
    if (opts.output.empty()) {
        p.archivePath = (opts.outputDir.empty() ? distDir : opts.outputDir) / detail::default_archive_name(
            p.packageName, p.packageVersion, nameTriple, opts.mode,
            p.archive == ArchiveKind::None ? ArchiveKind::Tar : p.archive,
            opts.compression.algorithm);
    } else if (!opts.output.has_parent_path()) {
        // `-o name.tar.gz` (bare filename) → place in target/dist/.
        // `-o ./name.tar.gz` or `-o sub/name.tar.gz` → use as-is.
        // `-o /abs/path.tar.gz` → use as-is.
        p.archivePath = distDir / opts.output;
    } else {
        p.archivePath = opts.output;
    }
    // Derive the staging dir from the archive stem so the in-archive
    // wrapper directory and the on-disk staging dir share one name —
    // matches what GUI extractors create on click and what `tar -xzf`
    // produces on the CLI.
    p.stagingRoot = distDir / detail::wrapper_dirname_from_archive(p.archivePath);
    // A single file takes the tree's name in `target/dist`; its tree is
    // staged outside it.
    if (opts.format == Format::SingleFile)
        p.stagingRoot = projectRoot / "target" / "single-file" / "stage" / p.stagingRoot.filename();

    p.includeGlobs    = manifest.packConfig.include;
    p.excludeGlobs    = manifest.packConfig.exclude;
    p.alsoSkipLibs    = manifest.packConfig.alsoSkip;
    p.hostProvidedLibs = opts.hostProvidedLibs;
    p.forceBundleLibs = manifest.packConfig.forceBundle;

    return p;
}

namespace detail {

// A program's output and errors together. Returns the captured text on
// success, or an error message on non-zero exit.
std::expected<std::string, std::string>
run_capture(const std::vector<std::string>& argv, const mcpp::run::Env& env = {}) {
    auto r = mcpp::run::execute(argv, { .env = env });
    if (r.exit_code != 0) return std::unexpected(std::format(
        "command exited with {}: {}", r.exit_code, mcpp::build::command_line::display(argv)));
    return r.output;
}

// A program whose output is not read; its exit code.
int run_silent(const std::vector<std::string>& argv) {
    return mcpp::run::execute(argv, { .streams = mcpp::run::Streams::discard }).exit_code;
}

// ─── ldd parsing + manylinux skip-list ──────────────────────────────
//
// `ldd <bin>` output shapes we handle:
//   "\tlibm.so.6 => /lib/x86_64-linux-gnu/libm.so.6 (0x...)"
//   "\tlinux-vdso.so.1 (0x...)"                         ← skip (vDSO)
//   "\t/lib64/ld-linux-x86-64.so.2 (0x...)"             ← absolute interp line
//   "\tstatically linked"                                ← no deps; bail

struct ResolvedDep {
    std::string             soname;       // basename, e.g. "libcurl.so.4"
    std::filesystem::path   path;         // resolved absolute path
};

// PEP 600 / manylinux2014 standard skip list — these libs are assumed
// to exist on any target Linux glibc system, so Mode BundleProject
// doesn't ship them. Match by SONAME prefix.
constexpr std::array kManyLinuxAllow = std::to_array<std::string_view>({
    "libc.so",
    "libm.so",
    "libdl.so",
    "libpthread.so",
    "librt.so",
    "libutil.so",
    "libnsl.so",
    "libresolv.so",
    "libcrypt.so",
    "libstdc++.so",
    "libgcc_s.so",
    "linux-vdso.so",
    "ld-linux",       // ld-linux-x86-64.so.2 etc.
    "libld-linux",
});

bool is_system_lib(std::string_view soname) {
    for (auto& prefix : kManyLinuxAllow) {
        if (soname.starts_with(prefix)) return true;
    }
    return false;
}

bool soname_matches(std::string_view soname,
                    const std::vector<std::string>& list)
{
    for (auto& pat : list) if (soname == pat || soname.starts_with(pat)) return true;
    return false;
}

// What the loader reported: the names it found a file for, and the names it
// found none for (#634 A3). A name in `notFound` used to be dropped here, so a
// bundle that could not start said `closure = walked`.
struct LddClosure {
    std::vector<ResolvedDep> found;
    std::vector<std::string> notFound;
};

std::expected<LddClosure, std::string>
ldd_parse(const std::filesystem::path& binary)
{
    // Don't shell out to `ldd` directly — many distros (and our own
    // xim:glibc sandbox) ship `ldd` as a shell script that tries to
    // exec the binary. We get the same data by setting
    // LD_TRACE_LOADED_OBJECTS=1 and running the binary; the dynamic
    // linker honours that env var and prints the dep table without
    // executing main(). This routes through the binary's *own*
    // PT_INTERP so it works even when our sandbox's ldd wrapper is
    // broken or missing.
    auto out = run_capture({ binary.string() }, { { "LD_TRACE_LOADED_OBJECTS", "1" } });
    if (!out) return std::unexpected(out.error());

    LddClosure deps;
    std::istringstream is{*out};
    std::string line;
    while (std::getline(is, line)) {
        // Trim leading whitespace.
        std::size_t i = 0;
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        line.erase(0, i);
        if (line.empty()) continue;
        if (line.starts_with("statically linked")) return deps;   // no deps
        // Skip vDSO (no on-disk file).
        if (line.starts_with("linux-vdso")) continue;
        // "<soname> => <path> (0x...)" or "<absolute-path> (0x...)"
        ResolvedDep d;
        if (auto arrow = line.find(" => "); arrow != std::string::npos) {
            d.soname = line.substr(0, arrow);
            auto rest = line.substr(arrow + 4);
            // Trim "(0x...)" tail.
            if (auto paren = rest.find(" ("); paren != std::string::npos)
                rest = rest.substr(0, paren);
            // "not found": mcpp cannot ship a library it cannot see, and the
            // caller decides whether the mode leaves it to the target.
            if (rest == "not found") { deps.notFound.push_back(d.soname); continue; }
            d.path = rest;
        } else if (line.starts_with('/')) {
            // Absolute-path line (typically the dynamic linker itself).
            auto path = line;
            if (auto paren = path.find(" ("); paren != std::string::npos)
                path = path.substr(0, paren);
            d.path   = path;
            d.soname = std::filesystem::path(path).filename().string();
        } else {
            continue;
        }
        deps.found.push_back(std::move(d));
    }
    return deps;
}

// Sandbox-local patchelf path via xlings module. Fail soft if the
// bootstrap step left it uninstalled.
std::filesystem::path
sandbox_patchelf(const mcpp::config::GlobalConfig& cfg) {
    auto env = mcpp::config::make_xlings_env(cfg);
    auto bin = mcpp::provider::open(env)->pinned("patchelf", mcpp::provider::kPatchelfVersion) / "bin" / "patchelf";
    if (std::filesystem::exists(bin)) return bin;
    // Fallback: scan all versions (in case a different version is installed).
    auto root = mcpp::provider::open(env)->versions("patchelf");
    std::error_code ec;
    if (!std::filesystem::exists(root, ec)) return {};
    for (auto& v : std::filesystem::directory_iterator(root, ec)) {
        auto candidate = v.path() / "bin" / "patchelf";
        if (std::filesystem::exists(candidate)) return candidate;
    }
    return {};
}

// Set the search path (so the dynamic linker finds bundled libs in
// <prefix>/lib from anywhere). $ORIGIN is the directory of the object at load
// time, so $ORIGIN/../lib is the bundled lib dir relative to <prefix>/bin/<exe>
// and $ORIGIN is it relative to <prefix>/lib/<soname>.
//
// WHICH TAG. `patchelf --set-rpath` writes DT_RUNPATH by default, and for an
// EXECUTABLE that is wrong: DT_RUNPATH is consulted only for the object
// carrying it, so a packaged program cannot reach its bundled libraries
// through a dlopen() performed on its behalf by something else. That is the
// same defect as the link-time one (mcpp.build.loader_contract), one layer
// later, and it is why this takes the form from the shared contract instead of
// deciding for itself. Libraries keep DT_RUNPATH — forcing DT_RPATH on a
// library pushes its search path into every lookup below it.
std::expected<void, std::string>
set_search_path(const std::filesystem::path& object,
                std::string_view rpath,
                mcpp::build::loader::Form form,
                const std::filesystem::path& patchelf)
{
    if (patchelf.empty() || !std::filesystem::exists(patchelf))
        return std::unexpected("patchelf not available in sandbox");
    std::vector<std::string> argv{ patchelf.string(), "--set-rpath", std::string(rpath) };
    if (auto flag = mcpp::build::loader::patchelf_flag(
            mcpp::build::loader::required_tag(form)))
        argv.emplace_back(*flag);
    argv.push_back(object.string());
    int rc = run_silent(argv);
    if (rc != 0) return std::unexpected(std::format(
        "patchelf --set-rpath failed (exit {}): {}", rc, object.string()));
    return {};
}

// Set PT_INTERP (the absolute path to the dynamic linker baked into the
// ELF header). PT_INTERP doesn't support $ORIGIN — Mode BundleAll uses
// a wrapper script as the entry point instead.
std::expected<void, std::string>
set_interpreter(const std::filesystem::path& binary,
                std::string_view interp,
                const std::filesystem::path& patchelf)
{
    if (patchelf.empty() || !std::filesystem::exists(patchelf))
        return std::unexpected("patchelf not available in sandbox");
    int rc = run_silent({ patchelf.string(), "--set-interpreter", std::string(interp), binary.string() });
    if (rc != 0) return std::unexpected(std::format(
        "patchelf --set-interpreter failed (exit {}): {}", rc, binary.string()));
    return {};
}

// Remove the program's debug information.
//
// dh_strip's line is that a package strips what it built, and the program is
// the first thing this one built. The shared libraries the same graph compiled
// are the rest (`strip_staged_library` below, #649 E5); a library that came out
// of the store or off the host is somebody else's file and is left as shipped.
//
// Shared with `run_pe` deliberately: a MinGW `.exe` carries DWARF in-band just
// like an ELF one, so "does the bundle ship debug info" must not depend on
// which output family it lands in.
std::expected<void, Error>
strip_program(const Plan& plan, const std::filesystem::path& staged)
{
    if (!plan.strip) return {};
    auto r = mcpp::pack::strip_artifact(staged, mcpp::pack::ArtifactShape::Executable,
                                        plan.stripTools, plan.debugDir);
    if (!r) return std::unexpected(Error{r.error()});
    return {};
}

// IS `file` ONE OF `set`? Compared as file-system identity (after `..` and
// symbolic links are resolved), so the loader's spelling of a path and the
// plan's spelling of the same file agree.
bool path_in(const std::filesystem::path& file,
             const std::vector<std::filesystem::path>& set)
{
    std::error_code ec;
    const auto want = std::filesystem::weakly_canonical(file, ec);
    for (auto const& p : set) {
        std::error_code pec;
        if (std::filesystem::weakly_canonical(p, pec) == want) return true;
    }
    return false;
}

// DOES `file` LIE UNDER ONE OF `dirs`?
bool path_under(const std::filesystem::path& file,
                const std::vector<std::filesystem::path>& dirs)
{
    std::error_code ec;
    const auto f = std::filesystem::weakly_canonical(file, ec);
    for (auto const& d : dirs) {
        std::error_code dec;
        const auto rel = f.lexically_relative(std::filesystem::weakly_canonical(d, dec));
        if (!rel.empty() && *rel.begin() != "..") return true;
    }
    return false;
}

// SHOULD THE STAGED COPY OF `source` BE STRIPPED? (#649 E5)
//
// Two answers are yes:
//
//   built by this graph   a `SharedLibrary` link unit of the plan -- compiled
//                         from source here, so dh_strip's rule applies to it
//                         exactly as it applies to the program;
//   the toolchain's own   a runtime found in the directories the row's own
//     runtime             toolchain links from (`libc++_shared.so` from the
//                         NDK). The reason for leaving a store file alone is
//                         not changing a SHARED payload's bytes, and a copy in
//                         the staged tree is not shared. The Android Gradle
//                         plugin strips the same file.
//
// Everything else -- a library from the store, a prebuilt a package deployed,
// a host library -- stays as shipped: a vendor library may be signed, and its
// bytes are its publisher's.
bool strips_staged_copy(const Plan& plan,
                        const std::filesystem::path& source,
                        const std::vector<std::filesystem::path>& graphBuilt)
{
    if (path_in(source, graphBuilt)) return true;
    if (path_under(source, plan.opts.toolchainLibraryDirs)) return true;
    if (plan.opts.carryToolchainRuntime
        && path_under(source, plan.opts.toolchainRuntimeDirs)) return true;
    return false;
}

// Strip the staged copy of a shared library, keeping its exports
// (`--strip-unneeded`, the `SharedLibrary` shape). `--no-strip` reaches it
// through `plan.strip`, and `--debug-symbols` through `debugDir`, as they reach
// the program.
//
// NEVER THE BUILD'S OWN FILE. A member the build already placed where it is
// staged is the same file as its source (see `stage_closure`), and editing it
// would change an output ninja believes is up to date.
std::expected<void, Error>
strip_staged_library(const Plan& plan,
                     const std::filesystem::path& staged,
                     const std::filesystem::path& source,
                     const std::filesystem::path& debugDir)
{
    if (!plan.strip) return {};
    std::error_code ec;
    if (std::filesystem::equivalent(source, staged, ec)) return {};
    // A copy of a read-only store file is read-only too, and a strip tool
    // rewrites the file it is given.
    std::filesystem::permissions(staged, std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::add, ec);
    auto r = mcpp::pack::strip_artifact(staged,
                                        mcpp::pack::ArtifactShape::SharedLibrary,
                                        plan.stripTools, debugDir);
    if (!r) return std::unexpected(Error{r.error()});
    return {};
}

// Bundle all `deps` into <stagingRoot>/lib/<soname>. We dereference any
// symlinks so the bundle is self-contained even if /usr/lib/foo.so → /usr/lib/foo.so.1.
std::expected<void, std::string>
bundle_libs(const std::vector<ResolvedDep>& deps,
            const std::filesystem::path& stagingRoot)
{
    std::error_code ec;
    auto libDir = stagingRoot / "lib";
    std::filesystem::create_directories(libDir, ec);
    for (auto& d : deps) {
        auto target = libDir / d.soname;
        std::filesystem::copy_file(d.path, target,
            std::filesystem::copy_options::overwrite_existing
          | std::filesystem::copy_options::skip_symlinks, ec);
        if (ec) return std::unexpected(std::format(
            "failed to copy {} → {}: {}", d.path.string(), target.string(),
            ec.message()));
    }
    return {};
}

// Write `body` to `path` and chmod +x. Helper for the various entry-point
// scripts we drop into the bundle root.
std::expected<void, std::string>
write_executable_script(const std::filesystem::path& path,
                        std::string_view body)
{
    std::ofstream os(path);
    if (!os) return std::unexpected(std::format(
        "cannot write {}", path.string()));
    os << body;
    os.close();
    std::error_code ec;
    std::filesystem::permissions(path,
        std::filesystem::perms::owner_exec
      | std::filesystem::perms::group_exec
      | std::filesystem::perms::others_exec,
        std::filesystem::perm_options::add, ec);
    if (ec) return std::unexpected(std::format(
        "chmod +x {} failed: {}", path.string(), ec.message()));
    return {};
}

// BundleAll wrapper script: PT_INTERP can't be relative, so we provide a
// shell entry point that invokes the bundled dynamic linker with
// --library-path pointing at the bundled lib dir, then re-execs the
// real binary. AppImage / linuxdeployqt / nix-cc-wrapper all do the
// same trick.
//
// We drop the same script under TWO names at the bundle root:
//   run.sh           generic, distro-package-friendly entry name
//   <binary_name>    program-name entry, so users can `./hello` and have
//                    it Just Work — no need to remember which wrapper.
// Both files are byte-identical; users pick whichever they prefer.
std::expected<void, std::string>
write_bundle_all_wrappers(const std::filesystem::path& stagingRoot,
                          std::string_view binaryName,
                          std::string_view loaderName,
                          bool localeData = false,
                          bool gconvData = false)
{
    // glibc's compiled locales and its character-set converters, when the
    // bundle carries them (openxlings/xlings#621). The C library that runs here
    // is the bundled one, and it reads both from paths compiled into it, which
    // name the machine the payload was installed on. This script is the only
    // entry of the bundle, so exporting the variables here reaches every run;
    // a value the user already set is kept.
    std::string runtimeData;
    if (localeData)
        runtimeData += "LOCPATH=\"${LOCPATH:-$here/lib/locale}\"\nexport LOCPATH\n";
    if (gconvData)
        runtimeData += "GCONV_PATH=\"${GCONV_PATH:-$here/lib/gconv}\"\nexport GCONV_PATH\n";
    auto body = std::format(
        "#!/bin/sh\n"
        "# Auto-generated by `mcpp pack --mode self-contained`. Launches the\n"
        "# bundled binary through the bundled dynamic linker so the package\n"
        "# is fully portable across glibc versions.\n"
        "#\n"
        "# WHY MCPP_BUNDLE_DIR EXISTS -- the trap this launch has\n"
        "#\n"
        "# The ELF spec forbids $ORIGIN in PT_INTERP, so a bundle that carries\n"
        "# its own loader has to be started BY that loader. The kernel then\n"
        "# sets /proc/self/exe to the loader, not to the program, and\n"
        "# /proc/self/cmdline carries --library-path. Every \"find my resources\n"
        "# next to the executable\" path therefore resolves against lib/ --\n"
        "# silently: fonts and assets are simply not found, and a helper binary\n"
        "# shipped alongside the program is not found either.\n"
        "#\n"
        "# This variable is the answer that survives. Resolve against it first\n"
        "# and fall back to /proc/self/exe only when it is unset. Applications\n"
        "# that cannot be changed should use `--mode vendored` instead, where\n"
        "# PT_INTERP is the host loader and /proc/self/exe is correct.\n"
        "here=$(cd \"$(dirname \"$0\")\" && pwd)\n"
        "MCPP_BUNDLE_DIR=\"$here\"\n"
        "export MCPP_BUNDLE_DIR\n"
        "{}"
        "exec \"$here/lib/{}\" --library-path \"$here/lib\" \"$here/bin/{}\" \"$@\"\n",
        runtimeData, loaderName, binaryName);
    if (auto r = write_executable_script(stagingRoot / "run.sh", body); !r) return r;
    if (auto r = write_executable_script(stagingRoot / std::string(binaryName), body); !r) return r;
    return {};
}

// Static / BundleProject entry-point script — the bin/<name> binary can
// run on its own, but typing `./bin/myapp` from the bundle root is
// awkward. Drop a thin wrapper at the root so `./<name>` works straight
// after extraction. We use a shell wrapper rather than a symlink so the
// tarball survives extraction on filesystems where symlinks misbehave
// (network shares, some Windows tooling).
std::expected<void, std::string>
write_topentry_wrapper(const std::filesystem::path& stagingRoot,
                       std::string_view binaryName)
{
    auto body = std::format(
        "#!/bin/sh\n"
        "# Auto-generated by `mcpp pack`. Convenience entry point so\n"
        "# `./{}` from the bundle root runs the program directly.\n"
        "here=$(cd \"$(dirname \"$0\")\" && pwd)\n"
        "exec \"$here/bin/{}\" \"$@\"\n",
        binaryName, binaryName);
    return write_executable_script(stagingRoot / std::string(binaryName), body);
}

// Find the dynamic linker's SONAME in `deps` (something like
// "ld-linux-x86-64.so.2"). Empty when not found, e.g. for static.
std::string find_loader_soname(const std::vector<ResolvedDep>& deps) {
    for (auto& d : deps) {
        if (d.soname.starts_with("ld-linux") || d.soname.starts_with("libld-linux"))
            return d.soname;
    }
    return {};
}

// Walk each bundled .so's parent directory looking for a license file
// (COPYING / LICENSE / LICENSE.txt etc.) and copy it under
// <stagingRoot>/share/licenses/<soname>/. Best-effort: silently skips
// libs without a discoverable license file (the user will see those
// gaps when they audit the bundle).
void collect_licenses(const std::vector<ResolvedDep>& deps,
                      const std::filesystem::path& stagingRoot)
{
    static constexpr std::array kLicenseNames = {
        "LICENSE", "LICENSE.txt", "LICENCE", "COPYING",
        "COPYING.LIB", "COPYRIGHT", "NOTICE",
    };
    std::error_code ec;
    auto outRoot = stagingRoot / "share" / "licenses";
    for (auto& d : deps) {
        auto libDir = std::filesystem::path(d.path).parent_path();
        // Walk up at most 3 levels to catch e.g. /usr/lib/x86_64-linux-gnu
        // → /usr/share/doc/libfoo/copyright (Debian convention).
        for (int up = 0; up < 3; ++up) {
            for (auto& name : kLicenseNames) {
                auto cand = libDir / name;
                if (std::filesystem::exists(cand, ec)) {
                    auto dst = outRoot / d.soname / name;
                    std::filesystem::create_directories(dst.parent_path(), ec);
                    std::filesystem::copy_file(cand, dst,
                        std::filesystem::copy_options::overwrite_existing, ec);
                    goto next;
                }
            }
            libDir = libDir.parent_path();
            if (libDir.empty() || libDir == "/") break;
        }
        next: continue;
    }
}

void copy_if_exists(const std::filesystem::path& src,
                    const std::filesystem::path& dstDir)
{
    std::error_code ec;
    if (!std::filesystem::exists(src, ec)) return;
    std::filesystem::create_directories(dstDir, ec);
    std::filesystem::copy_file(src, dstDir / src.filename(),
        std::filesystem::copy_options::overwrite_existing, ec);
}

// The runtime files the build placed relative to the executable (#615), copied
// to the same relative path beside the staged executable. The build produced
// every one of them, so a missing file is an error naming it rather than a
// bundle that silently lacks it.
std::expected<void, Error>
stage_runtime_files(const Plan& plan, const std::filesystem::path& stagedExeDir,
                    const std::filesystem::path& builtExeDir = {})
{
    // The leg's own build directory: another architecture's leg deployed its
    // own files beside its own artifact.
    const auto builtDir = builtExeDir.empty() ? plan.builtBinary.parent_path() : builtExeDir;
    for (auto const& rel : plan.opts.runtimeFiles) {
        const auto src = builtDir / rel;
        const auto dst = stagedExeDir / rel;
        std::error_code ec;
        if (!std::filesystem::is_regular_file(src, ec))
            return std::unexpected(Error{std::format(
                "runtime file '{}' is not beside the built executable (looked at '{}')",
                rel.generic_string(), src.string())});
        std::filesystem::create_directories(dst.parent_path(), ec);
        std::filesystem::copy_file(src, dst,
            std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) return std::unexpected(Error{std::format(
            "failed to copy {} -> {}: {}", src.string(), dst.string(), ec.message())});
    }
    return {};
}

// Steps 1-3 of `run`'s generic (non-PE, non-wasm, non-shared-object) path:
// wipe and recreate the staging root, copy the program into `bin/`, stage the
// files `[runtime] deploy` / `deploy_files` declared (#615), and copy
// README/LICENSE and the host-requirements file when there is one to write.
//
// PORTABLE AND UNCONDITIONAL. Every one of these is a plain filesystem
// operation; none of them executes the artifact and none of them asks what
// the artifact's FORMAT is. That is exactly why it runs before `run` asks
// whether THIS HOST can walk the artifact's dependency closure (step 4,
// below) — the declared files need no loader, only the discovered ones do.
// Moved out of `run` so the Mach-O and Windows-host branches, which used to
// refuse before any of this ran, can stage the same tree a dispatched format
// receives. See §3 of
// .agents/docs/2026-09-13-630-what-a-framework-still-hits-in-the-engine.md.
//
// Returns the path of the staged binary, `<stagingRoot>/bin/<name>`.
std::expected<std::filesystem::path, Error>
stage_declared(const Plan& plan)
{
    std::error_code ec;
    std::filesystem::remove_all(plan.stagingRoot, ec);
    std::filesystem::create_directories(plan.stagingRoot / "bin", ec);
    if (ec) return std::unexpected(Error{std::format(
        "cannot create staging '{}': {}", plan.stagingRoot.string(), ec.message())});

    auto bundledBinary = plan.stagingRoot / "bin" / plan.binaryName;
    std::filesystem::copy_file(plan.builtBinary, bundledBinary,
        std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) return std::unexpected(Error{std::format(
        "copy binary failed: {}", ec.message())});
    std::filesystem::permissions(bundledBinary,
        std::filesystem::perms::owner_exec
      | std::filesystem::perms::group_exec
      | std::filesystem::perms::others_exec,
        std::filesystem::perm_options::add, ec);

    if (auto r = stage_runtime_files(plan, bundledBinary.parent_path()); !r)
        return std::unexpected(r.error());

    copy_if_exists(plan.projectRoot / "README.md", plan.stagingRoot);
    copy_if_exists(plan.projectRoot / "LICENSE",   plan.stagingRoot);

    // What the TARGET must provide. Only written when there is something to
    // say — an empty file would be read as "nothing is needed", which is a
    // claim, and for most programs the absence of the file is the honest
    // form of it. When it IS written it is load-bearing: a bundle that omits
    // the driver without saying so is a bundle that fails on the user's
    // machine with no way to find out why.
    if (!plan.hostRequirements.empty()) {
        std::ofstream out(plan.stagingRoot / std::filesystem::path(kFileName));
        if (!out) return std::unexpected(Error{std::format(
            "cannot write {} into the bundle", kFileName)});
        out << render(plan.hostRequirements);
        if (!out) return std::unexpected(Error{std::format(
            "failed writing {}", kFileName)});
    }

    return bundledBinary;
}

// What `run` does with a reason the closure could not be resolved, as a
// function of the plan's requested format — see `closure_unavailable_outcome`
// for the two outcomes and why they differ.
//
// `needs` is what a closure that was read but is incomplete did resolve
// (#634 A3); a provider receives those lines with `closure = not-walked`.
std::expected<ClosureResult, Error>
finish_without_closure(const Plan& plan, std::string reason,
                       std::vector<ClosureNeed> needs = {})
{
    if (closure_unavailable_outcome(plan.opts.format)
        == ClosureUnavailableOutcome::StageWithoutClosure)
        return ClosureResult{false, std::move(reason), std::move(needs)};
    return std::unexpected(Error{std::move(reason)});
}

// The refusal text for a closure with unresolved names: one line per name,
// under the object it was read for, then what the row accepts as a way out.
// `hint` is that row's sentence, since what makes a name resolve differs: a
// directory the link declared (Android), the program's run-time search path
// (ELF), an install name the loader resolves beside the program (Mach-O).
std::string unresolved_reason(std::string_view object,
                              const std::vector<std::string>& lines,
                              std::string_view hint)
{
    std::string out = std::format(
        "the dependency closure of '{}' is incomplete, so the staged tree would "
        "not load where it is installed:\n", object);
    for (auto const& l : lines) out += std::format("         {}\n", l);
    out += std::format("       {}\n", hint);
    out += "       A library the target provides is named in "
           "[pack.bundle-project] also_skip.";
    return out;
}

// Copy a read closure's members under `dir`, the directory the object itself
// is staged in, and describe every name it read as a `needs` line whose staged
// path is relative to the staging root (#634 A3). `[pack] also_skip` keeps a
// member out of the tree and states it as the target's, as it always has; it
// is matched against a name's last path component too, so a Mach-O
// `@rpath/libfoo.dylib` answers to `libfoo`. `prefix` leads each refusal line
// (a several-ABI tree names the leg).
std::expected<void, Error>
stage_closure(const Plan& plan, const ClosureRead& read,
              const std::filesystem::path& dir,
              std::vector<ClosureNeed>& needs,
              std::vector<std::string>& unresolvedLines,
              std::string_view prefix,
              std::vector<std::pair<std::filesystem::path, std::filesystem::path>>* copies = nullptr)
{
    auto skipped = [&](const std::string& name) {
        const auto leaf = std::filesystem::path(name).filename().string();
        const auto folded = mcpp::build::runtime_placement::fold(leaf);
        const bool hostProvided = std::ranges::any_of(plan.hostProvidedLibs,
            [&](const std::string& h) { return mcpp::build::runtime_placement::fold(h) == folded; });
        const bool skip = hostProvided
                       || soname_matches(name, plan.alsoSkipLibs)
                       || soname_matches(leaf, plan.alsoSkipLibs);
        const bool force = soname_matches(name, plan.forceBundleLibs)
                        || soname_matches(leaf, plan.forceBundleLibs);
        return skip && !force;
    };
    for (auto const& name : read.platform)
        needs.push_back({name, ClosureNeed::Kind::Platform, {}});
    for (auto const& m : read.members) {
        if (skipped(m.name)) {
            needs.push_back({m.name, ClosureNeed::Kind::Platform, {}});
            continue;
        }
        const auto dst = dir / m.dest;
        std::error_code ec;
        std::filesystem::create_directories(dst.parent_path(), ec);
        // The build may already have placed it where it is staged, in which
        // case source and destination are the same file.
        if (!std::filesystem::equivalent(m.source, dst, ec)) {
            std::error_code cec;
            std::filesystem::copy_file(m.source, dst,
                std::filesystem::copy_options::overwrite_existing, cec);
            if (cec) return std::unexpected(Error{std::format(
                "failed to copy {} -> {}: {}",
                m.source.string(), dst.string(), cec.message())});
        }
        // (staged, source), for the caller that strips what the graph built.
        if (copies) copies->emplace_back(dst, m.source);
        needs.push_back({m.name, ClosureNeed::Kind::Staged,
                         dst.lexically_relative(plan.stagingRoot).generic_string()});
    }
    for (auto const& u : read.unresolved) {
        // Named as the target's: the tree need not carry what this machine
        // does not have.
        if (skipped(u.name)) {
            needs.push_back({u.name, ClosureNeed::Kind::Platform, {}});
            continue;
        }
        needs.push_back({u.name, ClosureNeed::Kind::Unresolved, {}});
        unresolvedLines.push_back(std::format("{}{}: {}", prefix, u.name, u.why));
    }
    return {};
}

std::expected<void, Error>
make_tarball(const std::filesystem::path& stagingRoot,
             const std::filesystem::path& tarballPath,
             mcpp::pack::compress::Choice compression = {})
{
    // Pack with a top-level wrapper directory whose name matches the
    // tarball stem (computed by make_plan via wrapper_dirname_from_tarball).
    // This keeps click-to-extract and `tar -xf` aligned: both surface a
    // single self-contained directory in the user's cwd. The codec is mcpp's
    // own (mcpp.pack.distribution::write_tar).
    if (auto r = mcpp::pack::distribution::write_tar(stagingRoot, tarballPath, compression); !r)
        return std::unexpected(Error{r.error()});
    return {};
}

std::expected<void, Error>
make_zip(const std::filesystem::path& stagingRoot, const std::filesystem::path& archive,
         mcpp::pack::compress::Choice compression, std::string_view program = {})
{
    if (auto r = mcpp::pack::distribution::write_zip(stagingRoot, archive, compression, program); !r)
        return std::unexpected(Error{r.error()});
    return {};
}

// THE TREE MADE A PACKAGE (SPEC-014 §4.5, §7): each program, image, library
// and loader signed by `[pack] sign`, then every file described in
// `mcpp-package.json` with its role, its leg and its digest. Before the
// container, so the archive holds what the document says.
// `[pack] include`: the project's files the globs name, at the same path
// below the tree's root, once whatever the legs (a `share/**` is the
// package's, not an architecture's); `[pack] exclude` drops what it names.
std::expected<std::vector<std::string>, Error> stage_included(const Plan& plan) {
    namespace fs = std::filesystem;
    std::vector<std::string> staged;
    if (plan.includeGlobs.empty()) return staged;
    std::vector<fs::path> files;
    auto excluded = [&](const fs::path& f) {
        return std::ranges::any_of(plan.excludeGlobs, [&](std::string const& g) {
            return mcpp::modgraph::path_matches_glob(f, plan.projectRoot, g);
        });
    };
    auto onFile = [&](const fs::path& f) {
        const bool named = std::ranges::any_of(plan.includeGlobs, [&](std::string const& g) {
            return mcpp::modgraph::path_matches_glob(f, plan.projectRoot, g);
        });
        if (named && !excluded(f)) files.push_back(f);
    };
    mcpp::modgraph::walk_glob_tree(plan.projectRoot, plan.projectRoot,
                                   [](const fs::path&) { return true; }, onFile);
    std::error_code ec;
    for (auto const& f : files) {
        const auto to = plan.stagingRoot / f.lexically_relative(plan.projectRoot);
        fs::create_directories(to.parent_path(), ec);
        fs::copy_file(f, to, fs::copy_options::overwrite_existing, ec);
        if (ec) return std::unexpected(Error{std::format("[pack] include: cannot stage {}: {}",
                                                         f.string(), ec.message())});
        staged.push_back(f.lexically_relative(plan.projectRoot).generic_string());
    }
    return staged;
}

std::expected<void, Error> finalize_tree(const Plan& plan) {
    namespace fs = std::filesystem;
    namespace d = mcpp::pack::distribution;
    std::error_code ec;
    if (!fs::is_directory(plan.stagingRoot, ec)) return {};
    auto included = stage_included(plan);
    if (!included) return std::unexpected(included.error());
    const auto canonical = [](std::string const& t) {
        if (t.empty()) return mcpp::toolchain::triple::host_triple().str();
        auto parsed = mcpp::toolchain::triple::parse(t);
        return parsed ? parsed->str() : t;
    };
    d::Package pkg;
    pkg.name     = plan.packageName;
    pkg.version  = plan.packageVersion;
    if (plan.platform.active) pkg.platform = std::format("{}@{}", plan.platform.package, plan.platform.version);
    pkg.linkage  = plan.linkageName;
    pkg.mode     = std::string(mode_cli_name(plan.opts.mode == Mode::Static && plan.targetIsLoaded
                                             ? Mode::BundleAll : plan.opts.mode));
    pkg.legs.push_back(canonical(plan.triple));
    for (auto const& l : plan.loadedLegs) pkg.legs.push_back(l.triple);
    for (auto const& l : plan.extraSharedLegs) pkg.legs.push_back(l.triple);
    d::Stated stated;
    if (plan.opts.tree) {
        pkg.runner = plan.platform.fields.runner.cmd;
        const auto stem = fs::path(plan.binaryName).stem().string();
        stated.role["bin/" + stem] = "launcher";
        stated.role["bin/" + stem + ".exe"] = "launcher";
        std::vector<std::pair<std::string, std::string>> legs{
            { canonical(plan.triple), [&] { auto t = mcpp::toolchain::triple::parse(plan.triple);
                                            return t ? t->arch : plan.triple; }() } };
        for (auto const& l : plan.loadedLegs) legs.emplace_back(l.triple, l.arch);
        // Every file of `lib/<arch>/` is an image of that leg.
        auto state_leg = [&](std::string const& triple, std::string const& arch) {
            for (auto const& e : fs::directory_iterator(plan.stagingRoot / "lib" / arch, ec)) {
                const auto rel = std::format("lib/{}/{}", arch, e.path().filename().string());
                stated.role[rel] = "image";
                stated.leg[rel]  = triple;
            }
        };
        for (auto const& [triple, arch] : legs) {
            pkg.entry[arch] = std::format("lib/{}/{}", arch, plan.binaryName);
            state_leg(triple, arch);
        }
        pkg.systems = d::loader_systems(plan.stagingRoot);
    } else {
        pkg.program = plan.targetIsPe ? plan.binaryName : "bin/" + plan.binaryName;
        if (plan.targetIsLoaded && plan.opts.mode == Mode::BundleAll)
            pkg.program = "bin/" + fs::path(plan.binaryName).stem().string()
                        + std::string(mcpplibs::os::exe_suffix);
        stated.role[pkg.program] = "program";
        // The carrier that is the program says which system it is a program
        // of; an index entry reads it there (SPEC-014 §8).
        pkg.systems = carried_systems(plan);
    }
    // Where it runs, once (design 2026-10-29 §4.4): every reader of the
    // package -- an index entry, a merge, the summary -- asks here.
    d::compute_runs_on(pkg, plan.platform.active ? plan.platform.fields.runner.hosts : std::vector<std::string>{},
                       plan.platform.active && plan.platform.fields.runner.host == "any");
    // A package of images starts on Windows through its `.exe`; without one
    // it does not run there.
    if (!pkg.runner.empty()
        && !fs::is_regular_file(plan.stagingRoot / "bin" / (fs::path(plan.binaryName).stem().string() + ".exe"), ec)) {
        auto windows = [](std::string const& p) { return p.starts_with("windows-"); };
        std::erase_if(pkg.runsOnSelf, windows);
        for (auto& [r, pairs] : pkg.runsOnWith) std::erase_if(pairs, windows);
        std::erase_if(pkg.runsOnWith, [](auto const& kv) { return kv.second.empty(); });
    }
    // A TREE THAT RUNS NOWHERE IS NOT A PACKAGE (design 2026-10-29 §4.4): it
    // carries no runner a system can start, and the platform names no
    // system of its legs' architectures its runner exists for.
    if (plan.opts.tree && pkg.runsOnSelf.empty() && pkg.runsOnWith.empty()) {
        auto const& runner = plan.platform.fields.runner;
        return std::unexpected(Error{std::format(
            "{} runs on no system: it carries no runner built here{}, and {} names no system of "
            "its architectures its runner `{}` exists for (runner.hosts).\n  use: --mode "
            "self-contained --systems <a system this mcpp builds for>",
            plan.binaryName, plan.opts.mode == Mode::BundleAll ? "" : " (--mode vendored)",
            plan.platform.display(), runner.cmd.empty() ? std::string() : runner.cmd.front())});
    }
    // What `[pack] include` named is the package's resource, whatever its name.
    for (auto const& rel : *included) stated.role[rel] = "resource";
    auto described = d::describe(plan.stagingRoot, pkg, stated);
    if (!described) return std::unexpected(Error{described.error()});
    if (plan.opts.sign.empty()) return {};
    for (auto const& f : described->files) {
        // Every program of `bin/` and `libexec/`, the Windows launcher with them.
        if (f.role != "program" && f.role != "image" && f.role != "library" && f.role != "loader"
            && !(f.role == "launcher" && f.path.ends_with(".exe")))
            continue;
        std::vector<std::string> argv;
        for (auto const& a : plan.opts.sign)
            argv.push_back(a == "{}" ? (plan.stagingRoot / f.path).string() : a);
        int spawnError = 0;
        auto r = mcpp::run::capture_exec(argv, {}, {}, &spawnError);
        if (spawnError != 0 || r.exit_code != 0)
            return std::unexpected(Error{std::format(
                "[pack] sign failed on {} (exit {}): {}\n{}", f.path, r.exit_code,
                mcpp::build::command_line::display(argv), r.output)});
    }
    // The digests of what was signed.
    if (auto again = d::describe(plan.stagingRoot, pkg, stated); !again)
        return std::unexpected(Error{again.error()});
    return {};
}

// The container of a built-in archive format, or nothing for a directory, a
// single file or a dispatched format; the tree is finalized first.
std::expected<void, Error> write_container(const Plan& plan) {
    if (auto r = finalize_tree(plan); !r) return r;
    switch (plan.archive) {
        case ArchiveKind::None: return {};
        case ArchiveKind::Tar:  return make_tarball(plan.stagingRoot, plan.archivePath,
                                                    plan.opts.compression);
        case ArchiveKind::Zip:  return make_zip(plan.stagingRoot, plan.archivePath,
                                                plan.opts.compression, plan.binaryName);
    }
    return {};
}

} // namespace detail

std::expected<RuntimeDllPlacement, Error>
place_runtime_dlls(const std::filesystem::path& program,
                   const std::vector<std::filesystem::path>& searchDirs,
                   const std::vector<std::string>& placedBefore,
                   const std::vector<std::string>& placedByOthers,
                   const RuntimeCrtRule& crtRule)
{
    namespace rp = mcpp::build::runtime_placement;
    const bool crtRuleApplies = crtRule.policy == "carry" || crtRule.policy == "system"
                             || crtRule.policy == "static";
    std::vector<std::filesystem::path> lateCrt;   // runtime names the plan did not see
    const auto programDir = program.parent_path();
    auto same_dir = [](const std::filesystem::path& a, const std::filesystem::path& b) {
        std::error_code ec;
        if (std::filesystem::equivalent(a, b, ec)) return true;
        return a.lexically_normal() == b.lexically_normal();
    };
    // PE names are case-insensitive, as the loader treats them.
    auto lower = [](std::string_view s) {
        std::string l(s);
        std::ranges::transform(l, l.begin(),
                               [](unsigned char c) { return std::tolower(c); });
        return l;
    };
    std::set<std::string> deployedNames;
    for (auto const& n : placedByOthers) deployedNames.insert(lower(n));

    ClosureReadInput in;
    in.object = program;
    in.rule   = ClosureRule::Pe;
    in.searchDirs.push_back(programDir.empty() ? std::filesystem::path(".") : programDir);
    for (auto const& d : searchDirs)
        if (!same_dir(d, in.searchDirs.front())) in.searchDirs.push_back(d);
    // A name the deploy list places is, by the time this edge runs, already
    // sitting in the program's own directory (that edge is an order-only
    // dependency of the link this edge reads). Left alone, the closure
    // resolution below would find that very copy in `searchDirs.front()` and
    // treat the name as already resolved, so the runtime search directories'
    // copy — the one to compare against — would never be looked at. Folding
    // `placedByOthers` into `notInFirstDir` forces resolution from the OTHER
    // search directories instead, exactly as it already does for a name THIS
    // function placed on a previous run.
    in.notInFirstDir = placedBefore;
    in.notInFirstDir.insert(in.notInFirstDir.end(),
                           placedByOthers.begin(), placedByOthers.end());
    const auto read = read_closure(in);

    // The program itself is the one object the caller chose, so a program that
    // cannot be read is an error; `read_closure` reports it as unresolved
    // under the program's own name.
    for (auto const& u : read.unresolved)
        if (u.name == program.filename().string())
            return std::unexpected(Error{std::format(
                "cannot read the imports of '{}': {}", program.string(), u.why)});

    RuntimeDllPlacement out;
    for (auto const& m : read.members) {
        if (same_dir(m.source.parent_path(), in.searchDirs.front())) continue;

        if (crtRuleApplies && rp::is_msvc_crt_name(m.name)) {
            // host-coupled: the system's runtime serves the program.
            if (crtRule.policy == "system") continue;
            // Placed by the plan as part of the chosen set; a dependency's
            // different copy was stated there once, as a packaging fault.
            if (deployedNames.contains(lower(m.name))) continue;
            lateCrt.push_back(m.source);
            continue;
        }

        // SPEC-007 R4.2/R4.3: one destination, one writer. A name the merged
        // deploy list already places beside this program is that list's
        // file, not this mechanism's — `add_deploy`'s content check (`mcpp
        // stage`) is the authority for it. This loop never writes over it; it
        // only compares and, on a real difference, warns.
        if (deployedNames.contains(lower(m.name))) {
            const auto existing = programDir / m.name;
            std::error_code ec;
            if (std::filesystem::is_regular_file(existing, ec)
                && !mcpp::build::stage::same_content(m.source, existing)) {
                out.warnings.push_back(std::format(
                    "'{}' is placed by this project's deploy list; the runtime "
                    "search directories also offer a different '{}', which was "
                    "not used", existing.string(), m.source.string()));
            }
            continue;
        }

        auto staged = mcpp::build::stage::stage_file(m.source, programDir / m.name);
        if (!staged)
            return std::unexpected(Error{std::format(
                "cannot place '{}' beside '{}': {}", m.source.string(),
                program.filename().string(), staged.error().message)});
        out.sources.push_back(m.source);
        out.names.push_back(m.name);
        // Search order decides between two directories that offer one name,
        // and the decision is stated, because the other copy may be the one
        // the author meant.
        std::vector<std::filesystem::path> offering;
        for (std::size_t i = 1; i < in.searchDirs.size(); ++i) {
            std::error_code ec;
            if (std::filesystem::is_regular_file(in.searchDirs[i] / m.name, ec))
                offering.push_back(in.searchDirs[i]);
        }
        if (offering.size() > 1) {
            std::string others;
            for (std::size_t i = 1; i < offering.size(); ++i)
                others += (others.empty() ? "" : ", ") + offering[i].string();
            out.notes.push_back(std::format(
                "'{}' is offered by {} and by {}; the program receives the first, in "
                "runtime search order", m.name, offering.front().string(), others));
        }
    }

    // A runtime name no plan saw: the same resolver the plan used, over the
    // toolset's set and every runtime file of the directories that brought
    // one (a set is judged whole, so the directory's other runtime files take
    // part in the comparison).
    if (!lateCrt.empty()) {
        rp::Input rin;
        rin.crt = crtRule.policy == "carry" ? rp::CrtPolicy::Carry : rp::CrtPolicy::Static;
        rin.versionOf = [](const std::filesystem::path& p) {
            return mcpp::pack::binfmt::pe_file_version(p);
        };
        auto add_dir = [&](const std::filesystem::path& dir, rp::Kind kind) {
            std::vector<std::filesystem::path> files;
            std::error_code ec;
            for (auto const& e : std::filesystem::directory_iterator(dir, ec))
                if (e.is_regular_file(ec) && rp::is_msvc_crt_name(e.path().filename().string()))
                    files.push_back(e.path());
            std::ranges::sort(files);
            for (auto const& f : files)
                rin.candidates.push_back({{f}, std::filesystem::path("bin") / f.filename(), kind});
        };
        if (!crtRule.toolsetCrtDir.empty()) add_dir(crtRule.toolsetCrtDir, rp::Kind::Toolchain);
        std::set<std::filesystem::path> dirs;
        for (auto const& src : lateCrt) dirs.insert(src.parent_path());
        for (auto const& d : dirs) add_dir(d, rp::Kind::Derived);
        auto decision = rp::resolve(rin);
        for (auto const& n : decision.notes) out.notes.push_back(n);
        for (auto const& p : decision.placed) {
            const auto name = p.dest.filename().string();
            if (deployedNames.contains(lower(name))) continue;
            const auto& src = p.sources.front();
            auto staged = mcpp::build::stage::stage_file(src, programDir / name);
            if (!staged)
                return std::unexpected(Error{std::format(
                    "cannot place '{}' beside '{}': {}", src.string(),
                    program.filename().string(), staged.error().message)});
            out.sources.push_back(src);
            out.names.push_back(name);
        }
    }
    return out;
}

ClosureRead read_closure(const ClosureReadInput& in)
{
    namespace bf = mcpp::pack::binfmt;
    ClosureRead out;

    auto is_file = [](const std::filesystem::path& p) {
        std::error_code ec;
        return std::filesystem::is_regular_file(p, ec);
    };
    auto lower = [](std::string_view s) {
        std::string l(s);
        std::ranges::transform(l, l.begin(),
                               [](unsigned char c) { return std::tolower(c); });
        return l;
    };
    auto dir_list = [](const std::vector<std::filesystem::path>& dirs) {
        std::string s;
        for (auto const& d : dirs) s += (s.empty() ? "" : ", ") + d.string();
        return s.empty() ? std::string("(no directory)") : s;
    };

    // Names are unique per process on all three formats: a second request for
    // one is the same library. PE compares them case-insensitively, as its
    // loader does.
    std::set<std::string> seen;

    if (in.rule != ClosureRule::MachO) {
        std::vector<std::filesystem::path> queue{in.object};
        while (!queue.empty()) {
            auto current = queue.back();
            queue.pop_back();
            auto names = bf::needed_names(current);
            if (!names) {
                // The object itself was chosen by the caller, so an unreadable
                // one is reported; a member that cannot be read was already
                // staged and contributes nothing further.
                if (current == in.object)
                    out.unresolved.push_back({current.filename().string(), names.error()});
                continue;
            }
            for (auto const& name : *names) {
                auto key = in.rule == ClosureRule::Pe ? lower(name) : name;
                if (!seen.insert(key).second) continue;
                // `[pack] force_bundle` is the escape hatch, and it has to reach
                // the SYSTEM list too -- on ELF it always did. Shipping a
                // Windows component is normally a broken program rather than a
                // heavier one, so this is a decision a human has to make
                // explicitly; when they have, mcpp does not know better.
                if (in.rule == ClosureRule::Pe
                    && bf::is_system_lib(bf::Format::Pe, name)
                    && !detail::soname_matches(name, in.forceBundle)) {
                    out.platform.push_back(name);
                    continue;
                }
                if (in.rule == ClosureRule::Android
                    && std::ranges::any_of(in.platformDirs, [&](auto const& d) {
                           return is_file(d / name); })) {
                    out.platform.push_back(name);
                    continue;
                }
                std::optional<std::filesystem::path> hit;
                const bool skipFirst = std::ranges::any_of(in.notInFirstDir,
                    [&](const std::string& n) { return lower(n) == lower(name); });
                for (std::size_t d = 0; d < in.searchDirs.size(); ++d) {
                    if (d == 0 && skipFirst) continue;
                    if (is_file(in.searchDirs[d] / name)) { hit = in.searchDirs[d] / name; break; }
                }
                if (hit) {
                    out.members.push_back({name, *hit, std::filesystem::path(name)});
                    // Transitive: a staged library brings its own needs, and a
                    // closure that stops at depth one ships a package that
                    // starts failing one load further in.
                    queue.push_back(*hit);
                    continue;
                }
                if (in.rule == ClosureRule::Pe) {
                    // Found nowhere: the target provides it (see the rule).
                    out.platform.push_back(name);
                    continue;
                }
                out.unresolved.push_back({name, std::format(
                    "found in none of {}", dir_list(in.searchDirs))});
            }
        }
    } else {
        struct Image {
            std::filesystem::path    source;     // on this machine
            std::filesystem::path    stagedDir;  // relative to the program's directory
            std::vector<std::string> names;
            std::vector<std::string> rpaths;
        };
        auto first = bf::macho_needed(in.object, in.arch);
        if (!first) {
            out.unresolved.push_back({in.object.filename().string(), first.error()});
            return out;
        }
        const auto programDir = in.object.parent_path();
        const auto programRpaths = first->rpaths;
        std::set<std::string> seenDest;
        std::vector<Image> queue{
            Image{in.object, {}, std::move(first->names), std::move(first->rpaths)}};

        // `@loader_path/<rest>` -> `<rest>`; nullopt when `s` does not begin
        // with the token followed by a separator.
        auto after = [](std::string_view s, std::string_view token)
            -> std::optional<std::string> {
            if (s.size() > token.size() + 1 && s.starts_with(token)
                && s[token.size()] == '/')
                return std::string(s.substr(token.size() + 1));
            return std::nullopt;
        };
        // An rpath entry that places a library in the carrying image's own
        // directory (`@loader_path`) or the program's (`@executable_path`).
        auto is_exactly = [](std::string_view rp, std::string_view token) {
            return rp == token
                || (rp.size() == token.size() + 1 && rp.starts_with(token)
                    && rp.back() == '/');
        };

        while (!queue.empty()) {
            auto image = std::move(queue.back());
            queue.pop_back();
            const auto imageDir = image.source.parent_path();
            for (auto const& name : image.names) {
                if (bf::is_system_lib(bf::Format::MachO, name)) {
                    if (seen.insert(name).second) out.platform.push_back(name);
                    continue;
                }
                // WHERE THE LOADER FINDS IT ON THIS MACHINE, by dyld's own
                // substitutions (`resolve_macho_names`): the needing image's
                // rpaths first, then the program's, which every image it loads
                // inherits.
                std::optional<std::filesystem::path> source;
                const std::array<std::string, 1> one{name};
                auto own = bf::resolve_macho_names(one, image.rpaths, programDir, imageDir);
                if (!own.front().unresolved) source = own.front().path;
                if (!source && image.source != in.object && name.starts_with("@rpath/")) {
                    auto inherited = bf::resolve_macho_names(one, programRpaths,
                                                             programDir, programDir);
                    if (!inherited.front().unresolved) source = inherited.front().path;
                }
                // `resolve_macho_names` asks only whether a path exists; a
                // member is a file to copy.
                if (source && !is_file(*source)) source.reset();

                // WHERE A COPY BESIDE THE PROGRAM IS WHAT THE LOADER FINDS.
                std::optional<std::filesystem::path> dest;
                std::string why;
                if (auto rest = after(name, "@executable_path")) {
                    dest = std::filesystem::path(*rest).lexically_normal();
                } else if (auto rest = after(name, "@loader_path")) {
                    dest = (image.stagedDir / *rest).lexically_normal();
                } else if (auto rest = after(name, "@rpath")) {
                    for (auto const& rp : image.rpaths) {
                        if (is_exactly(rp, "@loader_path"))
                            { dest = (image.stagedDir / *rest).lexically_normal(); break; }
                        if (is_exactly(rp, "@executable_path"))
                            { dest = std::filesystem::path(*rest).lexically_normal(); break; }
                    }
                    if (!dest && image.source != in.object) {
                        for (auto const& rp : programRpaths) {
                            if (is_exactly(rp, "@loader_path") || is_exactly(rp, "@executable_path"))
                                { dest = std::filesystem::path(*rest).lexically_normal(); break; }
                        }
                    }
                    if (!dest)
                        why = "no rpath of the image that needs it is exactly "
                              "@loader_path or @executable_path, so a copy beside "
                              "the program is not where the loader looks";
                } else if (name.starts_with("/")) {
                    why = "an absolute install name outside /usr/lib and "
                          "/System/Library, which the loader reads from that path "
                          "rather than from the staged tree";
                } else {
                    why = "an install name with no loader token";
                }
                if (dest && (dest->empty() || dest->begin()->string() == "..")) {
                    dest.reset();
                    why = "it names a location outside the program's directory";
                }
                if (dest && !source)
                    why = "the loader finds no file for it on this machine";

                if (dest && source) {
                    if (!seenDest.insert(dest->generic_string()).second) continue;
                    out.members.push_back({name, *source, *dest});
                    if (auto sub = bf::macho_needed(*source, in.arch)) {
                        queue.push_back(Image{*source, dest->parent_path(),
                                              std::move(sub->names),
                                              std::move(sub->rpaths)});
                    }
                    continue;
                }
                if (seen.insert(name).second) out.unresolved.push_back({name, why});
            }
        }
    }

    std::ranges::sort(out.members, [](const ClosureMember& a, const ClosureMember& b) {
        return a.dest.generic_string() < b.dest.generic_string();
    });
    std::ranges::sort(out.platform);
    out.platform.erase(std::unique(out.platform.begin(), out.platform.end()),
                       out.platform.end());
    std::ranges::sort(out.unresolved, [](const ClosureUnresolved& a,
                                         const ClosureUnresolved& b) {
        return a.name < b.name;
    });
    return out;
}

namespace detail {

// The PE half of `run`. Flat layout, deliberately: the Win32 loader resolves
// a DLL from the directory of the executable, so `bin/` + `lib/` would need a
// mechanism PE does not have. Extract-and-double-click is also what a Windows
// user expects, and it is what the earlier design already specified
// (.agents/docs/2026-05-19-pack-windows-design.md).
//
// No patchelf step and no wrapper script: "put the DLLs next to the exe" IS
// the relocation rule on this format, which is why the row for it in the
// design's layering table reads "no operation".
// THIS FUNCTION ONCE CRASHED THE COMPILER, and the shape it crashed on is
// worth not reintroducing.
//
// clang 20.1.7 targeting the MSVC ABI — the pinned Windows toolchain —
// segfaulted (0xC0000005) while compiling this module interface, with no
// diagnostic, on every Windows job at once. The first version carried three
// module-boundary constructs that the rest of this file does not:
//
//   1. a scoped enum from ANOTHER module as a defaulted member of an
//      EXPORTED struct (`dist::Contract cxxRuntime = …` in `Options`)
//   2. a ranges projection naming a member of an IMPORTED type
//      (`std::ranges::sort(entries, {}, &zip::Entry::name)`)
//   3. `std::span<const Entry>` across the module boundary into zip::write
//
// All three were removed together, so WHICH one it was is not established —
// stating otherwise would be a guess dressed as a finding. What is
// established: the crash is reproducible only on that toolchain, and this
// file has a documented history of the same class (see the `detail`
// namespace comment above, and hostflags.cppm on a neighbouring function
// being miscompiled by an unrelated addition). Each replacement is also
// simpler than what it replaced, so nothing is being paid for the avoidance.
std::expected<std::vector<ClosureNeed>, Error>
run_pe(const Plan& plan)
{
    std::error_code ec;
    std::filesystem::remove_all(plan.stagingRoot, ec);
    std::filesystem::create_directories(plan.stagingRoot, ec);
    if (ec) return std::unexpected(Error{std::format(
        "cannot create staging '{}': {}", plan.stagingRoot.string(), ec.message())});

    auto stagedExe = plan.stagingRoot / plan.binaryName;
    std::filesystem::copy_file(plan.builtBinary, stagedExe,
        std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) return std::unexpected(Error{std::format(
        "copy binary failed: {}", ec.message())});
    if (auto r = stage_runtime_files(plan, stagedExe.parent_path()); !r)
        return std::unexpected(r.error());

    copy_if_exists(plan.projectRoot / "README.md", plan.stagingRoot);
    copy_if_exists(plan.projectRoot / "LICENSE",   plan.stagingRoot);
    if (!plan.hostRequirements.empty()) {
        std::ofstream out(plan.stagingRoot / std::filesystem::path(kFileName));
        if (!out) return std::unexpected(Error{std::format(
            "cannot write {} into the bundle", kFileName)});
        out << render(plan.hostRequirements);
    }

    std::vector<ClosureNeed> needs;
    std::vector<std::pair<std::filesystem::path, std::filesystem::path>> copies;
    if (plan.opts.mode != Mode::None && plan.opts.mode != Mode::Static) {
        // `vendored` and `self-contained` collect the same set here, and that
        // is a property of the PLATFORM rather than a simplification.
        // `self-contained` on ELF means "ship the loader too"; on PE there is
        // no loader to ship and kernel32.dll and friends may not be
        // redistributed — a process that loaded a private copy of one would
        // have two of something that must be unique. So the ceiling on
        // "everything" is the same for both modes: every dependency mcpp is
        // ALLOWED to carry.
        ClosureReadInput in;
        in.object      = stagedExe;
        in.rule        = ClosureRule::Pe;
        in.searchDirs  = plan.searchDirs;
        in.forceBundle = plan.forceBundleLibs;
        const auto read = read_closure(in);
        // PE's rule resolves every name (see `ClosureRule`); what can remain
        // is an executable the reader could not parse, which no format can
        // package.
        std::vector<std::string> unresolved;
        if (auto r = stage_closure(plan, read, plan.stagingRoot, needs, unresolved, {},
                                   &copies); !r)
            return std::unexpected(r.error());
        if (!unresolved.empty())
            return std::unexpected(Error{unresolved_reason(plan.binaryName, unresolved,
                "The executable's import table could not be read.")});
    }

    if (auto r = strip_program(plan, stagedExe); !r) return std::unexpected(r.error());
    // #649 E5: and the DLLs this graph built (a MinGW DLL carries its DWARF
    // in-band as the `.exe` does), plus the toolchain runtime it carries.
    for (auto const& [staged, source] : copies) {
        if (!strips_staged_copy(plan, source, plan.graphSharedLibraries)) continue;
        if (auto r = strip_staged_library(plan, staged, source, plan.debugDir); !r)
            return std::unexpected(r.error());
    }

    // The container: a zip unless `tar` was stated (`write_container`).
    // Plain `std::sort` with an explicit comparator there, not a ranges
    // projection onto an imported type's member: that shape was one of three
    // that crashed the clang 20.1.7 frontend (0xC0000005, no diagnostic) on
    // every Windows job. See the note at the top of run_pe.
    if (auto r = write_container(plan); !r) return std::unexpected(r.error());
    return needs;
}

// The wasm32-emscripten half of `run`. No dependency closure: the ordinary
// Emscripten link produces one static image with everything embedded (a
// `shared` target on this row is refused at plan time, before packaging ever
// sees it — see prepare.cppm), so there is nothing here to trace under a
// dynamic linker the way the PE and ELF halves do.
//
// THE STEM FAMILY RULE. The launcher (`<name>.js`) is the executable; the
// family is every other `<name>.<anything>` the link wrote beside it. `.wasm`
// is required — the link edge declares it as an implicit output
// (ninja_backend.cppm), so its absence names a build directory that does not
// match the graph rather than a program that legitimately has none. A
// `.data` (from `--preload-file`), a `.worker.js` or a `.wasm.map` are
// optional and travel exactly when the link wrote them: this packer stages
// whatever it finds, and asks the link, never a fixed extension list.
std::expected<void, Error>
run_wasm(const Plan& plan)
{
    std::error_code ec;
    std::filesystem::remove_all(plan.stagingRoot, ec);
    std::filesystem::create_directories(plan.stagingRoot / "bin", ec);
    if (ec) return std::unexpected(Error{std::format(
        "cannot create staging '{}': {}", plan.stagingRoot.string(), ec.message())});

    auto stagedJs = plan.stagingRoot / "bin" / plan.binaryName;
    std::filesystem::copy_file(plan.builtBinary, stagedJs,
        std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) return std::unexpected(Error{std::format(
        "copy launcher failed: {}", ec.message())});

    const auto builtDir = plan.builtBinary.parent_path();
    auto family = mcpp::pack::emscripten_stem_family(builtDir, plan.binaryName);
    if (!family.hasWasm) {
        const auto stem = plan.builtBinary.stem().string();
        return std::unexpected(Error{std::format(
            "'{}.wasm' is missing beside the built launcher '{}' -- the "
            "wasm32-emscripten link declares it as an implicit output, so its "
            "absence means the build directory does not match the graph",
            stem, plan.builtBinary.string())});
    }
    for (auto const& name : family.siblings) {
        std::error_code fec;
        std::filesystem::copy_file(builtDir / name,
            plan.stagingRoot / "bin" / name,
            std::filesystem::copy_options::overwrite_existing, fec);
        if (fec) return std::unexpected(Error{std::format(
            "failed to copy {} -> {}: {}", (builtDir / name).string(),
            (plan.stagingRoot / "bin" / name).string(), fec.message())});
    }

    if (auto r = stage_runtime_files(plan, stagedJs.parent_path()); !r) return r;

    copy_if_exists(plan.projectRoot / "README.md", plan.stagingRoot);
    copy_if_exists(plan.projectRoot / "LICENSE",   plan.stagingRoot);

    return write_container(plan);
}

// AN ARTIFACT A LOADER RUNS (an ok image, `<arch>-openkal`; design
// 2026-10-26 part 2, §8.2 and §8.5; SPEC-013 §4.7).
//
// The modes keep their meaning -- what the machine that runs the package must
// provide -- and on such a row they are:
//
//   system          the artifact alone; the machine provides the loader and
//                   every artifact it names (M2);
//   vendored        the artifact and the artifacts it names (`DT_NEEDED`),
//                   beside it; the machine provides the loader (M2, the default);
//   self-contained  a program of THIS machine's system and the artifact's
//                   architecture that carries the artifact as a
//                   section and runs it through the platform's CARRIER (M3),
//                   with the artifacts it names beside it; the machine provides
//                   nothing. The artifact is not recompiled: the program is the
//                   carrier package, the carrier's entry source and the bytes,
//                   built by mcpp with that system's own conventions -- the
//                   bootstrap boundary (§6.2), so no platform is selected for it.
//
// THE TREE (SPEC-014 §3, §4.2) is the one layout of an image of a platform
// that states a runner, whatever the number of legs: one `lib/<arch>/` per
// leg, launchers in `bin/` that pick the machine's architecture and start the
// platform's runner, and under `self-contained` the runner's program for each
// system of `--systems` and each architecture of the legs -- the carrier built
// with nothing carried, which runs the image its first argument names --
// under `libexec/<runner>/<os>-<arch>/`.
//
// `--linkage static` is a property of the image (no `.okso`), and `--mode
// static` is `--linkage static --mode self-contained`.
namespace {

// A reference as a dependency entry of the generated manifest.
std::string dependency_entry(const mcpp::manifest::platform::PackageRef& r) {
    auto const& sp = r.spec;
    const std::string ns = sp.namespace_.empty() ? std::string("mcpplibs") : sp.namespace_;
    std::string value;
    if (!sp.path.empty())
        value = std::format("{{ path = \"{}\" }}", sp.path);
    else if (!sp.git.empty())
        value = std::format("{{ git = \"{}\", {} = \"{}\" }}", sp.git,
                            sp.gitRefKind.empty() ? "rev" : sp.gitRefKind, sp.gitRev);
    else
        value = std::format("\"{}\"", sp.version);
    return std::format("[dependencies.{}]\n{} = {}\n", ns, sp.shortName, value);
}

// The system names a package speaks of, and this machine's.
constexpr std::string_view host_system() {
    if constexpr (mcpplibs::os::is_windows) return "windows";
    else if constexpr (mcpplibs::os::is_macos) return "macos";
    else return "linux";
}

// The triple a program of `system` on `arch` is built for, and the mode it
// is packed in: a Linux program is static (musl), the one form that runs on
// every distribution; a program of this machine's system and architecture on
// macOS or Windows takes this machine's own row; another system's is its
// conventional row. `single`: the program is the whole package (a single
// file), so on Windows it takes its C++ run-time from the system.
struct ProgramRow { std::string triple; std::string mode; std::string platform; };

// The platform a generated program of `triple` is built over, as `--platform`
// names it: the target's platform where its hosted arrangement serves the
// triple (SPEC-013 §5.4), else none. A Linux program over the hosted
// arrangement is static on every architecture, its C library and C++ run-time
// built by the graph with the platform's toolchain; the musl row's own payload
// is a cross GCC published for an x86_64 host only.
std::string hosted_platform_for(const mcpp::platform::Resolved* P, std::string_view triple) {
    if (!P || !P->active || !P->fields.hosted.set) return {};
    auto t = mcpp::toolchain::triple::parse(triple);
    if (!t) return {};
    auto const& targets = P->fields.hosted.targets;
    const bool listed = !targets.set || std::ranges::any_of(targets.value, [&](std::string const& x) {
        auto h = mcpp::toolchain::triple::parse(x);
        return h && h->str() == t->str();
    });
    if (!listed) return {};
    if (!P->selection.ref.spec.path.empty()) return "path:" + P->root.generic_string();
    return std::format("{}@{}", P->package, P->version);
}

ProgramRow program_row(std::string_view system, std::string_view arch, bool single,
                       const mcpp::platform::Resolved* P = nullptr) {
    if (system == "linux") {
        const auto triple = std::format("{}-linux-musl", arch);
        return { triple, "static", hosted_platform_for(P, triple) };
    }
    // Another system's program is built over the platform's hosted arrangement
    // where it serves the row, as Linux's is: the same program whichever
    // machine packs it, needing no SDK and no C run-time of that system.
    // Measured without it: on Linux an aarch64-macos runner needed the macOS
    // SDK no payload carries, and a runner packed on Windows took the MSVC
    // row and VCRUNTIME140.dll with it.
    const auto triple = std::format("{}-{}", arch, system == "macos" ? "macos" : "windows-gnu");
    const auto mode = system == "macos" || !single ? "vendored" : "system";
    if (auto hosted = hosted_platform_for(P, triple); !hosted.empty()) return { triple, mode, hosted };
    const bool here = system == host_system() && arch == mcpp::toolchain::platform::host_arch;
    return { here ? std::string() : triple, mode };
}

// The bytes of `carried` as a section of a program of `system`, with the
// symbols the carrier's entry reads; `name` and `digest` as strings. An empty
// `carried` places nothing: `mcpp_carried_begin == mcpp_carried_end`.
std::string carried_assembly(std::string_view system, std::string_view sectionName,
                             const std::filesystem::path& carried, std::string_view name,
                             std::string_view digest) {
    const bool macho = system == "macos";
    const bool coff  = system == "windows";
    const std::string sym = macho ? "_" : "";
    std::string bare(sectionName);
    while (!bare.empty() && bare.front() == '.') bare.erase(bare.begin());
    std::string upper = bare;
    for (auto& ch : upper) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    const std::string section = macho ? std::format("\t.section __{},__carried\n", upper.substr(0, 14))
                              : coff  ? std::format("\t.section .{},\"dr\"\n", bare)
                                      : std::format("\t.section .{},\"a\",@progbits\n", bare);
    std::string text = std::format(
        "# What the program carries, unchanged, and its name (generated by mcpp pack).\n{}"
        "\t.p2align 4\n\t.globl {}mcpp_carried_begin\n{}mcpp_carried_begin:\n",
        section, sym, sym);
    if (!carried.empty())
        text += std::format("\t.incbin \"{}\"\n", std::filesystem::absolute(carried).generic_string());
    text += std::format(
        "\t.globl {0}mcpp_carried_end\n{0}mcpp_carried_end:\n"
        "\t.globl {0}mcpp_carried_name\n{0}mcpp_carried_name:\n\t.asciz \"{1}\"\n"
        "\t.globl {0}mcpp_carried_digest\n{0}mcpp_carried_digest:\n\t.asciz \"{2}\"\n",
        sym, name, digest);
    return text;
}

// Where a program generated under `dir` is packed: the tree, and the program
// in it -- under the tree's `bin/`, or, for a Windows program, whose tree is
// flat, at its root.
std::filesystem::path packed_tree(const std::filesystem::path& dir) {
    return dir / "target" / "dist" / "packed";
}
std::filesystem::path packed_program(const std::filesystem::path& dir, std::string_view stem,
                                     std::string_view system) {
    if (system == "windows") return packed_tree(dir) / (std::string(stem) + ".exe");
    return packed_tree(dir) / "bin" / std::string(stem);
}

// The generated project of the carrier named `stem` for `system` and `arch`.
std::filesystem::path carrier_dir(const std::filesystem::path& projectRoot, std::string_view stem,
                                  std::string_view system, std::string_view arch) {
    return projectRoot / "target" / "carrier-pack" / std::format("{}-{}-{}", stem, system, arch);
}

// A program generated under `dir` (its manifest, entry and carried section)
// packed for `row` as a directory; returns the directory that holds the
// program and what it needs beside it: the tree's `bin/`, or, for a Windows
// program, whose tree is flat, the tree itself.
std::expected<std::filesystem::path, Error>
pack_generated_program(const std::filesystem::path& dir, const ProgramRow& row) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const auto out = packed_tree(dir);
    fs::remove_all(out, ec);
    std::vector<std::string> argv{ mcpp::invocation::self_exe_path().string(), "pack",
                                   "--pkg-format", "dir", "-o", "packed",
                                   "--platform", row.platform.empty() ? std::string("none") : row.platform,
                                   "--mode", row.mode };
    if (!row.triple.empty()) argv.insert(argv.end(), { "--target", row.triple });
    int spawnError = 0;
    auto built = mcpp::run::capture_exec(argv, {}, dir.string(), &spawnError);
    if (spawnError != 0 || built.exit_code != 0)
        return std::unexpected(Error{std::format(
            "packing the program under {} failed:\n{}", dir.string(), built.output)});
    if (fs::is_directory(out / "bin", ec)) return out / "bin";
    // A flat tree (PE) holds the generated program's own description beside
    // it; what is placed from here is the program, and the package it is
    // placed into describes itself.
    if (fs::is_directory(out, ec)) {
        fs::remove(out / "mcpp-package.json", ec);
        return out;
    }
    return std::unexpected(Error{std::format("the program's package under {} was not written",
                                             out.string())});
}

// THE STATIC ROW'S TOOLCHAIN, STATED: the carrier's own graph supplies a
// kernel interface, which would otherwise cancel the row's convention and
// leave a compiler without a musl C runtime to link with (L10, measured: clang
// took the host GCC's `crtbeginT.o`). The row's own pin on this machine's
// architecture; for another architecture's row the platform's toolchain,
// which every host carries (the row's cross GCC is published for an x86_64
// host only: measured, `x86_64-linux-musl-gcc@16.1.0` has no build for an
// aarch64 Linux host).
std::string static_row_pin(const ProgramRow& row, std::string_view platformToolchain = {}) {
    if (!row.triple.ends_with("-linux-musl") || !row.platform.empty()) return {};
    auto t = mcpp::toolchain::triple::parse(row.triple);
    if (!t) return {};
    std::string pin;
    if (t->arch != mcpp::toolchain::platform::host_arch && !platformToolchain.empty())
        pin = std::string(platformToolchain);
    else if (auto* k = mcpp::toolchain::triple::find_known_target(*t); k && !k->pin.empty())
        pin = std::string(k->pin);
    if (pin.empty()) return {};
    return std::format("\n[target.{}]\ntoolchain = \"{}\"\n", row.triple, pin);
}

// Whether this mcpp builds programs for `row`: a target its table does not
// state (`aarch64-windows-gnu`), or states as planned (`x86_64-macos`, its
// toolchain not published), is not built.
bool row_is_built(const ProgramRow& row) {
    if (row.triple.empty()) return true;   // this machine's own row
    auto t = mcpp::toolchain::triple::parse(row.triple);
    if (!t) return false;
    auto* k = mcpp::toolchain::triple::find_known_target(*t);
    return k && k->tier != "planned";
}

} // namespace

// What a carrier carries and for which system it is built.
struct CarrierBuild {
    std::filesystem::path carried;       // empty: nothing (the runner's form)
    std::string           carriedName;   // the artifact's name, as the carrier reports it
    std::string           stem;          // the program's name
    std::string           system;        // linux | macos | windows
    std::string           arch;
    bool                  single = false;
    // What the runner's form is for, as the `Packing` line says it.
    std::string_view      role = "the runner of the package";
};

// A carrier for a row this mcpp builds no program for, refused before a
// project is generated for it and naming the row: a Windows machine packing an
// aarch64 image's carrier (`aarch64-windows-gnu`) otherwise failed inside the
// generated project's pack. nullopt: the row is built.
std::optional<std::string> unbuilt_carrier(const mcpp::platform::Resolved& platform,
                                           std::string_view system, std::string_view arch,
                                           bool single) {
    const auto row = program_row(system, arch, single, &platform);
    if (row_is_built(row)) return std::nullopt;
    return std::format("{}'s carrier is built as a program of {}-{} ({}), and this mcpp builds "
                       "no program for that row", platform.display(), system, arch, row.triple);
}

std::expected<std::filesystem::path, Error>
carrier_build(const Plan& plan, const CarrierBuild& b)
{
    namespace fs = std::filesystem;
    // The carrier entry serving this program's system and architecture
    // (design 2026-10-29 §4.5).
    const auto* entry = plan.platform.active
        ? mcpp::manifest::platform::carrier_for(plan.platform.fields, std::format("{}-{}", b.system, b.arch))
        : nullptr;
    if (!entry)
        return std::unexpected(Error{std::format(
            "--mode self-contained of {} builds a program of {}-{} over its platform's carrier, and "
            "{} states none for it ([platform] carrier = {{ package, section, main, hosts }}).",
            plan.binaryName, b.system, b.arch, plan.platform.display())});
    auto const& c = *entry;
    std::error_code ec;
    if (c.main.empty() || !fs::is_regular_file(c.main, ec))
        return std::unexpected(Error{std::format(
            "the carrier of {} names its entry source {}, which is not a file ({})",
            plan.platform.display(), c.main.empty() ? std::string("(none)") : c.main,
            mcpp::platform::origin_text(c.origin))});

    const auto row = program_row(b.system, b.arch, b.single, &plan.platform);
    if (auto why = unbuilt_carrier(plan.platform, b.system, b.arch, b.single))
        return std::unexpected(Error{*why});
    const auto dir = carrier_dir(plan.projectRoot, b.stem, b.system, b.arch);
    fs::create_directories(dir / "src", ec);
    if (ec) return std::unexpected(Error{std::format("cannot create {}: {}", dir.string(), ec.message())});
    // RESOLVED AS THE BUILD THAT MADE THE ARTIFACT RESOLVED ITS PLATFORM. The
    // generated project is this pack's, not a project of its own: a lock it
    // kept from an earlier pack held the carrier at that pack's commit while
    // the platform had moved on.
    fs::remove(dir / "mcpp.lock", ec);

    auto write = [&](const fs::path& p, std::string_view text) -> bool {
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        out << text;
        return bool(out);
    };
    const std::string manifest = std::format(
        "# Generated by `mcpp pack` for {}: a program of {}-{} that {} through {}'s\n"
        "# carrier (SPEC-013 §11, SPEC-014 §4.2).\n"
        "[package]\nname    = \"{}\"\nversion = \"{}\"\n\n"
        "[targets.{}]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n\n"
        "[build]\nsources = [\"src/main.cpp\", \"src/carried.S\"]\n\n"
        "{}{}",
        plan.binaryName, b.system, b.arch,
        b.carried.empty() ? "runs the image it is given" : "carries it and runs it",
        plan.platform.display(), b.stem,
        plan.packageVersion.empty() ? "0.0.0" : plan.packageVersion, b.stem,
        dependency_entry(c.package),
        static_row_pin(row, plan.platform.fields.toolchain.set ? plan.platform.fields.toolchain.value
                                                               : std::string()));
    std::string mainText;
    {
        std::ifstream in(c.main, std::ios::binary);
        mainText.assign(std::istreambuf_iterator<char>(in), {});
    }
    if (!write(dir / "mcpp.toml", manifest)
        || !write(dir / "src" / "carried.S",
                  carried_assembly(b.system, c.section, b.carried, b.carriedName, {}))
        || !write(dir / "src" / "main.cpp", mainText))
        return std::unexpected(Error{std::format("cannot write the program's sources under {}", dir.string())});

    // PACKED, NOT MERELY BUILT: the program obeys every rule this command
    // keeps for a program -- no build-machine path, its closure beside it. On
    // macOS and Windows the system's own libraries are the only ones it names.
    mcpp::ui::info("Packing", b.carried.empty()
        ? std::format("{} for {}-{} ({}'s carrier, {})", b.stem,
                      b.system, b.arch, plan.platform.display(), b.role)
        : std::format("{} into a program of {}-{} ({}'s carrier, M3)", plan.binaryName,
                      b.system, b.arch, plan.platform.display()));
    return pack_generated_program(dir, row);
}

namespace {

// The images an image names (`DT_NEEDED`), found where its build placed them.
std::expected<std::vector<std::filesystem::path>, Error>
needed_images(const std::filesystem::path& artifact, std::string_view binaryName) {
    namespace fs = std::filesystem;
    std::vector<fs::path> needed;
    auto facts = mcpp::runtime::elf::inspect_elf_runtime(artifact);
    if (!facts) return std::unexpected(Error{facts.error()});
    const auto builtDir = artifact.parent_path();
    std::error_code ec;
    for (auto const& name : facts->needed) {
        fs::path hit;
        for (auto const& d : { builtDir, builtDir.parent_path() / "lib" })
            if (fs::is_regular_file(d / name, ec)) { hit = d / name; break; }
        if (hit.empty()) return std::unexpected(Error{std::format(
            "{} names the image '{}', which the build did not place beside it", binaryName, name)});
        needed.push_back(hit);
    }
    return needed;
}

std::string arch_of(std::string_view triple) {
    auto t = mcpp::toolchain::triple::parse(triple);
    return t ? t->arch : std::string(triple.substr(0, triple.find('-')));
}

} // namespace

std::expected<void, Error>
run_loaded(const Plan& plan)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::remove_all(plan.stagingRoot, ec);
    fs::create_directories(plan.stagingRoot / "bin", ec);
    if (ec) return std::unexpected(Error{std::format(
        "cannot create staging '{}': {}", plan.stagingRoot.string(), ec.message())});
    auto copy = [&](const fs::path& from, const fs::path& to) -> std::expected<void, Error> {
        fs::create_directories(to.parent_path(), ec);
        fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
        if (ec) return std::unexpected(Error{std::format("copy {} failed: {}", from.string(), ec.message())});
        return {};
    };
    const bool bundles = plan.opts.mode != Mode::None;

    if (!plan.opts.tree) {
        // NO TREE (a single file's staging, or a platform that states no
        // runner): the image in `bin/`, the images it names beside it, or
        // under `self-contained` the carrier that carries it (M3).
        std::vector<fs::path> needed;
        if (bundles) {
            auto n = needed_images(plan.builtBinary, plan.binaryName);
            if (!n) return std::unexpected(n.error());
            needed = std::move(*n);
        }
        fs::path staged;
        // A single file is made from the images (`single_file`); the tree
        // holds them as `vendored` does. The carrier is a program of this
        // machine's system and the IMAGE'S architecture, as a single file's
        // is: one of this machine's architecture cannot map an image of
        // another (measured on 2026.10.28.1: an x86-64 program carrying an
        // aarch64 image, runnable on no machine).
        if (plan.opts.mode == Mode::BundleAll && plan.opts.format != Format::SingleFile) {
            auto packed = carrier_build(plan, { plan.builtBinary, plan.binaryName,
                fs::path(plan.binaryName).stem().string(), std::string(host_system()),
                arch_of(plan.triple), false });
            if (!packed) return std::unexpected(packed.error());
            fs::copy(*packed, plan.stagingRoot / "bin",
                     fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
            staged = plan.stagingRoot / "bin"
                   / (fs::path(plan.binaryName).stem().string() + std::string(mcpplibs::os::exe_suffix));
        } else {
            staged = plan.stagingRoot / "bin" / plan.binaryName;
            if (auto r = copy(plan.builtBinary, staged); !r) return r;
        }
        for (auto const& n : needed)
            if (auto r = copy(n, plan.stagingRoot / "bin" / n.filename()); !r) return r;
        if (auto r = stage_runtime_files(plan, staged.parent_path()); !r) return r;
    } else {
        // THE TREE: a `lib/<arch>/` per leg, the launchers, and the runner's
        // program per system and architecture under `self-contained`.
        auto const& runner = plan.platform.fields.runner;
        if (!runner.set || runner.cmd.empty())
            return std::unexpected(Error{std::format(
                "the tree of {} starts its images through the platform's runner, and {} states none",
                plan.binaryName, plan.platform.display())});
        struct Leg { std::string triple, arch; fs::path artifact; };
        std::vector<Leg> legs{ { plan.triple, arch_of(plan.triple), plan.builtBinary } };
        for (auto const& l : plan.loadedLegs) legs.push_back({ l.triple, l.arch, l.artifact });
        mcpp::pack::distribution::Launch launch;
        launch.name = fs::path(plan.binaryName).stem().string();
        launch.runner = runner.cmd;
        for (auto const& leg : legs) launch.entry[leg.arch] = leg.artifact.filename().string();
        if (plan.opts.mode == Mode::BundleAll) launch.loader = fs::path(runner.cmd.front()).stem().string();
        for (auto const& leg : legs) {
            const auto dir = plan.stagingRoot / "lib" / leg.arch;
            if (auto r = copy(leg.artifact, dir / leg.artifact.filename()); !r) return r;
            if (bundles) {
                auto n = needed_images(leg.artifact, plan.binaryName);
                if (!n) return std::unexpected(n.error());
                for (auto const& f : *n)
                    if (auto r = copy(f, dir / f.filename()); !r) return r;
            }
            if (auto r = stage_runtime_files(plan, dir, leg.artifact.parent_path()); !r) return r;
        }
        if (plan.opts.mode == Mode::BundleAll) {
            for (auto const& leg : legs)
                for (auto const& system : mcpp::pack::systems_for(plan.opts.systems, leg.arch)) {
                    // A row this mcpp builds no program for (a planned
                    // target, `x86_64-macos`) carries no runner: said, and
                    // the package's metadata lists the systems it carries.
                    if (!row_is_built(program_row(system, leg.arch, false, &plan.platform))) {
                        mcpp::ui::warning(std::format(
                            "no runner for {}-{} in the package: this mcpp builds no program for "
                            "that system and architecture; the launcher there looks for `{}` on PATH",
                            system, leg.arch, launch.loader));
                        continue;
                    }
                    auto packed = carrier_build(plan, { {}, {}, launch.loader, system, leg.arch, false });
                    if (!packed) return std::unexpected(packed.error());
                    const auto into = plan.stagingRoot / "libexec" / launch.loader
                                    / std::format("{}-{}", system, leg.arch);
                    fs::create_directories(into, ec);
                    fs::copy(*packed, into,
                             fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
                    if (ec) return std::unexpected(Error{std::format(
                        "cannot place the runner for {}-{}: {}", system, leg.arch, ec.message())});
                }
        }
        const auto posix = plan.stagingRoot / "bin" / launch.name;
        {
            std::ofstream out(posix, std::ios::binary | std::ios::trunc);
            out << mcpp::pack::distribution::posix_launcher(launch);
        }
        fs::permissions(posix, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec
                               | fs::perms::others_read | fs::perms::others_exec,
                        fs::perm_options::replace, ec);
        // The Windows launcher, a program (design 2026-10-29 §4.3): built for
        // x86_64 Windows, which an arm64 Windows machine also runs. A host
        // that builds no program for that row leaves it out, says so, and
        // the package's `runs_on` names no Windows pair.
        // Over the platform's hosted arrangement where it serves the row, as
        // the runners are: the same program whichever machine packs it.
        const auto row = program_row("windows", "x86_64", true, &plan.platform);
        if (!row_is_built(row)) {
            mcpp::ui::warning(std::format(
                "no Windows launcher in the package: this mcpp builds no program for {}; the package "
                "does not run on Windows", row.triple));
        } else {
            const auto dir = plan.projectRoot / "target" / "launcher-pack" / launch.name;
            fs::create_directories(dir / "src", ec);
            fs::remove(dir / "mcpp.lock", ec);
            auto write = [&](const fs::path& p, std::string_view text) -> bool {
                std::ofstream out(p, std::ios::binary | std::ios::trunc);
                out.write(text.data(), static_cast<std::streamsize>(text.size()));
                return bool(out);
            };
            if (!write(dir / "mcpp.toml", mcpp::pack::distribution::exe_launcher_manifest(
                           launch.name, plan.packageVersion)
                           + static_row_pin(row, plan.platform.fields.toolchain.set
                                                     ? plan.platform.fields.toolchain.value : std::string()))
                || !write(dir / "src" / "main.cpp", mcpp::pack::distribution::exe_launcher_main(launch)))
                return std::unexpected(Error{std::format("cannot write the launcher's sources under {}",
                                                         dir.string())});
            auto packed = pack_generated_program(dir, row);
            if (!packed) return std::unexpected(packed.error());
            fs::copy_file(*packed / (launch.name + ".exe"), plan.stagingRoot / "bin" / (launch.name + ".exe"),
                          fs::copy_options::overwrite_existing, ec);
            if (ec) return std::unexpected(Error{std::format("cannot place the Windows launcher: {}",
                                                             ec.message())});
        }
    }

    copy_if_exists(plan.projectRoot / "README.md", plan.stagingRoot);
    copy_if_exists(plan.projectRoot / "LICENSE",   plan.stagingRoot);
    return write_container(plan);
}

// #622 A3/A10: the Application half whose form on this row is a shared
// object (`*-linux-android`). The object is staged under `lib/`, where every
// Android packaging tool expects native libraries, and it is never executed:
// a cross-compiled Android object names an interpreter this host does not have
// (`/system/bin/linker64`).
//
// #634 A3: ITS CLOSURE IS READ AND STAGED BESIDE IT. It used to be left to a
// provider (`dist-apk` walked `DT_NEEDED` itself), which meant the engine's
// own `dir` and `tar` shipped an object without the libraries it loads and said
// `closure = walked`. The rule is `ClosureRule::Android`: a name the API
// level's stub directory holds is the device's, and every other name --
// a graph-built dependency, the NDK's `libc++_shared.so` -- travels. The mode
// does not apply on this row: the device provides exactly the platform's
// libraries, so there is no `system` set to leave out and no loader or libc to
// carry.
//
// #630 A9: one triple stages flat (`lib/<name>.so`); more than one triple
// stages one `lib/<abi>/` per leg into the SAME tree, each with its own
// closure — see `Plan::extraSharedLegs`.
std::expected<ClosureResult, Error>
run_shared_program(const Plan& plan)
{
    std::error_code ec;
    std::filesystem::remove_all(plan.stagingRoot, ec);
    std::filesystem::create_directories(plan.stagingRoot / "lib", ec);
    if (ec) return std::unexpected(Error{std::format(
        "cannot create staging '{}': {}", plan.stagingRoot.string(), ec.message())});

    struct Leg {
        std::filesystem::path              dir;       // where the object is staged
        std::string                        abi;       // empty on the flat layout
        std::filesystem::path              artifact;
        std::vector<std::filesystem::path> searchDirs;
        std::vector<std::filesystem::path> platformDirs;
        // #649 E5: what this leg's graph built.
        std::vector<std::filesystem::path> graphBuilt;
    };
    auto primarySearch = plan.searchDirs;
    for (auto const& d : plan.opts.toolchainLibraryDirs) primarySearch.push_back(d);

    // #630 A9: MORE THAN ONE TRIPLE MEANS MORE THAN ONE `lib/<abi>/`, since a
    // flat `lib/<name>.so` cannot hold two architectures' bytes under one
    // name. `extraSharedLegs` is non-empty ONLY when the caller is the
    // several-`--target` route (`cmd_pack`, via `build_and_pack`'s trailing
    // parameter).
    std::vector<Leg> legs;
    if (!plan.extraSharedLegs.empty()) {
        auto t = mcpp::toolchain::triple::parse(plan.triple);
        auto primaryAbi = t ? mcpp::toolchain::triple::android_abi(*t) : plan.triple;
        legs.push_back({plan.stagingRoot / "lib" / primaryAbi, primaryAbi,
                        plan.builtBinary, primarySearch, plan.opts.platformLibraryDirs,
                        plan.graphSharedLibraries});
        for (auto const& leg : plan.extraSharedLegs)
            legs.push_back({plan.stagingRoot / "lib" / leg.abi, leg.abi, leg.artifact,
                            leg.searchDirs, leg.platformDirs, leg.graphSharedLibraries});
    } else {
        legs.push_back({plan.stagingRoot / "lib", {}, plan.builtBinary, primarySearch,
                        plan.opts.platformLibraryDirs, plan.graphSharedLibraries});
    }

    std::vector<ClosureNeed> needs;
    std::vector<std::string> unresolved;
    for (auto const& leg : legs) {
        std::error_code dec;
        std::filesystem::create_directories(leg.dir, dec);
        if (dec) return std::unexpected(Error{std::format(
            "cannot create staging '{}': {}", leg.dir.string(), dec.message())});
        std::filesystem::copy_file(leg.artifact, leg.dir / plan.binaryName,
            std::filesystem::copy_options::overwrite_existing, dec);
        if (dec) return std::unexpected(Error{std::format(
            "copy binary failed: {}", dec.message())});

        const std::string prefix = leg.abi.empty() ? std::string{} : leg.abi + ": ";
        // WITHOUT A PLATFORM DIRECTORY EVERY NAME WOULD BE A MEMBER: bionic's
        // `libc.so` stub is on the driver's search path too, and staging it
        // would ship a library with no code in it. The driver is what names
        // the directory, so a driver that did not answer is reported.
        if (leg.platformDirs.empty()) {
            unresolved.push_back(prefix + std::format(
                "the driver named no directory holding the platform's libc.so, so "
                "the device's libraries cannot be told from the ones '{}' carries",
                plan.binaryName));
            continue;
        }
        ClosureReadInput in;
        in.object       = leg.artifact;
        in.rule         = ClosureRule::Android;
        in.searchDirs   = leg.searchDirs;
        in.platformDirs = leg.platformDirs;
        const auto read = read_closure(in);
        std::vector<std::pair<std::filesystem::path, std::filesystem::path>> copies;
        if (auto r = stage_closure(plan, read, leg.dir, needs, unresolved, prefix,
                                   &copies); !r)
            return std::unexpected(r.error());

        // #649 E5: THIS ROW USED TO STRIP NOTHING. `run` dispatches a
        // shared-object program here before the ELF path that calls
        // `strip_program`, so the program, the libraries its graph built and
        // the NDK's `libc++_shared.so` all shipped their symbol tables while
        // the `Packing` line said "stripped", and `--debug-symbols` was
        // ignored. The program is a shared object, so it takes the
        // `SharedLibrary` shape, which keeps the `.dynsym` its host loads it
        // through. One tool serves every leg: the NDK's `llvm-strip` reads
        // every Android ABI. A several-ABI tree separates its debug files by
        // ABI, since two legs stage files of one name.
        const auto legDebugDir = (plan.debugDir.empty() || leg.abi.empty())
            ? plan.debugDir : plan.debugDir / leg.abi;
        if (plan.strip) {
            const auto stagedProgram = leg.dir / plan.binaryName;
            std::error_code pec;
            std::filesystem::permissions(stagedProgram, std::filesystem::perms::owner_write,
                                         std::filesystem::perm_options::add, pec);
            auto r = mcpp::pack::strip_artifact(stagedProgram,
                                                mcpp::pack::ArtifactShape::SharedLibrary,
                                                plan.stripTools, legDebugDir);
            if (!r) return std::unexpected(Error{r.error()});
        }
        for (auto const& [staged, source] : copies) {
            if (!strips_staged_copy(plan, source, leg.graphBuilt)) continue;
            if (auto r = strip_staged_library(plan, staged, source, legDebugDir); !r)
                return std::unexpected(r.error());
        }
    }

    // THE RUNTIME FILES TRAVEL AS ON EVERY OTHER ROW. `deploy` placed them
    // under `bin/<to>/` beside the built library; they are staged at the same
    // relative path under `bin/`, which is where a provider that maps them
    // into its own layout (`dist-apk`: `assets/`) reads them. Measured
    // 2026-09-12: without this the Android staged tree carried the library
    // alone and a deploy'd resource never reached the APK.
    if (!plan.opts.runtimeFiles.empty())
        if (auto r = stage_runtime_files(plan, plan.stagingRoot / "bin"); !r)
            return std::unexpected(r.error());

    copy_if_exists(plan.projectRoot / "README.md", plan.stagingRoot);
    copy_if_exists(plan.projectRoot / "LICENSE",   plan.stagingRoot);

    if (!unresolved.empty())
        return finish_without_closure(
            plan,
            unresolved_reason(plan.binaryName, unresolved,
                "A library the link used is found through the build's output "
                "directory, [runtime] library_dirs\n"
                "       or link_library_dirs, or the compiler's library search path."),
            std::move(needs));

    if (auto r = write_container(plan); !r) return std::unexpected(r.error());
    return ClosureResult{true, {}, std::move(needs)};
}

} // namespace detail

std::expected<ClosureResult, Error>
run(const Plan& plan, const mcpp::config::GlobalConfig& cfg)
{
    // A PE package is produced the same way on every host, because nothing in
    // that path executes the artifact. This is the branch the `#if
    // defined(_WIN32)` refusal used to occupy — and it was never really about
    // the host: `LD_TRACE_LOADED_OBJECTS` cannot trace a PE from Linux either.
    // #622 A3/A10: checked before the format-specific branches below --
    // this row's file is an ordinary ELF (`binfmt::identify` would answer
    // `Elf`, same as any Linux program), so it would otherwise fall into the
    // closure walk that follows and try to execute an object this host
    // cannot load at all. See the field comment on `programIsSharedObject`.
    //
    // These three dispatchers stage their own tree: PE and the Android
    // shared-object row read their closure from the files (#634 A3), and the
    // wasm32-emscripten launcher is a static image with everything embedded.
    // None of them is the "declare before discover" reorder below.
    if (plan.programIsSharedObject) return detail::run_shared_program(plan);

    if (plan.targetIsPe) {
        auto r = detail::run_pe(plan);
        if (!r) return std::unexpected(r.error());
        return ClosureResult{true, {}, std::move(*r)};
    }

    // wasm32-emscripten, before the Mach-O refusal and the ELF closure below:
    // this artifact is neither. `binfmt::identify` reports `Unknown` for the
    // `.js` launcher (plain text, none of the three magics), which is exactly
    // the branch the ELF path's own comment warns cannot be assumed away by
    // exclusion any more.
    if (plan.targetIsWasm) {
        if (auto r = detail::run_wasm(plan); !r) return std::unexpected(r.error());
        return ClosureResult{};
    }

    // An ok image: the images it names travel beside it, and under
    // `self-contained` the image is carried inside a program of this machine.
    if (plan.targetIsLoaded) {
        if (auto r = detail::run_loaded(plan); !r) return std::unexpected(r.error());
        return ClosureResult{};
    }

    using namespace detail;

    // STEPS 1-3: stage what is DECLARED before asking whether this host can
    // WALK what the artifact needs. The program, then the runtime files
    // `mcpp::deploy` and `[runtime] deploy` placed beside it, need no
    // loader — only the dependency closure below does. See `stage_declared`
    // and §3 of
    // .agents/docs/2026-09-13-630-what-a-framework-still-hits-in-the-engine.md.
    auto staged = stage_declared(plan);
    if (!staged) return std::unexpected(staged.error());
    auto bundledBinary = *staged;

    // STEP 4 FOR A MACH-O PROGRAM: ITS CLOSURE IS READ, ON EVERY HOST (#634 A3).
    //
    // The ELF closure below asks the dynamic linker for the dependency list by
    // running the artifact with `LD_TRACE_LOADED_OBJECTS=1`. That variable
    // belongs to glibc's ld.so; dyld has never heard of it, so on macOS the
    // command would RUN THE USER'S PROGRAM and parse whatever it printed. So a
    // Mach-O program never reaches that path: its load commands are read
    // (`ClosureRule::MachO`), which needs no loader and therefore no macOS
    // host either, and its dylibs are staged beside it in `bin/`, where the
    // `@loader_path` rpath a consumer of a graph-built dylib links with
    // already finds them. No load command is edited and nothing is re-signed,
    // which is also why the program is not stripped on this row.
    //
    // THE BUILT PROGRAM IS READ, NOT THE STAGED COPY: `@loader_path` must
    // expand to the directory the dylibs were built into.
    if (mcpp::pack::binfmt::identify(plan.builtBinary).format
        == mcpp::pack::binfmt::Format::MachO) {
        std::vector<ClosureNeed> needs;
        std::vector<std::string> unresolved;
        if (plan.opts.mode != Mode::None && plan.opts.mode != Mode::Static) {
            ClosureReadInput in;
            in.object = plan.builtBinary;
            in.rule   = ClosureRule::MachO;
            auto t = mcpp::toolchain::triple::parse(plan.triple);
            in.arch   = t ? t->arch : std::string(mcpp::toolchain::platform::host_arch);
            const auto read = read_closure(in);
            if (auto r = stage_closure(plan, read, plan.stagingRoot / "bin", needs,
                                       unresolved, {}); !r)
                return std::unexpected(r.error());
        }
        if (!unresolved.empty())
            return finish_without_closure(
                plan,
                unresolved_reason(plan.binaryName, unresolved,
                    "A dylib is staged beside the program when its install name "
                    "is @rpath/<file> and the program\n"
                    "       carries an @loader_path or @executable_path rpath, or "
                    "when it is @loader_path/<file>."),
                std::move(needs));
        if (auto r = write_topentry_wrapper(plan.stagingRoot, plan.binaryName); !r)
            return std::unexpected(Error{r.error()});
        if (auto r = write_container(plan); !r) return std::unexpected(r.error());
        return ClosureResult{true, {}, std::move(needs)};
    }

    if constexpr (mcpplibs::os::is_windows) {
        // STEP 4, second mechanism: a NON-PE artifact on a Windows host — a cross
        // build to Linux or macOS. The closure would be resolved by running the
        // binary under its own dynamic linker, which this machine cannot do — so
        // say that, rather than reporting a platform limitation that no longer
        // exists for the case a Windows user is actually likely to hit.
        //
        // Also step 4's outcome now, for the same reason the Mach-O branch above
        // is: the tree from `stage_declared` already exists when a dispatched
        // format reaches this point.
        (void)cfg;
        return finish_without_closure(plan, std::format(
            "cannot package a {} artifact from a Windows host.\n"
            "       The dependency closure for that format is resolved by running "
            "the artifact under\n"
            "       the target's own dynamic linker, which this machine has no way "
            "to do. Package it\n"
            "       on the target OS, or build for Windows (`--target "
            "x86_64-pc-windows-msvc`), which\n"
            "       needs no such step.",
            std::string(mcpp::pack::binfmt::format_name(
                mcpp::pack::binfmt::identify(plan.builtBinary).format))));
    } else {
        std::error_code ec;

        // STEP 4, third mechanism, and STEP 5 (the format tail): library
        // bundling for non-static modes.
        //
        //    BundleProject (default) — drop all manylinux-allowed system libs
        //    (libc/libstdc++/ld-linux/...) and bundle the rest. The user can
        //    extend the skip list via [pack.bundle-project].also_skip /
        //    .force_bundle.
        //
        //    BundleAll — ship every dep including the dynamic linker; entry
        //    point becomes `run.sh` which invokes the bundled ld with
        //    --library-path → fully portable across glibc versions.
        // #634 A3: what step 4 resolved, for the stage manifest, and the refusal
        // text when it is incomplete. Both stay empty under `--mode static`.
        std::vector<ClosureNeed> needs;
        std::string unresolvedText;
        // #649 E5: (staged, source) of every bundled library, for the strip step
        // at the end, which must see the final images (after the search-path edits).
        std::vector<std::pair<std::filesystem::path, std::filesystem::path>> bundledCopies;
        std::string bundledLoader;
        if (plan.opts.mode != Mode::Static) {
            // THE BUILT BINARY, NOT THE STAGED COPY, and the difference is
            // `$ORIGIN`.
            //
            // The closure is resolved by running the artifact under its own
            // loader. A dependency that mcpp itself built as a shared library
            // lives beside the artifact in `bin/` and is found through the
            // `$ORIGIN` in its RUNPATH — but the copy in the staging directory has
            // an empty `bin/` next to it, so that entry resolves to nothing, the
            // library never appears in the closure, and it is therefore never
            // bundled. The bundle builds, packs and uploads without complaint, and
            // fails on the user's machine with
            // `error while loading shared libraries: libfoo.so`.
            //
            // Measured on mcpp 2026.8.26.1 with an author-declared
            // `kind = "shared"` dependency, so this predates the consumer-side
            // form axis (#519) — but that axis is what makes it reachable for
            // EVERY package rather than the handful that declare themselves
            // shared, which is why it is fixed here.
            //
            // Tracing the built binary is safe: the staged file is a byte copy of
            // it and nothing has modified either one at this point (patchelf runs
            // further down). What changes is only which directory `$ORIGIN`
            // expands to while the loader is looking.
            //
            // A NON-ELF ARTIFACT REACHING THIS POINT ASSUMED ELF BY EXCLUSION.
            // PE and Mach-O are refused by name above `run()`'s `#else`; nothing
            // between there and here asks what is LEFT actually is ELF, because
            // ELF used to be the only format left once those two were excluded.
            // wasm32-emscripten is the first target where that assumption is
            // false: `binfmt::identify` reports `Format::Unknown` for a `.wasm`
            // module (it carries none of the three magics), and `ldd_parse`
            // below runs the file through the same LD_TRACE_LOADED_OBJECTS
            // mechanism the Mach-O branch above refuses by name rather than
            // risk — on a host where `.wasm` is registered in `binfmt_misc`,
            // that does not fail, it RUNS the module.
            if (auto fmt = mcpp::pack::binfmt::identify(plan.builtBinary).format;
                fmt != mcpp::pack::binfmt::Format::Elf) {
                return finish_without_closure(plan, std::format(
                    "cannot package the {} artifact '{}' yet.\n"
                    "       Its dependency closure is resolved by running the "
                    "artifact under its own\n"
                    "       dynamic linker, and this file is neither ELF, PE nor "
                    "Mach-O -- there is no\n"
                    "       such linker to ask.",
                    mcpp::pack::binfmt::format_name(fmt), plan.binaryName));
            }
            auto deps = ldd_parse(plan.builtBinary);
            if (!deps) return std::unexpected(Error{std::format(
                "ldd failed on {}: {}", plan.builtBinary.string(), deps.error())});

            auto skipped_by_mode = [&](std::string_view soname) {
                bool skip = false;
                if (plan.opts.mode == Mode::None) {
                    skip = true;  // system: host provides every .so, bundle nothing
                } else if (plan.opts.mode == Mode::BundleProject) {
                    if (is_system_lib(soname))                          skip = true;
                    if (soname_matches(soname, plan.alsoSkipLibs))      skip = true;
                    if (soname_matches(soname, plan.forceBundleLibs))   skip = false;  // override
                }
                // Mode::BundleAll: skip nothing — we want the loader too.
                return skip;
            };

            // #634 A3: the `needs` lines, from the same decisions that fill the
            // tree. A mode that bundles nothing states no closure, as on every
            // other row.
            const bool bundling = plan.opts.mode != Mode::None;
            std::vector<ResolvedDep> toBundle;
            for (auto& d : deps->found) {
                if (skipped_by_mode(d.soname)) {
                    if (bundling) needs.push_back({d.soname, ClosureNeed::Kind::Platform, {}});
                    continue;
                }
                toBundle.push_back(d);
                needs.push_back({d.soname, ClosureNeed::Kind::Staged, "lib/" + d.soname});
            }
            // A name the loader found no file for is one the tree cannot carry.
            // When the mode leaves it to the target anyway it is the target's;
            // otherwise the closure is incomplete, and an archive that says
            // `walked` would not start where it is installed.
            std::vector<std::string> unresolved;
            for (auto const& name : deps->notFound) {
                if (!bundling) continue;
                // `also_skip` names the target's libraries in every bundling mode
                // here, `self-contained` included: the tree cannot carry a file
                // this machine does not have.
                if (skipped_by_mode(name) || (soname_matches(name, plan.alsoSkipLibs)
                                              && !soname_matches(name, plan.forceBundleLibs))) {
                    needs.push_back({name, ClosureNeed::Kind::Platform, {}});
                    continue;
                }
                needs.push_back({name, ClosureNeed::Kind::Unresolved, {}});
                unresolved.push_back(name + ": the loader finds no file for it");
            }
            if (!unresolved.empty())
                unresolvedText = unresolved_reason(plan.binaryName, unresolved,
                    "The loader looks through the program's run-time search path "
                    "(DT_RPATH, DT_RUNPATH) and\n"
                    "       its own default directories.");
            // An archive refuses before any file is edited; a dispatched format
            // still receives the tree the resolved names allow.
            if (!unresolved.empty() && plan.opts.format != Format::Dispatched)
                return finish_without_closure(plan, unresolvedText, needs);

            if (auto r = bundle_libs(toBundle, plan.stagingRoot); !r)
                return std::unexpected(Error{r.error()});
            for (auto const& d : toBundle)
                bundledCopies.emplace_back(plan.stagingRoot / "lib" / d.soname, d.path);
            bundledLoader = find_loader_soname(toBundle);

            // Search path: point at bundled libs, or REMOVE THE TAG if there are none.
            //
            // The empty case used to be `patchelf --set-rpath ''`, which leaves the
            // tag present with an empty string — and a present-but-empty DT_RUNPATH
            // is not inert: it suppresses the inherited DT_RPATH chain exactly like
            // a stale one does (measured — see mcpp.pack.relocate). Harmless on an
            // executable, which is the top of that chain, but there is no reason to
            // write a tag that says nothing.
            //
            // OUTSIDE the patchelf guard, because it no longer needs patchelf. That
            // is the whole point of the in-process editor: `--mode system` on a host
            // where the sandbox has no patchelf used to leave the build machine's
            // store in the artifact and say nothing at all.
            if (toBundle.empty()) {
                if (auto r = mcpp::pack::relocate::strip_search_paths(bundledBinary); !r)
                    return std::unexpected(Error{r.error()});
            }

            auto patchelf = sandbox_patchelf(cfg);
            if (!patchelf.empty()) {
                //   non-empty bundle → "$ORIGIN/../lib" so the binary finds them
                if (!toBundle.empty()) {
                    if (auto r = set_search_path(bundledBinary, "$ORIGIN/../lib",
                                                 mcpp::build::loader::Form::Executable,
                                                 patchelf); !r)
                        return std::unexpected(Error{r.error()});
                }

                // EVERY BUNDLED LIBRARY, not just the executable.
                //
                // A bundled .so keeps whatever RUNPATH it was built with, and on
                // this ecosystem that is a set of ABSOLUTE paths into the BUILD
                // MACHINE's xlings store. Measured on a graphics artifact:
                //
                //   <store>/xim-x-glibc/2.44/lib64 : <store>/xim-x-gcc/16.1.0/lib64
                //   : <store>/compat-x-glx-runtime/…/lib : $ORIGIN
                //
                // Those directories do not exist on the target, and worse, if the
                // target happens to be another developer's machine they exist with
                // DIFFERENT contents. "Depends on the xlings ecosystem" would be a
                // design choice; "depends on this one machine's store" is a defect,
                // and it is invisible because the bundle runs fine where it was
                // built. $ORIGIN is where its siblings actually are.
                //
                // EXCEPT THE DYNAMIC LOADER. `ld-linux-*.so` is not a shared
                // library that gets searched for; it is the program that DOES the
                // searching, and it is loaded by the kernel from an absolute path
                // (PT_INTERP, or `run.sh`'s explicit invocation). Rewriting its
                // own search path is meaningless, and patchelf rewriting it is
                // destructive: Mode `self-contained` then segfaults before main,
                // because the thing that was supposed to resolve the process's
                // libraries no longer loads. Found by `30_pack_modes`.
                auto loaderSoname = find_loader_soname(toBundle);
                for (auto const& dep : toBundle) {
                    if (!loaderSoname.empty() && dep.soname == loaderSoname) continue;
                    auto staged = plan.stagingRoot / "lib" / dep.soname;
                    std::error_code ec;
                    if (!std::filesystem::is_regular_file(staged, ec)) continue;
                    if (auto r = set_search_path(
                            staged, "$ORIGIN",
                            mcpp::build::loader::Form::SharedLibrary, patchelf); !r)
                        return std::unexpected(Error{r.error()});
                }

                // PT_INTERP handling differs by mode:
                //   BundleProject → repoint to the target distro's loader
                //                   (LSB layout: /lib64/<loader> on x86_64,
                //                   /lib/<loader> elsewhere), derived from the
                //                   loader soname ldd resolved for THIS binary —
                //                   arch-correct without hardcoding a name.
                //   BundleAll     → leave PT_INTERP alone; the wrapper script
                //                   ignores it and launches via the bundled
                //                   loader directly.
                if (plan.opts.mode == Mode::BundleProject || plan.opts.mode == Mode::None) {
                    if (auto soname = find_loader_soname(deps->found); !soname.empty()) {
                        auto distroLoader =
                            (soname == "ld-linux-x86-64.so.2" ? "/lib64/" : "/lib/")
                            + soname;
                        if (auto r = set_interpreter(bundledBinary, distroLoader,
                                                     patchelf); !r)
                            return std::unexpected(Error{r.error()});
                    }
                }
            }

            if (plan.opts.mode == Mode::BundleAll) {
                auto loader = find_loader_soname(toBundle);
                if (loader.empty()) {
                    // No ld-linux in deps means binary was statically linked,
                    // which the user typically wouldn't combine with --mode
                    // bundle-all. Skip wrapper, ship as-is.
                } else {
                    // The runtime data of the C library the bundle carries
                    // (openxlings/xlings#621): its compiled locales and its
                    // character-set converters, from the payload the bundled
                    // loader came from (`<payload>/lib64/ld-linux-*` ->
                    // `<payload>/lib/{locale,gconv}`). A payload without them
                    // contributes nothing, and the program runs as before.
                    bool localeData = false, gconvData = false;
                    for (auto const& d : toBundle) {
                        if (d.soname != loader) continue;
                        std::error_code dec;
                        const auto payload =
                            std::filesystem::canonical(d.path, dec).parent_path().parent_path();
                        if (dec) break;
                        auto copy_dir = [&](std::string_view sub) {
                            const auto from = payload / "lib" / std::string(sub);
                            std::error_code cec;
                            if (!std::filesystem::is_directory(from, cec)) return false;
                            const auto to = plan.stagingRoot / "lib" / std::string(sub);
                            std::filesystem::create_directories(to, cec);
                            std::filesystem::copy(from, to,
                                std::filesystem::copy_options::recursive
                                | std::filesystem::copy_options::overwrite_existing, cec);
                            return !cec;
                        };
                        localeData = copy_dir("locale");
                        gconvData  = copy_dir("gconv");
                        break;
                    }
                    // Mode B writes BOTH `run.sh` and `<binary_name>` at the
                    // bundle root — same content, different names — so users
                    // can pick whichever entry point they prefer.
                    if (auto r = write_bundle_all_wrappers(plan.stagingRoot,
                            plan.binaryName, loader, localeData, gconvData); !r)
                        return std::unexpected(Error{r.error()});
                    collect_licenses(toBundle, plan.stagingRoot);
                }
            } else {
                // Static / BundleProject: drop a thin shell wrapper at the
                // root that exec's bin/<name>, so `./hello` from the
                // unpacked bundle runs the program directly.
                if (auto r = write_topentry_wrapper(plan.stagingRoot, plan.binaryName); !r)
                    return std::unexpected(Error{r.error()});
            }
        } else {
            // Mode::Static — also add the top-level entry-point wrapper.
            if (auto r = write_topentry_wrapper(plan.stagingRoot, plan.binaryName); !r)
                return std::unexpected(Error{r.error()});
        }

        // The format tail's last two steps, run only because step 4 above
        // produced a list (an early return took every path where it did not):
        // debug information does not travel either.
        //
        // AFTER every byte-changing step above (patchelf's search path, PT_INTERP)
        // and before the archive: strip must see the final image, and the archive
        // must see the stripped one. Same ordering rule the library packer states
        // at its leg loop.
        if (auto r = strip_program(plan, bundledBinary); !r) return std::unexpected(r.error());
        // #649 E5: and the shared libraries this graph built. The dynamic loader
        // is never one of them and is never edited, for the reason the
        // search-path loop above gives.
        for (auto const& [staged, source] : bundledCopies) {
            if (!bundledLoader.empty() && staged.filename() == bundledLoader) continue;
            if (!strips_staged_copy(plan, source, plan.graphSharedLibraries)) continue;
            if (auto r = strip_staged_library(plan, staged, source, plan.debugDir); !r)
                return std::unexpected(r.error());
        }

        // A dispatched format reaches this point with an incomplete closure; an
        // archive was refused before any file was edited.
        if (!unresolvedText.empty())
            return finish_without_closure(plan, unresolvedText, std::move(needs));

        // THE C LIBRARY THE MACHINE THAT RUNS IT MUST HAVE (design 2026-10-23
        // §4.11). A package that loads the target's own C library binds no base:
        // the base it was built against becomes a FLOOR, the highest `GLIBC_x.y`
        // the program and every library shipped beside it need. Said once on the
        // terminal and written to HOST-REQUIREMENTS, where a reader of the bundle
        // looks for what the target must provide.
        //
        // Only for a build that STATED its base: that is the case whose floor is a
        // decision someone made (an old glibc, for distributions older than this
        // machine). The ecosystem's own base packs exactly as it did.
        if (plan.opts.baseStated
            && (plan.opts.mode == Mode::BundleProject || plan.opts.mode == Mode::None)) {
            std::vector<std::string> needs;
            auto consider = [&](const std::filesystem::path& p) {
                if (auto f = mcpp::runtime::elf::inspect_elf_runtime(p))
                    needs.insert(needs.end(), f->requiredGlibcVersions.begin(),
                                 f->requiredGlibcVersions.end());
            };
            consider(bundledBinary);
            for (auto const& [staged, source] : bundledCopies) consider(staged);
            if (auto floor = mcpp::toolchain::base::highest_glibc(needs); !floor.empty()) {
                mcpp::ui::info("Requires", std::format(
                    "glibc >= {} on the machine that runs it (the highest GLIBC_ version "
                    "the program and the libraries packed with it need)", floor));
                auto reqs = plan.hostRequirements;
                reqs.push_back({std::format("c-abi.glibc>={}", floor), "PT_INTERP", true});
                std::ofstream out(plan.stagingRoot / std::filesystem::path(kFileName));
                out << render(reqs);
            }
        }

        // Output.
        if (auto r = write_container(plan); !r) return std::unexpected(r.error());
        return ClosureResult{true, {}, std::move(needs)};
    }
}

std::expected<std::filesystem::path, Error> write_another_container(const Plan& plan, ArchiveKind kind) {
    Plan q = plan;
    q.archive = kind;
    if (kind == ArchiveKind::Zip
        && (q.opts.compression.algorithm == mcpp::pack::compress::Algorithm::Zstd
            || q.opts.compression.algorithm == mcpp::pack::compress::Algorithm::Xz))
        q.opts.compression = {};
    q.archivePath = (q.opts.outputDir.empty() ? q.projectRoot / "target" / "dist" : q.opts.outputDir)
                  / detail::default_archive_name(
        q.packageName, q.packageVersion, q.opts.universalName.empty() ? q.triple : q.opts.universalName,
        q.opts.mode, kind, q.opts.compression.algorithm);
    if (auto r = detail::write_container(q); !r) return std::unexpected(r.error());
    return q.archivePath;
}

std::expected<void, Error> finalize_staged(const Plan& plan) {
    return detail::finalize_tree(plan);
}

std::vector<std::string> systems_for(const std::vector<std::string>& stated, std::string_view arch) {
    if (stated.empty()) return { std::string(detail::host_system()) };
    std::vector<std::string> out;
    auto add = [&](std::string os) {
        if (std::ranges::find(out, os) == out.end()) out.push_back(std::move(os));
    };
    for (auto const& e : stated) {
        if (e == "all") {
            for (auto os : { "linux", "macos", "windows" }) add(os);
            continue;
        }
        const auto dash = e.find('-');
        if (dash == std::string::npos) add(e);
        else if (std::string_view(e).substr(dash + 1) == arch) add(e.substr(0, dash));
    }
    return out;
}

std::vector<SingleFileRow> single_file_rows(const Plan& plan) {
    std::vector<std::string> archs{ detail::arch_of(plan.triple) };
    for (auto const& l : plan.loadedLegs) archs.push_back(l.arch);
    const bool all = std::ranges::find(plan.opts.systems, "all") != plan.opts.systems.end();
    std::vector<SingleFileRow> rows;
    for (auto const& arch : archs)
        for (auto const& system : systems_for(plan.opts.systems, arch)) {
            SingleFileRow r{ system, arch, {} };
            if (all)
                if (auto row = unbuilt_row(system, arch); !row.empty())
                    r.skipped = std::format("no carrier row: this mcpp builds no program for {}", row);
            rows.push_back(std::move(r));
        }
    return rows;
}

bool output_names_directory(const std::filesystem::path& output) {
    if (output.empty()) return false;
    const auto text = output.native();
    if (text.back() == '/' || text.back() == '\\') return true;
    std::error_code ec;
    return std::filesystem::is_directory(output, ec);
}

std::string unbuilt_row(std::string_view system, std::string_view arch) {
    const auto row = detail::program_row(system, arch, true);
    return detail::row_is_built(row) ? std::string() : row.triple;
}

std::vector<std::string> carried_systems(const Plan& plan) {
    // The one-leg tree of a single file holds the images, not the carrier
    // (`run_loaded`): the carrier is the file, made per system of `--systems`.
    if (!plan.opts.tree && plan.targetIsLoaded && plan.opts.mode == Mode::BundleAll
        && plan.opts.format != Format::SingleFile)
        return { std::format("{}-{}", detail::host_system(), detail::arch_of(plan.triple)) };
    return mcpp::pack::distribution::loader_systems(plan.stagingRoot);
}

std::expected<std::filesystem::path, Error>
runner_program(const mcpp::platform::Resolved& platform, const std::filesystem::path& projectRoot,
               std::string_view name, std::string_view arch, bool build) {
    const std::string system(detail::host_system());
    if (!platform.active
        || !mcpp::manifest::platform::carrier_for(platform.fields, std::format("{}-{}", system, arch)))
        return std::unexpected(Error{std::format(
            "the runner `{}` of {} runs on a machine of the artifact's architecture, {}, and its "
            "program for that architecture is the platform's carrier, which {} does not state for "
            "{}-{} ([platform] carrier = {{ package, section, main, hosts }})",
            name, platform.display(), arch, platform.display(), system, arch)});
    if (auto why = detail::unbuilt_carrier(platform, system, arch, false))
        return std::unexpected(Error{*why});
    const auto program = detail::packed_program(detail::carrier_dir(projectRoot, name, system, arch),
                                                name, system);
    if (!build) return program;
    Plan plan;
    plan.platform = platform;
    plan.projectRoot = projectRoot;
    plan.binaryName = std::string(name);
    auto packed = detail::carrier_build(plan, { {}, {}, std::string(name), system, std::string(arch),
                                                false, "the runner on the execution host" });
    if (!packed) return std::unexpected(packed.error());
    std::error_code ec;
    if (!std::filesystem::is_regular_file(program, ec))
        return std::unexpected(Error{std::format("the carrier built for {}-{} has no {}", system, arch,
                                                 program.filename().string())});
    return program;
}

std::expected<std::vector<std::filesystem::path>, Error> single_file(const Plan& plan) {
    namespace fs = std::filesystem;
    namespace d = mcpp::pack::distribution;
    std::error_code ec;
    const auto distDir = plan.projectRoot / "target" / "dist";
    auto output_for = [&](std::string_view qualifier, std::string_view suffix) {
        if (plan.opts.output.empty())
            return (plan.opts.outputDir.empty() ? distDir : plan.opts.outputDir)
                 / std::format("{}-{}-{}{}", plan.packageName, plan.packageVersion, qualifier, suffix);
        if (!plan.opts.output.has_parent_path()) return distDir / plan.opts.output;
        return plan.opts.output;
    };
    std::vector<fs::path> written;
    if (plan.targetIsLoaded) {
        // THE IMAGE, OR THE IMAGES AS ONE STORED ZIP: what the carrier's
        // entry reads between `mcpp_carried_begin` and `mcpp_carried_end`
        // (SPEC-013 §11): the image alone when it names no other, else every
        // image it needs as members of a zip whose data lies on 64 KiB
        // boundaries, the entry named by `mcpp_carried_name`. One file per
        // leg and system of the leg's architecture (design 2026-10-29 §4.5):
        // the first leg's images are the staged `bin/`, another leg's are its
        // artifact and the images that artifact names.
        struct Leg { std::string arch; std::vector<fs::path> images; fs::path carried; };
        std::vector<Leg> legs;
        {
            Leg first{ detail::arch_of(plan.triple), {}, {} };
            for (auto const& e : fs::directory_iterator(plan.stagingRoot / "bin", ec))
                if (e.is_regular_file(ec)) first.images.push_back(e.path());
            legs.push_back(std::move(first));
        }
        for (auto const& l : plan.loadedLegs) {
            auto n = detail::needed_images(l.artifact, plan.binaryName);
            if (!n) return std::unexpected(n.error());
            Leg leg{ l.arch, { l.artifact }, {} };
            leg.images.insert(leg.images.end(), n->begin(), n->end());
            legs.push_back(std::move(leg));
        }
        for (auto& leg : legs) {
            std::ranges::sort(leg.images);
            leg.carried = leg.images.empty() ? plan.stagingRoot / "bin" / plan.binaryName : leg.images.front();
            if (leg.images.size() < 2) continue;
            leg.carried = plan.projectRoot / "target" / "carrier-pack"
                        / std::format("{}-{}.carried.zip", fs::path(plan.binaryName).stem().string(), leg.arch);
            fs::create_directories(leg.carried.parent_path(), ec);
            std::vector<mcpp::pack::zip::Entry> entries;
            for (auto const& i : leg.images) entries.push_back({ i.filename().string(), i, false, {} });
            mcpp::pack::zip::WriteOptions o;
            o.align = 65536;
            if (auto r = mcpp::pack::zip::write(leg.carried, entries, o); !r)
                return std::unexpected(Error{r.error()});
        }
        struct File { std::string system, arch; fs::path carried; };
        std::vector<File> files;
        for (auto const& r : single_file_rows(plan)) {
            if (!r.skipped.empty()) continue;
            files.push_back({ r.system, r.arch, std::ranges::find(legs, r.arch, &Leg::arch)->carried });
        }
        if (files.empty())
            return std::unexpected(Error{std::string(
                "--systems names no system this mcpp makes a single file for, of the legs' "
                "architectures")});
        if (files.size() > 1 && !plan.opts.output.empty())
            return std::unexpected(Error{std::format(
                "--output names one file, and this pack makes {} single files (one per leg and "
                "system).\n  use: -o <directory>/ (each file is named <name>-<version>-<os>-<arch>)",
                files.size())});
        for (auto const& f : files) {
            const auto stem = fs::path(plan.binaryName).stem().string();
            auto packed = detail::carrier_build(plan, { f.carried, plan.binaryName, stem, f.system, f.arch, true });
            if (!packed) return std::unexpected(packed.error());
            const auto suffix = f.system == "windows" ? std::string(".exe") : std::string();
            const auto program = *packed / (stem + suffix);
            if (!fs::is_regular_file(program, ec))
                return std::unexpected(Error{std::format("the carrier built for {}-{} has no {}",
                                                         f.system, f.arch, program.filename().string())});
            const auto out = output_for(std::format("{}-{}", f.system, f.arch), suffix);
            fs::create_directories(out.parent_path(), ec);
            fs::copy_file(program, out, fs::copy_options::overwrite_existing, ec);
            if (ec) return std::unexpected(Error{std::format("cannot write {}: {}", out.string(), ec.message())});
            written.push_back(out);
        }
    } else {
        // THE ENGINE'S LAUNCHER CARRYING THE TREE. The entry: the program a
        // person runs from the unpacked tree -- a Windows program at the root,
        // a self-contained Linux tree's wrapper at the root, `bin/<name>`
        // otherwise.
        std::string entry = "bin/" + plan.binaryName;
        if (plan.targetIsPe) entry = plan.binaryName;
        else if (plan.opts.mode == Mode::BundleAll && fs::is_regular_file(plan.stagingRoot / plan.binaryName, ec))
            entry = plan.binaryName;
        auto tree = d::tree_payload(plan.stagingRoot);
        if (!tree) return std::unexpected(Error{tree.error()});
        auto c = plan.opts.compression;
        c.algorithm = mcpp::pack::compress::Algorithm::Zstd;   // `none` stores at level 1
        if (plan.opts.compression.algorithm == mcpp::pack::compress::Algorithm::None) c.level = 1;
        auto payload = mcpp::pack::compress::bytes(*tree, c);
        if (!payload) return std::unexpected(Error{payload.error()});
        const auto digest = d::sha256(*payload);
        const auto stem = fs::path(plan.binaryName).stem().string();
        const auto dir = plan.projectRoot / "target" / "single-file" / stem;
        fs::create_directories(dir / "src", ec);
        fs::remove(dir / "mcpp.lock", ec);
        auto write = [&](const fs::path& p, std::string_view text) -> bool {
            std::ofstream out(p, std::ios::binary | std::ios::trunc);
            out.write(text.data(), static_cast<std::streamsize>(text.size()));
            return bool(out);
        };
        auto t = mcpp::toolchain::triple::parse(plan.triple);
        const std::string arch = t ? t->arch : std::string(mcpp::toolchain::platform::host_arch);
        const std::string system = !t ? std::string(detail::host_system())
                                 : t->os == "windows" ? std::string("windows")
                                 : t->os == "macos" ? std::string("macos") : std::string("linux");
        const auto row = detail::program_row(system, arch, true);
        if (!write(dir / "payload.zst", *payload)
            || !write(dir / "mcpp.toml", d::launcher_manifest(stem, plan.packageVersion) + detail::static_row_pin(row))
            || !write(dir / "src" / "main.cpp", d::launcher_main())
            || !write(dir / "src" / "carried.S",
                      detail::carried_assembly(system, ".mcppsf", dir / "payload.zst", entry, digest)))
            return std::unexpected(Error{std::format("cannot write the launcher's sources under {}", dir.string())});
        mcpp::ui::info("Packing", std::format("{} into one file for {}-{} ({} payload, sha256 {})",
                                              plan.binaryName, system, arch,
                                              mcpp::pack::compress::name(c.algorithm), digest.substr(0, 12)));
        auto packed = detail::pack_generated_program(dir, row);
        if (!packed) return std::unexpected(packed.error());
        const auto suffix = system == "windows" ? std::string(".exe") : std::string();
        const auto program = *packed / (stem + suffix);
        const auto out = output_for(t ? t->str() : plan.triple, suffix);
        fs::create_directories(out.parent_path(), ec);
        fs::copy_file(program, out, fs::copy_options::overwrite_existing, ec);
        if (ec) return std::unexpected(Error{std::format("cannot write {}: {}", out.string(), ec.message())});
        written.push_back(out);
    }
    // The signer signs the file a person runs, too.
    if (!plan.opts.sign.empty())
        for (auto const& f : written) {
            std::vector<std::string> argv;
            for (auto const& a : plan.opts.sign) argv.push_back(a == "{}" ? f.string() : a);
            int spawnError = 0;
            auto r = mcpp::run::capture_exec(argv, {}, {}, &spawnError);
            if (spawnError != 0 || r.exit_code != 0)
                return std::unexpected(Error{std::format("[pack] sign failed on {} (exit {}): {}\n{}",
                                                         f.string(), r.exit_code,
                                                         mcpp::build::command_line::display(argv),
                                                         r.output)});
        }
    return written;
}

} // namespace mcpp::pack
