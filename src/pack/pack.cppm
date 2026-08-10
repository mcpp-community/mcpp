// mcpp.pack — bundle a built binary into a self-contained release tarball.
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
import mcpp.pack.host_requirements;
import mcpp.platform;
import mcpp.xlings;
import mcpp.manifest;

export namespace mcpp::pack {

enum class Mode { None, Static, BundleProject, BundleAll };

enum class Format { Tar, Dir };

struct Options {
    Mode                            mode         = Mode::BundleProject;
    Format                          format       = Format::Tar;
    std::filesystem::path           output;        // empty = derive from manifest
    std::string                     targetTriple;  // empty = host
};

// Resolved plan — all paths absolute, all decisions baked in.
struct Plan {
    Options                              opts;
    std::filesystem::path                projectRoot;
    std::filesystem::path                builtBinary;       // mcpp build artefact
    std::string                          binaryName;        // basename(builtBinary)
    std::filesystem::path                stagingRoot;       // target/dist/<root-dir>/
    std::filesystem::path                tarballPath;       // staging_root parent / <name>.tar.gz
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

// Default tarball name: `<name>-<version>-<triple>[-<mode>].tar.gz`.
// Mode suffix only for non-default modes so adjacent builds of different
// modes don't stomp each other in target/dist/.
std::string default_tarball_name(std::string_view name, std::string_view version,
                                 std::string_view triple, Mode mode)
{
    auto sfx = mode_tarball_suffix(mode);
    if (sfx.empty())
        return std::format("{}-{}-{}.tar.gz", name, version, triple);
    return std::format("{}-{}-{}-{}.tar.gz", name, version, triple, sfx);
}

// Strip the `.tar.gz` (or `.tgz`) suffix from a tarball filename to get
// the canonical wrapper-directory name. The result names both the disk
// staging dir and the top-level entry inside the archive — keeping the
// two in lock-step makes click-to-extract behave the way users expect.
std::string wrapper_dirname_from_tarball(const std::filesystem::path& tarball) {
    auto name = tarball.filename().string();
    for (auto suffix : {std::string_view{".tar.gz"}, std::string_view{".tgz"}}) {
        if (name.ends_with(suffix)) return name.substr(0, name.size() - suffix.size());
    }
    // No recognised compression suffix — fall back to the bare stem.
    return tarball.stem().string();
}

} // namespace detail

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

    auto distDir = projectRoot / "target" / "dist";
    if (opts.output.empty()) {
        p.tarballPath = distDir / detail::default_tarball_name(
            p.packageName, p.packageVersion, p.triple, opts.mode);
    } else if (!opts.output.has_parent_path()) {
        // `-o name.tar.gz` (bare filename) → place in target/dist/.
        // `-o ./name.tar.gz` or `-o sub/name.tar.gz` → use as-is.
        // `-o /abs/path.tar.gz` → use as-is.
        p.tarballPath = distDir / opts.output;
    } else {
        p.tarballPath = opts.output;
    }
    // Derive the staging dir from the tarball stem so the in-archive
    // wrapper directory and the on-disk staging dir share one name —
    // matches what GUI extractors create on click and what `tar -xzf`
    // produces on the CLI.
    p.stagingRoot = distDir / detail::wrapper_dirname_from_tarball(p.tarballPath);

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
    auto cmd = std::format(
        "tar -czf '{}' -C '{}' '{}'",
        tarballPath.string(),
        stagingRoot.parent_path().string(),
        stagingRoot.filename().string());
    auto r = run_capture(cmd);
    if (!r) return std::unexpected(Error{r.error()});
    return {};
}

} // namespace detail

std::expected<void, Error>
run(const Plan& plan, const mcpp::config::GlobalConfig& cfg)
{
#if defined(_WIN32)
    // `mcpp pack` is not yet supported on Windows.
    //
    // The current implementation relies on POSIX-only tools:
    //   - LD_TRACE_LOADED_OBJECTS=1  (ELF dynamic linker trick; no equivalent
    //                                 on Windows PE/COFF)
    //   - ldd / patchelf             (Linux ELF tools; not available on Windows)
    //   - tar -czf                   (GNU tar; not universally present on Windows)
    //
    // For CI-produced Windows zip packages, use the ci-windows.yml workflow
    // which zips the MSVC/Clang build output directly.
    //
    // Windows PE packaging (DLL collection + zip) is planned.
    // See .agents/docs/2026-05-19-pack-windows-design.md for the design.
    (void)plan;
    (void)cfg;
    return std::unexpected(Error{
        "error: `mcpp pack` is not yet supported on Windows.\n"
        "       Use the CI workflow (ci-windows.yml) to produce Windows zip packages.\n"
        "       Windows PE packaging (DLL collection + zip) is planned."
    });
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
        auto deps = ldd_parse(bundledBinary);
        if (!deps) return std::unexpected(Error{std::format(
            "ldd failed on {}: {}", bundledBinary.string(), deps.error())});

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

        auto patchelf = sandbox_patchelf(cfg);
        if (!patchelf.empty()) {
            // Search path: point at bundled libs (or clear if none).
            //   non-empty bundle → "$ORIGIN/../lib" so the binary finds them
            //   empty bundle      → clear the original dev-sandbox RUNPATH
            //                       (~/.mcpp/registry/... doesn't exist on
            //                       a user's target machine)
            const char* rpath = toBundle.empty() ? "" : "$ORIGIN/../lib";
            if (auto r = set_search_path(bundledBinary, rpath,
                                         mcpp::build::loader::Form::Executable,
                                         patchelf); !r)
                return std::unexpected(Error{r.error()});

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

    // 5. Output.
    if (plan.opts.format == Format::Tar) {
        if (auto r = make_tarball(plan.stagingRoot, plan.tarballPath); !r)
            return r;
    }
    return {};
#endif // !_WIN32
}

} // namespace mcpp::pack
