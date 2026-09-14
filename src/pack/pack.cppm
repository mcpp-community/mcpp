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
import mcpp.config;
import mcpp.pack.binfmt;
import mcpp.pack.host_requirements;
import mcpp.pack.relocate;
import mcpp.pack.stage_tree;
import mcpp.pack.strip;
import mcpp.pack.zip;
import mcpp.platform;
import mcpp.xlings;
import mcpp.manifest;
import mcpp.toolchain.triple;

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
enum class Format { Tar, Dir, Dispatched };

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
    Format                          format       = Format::Tar;
    // The `--format` value when `format == Dispatched`. Empty otherwise.
    //
    // Not validated by the CLI, and deliberately: the set of valid values is a
    // property of the RESOLVED GRAPH, so the refusal has to wait until build
    // programs have declared what they provide. It arrives before anything is
    // compiled, which is the earliest point at which it can be exact.
    std::string                     formatName;
    std::filesystem::path           output;        // empty = derive from manifest
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
    std::filesystem::path              artifact;
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
    std::string                          packageName;
    std::string                          packageVersion;
    std::string                          triple;            // e.g. "x86_64-linux-musl"
    // From manifest [pack]
    std::vector<std::string>             includeGlobs;
    std::vector<std::string>             excludeGlobs;
    std::vector<std::string>             alsoSkipLibs;
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
};

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
                                 std::string_view triple, Mode mode, bool pe)
{
    std::string_view ext = pe ? ".zip" : ".tar.gz";
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
    auto name = archive.filename().string();
    for (auto suffix : {std::string_view{".tar.gz"}, std::string_view{".tgz"},
                        std::string_view{".zip"}}) {
        if (name.ends_with(suffix)) return name.substr(0, name.size() - suffix.size());
    }
    // No recognised compression suffix — fall back to the bare stem.
    return archive.stem().string();
}

} // namespace detail

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

    auto distDir = projectRoot / "target" / "dist";
    if (opts.output.empty()) {
        p.archivePath = distDir / detail::default_archive_name(
            p.packageName, p.packageVersion, p.triple, opts.mode, p.targetIsPe);
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

    p.includeGlobs    = manifest.packConfig.include;
    p.excludeGlobs    = manifest.packConfig.exclude;
    p.alsoSkipLibs    = manifest.packConfig.alsoSkip;
    p.forceBundleLibs = manifest.packConfig.forceBundle;

    return p;
}

namespace detail {

// Run a shell command, capturing stdout. Returns the captured text on
// success, or an error message on non-zero exit.
std::expected<std::string, std::string>
run_capture(const std::string& cmd) {
    auto r = mcpp::platform::process::capture(cmd);
    if (r.exit_code != 0) return std::unexpected(std::format(
        "command exited with {}: {}", r.exit_code, cmd));
    return r.output;
}

// Run a shell command and discard stdout/stderr; return exit code.
int run_silent(const std::string& cmd) {
    auto silent = cmd + " " + std::string(mcpp::platform::shell::silent_redirect);
    return mcpp::platform::process::run_silent(silent);
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
    auto cmd = std::format(
        "LD_TRACE_LOADED_OBJECTS=1 '{}' 2>&1", binary.string());
    auto out = run_capture(cmd);
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
    auto bin = mcpp::xlings::paths::xim_tool(env, "patchelf",
        mcpp::xlings::pinned::kPatchelfVersion) / "bin" / "patchelf";
    if (std::filesystem::exists(bin)) return bin;
    // Fallback: scan all versions (in case a different version is installed).
    auto root = mcpp::xlings::paths::xim_tool_root(env, "patchelf");
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
    std::string extra;
    if (auto flag = mcpp::build::loader::patchelf_flag(
            mcpp::build::loader::required_tag(form)))
        extra = std::format(" {}", *flag);
    auto cmd = std::format("'{}' --set-rpath '{}'{} '{}'",
        patchelf.string(), rpath, extra, object.string());
    int rc = run_silent(cmd);
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
    auto cmd = std::format("'{}' --set-interpreter '{}' '{}'",
        patchelf.string(), interp, binary.string());
    int rc = run_silent(cmd);
    if (rc != 0) return std::unexpected(std::format(
        "patchelf --set-interpreter failed (exit {}): {}", rc, binary.string()));
    return {};
}

// Remove the program's debug information — and ONLY the program's.
//
// A bundled `.so` is somebody else's file: it came out of the store or off the
// host, mcpp did not build it, and stripping it would change a shared payload's
// bytes for no gain to this bundle. dh_strip draws the same line (a package
// strips what it built).
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
                          std::string_view loaderName)
{
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
        "exec \"$here/lib/{}\" --library-path \"$here/lib\" \"$here/bin/{}\" \"$@\"\n",
        loaderName, binaryName);
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
stage_runtime_files(const Plan& plan, const std::filesystem::path& stagedExeDir)
{
    const auto builtDir = plan.builtBinary.parent_path();
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
              std::string_view prefix)
{
    auto skipped = [&](const std::string& name) {
        const auto leaf = std::filesystem::path(name).filename().string();
        const bool skip = soname_matches(name, plan.alsoSkipLibs)
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
             const std::filesystem::path& tarballPath)
{
    std::error_code ec;
    if (tarballPath.has_parent_path())
        std::filesystem::create_directories(tarballPath.parent_path(), ec);
    // Pack with a top-level wrapper directory whose name matches the
    // tarball stem (computed by make_plan via wrapper_dirname_from_tarball).
    // This keeps click-to-extract and `tar -xzf` aligned: both surface a
    // single self-contained directory in the user's cwd.
    // `--force-local` on Windows: GNU tar reads `C:/path/x.tar.gz` as the rsh
    // form `host:path` and dies with `Cannot connect to C: resolve failed`,
    // naming neither its argument nor the drive letter. Latent here rather than
    // observed — a PE target is written as a zip — but it is the same command
    // with the same hazard as the library packer's, and the two should not
    // differ in whether they survive being run on Windows.
    auto cmd = std::format(
        "tar {}-czf '{}' -C '{}' '{}'",
        mcpp::platform::is_windows ? "--force-local " : "",
        tarballPath.string(),
        stagingRoot.parent_path().string(),
        stagingRoot.filename().string());
    auto r = run_capture(cmd);
    if (!r) return std::unexpected(Error{r.error()});
    return {};
}

} // namespace detail

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
                for (auto const& dir : in.searchDirs)
                    if (is_file(dir / name)) { hit = dir / name; break; }
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
        if (auto r = stage_closure(plan, read, plan.stagingRoot, needs, unresolved, {}); !r)
            return std::unexpected(r.error());
        if (!unresolved.empty())
            return std::unexpected(Error{unresolved_reason(plan.binaryName, unresolved,
                "The executable's import table could not be read.")});
    }

    if (auto r = strip_program(plan, stagedExe); !r) return std::unexpected(r.error());

    if (plan.opts.format != Format::Tar) return needs;

    std::vector<mcpp::pack::zip::Entry> entries;
    const auto wrapper = plan.stagingRoot.filename().string();
    for (auto const& e :
         std::filesystem::recursive_directory_iterator(plan.stagingRoot, ec)) {
        if (!e.is_regular_file(ec)) continue;
        auto rel = std::filesystem::relative(e.path(), plan.stagingRoot, ec);
        if (ec) continue;
        entries.push_back({
            wrapper + "/" + rel.generic_string(),
            e.path(),
            e.path().filename() == plan.builtBinary.filename(),
        });
    }
    // Deterministic order: a directory iteration order that leaks into an
    // archive is how two identical builds get two different checksums.
    //
    // Plain `std::sort` with an explicit comparator, not
    // `std::ranges::sort(entries, {}, &Entry::name)`. A ranges projection
    // naming a member of an IMPORTED type is one of three module-boundary
    // shapes this function used to carry, and together they crashed the clang
    // 20.1.7 frontend (0xC0000005, no diagnostic) on every Windows job. See
    // the note at the top of run_pe.
    std::sort(entries.begin(), entries.end(),
              [](const mcpp::pack::zip::Entry& a,
                 const mcpp::pack::zip::Entry& b) { return a.name < b.name; });
    if (auto r = mcpp::pack::zip::write(plan.archivePath, entries); !r)
        return std::unexpected(Error{r.error()});
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

    if (plan.opts.format != Format::Tar) return {};
    return make_tarball(plan.stagingRoot, plan.archivePath);
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
                        plan.builtBinary, primarySearch, plan.opts.platformLibraryDirs});
        for (auto const& leg : plan.extraSharedLegs)
            legs.push_back({plan.stagingRoot / "lib" / leg.abi, leg.abi, leg.artifact,
                            leg.searchDirs, leg.platformDirs});
    } else {
        legs.push_back({plan.stagingRoot / "lib", {}, plan.builtBinary, primarySearch,
                        plan.opts.platformLibraryDirs});
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
        if (auto r = stage_closure(plan, read, leg.dir, needs, unresolved, prefix); !r)
            return std::unexpected(r.error());
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

    if (plan.opts.format == Format::Tar)
        if (auto r = make_tarball(plan.stagingRoot, plan.archivePath); !r)
            return std::unexpected(r.error());
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
            in.arch   = t ? t->arch : std::string(mcpp::platform::host_arch);
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
        if (plan.opts.format == Format::Tar)
            if (auto r = make_tarball(plan.stagingRoot, plan.archivePath); !r)
                return std::unexpected(r.error());
        return ClosureResult{true, {}, std::move(needs)};
    }

#if defined(_WIN32)
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
#else
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
                // Mode B writes BOTH `run.sh` and `<binary_name>` at the
                // bundle root — same content, different names — so users
                // can pick whichever entry point they prefer.
                if (auto r = write_bundle_all_wrappers(plan.stagingRoot,
                        plan.binaryName, loader); !r)
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

    // A dispatched format reaches this point with an incomplete closure; an
    // archive was refused before any file was edited.
    if (!unresolvedText.empty())
        return finish_without_closure(plan, unresolvedText, std::move(needs));

    // Output.
    if (plan.opts.format == Format::Tar) {
        if (auto r = make_tarball(plan.stagingRoot, plan.archivePath); !r)
            return std::unexpected(r.error());
    }
    return ClosureResult{true, {}, std::move(needs)};
#endif // !_WIN32
}

} // namespace mcpp::pack
