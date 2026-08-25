// mcpp.doctor — diagnostics + self-maintenance: environment report,
// health checks, resolution explanation (why), error-code explanations,
// sandbox init/reset, and xlings mirror configuration.
// Bodies moved verbatim from the CLI layer. Zero behavior change.

module;
#include <cstdio>
#include <cstdlib>

export module mcpp.doctor;

import std;
import mcpp.build.program_protocol;
import mcpp.source_kind;
import mcpp.manifest;
import mcpp.bmi_cache.maintenance;
import mcpp.build.prepare;
import mcpp.build.refusal;
import mcpp.targetside;
import mcpp.wire;
import mcpp.build.plan;
import mcpp.build.runtime_validation;
import mcpp.config;
import mcpp.fallback.probe_sysroot;
import mcpp.fallback.xlings_binary;
import mcpp.fallback.install_integrity;
import mcpp.fetcher.progress;
import mcpp.home;
import mcpp.libs.json;
import mcpp.platform;
import mcpp.platform.process;
import mcpp.platform.env;
import mcpp.platform.elf_runtime;
import mcpp.pm.index_refresh;   // staleness_note for `mcpp why deps`
import mcpp.project;
import mcpp.toolchain.detect;
import mcpp.toolchain.msvc;
import mcpp.toolchain.registry;
import mcpp.toolchain.linkmodel;
import mcpp.toolchain.stdmod;
import mcpp.toolchain.abi;
import mcpp.ui;
import mcpp.platform.xlings;

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
        // Windows SDK. REPORTED PER ORIGIN, because it is chosen per origin
        // (design §2.2): `msvc@system` searches the machine, a pinned toolset
        // takes the payload installed with it. One unlabelled line here would
        // be the same "one question, two answerers" shape the SDK axis exists
        // to close — a user reading it would believe it applied to their
        // pinned build, and it does not.
        if (auto sdk = mcpp::toolchain::msvc::find_windows_sdk()) {
            ok(std::format("Windows SDK (msvc@system) {} at {}", sdk->version,
                           sdk->root.string()));
        } else {
            warn("no Windows SDK found for msvc@system — native builds with "
                 "the machine's Visual Studio will fail (install the "
                 "'Windows 11 SDK' VS component)");
        }
        {
            std::error_code sdkEc;
            auto msvcRoot = mcpp::home::root()
                / "registry" / "data" / "xpkgs" / "xim-x-msvc";
            for (auto& v : std::filesystem::directory_iterator(msvcRoot, sdkEc)) {
                if (!v.is_directory(sdkEc)) continue;
                auto ver = v.path().filename().string();
                auto inst = mcpp::toolchain::msvc::installation_at(
                    v.path(), ver, /*identifyVersion=*/false);
                if (!inst) continue;
                // The SAME resolution a build performs, not a second one
                // shaped like it: what doctor prints is what the user will
                // believe, so the two must not be able to disagree.
                auto choice = mcpp::toolchain::msvc::resolve_sdk_for(inst->clPath);
                if (choice.sdk) {
                    ok(std::format("Windows SDK (msvc@{}) {} at {}",
                                   ver, choice.sdk->version,
                                   choice.sdk->root.string()));
                } else {
                    warn(std::format(
                        "msvc@{} has no usable Windows SDK — reinstall it to "
                        "pull its `xim:windows-sdk` dependency", ver));
                }
                if (!choice.note.empty()) warn(choice.note);
            }
        }

        mcpp::ui::status("Checking", "mingw (xim:mingw-gcc)");
        {
            auto pkgs = mcpp::home::root()
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

        // The other direction: a windows-hosted cross toolchain that produces
        // Linux ELF. Same shape as the mingw probe above; the package is named
        // by triple, matching to_xim_package()'s `<triple>-gcc`.
        {
            auto triple = std::string(mcpp::platform::host_arch) + "-linux-musl";
            auto label  = std::format("linux cross (xim:{}-gcc)", triple);
            mcpp::ui::status("Checking", label);
            auto pkgs = mcpp::home::root() / "registry" / "data" / "xpkgs"
                      / std::format("xim-x-{}-gcc", triple);
            std::error_code ec;
            bool any = false;
            if (std::filesystem::exists(pkgs, ec)) {
                for (auto& v : std::filesystem::directory_iterator(pkgs, ec)) {
                    if (!v.is_directory(ec)) continue;
                    ok(std::format("{} {} installed",
                                   triple, v.path().filename().string()));
                    any = true;
                }
            }
            if (!any)
                ok(std::format("{} not installed (optional — "
                               "`mcpp toolchain install gcc 16.1.0 --target {}`)",
                               triple, triple));
        }
    }

    mcpp::ui::status("Checking", "std module");
    if (tc) {
        // Entries live at <cache>/std/<identity>/{gcm,pcm}.cache/std.*; the
        // object sits at the entry root. Look for the object rather than one
        // compiler's BMI extension so this reports on clang and MSVC too.
        auto stdRoot = mcpp::toolchain::default_cache_root() / "std";
        std::error_code ec;
        if (std::filesystem::exists(stdRoot, ec)) {
            bool found = false;
            for (auto& e : std::filesystem::directory_iterator(stdRoot, ec)) {
                for (auto name : {"std.o", "std.obj"}) {
                    auto obj = e.path() / name;
                    if (!std::filesystem::exists(obj, ec)) continue;
                    ok(std::format("{}  (entry {})", e.path().string(),
                                   mcpp::bmi_cache::human_bytes(
                                       mcpp::bmi_cache::dir_size(e.path()))));
                    found = true;
                    break;
                }
                if (found) break;
            }
            if (!found) warn("no std module cached yet (built on first `mcpp build`)");
        } else {
            warn(std::format("no std module cache at '{}' yet "
                             "(built on first `mcpp build`)", stdRoot.string()));
        }
    }

    mcpp::ui::status("Checking", "registry");
    auto cfg = mcpp::config::load_or_init(/*quiet=*/false, mcpp::fetcher::make_bootstrap_progress_callback());

    // Whose sysroot is this? gcc bakes `--sysroot=<...>/subos/default`
    // at build time, and that path is a string, not a reference -- it
    // keeps naming wherever the compiler was built no matter which
    // project it now serves. A developer machine has many directories by
    // that name, so the baked one frequently EXISTS while belonging to an
    // unrelated checkout, and headers then come from a tree this build
    // never declared. Existence is not ownership.
    if (tc && cfg) {
        std::error_code cwdEc;
        auto project = std::filesystem::current_path(cwdEc);
        if (mcpp::fallback::sysroot_is_foreign(
                tc->sysroot, (*cfg).registryDir,
                cwdEc ? std::filesystem::path{} : project))
            warn(std::format(
                "sysroot {} belongs to neither this mcpp home ({}) nor "
                "this project — headers would come from a tree nothing "
                "here declared. mcpp remaps a baked sysroot when it can "
                "find the equivalent under the registry; seeing it here "
                "means it could not",
                tc->sysroot.string(), (*cfg).registryDir.string()));
    }
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
    if (sz > std::uintmax_t(4) * 1024 * 1024 * 1024) {
        warn(std::format("build cache occupies {} "
                         "(`mcpp cache gc --max-size 4GiB` to collect)",
                         mcpp::bmi_cache::human_bytes(sz)));
    } else {
        ok(std::format("build cache size = {}", mcpp::bmi_cache::human_bytes(sz)));
    }

    // Reuse the verdict produced at the link seam.  Doctor deliberately does
    // not parse the artifact or inspect the current host: either would answer
    // a different question after a SubOS/driver update and could contradict
    // the exact RuntimeBinding the build used.
    mcpp::ui::status("Checking", "last runtime closure verdict");
    if (auto projectRoot = mcpp::project::find_manifest_root(
            std::filesystem::current_path())) {
        auto stored = mcpp::build::runtime_validation::latest_stored_verdict(
            *projectRoot / "target");
        if (!stored) {
            ok("no linked ELF verdict stored yet (created on the next Linux link)");
        } else {
            using Status = mcpp::platform::elf::RuntimeVerdict::Status;
            auto detail = stored->verdict.explain();
            auto subject = std::format("{} (RuntimeBinding {})",
                stored->artifact.string(), stored->contractHash);
            if (stored->verdict.status == Status::Pass) {
                ok(std::format("{}: pass", subject));
            } else if (stored->verdict.status == Status::Inconclusive) {
                warn(std::format("{}: inconclusive{}{}", subject,
                    detail.empty() ? "" : "\n", detail));
            } else if (stored->verdict.status == Status::Unresolvable) {
                err(std::format("{}: unresolvable runtime closure{}{}", subject,
                    detail.empty() ? "" : "\n", detail));
            } else {
                err(std::format("{}: proven mismatch{}{}", subject,
                    detail.empty() ? "" : "\n", detail));
            }
        }
    } else {
        ok("not in an mcpp project; no project runtime verdict to report");
    }
    // The pre-v1 cache was keyed by whole-project fingerprint, which folded in
    // the consumer's own name and version — so it accumulated one copy of every
    // dependency per project configuration and never produced a cross-project
    // hit. Nothing reads it now. Report the size, never delete it.
    {
        auto legacyRoot = mcpp::home::legacy_bmi_root();
        std::error_code lec;
        if (std::filesystem::is_directory(legacyRoot, lec)) {
            auto lsz = mcpp::bmi_cache::dir_size(legacyRoot);
            warn(std::format("pre-v1 cache at '{}' occupies {} and is no longer "
                             "used — `mcpp cache clean --legacy` reclaims it",
                             legacyRoot.string(),
                             mcpp::bmi_cache::human_bytes(lsz)));
        }
    }
    // Pre-#311 builds could park the std BMI cache in the current working
    // directory (`.mcpp-bmi/`) whenever neither MCPP_HOME nor HOME resolved —
    // the common case on Windows PowerShell. Point at leftovers; never delete.
    {
        std::error_code lec;
        auto legacy = std::filesystem::current_path(lec) / ".mcpp-bmi";
        if (!lec && std::filesystem::is_directory(legacy, lec)) {
            warn(std::format("legacy BMI cache at '{}' — no longer used, safe to delete",
                             legacy.string()));
        }
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
                    if (c == cap) { provider = p.canonical(); break; }
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
                    // payload_frontend, not toolchain_frontend(root/"bin"):
                    // cl.exe is four levels down at
                    // VC/Tools/MSVC/<ver>/bin/Host<h>/<arch>/, so the `bin/`
                    // lookup finds nothing and `continue` drops every
                    // installed msvc toolset. That is the defect #436 fixed
                    // in `toolchain list`; doctor kept its own copy, so the
                    // two commands disagreed about the same machine.
                    auto bin = mcpp::toolchain::payload_frontend(
                        vEntry.path(), mcpp::toolchain::to_xim_package(s), s.family);
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

        // The vendored xlings, against the version this mcpp expects.
        //
        // Nothing else surfaces this. `mcpp self env` prints both numbers and
        // says nothing about the gap, and a home that acquired its xlings once
        // never revisited it -- so a machine could sit years behind while every
        // command looked healthy. What goes missing is silent by nature:
        // features mcpp reads FROM xlings (the subos_info block, for one)
        // simply never appear, and the code that consumes them degrades
        // quietly because a missing block is also a legitimate state.
        {
            auto have = mcpp::fallback::vendored_xlings_version((*cfg).xlingsBinary);
            const auto want = std::string(mcpp::config::kXlingsPinnedVersion);
            if (have.empty()) {
                warn(std::format("cannot read the vendored xlings version at {}",
                                 (*cfg).xlingsBinary.string()));
            } else if (mcpp::fallback::version_is_older(have, want)) {
                warn(std::format(
                    "vendored xlings is {} but this mcpp expects {} — features "
                    "mcpp reads from xlings may be silently absent (the subos "
                    "self-description arrived in 2026.8.5.1). It is replaced "
                    "automatically on the next `mcpp self init`",
                    have, want));
            } else {
                ok(std::format("vendored xlings {} (pinned {})", have, want));
            }
        }

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

    // ── Build-policy knobs that are otherwise invisible ────────────────────
    //
    // Both of these change behaviour without changing anything a user can see
    // in the output of a successful build, which is how "I set the key and
    // nothing happened" becomes unanswerable. Report the EFFECTIVE value and,
    // for the ones that have one, where it came from.
    {
        mcpp::ui::status("Checking", "build policy");

        std::error_code pec;
        auto manifestPath = std::filesystem::current_path(pec) / "mcpp.toml";
        std::optional<mcpp::manifest::Manifest> m;
        if (!pec && std::filesystem::exists(manifestPath, pec))
            if (auto loaded = mcpp::manifest::load(manifestPath)) m = std::move(*loaded);

        // Module-interface extensions: built-ins plus this project's additions.
        {
            auto table = mcpp::extension_table_for(
                m ? m->buildConfig.moduleExtensions : std::vector<std::string>{});
            std::string list;
            for (auto const& e : table.moduleInterface) {
                if (!list.empty()) list += ' ';
                list += e;
            }
            const auto extra = table.moduleInterface.size() - 1;   // built-in is .cppm
            ok(std::format("module interfaces: {}{}", list,
                           extra ? std::format("  ({} from [build] module_extensions)", extra)
                                 : "  (built-in only)"));
        }

        // Run bound for build.mcpp, with its source named.
        {
            namespace pp = mcpp::build::program_protocol;
            auto envSecs = pp::env_timeout_override();
            auto manSecs = m ? m->buildConfig.buildProgramTimeoutSecs
                             : std::optional<int>{};
            auto effective = pp::run_timeout(envSecs, manSecs).count() / 1000;
            std::string_view from = envSecs ? "MCPP_BUILD_PROGRAM_TIMEOUT"
                                  : manSecs ? "[build] build_program_timeout"
                                            : "built-in default";
            ok(std::format("build.mcpp run bound: {}  (from {})",
                           effective ? std::format("{}s", effective)
                                     : std::string("none — 0 disables it"),
                           from));
        }

        // Whether a deadline is actually enforced here. This used to be "no"
        // on Windows while every knob claimed otherwise.
        ok("process deadlines: enforced (POSIX SIGKILL / Windows job object)");
    }

    std::println("");
    if (errors)        std::println("Doctor result: {} errors, {} warnings", errors, warns);
    else if (warns)    std::println("Doctor result: {} warnings", warns);
    else               std::println("Doctor result: all checks passed");
    return errors ? 2 : (warns ? 1 : 0);
}

std::optional<std::pair<std::filesystem::path, nlohmann::json>>
latest_runtime_resolution(const std::filesystem::path& projectRoot) {
    const auto target = projectRoot / "target";
    std::error_code ec;
    if (!std::filesystem::is_directory(target, ec)) return std::nullopt;
    std::filesystem::path newest;
    std::filesystem::file_time_type newestTime{};
    bool found = false;
    for (auto it = std::filesystem::recursive_directory_iterator(
             target, std::filesystem::directory_options::skip_permission_denied, ec);
         !ec && it != std::filesystem::recursive_directory_iterator{};
         it.increment(ec)) {
        if (!it->is_regular_file(ec)
            || it->path().filename() != "resolution.json") continue;
        auto time = it->last_write_time(ec);
        if (ec) { ec.clear(); continue; }
        if (!found || time > newestTime
            || (time == newestTime && it->path() < newest)) {
            found = true;
            newest = it->path();
            newestTime = time;
        }
    }
    if (!found) return std::nullopt;
    std::ifstream input(newest);
    auto doc = nlohmann::json::parse(input, nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) return std::nullopt;
    return std::pair{std::move(newest), std::move(doc)};
}

int print_stored_runtime_resolution() {
    auto root = mcpp::project::find_manifest_root(std::filesystem::current_path());
    if (!root) {
        std::println(stderr,
            "error: no mcpp.toml found; `mcpp why runtime` only reads a project's stored resolution");
        return 2;
    }
    auto stored = latest_runtime_resolution(*root);
    if (!stored) {
        std::println(stderr,
            "error: no stored runtime resolution; run `mcpp build` once");
        return 2;
    }
    auto const& [path, doc] = *stored;
    auto runtime = doc.find("runtime");
    if (runtime == doc.end() || !runtime->is_object()) {
        std::println(stderr, "error: {} has no runtime object", path.string());
        return 2;
    }
    std::println("runtime resolution: {}", path.string());
    if (auto binding = runtime->find("binding");
        binding != runtime->end() && binding->is_object()) {
        const bool declared = binding->value("declared", true);
        std::println("binding: {} via {} (contract {}){}",
            declared ? binding->value("runtime_id", "?") : "(undeclared)",
            binding->value("provider_id", "?"),
            binding->value("contract_hash", "?"),
            declared ? "" : "  — this SubOS did not describe itself");
        // The note, when there is one. A degradation that only ever appeared
        // once during the build it happened in is a degradation nobody can look
        // up afterwards, and "why is my verdict inconclusive" is exactly the
        // question this command exists to answer.
        if (auto note = binding->value("note", std::string{}); !note.empty())
            for (auto line : std::views::split(note, '\n'))
                std::println("  {}", std::string_view(line));
    }

    std::println("requirements:");
    auto requirements = runtime->find("requirements");
    if (requirements == runtime->end() || !requirements->is_array()
        || requirements->empty()) {
        std::println("  (none)");
    } else {
        for (auto const& requirement : *requirements) {
            if (!requirement.is_object()) continue;
            std::string requester = "?";
            if (auto id = requirement.find("requester");
                id != requirement.end() && id->is_object())
                requester = id->value("canonical", "?");
            std::println("  - {}:{} [{}] <- {} ({})",
                requirement.value("kind", "?"),
                requirement.value("value", "?"),
                requirement.value("phase", "?"), requester,
                requirement.value("required", true) ? "required" : "optional");
        }
    }

    std::println("providers:");
    auto providers = runtime->find("providers");
    if (providers == runtime->end() || !providers->is_array()
        || providers->empty()) {
        std::println("  (none resolved)");
    } else {
        for (auto const& entry : *providers) {
            if (!entry.is_object()) continue;
            std::string provider = "?";
            std::string source;
            if (auto id = entry.find("provider");
                id != entry.end() && id->is_object()) {
                provider = id->value("canonical", "?");
                source = id->value("source", "");
            }
            std::println("  - {} -> {}{}{}{}", entry.value("capability", "?"),
                provider, source.empty() ? "" : " [", source,
                source.empty() ? "" : "]");
        }
    }

    std::println("artifacts:");
    auto artifacts = runtime->find("artifacts");
    if (artifacts == runtime->end() || !artifacts->is_array()
        || artifacts->empty()) {
        // NOT "none". A provider can be resolved by name and still have no
        // artifact behind it, and `(none declared)` read as a clean bill of
        // health for exactly that state — the graphics stack sat there for
        // weeks with `providers:` populated and nothing to check.
        std::println("  (not declared by the environment — nothing to verify)");
        std::println("  note: a resolved provider with no artifact is UNVERIFIED,");
        std::println("        not verified-good");
    } else {
        for (auto const& artifact : *artifacts) {
            if (!artifact.is_object()) continue;
            std::string provider = "?";
            if (auto id = artifact.find("provider");
                id != artifact.end() && id->is_object())
                provider = id->value("canonical", "?");
            auto identity = artifact.value("identity", "unverified");
            std::println("  - {} {} <- {} [{}; abi={}; identity={}]",
                artifact.value("role", "?"), artifact.value("path", "?"),
                provider, artifact.value("provenance", "?"),
                artifact.value("abi", "?"), identity);
            if (identity == "mismatch") {
                std::println("      ^ STALE BINDING: this resolves into a "
                             "different version than the one declared.");
                std::println("        The declaration is a promise about which "
                             "payload the loader reaches;");
                std::println("        a later install repointed it.");
            } else if (identity == "missing") {
                std::println("      ^ declared, but nothing is at that path");
            }
        }
    }

    if (auto search = runtime->find("search");
        search != runtime->end() && search->is_object()) {
        std::println("search: format={} link={} transitive={} runtime={}",
            search->value("format", "?"), search->value("link_library", "?"),
            search->value("transitive_needed", "?"),
            search->value("runtime", "?"));
        // The closure IN ORDER, with where each directory came from. Order is
        // the answer to "why does my GL program find its driver" and to "why
        // is my libc still the pinned one" — both invisible when the report
        // says only which mechanism is used.
        if (auto closure = search->find("closure");
            closure != search->end() && closure->is_array() && !closure->empty()) {
            std::println("  runtime search closure (loader order):");
            for (auto const& dir : *closure) {
                if (!dir.is_object()) continue;
                std::println("    {:<12} {}{}",
                    dir.value("origin", "?"), dir.value("path", "?"),
                    dir.value("machine_local", false) ? "  [machine-local]" : "");
            }
            std::println("    note: the mutable SubOS farm is LAST on purpose — "
                         "libc stays pinned to its payload");
        }
    }
    if (auto validation = runtime->find("validation");
        validation != runtime->end() && validation->is_object()) {
        std::println("validation: {} (source {})",
            validation->value("status", "?"),
            validation->value("source", "?"));
        if (auto checked = validation->find("artifacts");
            checked != validation->end() && checked->is_array()) {
            for (auto const& artifact : *checked) {
                if (!artifact.is_object()) continue;
                std::println("  - {}: {}", artifact.value("path", "?"),
                             artifact.value("status", "?"));
                if (auto diagnostics = artifact.find("diagnostics");
                    diagnostics != artifact.end() && diagnostics->is_array())
                    for (auto const& diagnostic : *diagnostics)
                        if (diagnostic.is_string())
                            std::println("      {}", diagnostic.get<std::string>());
            }
        }
    }
    std::println("provider and host-service re-diagnostics are owned by xlings; run `xlings doctor`");
    return 0;
}

// `mcpp why [topic]` / `mcpp resolve --explain`.

// ⭐⭐ THE SAME RESOLUTION `why toolchain` PRINTS, AS DATA.
//
// A build for one (target, toolchain) pair resolves five layers, a driver, a
// triple and a sysroot, and then either proceeds or refuses. Every one of those
// is a finite value, and until this function the only way to read them from
// outside was to match the sentences mcpp prints for a person. The cost of that
// was measured on 2026-08-26: rewording one refusal turned an e2e assertion
// into a no-op, silently, in the same session that wrote it.
//
// ⚠️ EXIT 0 WHENEVER THE QUESTION WAS ANSWERED, INCLUDING WHEN THE ANSWER IS
// "REFUSED". This is a query: "would this build, and if not why" is answered
// successfully by "no, because the row's pin is a capability". Overloading the
// exit code would give a client exactly the ambiguity the envelope exists to
// remove — and `data.status` is unambiguous. A non-zero exit here means the
// query itself could not run.
//
// It builds nothing. `prepare_build` stops before any compile, which is also
// why the target matrix can afford to ask it for every cell.
export int why_toolchain_json(std::string_view target, std::string_view tcSpec) {
    mcpp::build::BuildOverrides ov;
    ov.target_triple = std::string(target);
    // ⚠️ `MCPP_TOOLCHAIN`, not a field on the overrides — that is the channel
    // `--toolchain` already uses, and `prepare_build` reads it as a
    // user-explicit declaration. Adding a second way in would give the two
    // spellings different provenance, and provenance is exactly what the
    // convention/capability distinction turns on.
    //
    // ⚠️ RESTORED AFTERWARDS. This is a library function; leaving a declared
    // toolchain in the environment would make the NEXT thing this process does
    // inherit a compiler nobody asked it for.
    // ⭐ `ScopedEnv` already exists for exactly this and restores the prior
    // value, including "there was none".
    std::optional<mcpp::platform::env::ScopedEnv> tcGuard;
    if (!tcSpec.empty())
        tcGuard.emplace("MCPP_TOOLCHAIN", std::string(tcSpec));

    // ⚠️⚠️ CLEARED BEFORE THE CALL, NOT ONLY READ AFTER IT.
    //
    // The sink is per-thread and `prepare_build` recurses for tool
    // provisioning. Reading it afterwards without clearing first means a code
    // recorded by an EARLIER query — or by a nested sub-build of a previous
    // one — can be reported as this query's reason. A stale reason is worse
    // than none: it is a specific, plausible, wrong answer.
    (void)mcpp::build::refusal::take();

    // ⚠️⚠️ STDOUT BELONGS TO THE ENVELOPE, AND `prepare_build` NARRATES.
    //
    // `Resolving toolchain` / `Resolved …` / `Target … → …` are status lines
    // for a person, and they go to stdout. A client is told to detect the
    // protocol BY PARSING stdout — see the module comment in mcpp.wire — so
    // three lines of prose ahead of the JSON is not a cosmetic problem, it is
    // the envelope failing to arrive. Measured on the first run of this
    // function: `json.tool` refused at `line 1 column 4`.
    //
    // Restored afterwards rather than left set: this is a library function and
    // the process may go on to do something that should narrate.
    const bool wasQuiet = mcpp::ui::is_quiet();
    mcpp::ui::set_quiet(true);
    struct QuietGuard {
        bool prev;
        ~QuietGuard() { mcpp::ui::set_quiet(prev); }
    } quietGuard{wasQuiet};

    nlohmann::json data;
    data["requested"] = {
        {"target",    std::string(target)},
        {"toolchain", std::string(tcSpec)},
    };

    auto ctx = mcpp::build::prepare_build(/*print_fingerprint=*/false,
                                          /*includeDevDeps=*/false, {}, ov);
    std::vector<mcpp::wire::Diagnostic> diags;
    if (!ctx) {
        // ⚠️ `take()` AFTER the call and only here: a site that recorded a code
        // and then did not refuse would otherwise leak it into the next query.
        // ⚠️⚠️ `None` MEANS "NOTHING RECORDED", AND HERE THAT IS NOT "no reason"
        // — the build demonstrably refused. Reporting `none` beside
        // `refused` gives one word two meanings, which is the defect this
        // whole release is about, reintroduced in the machinery built to
        // detect it. Measured: `gcc` + `openkal-llvm-runtime` refused through
        // a site that had no code, and the matrix recorded
        // `unsupported / none`.
        //
        // ⭐ `other` is a visible admission: a refusal exists and its branch
        // has not been named yet.
        auto code = mcpp::build::refusal::take();
        if (code == mcpp::build::refusal::Code::None)
            code = mcpp::build::refusal::Code::Other;
        data["status"] = "refused";
        data["reason"] = std::string(mcpp::build::refusal::name(code));
        diags.push_back({
            .code     = std::format("target.{}",
                                    mcpp::build::refusal::name(code)),
            .severity = mcpp::wire::Severity::Error,
            .message  = ctx.error(),
        });
        mcpp::wire::emit({
            .kind        = "mcpp.why.toolchain",
            .effects     = { mcpp::wire::Effect::ReadProject },
            .data        = data,
            .diagnostics = diags,
        });
        return 0;
    }
    (void)mcpp::build::refusal::take();

    const auto& tc = ctx->tc;
    const auto& ts = ctx->plan.targetSide;

    data["status"] = "ok";
    data["reason"] = "none";
    data["compiler"] = {
        {"family",  std::string(tc.compiler_name())},
        {"version", tc.version},
        {"driver",  tc.binaryPath.string()},
    };
    data["triple"] = {
        {"requested", std::string(target)},
        {"toolchain", tc.targetTriple},
        {"llvm",      ts.llvmTriple},
    };

    // ⭐⭐ THE C LIBRARY MODEL, NOT `tc.sysroot`.
    //
    // `tc.sysroot` is what the driver reports for `-print-sysroot`, and it is
    // frequently empty for a toolchain that nonetheless receives an explicit
    // `--sysroot` on every command line. The first version of this reported it
    // and disagreed with the build: the query said `none` while build.ninja
    // carried `--sysroot=…/registry/subos/default`.
    //
    // `resolve_link_model` is the function the flag emitter itself calls, and
    // it is a pure function of the toolchain — so asking it here is asking the
    // same question of the same authority rather than re-deriving an answer
    // that can drift from the one the build uses.
    const auto lm = mcpp::toolchain::resolve_link_model(tc);
    auto path_origin = [](const std::filesystem::path& p) -> std::string_view {
        if (p.empty()) return "none";
        const auto sp = p.generic_string();
        if (sp.find("registry/data/xpkgs") != std::string::npos) return "payload";
        if (sp.find("registry/subos")      != std::string::npos) return "subos";
        return "host";
    };
    const auto& srPath =
        lm.mode == mcpp::toolchain::CLibMode::Sysroot ? lm.sysroot
                                                      : lm.crtDir;
    data["cLibrary"] = {
        {"mode",   lm.mode == mcpp::toolchain::CLibMode::Sysroot      ? "sysroot"
                 : lm.mode == mcpp::toolchain::CLibMode::PayloadFirst ? "payload-first"
                                                                      : "none"},
        {"path",   srPath.string()},
        {"origin", std::string(path_origin(srPath))},
    };

    auto layer = [](std::string_view label,
                    const mcpp::targetside::Layer& l) {
        return nlohmann::json{
            {"layer",     std::string(label)},
            {"interface", l.interfaceName},
            {"impl",      l.impl},
            {"origin",    l.absent()
                            ? std::string("none")
                            : std::string(mcpp::targetside::origin_name(l.origin))},
            {"subset",    l.subset},
        };
    };
    data["layers"] = nlohmann::json::array({
        layer("compiler",         ts.compiler),
        layer("compiler-runtime", ts.compilerRuntime),
        layer("kernel-abi",       ts.kernelAbi),
        layer("c-abi",            ts.cAbi),
        layer("c++-abi",          ts.cxx),
    });

    mcpp::wire::emit({
        .kind        = "mcpp.why.toolchain",
        .effects     = { mcpp::wire::Effect::ReadProject },
        .data        = data,
        .diagnostics = diags,
    });
    return 0;
}

export int why_report(const std::string& topic) {
    const bool all = topic.empty() || topic == "all";

    // The dedicated runtime view is a pure interpreter of the build's stored
    // facts: no dependency resolution, xlings invocation, hardware query, or
    // artifact re-parse is allowed on this path.
    if (topic == "runtime") return print_stored_runtime_resolution();

    auto ctx = mcpp::build::prepare_build(/*print_fingerprint=*/false);
    if (!ctx) { std::println(stderr, "error: {}", ctx.error()); return 2; }
    auto& tc   = ctx->tc;

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
    if (all) (void)print_stored_runtime_resolution();
    if (all || topic == "deps") {
        // Which index answered, and how stale it is. Since #315 a build only
        // refreshes on a resolution miss, so "why did I get this version"
        // frequently has "because that is the newest one your local index
        // knows" as its answer — which is unguessable without this line.
        if (auto cfgW = mcpp::config::load_or_init(/*quiet=*/true)) {
            std::println("package index: {}",
                mcpp::pm::staleness_note(mcpp::config::make_xlings_env(*cfgW)));
        }
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
        {"E0005", "build cache corruption",
         "A file listed in a cache entry's entry.json is missing on disk. Such an\n"
         "entry is treated as a miss and rebuilt, so this is never wrong output —\n"
         "only wasted space. `mcpp cache verify` lists every affected entry and\n"
         "`mcpp cache gc --older-than 0s` reclaims them."},
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

        // Resolve MCPP_HOME without running bootstrap (which may fail). The
        // shared resolver also covers self-contained installs — the local copy
        // this replaced would have wiped ~/.mcpp for a `<root>/bin/mcpp` tree.
        std::filesystem::path home = mcpp::home::root();
        if (!home.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(home / "registry", ec);
            std::filesystem::remove_all(home / "cache", ec);        // index metadata
            std::filesystem::remove_all(home / "build-cache", ec);  // compiled artifacts
            std::filesystem::remove_all(home / "bmi", ec);          // pre-v1 build cache
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
