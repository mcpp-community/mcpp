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
#include <cstdio>      // popen, pclose

export module mcpp.pack;

import std;
import mcpp.config;
import mcpp.xlings;
import mcpp.manifest;

export namespace mcpp::pack {

enum class Mode { Static, BundleProject, BundleAll };

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
          std::string_view triple);

// Execute the plan: copies binary + .so + extra files, runs patchelf,
// writes the final tarball or directory.
std::expected<void, Error>
run(const Plan& plan, const mcpp::config::GlobalConfig& cfg);

// Helpers used by cli.cppm to render mode names + parse `--mode`.
std::string_view mode_name(Mode m);
std::optional<Mode> parse_mode(std::string_view s);

} // namespace mcpp::pack

namespace mcpp::pack {

std::string_view mode_name(Mode m) {
    switch (m) {
        case Mode::Static:        return "static";
        case Mode::BundleProject: return "bundle-project";
        case Mode::BundleAll:     return "bundle-all";
    }
    return "?";
}

std::optional<Mode> parse_mode(std::string_view s) {
    if (s == "static")         return Mode::Static;
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
    if (mode == Mode::BundleProject)
        return std::format("{}-{}-{}.tar.gz", name, version, triple);
    return std::format("{}-{}-{}-{}.tar.gz", name, version, triple, mode_name(mode));
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
          std::string_view triple)
{
    Plan p;
    p.opts            = opts;
    p.projectRoot     = projectRoot;
    p.builtBinary     = builtBinary;
    p.binaryName      = builtBinary.filename().string();
    p.packageName     = manifest.package.name;
    p.packageVersion  = manifest.package.version;
    p.triple          = std::string(triple);

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
    std::FILE* fp = ::popen(cmd.c_str(), "r");
    if (!fp) return std::unexpected("popen failed: " + cmd);
    std::string out;
    std::array<char, 4096> buf{};
    while (std::fgets(buf.data(), buf.size(), fp))
        out += buf.data();
    int rc = ::pclose(fp);
    if (rc != 0) return std::unexpected(std::format(
        "command exited with {}: {}", rc, cmd));
    return out;
}

// Run a shell command and discard stdout/stderr; return exit code.
int run_silent(const std::string& cmd) {
    auto silent = cmd + " >/dev/null 2>&1";
    return std::system(silent.c_str());
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

// Set RUNPATH (so the dynamic linker finds bundled libs in <prefix>/lib
// from anywhere). $ORIGIN is the directory of the binary at load time,
// so $ORIGIN/../lib is the bundled lib dir relative to <prefix>/bin/<exe>.
std::expected<void, std::string>
set_runpath(const std::filesystem::path& binary,
            std::string_view rpath,
            const std::filesystem::path& patchelf)
{
    if (patchelf.empty() || !std::filesystem::exists(patchelf))
        return std::unexpected("patchelf not available in sandbox");
    auto cmd = std::format("'{}' --set-rpath '{}' '{}'",
        patchelf.string(), rpath, binary.string());
    int rc = run_silent(cmd);
    if (rc != 0) return std::unexpected(std::format(
        "patchelf --set-rpath failed (exit {}): {}", rc, binary.string()));
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
        "# Auto-generated by `mcpp pack --mode bundle-all`. Launches the\n"
        "# bundled binary through the bundled dynamic linker so the package\n"
        "# is fully portable across glibc versions.\n"
        "here=$(cd \"$(dirname \"$0\")\" && pwd)\n"
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
            if (plan.opts.mode == Mode::BundleProject) {
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
            // RUNPATH: point at bundled libs (or clear if none).
            //   non-empty bundle → "$ORIGIN/../lib" so the binary finds them
            //   empty bundle      → clear the original dev-sandbox RUNPATH
            //                       (~/.mcpp/registry/... doesn't exist on
            //                       a user's target machine)
            const char* rpath = toBundle.empty() ? "" : "$ORIGIN/../lib";
            if (auto r = set_runpath(bundledBinary, rpath, patchelf); !r)
                return std::unexpected(Error{r.error()});

            // PT_INTERP handling differs by mode:
            //   BundleProject → repoint to /lib64/ld-linux-x86-64.so.2
            //                   (LSB-mandated symlink on glibc distros)
            //   BundleAll     → leave PT_INTERP alone; the wrapper script
            //                   ignores it and launches via the bundled
            //                   loader directly.
            if (plan.opts.mode == Mode::BundleProject) {
                if (auto r = set_interpreter(bundledBinary,
                        "/lib64/ld-linux-x86-64.so.2", patchelf); !r)
                    return std::unexpected(Error{r.error()});
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
}

} // namespace mcpp::pack
