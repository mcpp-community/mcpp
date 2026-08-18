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
import mcpp.build.configure;
import mcpp.build.coff_exports;
import mcpp.build.stage;
import mcpp.build.schedule.detach_codegen;
import mcpp.build.test_targets;
import mcpp.dyndep;
import mcpp.log;
import mcpp.project;
import mcpp.manifest;
import mcpp.ui;

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

export int cmd_build(const mcpplibs::cmdline::ParsedArgs& parsed) {
    bool verbose  = parsed.is_flag_set("verbose") || mcpp::log::is_verbose();
    bool print_fp = parsed.is_flag_set("print-fingerprint");
    bool no_cache = parsed.is_flag_set("no-cache");
    bool configure_only = parsed.is_flag_set("configure-only");

    mcpp::build::BuildOverrides ov;
    if (auto t = parsed.value("target")) ov.target_triple = *t;
    if (auto p = parsed.value("package")) ov.package_filter = *p;
    // --cache global|local|off. --no-cache is the deprecated alias for off; the
    // old flag only ever cleared target/, which says nothing about a cache, so
    // it is expressed in terms of the new one rather than kept as a second axis.
    if (auto c = parsed.value("cache")) ov.cache_mode = *c;
    else if (no_cache)                  ov.cache_mode = "off";
    // Profile selection precedence: --profile NAME > --release / --dev > the
    // project default ([build].default-profile) > "release", resolved in
    // prepare_build. --release/--dev are shorthands only.
    if (auto pr = parsed.value("profile")) ov.profile = *pr;
    else if (parsed.is_flag_set("release")) ov.profile = "release";
    else if (parsed.is_flag_set("dev"))     ov.profile = "dev";
    if (auto fs = parsed.value("features")) ov.features = *fs;
    if (auto cp = parsed.value("cap")) ov.capabilities = *cp;
    ov.strict = parsed.is_flag_set("strict");
    ov.force_static = parsed.is_flag_set("static");

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
            int r = mcpp::build::run_build_plan(*ctx, verbose, no_cache, mo.target_triple);
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
    if (!print_fp && ov.target_triple.empty() && !ov.force_static
        && ov.profile.empty() && ov.features.empty() && !ov.strict
        && ov.capabilities.empty() && ov.package_filter.empty()
        && ov.cache_mode.empty()) {
        auto root = mcpp::project::find_manifest_root(std::filesystem::current_path());
        if (root) {
            if (auto rc = mcpp::build::try_fast_build(*root, verbose, no_cache)) {
                return *rc;
            }
        }
    }

    auto ctx = mcpp::build::prepare_build(print_fp, /*includeDevDeps=*/false,
                                          /*extraTargets=*/{}, ov);
    if (!ctx) { std::println(stderr, "error: {}", ctx.error()); return 2; }

    return mcpp::build::run_build_plan(*ctx, verbose, no_cache, ov.target_triple);
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
    std::string target_triple;
    if (auto tt = parsed.value("target-triple")) target_triple = *tt;
    return mcpp::build::build_run_target(targetName, passthrough, package_filter,
                                         cache_mode, no_cache, target_triple);
}

export int cmd_test(const mcpplibs::cmdline::ParsedArgs& parsed,
             std::span<const std::string> passthrough) {
    // Pre-`--` flags select the build mode for the test build (so e.g.
    // `mcpp test --profile contracts` compiles the code-under-test plus the
    // test binaries under that profile — a whole-build mode, the right
    // granularity for sanitizers / contract evaluation semantics). Post-`--`
    // args go to each test binary.
    mcpp::build::BuildOverrides ov;
    if (auto pr = parsed.value("profile"))  ov.profile  = *pr;
    if (auto fs = parsed.value("features")) ov.features = *fs;
    if (auto cp = parsed.value("cap")) ov.capabilities = *cp;
    ov.strict = parsed.is_flag_set("strict");
    if (auto p = parsed.value("package")) ov.package_filter = *p;
    if (auto c = parsed.value("cache")) ov.cache_mode = *c;
    else if (parsed.is_flag_set("no-cache")) ov.cache_mode = "off";

    mcpp::build::TestOptions to;
    if (parsed.positional_count() > 0) to.filter = parsed.positional(0);
    to.list = parsed.is_flag_set("list");
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
        std::vector<std::string> notRun;
        std::vector<std::pair<std::string, long long>> memberTimes;
        int totalPassed = 0, totalFailed = 0;
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
            memberTimes.emplace_back(mp, sum.elapsedMs);
            auto secs = static_cast<double>(sum.elapsedMs) / 1000.0;
            if (r != 0) {
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
            std::println("{{\"workspace_summary\":{{\"members\":{},\"passed\":{},\"failed\":{},"
                         "\"failed_members\":[{}],\"not_run\":[{}],\"elapsed_ms\":{}}}}}",
                         members->size(), totalPassed, totalFailed,
                         join(failed), join(notRun), wsElapsed);
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
        if (failed.empty() && notRun.empty())
            mcpp::ui::status("workspace result",
                std::format("ok. {} member(s); {} passed; 0 failed; finished in {:.2f}s",
                            members->size(), totalPassed,
                            static_cast<double>(wsElapsed) / 1000.0));
        else
            mcpp::ui::error(std::format(
                "workspace test: {}/{} member(s) failed; {} passed; {} failed; "
                "finished in {:.2f}s",
                failed.size(), members->size(), totalPassed, totalFailed,
                static_cast<double>(wsElapsed) / 1000.0));
        if (!failed.empty())
            mcpp::ui::plain(std::format("    failed members: {}", join_names(failed)));
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
            std::ifstream is{std::filesystem::path{parsed.positional(0)}};
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
        body = mcpp::dyndep::emit_dyndep_single(parsed.positional(0), opts);
    } else {
        std::vector<std::filesystem::path> ddis;
        for (std::size_t i = 0; i < parsed.positional_count(); ++i)
            ddis.emplace_back(parsed.positional(i));
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
        std::filesystem::path{parsed.positional(0)}, outPath, opts);
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
        std::filesystem::path{parsed.positional(0)},
        std::filesystem::path{parsed.positional(1)});
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
        std::ifstream in(obj, std::ios::binary);
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
    if (out.has_parent_path()) std::filesystem::create_directories(out.parent_path(), ec);
    std::ofstream o(out, std::ios::binary | std::ios::trunc);
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
    req.bmi       = std::filesystem::path{opt_value(parsed, "bmi")};
    req.slot      = std::filesystem::path{opt_value(parsed, "slot")};
    req.self      = std::filesystem::path{opt_value(parsed, "self")};
    req.semaphore = std::filesystem::path{opt_value(parsed, "sem")};
    req.maxCompilers = 0;
    if (const auto cap = opt_value(parsed, "cap"); !cap.empty())
        std::from_chars(cap.data(), cap.data() + cap.size(), req.maxCompilers);
    req.commandFile = std::filesystem::path{opt_value(parsed, "command-file")};
    req.depFrom     = std::filesystem::path{opt_value(parsed, "dep-from")};
    req.depTo       = std::filesystem::path{opt_value(parsed, "dep-to")};
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
    const std::filesystem::path slot{opt_value(parsed, "slot")};
    const std::filesystem::path token{opt_value(parsed, "token")};
    const auto command = read_command_file(std::filesystem::path{opt_value(parsed, "command-file")});
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
    const std::filesystem::path slot{opt_value(parsed, "slot")};
    const std::filesystem::path object{opt_value(parsed, "object")};
    if (slot.empty()) {
        std::println(stderr, "error: bmi-await needs --slot");
        return 2;
    }
    return mcpp::build::schedule::detach::await_unit(slot, object);
}

} // namespace mcpp::cli
