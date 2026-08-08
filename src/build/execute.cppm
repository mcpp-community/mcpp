// mcpp.build.execute — drives a prepared BuildContext: ninja execution,
// build cache + fast-path rebuilds, and the run/test/clean pipelines.
// Bodies moved verbatim from the CLI layer. Zero behavior change.

module;
#include <cstdio>
#include <cstdlib>

export module mcpp.build.execute;

import std;
import mcpp.build.build_program;   // #359 glob inputs the mtime sweep cannot see
import mcpp.build.prepare;
import mcpp.diag;
import mcpp.build.plan;
import mcpp.build.backend;
import mcpp.build.ninja;
import mcpp.bmi_cache;
import mcpp.manifest;
import mcpp.modgraph.scanner;
import mcpp.toolchain.post_install;
import mcpp.toolchain.stdmod;
import mcpp.xlings;
import mcpp.xlings.subos_info;
import mcpp.log;
import mcpp.platform;
import mcpp.fetcher.progress;
import mcpp.project;
import mcpp.ui;

namespace mcpp::build {

// ─── P0: build cache for fast-path rebuilds ─────────────────────────

constexpr std::string_view kBuildCacheFile = "target/.build_cache";
// P3: LRU capacity. Entries are keyed by (target triple, profile), so the
// working set is now targets × profiles rather than targets alone — 4 was
// enough for one profile, not for a dev/release/dist rotation across a host
// and a cross target.
constexpr int kBuildCacheMaxEntries = 8;

// P3: one entry per (target, fingerprint) pair.
struct BuildCacheEntry {
    std::string targetTriple;    // "" for default target
    std::string outputDir;
    std::string ninjaProgram;
    std::string fingerprint;     // outputDir basename
    std::string runtimeEnvKey;   // "-" means intentionally empty; "" means old cache
    std::string runtimeEnvValue;
    // mcpp#225 (E2): resolved binary run-targets, cached alongside the
    // fingerprint so `mcpp run` can skip prepare_build (toolchain
    // resolution + modgraph scan) on a cache hit — see build_run_target's
    // fast path. name -> exe path relative to outputDir. Caches written
    // before this field existed leave it empty, which the run fast-path
    // treats as a miss (falls back to prepare_build once, never crashes).
    std::vector<std::pair<std::string, std::string>> runTargets;
    // The process environment needed to exec those targets (e.g.
    // LD_LIBRARY_PATH for dep .so's not covered by the exe's own RUNPATH),
    // cached the same way as runtimeEnvKey/Value above but for RUNNING the
    // binary rather than invoking the toolchain. "" (default-constructed)
    // means old cache / not yet resolved — the run fast-path exec's with no
    // extra env in that case, matching prepare_build's behavior when
    // plan.runtimeLibraryDirs is empty.
    std::string runEnvKey;
    std::string runEnvValue;
    // The subos this build's toolchain belongs to (mcpp#352). The DIRECTORY,
    // never the resolved variables: the environment is the subos's property
    // and must be re-read on every run, while WHICH subos is the build's
    // property and would otherwise be unknowable on the fast path -- which
    // has no toolchain to derive it from.
    std::string subosDir;
    // Was the line present at all? An EMPTY subosDir is a legitimate answer
    // (a system toolchain outside the xpkgs store has no subos), so it cannot
    // stand in for "this cache predates the field" -- and those two need
    // opposite treatment: the first runs, the second must rebuild once.
    bool        subosRecorded = false;
    // The resolved profile this entry was built for. Entries used to be keyed
    // by target triple alone, and the fast paths only refuse to run when an
    // EXPLICIT --profile/--dev/--release is passed — so a bare `mcpp build`
    // after `mcpp build --release` took the fast path against the release
    // build.ninja and reported success without ever rebuilding at -O0 -g.
    // Empty means "cache predates this field" and is treated as a miss (a
    // bare rebuild once, never a wrong artifact).
    std::string profile;
    // The global-cache mode this build.ninja was generated under. A graph built
    // under `global` contains stage_file edges reading the cache; replaying it
    // for a request that asked for `local` would use the cache the manifest just
    // said not to use — and ruling the cache out is `local`'s entire purpose.
    // Same back-compat contract as `profile`: empty ⇒ miss.
    std::string cacheMode;
};

std::vector<BuildCacheEntry> read_build_cache(const std::filesystem::path& projectRoot) {
    auto path = projectRoot / kBuildCacheFile;
    std::ifstream f(path);
    if (!f) return {};

    std::string firstLine;
    if (!std::getline(f, firstLine) || firstLine.empty()) return {};

    // Detect legacy format (first line is an absolute path, not "[target=...]").
    if (firstLine[0] != '[') {
        // Legacy 4-line format: outputDir, ninjaProgram, target, fingerprint.
        BuildCacheEntry e;
        e.outputDir = firstLine;
        std::getline(f, e.ninjaProgram);
        std::getline(f, e.targetTriple);
        std::getline(f, e.fingerprint);
        if (e.outputDir.empty() || e.ninjaProgram.empty()) return {};
        return {e};
    }

    // P3 multi-entry format: sections of [target=<triple>] + 3 mandatory
    // lines, plus optional runtime-env lines added after toolenv moved out of
    // build.ninja. Old cache entries omit them and are treated as stale.
    std::vector<BuildCacheEntry> entries;
    std::string line = firstLine;
    while (true) {
        // Parse [target=<triple>]
        if (line.size() < 9 || !line.starts_with("[target=") || line.back() != ']')
            break;
        BuildCacheEntry e;
        e.targetTriple = line.substr(8, line.size() - 9);
        if (!std::getline(f, e.outputDir) || e.outputDir.empty()) break;
        if (!std::getline(f, e.ninjaProgram) || e.ninjaProgram.empty()) break;
        std::getline(f, e.fingerprint);
        bool haveNextLine = static_cast<bool>(std::getline(f, line));
        if (haveNextLine && !line.starts_with("[target=")
                         && !line.starts_with("runTargets=")) {
            e.runtimeEnvKey = line;
            std::getline(f, e.runtimeEnvValue);
            haveNextLine = static_cast<bool>(std::getline(f, line));
        }
        // mcpp#225 (E2): optional runTargets block. Absent on caches written
        // before this field existed (or truncated/corrupt mid-block) — in
        // either case e.runTargets stays empty, which the run fast-path
        // treats as a miss, never a crash.
        if (haveNextLine && line.starts_with("runTargets=")) {
            std::size_t n = 0;
            try { n = std::stoul(line.substr(11)); } catch (...) { n = 0; }
            for (std::size_t i = 0; i < n && std::getline(f, line); ++i) {
                auto tab = line.find('\t');
                if (tab == std::string::npos) continue;
                e.runTargets.emplace_back(line.substr(0, tab), line.substr(tab + 1));
            }
            haveNextLine = static_cast<bool>(std::getline(f, line));
        }
        // mcpp#225 (E2): optional run-env block (the process env needed to
        // exec a cached run-target, e.g. LD_LIBRARY_PATH). Same back-compat
        // contract as runTargets above.
        if (haveNextLine && line.starts_with("runEnv=")) {
            e.runEnvKey = line.substr(7);
            std::getline(f, e.runEnvValue);
            haveNextLine = static_cast<bool>(std::getline(f, line));
        }
        // Optional subos line. Same back-compat contract: absent ⇒ empty ⇒
        // the run fast path treats the entry as a miss, exactly as it already
        // does for a cache written before runtimeEnvKey existed. Running with
        // a DIFFERENT environment than the full path would be worse than not
        // using the cache at all -- the program would work once and then
        // silently stop finding its runtime data.
        if (haveNextLine && line.starts_with("subos=")) {
            e.subosDir      = line.substr(6);
            e.subosRecorded = true;
            haveNextLine = static_cast<bool>(std::getline(f, line));
        }
        // Optional profile line. Same back-compat contract as the two blocks
        // above: absent ⇒ e.profile stays empty ⇒ every fast path treats the
        // entry as a miss and falls through to prepare_build.
        if (haveNextLine && line.starts_with("profile=")) {
            e.profile = line.substr(8);
            haveNextLine = static_cast<bool>(std::getline(f, line));
        }
        if (haveNextLine && line.starts_with("cacheMode=")) {
            e.cacheMode = line.substr(10);
            haveNextLine = static_cast<bool>(std::getline(f, line));
        }
        entries.push_back(std::move(e));
        if (!haveNextLine || line.empty()) break;
    }
    return entries;
}

void write_build_cache(const std::filesystem::path& projectRoot,
                       const std::filesystem::path& outputDir,
                       const std::string& ninjaProgram,
                       const std::string& targetTriple,
                       const std::string& fingerprintHex = "",
                       const std::string& runtimeEnvKey = "-",
                       const std::string& runtimeEnvValue = "",
                       std::vector<std::pair<std::string, std::string>> runTargets = {},
                       const std::string& runEnvKey = "",
                       const std::string& runEnvValue = "",
                       const std::string& profile = "",
                       const std::string& cacheMode = "",
                       const std::string& subosDir = "") {
    auto path = projectRoot / kBuildCacheFile;
    auto entries = read_build_cache(projectRoot);

    // Remove the existing entry for this (target, profile) pair. Keying on the
    // triple alone made a release build evict the dev entry and vice versa, so
    // switching profiles back and forth could never be incremental AND the
    // surviving entry pointed at the other profile's build dir.
    std::erase_if(entries, [&](const BuildCacheEntry& e) {
        return e.targetTriple == targetTriple && e.profile == profile;
    });

    // Insert at front (MRU).
    BuildCacheEntry newEntry{targetTriple, outputDir.string(), ninjaProgram, fingerprintHex,
                             runtimeEnvKey, runtimeEnvValue, std::move(runTargets),
                             runEnvKey, runEnvValue, subosDir, /*subosRecorded=*/true,
                             profile, cacheMode};
    entries.insert(entries.begin(), std::move(newEntry));

    // Trim to LRU capacity.
    if ((int)entries.size() > kBuildCacheMaxEntries)
        entries.resize(kBuildCacheMaxEntries);

    // Write P3 format.
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream f(path, std::ios::trunc);
    if (!f) return;
    for (auto& e : entries) {
        f << "[target=" << e.targetTriple << "]\n";
        f << e.outputDir << '\n';
        f << e.ninjaProgram << '\n';
        f << e.fingerprint << '\n';
        f << (e.runtimeEnvKey.empty() ? "-" : e.runtimeEnvKey) << '\n';
        f << e.runtimeEnvValue << '\n';
        // mcpp#225 (E2): run-targets + their exec env, always written (even
        // when empty) so a reader never has to guess whether a missing
        // block means "no targets" vs "cache predates this field" — the
        // count-prefixed block is unambiguous either way, and back-compat
        // for OLD caches (no such block at all) is handled on the read side.
        f << "runTargets=" << e.runTargets.size() << '\n';
        for (auto& [name, exe] : e.runTargets) f << name << '\t' << exe << '\n';
        f << "runEnv=" << e.runEnvKey << '\n';
        f << e.runEnvValue << '\n';
        f << "subos=" << e.subosDir << '\n';
        f << "profile=" << e.profile << '\n';
        f << "cacheMode=" << e.cacheMode << '\n';
    }
}

std::vector<std::string> read_ninja_command_prefixes(const std::filesystem::path& ninjaPath) {
    std::ifstream f(ninjaPath);
    if (!f) return {};

    std::vector<std::string> prefixes;
    std::string line;
    while (std::getline(f, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        auto key = line.substr(0, eq);
        while (!key.empty() && std::isspace(static_cast<unsigned char>(key.back())))
            key.pop_back();
        // `mcpp` drives the dyndep + stage_file rules; treating it as a command
        // prefix filters the echoed command line while keeping the diagnostic
        // mcpp itself printed (#311).
        if (key != "cxx" && key != "cc" && key != "ar" && key != "scan_deps"
            && key != "mcpp")
            continue;

        std::string value = line.substr(eq + 1);
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
            value.erase(value.begin());
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
            value.pop_back();
        if (!value.empty())
            prefixes.push_back(std::move(value));
    }
    return prefixes;
}

bool is_stale_ninja_failure(std::string_view output) {
    return output.find("loading 'build.ninja'") != std::string_view::npos
        || output.find("loading build.ninja") != std::string_view::npos
        || output.find("unknown target") != std::string_view::npos
        || output.find("manifest 'build.ninja' still dirty") != std::string_view::npos
        // A cached build.ninja can reference an input (e.g. a dependency
        // source under the registry) that moved or was reinstalled since the
        // graph was generated — the build fingerprint does not yet cover
        // registry dep state, so the stale graph is reused. Ninja then aborts
        // with this signature. Treat it as stale → drop to a full regen
        // instead of hard-failing and forcing the user to `mcpp clean`.
        || output.find("missing and no known rule to make") != std::string_view::npos;
}

// mcpp#225 (E2): the (name, exe-path-relative-to-outputDir) pairs for every
// binary link unit in a resolved plan, cached alongside the build
// fingerprint so `mcpp run` can locate an executable without re-running
// prepare_build (see BuildCacheEntry::runTargets / try_fast_run below).
// TestBinary/library link units never run via `mcpp run`, so only Binary
// link units are collected.
std::vector<std::pair<std::string, std::string>>
compute_run_targets(const mcpp::build::BuildPlan& plan) {
    std::vector<std::pair<std::string, std::string>> out;
    for (auto& lu : plan.linkUnits) {
        if (lu.kind != mcpp::build::LinkUnit::Binary) continue;
        out.emplace_back(lu.targetName, lu.output.generic_string());
    }
    return out;
}

// mcpp#225 (E2): the process env needed to exec a run-target (e.g.
// LD_LIBRARY_PATH for dep .so's not covered by the exe's own RUNPATH).
// Shared between build_run_target's normal (prepare_build) path and its
// cached fast path so both derive the same env from the same source.
std::pair<std::string, std::string>
compute_run_env(const mcpp::build::BuildPlan& plan) {
    auto key = mcpp::platform::env::runtime_library_path_key();
    auto value = mcpp::platform::env::prepend_path_list(key, plan.runtimeLibraryDirs);
    if (key.empty() || value.empty()) return {"", ""};
    return {key, value};
}

// The environment the active subos declares for the programs it hosts
// (mcpp#352).
//
// A GL application needs three things and mcpp only ever supplied two: the
// binary links (bootstrap), it finds its libraries (RPATH), and then it has to
// be told which driver module to load and which GL vendors exist. That third
// one is a set of environment variables, xlings's graphics packages declare
// them into the subos, and until now nothing carried them to a program mcpp
// launched — `xlings subos use` applied them, `mcpp run` did not. Hence a
// binary that links fine and exits 255 with no output.
//
// Resolved at RUN time, deliberately not cached with the build: these values
// belong to the subos, not to the build, and a user who switches subos between
// `mcpp build` and `mcpp run` must get the new one. It is a file read.
//
// mcpp does not know what any of these variables MEAN, and that is the design:
// when the ecosystem gains a Vulkan loader or a new driver bridge, the
// declaration changes and this code does not.
// The subos a RUN should use: an explicit override if the caller set one,
// otherwise the subos this build belongs to.
//
// The override lives HERE and not in the derivation, because the derivation's
// answer is cached and this one must not be: MCPP_SUBOS_DIR says "for this
// invocation". It exists so tests can exercise this path without touching a
// developer's real subos — an earlier e2e wrote through a symlink and
// permanently broke a real toolchain — and so a user can point one run at
// another subos without switching the active one.
std::filesystem::path subos_dir_for_run(const std::filesystem::path& buildSubos) {
    if (const char* e = std::getenv("MCPP_SUBOS_DIR"); e && *e)
        return std::filesystem::path(e);
    return buildSubos;
}

std::vector<std::pair<std::string, std::string>>
compute_subos_env(const mcpp::build::BuildPlan& plan) {
    auto built = mcpp::xlings::paths::subos_dir_of(plan.toolchain.binaryPath);
    auto dir = subos_dir_for_run(built ? *built : std::filesystem::path{});
    if (dir.empty()) return {};
    auto info = mcpp::xlings::subos::read(dir);
    // The note is a `verbose` line rather than a warning: a subos with no
    // self-description is the normal state of every machine whose subos
    // predates the block, and a warning on every run would train people to
    // ignore it. It becomes loud only where it explains a failure — the GL
    // diagnostic path in doctor.
    if (!info.note.empty())
        mcpp::log::verbose("subos", info.note);
    // Resolved AGAINST the caller's environment, not in a vacuum: these
    // entries replace the variable in the child, so a `set` that ignores an
    // exported value overwrites it and a `prepend` that ignores it drops it.
    return mcpp::xlings::subos::resolve_env(
        info, dir, [](std::string_view v) -> std::optional<std::string> {
            if (const char* e = std::getenv(std::string(v).c_str()))
                return std::string(e);
            return std::nullopt;
        });
}

// Compile a prepared BuildContext. Shared between `mcpp build` and `mcpp run`
// so the latter doesn't call prepare_build twice (and re-print the toolchain
// resolution banner).
export int run_build_plan(BuildContext& ctx, bool verbose, bool no_cache,
                   std::string_view targetOverride = "") {
    // `--cache=off` means a cold build: no global cache, and target/ cleared —
    // which is exactly what `--no-cache` has always done, hence the alias.
    const bool coldBuild = no_cache || ctx.cacheMode == CacheMode::Off;
    if (coldBuild) {
        std::error_code ec;
        std::filesystem::remove_all(ctx.outputDir, ec);
    }

    // The generated `*link:` spec lives in the output directory, and the line
    // above is allowed to delete that directory. prepare wrote the file before
    // this point, so a cold build reached ninja with a link command naming a
    // spec that no longer existed -- `g++: fatal error: cannot read spec file`,
    // on every `--no-cache` build with gcc.
    //
    // Regenerating here rather than reordering: the invariant worth holding is
    // "the spec exists when ninja runs", and stating it as an invariant
    // survives the next thing that clears target/ (a user with `rm -rf`, for
    // one). The write is idempotent, so the warm path costs one stat.
    if (!ctx.plan.gccCleanSpecs.empty()) {
        std::error_code ec;
        if (!std::filesystem::exists(ctx.plan.gccCleanSpecs, ec))
            ctx.plan.gccCleanSpecs = mcpp::toolchain::write_clean_link_specs(
                ctx.tc.binaryPath, ctx.outputDir);
    }

    auto be = mcpp::build::make_ninja_backend();

    // M5.0: print "Inferred" banner when defaults / target inference fired.
    for (auto& note : ctx.manifest.inferredNotes) {
        mcpp::ui::status("Inferred", note);
    }

    // Announce the package being built (and any deps). A dep served from the
    // global cache says "Cached" and HOW MANY translation units that saved. The
    // count is the point: the bare word "Cached" was printed for three months
    // while ninja recompiled every one of those units behind it, and no output
    // contradicted it. A number that has to match the edges ninja actually skips
    // cannot be quietly wrong in the same way.
    std::map<std::string, std::size_t> cachedUnits;
    for (auto& dep : ctx.cachedDeps) cachedUnits[dep.name] = dep.units;
    std::set<std::string> announced;
    announced.insert(ctx.manifest.package.name);
    mcpp::ui::status("Compiling",
        std::format("{} v{} (.)",
                    ctx.manifest.package.name, ctx.manifest.package.version));
    for (auto& [name, spec] : ctx.manifest.dependencies) {
        if (announced.contains(name)) continue;
        announced.insert(name);
        // `spec.version` is the constraint the manifest WROTE. Announcing it
        // printed "Compiling compat.imgui v^1.92.8" — a banner naming a version
        // that does not exist (mcpp#363). prepare_build hands the resolution
        // result over in ctx.resolvedVersions; fall back to the spec only for
        // deps that never went through resolution (git, or an exact pin).
        auto rit = ctx.resolvedVersions.find(name);
        std::string ver = spec.isPath()
            ? "(path)"
            : std::string("v") + (rit != ctx.resolvedVersions.end() ? rit->second
                                                                    : spec.version);
        auto it = cachedUnits.find(name);
        if (it == cachedUnits.end()) {
            mcpp::ui::status("Compiling", std::format("{} {}", name, ver));
        } else {
            mcpp::ui::status("Cached", std::format("{} {} ({} unit{})",
                name, ver, it->second, it->second == 1 ? "" : "s"));
        }
    }

    mcpp::build::BuildOptions opts;
    opts.verbose = verbose;
    auto r = be->build(ctx.plan, opts);
    if (!r) {
        std::fflush(stdout);
        mcpp::ui::error(r.error().message);
        if (!r.error().diagnosticOutput.empty()) {
            std::fputs(r.error().diagnosticOutput.c_str(), stderr);
            if (r.error().diagnosticOutput.back() != '\n')
                std::fputc('\n', stderr);
        }
        return 1;
    }

    // Populate the global cache for deps that did NOT hit. prepare_build leaves
    // depsToPopulate empty under --cache=local|off, so the mode gate is already
    // enforced there; asserting it again here keeps the write side legible on
    // its own terms rather than as a consequence of something in prepare.
    if (ctx.cacheMode != CacheMode::Global) ctx.depsToPopulate.clear();
    for (auto& task : ctx.depsToPopulate) {
        auto pr = mcpp::bmi_cache::populate_from(task.key, ctx.outputDir, task.artifacts);
        if (!pr) {
            mcpp::ui::warning(std::format(
                "bmi cache populate failed for {}@{}: {}",
                task.key.packageName, task.key.version, pr.error()));
        }
    }

    // P1.5: warn if fingerprint changed from last build (explains full rebuild).
    // Compared against the entry for the SAME profile: the profile is now a
    // fingerprint input, so a dev↔release switch always changes the fp. That is
    // exactly what the user asked for, and warning about it turns a useful
    // signal ("something you didn't expect invalidated your build dir") into
    // noise on every profile switch.
    {
        auto entries = read_build_cache(ctx.projectRoot);
        for (auto& e : entries) {
            if (e.targetTriple == targetOverride && e.profile == ctx.profile
                && e.cacheMode == cache_mode_name(ctx.cacheMode)
                && !e.fingerprint.empty()) {
                auto newFp = ctx.outputDir.filename().string();
                if (e.fingerprint != newFp) {
                    mcpp::ui::warning(std::format(
                        "fingerprint changed ({} → {}), full rebuild",
                        e.fingerprint, newFp));
                }
                break;
            }
        }
    }

    // P0: save build cache for fast-path on next invocation.
    if (!coldBuild && !r->ninjaProgram.empty()) {
        auto fpHex = ctx.outputDir.filename().string();
        auto runTargets = compute_run_targets(ctx.plan);
        auto [runEnvKey, runEnvValue] = compute_run_env(ctx.plan);
        auto subosDir = mcpp::xlings::paths::subos_dir_of(ctx.plan.toolchain.binaryPath);
        write_build_cache(ctx.projectRoot, ctx.outputDir, r->ninjaProgram,
                          std::string(targetOverride), fpHex,
                          r->runtimeEnvKey.empty() ? "-" : r->runtimeEnvKey,
                          r->runtimeEnvValue,
                          std::move(runTargets), runEnvKey, runEnvValue,
                          ctx.profile, std::string(cache_mode_name(ctx.cacheMode)),
                          subosDir ? subosDir->string() : std::string{});
    }

    // The one place the --strict policy is settled. Degradations reported by
    // the backend (e.g. a toolchain/platform combination that cannot emit a
    // depfile, #257) are discovered during emission, so this has to come
    // after the build rather than at the end of prepare_build. Without this
    // call the whole diag channel would report and then be ignored — the
    // exact failure mode it exists to prevent.
    if (!mcpp::diag::flush(ctx.strict)) return 1;

    // The descriptor comes from the knobs this build actually resolved, so it
    // cannot disagree with the compiler flags the way the old hardcoded
    // "release [optimized]" did.
    {
        const auto& bc = ctx.manifest.buildConfig;
        std::string descriptor =
            (bc.optLevel.empty() || bc.optLevel == "0") ? "unoptimized" : "optimized";
        if (bc.debug) descriptor += " + debuginfo";
        if (bc.lto)   descriptor += " + lto";
        mcpp::ui::finished(ctx.profile, r->elapsed, descriptor);
    }
    return 0;
}

// ─── P0 fast-path: skip prepare_build when build.ninja is fresh ──────
//
// On a successful build, we write `target/.build_cache` containing the
// outputDir path. On the next invocation, if build.ninja in that dir
// is newer than all source files and mcpp.toml, we invoke ninja directly
// without re-running the scanner, make_plan, or emit phases.
//
// This reduces no-change builds from ~10s to <0.5s.

// mcpp#225: is any tracked source file under `projectRoot` newer than
// `ninjaTime`? Shared by try_fast_build's and try_fast_run's freshness
// gates. Uses expand_glob's bounded ("src" prefix) + vcs/build-dir-excluded
// walk instead of a hand-rolled recursive_directory_iterator — the OLD
// staleness check here walked ALL of src/ unfiltered (harmless when src/ is
// the whole tree, but wasteful/wrong the moment a huge unrelated directory
// lives elsewhere under the project root and gets swept in by some other
// caller's broader glob; and it's the same choke-point fix as expand_glob
// itself, see scanner.cppm).
bool sources_newer_than(const std::filesystem::path& projectRoot,
                        std::filesystem::file_time_type ninjaTime,
                        const std::vector<std::filesystem::path>& resourceScripts = {}) {
    std::error_code ec;
    // The root build.mcpp is a build input too — its directives shape
    // build.ninja (flags, generated/selected sources). A changed program must
    // abandon the fast path and fall through to prepare_build, where the
    // declared-input cache decides whether it actually re-runs. Without this
    // the documented "re-runs when the build.mcpp source itself changes" was
    // unreachable behind a fresh build.ninja.
    if (auto bp = projectRoot / "build.mcpp"; std::filesystem::exists(bp, ec)) {
        auto bt = std::filesystem::last_write_time(bp, ec);
        if (ec || bt > ninjaTime) return true;
    }
    // #359: a GLOB input changes without any existing file's mtime changing —
    // a new .proto appears and every timestamp below is unmoved. The mtime
    // sweep therefore cannot see it, and the fast path would report
    // "Finished dev in 0.00s" while the new file is never generated. Same
    // question as the build.mcpp check above, different kind of input.
    if (mcpp::build::glob_inputs_stale(projectRoot)) return true;
    // mcpp#365: an author-written `.rc` is a third input of the same kind. It
    // is not under src/ and has no C++ extension, so the sweep below cannot see
    // it — and unlike the icon or a header the script includes, editing it can
    // change WHAT THE GRAPH SHOULD BE: the implicit-input set comes from
    // scanning the script, and the "your VERSIONINFO is named by string"
    // diagnostic is produced while scanning. Both happen in prepare_build, so a
    // fresh build.ninja made the edit invisible — the resource itself rebuilt
    // (ninja tracks it), but a newly added `#include "ids.h"` went untracked and
    // the diagnostic never fired again after the first build.
    //
    // Only `files` is swept. `icon` and `extra-inputs` are already ninja
    // implicit inputs and changing them cannot change the shape of the graph,
    // so forcing a full prepare on every icon tweak would buy nothing.
    for (auto const& f : resourceScripts) {
        auto p = f.is_absolute() ? f : (projectRoot / f);
        auto ft = std::filesystem::last_write_time(p, ec);
        if (ec) { ec.clear(); continue; }   // missing → prepare_build reports it
        if (ft > ninjaTime) return true;
    }
    for (auto& f : mcpp::modgraph::expand_glob(projectRoot, "src/**/*")) {
        auto ext = f.extension().string();
        if (ext != ".cppm" && ext != ".cpp" && ext != ".cc" &&
            ext != ".cxx" && ext != ".c" && ext != ".h" && ext != ".hpp")
            continue;
        auto ft = std::filesystem::last_write_time(f, ec);
        if (ec || ft > ninjaTime) return true;
    }
    return false;
}

// mcpp#225: run ninja quietly against an already-verified-fresh build.ninja.
// Shared by try_fast_build (which just reports "Finished" on success) and
// try_fast_run (which goes on to locate + exec a binary). Returns nullopt
// when ninja's failure looks like a stale-graph signature — the caller
// should abandon the fast path and fall back to a full prepare_build — or
// an exit code otherwise (0 success; 1 hard failure, diagnostics already
// printed to stderr).
std::optional<int> run_ninja_fast(const std::string& ninjaProgram,
                                  const std::filesystem::path& outputDir,
                                  const std::filesystem::path& ninjaPath,
                                  bool verbose,
                                  const std::string& runtimeEnvKey,
                                  const std::string& runtimeEnvValue,
                                  std::chrono::milliseconds* elapsedOut = nullptr) {
    std::vector<std::string> argv{ninjaProgram};
    if (!verbose) argv.push_back("--quiet");
    argv.push_back("-C");
    argv.push_back(outputDir.string());
    if (verbose) argv.push_back("-v");

    std::vector<std::pair<std::string, std::string>> childEnv;
    if (runtimeEnvKey == "@env") {
        // Multi-var encoding (MSVC INCLUDE/LIB/PATH/VSLANG + optional runtime
        // pair): \x1f-separated k=v records in the single value slot.
        std::string_view rest = runtimeEnvValue;
        while (!rest.empty()) {
            auto sep = rest.find('\x1f');
            auto rec = rest.substr(0, sep);
            if (auto eq = rec.find('='); eq != std::string_view::npos && eq > 0)
                childEnv.emplace_back(std::string(rec.substr(0, eq)),
                                      std::string(rec.substr(eq + 1)));
            if (sep == std::string_view::npos) break;
            rest.remove_prefix(sep + 1);
        }
    } else if (runtimeEnvKey != "-" && !runtimeEnvValue.empty()) {
        childEnv.emplace_back(runtimeEnvKey, runtimeEnvValue);
    }

    auto t0 = std::chrono::steady_clock::now();
    // capture_exec merges stderr into the captured output (replacing `2>&1`),
    // so is_stale_ninja_failure / filter_ninja_output still see ninja errors.
    auto r = mcpp::platform::process::capture_exec(argv, childEnv);
    std::string out = r.output;
    int status = r.exit_code;
    if (status != 0) {
        if (is_stale_ninja_failure(out))
            return std::nullopt;
        std::fflush(stdout);
        mcpp::ui::error("build failed");
        auto prefixes = read_ninja_command_prefixes(ninjaPath);
        auto diagnostics = verbose ? out : mcpp::build::filter_ninja_output(out, prefixes);
        if (!diagnostics.empty()) {
            std::fputs(diagnostics.c_str(), stderr);
            if (diagnostics.back() != '\n')
                std::fputc('\n', stderr);
        }
        return 1;
    }
    if (verbose && !out.empty())
        std::fputs(out.c_str(), stdout);

    if (elapsedOut) {
        *elapsedOut = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0);
    }
    return 0;
}

// Which profile does this invocation mean? The fast paths exist precisely to
// avoid prepare_build, where the profile is normally settled — so they settle
// it here from the same pure rule (resolve_profile_name), which needs nothing
// but the manifest. nullopt = manifest unreadable ⇒ no fast path.
//
// Without this, an entry matched on target triple alone could have been built
// for a different profile, and the fast path would run ninja against that
// profile's build.ninja: `mcpp build --release` then a bare `mcpp build`
// reported success in 0.00s and left -O2 artifacts where -O0 -g was asked for.
struct FastPathIdentity {
    std::string profile;
    std::string cacheMode;
    // mcpp#365: author-written resource scripts, for the freshness sweep. They
    // ride along here because this is the one place on the fast path that
    // already parses the manifest — re-reading it to answer a second question
    // would be a second derivation of the same fact.
    std::vector<std::filesystem::path> resourceScripts;
};

std::optional<FastPathIdentity>
fast_path_identity(const std::filesystem::path& projectRoot,
                   std::string_view profileOverride = "") {
    auto m = mcpp::manifest::load(projectRoot / "mcpp.toml");
    if (!m) return std::nullopt;
    return FastPathIdentity{
        mcpp::build::resolve_profile_name(*m, profileOverride),
        std::string(mcpp::build::cache_mode_name(
            mcpp::build::resolve_cache_mode(*m, ""))),
        m->resources.files,
    };
}

// Try to fast-path: if build.ninja is newer than all inputs, just run ninja.
// Returns exit code on fast-path, or nullopt if full rebuild needed.
export std::optional<int> try_fast_build(const std::filesystem::path& projectRoot,
                                  bool verbose, bool no_cache,
                                  std::string_view currentTarget = "") {
    if (no_cache) return std::nullopt;

    auto want = fast_path_identity(projectRoot);
    if (!want) return std::nullopt;

    // P3: read multi-entry cache and find the entry matching this
    // (target, profile, cache mode) triple. Matching on the target alone served
    // the wrong profile's artifacts, and ignoring the cache mode replayed a
    // cache-reading graph for a request that asked not to read the cache.
    auto entries = read_build_cache(projectRoot);
    const BuildCacheEntry* match = nullptr;
    for (auto& e : entries) {
        if (e.targetTriple == currentTarget && e.profile == want->profile
            && e.cacheMode == want->cacheMode) {
            match = &e;
            break;
        }
    }
    if (!match) return std::nullopt;

    auto outputDirStr = match->outputDir;
    auto ninjaProgram = match->ninjaProgram;
    // Legacy caches stored a shell-quoted path; execvp needs the raw path.
    if (ninjaProgram.size() >= 2 && ninjaProgram.front() == '\''
                                 && ninjaProgram.back() == '\'')
        ninjaProgram = ninjaProgram.substr(1, ninjaProgram.size() - 2);
    auto cachedFingerprint = match->fingerprint;
    auto runtimeEnvKey = match->runtimeEnvKey;
    auto runtimeEnvValue = match->runtimeEnvValue;
    if (runtimeEnvKey.empty())
        return std::nullopt; // old cache entry; regenerate build.ninja once

    // P1: verify fingerprint matches the outputDir basename.
    if (!cachedFingerprint.empty()) {
        auto dirBasename = std::filesystem::path(outputDirStr).filename().string();
        if (dirBasename != cachedFingerprint) {
            return std::nullopt;
        }
    }

    std::error_code ec;
    std::filesystem::path outputDir(outputDirStr);

    auto ninjaPath = outputDir / "build.ninja";
    if (!std::filesystem::exists(ninjaPath, ec)) return std::nullopt;

    auto ninjaTime = std::filesystem::last_write_time(ninjaPath, ec);
    if (ec) return std::nullopt;

    // Check mcpp.toml
    auto tomlPath = projectRoot / "mcpp.toml";
    auto tomlTime = std::filesystem::last_write_time(tomlPath, ec);
    if (ec || tomlTime > ninjaTime) return std::nullopt;

    // mcpp#225: bounded + vcs/build-dir-excluded walk (see sources_newer_than)
    // instead of a hand-rolled recursive_directory_iterator over src/.
    if (sources_newer_than(projectRoot, ninjaTime, want->resourceScripts)) return std::nullopt;

    // All inputs are older than build.ninja → fast-path: just run ninja.
    std::chrono::milliseconds elapsed{};
    auto rc = run_ninja_fast(ninjaProgram, outputDir, ninjaPath, verbose,
                             runtimeEnvKey, runtimeEnvValue, &elapsed);
    if (!rc) return std::nullopt;
    if (*rc != 0) return rc;

    mcpp::ui::finished(want->profile, elapsed);
    return 0;
}

// mcpp#225 (E2): `mcpp run`'s fast path. Mirrors try_fast_build's
// fingerprint/freshness gate against the SAME cache entry `mcpp build`
// wrote (targetTriple == "" — `mcpp run` never takes a --target flag), then
// on a hit runs ninja and execs the cached run-target directly — skipping
// prepare_build (toolchain resolution + full modgraph scan) entirely.
// Returns nullopt when there's no usable cache entry (build_run_target
// falls back to the full prepare_build path, which also refreshes the
// cache for next time), an exit code otherwise.
std::optional<int> try_fast_run(const std::filesystem::path& projectRoot,
                                const std::optional<std::string>& targetName,
                                std::span<const std::string> passthrough) {
    auto want = fast_path_identity(projectRoot);
    if (!want) return std::nullopt;

    auto entries = read_build_cache(projectRoot);
    const BuildCacheEntry* match = nullptr;
    for (auto& e : entries) {
        if (e.targetTriple.empty() && e.profile == want->profile
            && e.cacheMode == want->cacheMode) {
            match = &e;
            break;
        }
    }
    if (!match || match->runTargets.empty()) return std::nullopt;

    auto outputDirStr = match->outputDir;
    auto ninjaProgram = match->ninjaProgram;
    // Legacy caches stored a shell-quoted path; execvp needs the raw path.
    if (ninjaProgram.size() >= 2 && ninjaProgram.front() == '\''
                                 && ninjaProgram.back() == '\'')
        ninjaProgram = ninjaProgram.substr(1, ninjaProgram.size() - 2);
    if (match->runtimeEnvKey.empty())
        return std::nullopt; // old cache entry; go through prepare_build once
    // Written before this mcpp knew about subos environments (mcpp#352). Taking
    // the fast path here would run the program without them -- which is the
    // defect this field exists to fix, surviving an upgrade.
    //
    // It survives it for a long time, too: the fast path's identity is the
    // profile, the cache mode and the resource list, and its fingerprint check
    // compares a cached entry against ITSELF. Neither notices that a different
    // mcpp wrote the entry, so without this line an upgraded mcpp would reuse a
    // pre-upgrade build until something else happened to invalidate it. Measured
    // on a real upgrade from 2026.8.7.1, not reasoned about.
    if (!match->subosRecorded)
        return std::nullopt; // predates `subos=`; rebuild once, then it is there

    // P1: verify fingerprint matches the outputDir basename.
    if (!match->fingerprint.empty()) {
        auto dirBasename = std::filesystem::path(outputDirStr).filename().string();
        if (dirBasename != match->fingerprint) return std::nullopt;
    }

    // Locate the requested run-target before doing any filesystem freshness
    // work — an unrecognized name falls back to prepare_build, which gives
    // a proper "no binary target 'x' found" error instead of a silent miss.
    const std::pair<std::string, std::string>* chosen = nullptr;
    for (auto& rt : match->runTargets) {
        if (targetName && rt.first != *targetName) continue;
        chosen = &rt;
        if (targetName) break;
    }
    if (!chosen) return std::nullopt;

    std::error_code ec;
    std::filesystem::path outputDir(outputDirStr);
    auto ninjaPath = outputDir / "build.ninja";
    if (!std::filesystem::exists(ninjaPath, ec)) return std::nullopt;
    auto ninjaTime = std::filesystem::last_write_time(ninjaPath, ec);
    if (ec) return std::nullopt;

    auto tomlPath = projectRoot / "mcpp.toml";
    auto tomlTime = std::filesystem::last_write_time(tomlPath, ec);
    if (ec || tomlTime > ninjaTime) return std::nullopt;

    if (sources_newer_than(projectRoot, ninjaTime, want->resourceScripts)) return std::nullopt;

    // Fresh → run ninja (picks up any incremental object/link work) then
    // exec the cached exe path directly.
    auto rc = run_ninja_fast(ninjaProgram, outputDir, ninjaPath, /*verbose=*/false,
                             match->runtimeEnvKey, match->runtimeEnvValue);
    if (!rc) return std::nullopt;
    if (*rc != 0) return rc;

    auto exe = outputDir / chosen->second;
    auto pathCtx = mcpp::fetcher::make_path_ctx(/*cfg=*/nullptr, projectRoot);
    mcpp::ui::status("Running",
        std::format("`{}`", mcpp::ui::shorten_path(exe, pathCtx)));
    std::println("");
    std::fflush(stdout);
    std::vector<std::string> argv;
    argv.push_back(exe.string());
    for (auto& a : passthrough) argv.push_back(a);

    std::vector<std::pair<std::string, std::string>> childEnv;
    if (!match->runEnvKey.empty() && !match->runEnvValue.empty())
        childEnv.emplace_back(match->runEnvKey, match->runEnvValue);
    // ...and the subos's declared environment, re-READ here rather than taken
    // from the cache. Which subos is a build property (cached above); what it
    // declares is the subos's own, and a user who installs a graphics stack
    // between two runs must get it without rebuilding.
    //
    // This is the half that a fast path is most likely to lose, and losing it
    // would be invisible in the worst way: the first `mcpp run` after a build
    // takes the full path and works, every later one takes this path and does
    // not. A GL program would run once and then stop finding its driver.
    {
        // Same rule as the full path, through the same helper: an override
        // for this invocation, else the subos this build was recorded against.
        auto subosDir = subos_dir_for_run(std::filesystem::path(match->subosDir));
        if (!subosDir.empty()) {
            auto info = mcpp::xlings::subos::read(subosDir);
            for (auto& kv : mcpp::xlings::subos::resolve_env(
                     info, subosDir,
                     [](std::string_view v) -> std::optional<std::string> {
                         if (const char* e = std::getenv(std::string(v).c_str()))
                             return std::string(e);
                         return std::nullopt;
                     }))
                childEnv.push_back(std::move(kv));
        }
    }

    return mcpp::platform::process::run_exec(argv, childEnv) == 0 ? 0 : 1;
}

// `mcpp run` driver: build, locate the binary target, exec it with the
// resolved runtime environment. `package_filter` (`-p`/`--package`) scopes
// a workspace invocation to one member — single-member only, no
// `--workspace` fan-out (running N binaries in one invocation isn't a
// coherent "run"). Threaded straight to prepare_build's BuildOverrides,
// which already does the member switch (basename OR member path — the same
// rule mcpp::project::resolve_member_dir documents for build/test).
export int build_run_target(const std::optional<std::string>& targetName,
                            std::span<const std::string> passthrough,
                            const std::string& package_filter = {},
                            const std::string& cache_mode = {},
                            bool no_cache = false) {
    // mcpp#225 (E2): reuse the resolved build cache when it's still fresh,
    // skipping prepare_build's toolchain resolution + modgraph scan
    // entirely — mirrors cmd_build's try_fast_build fast path. The cached
    // entry was written for whichever package occupied the project root
    // last time; a `-p` filter always needs prepare_build's member switch,
    // so skip the fast path in that case (mirrors cmd_build's fast-path
    // bypass whenever ov.package_filter is set).
    // A --cache/--no-cache override also bypasses the fast path, for the same
    // reason --profile does: the cached build.ninja was generated under the
    // previous mode, so reusing it would silently ignore the flag.
    if (package_filter.empty() && cache_mode.empty() && !no_cache) {
        if (auto root = mcpp::project::find_manifest_root(std::filesystem::current_path())) {
            if (auto rc = try_fast_run(*root, targetName, passthrough)) {
                return *rc;
            }
        }
    }

    // Build first. Single prepare_build → drive build → reuse ctx to locate
    // the binary, so we don't re-resolve the toolchain or re-scan modgraph.
    mcpp::build::BuildOverrides ov;
    ov.package_filter = package_filter;
    ov.cache_mode     = cache_mode;
    auto ctx = prepare_build(/*print_fp=*/false, /*includeDevDeps=*/false,
                             /*extraTargets=*/{}, ov);
    if (!ctx) { std::println(stderr, "error: {}", ctx.error()); return 2; }
    if (auto rc = run_build_plan(*ctx, /*verbose=*/false, no_cache); rc != 0)
        return rc;

    // Find binary target
    const mcpp::build::LinkUnit* chosen = nullptr;
    for (auto& lu : ctx->plan.linkUnits) {
        if (lu.kind != mcpp::build::LinkUnit::Binary) continue;
        if (targetName && lu.targetName != *targetName) continue;
        chosen = &lu;
        if (targetName) break;
    }
    if (!chosen) {
        std::println(stderr, "error: no binary target {}",
            targetName ? std::format("'{}' found", *targetName) : "in this package");
        return 2;
    }

    auto exe = ctx->outputDir / chosen->output;
    auto pathCtx = mcpp::fetcher::make_path_ctx(/*cfg=*/nullptr, ctx->projectRoot);
    mcpp::ui::status("Running",
        std::format("`{}`", mcpp::ui::shorten_path(exe, pathCtx)));
    std::println("");
    std::fflush(stdout);
    std::vector<std::string> argv;
    argv.push_back(exe.string());
    for (auto& a : passthrough) argv.push_back(a);

    std::vector<std::pair<std::string, std::string>> childEnv;
    auto [runEnvKey, runEnvValue] = compute_run_env(ctx->plan);
    if (!runEnvKey.empty() && !runEnvValue.empty())
        childEnv.emplace_back(runEnvKey, runEnvValue);
    // ...plus whatever the subos declares for the programs it hosts (#352).
    for (auto& kv : compute_subos_env(ctx->plan)) childEnv.push_back(std::move(kv));

    // Direct exec (no /bin/sh): the loader env reaches ONLY the target child,
    // never mcpp or a host shell. Fixes the bundled-glibc-vs-host-libtinfo
    // crash on newer-glibc distros.
    return mcpp::platform::process::run_exec(argv, childEnv) == 0 ? 0 : 1;
}

export enum class TestMessageFormat { Human, Json };

export struct TestOptions {
    std::string        filter;   // substring match on the path-based test name; empty = all
    TestMessageFormat  format = TestMessageFormat::Human;
    bool               list = false;   // enumerate only, no build/run
    // Per-test RUN deadline. The default is deliberately non-zero: `mcpp test`
    // is something CI runs unattended, and an unbounded default makes a single
    // hung test able to consume the whole job with nothing to show for it.
    // `--timeout 0` still means "no limit", it just has to be asked for.
    int                timeoutSecs = 300;
    // Per-ninja-invocation deadline (Phase A, the bulk pass, and each per-test
    // drive are timed separately). Covers the half `--timeout` never could:
    // a compile or link that never returns. POSIX only — see BuildOptions.
    //
    // Unlike timeoutSecs this defaults to OFF, and the asymmetry is measured,
    // not stylistic. A single test binary running longer than five minutes is
    // unusual; a cold dependency build taking longer than fifteen is ordinary —
    // one mcpp-index member (OpenCV from source) measures 1019s on Linux and
    // 1289s on Windows. A default ceiling would turn those slow-but-correct
    // builds red and blame mcpp for it. "How long may a build take" is a
    // property of the project, so the project says it; mcpp only has to make
    // saying it possible, which is what was missing.
    int                buildTimeoutSecs = 0;
};

// What one member's `run_tests` actually did. `--workspace` fans out over
// members and needs this to report per-member progress and a workspace total;
// the exit code alone cannot say how many tests ran or where the time went.
export struct TestRunSummary {
    int       passed    = 0;
    int       failed    = 0;
    long long buildMs   = 0;   // Phase A + bulk pass + per-test drives
    long long runMs     = 0;   // the test binaries' own execution
    long long elapsedMs = 0;   // wall clock for the whole member
    bool      packageError = false;   // Phase A failed: no test ever ran
};

// Minimal JSON string escaping for the --message-format json records. Same
// shape as json_escape in cmd_xpkg.cppm — kept local (15 lines) rather than
// shared across the cli/build module boundary.
static std::string test_json_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20)
                    out += std::format("\\u{:04x}", static_cast<unsigned char>(c));
                else out += c;
        }
    }
    return out;
}

// `mcpp test` driver: discover tests/**/*.cpp, synthesize targets, build
// with dev-deps, run each test binary, summarize.
export int run_tests(std::span<const std::string> passthrough,
                     BuildOverrides overrides = {},
                     TestOptions testOpts = {},
                     TestRunSummary* summaryOut = nullptr) {
    const bool json = (testOpts.format == TestMessageFormat::Json);
    // The member this call is scoped to (empty outside a workspace). Threaded
    // into every JSON record so a `--workspace` stream can be attributed: a
    // bare test name is ambiguous the moment two members both have a `smoke`.
    const std::string memberName = overrides.package_filter;
    TestRunSummary summary;
    struct SummaryWriter {
        TestRunSummary* out; const TestRunSummary* src;
        ~SummaryWriter() { if (out) *out = *src; }
    } summaryWriter{summaryOut, &summary};
    // Wall clock for the WHOLE member, started before Phase A. The old `t0`
    // sat after Phase A and the bulk pass, so `finished in` reported only the
    // per-test loop: measured on one member, 6.53s printed against 93.5s
    // actual — a 14x understatement, and worst exactly on the build-heavy
    // members where the number matters.
    auto tMember = std::chrono::steady_clock::now();
    auto member_ms = [&tMember] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - tMember).count();
    };
    // JSON mode: stdout carries NDJSON only. All ui::status/info lines print
    // to stdout, so silence them wholesale; errors already go to stderr.
    if (json) mcpp::ui::set_quiet(true);

    auto root = mcpp::project::find_manifest_root(std::filesystem::current_path());
    if (!root) {
        mcpp::ui::error("no mcpp.toml found in current directory or any parent");
        return 2;
    }

    // Workspace scoping: discovery must run against the MEMBER, not the
    // workspace root — otherwise `tests/**/*.cpp` globs every member's tests
    // together (two `tests/main.cpp` → "duplicate test name 'main'"). When a
    // member is selected (via -p, threaded as package_filter), glob from its
    // dir; prepare_build below resolves the SAME member, so the two agree.
    // (--workspace fans out over members at the cmd layer, one call per member.)
    auto testRoot = *root;
    if (auto rm = mcpp::manifest::load(*root / "mcpp.toml"); rm) {
        auto member = mcpp::project::resolve_member_dir(*rm, *root, overrides.package_filter);
        if (!member) { mcpp::ui::error(member.error()); return 2; }
        if (!member->empty()) testRoot = *member;
    }

    // 1. Discover test files (scoped to the member/package).
    auto testFiles = mcpp::modgraph::expand_glob(testRoot, "tests/**/*.cpp");
    if (testFiles.empty()) {
        std::println("no tests found in tests/");
        return 0;
    }

    // [build].flags globs also cover tests: a glob names files — whether they
    // are scanned sources or test TUs is orthogonal. Matched entries ride the
    // per-target flag channel (issue #131) on the synthesized test target.
    // (Feature-folded entries are prepare-time state; tests take the base
    // [build].flags — sufficient for per-test compile options.)
    struct TestGlobFlags {
        mcpp::manifest::GlobFlags       gf;
        std::set<std::filesystem::path> files;
    };
    std::vector<TestGlobFlags> testGlobFlags;
    if (auto mm = mcpp::manifest::load(testRoot / "mcpp.toml")) {
        for (auto const& gf : mm->buildConfig.globFlags) {
            auto hits = mcpp::modgraph::expand_glob(testRoot, gf.glob);
            testGlobFlags.push_back({gf, {hits.begin(), hits.end()}});
        }
    }

    // 2. Synthesize a Target for each test file.
    //    Name = path relative to tests/, extension dropped, '/' separators —
    //    so tests/00-a/0.cpp and tests/01-b/0.cpp coexist as '00-a/0' and
    //    '01-b/0' (stems alone would collide). Flat layouts keep their old
    //    names ('tests/smoke.cpp' → 'smoke').
    std::vector<mcpp::manifest::Target> testTargets;
    std::set<std::string> seenNames;
    for (auto& f : testFiles) {
        auto rel  = std::filesystem::relative(f, testRoot / "tests");
        auto name = rel.replace_extension("").generic_string();
        if (!seenNames.insert(name).second) {
            mcpp::ui::error(std::format(
                "duplicate test name '{}' (two test files map to the same name)", name));
            return 2;
        }
        mcpp::manifest::Target t;
        t.name = name;
        t.kind = mcpp::manifest::Target::TestBinary;
        // Relative to the member/package root prepare_build will operate on.
        t.main = std::filesystem::relative(f, testRoot).string();
        for (auto const& tgf : testGlobFlags) {
            if (!tgf.files.contains(f)) continue;
            for (auto const& d  : tgf.gf.defines)  t.defines.push_back(d);
            for (auto const& fl : tgf.gf.cflags)   t.cflags.push_back(fl);
            for (auto const& fl : tgf.gf.cxxflags) t.cxxflags.push_back(fl);
        }
        testTargets.push_back(std::move(t));
    }

    // --list: enumerate (filtered) tests and stop — no toolchain resolution,
    // no build. Names/paths come straight from discovery, so this also works
    // on tests that do not currently compile.
    if (testOpts.list) {
        std::size_t total = 0;
        for (auto& t : testTargets) {
            if (!testOpts.filter.empty()
                && t.name.find(testOpts.filter) == std::string::npos) continue;
            ++total;
            auto abs = std::filesystem::absolute(testRoot / t.main)
                           .lexically_normal().generic_string();
            if (json)
                std::println("{{\"member\":\"{}\",\"test\":\"{}\",\"main\":\"{}\"}}",
                             test_json_escape(memberName),
                             test_json_escape(t.name), test_json_escape(abs));
            else
                std::println("{}", t.name);
        }
        if (json) {
            std::println("{{\"summary\":{{\"total\":{}}}}}", total);
            std::fflush(stdout);
        }
        return 0;
    }

    // 3. prepare_build with dev-deps enabled + synthetic targets.
    auto ctx = prepare_build(/*print_fp=*/false,
                             /*includeDevDeps=*/true,
                             std::move(testTargets),
                             std::move(overrides));
    if (!ctx) { mcpp::ui::error(ctx.error()); return 2; }

    // Filter guard. The filter selects at the build/run stage ONLY — the plan
    // above always contains every test, so build.ninja and
    // compile_commands.json stay complete (clangd depends on the latter; a
    // filtered run must not clobber it down to one entry).
    auto filter_match = [&](const mcpp::build::LinkUnit& lu) {
        return lu.kind == mcpp::build::LinkUnit::TestBinary
            && (testOpts.filter.empty()
                || lu.targetName.find(testOpts.filter) != std::string::npos);
    };
    if (!testOpts.filter.empty()) {
        bool any = false;
        for (auto& lu : ctx->plan.linkUnits)
            if (filter_match(lu)) { any = true; break; }
        if (!any) {
            if (json)
                std::println("{{\"error\":\"no-tests-matched\",\"filter\":\"{}\"}}",
                             test_json_escape(testOpts.filter));
            mcpp::ui::error(std::format("no tests match '{}'", testOpts.filter));
            return 2;
        }
    }

    // 4. "Compiling test_X (test)" lines for the test binaries.
    std::map<std::string, std::size_t> cachedUnits;
    for (auto& dep : ctx->cachedDeps) cachedUnits[dep.name] = dep.units;
    auto announce = [&](const std::string& name,
                        const mcpp::manifest::DependencySpec& spec,
                        std::string_view suffix) {
        std::string ver = spec.isPath() ? "(path)" : std::string("v") + spec.version;
        auto it = cachedUnits.find(name);
        if (it == cachedUnits.end()) {
            mcpp::ui::status("Compiling",
                std::format("{} {}{}", name, ver, suffix));
        } else {
            mcpp::ui::status("Cached",
                std::format("{} {} ({} unit{}){}", name, ver, it->second,
                            it->second == 1 ? "" : "s", suffix));
        }
    };
    std::set<std::string> announced;
    announced.insert(ctx->manifest.package.name);
    mcpp::ui::status("Compiling",
        std::format("{} v{} (.)",
                    ctx->manifest.package.name, ctx->manifest.package.version));
    for (auto& [name, spec] : ctx->manifest.dependencies) {
        if (announced.contains(name)) continue;
        announced.insert(name);
        announce(name, spec, "");
    }
    for (auto& [name, spec] : ctx->manifest.devDependencies) {
        if (announced.contains(name)) continue;
        announced.insert(name);
        announce(name, spec, " (dev)");
    }
    // List test binaries.
    // (Per-test "Compiling" lines print in Phase B, interleaved with each
    // test's own result — announcing them all up front separated the three
    // pieces of one test's story across the whole output.)

    // 5. Two-phase build. Phase A: package-level artifacts (everything that
    //    is not a test binary — libs, deps). A failure here is the PACKAGE's
    //    fault, not any single test's: report it as a build error, never as
    //    N red tests. Phase B (below): each test is built as its own ninja
    //    goal, so a compile failure is attributed to exactly that test and
    //    the rest still build and run.
    struct TestResult {
        std::string name;
        enum class St { Pass, CompileFail, RunFail } status;
        int         exitCode = 0;
        std::string compileOutput;
        std::string runOutput;
        long long   durationMs = 0;    // build+run wall time for THIS test
        bool        timedOut = false;  // killed by --timeout
    };
    std::vector<TestResult> results;

    // Streaming NDJSON: one record per test, emitted as it finishes — a
    // consumer (e.g. the d2x provider) sees progress live, and a crash
    // mid-run still leaves the completed records on stdout.
    auto emit_json = [&](const TestResult& r) {
        if (!json) return;
        const char* st = r.status == TestResult::St::Pass ? "pass"
                       : r.status == TestResult::St::CompileFail ? "compile_fail"
                                                                 : "run_fail";
        std::string signal = (r.exitCode > 128 && r.exitCode < 128 + 65)
            ? std::to_string(r.exitCode - 128) : "null";
        std::println("{{\"member\":\"{}\",\"test\":\"{}\",\"status\":\"{}\","
                     "\"exit_code\":{},\"signal\":{},"
                     "\"duration_ms\":{},\"timed_out\":{},"
                     "\"compile_output\":\"{}\",\"run_output\":\"{}\"}}",
                     test_json_escape(memberName),
                     test_json_escape(r.name), st, r.exitCode, signal, r.durationMs,
                     r.timedOut ? "true" : "false",
                     test_json_escape(r.compileOutput), test_json_escape(r.runOutput));
        std::fflush(stdout);
    };

    auto backend = mcpp::build::make_ninja_backend();

    // Phase A goal set: every shared prerequisite — all package/dep compile
    // units EXCEPT the tests' own main TUs, plus any non-test link outputs.
    // In test mode the lib link unit is skipped entirely (plan.cppm), so the
    // package's module objects are the only place shared breakage can show
    // up; building them here is what keeps a broken src/ module a PACKAGE
    // error instead of N identical per-test compile failures.
    std::set<std::filesystem::path> testMains;
    for (auto& lu : ctx->plan.linkUnits)
        if (lu.kind == mcpp::build::LinkUnit::TestBinary && lu.entryMain)
            testMains.insert(*lu.entryMain);
    std::vector<std::string> pkgTargets;
    for (auto& cu : ctx->plan.compileUnits)
        if (!testMains.contains(cu.source))
            pkgTargets.push_back(cu.object.generic_string());
    for (auto& lu : ctx->plan.linkUnits)
        if (lu.kind != mcpp::build::LinkUnit::TestBinary)
            pkgTargets.push_back(lu.output.generic_string());
    if (!pkgTargets.empty()) {
        mcpp::build::BuildOptions aOpts;
        aOpts.ninjaTargets = pkgTargets;
        aOpts.buildTimeoutSecs = static_cast<unsigned>(testOpts.buildTimeoutSecs);
        auto tPhaseA = std::chrono::steady_clock::now();
        auto a = backend->build(ctx->plan, aOpts);
        summary.buildMs += std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - tPhaseA).count();
        if (!a) {
            summary.packageError = true;
            summary.elapsedMs = member_ms();
            std::fflush(stdout);
            if (json)
                std::println("{{\"error\":\"package\",\"compile_output\":\"{}\"}}",
                             test_json_escape(a.error().diagnosticOutput));
            mcpp::ui::error(a.error().message);
            // Surface the compiler/linker stderr (parity with run_build_plan) —
            // otherwise `mcpp test` failures show only "build failed" with no
            // diagnostic, which is undebuggable (notably on CI).
            if (!a.error().diagnosticOutput.empty()) {
                std::fputs(a.error().diagnosticOutput.c_str(), stderr);
                if (a.error().diagnosticOutput.back() != '\n')
                    std::fputc('\n', stderr);
            }
            return 1;
        }

        // M3.2: populate BMI cache for deps that did NOT hit cache — deps
        // are package-level artifacts, so this belongs right after Phase A.
        for (auto& task : ctx->depsToPopulate) {
            auto pr = mcpp::bmi_cache::populate_from(task.key, ctx->outputDir, task.artifacts);
            if (!pr) {
                mcpp::ui::warning(std::format(
                    "bmi cache populate failed for {}@{}: {}",
                    task.key.packageName, task.key.version, pr.error()));
            }
        }

        // No "Finished test" line here: Phase A only built the shared
        // prerequisites. Printing a success banner right before per-test
        // failures read as a contradiction; the final summary carries timing.
    }

    // 6. Phase B. First a single keep-going bulk build over every selected
    //    test goal — ninja parallelizes across tests and a failing test does
    //    not stop the rest (-k 0). The result is deliberately ignored: the
    //    per-test loop below re-drives each goal, where successes are cache
    //    hits (near no-ops) and failures re-fail fast, yielding cleanly
    //    attributed per-test diagnostics without sacrificing parallelism.
    {
        mcpp::build::BuildOptions bulk;
        bulk.keepGoing = true;
        bulk.buildTimeoutSecs = static_cast<unsigned>(testOpts.buildTimeoutSecs);
        for (auto& lu : ctx->plan.linkUnits)
            if (filter_match(lu))
                bulk.ninjaTargets.push_back(lu.output.generic_string());
        if (!bulk.ninjaTargets.empty()) {
            auto tBulk = std::chrono::steady_clock::now();
            (void)backend->build(ctx->plan, bulk);
            summary.buildMs += std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - tBulk).count();
        }
    }

    //    Then build + run each test in sequence; collect results.

    auto runtimeEnvKey = mcpp::platform::env::runtime_library_path_key();
    auto runtimeEnvValue = mcpp::platform::env::prepend_path_list(
        runtimeEnvKey, ctx->plan.runtimeLibraryDirs);
    // Read once for the whole run rather than per test: it is one file, and
    // every test in a run belongs to the same subos.
    const auto subosEnv = compute_subos_env(ctx->plan);

    // macOS deliberately has no runtime-library-path key (env.cppm): injecting
    // DYLD_LIBRARY_PATH would reach every executable ninja launches and can
    // make system frameworks load a private libc++. The consequence is that a
    // test needing `[runtime] library_dirs` passes on Linux/Windows and fails
    // here with a dyld error that names neither the cause nor the platform —
    // so say it out loud rather than leaving the difference silent.
    if constexpr (mcpp::platform::is_macos) {
        if (runtimeEnvKey.empty() && !ctx->plan.runtimeLibraryDirs.empty()) {
            mcpp::diag::warning("test/runtime-path",
                "macOS does not inject a runtime library path for test binaries "
                "(DYLD_LIBRARY_PATH is deliberately not set); dependencies must be "
                "reachable through the binary's rpath. A dyld 'image not found' "
                "failure below is this difference, not a broken test.");
        }
    }

    for (auto& lu : ctx->plan.linkUnits) {
        if (!filter_match(lu)) continue;

        auto tTest = std::chrono::steady_clock::now();
        auto test_ms = [&tTest] {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - tTest).count();
        };

        mcpp::ui::status("Compiling", std::format("{} (test)", lu.targetName));

        mcpp::build::BuildOptions bOpts;
        bOpts.ninjaTargets = {lu.output.generic_string()};
        bOpts.buildTimeoutSecs = static_cast<unsigned>(testOpts.buildTimeoutSecs);
        auto tBuild = std::chrono::steady_clock::now();
        auto b = backend->build(ctx->plan, bOpts);
        summary.buildMs += std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - tBuild).count();
        if (!b) {
            if (!json) {
                // The test's own diagnostics, right under its FAIL line — a
                // reader fixes one test with one contiguous block of output.
                mcpp::ui::plain(std::format("{} ... FAIL ({}, {:.2f}s)",
                                            lu.targetName,
                                            b.error().timedOut
                                                ? std::format("build timeout after {}s",
                                                              testOpts.buildTimeoutSecs)
                                                : std::string{"compile"},
                                            static_cast<double>(test_ms()) / 1000.0));
                std::fflush(stdout);
                if (!b.error().diagnosticOutput.empty()) {
                    std::fputs(b.error().diagnosticOutput.c_str(), stderr);
                    if (b.error().diagnosticOutput.back() != '\n')
                        std::fputc('\n', stderr);
                    std::fflush(stderr);
                }
            }
            results.push_back({lu.targetName, TestResult::St::CompileFail, 0,
                               b.error().diagnosticOutput, {}, test_ms()});
            emit_json(results.back());
            continue;
        }

        auto exe = ctx->outputDir / lu.output;
        mcpp::ui::status("Running", std::format("bin/{}", lu.targetName));

        std::vector<std::string> argv;
        argv.push_back(exe.string());
        for (auto& a : passthrough) argv.push_back(a);

        std::vector<std::pair<std::string, std::string>> childEnv;
        if (!runtimeEnvKey.empty() && !runtimeEnvValue.empty())
            childEnv.emplace_back(runtimeEnvKey, runtimeEnvValue);
        // ...and the subos's declared environment, same as `mcpp run` (#352).
        // A GL test that cannot find a driver fails the same way a GL program
        // does, so it must be told the same things.
        for (auto& kv : subosEnv) childEnv.push_back(kv);

        // Prepend the sandbox's subos/default/bin to the CHILD PATH so test
        // binaries that shell out to bootstrapped tools (patchelf, ninja) find
        // them — applied to the child only, not via a leaky shell prefix.
        if constexpr (!mcpp::platform::is_windows) {
            if (auto xpkgs = mcpp::xlings::paths::xpkgs_from_compiler(ctx->tc.binaryPath)) {
                // xpkgs is <registry>/data/xpkgs → registry = xpkgs/../..
                auto registryDir = xpkgs->parent_path().parent_path();
                auto sandboxBin  = registryDir / "subos" / "default" / "bin";
                if (std::filesystem::exists(sandboxBin)) {
                    std::array<std::filesystem::path, 1> extra{sandboxBin};
                    auto pathVal = mcpp::platform::env::prepend_path_list("PATH", extra);
                    if (!pathVal.empty()) childEnv.emplace_back("PATH", pathVal);
                }
            }
        }

        // JSON mode captures the test's combined stdout+stderr into the
        // record; human mode streams it to the terminal as before.
        auto deadline = std::chrono::milliseconds(
            static_cast<long long>(testOpts.timeoutSecs) * 1000);
        bool timedOut = false;
        int exitCode;
        std::string runOutput;
        auto tRun = std::chrono::steady_clock::now();
        if (json) {
            auto rr = mcpp::platform::process::capture_exec_deadline(
                argv, childEnv, deadline, &timedOut);
            exitCode  = rr.exit_code;
            runOutput = std::move(rr.output);
        } else {
            exitCode = mcpp::platform::process::run_exec_deadline(
                argv, childEnv, deadline, &timedOut);
        }
        summary.runMs += std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - tRun).count();

        if (timedOut) {
            if (!json) mcpp::ui::plain(std::format("{} ... FAIL (timeout after {}s)",
                                                   lu.targetName, testOpts.timeoutSecs));
            results.push_back({lu.targetName, TestResult::St::RunFail, exitCode, {},
                               std::move(runOutput), test_ms(), true});
        } else if (exitCode == 0) {
            if (!json) mcpp::ui::plain(std::format("{} ... ok ({:.2f}s)", lu.targetName,
                                                   static_cast<double>(test_ms()) / 1000.0));
            results.push_back({lu.targetName, TestResult::St::Pass, 0, {},
                               std::move(runOutput), test_ms()});
        } else {
            if (!json) mcpp::ui::plain(std::format("{} ... FAIL (exit {}, {:.2f}s)",
                                                   lu.targetName, exitCode,
                                                   static_cast<double>(test_ms()) / 1000.0));
            results.push_back({lu.targetName, TestResult::St::RunFail, exitCode, {},
                               std::move(runOutput), test_ms()});
        }
        emit_json(results.back());
    }
    summary.elapsedMs = member_ms();

    // 7. Summary.
    int passed = 0;
    int failed = 0;
    std::vector<std::string> failures;
    for (auto& r : results) {
        if (r.status == TestResult::St::Pass) ++passed;
        else { ++failed; failures.push_back(r.name); }
    }

    summary.passed = passed;
    summary.failed = failed;

    // "build X + run Y" rather than one merged number: on a member whose tests
    // are cheap but whose link is not, those two are three orders of magnitude
    // apart, and only the split says which one to go look at.
    auto timing = std::format("{:.2f}s (build {:.2f}s + run {:.2f}s)",
                              static_cast<double>(summary.elapsedMs) / 1000.0,
                              static_cast<double>(summary.buildMs)   / 1000.0,
                              static_cast<double>(summary.runMs)     / 1000.0);

    if (json) {
        std::println("{{\"summary\":{{\"member\":\"{}\",\"passed\":{},\"failed\":{},"
                     "\"elapsed_ms\":{},\"build_ms\":{},\"run_ms\":{}}}}}",
                     test_json_escape(memberName), passed, failed,
                     summary.elapsedMs, summary.buildMs, summary.runMs);
        std::fflush(stdout);
        return failed == 0 ? 0 : 1;
    }

    std::println("");
    if (failed == 0) {
        mcpp::ui::status("test result",
            std::format("ok. {} passed; 0 failed; finished in {}", passed, timing));
        return 0;
    }
    mcpp::ui::error(std::format(
        "test result: FAILED. {} passed; {} failed; finished in {}",
        passed, failed, timing));
    std::println("");
    std::println("failures:");
    for (auto& n : failures) std::println("    {}", n);
    // (Each compile failure's diagnostics already printed inline under its
    // FAIL line in Phase B — the summary stays a compact name list.)
    return 1;
}

// `mcpp clean` driver.
export int clean_project(bool wipe_bmi) {
    auto root = mcpp::project::find_manifest_root(std::filesystem::current_path());
    if (!root) { std::println(stderr, "error: not in an mcpp package"); return 2; }
    std::error_code ec;
    std::filesystem::remove_all(*root / "target", ec);
    if (ec) {
        std::println(stderr, "error: cannot remove target/: {}", ec.message());
        return 1;
    }
    std::println("Cleaned: {}", (*root / "target").string());

    if (wipe_bmi) {
        auto cache = mcpp::toolchain::default_cache_root();
        std::filesystem::remove_all(cache, ec);
        std::println("Cleaned build cache: {}", cache.string());
        std::println("  (`mcpp cache clean --legacy` also removes the unused "
                     "pre-v1 cache, if any)");
    }
    return 0;
}

} // namespace mcpp::build
