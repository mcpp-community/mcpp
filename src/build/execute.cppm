// mcpp.build.execute — drives a prepared BuildContext: ninja execution,
// build cache + fast-path rebuilds, and the run/test/clean pipelines.
// Bodies moved verbatim from the CLI layer. Zero behavior change.

module;
#include <cstdio>
#include <cstdlib>

export module mcpp.build.execute;

import std;
import mcpp.build.prepare;
import mcpp.build.plan;
import mcpp.build.backend;
import mcpp.build.ninja;
import mcpp.bmi_cache;
import mcpp.manifest;
import mcpp.modgraph.scanner;
import mcpp.toolchain.stdmod;
import mcpp.xlings;
import mcpp.platform;
import mcpp.fetcher.progress;
import mcpp.project;
import mcpp.ui;

namespace mcpp::build {

// ─── P0: build cache for fast-path rebuilds ─────────────────────────

constexpr std::string_view kBuildCacheFile = "target/.build_cache";
constexpr int kBuildCacheMaxEntries = 4;   // P3: LRU capacity

// P3: one entry per (target, fingerprint) pair.
struct BuildCacheEntry {
    std::string targetTriple;    // "" for default target
    std::string outputDir;
    std::string ninjaProgram;
    std::string fingerprint;     // outputDir basename
    std::string runtimeEnvKey;   // "-" means intentionally empty; "" means old cache
    std::string runtimeEnvValue;
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
        if (haveNextLine && !line.starts_with("[target=")) {
            e.runtimeEnvKey = line;
            std::getline(f, e.runtimeEnvValue);
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
                       const std::string& runtimeEnvValue = "") {
    auto path = projectRoot / kBuildCacheFile;
    auto entries = read_build_cache(projectRoot);

    // Remove existing entry for this target (will be re-added at front).
    std::erase_if(entries, [&](const BuildCacheEntry& e) {
        return e.targetTriple == targetTriple;
    });

    // Insert at front (MRU).
    BuildCacheEntry newEntry{targetTriple, outputDir.string(), ninjaProgram, fingerprintHex,
                             runtimeEnvKey, runtimeEnvValue};
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
        if (key != "cxx" && key != "cc" && key != "ar" && key != "scan_deps")
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
        || output.find("manifest 'build.ninja' still dirty") != std::string_view::npos;
}

// Compile a prepared BuildContext. Shared between `mcpp build` and `mcpp run`
// so the latter doesn't call prepare_build twice (and re-print the toolchain
// resolution banner).
export int run_build_plan(BuildContext& ctx, bool verbose, bool no_cache,
                   std::string_view targetOverride = "") {
    if (no_cache) {
        std::error_code ec;
        std::filesystem::remove_all(ctx.outputDir, ec);
    }

    auto be = mcpp::build::make_ninja_backend();

    // M5.0: print "Inferred" banner when defaults / target inference fired.
    for (auto& note : ctx.manifest.inferredNotes) {
        mcpp::ui::status("Inferred", note);
    }

    // Announce the package being built (and any deps).
    // Deps that hit the BMI cache get "Cached" instead of "Compiling".
    std::set<std::string> cachedNames;
    for (auto& label : ctx.cachedDepLabels) {
        auto sp = label.find(' ');
        cachedNames.insert(sp == std::string::npos ? label : label.substr(0, sp));
    }
    std::set<std::string> announced;
    announced.insert(ctx.manifest.package.name);
    mcpp::ui::status("Compiling",
        std::format("{} v{} (.)",
                    ctx.manifest.package.name, ctx.manifest.package.version));
    for (auto& [name, spec] : ctx.manifest.dependencies) {
        if (announced.contains(name)) continue;
        announced.insert(name);
        std::string ver = spec.isPath() ? "(path)" : std::string("v") + spec.version;
        const char* verb = cachedNames.contains(name) ? "Cached" : "Compiling";
        mcpp::ui::status(verb, std::format("{} {}", name, ver));
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

    // M3.2: populate BMI cache for deps that did NOT hit cache.
    for (auto& task : ctx.depsToPopulate) {
        auto pr = mcpp::bmi_cache::populate_from(task.key, ctx.outputDir, task.artifacts);
        if (!pr) {
            mcpp::ui::warning(std::format(
                "bmi cache populate failed for {}@{}: {}",
                task.key.packageName, task.key.version, pr.error()));
        }
    }

    // P1.5: warn if fingerprint changed from last build (explains full rebuild).
    {
        auto entries = read_build_cache(ctx.projectRoot);
        for (auto& e : entries) {
            if (e.targetTriple == targetOverride && !e.fingerprint.empty()) {
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
    if (!no_cache && !r->ninjaProgram.empty()) {
        auto fpHex = ctx.outputDir.filename().string();
        write_build_cache(ctx.projectRoot, ctx.outputDir, r->ninjaProgram,
                          std::string(targetOverride), fpHex,
                          r->runtimeEnvKey.empty() ? "-" : r->runtimeEnvKey,
                          r->runtimeEnvValue);
    }

    mcpp::ui::finished("release", r->elapsed);
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

// Try to fast-path: if build.ninja is newer than all inputs, just run ninja.
// Returns exit code on fast-path, or nullopt if full rebuild needed.
export std::optional<int> try_fast_build(const std::filesystem::path& projectRoot,
                                  bool verbose, bool no_cache,
                                  std::string_view currentTarget = "") {
    if (no_cache) return std::nullopt;

    // P3: read multi-entry cache and find entry matching currentTarget.
    auto entries = read_build_cache(projectRoot);
    const BuildCacheEntry* match = nullptr;
    for (auto& e : entries) {
        if (e.targetTriple == currentTarget) { match = &e; break; }
    }
    if (!match) return std::nullopt;

    auto outputDirStr = match->outputDir;
    auto ninjaProgram = match->ninjaProgram;
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

    // Check all source files under src/
    auto srcDir = projectRoot / "src";
    if (std::filesystem::exists(srcDir, ec)) {
        for (auto& entry : std::filesystem::recursive_directory_iterator(srcDir, ec)) {
            if (!entry.is_regular_file()) continue;
            auto ext = entry.path().extension().string();
            if (ext != ".cppm" && ext != ".cpp" && ext != ".cc" &&
                ext != ".cxx" && ext != ".c" && ext != ".h" && ext != ".hpp")
                continue;
            auto ft = std::filesystem::last_write_time(entry.path(), ec);
            if (ec || ft > ninjaTime) return std::nullopt;
        }
    }

    // All inputs are older than build.ninja → fast-path: just run ninja.
    std::string cmd = ninjaProgram;
    if (!verbose) cmd += " --quiet";
    cmd += std::format(" -C {}", mcpp::platform::shell::quote(outputDir.string()));
    if (verbose) cmd += " -v";
    cmd += " 2>&1";

    auto t0 = std::chrono::steady_clock::now();
    std::string out;
    std::optional<mcpp::platform::env::ScopedEnv> scopedEnv;
    if (runtimeEnvKey != "-" && !runtimeEnvValue.empty())
        scopedEnv.emplace(runtimeEnvKey, runtimeEnvValue);
    auto r = mcpp::platform::process::capture(cmd);
    out = r.output;
    int status = r.exit_code;
    bool ok = (status == 0);
    if (!ok) {
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

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0);
    mcpp::ui::finished("release", elapsed);
    return 0;
}

// `mcpp run` driver: build, locate the binary target, exec it with the
// resolved runtime environment.
export int build_run_target(const std::optional<std::string>& targetName,
                            std::span<const std::string> passthrough) {
    // Build first. Single prepare_build → drive build → reuse ctx to locate
    // the binary, so we don't re-resolve the toolchain or re-scan modgraph.
    auto ctx = prepare_build(/*print_fp=*/false);
    if (!ctx) { std::println(stderr, "error: {}", ctx.error()); return 2; }
    if (auto rc = run_build_plan(*ctx, /*verbose=*/false, /*no_cache=*/false); rc != 0)
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
    std::string cmd = mcpp::platform::shell::quote(exe.string());
    for (auto& a : passthrough) cmd += " " + mcpp::platform::shell::quote(a);

    std::optional<mcpp::platform::env::ScopedEnv> runtimeEnv;
    auto runtimeEnvKey = mcpp::platform::env::runtime_library_path_key();
    auto runtimeEnvValue = mcpp::platform::env::prepend_path_list(
        runtimeEnvKey, ctx->plan.runtimeLibraryDirs);
    if (!runtimeEnvKey.empty() && !runtimeEnvValue.empty()) {
        runtimeEnv.emplace(runtimeEnvKey, runtimeEnvValue);
    }

    int rc = std::system(cmd.c_str());
    return mcpp::platform::process::extract_exit_code(rc) == 0 ? 0 : 1;
}

// `mcpp test` driver: discover tests/**/*.cpp, synthesize targets, build
// with dev-deps, run each test binary, summarize.
export int run_tests(std::span<const std::string> passthrough,
                     BuildOverrides overrides = {}) {
    auto root = mcpp::project::find_manifest_root(std::filesystem::current_path());
    if (!root) {
        mcpp::ui::error("no mcpp.toml found in current directory or any parent");
        return 2;
    }

    // 1. Discover test files.
    auto testFiles = mcpp::modgraph::expand_glob(*root, "tests/**/*.cpp");
    if (testFiles.empty()) {
        std::println("no tests found in tests/");
        return 0;
    }

    // 2. Synthesize a Target for each test file.
    //    Name = file stem; collisions → error.
    std::vector<mcpp::manifest::Target> testTargets;
    std::set<std::string> seenNames;
    for (auto& f : testFiles) {
        auto name = f.stem().string();
        if (!seenNames.insert(name).second) {
            mcpp::ui::error(std::format(
                "duplicate test name '{}' (two .cpp files share the same stem)", name));
            return 2;
        }
        mcpp::manifest::Target t;
        t.name = name;
        t.kind = mcpp::manifest::Target::TestBinary;
        // Store as path relative to project root for portability of error messages.
        t.main = std::filesystem::relative(f, *root).string();
        testTargets.push_back(std::move(t));
    }

    // 3. prepare_build with dev-deps enabled + synthetic targets.
    auto ctx = prepare_build(/*print_fp=*/false,
                             /*includeDevDeps=*/true,
                             std::move(testTargets),
                             std::move(overrides));
    if (!ctx) { mcpp::ui::error(ctx.error()); return 2; }

    // 4. "Compiling test_X (test)" lines for the test binaries.
    std::set<std::string> cachedNames;
    for (auto& label : ctx->cachedDepLabels) {
        auto sp = label.find(' ');
        cachedNames.insert(sp == std::string::npos ? label : label.substr(0, sp));
    }
    std::set<std::string> announced;
    announced.insert(ctx->manifest.package.name);
    mcpp::ui::status("Compiling",
        std::format("{} v{} (.)",
                    ctx->manifest.package.name, ctx->manifest.package.version));
    for (auto& [name, spec] : ctx->manifest.dependencies) {
        if (announced.contains(name)) continue;
        announced.insert(name);
        std::string ver = spec.isPath() ? "(path)" : std::string("v") + spec.version;
        const char* verb = cachedNames.contains(name) ? "Cached" : "Compiling";
        mcpp::ui::status(verb, std::format("{} {}", name, ver));
    }
    for (auto& [name, spec] : ctx->manifest.devDependencies) {
        if (announced.contains(name)) continue;
        announced.insert(name);
        std::string ver = spec.isPath() ? "(path)" : std::string("v") + spec.version;
        const char* verb = cachedNames.contains(name) ? "Cached" : "Compiling";
        mcpp::ui::status(verb,
            std::format("{} {} (dev)", name, ver));
    }
    // List test binaries.
    for (auto& lu : ctx->plan.linkUnits) {
        if (lu.kind == mcpp::build::LinkUnit::TestBinary) {
            mcpp::ui::status("Compiling",
                std::format("{} (test)", lu.targetName));
        }
    }

    // 5. Build everything.
    auto backend = mcpp::build::make_ninja_backend();
    mcpp::build::BuildOptions opts;
    auto buildResult = backend->build(ctx->plan, opts);
    if (!buildResult) {
        mcpp::ui::error(buildResult.error().message);
        return 1;
    }

    // M3.2: populate BMI cache for deps that did NOT hit cache.
    for (auto& task : ctx->depsToPopulate) {
        auto pr = mcpp::bmi_cache::populate_from(task.key, ctx->outputDir, task.artifacts);
        if (!pr) {
            mcpp::ui::warning(std::format(
                "bmi cache populate failed for {}@{}: {}",
                task.key.packageName, task.key.version, pr.error()));
        }
    }

    mcpp::ui::finished("test", buildResult->elapsed);

    // 6. Run each test binary in sequence; collect pass/fail.
    auto t0 = std::chrono::steady_clock::now();
    int passed = 0;
    int failed = 0;
    std::vector<std::string> failures;

    std::optional<mcpp::platform::env::ScopedEnv> runtimeEnv;
    auto runtimeEnvKey = mcpp::platform::env::runtime_library_path_key();
    auto runtimeEnvValue = mcpp::platform::env::prepend_path_list(
        runtimeEnvKey, ctx->plan.runtimeLibraryDirs);
    if (!runtimeEnvKey.empty() && !runtimeEnvValue.empty()) {
        runtimeEnv.emplace(runtimeEnvKey, runtimeEnvValue);
    }

    for (auto& lu : ctx->plan.linkUnits) {
        if (lu.kind != mcpp::build::LinkUnit::TestBinary) continue;
        auto exe = ctx->outputDir / lu.output;
        mcpp::ui::status("Running", std::format("bin/{}", lu.targetName));

        // Prepend the sandbox's subos/default/bin to PATH so tools
        // bootstrapped during sandbox init (patchelf, ninja, etc.) are
        // visible to test binaries that shell out to them. The
        // toolchain binary's path encodes the registry root — derive it.
        std::string pathPrefix;
        if constexpr (!mcpp::platform::is_windows) {
            if (auto xpkgs = mcpp::xlings::paths::xpkgs_from_compiler(ctx->tc.binaryPath)) {
                // xpkgs is <registry>/data/xpkgs → registry = xpkgs/../..
                auto registryDir = xpkgs->parent_path().parent_path();
                auto sandboxBin  = registryDir / "subos" / "default" / "bin";
                if (std::filesystem::exists(sandboxBin))
                    pathPrefix = std::format("PATH={}:\"$PATH\" ",
                                             mcpp::platform::shell::quote(sandboxBin.string()));
            }
        }

        std::string cmd = pathPrefix + mcpp::platform::shell::quote(exe.string());
        for (auto& a : passthrough) cmd += " " + mcpp::platform::shell::quote(a);
        int exitCode = mcpp::platform::process::extract_exit_code(std::system(cmd.c_str()));

        if (exitCode == 0) {
            std::println("{} ... ok", lu.targetName);
            ++passed;
        } else {
            std::println("{} ... FAIL (exit {})", lu.targetName, exitCode);
            ++failed;
            failures.push_back(lu.targetName);
        }
    }
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0);

    // 7. Summary.
    std::println("");
    if (failed == 0) {
        mcpp::ui::status("test result",
            std::format("ok. {} passed; 0 failed; finished in {:.2f}s",
                        passed, static_cast<double>(elapsed.count()) / 1000.0));
        return 0;
    }
    mcpp::ui::error(std::format(
        "test result: FAILED. {} passed; {} failed; finished in {:.2f}s",
        passed, failed, static_cast<double>(elapsed.count()) / 1000.0));
    std::println("");
    std::println("failures:");
    for (auto& n : failures) std::println("    {}", n);
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
        auto bmi = mcpp::toolchain::default_cache_root();
        std::filesystem::remove_all(bmi, ec);
        std::println("Cleaned BMI cache: {}", bmi.string());
    }
    return 0;
}

} // namespace mcpp::build
