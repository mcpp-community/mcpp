// mcpp.doctor — diagnostics + self-maintenance: environment report,
// health checks, resolution explanation (why), error-code explanations,
// sandbox init/reset, and xlings mirror configuration.
// Bodies moved verbatim from the CLI layer. Zero behavior change.

module;
#include <cstdio>
#include <cstdlib>

export module mcpp.doctor;

import std;
import mcpp.bmi_cache.maintenance;
import mcpp.build.prepare;
import mcpp.build.plan;
import mcpp.config;
import mcpp.fallback.install_integrity;
import mcpp.fetcher.progress;
import mcpp.platform;
import mcpp.platform.process;
import mcpp.toolchain.detect;
import mcpp.toolchain.msvc;
import mcpp.toolchain.registry;
import mcpp.toolchain.stdmod;
import mcpp.toolchain.abi;
import mcpp.ui;
import mcpp.xlings;

namespace mcpp::doctor {

// Parse the RUNPATH/RPATH search dirs out of a `readelf -d <binary>` dump.
// readelf prints (one per DT_RUNPATH / DT_RPATH dynamic entry):
//   0x...001d (RUNPATH)  Library runpath: [/a/lib:/b/lib:...]
//   0x...000f (RPATH)    Library rpath:   [/a/lib:/b/lib:...]
// We pull the text inside the [...] and split on ':'. Exported so it can be
// unit-tested without spawning a process. Empty entries are dropped.
export std::vector<std::string> parse_readelf_runpath(std::string_view dump) {
    std::vector<std::string> out;
    std::size_t pos = 0;
    while (pos < dump.size()) {
        auto nl = dump.find('\n', pos);
        std::string_view line = dump.substr(pos, nl == std::string_view::npos
            ? std::string_view::npos : nl - pos);
        pos = (nl == std::string_view::npos) ? dump.size() : nl + 1;

        if (line.find("(RUNPATH)") == std::string_view::npos
            && line.find("(RPATH)") == std::string_view::npos)
            continue;
        auto lb = line.find('[');
        auto rb = line.find(']', lb == std::string_view::npos ? 0 : lb);
        if (lb == std::string_view::npos || rb == std::string_view::npos || rb <= lb + 1)
            continue;
        std::string_view body = line.substr(lb + 1, rb - lb - 1);
        std::size_t s = 0;
        while (s <= body.size()) {
            auto c = body.find(':', s);
            std::string_view tok = body.substr(s, c == std::string_view::npos
                ? std::string_view::npos : c - s);
            if (!tok.empty()) out.emplace_back(tok);
            if (c == std::string_view::npos) break;
            s = c + 1;
        }
    }
    return out;
}

// `mcpp self env`.
export int env_report() {
    auto cfg = mcpp::config::load_or_init(/*quiet=*/false, mcpp::fetcher::make_bootstrap_progress_callback());
    if (!cfg) { mcpp::ui::error(cfg.error().message); return 4; }

    mcpp::config::print_env(*cfg);

    auto tc = mcpp::toolchain::detect();
    if (tc) {
        std::println("");
        std::println("Toolchain       = {}", tc->label());
        std::println("std module src  = {}", tc->stdModuleSource.string());
    } else {
        std::println("");
        std::println("Toolchain       = (not detected: {})", tc.error().message);
    }
    return 0;
}

// `mcpp self doctor`.
export int doctor_report() {
    int warns = 0, errors = 0;
    auto ok    = [](std::string_view m) { mcpp::ui::status("ok", m); };
    auto warn = [&](std::string_view m) { mcpp::ui::warning(m); ++warns; };
    auto err   = [&](std::string_view m) { mcpp::ui::error(m);   ++errors; };

    mcpp::ui::status("Checking", "toolchain");
    auto tc = mcpp::toolchain::detect();
    if (!tc) {
        err(std::format("toolchain detection failed: {}", tc.error().message));
    } else {
        ok(std::format("{} at {}", tc->label(), tc->binaryPath.string()));
    }

    // Windows: report the system MSVC (msvc@system). Absence is a warning,
    // not an error — mcpp works with LLVM/Clang without it, and mcpp never
    // installs MSVC itself.
    if (mcpp::platform::is_windows) {
        mcpp::ui::status("Checking", "msvc (system)");
        if (auto inst = mcpp::toolchain::msvc::detect_installation()) {
            ok(std::format("msvc {}{} (VC tools {})",
                inst->display_version(),
                inst->vsProduct.empty()
                    ? std::string{}
                    : std::format(" (VS {})", inst->vsProduct),
                inst->toolsVersion));
            ok(std::format("cl at {}", inst->clPath.string()));
            if (inst->hasStdModules) {
                ok("import std: std.ixx available");
            } else {
                warn("MSVC STL std.ixx missing (VC tools too old for import std?)");
            }
        } else {
            warn("msvc not detected — run `mcpp toolchain default msvc` for "
                 "setup guidance (mcpp does not install MSVC)");
        }
        // Windows SDK (native cl.exe builds need its UCRT/um headers).
        if (auto sdk = mcpp::toolchain::msvc::find_windows_sdk()) {
            ok(std::format("Windows SDK {} at {}", sdk->version,
                           sdk->root.string()));
        } else {
            warn("no Windows SDK found — native msvc builds will fail "
                 "(install the 'Windows 11 SDK' VS component)");
        }

        mcpp::ui::status("Checking", "mingw (xim:mingw-gcc)");
        {
            auto pkgs = std::filesystem::path(
                std::getenv("MCPP_HOME") ? std::getenv("MCPP_HOME")
                                         : (std::string(std::getenv("USERPROFILE")
                                               ? std::getenv("USERPROFILE") : "") + "\\.mcpp"))
                / "registry" / "data" / "xpkgs" / "xim-x-mingw-gcc";
            std::error_code ec;
            bool any = false;
            if (std::filesystem::exists(pkgs, ec)) {
                for (auto& v : std::filesystem::directory_iterator(pkgs, ec)) {
                    if (!v.is_directory(ec)) continue;
                    ok(std::format("mingw {} installed", v.path().filename().string()));
                    any = true;
                }
            }
            if (!any)
                ok("mingw not installed (optional — `mcpp toolchain install mingw 16.1.0`)");
        }
    }

    mcpp::ui::status("Checking", "std module");
    if (tc) {
        auto cacheRoot = mcpp::toolchain::default_cache_root();
        auto fpDir = cacheRoot;
        std::error_code ec;
        if (std::filesystem::exists(cacheRoot, ec)) {
            bool found = false;
            for (auto& e : std::filesystem::directory_iterator(cacheRoot, ec)) {
                auto stdgcm = e.path() / "std.gcm";
                if (std::filesystem::exists(stdgcm)) {
                    ok(std::format("{}  ({})",
                        stdgcm.string(),
                        mcpp::bmi_cache::human_bytes(std::filesystem::file_size(stdgcm))));
                    found = true;
                    break;
                }
            }
            if (!found) warn("no std.gcm found yet (will be built on first `mcpp build`)");
        } else {
            warn(std::format("cache root '{}' missing (run `mcpp env` to init)",
                             cacheRoot.string()));
        }
    }

    mcpp::ui::status("Checking", "registry");
    auto cfg = mcpp::config::load_or_init(/*quiet=*/false, mcpp::fetcher::make_bootstrap_progress_callback());
    if (!cfg) {
        err(cfg.error().message);
    } else {
        if (std::filesystem::exists((*cfg).xlingsBinary)) {
            ok(std::format("xlings at {}", (*cfg).xlingsBinary.string()));
        } else {
            warn(std::format("xlings binary missing at '{}'",
                             (*cfg).xlingsBinary.string()));
        }
        ok(std::format("default index = '{}'", (*cfg).defaultIndex));
    }

    mcpp::ui::status("Checking", "cache health");
    auto bmiRoot = mcpp::toolchain::default_cache_root();
    auto sz = mcpp::bmi_cache::dir_size(bmiRoot);
    if (sz > std::uintmax_t(1) * 1024 * 1024 * 1024) {
        warn(std::format("BMI cache occupies {} (run `mcpp cache prune` to GC)",
                         mcpp::bmi_cache::human_bytes(sz)));
    } else {
        ok(std::format("BMI cache size = {}", mcpp::bmi_cache::human_bytes(sz)));
    }

    mcpp::ui::status("Checking", "runtime capabilities");
    {
        // Capability/provider-driven — no platform special-casing in mcpp.
        // Required capabilities and the sonames to probe come entirely from the
        // dependency graph's provider packages (e.g. compat.glx-runtime); the
        // search dirs are the resolved runtime library_dirs. The same code path
        // works on every platform — providers carry the platform knowledge.
        auto pctx = mcpp::build::prepare_build(/*print_fingerprint=*/false);
        if (!pctx) {
            ok("(run inside a package to check its runtime capabilities)");
        } else if (pctx->plan.runtimeCapabilities.empty()) {
            ok("no host runtime capabilities required");
        } else {
            auto& plan = pctx->plan;
            for (auto& cap : plan.runtimeCapabilities) {
                std::string provider;
                for (auto& [c, p] : plan.runtimeProviders)
                    if (c == cap) { provider = p; break; }
                ok(std::format("{}: required (provider {})",
                               cap, provider.empty() ? "?" : provider));
            }
            auto resolves = [&](std::string_view soname) {
                for (auto& dir : plan.runtimeLibraryDirs) {
                    std::error_code ec;
                    if (!std::filesystem::exists(dir, ec)) continue;
                    for (auto& e : std::filesystem::directory_iterator(dir, ec)) {
                        auto fn = e.path().filename().string();
                        if (fn == soname || fn.rfind(soname, 0) == 0) return true;
                    }
                }
                return false;
            };
            for (auto& lib : plan.runtimeDlopenLibs) {
                if (resolves(lib)) ok(std::format("dlopen {}: resolvable on RUNPATH", lib));
                else warn(std::format("dlopen {}: not found on resolved runtime dirs", lib));
            }
        }
    }

#if !defined(__APPLE__) && !defined(_WIN32)
    // ─── Toolchain runtime dependencies (Linux/ELF only) ────────────────
    //
    // Installed xim toolchains bake absolute RUNPATH entries into their
    // compiler binaries (e.g. clang++ points at xim-x-zlib/.../lib for
    // libz.so.1). If the providing xim package is later removed, the
    // RUNPATH dir vanishes and `<compiler>` dies at runtime with
    // "libz.so.1: cannot open shared object" (exit 127) — the package
    // builds fine but the produced binary can't run. We detect the broken
    // state here before a build mysteriously fails.
    //
    // Two symptoms, both stemming from a deleted provider package:
    //   1. a compiler RUNPATH entry pointing at a now-missing dir, and
    //   2. dangling symlinks under <xlingsHome>/subos/default/lib
    //      (std::filesystem::exists follows symlinks → false for dangling).
    mcpp::ui::status("Checking", "toolchain runtime deps");
    if (cfg) {
        auto pkgsDir = (*cfg).xlingsHome() / "data" / "xpkgs";
        std::error_code ec;
        bool sawAny = false;
        bool anyMissing = false;

        if (std::filesystem::exists(pkgsDir, ec)) {
            // Mirror `mcpp toolchain list`: each xim-x-<name>/<version>/bin
            // holds one installed toolchain frontend (clang++/g++/musl-gcc-…).
            for (auto& entry : std::filesystem::directory_iterator(pkgsDir, ec)) {
                auto name = entry.path().filename().string();
                if (name.rfind("xim-x-", 0) != 0) continue;          // toolchains only
                auto id = mcpp::toolchain::identify_xim_payload(
                    name.substr(std::string("xim-x-").size()));
                if (!id) continue;                                   // not a compiler pkg

                for (auto& vEntry : std::filesystem::directory_iterator(entry.path(), ec)) {
                    mcpp::toolchain::ToolchainSpec s;
                    s.family  = id->family;
                    s.version = vEntry.path().filename().string();
                    s.target  = id->target;
                    auto bin = mcpp::toolchain::toolchain_frontend(
                        vEntry.path() / "bin", mcpp::toolchain::to_xim_package(s));
                    if (bin.empty()) continue;
                    sawAny = true;

                    auto label = s.display();

                    // readelf is part of binutils, always present in our sandbox.
                    auto cmd = std::format("readelf -d \"{}\"", bin.string());
                    auto r = mcpp::platform::process::capture(cmd);
                    if (r.exit_code != 0) {
                        warn(std::format(
                            "{}: could not read RUNPATH from '{}' (readelf exit {})",
                            label, bin.string(), r.exit_code));
                        continue;
                    }
                    for (auto& dir : parse_readelf_runpath(r.output)) {
                        // Only absolute paths name on-disk dirs we can verify;
                        // $ORIGIN-relative entries are resolved by the loader.
                        if (dir.empty() || dir.front() != '/') continue;
                        if (!std::filesystem::exists(dir, ec)) {
                            anyMissing = true;
                            warn(std::format(
                                "{}: RUNPATH dir missing: {}  "
                                "(its providing xim package may have been removed — "
                                "reinstall the toolchain to repair)",
                                label, dir));
                        }
                    }
                }
            }
        }
        if (sawAny && !anyMissing)
            ok("all installed toolchain RUNPATH dirs present");
        else if (!sawAny)
            ok("no installed toolchains to check");

        // Dangling symlinks under registry/subos/default/lib — these point
        // into xim payload lib dirs; a removed package leaves them broken.
        auto subosLib = (*cfg).xlingsHome() / "subos" / "default" / "lib";
        if (std::filesystem::exists(subosLib, ec)) {
            bool anyDangling = false;
            for (auto& e : std::filesystem::directory_iterator(subosLib, ec)) {
                if (!e.is_symlink(ec)) continue;
                // exists() follows the link → false when the target is gone.
                if (!std::filesystem::exists(e.path(), ec)) {
                    anyDangling = true;
                    auto target = std::filesystem::read_symlink(e.path(), ec);
                    warn(std::format(
                        "dangling subos symlink: {} -> {}  "
                        "(target's xim package may have been removed)",
                        e.path().filename().string(), target.string()));
                }
            }
            if (!anyDangling)
                ok(std::format("subos lib symlinks all resolve ({})", subosLib.string()));
        }
    }
#endif

    std::println("");
    if (errors)        std::println("Doctor result: {} errors, {} warnings", errors, warns);
    else if (warns)    std::println("Doctor result: {} warnings", warns);
    else               std::println("Doctor result: all checks passed");
    return errors ? 2 : (warns ? 1 : 0);
}

// `mcpp why [topic]` / `mcpp resolve --explain`.
export int why_report(const std::string& topic) {
    const bool all = topic.empty() || topic == "all";

    auto ctx = mcpp::build::prepare_build(/*print_fingerprint=*/false);
    if (!ctx) { std::println(stderr, "error: {}", ctx.error()); return 2; }
    auto& tc   = ctx->tc;
    auto& plan = ctx->plan;

    if (all || topic == "toolchain") {
        const auto prof = mcpp::toolchain::abi_profile(tc);
        std::println("toolchain: {}", tc.label());
        std::println("  abi(libc)={}  cxxstdlib={}  arch={}  os={}  triple={}",
                     prof.libc, prof.cxxStdlib, prof.arch, prof.os, tc.targetTriple);
        std::println("  reason: [toolchain] in mcpp.toml if set, else platform-native default");
        if (!ctx->manifest.package.platforms.empty()) {
            std::string ps;
            for (auto& p : ctx->manifest.package.platforms) {
                if (!ps.empty()) ps += ", ";
                ps += p;
            }
            std::println("  declared platforms: {}  (CI matrix hint)", ps);
        }
    }
    if (all || topic == "runtime") {
        std::println("runtime library dirs (baked into binary RUNPATH):");
        if (plan.runtimeLibraryDirs.empty()) std::println("  (none)");
        for (auto& d : plan.runtimeLibraryDirs) {
            auto s = d.string();
            std::string note;
            if (s.find("glx_runtime") != std::string::npos)
                note = "   <- host GL/GLX runtime (compat.glx-runtime)";
            else if (s.find("glibc") != std::string::npos) note = "   <- glibc";
            else if (s.find("xim-x-gcc") != std::string::npos
                  || s.find("xim-x-llvm") != std::string::npos) note = "   <- toolchain";
            std::println("  - {}{}", s, note);
        }
        if (!plan.runtimeCapabilities.empty()) {
            std::println("runtime capabilities (provider):");
            for (auto& cap : plan.runtimeCapabilities) {
                std::string prov;
                for (auto& [c, p] : plan.runtimeProviders) if (c == cap) { prov = p; break; }
                std::println("  - {}  -> {}", cap, prov.empty() ? "?" : prov);
            }
        }
    }
    if (all || topic == "deps") {
        std::println("dependencies (mcpp.lock):");
        std::ifstream in(ctx->projectRoot / "mcpp.lock");
        if (!in) {
            std::println("  (no mcpp.lock — run `mcpp build` or `mcpp update`)");
        } else {
            std::string line, cur;
            auto quoted = [](const std::string& l) -> std::string {
                auto a = l.find('"'); if (a == std::string::npos) return {};
                auto b = l.find('"', a + 1); if (b == std::string::npos) return {};
                return l.substr(a + 1, b - a - 1);
            };
            while (std::getline(in, line)) {
                if (line.find("[package.\"") != std::string::npos) cur = quoted(line);
                else if (!cur.empty() && line.find("version") != std::string::npos) {
                    std::println("  - {} {}", cur, quoted(line));
                    cur.clear();
                }
            }
        }
    }
    return 0;
}

// ─── M4 #8.2: mcpp --explain CODE ───────────────────────────────────────
export int explain_code(std::string_view code) {
    struct Entry { std::string_view code, title, body; };
    static constexpr Entry table[] = {
        {"E0001", "dependency name mismatch",
         "The package located at the [dependencies.<key>] path declares a different\n"
         "name in its own [package].name. Either rename the [dependencies.<key>] in\n"
         "the consumer's mcpp.toml to match the producer, or fix the producer's\n"
         "[package].name."},
        {"E0002", "module imported but not provided",
         "A source file does `import X;` but no source file in the build graph\n"
         "exports `X`. Either add a dependency that provides X (mcpp add or\n"
         "[dependencies.X] path = \"...\") or fix the import."},
        {"E0003", "version constraint unsatisfiable",
         "No published version of the package matches the requested constraint.\n"
         "Run `mcpp search <pkg>` to list available versions, then loosen the\n"
         "constraint in mcpp.toml (e.g. ^1.2 instead of =1.2.3)."},
        {"E0004", "toolchain pin mismatch",
         "The [toolchain] pin in mcpp.toml does not match the detected toolchain.\n"
         "Either install the pinned toolchain (xlings install ...) or relax the\n"
         "pin (e.g. \"gcc@>=15\" instead of \"gcc@15.1.0\")."},
        {"E0005", "BMI cache corruption",
         "A cached BMI file referenced by manifest.txt is missing on disk. Run\n"
         "`mcpp cache prune --older-than 0d` to drop stale entries; the next build\n"
         "will repopulate."},
        {"E0006", "index requires a newer mcpp",
         "The package index declares (index.toml [index].min_mcpp) that its\n"
         "descriptors need a newer mcpp than this binary — parsing them would\n"
         "silently misbehave, so resolution stops instead. Upgrade mcpp:\n"
         "  curl -fsSL https://github.com/mcpp-community/mcpp/releases/latest/download/install.sh | bash\n"
         "To bypass for debugging only: MCPP_INDEX_FLOOR=ignore mcpp build"},
    };
    for (auto& e : table) {
        if (e.code == code) {
            std::println("{}: {}", e.code, e.title);
            std::println("");
            std::println("{}", e.body);
            return 0;
        }
    }
    std::println(stderr, "error: unknown error code '{}'", code);
    std::println(stderr, "       known codes: E0001..E0006");
    return 2;
}

// ─── M6.1: `mcpp self ...` — about mcpp itself ──────────────────────────
//
// `self` is declared as a parent subcommand on the top-level App with
// nested `doctor / env / version / explain` subcommands. Each nested
// subcommand has its own action; these helpers wrap the bodies so we
// can share `cmd_doctor` / `cmd_env` between top-level and `mcpp self`.

// `mcpp self init [--force]`.
export int self_init(bool force) {
    if (force) {
        // --force: delete registry (sandbox) + caches and re-bootstrap.
        // Preserves: bin/mcpp (self-contained mode), config.toml, log/.
        mcpp::ui::info("Resetting", "mcpp sandbox (registry, caches)");

        // Resolve MCPP_HOME without running bootstrap (which may fail).
        std::filesystem::path home;
        if (auto* e = std::getenv("MCPP_HOME"); e && *e)
            home = e;
        else {
            const char* userHome = nullptr;
#if defined(_WIN32)
            userHome = std::getenv("USERPROFILE");
#endif
            if (!userHome) userHome = std::getenv("HOME");
            if (userHome) home = std::filesystem::path(userHome) / ".mcpp";
        }
        if (!home.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(home / "registry", ec);
            std::filesystem::remove_all(home / "bmi", ec);
            std::filesystem::remove_all(home / "cache", ec);
        }
    }

    // (Re-)run the full load_or_init, which does bootstrap.
    mcpp::ui::info("Initializing", "mcpp sandbox");
    auto cfg = mcpp::config::load_or_init();
    if (!cfg) {
        mcpp::ui::error(cfg.error().message);
        return 1;
    }

    // Clean any incomplete xpkg installations (interrupted downloads, etc.).
    auto xpkgsBase = cfg->xlingsHome() / "data" / "xpkgs";
    int cleaned = mcpp::fallback::clean_all_incomplete(xpkgsBase);
    if (cleaned > 0) {
        mcpp::ui::info("Cleaned", std::format(
            "{} incomplete installation(s)", cleaned));
    }

    // Verify result.
    auto problem = mcpp::config::check_base_init(*cfg);
    if (!problem.empty()) {
        mcpp::ui::error(std::format("init incomplete: {}", problem));
        return 1;
    }

    mcpp::ui::status("Ready", "sandbox initialized");
    return 0;
}

std::string upper_ascii(std::string s) {
    for (char& ch : s) {
        if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
    }
    return s;
}


// `mcpp self config [--mirror CN|GLOBAL]` (mirror = raw option value).
export int self_config(std::string mirror) {
    if (!mirror.empty()) {
        mirror = upper_ascii(std::move(mirror));
        if (mirror != "CN" && mirror != "GLOBAL") {
            mcpp::ui::error(std::format(
                "invalid mirror '{}'; expected CN or GLOBAL", mirror));
            return 2;
        }
    }

    // When --mirror is given AND this is a fresh MCPP_HOME, seed .xlings.json
    // with the user's choice on the very first write so the immediately-
    // following xlings sandbox bootstrap (patchelf / ninja download) uses
    // their mirror — not the historical CN default that an overseas user
    // is trying to redirect away from. For an already-initialized MCPP_HOME
    // the seed is skipped and config_set_mirror below updates the existing
    // file via xlings.
    //
    // TODO(mirror-default): the default "CN" lives in
    // mcpp::xlings::seed_xlings_json — see the matching note there for the
    // long-term plan (flip default to GLOBAL, or auto-detect on first init).
    auto cfg = mcpp::config::load_or_init(
        /*quiet=*/false, mcpp::fetcher::make_bootstrap_progress_callback(), mirror);
    if (!cfg) {
        mcpp::ui::error(cfg.error().message);
        return 4;
    }

    auto env = mcpp::config::make_xlings_env(*cfg);
    if (mirror.empty()) {
        auto rc = mcpp::xlings::config_show(env);
        return rc == 0 ? 0 : 1;
    }

    auto rc = mcpp::xlings::config_set_mirror(env, mirror, /*quiet=*/true);
    if (rc != 0) {
        mcpp::ui::error(std::format("failed to set xlings mirror to {}", mirror));
        return 1;
    }
    mcpp::ui::status("Configured", std::format("xlings mirror = {}", mirror));
    return 0;
}

} // namespace mcpp::doctor
