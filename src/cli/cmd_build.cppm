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
import mcpp.build.ninja;          // write_module_maps
import mcpp.bmi_cache.maintenance;   // parse_duration, for `clean --stale --older-than`
import mcpp.build.directives;      // the device-slot table
import mcpp.build.configure;
import mcpp.build.coff_exports;
import mcpp.build.stage;
import mcpp.build.schedule.detach_codegen;
import mcpp.build.test_targets;
import mcpp.cli.selection;
import mcpp.build.build_database;
import mcpp.build.build_program;
import mcpp.build.progress;         // the report every building command opens
import mcpp.build.refusal;          // offline-download-required (#648 A1)
import mcpp.diag;                   // a member's own diagnostics, in the envelope (WS3)
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

// A member's tests, discovered from the member's own directory. Discovery
// resolves the path it is given as `-p` would, so a member whose path is also
// another member's package name would be read as that member: a member is
// discovered from its own directory or refused, and never from another's.
std::expected<mcpp::build::TestTargetSet, std::string>
discover_member_tests(const std::filesystem::path& wsRoot, const std::string& mp) {
    auto d = mcpp::build::discover_test_targets(wsRoot, mp);
    if (!d) return d;
    std::error_code ec;
    const bool same = std::filesystem::equivalent(d->packageRoot, wsRoot / mp, ec);
    if (!ec && !same)
        return std::unexpected(std::format(
            "the path '{}' is also the name of another member's package, so its tests "
            "cannot be told from that member's; select it by its package name (-p <name>)",
            mp));
    return d;
}

// The tests of each member of a configuration group, for a plan of the group
// that includes them (`--configure-only`, `mcpp emit build-database`): the
// targets by member path, and the root and globs each member's discovery read.
struct GroupTests {
    std::map<std::string, std::vector<mcpp::manifest::Target>>              targets;
    std::vector<std::pair<std::filesystem::path, std::vector<std::string>>> discovery;
};
std::expected<GroupTests, std::string>
group_tests(const std::filesystem::path& wsRoot, const std::vector<std::string>& group) {
    GroupTests out;
    for (auto const& mp : group) {
        auto d = discover_member_tests(wsRoot, mp);
        if (!d) return std::unexpected(std::format("{}: {}", mp, d.error()));
        if (!d->targets.empty()) out.targets[mp] = std::move(d->targets);
        out.discovery.emplace_back(d->packageRoot, std::move(d->discover));
    }
    return out;
}

// run_build_plan, wrapped in the project's `[hooks]` lifecycle (#496).
//
// The hooks are those of the package being built: the root's, or in a
// workspace plan those of each selected member, in selection order (workspace
// design 2026-09-29 §15), around the one build of the plan. The lifecycle is
// deliberately paired: build_finished/build_failed are only ever reached after
// build_start has run, so a project that could not be prepared at all (bad
// manifest, unresolvable dependency, no toolchain) fires nothing — its hook
// programs may be exactly what preparation failed to install.
int run_build_with_hooks(mcpp::build::BuildContext& ctx, bool verbose,
                         bool no_cache, std::string_view targetOverride) {
    struct Subject {
        const mcpp::manifest::Hooks* hooks;
        std::filesystem::path        root;
    };
    std::vector<Subject> subjects;
    if (ctx.workspaceMembers.empty())
        subjects.push_back({&ctx.manifest.hooks, ctx.projectRoot});
    else
        for (auto const& m : ctx.workspaceMembers) subjects.push_back({&m.manifest.hooks, m.root});

    // `during_build` opens first and closes last: its interval is the one that
    // spans everything below. Its output is discarded unless --verbose, which
    // is the only way it could interleave into a compiler diagnostic.
    std::vector<std::unique_ptr<mcpp::hooks::Span>> spans;
    for (auto const& sub : subjects) {
        spans.push_back(std::make_unique<mcpp::hooks::Span>(
            *sub.hooks, sub.root, /*inheritOutput=*/verbose));
        if (!spans.back()->ok()) return 1;
    }

    // A hook command writes to the terminal itself, so the build's live
    // region is erased while one runs (build progress design 2026-09-29,
    // §5.3); a `during_build` command that writes while the build runs keeps
    // it erased for the whole build.
    std::optional<mcpp::ui::SuspendRegion> sharedTerminal;
    if (std::ranges::any_of(spans, [](auto const& s) { return s->writes_to_terminal(); }))
        sharedTerminal.emplace();
    auto invoke = [](auto const& hooks, mcpp::hooks::Event event, auto const& root) {
        mcpp::ui::SuspendRegion hookOwnsTheTerminal;
        return mcpp::hooks::invoke(hooks, event, root);
    };

    for (auto const& sub : subjects)
        if (!invoke(*sub.hooks, mcpp::hooks::Event::BuildStart, sub.root))
            return 1;

    int rc = mcpp::build::run_build_plan(ctx, verbose, no_cache, targetOverride);

    // Closed BEFORE the terminal hook. A "build finished" sound competing with
    // the background music it replaces is the ordering this line settles.
    bool spanOk = true;
    for (auto it = spans.rbegin(); it != spans.rend(); ++it)
        spanOk = (*it)->finish() && spanOk;

    auto terminalEvent = rc == 0 ? mcpp::hooks::Event::BuildFinished
                                 : mcpp::hooks::Event::BuildFailed;
    // The build's own exit code outranks the hook's: `mcpp build` returning
    // "the notifier failed" for a compile error would answer a question nobody
    // asked. A hook failure only decides the exit code of a build that worked.
    bool hookOk = true;
    sharedTerminal.reset();
    for (auto const& sub : subjects)
        hookOk = invoke(*sub.hooks, terminalEvent, sub.root) && hookOk;
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
    // The build's report: its steps, its status line, `Finished` (build
    // progress design 2026-09-29).
    mcpp::build::progress::open(verbose);

    mcpp::build::BuildOverrides ov = overrides_from_selectors(parsed);
    // --cache global|local|off. --no-cache is the deprecated alias for off; the
    // old flag only ever cleared target/, which says nothing about a cache, so
    // it is expressed in terms of the new one rather than kept as a second axis.
    if (auto c = parsed.value("cache")) ov.cache_mode = *c;
    else if (no_cache)                  ov.cache_mode = "off";

    // Fan-out prefixes every diagnostic with the member it came from; the
    // single-package path has nothing to disambiguate and passes "".
    // `configured`, when given, receives the plan's output directory and the
    // plan leaves the root compile database to the caller, which publishes
    // it once for every configuration it planned.
    auto configure_member = [&](mcpp::build::BuildOverrides memberOv,
                                std::string_view label,
                                std::vector<std::filesystem::path>* configured = nullptr) -> int {
        auto where = [&](std::string_view msg) {
            if (label.empty()) mcpp::ui::error(std::format("{}", msg));
            else               mcpp::ui::error(std::format("{}: {}", label, msg));
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
        if (configured) {
            ctx->plan.publishRootCompileDb = false;
            configured->push_back(ctx->outputDir);
        }
        return mcpp::build::run_configure_plan(*ctx, verbose);
    };

    // A workspace: the selected members, planned by configuration group
    // (workspace design 2026-09-29 §15). Each group is one plan, one graph and
    // one build directory, so a member that others use is compiled once.
    // Groups are independent (a member reached from two groups is a node of
    // each), and a failed group does not stop the others; the first non-zero
    // exit wins.
    auto selection = mcpp::cli::select_members(member_request(parsed));
    if (!selection) { mcpp::ui::error(std::format("{}", selection.error())); return 2; }
    if (*selection) {
        auto const& members = (*selection)->members;
        if (configure_only && members.size() == 1) {
            mcpp::build::BuildOverrides mo = ov;
            mo.package_filter = members.front();
            return configure_member(std::move(mo), "");
        }
        auto groups = workspace_groups((*selection)->root, members);
        if (!groups) { mcpp::ui::error(std::format("{}", groups.error())); return 2; }
        std::vector<std::string> request;
        for (auto const& g : *groups)
            for (auto const& mp : g) request.push_back(mp);
        // Configured as built: one plan per configuration group, with each
        // member's tests and dev-dependencies (the surface an editor needs,
        // as for one member), so the compile database of a group describes
        // each of its packages once.
        if (configure_only) {
            int rc = 0;
            std::vector<std::filesystem::path> configured;
            for (auto const& g : *groups) {
                auto configure_one_by_one = [&] {
                    // Planned member by member, so a member's failure affects
                    // that member only, as a plan of one member always did.
                    for (auto const& mp : g) {
                        mcpp::build::BuildOverrides one = ov;
                        one.package_filter = mp;
                        if (int r = configure_member(std::move(one), mp, &configured); r != 0) rc = r;
                    }
                };
                auto tests = group_tests((*selection)->root, g);
                if (!tests && g.size() > 1) { configure_one_by_one(); continue; }
                if (!tests) { mcpp::ui::error(std::format("{}", tests.error())); rc = 2; continue; }
                mcpp::build::BuildOverrides mo = ov;
                mo.package_filter.clear();
                mo.project_root = (*selection)->root;
                mo.workspace_members = g;
                mo.workspace_request = request;
                const bool includeDevDeps = !tests->targets.empty();
                mo.member_targets = std::move(tests->targets);
                auto ctx = mcpp::build::prepare_build(print_fp, includeDevDeps,
                                                      /*extraTargets=*/{}, mo);
                if (!ctx && g.size() > 1) { configure_one_by_one(); continue; }
                if (!ctx) { mcpp::ui::error(std::format("{}: {}", g.front(), ctx.error())); rc = 2; continue; }
                ctx->plan.publishRootCompileDb = false;
                if (int r = mcpp::build::run_configure_plan(*ctx, verbose); r != 0) rc = r;
                configured.push_back(ctx->outputDir);
            }
            if (!configured.empty())
                mcpp::build::publish_workspace_compile_commands((*selection)->root, configured);
            return rc;
        }
        const bool plain = !print_fp && ov.target_triple.empty() && !ov.force_static
            && ov.profile.empty() && ov.features.empty() && !ov.strict
            && ov.capabilities.empty() && ov.cache_mode.empty() && ov.accel.empty();
        if (plain) {
            if (auto rc = mcpp::build::try_fast_workspace_build(
                    (*selection)->root, *groups, verbose, no_cache))
                return *rc;
        }
        // Every group is planned first, one after another: planning runs
        // build programs and narrates what it resolves. The groups' builds
        // are then independent (§6), and run at the same time, each with a
        // static share of the machine's jobs, so the total stays within what
        // one build would use.
        int rc = 0;
        std::vector<mcpp::build::BuildContext> contexts;
        for (auto const& g : *groups) {
            mcpp::build::BuildOverrides mo = ov;
            mo.package_filter.clear();
            mo.project_root = (*selection)->root;
            mo.workspace_members = g;
            mo.workspace_request = request;
            auto ctx = mcpp::build::prepare_build(print_fp, /*includeDevDeps=*/false,
                                                  /*extraTargets=*/{}, mo);
            if (!ctx) { mcpp::ui::error(std::format("{}", ctx.error())); rc = 2; continue; }
            contexts.push_back(std::move(*ctx));
        }
        if (contexts.size() == 1) {
            const int r = run_build_with_hooks(contexts.front(), verbose, no_cache,
                                               ov.target_triple);
            return r != 0 ? r : rc;
        }
        // One report for every configuration: each package line names its
        // configuration, and one `Finished` follows them all (design §9).
        mcpp::build::progress::configurations(contexts.size());
        mcpp::build::progress::defer_finished();
        const std::size_t hw = std::max(1u, std::thread::hardware_concurrency());
        std::set<std::filesystem::path> directories;
        for (auto const& c : contexts) directories.insert(c.outputDir.lexically_normal());
        // The root compile database is the command's, published once below
        // from every group's (execute.cppm), not by each concurrent build.
        for (auto& c : contexts) c.plan.publishRootCompileDb = false;
        for (auto& c : contexts) {
            const std::size_t want = c.plan.scheduleNinjaJobs > 0
                ? static_cast<std::size_t>(c.plan.scheduleNinjaJobs) : hw + 2;
            c.plan.scheduleNinjaJobs = static_cast<int>(
                std::max<std::size_t>(1, want / directories.size()));
        }
        // Concurrency is across build directories. Two groups whose values
        // resolve to one directory (one toolchain spelled two ways) are built
        // one after the other in it, since one ninja owns a directory.
        std::map<std::filesystem::path, std::vector<std::size_t>> byDirectory;
        for (std::size_t i = 0; i < contexts.size(); ++i)
            byDirectory[contexts[i].outputDir.lexically_normal()].push_back(i);
        std::vector<int> results(contexts.size(), 0);
        {
            std::vector<std::jthread> builds;
            for (auto const& [dir, indices] : byDirectory)
                builds.emplace_back([&, indices] {
                    for (auto i : indices)
                        results[i] = run_build_with_hooks(contexts[i], verbose, no_cache,
                                                          ov.target_triple);
                });
        }
        for (int r : results)
            if (r != 0 && rc == 0) rc = r;
        std::vector<std::filesystem::path> dirs;
        for (auto const& c : contexts) dirs.push_back(c.outputDir);
        mcpp::build::publish_workspace_compile_commands((*selection)->root, dirs);
        if (rc == 0) mcpp::build::progress::finish_deferred();
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
    if (!ctx) { mcpp::ui::error(std::format("{}", ctx.error())); return 2; }

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
    // The run's closing notices belong in the envelope as `note` diagnostics;
    // taken here, they are not printed again as `tip:` lines at exit.
    auto take_closing_notes = [&] {
        for (auto& n : mcpp::ui::take_closing_notices())
            diagnostics.push_back({std::move(n.code), Severity::Note, std::move(n.message)});
    };
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
    // A failure with nothing to describe is one envelope with diagnostics and
    // no `data` (S2 §3.4: a command without data has failed), and exit 1. `fail_no_data` finishes with whatever `diagnostics` already holds —
    // used once a single diagnostic is pushed onto it (`failed`, below, for a
    // usage error decided before any member is tried) and once every
    // selected member's own planning has failed in turn (#699 item 1, E1: a
    // workspace where nothing planned still names each member's own reason).
    auto fail_no_data = [&]() -> int {
        if (!envelope) {
            for (auto const& d : diagnostics)
                std::println(stderr, "{}: {}",
                             mcpp::wire::severity_name(d.severity), d.message);
            return 1;
        }
        take_closing_notes();
        const auto text = mcpp::wire::to_json(mcpp::wire::Envelope{
            .kind = "mcpp.build-database",
            .effects = {Effect::ReadProject},
            .data = nullptr,
            .diagnostics = diagnostics,
        }).dump(2) + "\n";
        (void)publish(text);
        return 1;
    };
    auto failed = [&](std::string code, std::string message) -> int {
        diagnostics.push_back({std::move(code), Severity::Error, std::move(message)});
        return fail_no_data();
    };

    auto root = mcpp::project::find_manifest_root(std::filesystem::current_path());
    if (!root)
        return failed("MCPP_BUILD_DATABASE_NO_PROJECT",
                      "no mcpp.toml found in current directory or any parent");

    // One plan per configuration group, as `mcpp build` plans a workspace
    // (workspace design 2026-09-29 §15): a package the members share is one
    // set of the document, described once. A selection of one member is one
    // plan of that member, as before.
    struct PlanRequest {
        std::string                 member;   // one member's path; empty for a group or a package
        std::vector<std::string>    group;    // the members of a group plan
        mcpp::build::BuildOverrides mo;
    };
    std::vector<PlanRequest> requests;
    auto plan_alone = [&](const std::string& mp) {
        auto one = ov;
        one.package_filter = mp;
        requests.push_back({mp, {}, std::move(one)});
    };
    std::filesystem::path wsRoot = *root;
    const auto request = member_request(parsed);
    auto selection = mcpp::cli::select_members(request);
    if (!selection)
        return failed("MCPP_BUILD_DATABASE_PLAN_FAILED", selection.error());
    if (*selection && (*selection)->members.size() > 1) {
        wsRoot = (*selection)->root;
        // A member whose manifest cannot be read has no configuration; the
        // members are then planned one by one, and it fails alone (R5.2).
        auto groups = workspace_groups(wsRoot, (*selection)->members);
        if (!groups) {
            for (auto const& mp : (*selection)->members) plan_alone(mp);
        } else {
            for (auto const& g : *groups) {
                auto mo = ov;
                mo.package_filter.clear();
                mo.project_root = wsRoot;
                mo.workspace_members = g;
                mo.workspace_request = (*selection)->members;
                requests.push_back({g.size() == 1 ? g.front() : std::string{}, g, std::move(mo)});
            }
        }
    } else if (*selection && (*selection)->whole) {
        // The whole workspace, which lists one member (or one is left by
        // `--exclude`): that member's plan, as a selection of several plans.
        plan_alone((*selection)->members.front());
    } else {
        requests.push_back({std::string{}, {}, ov});
    }

    // An offline plan that needs a download is not a defect of the project, and
    // a client that plans offline by default (an editor) has to tell the two
    // apart without reading the message (#648 A1). The code is taken right
    // where a member's planning ends: `take()` clears the sink, so a refusal
    // recorded by a member that failed cannot relabel a later member's own
    // reason (the per-member analogue of the rule this used to apply once).
    auto plan_failure_code = [&] {
        const bool offline = mcpp::platform::env::offline_mode()
                          || mcpp::platform::env::no_auto_install();
        const bool wasOfflineDownload = mcpp::build::refusal::take()
            == mcpp::build::refusal::Code::OfflineDownloadRequired;
        return std::string(wasOfflineDownload && offline
            ? "MCPP_OFFLINE_DOWNLOAD_REQUIRED" : "MCPP_BUILD_DATABASE_PLAN_FAILED");
    };

    std::vector<mcpp::build::BuildContext> contexts;
    std::vector<std::filesystem::path>     workDirs;
    std::vector<std::vector<std::pair<std::filesystem::path, std::vector<std::string>>>> testDiscovery;
    // The root of every member whose planning failed: `mcpp.toml` and
    // `build.mcpp` (when it exists) join `watch` exactly as a planned
    // member's do (render(), below), so an edit that might fix the failure is
    // what wakes a consumer to ask again.
    std::vector<std::filesystem::path>     failedMemberRoots;
    // Each member's own diagnostics, as that member's (WS3 of the 2026-09-28
    // design): the terminal prints a fact once per process, and the envelope
    // keeps every occurrence with the member it belongs to. A record's domain
    // is its code: `build/msvc-crt-word` is `MCPP_BUILD_MSVC_CRT_WORD`.
    auto take_member_diagnostics = [&](const std::string& memberPath) {
        for (auto& r : mcpp::diag::take()) {
            std::string code = "MCPP_";
            for (char c : r.domain)
                code += (c == '/' || c == '-') ? '_'
                      : static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            diagnostics.push_back({std::move(code),
                                   r.severity == mcpp::diag::Severity::Note
                                       ? Severity::Note : Severity::Warning,
                                   r.format(), memberPath});
        }
    };
    {
        // mcpp narrates on standard error, but planning may start programs
        // that inherit standard output (build programs, installers); the
        // document is printed after this scope, alone.
        mcpp::platform::terminal::StdoutToStderr narration;
        // A group whose plan fails is planned member by member (appended to
        // `requests` and reached by this same loop), so a member's failure
        // still affects that member only (SPEC-005 R5.2).
        for (std::size_t ri = 0; ri < requests.size(); ++ri) {
            auto req = requests[ri];
            auto& mo = req.mo;
            const auto memberRoot = req.member.empty() ? wsRoot : wsRoot / req.member;
            const auto memberPath = req.member.empty() ? std::string("mcpp.toml")
                                                       : req.member + "/mcpp.toml";
            // The roots whose `mcpp.toml` and `build.mcpp` join `watch` when
            // this plan fails: the member's, or every member's of a group.
            std::vector<std::filesystem::path> planRoots;
            if (req.group.empty()) planRoots.push_back(memberRoot);
            for (auto const& mp : req.group) planRoots.push_back(wsRoot / mp);
            auto fail_plan = [&](std::string message) {
                diagnostics.push_back({plan_failure_code(), Severity::Error,
                                       std::move(message), memberPath});
                for (auto const& r : planRoots) failedMemberRoots.push_back(r);
            };
            // As `--configure-only`: tests and dev-dependencies are part of the
            // surface an editor needs, each member's own in a group plan.
            std::vector<mcpp::manifest::Target> extraTargets;
            std::vector<std::pair<std::filesystem::path, std::vector<std::string>>> discovery;
            bool includeDevDeps = false;
            if (req.group.empty()) {
                auto discovered = mcpp::build::discover_test_targets(*root, mo.package_filter);
                if (!discovered) {
                    fail_plan(req.member.empty() ? discovered.error()
                              : std::format("{}: {}", req.member, discovered.error()));
                    continue;
                }
                includeDevDeps = !discovered->targets.empty();
                discovery.emplace_back(discovered->packageRoot, discovered->discover);
                extraTargets = std::move(discovered->targets);
            } else {
                auto tests = group_tests(wsRoot, req.group);
                if (!tests && req.group.size() == 1) { fail_plan(tests.error()); continue; }
                if (!tests) {
                    for (auto const& mp : req.group) plan_alone(mp);
                    continue;
                }
                includeDevDeps = !tests->targets.empty();
                discovery = std::move(tests->discovery);
                mo.member_targets = std::move(tests->targets);
            }
            mo.plan_only = true;
            std::string workKey = mo.package_filter;
            for (auto const& mp : req.group) workKey += (workKey.empty() ? "" : ",") + mp;
            mo.work_dir = build_database_work_dir(req.group.empty() ? *root : wsRoot, workKey);
            std::error_code ec;
            std::filesystem::remove(mo.work_dir / "mcpp.lock", ec);
            const auto diagnosticsBefore = diagnostics.size();
            auto ctx = mcpp::build::prepare_build(/*print_fingerprint=*/false,
                                                  includeDevDeps,
                                                  std::move(extraTargets), mo);
            if (!ctx && req.group.size() > 1) {
                // What the group's attempt recorded is recorded again, per
                // member, by the plans below.
                (void)mcpp::diag::take();
                (void)mcpp::build::take_notes_on_failure();
                (void)mcpp::build::refusal::take();
                diagnostics.erase(diagnostics.begin() + static_cast<std::ptrdiff_t>(diagnosticsBefore),
                                  diagnostics.end());
                for (auto const& mp : req.group) plan_alone(mp);
                continue;
            }
            take_member_diagnostics(memberPath);
            if (!ctx) {
                // A wholly-failed member contributes exactly one `error`
                // diagnostic, `path` its `mcpp.toml` (SPEC-005 R5.2) — that
                // invariant is kept exactly, so a note an earlier phase
                // recorded (most importantly
                // `MCPP_BUILD_DATABASE_PROGRAM_FAILED`) is folded into THIS
                // diagnostic's own message instead of becoming a diagnostic of
                // its own. Without it, a later phase's failure that follows
                // from the missing directives (SPEC-005 R5.2's own words) read
                // as a single, unexplained symptom, and the actual cause —
                // recorded, then discarded the moment `prepare_build` returned
                // — never reached the reader (design 2026-09-27 §4.2, mcpp#724
                // side finding A, fix item 2).
                std::string message = req.member.empty() ? ctx.error()
                                                         : std::format("{}: {}", req.member, ctx.error());
                for (auto const& note : mcpp::build::take_notes_on_failure())
                    message += note.path.empty()
                        ? std::format("\n       earlier in this pass, {}: {}",
                                      note.code, note.message)
                        : std::format("\n       earlier in this pass, {} ({}): {}",
                                      note.code, note.path, note.message);
                diagnostics.push_back({plan_failure_code(), Severity::Error,
                                       std::move(message), memberPath});
                for (auto const& r : planRoots) failedMemberRoots.push_back(r);
                continue;
            }
            // The argument lists name the plan's module maps; the files are
            // written into the plan's work directory, so a reader that
            // expands them finds them.
            mcpp::build::write_module_maps(ctx->plan);
            contexts.push_back(std::move(*ctx));
            workDirs.push_back(mo.work_dir);
            testDiscovery.push_back(std::move(discovery));
        }
    }
    // Every selected member was planned independently (#699 item 1, E1): one
    // that failed contributed its own diagnostic above and nothing else.
    // Only when none of them planned is there nothing left to describe. A
    // document with `data` and error diagnostics is S2 0.3.0's partial answer
    // (S2-3.4-12, S2-3.4-13): it describes everything the errors do not name.
    if (contexts.empty())
        return fail_no_data();

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

    // A set is named by its package; a document of several configurations
    // prefixes each name with its configuration's name, the name of the build
    // directory the configuration is built in (SPEC-005 R3.3), so a package
    // two configurations compile is a set of each.
    std::set<std::string> configurations;
    for (auto const& c : contexts) configurations.insert(c.outputDir.filename().string());
    auto configurationPrefix = [&](const mcpp::build::BuildContext& c) {
        return configurations.size() > 1 ? c.outputDir.filename().string() + "/" : std::string{};
    };
    std::vector<mcpp::build::database::Member> members;
    bool ranBuildPrograms = false;
    for (std::size_t i = 0; i < contexts.size(); ++i) {
        members.push_back({&contexts[i], configurationPrefix(contexts[i]), workDirs[i],
                           testDiscovery[i]});
        if (!mcpp::build::declared_program_inputs(workDirs[i]).empty())
            ranBuildPrograms = true;
        for (auto const& sp : contexts[i].sourcePackages) {
            std::error_code ec;
            if (std::filesystem::exists(sp.root / "build.mcpp", ec)) ranBuildPrograms = true;
        }
    }
    // One line per selector: a value never spans lines, and a `\x1f` separator
    // before `f`, `c` or `a` reads as a longer hex escape (clang refuses it).
    // The members the flags name, each as written and in command-line order.
    // One `-p` reads as it always has, so the fingerprint of a command that
    // names one member does not change; `--exclude` adds a line only when it
    // is given.
    std::string packages;
    for (auto const& p : request.packages) packages += (packages.empty() ? "" : ",") + p;
    std::string excludes;
    for (auto const& e : request.excludes) excludes += (excludes.empty() ? "" : ",") + e;
    const auto selector = std::format(
        "spec={}\ntarget={}\ntoolchain={}\nprofile={}\nfeatures={}\n"
        "cap={}\naccel={}\nstatic={}\npackage={}\nworkspace={}{}",
        spec, ov.target_triple, mcpp::platform::env::get("MCPP_TOOLCHAIN").value_or(""),
        ov.profile, ov.features, ov.capabilities, ov.accel, ov.force_static,
        packages, parsed.is_flag_set("workspace"),
        excludes.empty() ? std::string{} : std::format("\nexclude={}", excludes));
    auto rendered = mcpp::build::database::render(members, failedMemberRoots,
                                                  wsRoot, selector);
    // A note's severity is its own (E3's program-failure note is an error;
    // every other note today is a warning) and its `path`, when set, already
    // names the file relative to the workspace root — render() rewrote it.
    for (auto& note : rendered.notes)
        diagnostics.push_back({std::move(note.code), note.severity,
                               std::move(note.message), std::move(note.path)});

    auto document = spec == "s1" ? std::move(rendered.database)
                                 : std::move(rendered.compileCommands);
    // The exit status is 1 whenever an error is present (#699 item 1, E1;
    // item 2, E3) even though `data` is: a workspace member's own failure, or
    // a build program's, is still a failure this command reports through its
    // exit code, only not by withholding the sets that DID plan.
    const bool hasError = std::ranges::any_of(diagnostics,
        [](const Diagnostic& d) { return d.severity == Severity::Error; });
    if (!envelope) {
        for (auto const& d : diagnostics)
            std::println(stderr, "{}: {}", mcpp::wire::severity_name(d.severity),
                         d.message);
        if (const auto rc = publish(document.dump(2) + "\n"); rc != 0) return rc;
        return hasError ? 1 : 0;
    }
    take_closing_notes();
    std::vector<Effect> effects{Effect::ReadProject, Effect::WriteGlobalCache};
    if (ranBuildPrograms) effects.push_back(Effect::ExecBuildScript);
    nlohmann::json specJson{{"name", spec}};
    if (spec == "s1")
        specJson["version"] = std::string(mcpp::build::database::kProfileVersion);
    if (const auto rc = publish(mcpp::wire::to_json(mcpp::wire::Envelope{
            .kind = "mcpp.build-database",
            .effects = std::move(effects),
            .data = nlohmann::json{
                {"spec",               std::move(specJson)},
                {"database",           std::move(document)},
                {"watch",              std::move(rendered.watch)},
                {"inputs-fingerprint", std::move(rendered.inputsFingerprint)},
            },
            .diagnostics = diagnostics,
        }).dump(2) + "\n"); rc != 0) return rc;
    return hasError ? 1 : 0;
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
    // `mcpp run` is single-member only — no `--workspace` fan-out — because an
    // artifact to execute is one program. The option is repeatable on the
    // commands that act on several members, so a second `-p` here is refused,
    // naming every member asked for, and never read as "the last one".
    const auto packages = parsed.option_or_empty("package").values;
    if (packages.size() > 1) {
        std::string named;
        for (std::size_t i = 0; i < packages.size(); ++i)
            named += std::format("{}'{}'",
                i == 0 ? "" : (i + 1 == packages.size() ? " and " : ", "), packages[i]);
        mcpp::ui::error(std::format(
            "mcpp run runs one program, so it acts on one workspace member, and -p names {}: "
            "pass one -p (mcpp build and mcpp test accept several)", named));
        return 2;
    }
    std::string package_filter;
    if (!packages.empty()) package_filter = packages.front();
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
    // Closed before the program starts: the program owns the terminal.
    mcpp::build::progress::open(mcpp::log::is_verbose());
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
    to.noRun    = parsed.is_flag_set("no-run");
    // The two read alike and mean opposite things: `--no-runner` says to run
    // the binaries WITHOUT the declared runner, `--no-run` says not to run
    // them at all. Asking for both is not a preference to resolve.
    if (to.noRun && to.noRunner) {
        mcpp::ui::error("--no-run and --no-runner cannot be combined: "
                        "--no-runner runs the test binaries directly, "
                        "--no-run does not run them.");
        return 2;
    }
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

    // The members this command tests, by the one selection every command reads
    // (`mcpp::cli::select_members`). A selection of one member, named or implied
    // by the directory, is that member's own test run, as it always was. A
    // selection of several members, or of the whole workspace, fans out: the
    // members are planned once per configuration group and each group is built
    // once, then each member's tests run in member order, continuing past a
    // failing member so that one red member never hides the rest (member
    // selection design 2026-09-30, S4).
    auto selection = mcpp::cli::select_members(member_request(parsed));
    if (!selection) { mcpp::ui::error(std::format("{}", selection.error())); return 2; }
    if (*selection && ((*selection)->whole || (*selection)->members.size() > 1)) {
        auto const& sel = **selection;
        auto const& members = sel.members;
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
        // Carried for the same reason `totalNotRun` is: without it a
        // `--no-run` workspace reports "0 passed; 0 failed", which is
        // what a workspace with no tests at all reports.
        int totalBuilt = 0;
        auto tWs = std::chrono::steady_clock::now();
        auto ws_ms = [&tWs] {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - tWs).count();
        };
        const long long wsDeadlineMs =
            static_cast<long long>(workspaceTimeoutSecs) * 1000;

        // Asked before a member's tests start. The deadline is checked BEFORE
        // starting a member rather than after: stopping mid-member would leave
        // a half-tested member reported as neither run nor skipped. It is
        // measured from the start of the command, so the build the members
        // share counts against it, and a member not started by then is listed
        // as not run; the build itself is bounded by --build-timeout.
        auto member_begin = [&](std::size_t i, const std::string& mp) -> bool {
            if (wsDeadlineMs > 0 && ws_ms() >= wsDeadlineMs) {
                notRun.push_back(mp);
                return false;
            }
            mcpp::ui::status("Workspace",
                std::format("testing member '{}' ({}/{})", mp, i + 1, members.size()));
            return true;
        };
        // The member's line: how it ended, and how long its own tests ran. The
        // build is the group's, stated once by the group's line, so a member's
        // line does not state it again.
        auto member_end = [&](std::size_t i, const std::string& mp, int r,
                              const mcpp::build::TestRunSummary& sum) {
            const auto idx = i + 1;
            totalPassed += sum.passed;
            totalFailed += sum.failed;
            totalNotRun += sum.notRun;
            totalBuilt  += sum.built;
            // Ranked by its run, which is the member's own: a build a group
            // shares belongs to no one member.
            memberTimes.emplace_back(mp, sum.buildGroup >= 0 ? sum.runMs : sum.elapsedMs);
            auto secs = static_cast<double>(sum.buildGroup >= 0 ? sum.runMs : sum.elapsedMs) / 1000.0;
            const char* took = sum.buildGroup >= 0 ? "run " : "";
            const char* in   = sum.buildGroup >= 0 ? ", " : " in ";
            if (r == 2 && sum.failed == 0 && sum.notRun > 0) {
                // Built and not executed (#544): the member did not fail, and
                // it did not pass. 2 outranks 0 and yields to 1, as it does
                // for a single member.
                if (rc == 0) rc = 2;
                unrunnable.push_back(mp);
                mcpp::ui::status("Workspace",
                    std::format("member '{}' ({}/{}) NOT RUN — {} passed, {} not run{}{}{:.2f}s",
                                mp, idx, members.size(), sum.passed, sum.notRun, in, took, secs));
            } else if (r != 0) {
                rc = r;
                failed.push_back(mp);
                mcpp::ui::status("Workspace",
                    std::format("member '{}' ({}/{}) FAILED — {} passed, {} failed{}{}{:.2f}s",
                                mp, idx, members.size(), sum.passed, sum.failed, in, took, secs));
            } else {
                // Under `--no-run` nothing passed and nothing was meant to:
                // reporting "0 passed" for a member whose tests all built is
                // the same sentence a member with no tests would produce.
                mcpp::ui::status("Workspace",
                    sum.built
                        ? std::format("member '{}' ({}/{}) ok — {} built, not run{}{}{:.2f}s",
                                      mp, idx, members.size(), sum.built, in, took, secs)
                        : std::format("member '{}' ({}/{}) ok — {} passed{}{}{:.2f}s",
                                      mp, idx, members.size(), sum.passed, in, took, secs));
            }
        };

        if (to.list) {
            // A listing builds nothing, so there is nothing to plan once: each
            // member lists its own tests.
            for (std::size_t i = 0; i < members.size(); ++i) {
                if (!member_begin(i, members[i])) continue;
                mcpp::build::BuildOverrides mo = ov;
                mo.package_filter = members[i];
                mcpp::build::TestRunSummary sum;
                int r = mcpp::build::run_tests(passthrough, mo, to, &sum);
                member_end(i, members[i], r, sum);
            }
        } else {
            // Each member's own tests, discovered from the member's own
            // directory: two members may each have a `tests/main.cpp`.
            std::vector<mcpp::build::WorkspaceTestMember> inputs;
            for (auto const& mp : members) {
                mcpp::build::WorkspaceTestMember wm;
                wm.path = mp;
                auto d = discover_member_tests(sel.root, mp);
                if (!d) {
                    wm.error = std::format("{}: {}", mp, d.error());
                } else {
                    wm.targets = std::move(d->targets);
                    if (wm.targets.empty()) {
                        // Names where it looked when the manifest chose the
                        // place, so that a glob that matches nothing is not
                        // read as a project without tests.
                        if (d->discoverDeclared) {
                            std::string globs;
                            for (auto const& g : d->discover)
                                globs += std::format("{}\"{}\"", globs.empty() ? "" : ", ", g);
                            wm.noTests = std::format("no tests found ([test] discover = [{}])", globs);
                        } else {
                            wm.noTests = "no tests found in tests/";
                        }
                    }
                }
                inputs.push_back(std::move(wm));
            }
            // Members whose root-position values are equal are planned together,
            // as `mcpp build` plans them. A member whose manifest cannot be read
            // has no configuration: each member is then its own group, and it
            // fails alone when it is planned.
            std::vector<std::vector<std::string>> groups;
            if (auto g = workspace_groups(sel.root, members)) groups = std::move(*g);
            else for (auto const& mp : members) groups.push_back({mp});

            mcpp::build::WorkspaceTestHooks hooks;
            hooks.begin = member_begin;
            hooks.end = member_end;
            mcpp::build::run_workspace_tests(passthrough, ov, to, sel.root, groups,
                                             std::move(inputs), hooks);
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
                         "\"tests_not_run\":{},\"tests_built\":{},"
                         "\"failed_members\":[{}],\"unrunnable_members\":[{}],"
                         "\"not_run\":[{}],\"elapsed_ms\":{}}}}}",
                         members.size(), totalPassed, totalFailed, totalNotRun,
                         totalBuilt,
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
        if (totalBuilt)
            notRunCounts += std::format("; {} built, not run", totalBuilt);
        if (failed.empty() && notRun.empty() && unrunnable.empty())
            mcpp::ui::result("workspace result",
                std::format("ok. {} member(s); {} passed; 0 failed{}; finished in {:.2f}s",
                            members.size(), totalPassed, notRunCounts,
                            static_cast<double>(wsElapsed) / 1000.0));
        else
            mcpp::ui::error(std::format(
                "workspace test: {}/{} member(s) failed; {} passed; {} failed{}; "
                "finished in {:.2f}s",
                failed.size(), members.size(), totalPassed, totalFailed, notRunCounts,
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
    // The unit's module map, when a BMI it reaches lies below its provider's
    // directory (pack drive and selection design 2026-10-01, B1).
    std::map<std::string, std::string, std::less<>> moduleMap;
    if (auto mm = parsed.option_or_empty("module-map").value(); !mm.empty()) {
        std::ifstream is{mcpp::platform::fs::extended_length(std::filesystem::path{mm})};
        if (!is) {
            std::println(stderr, "error: cannot read module map '{}'", mm);
            return 1;
        }
        std::string mapBody{std::istreambuf_iterator<char>(is), {}};
        moduleMap = mcpp::dyndep::parse_module_map(mapBody);
        opts.moduleMap = &moduleMap;
    }

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
//   mcpp stage --output <dst> <src>...
//
// Publishes a cache-owned artifact (std BMI, std.o, runtime DLL) into the
// build directory. See mcpp.build.stage for the semantics — in particular why
// an already-equivalent destination is left untouched (#311).
//
// More than one source (SPEC-007 R4.2, mcpp#723) means two or more packages
// of this graph deploy the same destination; `stage_files` places it when
// every source is byte-identical and otherwise fails, naming every source
// and the destination. One source — every invocation before this feature —
// takes the exact path it always has.
export int cmd_stage(const mcpplibs::cmdline::ParsedArgs& parsed) {
    // `--list FILE` (#734 E4): many placements, one process. Each destination
    // keeps the single-file semantics below -- content comparison, an
    // out-of-place write, the check that several sources agree -- because each
    // group is handed to the same `stage_files`.
    if (auto listFile = parsed.option_or_empty("list").value(); !listFile.empty()) {
        mcpp::build::stage::StageOptions opts;
        std::string verify = parsed.option_or_empty("verify").value();
        if (verify.empty())
            if (const char* e = std::getenv("MCPP_STAGE_VERIFY"); e && *e) verify = e;
        if (!verify.empty()) opts.verify = mcpp::build::stage::parse_verify(verify);
        std::ifstream in(std::filesystem::path{listFile}, std::ios::binary);
        if (!in) {
            std::println(stderr, "error: cannot read the placement list {}", listFile);
            return 1;
        }
        std::vector<std::pair<std::string, std::vector<std::filesystem::path>>> groups;
        std::vector<std::vector<std::string>> spelled;   // each group's sources as the list writes them
        std::map<std::string, std::size_t> index;
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            const auto tab = line.find('\t');
            if (tab == std::string::npos) {
                std::println(stderr, "error: placement list line without a tab: {}", line);
                return 2;
            }
            std::string src = line.substr(0, tab), dst = line.substr(tab + 1);
            auto [it, fresh] = index.emplace(dst, groups.size());
            if (fresh) { groups.push_back({dst, {}}); spelled.emplace_back(); }
            groups[it->second].second.push_back(
                mcpp::platform::fs::extended_length(std::filesystem::path{src}));
            spelled[it->second].push_back(src);
        }
        for (std::size_t g = 0; g < groups.size(); ++g) {
            auto const& [dst, srcs] = groups[g];
            auto r = mcpp::build::stage::stage_files(
                srcs, mcpp::platform::fs::extended_length(std::filesystem::path{dst}), opts);
            if (!r) {
                // One edge places the whole list, so ninja's echo of the
                // command no longer shows which files were involved; the
                // entries are named here as the list writes them.
                std::println(stderr, "error: {}", r.error().message);
                std::println(stderr, "  placement list entries ({}):", listFile);
                for (auto const& src : spelled[g]) std::println(stderr, "    {} -> {}", src, dst);
                return 1;
            }
        }
        return 0;
    }
    std::filesystem::path outPath = parsed.option_or_empty("output").value();
    if (outPath.empty()) {
        std::println(stderr, "error: --output <path> required");
        return 2;
    }
    if (parsed.positional_count() < 1) {
        std::println(stderr, "error: stage requires at least one source path");
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

    std::vector<std::filesystem::path> sources;
    for (std::size_t i = 0; i < parsed.positional_count(); ++i)
        sources.push_back(mcpp::platform::fs::extended_length(
            std::filesystem::path{parsed.positional(i)}));

    auto r = mcpp::build::stage::stage_files(
        sources, mcpp::platform::fs::extended_length(outPath), opts);
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
