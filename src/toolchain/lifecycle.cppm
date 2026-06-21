// mcpp.toolchain.lifecycle — toolchain lifecycle operations
// (list / install / set-default / remove) + version-spec matching.
// Bodies moved verbatim from the CLI layer. Zero behavior change.

module;
#include <cstdio>
#include <cstdlib>

export module mcpp.toolchain.lifecycle;

import std;
import mcpp.config;
import mcpp.fetcher;
import mcpp.fetcher.progress;
import mcpp.manifest;
import mcpp.platform;
import mcpp.toolchain.detect;
import mcpp.toolchain.registry;
import mcpp.toolchain.post_install;
import mcpp.ui;
import mcpp.log;
import mcpp.xlings;

namespace mcpp::toolchain {

// ─── Toolchain version-spec helpers ──────────────────────────────────
//
// Partial versions: `mcpp toolchain install gcc 15` must match
// the latest installed/available 15.x.y, `gcc 15.1` matches the latest
// 15.1.y, etc. Accept either `<comp> <ver>` (two positionals) or `<comp>@<ver>`
// (one positional with `@`) — both forms are normalised here.

// Split "X.Y.Z…" into integer components. A trailing "-musl" (or any other
// non-numeric tail) is dropped — the caller has already handled the libc
// flavour and we only care about the numeric prefix for matching.
std::vector<int> parse_version_components(std::string_view s) {
    std::vector<int> out;
    int cur = 0;
    bool any = false;
    for (char c : s) {
        if (c >= '0' && c <= '9') { cur = cur * 10 + (c - '0'); any = true; }
        else if (c == '.') {
            if (any) { out.push_back(cur); cur = 0; any = false; }
            else     { out.clear(); break; }
        } else {
            break; // non-numeric tail (e.g. "-musl")
        }
    }
    if (any) out.push_back(cur);
    return out;
}

// Pick the version from `available` that best matches `partial`:
//   ""       → highest version overall
//   "15"     → highest 15.X.Y
//   "15.1"   → highest 15.1.Y
//   "15.1.0" → exact match (or empty if not present)
// Empty result = no match.
std::optional<std::string>
resolve_version_match(std::string_view partial,
                      std::vector<std::string> available)
{
    if (available.empty()) return std::nullopt;
    auto want = parse_version_components(partial);
    auto matches = [&](const std::vector<int>& cand) {
        if (want.size() > cand.size()) return false;
        for (std::size_t i = 0; i < want.size(); ++i)
            if (cand[i] != want[i]) return false;
        return true;
    };
    std::optional<std::string>      best;
    std::vector<int>                bestVec;
    for (auto& v : available) {
        auto comps = parse_version_components(v);
        if (comps.empty()) continue;
        if (!matches(comps)) continue;
        if (!best || std::lexicographical_compare(
                bestVec.begin(), bestVec.end(), comps.begin(), comps.end()))
        {
            best    = v;
            bestVec = std::move(comps);
        }
    }
    return best;
}

// Enumerate installed `<pkgsDir>/xim-x-<name>/<version>/` subdirs.
std::vector<std::string>
list_installed_versions(const std::filesystem::path& pkgsDir,
                        std::string_view ximName)
{
    std::vector<std::string> out;
    auto root = pkgsDir / std::format("xim-x-{}", ximName);
    std::error_code ec;
    if (!std::filesystem::exists(root, ec)) return out;
    for (auto& v : std::filesystem::directory_iterator(root, ec)) {
        if (v.is_directory(ec)) out.push_back(v.path().filename().string());
    }
    return out;
}

// Look up available versions for `xim:<name>` from the locally synced index.
// Falls back to an empty list silently — the caller will then either error
// out with a clear message or just keep the partial as-is.
//
// Index layout in mcpp's sandbox is two-tier:
//   <reg>/data/xim-pkgindex/pkgs/<n[0]>/<name>.lua          — primary
//   <reg>/data/xim-index-repos/<sub-index>/pkgs/<n[0]>/<name>.lua
// We scan both so a package living in either tier resolves.
std::vector<std::string>
list_available_xpkg_versions(const mcpp::config::GlobalConfig& cfg,
                             std::string_view ximName)
{
    if (ximName.empty()) return {};
    std::string subdir(1, ximName[0]);
    std::string fname = std::string(ximName) + ".lua";

    auto try_load = [&](const std::filesystem::path& p)
        -> std::optional<std::vector<std::string>>
    {
        std::error_code ec;
        if (!std::filesystem::exists(p, ec)) return std::nullopt;
        std::ifstream is(p);
        std::string body((std::istreambuf_iterator<char>(is)), {});
        return mcpp::manifest::list_xpkg_versions(body, "linux");
    };

    auto data = cfg.xlingsHome() / "data";
    if (auto v = try_load(data / "xim-pkgindex" / "pkgs" / subdir / fname); v)
        return std::move(*v);

    std::error_code ec;
    auto repos = data / "xim-index-repos";
    if (std::filesystem::exists(repos, ec)) {
        for (auto& repo : std::filesystem::directory_iterator(repos, ec)) {
            auto cand = repo.path() / "pkgs" / subdir / fname;
            if (auto v = try_load(cand); v) return std::move(*v);
        }
    }
    return {};
}

// ─── M4 #3: mcpp remove / mcpp update ───────────────────────────────────
// `cmd_remove` and `cmd_update` have moved to `mcpp.pm.commands` (PR-R5).

// ─── M4 #2: mcpp publish ────────────────────────────────────────────────
// ─── M5.5 #toolchain: mcpp toolchain install/list/default/remove ────────
//
// `mcpp toolchain` is dispatched as a single nested-subcommand handler
// (the App-level `toolchain` action calls this with the toolchain
// ParsedArgs that includes a populated `subcommand_name_`). This
// preserves the shared setup (cfg load + xlings bootstrap progress)

// `mcpp toolchain list` — installed + available toolchains.
// The toolchain that `mcpp build`/`run` actually resolves from the current
// directory. A project mcpp.toml `[toolchain]` shadows the global config
// default (see prepare.cppm resolution order), so `mcpp toolchain default`
// and `mcpp toolchain list`'s `*` — which read only the global config — can
// otherwise disagree with what a build in this directory really uses.
// (`--target` overrides are intentionally not folded in: they only take
// effect when an explicit `--target` is passed.)
struct EffectiveDefault {
    std::string spec;
    bool        fromProject = false;
};

EffectiveDefault effective_default_toolchain(const mcpp::config::GlobalConfig& cfg) {
    std::error_code ec;
    auto mpath = std::filesystem::current_path(ec) / "mcpp.toml";
    if (!ec && std::filesystem::exists(mpath, ec)) {
        if (auto m = mcpp::manifest::load(mpath)) {
            if (auto t = m->toolchain.for_platform(mcpp::platform::name);
                t && !t->empty())
                return { *t, true };
        }
    }
    return { cfg.defaultToolchain, false };
}

export int toolchain_list(const mcpp::config::GlobalConfig& cfg) {
    auto pkgsDir = cfg.xlingsHome() / "data" / "xpkgs";
    auto effective = effective_default_toolchain(cfg);
        auto pathCtx = mcpp::fetcher::make_path_ctx(&cfg);
        struct Row {
            std::string compiler;
            std::string version;
            std::filesystem::path bin;
            bool isDefault = false;
        };
        std::vector<Row> installed;

        std::error_code ec;
        if (std::filesystem::exists(pkgsDir, ec)) {
            for (auto& entry : std::filesystem::directory_iterator(pkgsDir, ec)) {
                auto name = entry.path().filename().string();
                std::string compiler = name;
                if (auto sep = name.find("-x-"); sep != std::string::npos)
                    compiler = name.substr(sep + 3);
                for (auto& vEntry : std::filesystem::directory_iterator(entry.path(), ec)) {
                    auto bin = mcpp::toolchain::toolchain_frontend(
                        vEntry.path() / "bin", compiler);
                    if (bin.empty()) continue;
                    Row r;
                    r.compiler  = compiler;
                    r.version   = vEntry.path().filename().string();
                    r.bin       = bin;
                    r.isDefault = mcpp::toolchain::matches_default_toolchain(
                        effective.spec, r.compiler, r.version);
                    installed.push_back(std::move(r));
                }
            }
        }

        if (installed.empty()) {
            std::println("(no toolchains installed — run `mcpp toolchain install gcc 16.1.0` "
                         "or just `mcpp build` to auto-install the default)");
        } else {
            std::println("Installed:");
            std::println("  {:<3}{:<22}  {}", "", "TOOLCHAIN", "BINARY");
            for (auto& r : installed) {
                std::println("  {:<3}{:<22}  {}",
                    r.isDefault ? "*" : "",
                    mcpp::toolchain::display_label(r.compiler, r.version),
                    mcpp::ui::shorten_path(r.bin, pathCtx));
            }
            // Explain the `*` when it reflects a project override rather than
            // the global default, so it never silently disagrees with what a
            // build in this directory resolves.
            if (effective.fromProject) {
                std::println("  (* = effective toolchain from project mcpp.toml "
                             "[toolchain]; global default is '{}')",
                             cfg.defaultToolchain.empty() ? "<none>"
                                                          : cfg.defaultToolchain);
            }
        }

        // ─── Available section ──────────────────────────────────────────
        // List xim:gcc + xim:musl-gcc versions known to the local index
        // that aren't already installed. Helpful to discover what users
        // can install without going to a website.
        struct AvailRow { std::string compiler; std::string version; };
        std::vector<AvailRow> avail;
        auto add_avail = [&](std::string_view ximName, std::string_view compiler) {
            auto versions = list_available_xpkg_versions(cfg, ximName);
            for (auto& v : versions) {
                // xim packages declare a `["latest"] = { ref = "X.Y.Z" }`
                // alias entry; it shows up here but isn't a version users
                // can `mcpp toolchain install` directly. Skip non-numeric
                // entries.
                if (v.empty() || !std::isdigit(static_cast<unsigned char>(v[0])))
                    continue;
                bool already = std::any_of(installed.begin(), installed.end(),
                    [&](const Row& r){ return r.compiler == compiler && r.version == v; });
                if (!already) avail.push_back({std::string(compiler), v});
            }
        };
        for (auto& [ximName, compiler] : mcpp::toolchain::available_toolchain_indexes())
            add_avail(ximName, compiler);

        if (!avail.empty()) {
            // Newest first. Lexicographic-on-strings is good enough for
            // semver-shaped versions — any "X.Y.Z" with single-digit
            // segments compares the way users expect.
            std::sort(avail.begin(), avail.end(),
                [](const AvailRow& a, const AvailRow& b) {
                    if (a.compiler != b.compiler) return a.compiler < b.compiler;
                    return b.version < a.version;
                });
            if (!installed.empty()) std::println("");
            std::println("Available (run `mcpp toolchain install <compiler> <version>`):");
            std::println("  {:<3}{}", "", "TOOLCHAIN");
            for (auto& a : avail) {
                std::println("  {:<3}{}", "",
                    mcpp::toolchain::display_label(a.compiler, a.version));
            }
        }
        return 0;
    }

// `mcpp toolchain install <spec>` — install + post-install fixups.
export int toolchain_install(const mcpp::config::GlobalConfig& cfg,
                             const std::string& pos0, const std::string& pos1) {
    auto xlEnv = mcpp::config::make_xlings_env(cfg);
        // Toolchain install needs patchelf (ELF fixup) and ninja (build).
        // Fail early if bootstrap is incomplete rather than producing a
        // broken toolchain with missing fixups.
        auto bsProblem = mcpp::config::check_base_init(cfg);
        if (!bsProblem.empty()) {
            mcpp::ui::error(std::format(
                "{}\n  hint: run `mcpp self init --force` to reset and re-initialize",
                bsProblem));
            return 1;
        }

        // Accept three input shapes — they all collapse to (compiler, version):
        //   mcpp toolchain install gcc 16.1.0      → ("gcc", "16.1.0")
        //   mcpp toolchain install gcc@16.1.0      → ("gcc", "16.1.0")
        //   mcpp toolchain install gcc 15          → ("gcc", "15")  partial
        auto spec = mcpp::toolchain::parse_toolchain_spec(
            pos0, pos1);
        if (!spec || spec->compiler.empty()) {
            mcpp::ui::error("missing compiler name; e.g. `mcpp toolchain install gcc 16.1.0`");
            return 2;
        }
        auto pkg = mcpp::toolchain::to_xim_package(*spec);

        // Partial-version resolution: `gcc 15` → highest available 15.x.y in
        // the synced index. Empty version → latest of any major.
        if (auto picked = resolve_version_match(
                pkg.ximVersion, list_available_xpkg_versions(cfg, pkg.ximName))) {
            if (*picked != pkg.ximVersion) {
                mcpp::ui::info("Resolved",
                    std::format("{}@{} → {}@{}", spec->compiler, spec->version,
                                spec->compiler, *picked));
            }
            spec = mcpp::toolchain::with_resolved_xim_version(*spec, *picked);
            pkg = mcpp::toolchain::to_xim_package(*spec);
        }

        mcpp::ui::info("Installing",
            std::format("{} {} via mcpp's xlings", spec->compiler, spec->version));
        mcpp::log::verbose("toolchain", std::format(
            "install: target='{}' xlingsHome='{}'", pkg.target(), cfg.xlingsHome().string()));
        mcpp::log::debug("toolchain", std::format(
            "  ximName='{}' needsGccFixup={} xlingsBinary='{}'",
            pkg.ximName, pkg.needsGccPostInstallFixup, cfg.xlingsBinary.string()));
        mcpp::fetcher::Fetcher fetcher(cfg);
        mcpp::fetcher::InstallProgressHandler progress;

        // Ensure sysroot dependencies (glibc, linux-headers) are installed.
        // These are required for C library + kernel headers during compilation.
        // musl-gcc is self-contained and doesn't need these.
        if (!spec->isMusl) {
            for (auto dep : {"xim:glibc", "xim:linux-headers"}) {
                mcpp::log::verbose("toolchain", std::format("installing dep: {}", dep));
                auto depPayload = fetcher.resolve_xpkg_path(dep, /*autoInstall=*/true, &progress);
                mcpp::log::debug("toolchain", std::format("dep {} result: {}",
                    dep, depPayload ? "ok" : depPayload.error().message));
            }
        }

        mcpp::log::verbose("toolchain", std::format("installing main: {}", pkg.target()));
        auto payload = fetcher.resolve_xpkg_path(pkg.target(), /*autoInstall=*/true, &progress);
        mcpp::log::verbose("toolchain", std::format("main install result: {}",
            payload ? ("ok → " + payload->root.string()) : payload.error().message));
        if (!payload) {
            mcpp::ui::error(std::format("install failed: {}", payload.error().message));
            return 1;
        }

        auto bin = mcpp::toolchain::toolchain_frontend(payload->binDir, pkg);
        if (!std::filesystem::exists(bin)) {
            mcpp::ui::error(std::format(
                "installed package has no known C++ frontend in '{}'",
                payload->binDir.string()));
            return 1;
        }

        // For GNU gcc only: post-install ELF + specs fixup so the toolchain
        // is self-contained in the sandbox.
        //   1. patchelf walk: rewrites PT_INTERP of gcc/binutils binaries
        //      themselves (xim's elfpatch is supposed to but currently
        //      scans 0 files — see `patchelf_walk_set_interp` doc).
        //   2. specs fixup: rewrites the linker-spec dynamic-linker/rpath
        //      so binaries gcc *compiles* use sandbox-local paths.
        // musl-gcc ships its own self-contained sysroot at
        // `<root>/x86_64-linux-musl/{include,lib}` and doesn't link against
        // xim:glibc, so this fixup is both unnecessary and harmful for it.
        if (pkg.needsGccPostInstallFixup) {
            mcpp::toolchain::gcc_post_install_fixup(cfg, payload->root);
        }

        // For LLVM/Clang: post-install fixup so the shared libraries and
        // clang++.cfg paths point to the payload's actual location instead
        // of the xlings install-time paths (which become stale after copy).
        if (pkg.ximName == "llvm") {
            auto glibcRoot = mcpp::xlings::paths::xim_tool_root(xlEnv, "glibc");
            std::filesystem::path glibcLibDir;
            if (std::filesystem::exists(glibcRoot)) {
                for (auto& v : std::filesystem::directory_iterator(glibcRoot)) {
                    auto candidate = v.path() / "lib64";
                    if (std::filesystem::exists(candidate / "ld-linux-x86-64.so.2")) {
                        glibcLibDir = candidate;
                        break;
                    }
                    candidate = v.path() / "lib";
                    if (std::filesystem::exists(candidate / "ld-linux-x86-64.so.2")) {
                        glibcLibDir = candidate;
                        break;
                    }
                }
            }

            // patchelf: rewrite RUNPATH for LLVM runtime shared libraries
            // (libc++.so, libunwind.so, etc.) so transitive deps like
            // libatomic.so.1 are found at runtime.  Without this,
            // libc++.so.1 keeps stale xlings-era RUNPATH and cannot locate
            // libatomic.so.1 which lives in the same xpkg.
            //
            // Only walk lib/ dirs — NOT bin/. The clang++ binary has its
            // own RUNPATH set by xlings (pointing to zlib, libxml2, etc.)
            // that must be preserved.
            auto patchelfBin = mcpp::xlings::paths::xim_tool(xlEnv, "patchelf",
                mcpp::xlings::pinned::kPatchelfVersion) / "bin" / "patchelf";
            if (!glibcLibDir.empty() && std::filesystem::exists(patchelfBin)) {
                auto loader = glibcLibDir / "ld-linux-x86-64.so.2";
                auto llvmTargetLib = payload->root / "lib" / "x86_64-unknown-linux-gnu";
                auto llvmLib = payload->root / "lib";
                std::string rpath = llvmTargetLib.string()
                    + ":" + llvmLib.string()
                    + ":" + glibcLibDir.string();
                mcpp::log::verbose("toolchain", std::format(
                    "llvm fixup: patchelf_walk lib/ rpath='{}'", rpath));
                mcpp::toolchain::patchelf_walk(llvmLib, loader, rpath, patchelfBin);
            }

            mcpp::log::verbose("toolchain", "llvm fixup: fixup_clang_cfg");
            mcpp::toolchain::fixup_clang_cfg(payload->root, glibcLibDir);
        }

        mcpp::ui::status("Installed",
            std::format("{} → {}", pkg.display_spec(), bin.string()));
        if (cfg.defaultToolchain.empty()) {
            std::println("");
            std::println("Tip: `mcpp toolchain default {}@{}` to make this the default.",
                         spec->compiler, spec->version);
        }
        return 0;
    }

// `mcpp toolchain default <spec>` — persist the default toolchain.
export int toolchain_set_default(const mcpp::config::GlobalConfig& cfg,
                                 const std::string& pos0, const std::string& pos1) {
    auto pkgsDir = cfg.xlingsHome() / "data" / "xpkgs";
    auto xlEnv = mcpp::config::make_xlings_env(cfg);
        // Accept three input shapes (mirrors `install`):
        //   mcpp toolchain default gcc@16.1.0
        //   mcpp toolchain default gcc 16.1.0
        //   mcpp toolchain default gcc 15        ← partial; picks highest 15.x.y
        auto spec = mcpp::toolchain::parse_toolchain_spec(
            pos0, pos1);
        if (!spec || spec->compiler.empty()) {
            mcpp::ui::error("missing spec; e.g. `mcpp toolchain default gcc@16.1.0`");
            return 2;
        }
        auto pkg = mcpp::toolchain::to_xim_package(*spec);

        // Partial-version resolution against installed payloads.
        if (auto picked = resolve_version_match(
                pkg.ximVersion, list_installed_versions(pkgsDir, pkg.ximName))) {
            spec = mcpp::toolchain::with_resolved_xim_version(*spec, *picked);
            pkg = mcpp::toolchain::to_xim_package(*spec);
        }

        // Reconstruct the user-visible spec for messages / config.toml.
        std::string displaySpec = pkg.display_spec();

        auto installDir = mcpp::xlings::paths::xim_tool(xlEnv, pkg.ximName, pkg.ximVersion);
        if (!std::filesystem::exists(installDir)) {
            mcpp::ui::error(std::format(
                "{} is not installed. Run `mcpp toolchain install {} {}` first.",
                displaySpec, spec->compiler,
                spec->version.empty() ? pkg.ximVersion : spec->version));
            return 1;
        }
        auto wr = mcpp::config::write_default_toolchain(cfg, displaySpec);
        if (!wr) {
            mcpp::ui::error(wr.error().message);
            return 1;
        }
        mcpp::ui::status("Default", std::format(
            "set to {} (was: {})", displaySpec,
            cfg.defaultToolchain.empty() ? "<none>" : cfg.defaultToolchain));
        return 0;
    }

// `mcpp toolchain remove <spec>` — uninstall a toolchain payload.
export int toolchain_remove(const mcpp::config::GlobalConfig& cfg,
                            const std::string& pos0) {
    auto xlEnv = mcpp::config::make_xlings_env(cfg);
        auto parsedSpec = mcpp::toolchain::parse_toolchain_spec(pos0);
        if (!parsedSpec || parsedSpec->version.empty()) {
            mcpp::ui::error(std::format("invalid spec '{}'", pos0));
            return 2;
        }
        auto pkg = mcpp::toolchain::to_xim_package(*parsedSpec);
        auto spec = pkg.display_spec();
        auto installDir = mcpp::xlings::paths::xim_tool(xlEnv, pkg.ximName, pkg.ximVersion);
        std::error_code ec;
        if (!std::filesystem::exists(installDir, ec)) {
            mcpp::ui::error(std::format("{} is not installed", spec));
            return 1;
        }
        std::filesystem::remove_all(installDir, ec);
        if (ec) {
            mcpp::ui::error(std::format("remove failed: {}", ec.message()));
            return 1;
        }
        mcpp::ui::status("Removed", spec);
        if (cfg.defaultToolchain == spec) {
            mcpp::ui::warning(std::format(
                "default toolchain '{}' was just removed; consider `mcpp toolchain default <other>`",
                spec));
        }
        return 0;
    }

} // namespace mcpp::toolchain
