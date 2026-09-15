// mcpp.cli.cmd_build — CLI parsing + routing for build / run / test /
// clean / dyndep / stage. Implementations live in mcpp.build.prepare,
// mcpp.build.execute, mcpp.dyndep and mcpp.build.stage.

module;
#include <cstdio>
#include <cstdlib>

export module mcpp.cli.cmd_build;

import std;
import mcpplibs.cmdline;
import mcpp.build.prepare;
import mcpp.build.execute;
import mcpp.bmi_cache.maintenance;   // parse_duration, for `clean --stale --older-than`
import mcpp.build.directives;      // the device-slot table
import mcpp.build.configure;
import mcpp.build.coff_exports;
import mcpp.build.stage;
import mcpp.build.schedule.detach_codegen;
import mcpp.build.test_targets;
import mcpp.build.build_database;
import mcpp.build.build_program;
import mcpp.build.refusal;          // offline-download-required (#648 A1)
import mcpp.dyndep;
import mcpp.home;
import mcpp.hooks;
import mcpp.libs.json;
import mcpp.log;
import mcpp.platform;
import mcpp.platform.terminal;
import mcpp.project;
import mcpp.manifest;
import mcpp.toolchain.fingerprint;
import mcpp.ui;
import mcpp.wire;

namespace mcpp::cli {

// Decide whether a build/test invocation fans out over workspace members, and
// if so which. Fan out when `--workspace` is given, or at a *virtual* workspace
// root with no `-p` (the intuitive "act on the whole workspace"). Returns the
// member paths to iterate, or nullopt for the single-package / single-`-p` /
// rooted-bare path (handled by the existing per-package pipeline).
std::optional<std::vector<std::string>>
workspace_fanout_members(bool wantAll, const std::string& package_filter) {
    auto root = mcpp::project::find_manifest_root(std::filesystem::current_path());
    if (!root) return std::nullopt;
    auto m = mcpp::manifest::load(*root / "mcpp.toml");
    if (!m || !m->workspace.present || m->workspace.members.empty()) return std::nullopt;
    bool virtualWs = m->package.name.empty();
    if (wantAll || (virtualWs && package_filter.empty()))
        return m->workspace.members;
    return std::nullopt;
}

// run_build_plan, wrapped in the project's `[hooks]` lifecycle (#496).
//
// The hooks come off the context's own manifest, so they are the ones belonging
// to the package being built — which in a workspace fan-out is the MEMBER, once
// per member. The lifecycle is deliberately paired: build_finished/build_failed
// are only ever reached after build_start has run, so a project that could not
// be prepared at all (bad manifest, unresolvable dependency, no toolchain)
// fires nothing — its hook programs may be exactly what preparation failed to
// install.
int run_build_with_hooks(mcpp::build::BuildContext& ctx, bool verbose,
                         bool no_cache, std::string_view targetOverride) {
    auto const& hooks = ctx.manifest.hooks;

    // `during_build` opens first and closes last: its interval is the one that
    // spans everything below. Its output is discarded unless --verbose, which
    // is the only way it could interleave into a compiler diagnostic.
    mcpp::hooks::Span span(hooks, ctx.projectRoot, /*inheritOutput=*/verbose);
    if (!span.ok()) return 1;

    if (!mcpp::hooks::invoke(hooks, mcpp::hooks::Event::BuildStart,
                             ctx.projectRoot))
        return 1;

    int rc = mcpp::build::run_build_plan(ctx, verbose, no_cache, targetOverride);

    // Closed BEFORE the terminal hook. A "build finished" sound competing with
    // the background music it replaces is the ordering this line settles.
    bool spanOk = span.finish();

    auto terminalEvent = rc == 0 ? mcpp::hooks::Event::BuildFinished
                                 : mcpp::hooks::Event::BuildFailed;
    // The build's own exit code outranks the hook's: `mcpp build` returning
    // "the notifier failed" for a compile error would answer a question nobody
    // asked. A hook failure only decides the exit code of a build that worked.
    bool hookOk = mcpp::hooks::invoke(hooks, terminalEvent, ctx.projectRoot);
    return rc != 0 ? rc : ((spanOk && hookOk) ? 0 : 1);
}

// The build selectors, read once for every command that plans a build: `mcpp
// build` and `mcpp emit build-database` select the same plan from the same
// flags, so the database describes the build the same flags would run.
// The profile the selectors name; see `mcpp::build::profile_override_from_flags`.
std::string profile_from_selectors(const mcpplibs::cmdline::ParsedArgs& parsed) {
    return mcpp::build::profile_override_from_flags(
        parsed.value("profile").value_or(""),
        parsed.is_flag_set("release"), parsed.is_flag_set("dev"));
}

mcpp::build::BuildOverrides overrides_from_selectors(
        const mcpplibs::cmdline::ParsedArgs& parsed) {
    mcpp::build::BuildOverrides ov;
    if (auto t = parsed.value("target")) ov.target_triple = *t;
    // --accel / --no-accel stand to `[build] accel` exactly as --target stands
    // to [toolchain]: the manifest declares, the command line overrides for one
    // build. --no-accel is not the absence of --accel; it is an explicit
    // request for none, which is what a user needs in order to take a CPU-only
    // variant of a package that also publishes device builds.
    if (parsed.is_flag_set("no-accel"))       ov.accel = "(none)";
    else if (auto a = parsed.value("accel"))  ov.accel = *a;
    if (auto p = parsed.value("package")) ov.package_filter = *p;
    // Profile selection precedence: --profile NAME > --release / --dev > the
    // project default ([build].default-profile) > "release", resolved in
    // prepare_build. --release/--dev are shorthands only.
    ov.profile = profile_from_selectors(parsed);
    if (auto fs = parsed.value("features")) ov.features = *fs;
    if (auto cp = parsed.value("cap")) ov.capabilities = *cp;
    ov.strict = parsed.is_flag_set("strict");
    ov.force_static = parsed.is_flag_set("static");
    return ov;
}

export int cmd_build(const mcpplibs::cmdline::ParsedArgs& parsed) {
    bool verbose  = parsed.is_flag_set("verbose") || mcpp::log::is_verbose();
    bool print_fp = parsed.is_flag_set("print-fingerprint");
    bool no_cache = parsed.is_flag_set("no-cache");
    bool configure_only = parsed.is_flag_set("configure-only");

    mcpp::build::BuildOverrides ov = overrides_from_selectors(parsed);
    // --cache global|local|off. --no-cache is the deprecated alias for off; the
    // old flag only ever cleared target/, which says nothing about a cache, so
    // it is expressed in terms of the new one rather than kept as a second axis.
    if (auto c = parsed.value("cache")) ov.cache_mode = *c;
    else if (no_cache)                  ov.cache_mode = "off";

    // Fan-out prefixes every diagnostic with the member it came from; the
    // single-package path has nothing to disambiguate and passes "".
    auto configure_member = [&](mcpp::build::BuildOverrides memberOv,
                                std::string_view label) -> int {
        auto where = [&](std::string_view msg) {
            if (label.empty()) std::println(stderr, "error: {}", msg);
            else               std::println(stderr, "error: {}: {}", label, msg);
        };
        auto root = mcpp::project::find_manifest_root(std::filesystem::current_path());
        if (!root) {
            where("no mcpp.toml found in current directory or any parent");
            return 2;
        }
        auto discovered = mcpp::build::discover_test_targets(*root,
                                                              memberOv.package_filter);
        if (!discovered) {
            where(discovered.error());
            return 2;
        }
        // configure-only deliberately includes dev-dependencies and test TUs:
        // clangd needs the same include/module surface as `mcpp test`, even
        // though Ninja is run in dry-run mode and compiles no object.
        // 必须在移动 targets 前读取状态；函数实参的求值顺序未指定。
        const bool includeDevDeps = !discovered->targets.empty();
        auto ctx = mcpp::build::prepare_build(
            print_fp, includeDevDeps,
            std::move(discovered->targets), std::move(memberOv));
        if (!ctx) {
            where(ctx.error());
            return 2;
        }
        return mcpp::build::run_configure_plan(*ctx, verbose);
    };

    // Workspace fan-out: build every member, one per the existing per-package
    // pipeline (continue-on-failure; first non-zero exit wins). Checked before
    // the fast path, which is single-package only.
    if (auto members = workspace_fanout_members(parsed.is_flag_set("workspace"),
                                                ov.package_filter)) {
        int rc = 0;
        for (auto& mp : *members) {
            mcpp::build::BuildOverrides mo = ov;
            mo.package_filter = mp;
            if (configure_only) {
                int r = configure_member(std::move(mo), mp);
                if (r != 0) rc = r;
                continue;
            }
            auto ctx = mcpp::build::prepare_build(print_fp, /*includeDevDeps=*/false,
                                                  /*extraTargets=*/{}, mo);
            if (!ctx) { std::println(stderr, "error: {}: {}", mp, ctx.error()); rc = 2; continue; }
            int r = run_build_with_hooks(*ctx, verbose, no_cache, mo.target_triple);
            if (r != 0) rc = r;
        }
        return rc;
    }

    if (configure_only)
        return configure_member(std::move(ov), "");

    // P0: try fast-path if inputs haven't changed. Any resolution-affecting
    // override (--profile/--features/--strict, like --target/--static) must
    // bypass it: the cached build.ninja was generated without them, so taking
    // the fast path would silently ignore the flags.
    //
    // `--accel` / `--no-accel` are on that list. Measured before they were: a
    // successful `mcpp build` followed by `mcpp build --no-accel` reported
    // "Finished in 0.00s" and handed back the device build -- the axis that
    // decides which sources compile and which cfg sections apply, ignored
    // because the flag arrived after a build that did not carry it.
    if (!print_fp && ov.target_triple.empty() && !ov.force_static
        && ov.profile.empty() && ov.features.empty() && !ov.strict
        && ov.capabilities.empty() && ov.package_filter.empty()
        && ov.cache_mode.empty() && ov.accel.empty()) {
        auto root = mcpp::project::find_manifest_root(std::filesystem::current_path());
        if (root) {
            // A project with active `[hooks]` declines the fast path from
            // inside try_fast_build, where the manifest that says so is
            // already loaded.
            if (auto rc = mcpp::build::try_fast_build(*root, verbose, no_cache)) {
                return *rc;
            }
        }
    }

    auto ctx = mcpp::build::prepare_build(print_fp, /*includeDevDeps=*/false,
                                          /*extraTargets=*/{}, ov);
    if (!ctx) { std::println(stderr, "error: {}", ctx.error()); return 2; }

    return run_build_with_hooks(*ctx, verbose, no_cache, ov.target_triple);
}

// ─── `mcpp emit build-database` ─────────────────────────────────────────────
//
// The plan `mcpp build --configure-only` computes, printed as an S1 build
// database instead of being configured: nothing is written into the project
// (docs/specs/build-database.md, SPEC-005). Planning writes under a work
// directory in the mcpp home, and the std module is described, not compiled.
namespace {

// One work directory per project root and member, under the mcpp home: the
// planning pass writes its lock, its build programs' artifacts and its output
// directory there, never inside the project.
std::filesystem::path build_database_work_dir(const std::filesystem::path& root,
                                              std::string_view member) {
    return mcpp::home::root() / "cache" / "build-database"
         / mcpp::toolchain::hash_string(root.lexically_normal().generic_string()
                                        + "\x1f" + std::string(member));
}

std::optional<std::string> read_whole_file(const std::filesystem::path& p) {
    std::ifstream is(p, std::ios::binary);
    if (!is) return std::nullopt;
    return std::string((std::istreambuf_iterator<char>(is)),
                       std::istreambuf_iterator<char>());
}

} // namespace

export int cmd_emit_build_database(const mcpplibs::cmdline::ParsedArgs& parsed) {
    using mcpp::wire::Diagnostic;
    using mcpp::wire::Effect;
    using mcpp::wire::Severity;

    const auto formatValue = parsed.value("format");
    if (formatValue && !mcpp::wire::parse_format(*formatValue)) {
        std::println(stderr, "error: {}", mcpp::wire::unsupported_format(*formatValue));
        return 2;
    }
    const bool envelope = formatValue.has_value();
    // Which specification the document follows. The plan is one; the document
    // is a rendering of it, named by its specification rather than by any
    // consumer. `s1` is the default.
    const std::string spec = parsed.value("spec").value_or("s1");
    if (spec != "s1" && spec != "compile-commands") {
        std::println(stderr, "error: unsupported --spec '{}'; expected: s1, compile-commands",
                     spec);
        return 2;
    }
    const auto outputPath = parsed.value("output");
    const mcpp::build::BuildOverrides ov = overrides_from_selectors(parsed);

    std::vector<Diagnostic> diagnostics;
    auto publish = [&](const std::string& text) -> int {
        if (!outputPath) { std::print("{}", text); return 0; }
        const std::filesystem::path out{*outputPath};
        auto tmp = out;
        tmp += std::format(".tmp-{}", std::chrono::steady_clock::now()
                                          .time_since_epoch().count());
        {
            std::ofstream os(tmp, std::ios::binary);
            os << text;
            if (!os) {
                std::println(stderr, "error: cannot write '{}'", tmp.string());
                return 1;
            }
        }
        std::error_code ec;
        if (!mcpp::platform::fs::replace_file(tmp, out, ec)) {
            std::filesystem::remove(tmp, ec);
            std::println(stderr, "error: cannot replace '{}'", out.string());
            return 1;
        }
        return 0;
    };
    // A failure is one envelope with diagnostics and no `data` (S2 0.2.0 §3.4:
    // a command without data has failed), and exit 1.
    auto failed = [&](std::string code, std::string message) -> int {
        diagnostics.push_back({std::move(code), Severity::Error, std::move(message)});
        if (!envelope) {
            for (auto const& d : diagnostics)
                std::println(stderr, "{}: {}",
                             mcpp::wire::severity_name(d.severity), d.message);
            return 1;
        }
        const auto text = mcpp::wire::to_json(mcpp::wire::Envelope{
            .kind = "mcpp.build-database",
            .effects = {Effect::ReadProject},
            .data = nullptr,
            .diagnostics = diagnostics,
        }).dump(2) + "\n";
        (void)publish(text);
        return 1;
    };

    auto root = mcpp::project::find_manifest_root(std::filesystem::current_path());
    if (!root)
        return failed("MCPP_BUILD_DATABASE_NO_PROJECT",
                      "no mcpp.toml found in current directory or any parent");

    std::vector<std::pair<std::string, mcpp::build::BuildOverrides>> requests;
    if (auto members = workspace_fanout_members(parsed.is_flag_set("workspace"),
                                                ov.package_filter)) {
        for (auto const& mp : *members) {
            auto mo = ov;
            mo.package_filter = mp;
            requests.emplace_back(mp, std::move(mo));
        }
    } else {
        requests.emplace_back(std::string{}, ov);
    }

    std::vector<mcpp::build::BuildContext> contexts;
    std::vector<std::filesystem::path>     workDirs;
    std::vector<std::string>               prefixes;
    std::vector<std::pair<std::filesystem::path, std::vector<std::string>>> testDiscovery;
    std::optional<std::string>             planError;
    {
        // Planning narrates on stdout and may start programs that inherit it;
        // the document is printed after this scope, alone.
        mcpp::platform::terminal::StdoutToStderr narration;
        for (auto& [member, mo] : requests) {
            auto discovered = mcpp::build::discover_test_targets(*root, mo.package_filter);
            if (!discovered) {
                planError = member.empty() ? discovered.error()
                                           : std::format("{}: {}", member, discovered.error());
                break;
            }
            // As `--configure-only`: tests and dev-dependencies are part of the
            // surface an editor needs.
            const bool includeDevDeps = !discovered->targets.empty();
            auto discovery = std::pair{discovered->packageRoot, discovered->discover};
            mo.plan_only = true;
            mo.work_dir  = build_database_work_dir(*root, mo.package_filter);
            std::error_code ec;
            std::filesystem::remove(mo.work_dir / "mcpp.lock", ec);
            auto ctx = mcpp::build::prepare_build(/*print_fingerprint=*/false,
                                                  includeDevDeps,
                                                  std::move(discovered->targets), mo);
            if (!ctx) {
                planError = member.empty() ? ctx.error()
                                           : std::format("{}: {}", member, ctx.error());
                break;
            }
            contexts.push_back(std::move(*ctx));
            workDirs.push_back(mo.work_dir);
            prefixes.push_back(member.empty() ? std::string{} : member + "/");
            testDiscovery.push_back(std::move(discovery));
        }
    }
    // An offline plan that needs a download is not a defect of the project, and
    // a client that plans offline by default (an editor) has to tell the two
    // apart without reading the message (#648 A1). The code is taken only while
    // the run is offline, so a refusal recorded on a path that recovered cannot
    // relabel an unrelated failure.
    if (planError) {
        const bool offline = mcpp::platform::env::offline_mode()
                          || mcpp::platform::env::no_auto_install();
        if (mcpp::build::refusal::take()
                == mcpp::build::refusal::Code::OfflineDownloadRequired
            && offline)
            return failed("MCPP_OFFLINE_DOWNLOAD_REQUIRED", *planError);
        return failed("MCPP_BUILD_DATABASE_PLAN_FAILED", *planError);
    }

    // The lock this planning produced, against the project's. The project's is
    // never written; a difference is reported.
    for (std::size_t i = 0; i < contexts.size(); ++i) {
        const auto now = read_whole_file(workDirs[i] / "mcpp.lock");
        if (!now) continue;
        const auto projectLock = contexts[i].projectRoot / "mcpp.lock";
        const auto was = read_whole_file(projectLock);
        if (!was)
            diagnostics.push_back({"MCPP_LOCK_WOULD_CHANGE", Severity::Warning,
                std::format("'{}' does not exist; `mcpp build` would create it",
                            projectLock.string())});
        else if (*was != *now)
            diagnostics.push_back({"MCPP_LOCK_WOULD_CHANGE", Severity::Warning,
                std::format("this resolution differs from '{}'; `mcpp build` "
                            "would update it", projectLock.string())});
    }

    std::vector<mcpp::build::database::Member> members;
    bool ranBuildPrograms = false;
    for (std::size_t i = 0; i < contexts.size(); ++i) {
        members.push_back({&contexts[i], prefixes[i], workDirs[i],
                           testDiscovery[i].first, testDiscovery[i].second});
        if (!mcpp::build::declared_program_inputs(workDirs[i]).empty())
            ranBuildPrograms = true;
        for (auto const& sp : contexts[i].sourcePackages) {
            std::error_code ec;
            if (std::filesystem::exists(sp.root / "build.mcpp", ec)) ranBuildPrograms = true;
        }
    }
    // One line per selector: a value never spans lines, and a `\x1f` separator
    // before `f`, `c` or `a` reads as a longer hex escape (clang refuses it).
    const auto selector = std::format(
        "spec={}\ntarget={}\ntoolchain={}\nprofile={}\nfeatures={}\n"
        "cap={}\naccel={}\nstatic={}\npackage={}\nworkspace={}",
        spec, ov.target_triple, mcpp::platform::env::get("MCPP_TOOLCHAIN").value_or(""),
        ov.profile, ov.features, ov.capabilities, ov.accel, ov.force_static,
        ov.package_filter, parsed.is_flag_set("workspace"));
    auto rendered = mcpp::build::database::render(members, *root, selector);
    for (auto& note : rendered.notes)
        diagnostics.push_back({std::move(note.code), Severity::Warning,
                               std::move(note.message)});

    auto document = spec == "s1" ? std::move(rendered.database)
                                 : std::move(rendered.compileCommands);
    if (!envelope) {
        for (auto const& d : diagnostics)
            std::println(stderr, "{}: {}", mcpp::wire::severity_name(d.severity),
                         d.message);
        return publish(document.dump(2) + "\n");
    }
    std::vector<Effect> effects{Effect::ReadProject, Effect::WriteGlobalCache};
    if (ranBuildPrograms) effects.push_back(Effect::ExecBuildScript);
    nlohmann::json specJson{{"name", spec}};
    if (spec == "s1")
        specJson["version"] = std::string(mcpp::build::database::kProfileVersion);
    return publish(mcpp::wire::to_json(mcpp::wire::Envelope{
        .kind = "mcpp.build-database",
        .effects = std::move(effects),
        .data = nlohmann::json{
            {"spec",               std::move(specJson)},
            {"database",           std::move(document)},
            {"watch",              std::move(rendered.watch)},
            {"inputs-fingerprint", std::move(rendered.inputsFingerprint)},
        },
        .diagnostics = diagnostics,
    }).dump(2) + "\n");
}

export int cmd_run(const mcpplibs::cmdline::ParsedArgs& parsed,
            std::span<const std::string> passthrough) {
    // The action lambda has already split argv at the first "--" and
    // passed post-args as `passthrough`; the only positional we declare
    // is the optional binary target name.
    std::optional<std::string> targetName;
    if (parsed.positional_count() > 0) targetName = parsed.positional(0);
    // -p/--package <member>: scope to one workspace member, same flag/rule
    // as `mcpp build -p` / `mcpp test -p` (mcpp::project::resolve_member_dir).
    // `mcpp run` is single-member only — no `--workspace` fan-out.
    std::string package_filter;
    if (auto p = parsed.value("package")) package_filter = *p;
    std::string cache_mode;
    bool no_cache = parsed.is_flag_set("no-cache");
    if (auto c = parsed.value("cache")) cache_mode = *c;
    else if (no_cache)                  cache_mode = "off";
    // Both spellings; see cli.cppm for why the positional had to be renamed
    // before `--target` could exist here at all.
    std::string target_triple;
    if (auto tt = parsed.value("target"))        target_triple = *tt;
    if (auto tt = parsed.value("target-triple")) target_triple = *tt;
    // --no-runner: "this host can execute the artifact" is a fact about the
    // host, and the manifest has no host axis to state it on (#544, D3).
    const bool no_runner = parsed.is_flag_set("no-runner");
    // The named way to reach the artefact. Empty = the default runner, which is
    // what `mcpp run` has always meant.
    std::string runner_name;
    if (auto rn = parsed.value("runner")) runner_name = *rn;
    // The same two axes `build` and `test` take, read the same way. `--release`
    // and `--dev` are the shorthands the other verbs already accept.
    std::string features;
    if (auto fs = parsed.value("features")) features = *fs;
    const std::string profile = profile_from_selectors(parsed);
    // The device axis, read exactly as `build` reads it: `--no-accel` is an
    // explicit choice and not the absence of `--accel`, so it travels as the
    // same sentinel. Without this a project's CPU-only variant could be built
    // but not run through the command surface.
    std::string accel;
    if (parsed.is_flag_set("no-accel"))      accel = "(none)";
    else if (auto a = parsed.value("accel")) accel = *a;
    // #622 A10: the value space and the refusal text both come from `mcpp
    // pack --format`; `run` only adds the pairing with `--no-runner`, checked
    // in build_run_target where the runner is actually chosen.
    std::string format;
    if (auto f = parsed.value("format")) format = *f;
    if (parsed.is_flag_set("list-runners"))
        return mcpp::build::list_runners(package_filter, cache_mode, no_cache,
                                         target_triple, features, profile, accel);
    return mcpp::build::build_run_target(targetName, passthrough, package_filter,
                                         cache_mode, no_cache, target_triple,
                                         no_runner, runner_name, features,
                                         profile, accel, format);
}

export int cmd_test(const mcpplibs::cmdline::ParsedArgs& parsed,
             std::span<const std::string> passthrough) {
    // Pre-`--` flags select the build mode for the test build (so e.g.
    // `mcpp test --profile contracts` compiles the code-under-test plus the
    // test binaries under that profile — a whole-build mode, the right
    // granularity for sanitizers / contract evaluation semantics). Post-`--`
    // args go to each test binary.
    mcpp::build::BuildOverrides ov;
    ov.profile = profile_from_selectors(parsed);
    if (auto fs = parsed.value("features")) ov.features = *fs;
    if (auto cp = parsed.value("cap")) ov.capabilities = *cp;
    ov.strict = parsed.is_flag_set("strict");
    if (auto p = parsed.value("package")) ov.package_filter = *p;
    if (auto c = parsed.value("cache")) ov.cache_mode = *c;
    else if (parsed.is_flag_set("no-cache")) ov.cache_mode = "off";

    if (auto tt = parsed.value("target")) ov.target_triple = *tt;
    if (parsed.is_flag_set("no-accel"))       ov.accel = "(none)";
    else if (auto a = parsed.value("accel"))  ov.accel = *a;

    mcpp::build::TestOptions to;
    if (parsed.positional_count() > 0) to.filter = parsed.positional(0);
    to.list = parsed.is_flag_set("list");
    to.noRunner = parsed.is_flag_set("no-runner");   // see cmd_run
    // The three deadlines share one parser: they differ only in what they
    // bound, not in how they are spelled. 0 always means "no limit" — for
    // --timeout that now has to be asked for rather than being the default.
    int workspaceTimeoutSecs = 0;
    auto read_secs = [&parsed](std::string_view flag, int& out) -> bool {
        auto ts = parsed.value(std::string{flag});
        if (!ts) return true;
        int secs = 0;
        auto [p, ec] = std::from_chars(ts->data(), ts->data() + ts->size(), secs);
        if (ec != std::errc{} || p != ts->data() + ts->size() || secs < 0) {
            mcpp::ui::error(std::format("invalid --{} '{}' (whole seconds >= 0)", flag, *ts));
            return false;
        }
        out = secs;
        return true;
    };
    if (!read_secs("timeout", to.timeoutSecs)) return 2;
    if (!read_secs("build-timeout", to.buildTimeoutSecs)) return 2;
    if (!read_secs("workspace-timeout", workspaceTimeoutSecs)) return 2;
    if (auto mf = parsed.value("message-format")) {
        if (*mf == "json")       to.format = mcpp::build::TestMessageFormat::Json;
        else if (*mf != "human") {
            mcpp::ui::error(std::format("unknown --message-format '{}' (human|json)", *mf));
            return 2;
        }
    }

    // Workspace fan-out: test every member through run_tests (which scopes its
    // discovery to the member). Continue-on-failure + per-member summary so one
    // red member never hides the rest.
    if (auto members = workspace_fanout_members(parsed.is_flag_set("workspace"),
                                                ov.package_filter)) {
        const bool json = (to.format == mcpp::build::TestMessageFormat::Json);
        // Silence the ui BEFORE the first member, not inside run_tests. The
        // quiet flag used to be set by run_tests itself, so the fan-out's own
        // "testing member" line escaped for member #1 and was suppressed from
        // #2 on — one stdout stream, two behaviors, and the stray line broke
        // NDJSON for any consumer that parsed it strictly.
        if (json) mcpp::ui::set_quiet(true);

        int rc = 0;
        std::vector<std::string> failed;
        std::vector<std::string> notRun;        // --workspace-timeout reached
        std::vector<std::string> unrunnable;    // tests built, none executed (#544)
        std::vector<std::pair<std::string, long long>> memberTimes;
        int totalPassed = 0, totalFailed = 0, totalNotRun = 0;
        auto tWs = std::chrono::steady_clock::now();
        auto ws_ms = [&tWs] {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - tWs).count();
        };
        const long long wsDeadlineMs =
            static_cast<long long>(workspaceTimeoutSecs) * 1000;

        std::size_t idx = 0;
        for (auto& mp : *members) {
            ++idx;
            // Checked BEFORE starting a member rather than after: stopping
            // mid-member would leave a half-built member reported as neither
            // run nor skipped.
            if (wsDeadlineMs > 0 && ws_ms() >= wsDeadlineMs) {
                notRun.push_back(mp);
                continue;
            }
            mcpp::build::BuildOverrides mo = ov;
            mo.package_filter = mp;
            mcpp::ui::status("Workspace",
                std::format("testing member '{}' ({}/{})", mp, idx, members->size()));
            mcpp::build::TestRunSummary sum;
            int r = mcpp::build::run_tests(passthrough, mo, to, &sum);
            totalPassed += sum.passed;
            totalFailed += sum.failed;
            totalNotRun += sum.notRun;
            memberTimes.emplace_back(mp, sum.elapsedMs);
            auto secs = static_cast<double>(sum.elapsedMs) / 1000.0;
            if (r == 2 && sum.failed == 0 && sum.notRun > 0) {
                // Built and not executed (#544): the member did not fail, and
                // it did not pass. 2 outranks 0 and yields to 1, as it does
                // for a single member.
                if (rc == 0) rc = 2;
                unrunnable.push_back(mp);
                mcpp::ui::status("Workspace",
                    std::format("member '{}' ({}/{}) NOT RUN — {} passed, {} not run in {:.2f}s",
                                mp, idx, members->size(), sum.passed, sum.notRun, secs));
            } else if (r != 0) {
                rc = r;
                failed.push_back(mp);
                mcpp::ui::status("Workspace",
                    std::format("member '{}' ({}/{}) FAILED — {} passed, {} failed in {:.2f}s",
                                mp, idx, members->size(), sum.passed, sum.failed, secs));
            } else {
                mcpp::ui::status("Workspace",
                    std::format("member '{}' ({}/{}) ok — {} passed in {:.2f}s",
                                mp, idx, members->size(), sum.passed, secs));
            }
        }

        auto wsElapsed = ws_ms();
        if (!notRun.empty()) rc = rc ? rc : 1;

        if (json) {
            // Member paths are manifest-authored strings, so escape rather
            // than assume: one backslash in a member path would otherwise emit
            // a stream that is not JSON at all.
            auto join = [](const std::vector<std::string>& v) {
                std::string s;
                for (auto& x : v) {
                    if (!s.empty()) s += ',';
                    s += '"';
                    for (char c : x) {
                        if (c == '"' || c == '\\') s += '\\';
                        s += c;
                    }
                    s += '"';
                }
                return s;
            };
            // `not_run` keeps its meaning (members the --workspace-timeout
            // stopped before they started); `tests_not_run` and
            // `unrunnable_members` are #544's — tests that were built and not
            // executed, and the members all of whose tests were.
            std::println("{{\"workspace_summary\":{{\"members\":{},\"passed\":{},\"failed\":{},"
                         "\"tests_not_run\":{},"
                         "\"failed_members\":[{}],\"unrunnable_members\":[{}],"
                         "\"not_run\":[{}],\"elapsed_ms\":{}}}}}",
                         members->size(), totalPassed, totalFailed, totalNotRun,
                         join(failed), join(unrunnable), join(notRun), wsElapsed);
            std::fflush(stdout);
            return rc;
        }

        // Slowest members, printed unconditionally rather than only on failure:
        // "which member ate the wall clock" is the question a green-but-slow CI
        // run raises, and answering it used to mean reverse-engineering log
        // timestamps.
        std::ranges::sort(memberTimes, [](auto& a, auto& b) { return a.second > b.second; });
        std::string slowest;
        for (std::size_t i = 0; i < memberTimes.size() && i < 3; ++i) {
            if (memberTimes[i].second < 1000) break;
            if (!slowest.empty()) slowest += ", ";
            slowest += std::format("{} {:.1f}s", memberTimes[i].first,
                                   static_cast<double>(memberTimes[i].second) / 1000.0);
        }

        auto join_names = [](const std::vector<std::string>& v) {
            std::string s;
            for (auto& f : v) { if (!s.empty()) s += ", "; s += f; }
            return s;
        };
        // The not-run count is in the line whenever it is non-zero, at the
        // same weight as the failure count (#544): a member whose tests were
        // built and not executed must not read as a passing member.
        std::string notRunCounts = totalNotRun
            ? std::format("; {} not run", totalNotRun) : std::string{};
        if (failed.empty() && notRun.empty() && unrunnable.empty())
            mcpp::ui::status("workspace result",
                std::format("ok. {} member(s); {} passed; 0 failed{}; finished in {:.2f}s",
                            members->size(), totalPassed, notRunCounts,
                            static_cast<double>(wsElapsed) / 1000.0));
        else
            mcpp::ui::error(std::format(
                "workspace test: {}/{} member(s) failed; {} passed; {} failed{}; "
                "finished in {:.2f}s",
                failed.size(), members->size(), totalPassed, totalFailed, notRunCounts,
                static_cast<double>(wsElapsed) / 1000.0));
        if (!failed.empty())
            mcpp::ui::plain(std::format("    failed members: {}", join_names(failed)));
        if (!unrunnable.empty())
            mcpp::ui::plain(std::format("    not run (no runner on this host): {}",
                                        join_names(unrunnable)));
        if (!notRun.empty())
            mcpp::ui::plain(std::format(
                "    not run (--workspace-timeout {}s reached): {}",
                workspaceTimeoutSecs, join_names(notRun)));
        if (!slowest.empty())
            mcpp::ui::plain(std::format("    slowest: {}", slowest));
        return rc;
    }
    return mcpp::build::run_tests(passthrough, ov, to);
}

export int cmd_clean(const mcpplibs::cmdline::ParsedArgs& parsed) {
    // EVERY OPTION OF THIS MODE SELECTS IT. `--older-than` is meaningless to a
    // full wipe, so reading it as one leaves `mcpp clean --older-than 3d` --
    // a request for a *scoped* clean -- deleting every triple and profile
    // under target/, silently, which is the outcome `--stale` exists to avoid.
    const bool dryRun = parsed.is_flag_set("dry-run");
    const auto olderThan = parsed.value("older-than");
    if (parsed.is_flag_set("stale") || dryRun || olderThan) {
        if (parsed.is_flag_set("bmi-cache")) {
            std::println(stderr, "error: --stale/--dry-run/--older-than cannot be combined with "
                                 "--bmi-cache (the build cache is shared across projects; "
                                 "use `mcpp cache gc`)");
            return 2;
        }
        std::int64_t keepWithinSecs = 24 * 3600;
        if (olderThan) {
            // `parse_duration` wants a unit, so bare `0` reaches it as a
            // one-character string and is rejected; it also returns whatever
            // `stoll` read, so `-1s` parses to a negative window that keeps
            // nothing -- a typo for `1s` would silently disable the guard.
            auto secs = (*olderThan == "0") ? std::optional<std::int64_t>{0}
                                            : mcpp::bmi_cache::parse_duration(*olderThan);
            if (!secs || *secs < 0) {
                std::println(stderr, "error: invalid --older-than '{}' "
                                     "(expected <N>s, <N>m, <N>h, <N>d, or 0)", *olderThan);
                return 2;
            }
            keepWithinSecs = *secs;
        }
        return mcpp::build::clean_stale(dryRun, keepWithinSecs);
    }
    return mcpp::build::clean_project(parsed.is_flag_set("bmi-cache"));
}

// Hidden subcommand: aggregate P1689 .ddi files into a Ninja dyndep file.
// Invoked by ninja during build (cxx_collect / cxx_dyndep rules).
//
// Multi-file mode (legacy cxx_collect):
//   mcpp dyndep --output <build.ninja.dd> <ddi-1> <ddi-2> ...
//
// Single-file mode (P1 per-file dyndep, cxx_dyndep rule):
//   mcpp dyndep --single --output <file.dd> <file.ddi>
export int cmd_dyndep(const mcpplibs::cmdline::ParsedArgs& parsed) {
    std::filesystem::path outPath = parsed.option_or_empty("output").value();
    if (outPath.empty()) {
        std::println(stderr, "error: --output <path> required");
        return 2;
    }
    // Every path ninja hands this edge is relative to the build directory,
    // and a deep build directory takes it past the Windows path limit.
    outPath = mcpp::platform::fs::extended_length(outPath);

    bool single = parsed.is_flag_set("single");

    mcpp::dyndep::DyndepOptions opts;
    std::string bmiDirStorage = parsed.option_or_empty("bmi-dir").value();
    std::string bmiExtStorage = parsed.option_or_empty("bmi-ext").value();
    if (!bmiDirStorage.empty())
        opts.bmiDir = bmiDirStorage;
    if (!bmiExtStorage.empty())
        opts.bmiExt = bmiExtStorage;
    opts.splitModuleEdges = parsed.is_flag_set("split-module");

    std::expected<std::string, std::string> body;
    if (single) {
        if (parsed.positional_count() != 1) {
            std::println(stderr, "error: --single requires exactly one .ddi input");
            return 2;
        }
        // Plan-vs-ddi reconciliation: when the generator declared what the
        // planner assumed for this TU, compare against the compiler's own
        // scan and fail the edge on divergence (mandatory for
        // scan_overrides units; opt-in elsewhere via MCPP_VERIFY_MODGRAPH).
        std::string expProvides = parsed.option_or_empty("expect-provides").value();
        std::string expImports  = parsed.option_or_empty("expect-imports").value();
        if (!expProvides.empty() || !expImports.empty() ||
            parsed.is_flag_set("expect-none")) {
            std::ifstream is{mcpp::platform::fs::extended_length(
                std::filesystem::path{parsed.positional(0)})};
            std::string ddiBody{std::istreambuf_iterator<char>(is), {}};
            auto unit = mcpp::dyndep::parse_ddi(ddiBody);
            if (!unit) {
                std::println(stderr, "error: {}: {}", parsed.positional(0), unit.error());
                return 1;
            }
            std::optional<std::string> ep;
            if (!expProvides.empty()) ep = expProvides;
            std::vector<std::string> ei;
            for (std::size_t b = 0; b < expImports.size();) {
                auto e = expImports.find(',', b);
                if (e == std::string::npos) e = expImports.size();
                if (e > b) ei.emplace_back(expImports.substr(b, e - b));
                b = e + 1;
            }
            if (auto err = mcpp::dyndep::verify_unit_expectations(*unit, ep, ei)) {
                std::println(stderr, "error: {}", *err);
                return 1;
            }
        }
        body = mcpp::dyndep::emit_dyndep_single(
            mcpp::platform::fs::extended_length(
                std::filesystem::path{parsed.positional(0)}), opts);
    } else {
        std::vector<std::filesystem::path> ddis;
        for (std::size_t i = 0; i < parsed.positional_count(); ++i)
            ddis.emplace_back(mcpp::platform::fs::extended_length(
                std::filesystem::path{parsed.positional(i)}));
        body = mcpp::dyndep::emit_dyndep_from_files(ddis, /*stdImports=*/{}, opts);
    }

    if (!body) {
        std::println(stderr, "error: {}", body.error());
        return 1;
    }
    std::error_code ec;
    std::filesystem::create_directories(outPath.parent_path(), ec);
    std::ofstream os(outPath);
    os << *body;
    return os ? 0 : 1;
}

// Invoked by ninja during build (stage_file rule):
//   mcpp stage --output <dst> <src>
//
// Publishes a cache-owned artifact (std BMI, std.o, runtime DLL) into the
// build directory. See mcpp.build.stage for the semantics — in particular why
// an already-equivalent destination is left untouched (#311).
export int cmd_stage(const mcpplibs::cmdline::ParsedArgs& parsed) {
    std::filesystem::path outPath = parsed.option_or_empty("output").value();
    if (outPath.empty()) {
        std::println(stderr, "error: --output <path> required");
        return 2;
    }
    if (parsed.positional_count() != 1) {
        std::println(stderr, "error: stage requires exactly one source path");
        return 2;
    }

    mcpp::build::stage::StageOptions opts;
    std::string verify = parsed.option_or_empty("verify").value();
    if (verify.empty()) {
        if (const char* e = std::getenv("MCPP_STAGE_VERIFY"); e && *e)
            verify = e;
    }
    if (!verify.empty())
        opts.verify = mcpp::build::stage::parse_verify(verify);

    auto r = mcpp::build::stage::stage_file(
        mcpp::platform::fs::extended_length(std::filesystem::path{parsed.positional(0)}),
        mcpp::platform::fs::extended_length(outPath), opts);
    if (!r) {
        std::println(stderr, "error: {}", r.error().message);
        return 1;
    }
    return 0;
}


// `mcpp bmi-equal A B` — exit 0 when the two BMIs differ only by GCC's embedded
// wall clock. Invoked from the generated `cxx_module` rule in place of `cmp -s`,
// which can never succeed: GCC stamps `buildtime:`/`localtime:` into the BMI
// CONTENT, so two compiles of identical source always differ by four bytes and
// the interface-unchanged fast path never fired. See mcpp.build.stage.
export int cmd_bmi_equal(const mcpplibs::cmdline::ParsedArgs& parsed) {
    if (parsed.positional_count() != 2) {
        std::println(stderr, "error: bmi-equal requires exactly two paths");
        return 2;
    }
    const bool same = mcpp::build::stage::bmi_equivalent(
        mcpp::platform::fs::extended_length(std::filesystem::path{parsed.positional(0)}),
        mcpp::platform::fs::extended_length(std::filesystem::path{parsed.positional(1)}));
    // Exit status IS the answer, so it can drive `if ...; then` in the rule
    // exactly the way `cmp -s` did. No output on either path: this runs once per
    // module compile and any chatter would land in the build log.
    return same ? 0 : 1;
}

// `mcpp coff-def --output <def> --name <dll> <obj>...` — the auto-export edge.
//
// A subcommand rather than a shell fragment, for the reason bmi-equal is one:
// a generated ninja command written in POSIX shell is skipped entirely on
// Windows, which is the only platform this edge exists for. It also keeps the
// COFF reader testable as a pure function over bytes (tests/unit/test_coff_exports)
// instead of as whatever a command line happened to produce.
export int cmd_coff_def(const mcpplibs::cmdline::ParsedArgs& parsed) {
    std::filesystem::path out;
    if (auto v = parsed.value("output")) out = *v;
    if (out.empty()) {
        std::println(stderr, "error: coff-def requires --output");
        return 2;
    }
    std::string libName;
    if (auto v = parsed.value("name")) libName = *v;

    std::vector<mcpp::build::coff::Export> all;
    // When the AUTHOR has annotated the surface, the annotation wins and this
    // edge writes an empty EXPORTS section — the linker then takes its export
    // set from the objects' own `/EXPORT:` directives, exactly as if mcpp were
    // not here. Adding a generated list on top would export the same names twice
    // (LNK4197) and, worse, would export everything else as well, replacing a
    // chosen public surface with all of it.
    bool annotated = false;
    for (std::size_t i = 0; i < parsed.positional_count(); ++i) {
        const std::filesystem::path obj{ parsed.positional(i) };
        std::ifstream in(mcpp::platform::fs::extended_length(obj), std::ios::binary);
        if (!in) {
            std::println(stderr, "error: cannot read object '{}'", obj.string());
            return 1;
        }
        std::vector<std::byte> bytes;
        for (char c; in.get(c); ) bytes.push_back(static_cast<std::byte>(c));
        if (mcpp::build::coff::declares_exports(bytes)) annotated = true;
        auto syms = mcpp::build::coff::read_exports(bytes);
        if (!syms) {
            // Named with the object, because "which one" is the whole question
            // when one file out of two hundred is the problem.
            std::println(stderr, "error: {}: {}", obj.string(), syms.error());
            return 1;
        }
        all.insert(all.end(), syms->begin(), syms->end());
    }

    // Refused, not truncated. A `.def` cut at the ceiling links cleanly and
    // then fails at whichever consumer happens to need a symbol that fell off
    // the end — a diagnostic with no path back to this decision.
    if (annotated) all.clear();
    std::ranges::sort(all);
    all.erase(std::ranges::unique(all).begin(), all.end());
    if (all.size() > mcpp::build::coff::kMaxExports) {
        std::println(stderr,
            "error: {} exportable symbols, and PE addresses exports by 16-bit "
            "ordinal (max {}).\n"
            "  Auto-export cannot express this library. Mark its public surface "
            "with __declspec(dllexport)\n"
            "  and the export set becomes what you declared instead of "
            "everything.",
            all.size(), mcpp::build::coff::kMaxExports);
        return 1;
    }

    std::error_code ec;
    const auto outOpen = mcpp::platform::fs::extended_length(out);
    if (outOpen.has_parent_path())
        std::filesystem::create_directories(outOpen.parent_path(), ec);
    std::ofstream o(outOpen, std::ios::binary | std::ios::trunc);
    if (!o) {
        std::println(stderr, "error: cannot write '{}'", out.string());
        return 1;
    }
    o << mcpp::build::coff::write_def(libName, std::move(all));
    return o.good() ? 0 : 1;
}

// The three edges of the DetachCodegen shape. They are `mcpp` subcommands
// rather than shell fragments for two reasons: the previous BMI-equivalence
// logic lived in the generated ninja command as POSIX shell and was therefore
// SKIPPED ENTIRELY ON WINDOWS, and a shell fragment cannot outlive its shell —
// which is exactly what phase 1 has to do.
//
// `--` separates mcpp's own options from the compiler command line, so a
// compiler flag can never be mistaken for one of ours.
namespace {

// The compiler command, one argument per line, read from a file.
//
// NOT `--`: the cmdline parser implements that separator only at the top level,
// so a subcommand receives nothing after it — silently, with an empty argument
// list rather than an error. A file also sidesteps MAX_ARG_STRLEN (128 KiB for
// a single argv entry, which mcpp has hit before on link lines) and needs no
// quoting rules that a compiler flag could violate.
std::string read_command_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::string text((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    return text;
}

// `option_or_empty(...).value()`, the idiom the rest of this file uses.
// `parsed.value(name)` looks plausible and returns nothing here — the two are
// not interchangeable, and the difference is silent.
std::string opt_value(const mcpplibs::cmdline::ParsedArgs& parsed, std::string_view name) {
    return parsed.option_or_empty(name).value();
}

}  // namespace

// Phase 1: start the compiler, return when the BMI is published.
export int cmd_bmi_compile(const mcpplibs::cmdline::ParsedArgs& parsed) {
    mcpp::build::schedule::detach::CompileRequest req;
    // Every file path this edge opens goes through `file_path`; `--self` does
    // not, because it names an executable to spawn rather than a file to open.
    const auto file_path = [&](std::string_view name) {
        return mcpp::platform::fs::extended_length(
            std::filesystem::path{opt_value(parsed, name)});
    };
    req.bmi       = file_path("bmi");
    req.bmiTarget = opt_value(parsed, "bmi");
    req.slot      = file_path("slot");
    req.self      = std::filesystem::path{opt_value(parsed, "self")};
    req.semaphore = file_path("sem");
    req.maxCompilers = 0;
    if (const auto cap = opt_value(parsed, "cap"); !cap.empty())
        std::from_chars(cap.data(), cap.data() + cap.size(), req.maxCompilers);
    req.commandFile = file_path("command-file");
    req.depFrom     = file_path("dep-from");
    req.depTo       = file_path("dep-to");
    req.command     = read_command_file(req.commandFile);
    if (req.slot.empty()) {
        std::println(stderr, "error: bmi-compile needs --slot");
        return 2;
    }
    if (req.command.empty()) {
        std::println(stderr, "error: bmi-compile got no command from --command-file");
        return 2;
    }
    return mcpp::build::schedule::detach::compile_release_at_bmi(req);
}

// The supervisor. Detached by phase 1; never named by a build edge.
export int cmd_bmi_supervise(const mcpplibs::cmdline::ParsedArgs& parsed) {
    const auto slot = mcpp::platform::fs::extended_length(
        std::filesystem::path{opt_value(parsed, "slot")});
    const auto token = mcpp::platform::fs::extended_length(
        std::filesystem::path{opt_value(parsed, "token")});
    const auto command = read_command_file(mcpp::platform::fs::extended_length(
        std::filesystem::path{opt_value(parsed, "command-file")}));
    // Only `--slot` is checked here. An empty COMMAND is handed to supervise()
    // on purpose: it is the one place that can record the failure in the file
    // both waiters are polling. Returning early instead left them waiting on an
    // `.rc` that nobody would ever write.
    if (slot.empty()) {
        std::println(stderr, "error: bmi-supervise needs --slot");
        return 2;
    }
    return mcpp::build::schedule::detach::supervise(slot, token, command);
}

// Phase 2: join the detached compiler before anything reads its object.
export int cmd_bmi_await(const mcpplibs::cmdline::ParsedArgs& parsed) {
    const auto slot = mcpp::platform::fs::extended_length(
        std::filesystem::path{opt_value(parsed, "slot")});
    const auto object = mcpp::platform::fs::extended_length(
        std::filesystem::path{opt_value(parsed, "object")});
    if (slot.empty()) {
        std::println(stderr, "error: bmi-await needs --slot");
        return 2;
    }
    return mcpp::build::schedule::detach::await_unit(slot, object);
}

} // namespace mcpp::cli
