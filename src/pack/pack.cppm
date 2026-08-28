// mcpp.pack — bundle a built binary into a self-contained release archive.
//
// TWO OUTPUT FAMILIES, ONE PIPELINE. An ELF/Mach-O artifact becomes a
// `.tar.gz` whose libraries live in `lib/` and are reached through a rewritten
// RUNPATH; a PE artifact becomes a `.zip` whose DLLs sit BESIDE the .exe,
// because that is where the Win32 loader looks and PE has no rpath to rewrite.
// Same contract, same modes, different mechanism — see
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
// See docs/35-pack-design.md for the full design. Three modes:
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
import mcpp.pack.strip;
import mcpp.pack.zip;
import mcpp.platform;
import mcpp.platform.xlings;
import mcpp.manifest;

export namespace mcpp::pack {

enum class Mode { None, Static, BundleProject, BundleAll };

enum class Format { Tar, Dir };

struct Options {
    Mode                            mode         = Mode::BundleProject;
    Format                          format       = Format::Tar;
    std::filesystem::path           output;        // empty = derive from manifest
    std::string                     targetTriple;  // empty = host
    // Where a dependency NAME may be resolved to a file.
    //
    // Only used where the closure is read STATICALLY (PE): on ELF the loader
    // hands back resolved paths and no search is performed here. Deliberately
    // never the target's own system directories — a DLL that resolves only
    // there is the target's to provide, and copying one is a broken program
    // rather than a heavier one.
    std::vector<std::filesystem::path> depSearchDirs;
    // The TOOLCHAIN's own runtime directory: libstdc++'s for gcc, the MSVC
    // toolset's `VC\Redist\MSVC\<v>\<arch>\Microsoft.VC*.CRT\` for cl.
    // Searched ONLY under the toolchain-coupled contract — see make_plan.
    std::vector<std::filesystem::path> toolchainRuntimeDirs;
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
    // The search set the PE closure resolves names against, after the
    // contract has had its say (see make_plan).
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
          std::span<const mcpp::manifest::RuntimeRequirement> resolvedRequirements = {});

// Execute the plan: copies binary + .so + extra files, runs patchelf,
// writes the final tarball or directory.
std::expected<void, Error>
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
          std::span<const mcpp::manifest::RuntimeRequirement> resolvedRequirements)
{
    Plan p;
    p.opts            = opts;
    p.projectRoot     = projectRoot;
    p.builtBinary     = builtBinary;
    p.binaryName      = builtBinary.filename().string();
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

std::expected<std::vector<ResolvedDep>, std::string>
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

    std::vector<ResolvedDep> deps;
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
            // "not found" → mcpp can't ship a lib it can't see.
            if (rest == "not found") continue;
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
        deps.push_back(std::move(d));
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

// ─── PE: the closure, read rather than executed ─────────────────────────
//
// BFS over the import tables, resolving each name against `searchDirs`. A
// name that resolves NOWHERE is deliberately not an error: on a Linux host
// `kernel32.dll` has no file to find, and on a Windows host it would resolve
// only in the system directory, which is not searched. Both are the same
// answer — the target provides it — and both are correct.
std::vector<ResolvedDep>
pe_closure(const std::filesystem::path& binary,
           std::span<const std::filesystem::path> searchDirs,
           const std::vector<std::string>& forceBundle)
{
    namespace bf = mcpp::pack::binfmt;
    std::vector<ResolvedDep> out;
    std::set<std::string> seen;               // lowercased: PE names are not
    std::vector<std::filesystem::path> queue{binary};

    auto lower = [](std::string_view s) {
        std::string l(s);
        std::ranges::transform(l, l.begin(),
                               [](unsigned char c) { return std::tolower(c); });
        return l;
    };

    while (!queue.empty()) {
        auto current = queue.back();
        queue.pop_back();
        auto names = bf::needed_names(current);
        if (!names) continue;                 // unreadable: nothing to add
        for (auto const& name : *names) {
            auto key = lower(name);
            if (!seen.insert(key).second) continue;
            // `[pack] force_bundle` is the escape hatch, and it has to reach
            // the SYSTEM list too — on ELF it always did. Shipping a Windows
            // component is normally a broken program rather than a heavier
            // one, so this is a decision a human has to make explicitly; when
            // they have, mcpp does not know better than them.
            if (bf::is_system_lib(bf::Format::Pe, name)
                && !soname_matches(name, forceBundle))
                continue;
            std::error_code ec;
            for (auto const& dir : searchDirs) {
                auto cand = dir / name;
                if (!std::filesystem::is_regular_file(cand, ec)) continue;
                out.push_back({name, cand});
                // Transitive: a bundled DLL brings its own imports, and a
                // closure that stops at depth one ships a package that starts
                // failing one link further in.
                queue.push_back(cand);
                break;
            }
        }
    }
    std::sort(out.begin(), out.end(),
              [](const ResolvedDep& a, const ResolvedDep& b) {
                  return a.soname < b.soname;
              });
    return out;
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
// ⚠️ THIS FUNCTION ONCE CRASHED THE COMPILER, and the shape it crashed on is
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
std::expected<void, Error>
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

    copy_if_exists(plan.projectRoot / "README.md", plan.stagingRoot);
    copy_if_exists(plan.projectRoot / "LICENSE",   plan.stagingRoot);
    if (!plan.hostRequirements.empty()) {
        std::ofstream out(plan.stagingRoot / std::filesystem::path(kFileName));
        if (!out) return std::unexpected(Error{std::format(
            "cannot write {} into the bundle", kFileName)});
        out << render(plan.hostRequirements);
    }

    std::vector<ResolvedDep> deps;
    if (plan.opts.mode != Mode::None && plan.opts.mode != Mode::Static) {
        // `vendored` and `self-contained` collect the same set here, and that
        // is a property of the PLATFORM rather than a simplification.
        // `self-contained` on ELF means "ship the loader too"; on PE there is
        // no loader to ship and kernel32.dll and friends may not be
        // redistributed — a process that loaded a private copy of one would
        // have two of something that must be unique. So the ceiling on
        // "everything" is the same for both modes: every dependency mcpp is
        // ALLOWED to carry.
        deps = pe_closure(stagedExe, plan.searchDirs, plan.forceBundleLibs);
        for (auto const& d : deps) {
            if (soname_matches(d.soname, plan.alsoSkipLibs)
                && !soname_matches(d.soname, plan.forceBundleLibs))
                continue;
            auto dst = plan.stagingRoot / d.soname;
            // The build may already have staged it beside the artifact, in
            // which case source and destination are the same file.
            std::error_code cec;
            if (std::filesystem::equivalent(d.path, dst, cec)) continue;
            std::filesystem::copy_file(d.path, dst,
                std::filesystem::copy_options::overwrite_existing, cec);
            if (cec) return std::unexpected(Error{std::format(
                "failed to copy {} → {}: {}",
                d.path.string(), dst.string(), cec.message())});
        }
    }

    if (auto r = strip_program(plan, stagedExe); !r) return r;

    if (plan.opts.format != Format::Tar) return {};

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
    return {};
}

} // namespace detail

std::expected<void, Error>
run(const Plan& plan, const mcpp::config::GlobalConfig& cfg)
{
    // A PE package is produced the same way on every host, because nothing in
    // that path executes the artifact. This is the branch the `#if
    // defined(_WIN32)` refusal used to occupy — and it was never really about
    // the host: `LD_TRACE_LOADED_OBJECTS` cannot trace a PE from Linux either.
    if (plan.targetIsPe) return detail::run_pe(plan);

    // A Mach-O artifact is REFUSED, on every host including macOS.
    //
    // The closure below asks the dynamic linker for the dependency list by
    // running the artifact with `LD_TRACE_LOADED_OBJECTS=1`. That variable
    // belongs to glibc's ld.so; dyld has never heard of it (its counterpart is
    // `DYLD_PRINT_LIBRARIES`), so on macOS the command does not trace anything
    // — IT RUNS THE USER'S PROGRAM. Whatever that program prints is then parsed
    // as a dependency table, which yields nothing, and the bundle is written
    // and reported as `Packed`. A program with side effects performs them; an
    // interactive one hangs the packer.
    //
    // ASKED OF THE FORMAT, NOT OF THE HOST — the same correction the `_WIN32`
    // branch below already carries. `LD_TRACE_LOADED_OBJECTS` cannot trace a
    // Mach-O from Linux either, and a macOS host is not the thing that makes
    // this impossible.
    //
    // docs/02 lists macOS bundling under "Planned Support"; until it lands,
    // saying so is strictly better than producing an empty bundle that claims
    // to be one.
    //
    // NAMES THE ARTIFACT. Not decoration: `mcpp pack <name>` routes on the
    // target's kind, and a refusal that does not say WHICH program it got to is
    // indistinguishable from one that resolved the wrong target — which is the
    // exact defect `route_pack_target` exists to prevent. It is also the only
    // way an e2e can check that routing on macOS, where no program bundle can
    // be produced to inspect.
    if (mcpp::pack::binfmt::identify(plan.builtBinary).format
        == mcpp::pack::binfmt::Format::MachO) {
        return std::unexpected(Error{std::format(
            "cannot package the Mach-O program '{}' yet.\n", plan.binaryName) +
            "       The dependency closure for that format is resolved by running the "
            "artifact under\n"
            "       the target's own dynamic linker, and the mechanism mcpp uses "
            "(LD_TRACE_LOADED_OBJECTS)\n"
            "       is glibc's — dyld ignores it and simply RUNS the program, which is "
            "why this is\n"
            "       refused rather than attempted.\n"
            "       A `kind = \"lib\"` / `\"shared\"` target packs normally on macOS "
            "(`mcpp pack <lib-target>`);\n"
            "       for a program, ship the build tree or use a platform bundler until "
            "macOS support lands."});
    }

#if defined(_WIN32)
    // A NON-PE artifact on a Windows host: a cross build to Linux or macOS.
    // The closure below asks the dynamic linker by running the binary, which
    // this machine cannot do — so say that, rather than reporting a platform
    // limitation that no longer exists for the case a Windows user is
    // actually likely to hit.
    (void)cfg;
    return std::unexpected(Error{std::format(
        "cannot package a {} artifact from a Windows host.\n"
        "       The dependency closure for that format is resolved by running "
        "the artifact under\n"
        "       the target's own dynamic linker, which this machine has no way "
        "to do. Package it\n"
        "       on the target OS, or build for Windows (`--target "
        "x86_64-pc-windows-msvc`), which\n"
        "       needs no such step.",
        std::string(mcpp::pack::binfmt::format_name(
            mcpp::pack::binfmt::identify(plan.builtBinary).format)))});
#else
    using namespace detail;
    std::error_code ec;

    // 1. Wipe + recreate staging dir for a clean snapshot.
    std::filesystem::remove_all(plan.stagingRoot, ec);
    std::filesystem::create_directories(plan.stagingRoot / "bin", ec);
    if (ec) return std::unexpected(Error{std::format(
        "cannot create staging '{}': {}", plan.stagingRoot.string(), ec.message())});

    // 2. Main binary.
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

    // 3. README / LICENSE if present at project root.
    copy_if_exists(plan.projectRoot / "README.md", plan.stagingRoot);
    copy_if_exists(plan.projectRoot / "LICENSE",   plan.stagingRoot);

    // 3b. What the TARGET must provide.
    //
    // Only written when there is something to say — an empty file would be
    // read as "nothing is needed", which is a claim, and for most programs the
    // absence of the file is the honest form of it. When it IS written it is
    // load-bearing: a bundle that omits the driver without saying so is a
    // bundle that fails on the user's machine with no way to find out why.
    if (!plan.hostRequirements.empty()) {
        std::ofstream out(plan.stagingRoot / std::filesystem::path(kFileName));
        if (!out) return std::unexpected(Error{std::format(
            "cannot write {} into the bundle", kFileName)});
        out << render(plan.hostRequirements);
        if (!out) return std::unexpected(Error{std::format(
            "failed writing {}", kFileName)});
    }

    // 4. Library bundling for non-static modes.
    //
    //    BundleProject (default) — drop all manylinux-allowed system libs
    //    (libc/libstdc++/ld-linux/...) and bundle the rest. The user can
    //    extend the skip list via [pack.bundle-project].also_skip /
    //    .force_bundle.
    //
    //    BundleAll — ship every dep including the dynamic linker; entry
    //    point becomes `run.sh` which invokes the bundled ld with
    //    --library-path → fully portable across glibc versions.
    if (plan.opts.mode != Mode::Static) {
        // ⚠️ THE BUILT BINARY, NOT THE STAGED COPY, and the difference is
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
        auto deps = ldd_parse(plan.builtBinary);
        if (!deps) return std::unexpected(Error{std::format(
            "ldd failed on {}: {}", plan.builtBinary.string(), deps.error())});

        std::vector<ResolvedDep> toBundle;
        for (auto& d : *deps) {
            bool skip = false;
            if (plan.opts.mode == Mode::None) {
                skip = true;  // system: host provides every .so, bundle nothing
            } else if (plan.opts.mode == Mode::BundleProject) {
                if (is_system_lib(d.soname))                          skip = true;
                if (soname_matches(d.soname, plan.alsoSkipLibs))      skip = true;
                if (soname_matches(d.soname, plan.forceBundleLibs))   skip = false;  // override
            }
            // Mode::BundleAll: skip nothing — we want the loader too.
            if (!skip) toBundle.push_back(d);
        }

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
                if (auto soname = find_loader_soname(*deps); !soname.empty()) {
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

    // 4b. Debug information does not travel either.
    //
    // AFTER every byte-changing step above (patchelf's search path, PT_INTERP)
    // and before the archive: strip must see the final image, and the archive
    // must see the stripped one. Same ordering rule the library packer states
    // at its leg loop.
    if (auto r = strip_program(plan, bundledBinary); !r) return r;

    // 5. Output.
    if (plan.opts.format == Format::Tar) {
        if (auto r = make_tarball(plan.stagingRoot, plan.archivePath); !r)
            return r;
    }
    return {};
#endif // !_WIN32
}

} // namespace mcpp::pack
