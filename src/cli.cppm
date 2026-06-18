// mcpp.cli — top-level command dispatch (and nothing else).
//
// The cli layer only parses arguments and routes:
//   mcpp.cli.cmd_build / cmd_new / cmd_registry / cmd_cache /
//   mcpp.cli.cmd_toolchain / cmd_publish / cmd_self   (parse + route)
//   mcpp.pm.commands                                  (add / remove / update)
// Domain logic lives in its owning subsystem: mcpp.build.{prepare,execute},
// mcpp.pm.index_management, mcpp.toolchain.lifecycle, mcpp.scaffold.create,
// mcpp.publish.pipeline, mcpp.pack.pipeline, mcpp.bmi_cache.maintenance, mcpp.doctor,
// mcpp.project, mcpp.fetcher.progress.
// See .agents/docs/2026-06-10-cli-modularization.md for the architecture.

module;
#include <cstdio>
#include <cstdlib>

export module mcpp.cli;

import std;
import mcpplibs.cmdline;
import mcpp.cli.cmd_build;
import mcpp.cli.cmd_cache;
import mcpp.cli.cmd_new;
import mcpp.cli.cmd_publish;
import mcpp.cli.cmd_registry;
import mcpp.cli.cmd_self;
import mcpp.cli.cmd_toolchain;
import mcpp.pm.commands;
import mcpp.toolchain.fingerprint;   // MCPP_VERSION
import mcpp.ui;
import mcpp.log;

export namespace mcpp::cli {

int run(int argc, char** argv);

} // namespace mcpp::cli

namespace mcpp::cli {

// Custom top-level help. cmdline's auto-generated `print_help` is a fine
// default but its layout (`USAGE:`, no command-specific blurbs) doesn't
// match what the e2e tests assert against — they check for `Usage:`
// (mixed case) plus `mcpp new` / `mcpp build` literals. We keep the
// canonical printer here so the docs/CHANGELOG examples don't drift
// every time cmdline tweaks its formatting.
void print_usage() {
    std::println("mcpp v{} — modern C++23 build tool", mcpp::toolchain::MCPP_VERSION);
    std::println("");
    std::println("Usage:");
    std::println("Project commands:");
    std::println("  mcpp new <name>                      Create a new package skeleton");
    std::println("  mcpp build [options]                 Build the current package");
    std::println("  mcpp run [target] [-- args...]       Build + run a binary target");
    std::println("  mcpp test [-- args...]               Build + run all tests/**/*.cpp");
    std::println("  mcpp clean [--bmi-cache]             Remove target/ (and optionally BMI cache)");
    std::println("  mcpp add <pkg>[@<ver>]               Add a dependency to mcpp.toml");
    std::println("  mcpp remove <pkg>                    Remove a dependency from mcpp.toml");
    std::println("  mcpp update [pkg]                    Re-resolve deps and rewrite mcpp.lock");
    std::println("  mcpp search <keyword>                Search packages in registries");
    std::println("  mcpp publish [--dry-run]             Publish package to default registry");
    std::println("  mcpp pack [--mode <m>]               Build + bundle a tarball (m: system|vendored|self-contained|static)");
    std::println("  mcpp emit xpkg [-V VER] [-o FILE]    Generate xpkg Lua entry");
    std::println("");
    std::println("Resource management:");
    std::println("  mcpp toolchain install|list|default  Manage mcpp's private toolchains");
    std::println("  mcpp cache list|prune|clean|info     Inspect/manage the global BMI cache");
    std::println("  mcpp index list|add|remove|update    Manage package registries");
    std::println("");
    std::println("About mcpp itself:");
    std::println("  mcpp self doctor                     Diagnose mcpp environment health");
    std::println("  mcpp self env                        Print mcpp paths and toolchain");
    std::println("  mcpp self config [--mirror CN|GLOBAL] Show or modify mcpp's xlings config");
    std::println("  mcpp self version                    Show mcpp version");
    std::println("  mcpp self explain <CODE>             Show extended description for an error code");
    std::println("  mcpp --help / --version              Help / version");
    std::println("");
    std::println("Build options:");
    std::println("  --verbose, -v                        Verbose compiler output");
    std::println("  --quiet, -q                          Suppress status output");
    std::println("  --print-fingerprint                  Show toolchain fingerprint and 10 inputs");
    std::println("  --no-cache                           Force-clear target/ before building");
    std::println("  --no-color                           Disable colored output");
    std::println("");
    std::println("Docs: https://github.com/mcpp-community/mcpp/tree/main/docs");
}

int run(int argc, char** argv) {
    namespace cl = mcpplibs::cmdline;

    // ─── --quiet / --no-color: pre-scan ─────────────────────────────────
    // The cmdline lib propagates global options into nested subcommand
    // ParsedArgs, but we set ui:: state up-front so that *every* line
    // emitted from the action lambdas (including the very first
    // "Resolving toolchain" banner) honours the user's intent. This is
    // a side-channel only — the global options are still declared on
    // the App below so they show up in --help and pass schema checks.
    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        if      (a == "--quiet" || a == "-q") mcpp::ui::set_quiet(true);
        else if (a == "--no-color")           mcpp::ui::disable_color();
        else if (a == "--verbose" || a == "-v") mcpp::log::set_verbose(true);
    }

    // ─── top-level --help / -h / --version intercept ────────────────────
    // cmdline auto-handles these but its formatter doesn't match the
    // mixed-case "Usage:" + per-command blurbs that documentation +
    // e2e tests pin. Print our canonical screen and `mcpp X.Y.Z`
    // version line up-front so the App never sees these tokens.
    if (argc >= 2) {
        std::string_view a = argv[1];
        if (a == "--help" || a == "-h") { print_usage(); return 0; }
        if (a == "--version" || a == "-V") {
            std::println("mcpp {} - ", mcpp::toolchain::MCPP_VERSION);
            return 0;
        }
    }

    // ─── action_rc plumbing ─────────────────────────────────────────────
    // cmdline's action callback returns void. Capture int return codes
    // via a shared local; `wrap_rc` adapts an `int(ParsedArgs&)` lambda
    // into the void-returning shape cmdline expects.
    int action_rc = 0;
    auto wrap_rc = [&action_rc](auto&& fn) {
        return [fn = std::forward<decltype(fn)>(fn), &action_rc]
               (const cl::ParsedArgs& args) {
            action_rc = fn(args);
        };
    };

    // ─── nested-subcommand dispatcher ───────────────────────────────────
    // cmdline's run() only dispatches one level — when a parent subcommand
    // (e.g. `self`, `cache`, `index`, `emit`) has its own children but no
    // parent action, the matched leaf never gets invoked. We give every
    // such parent an action that switches on the parsed child name and
    // forwards to the right cmd_* directly.
    using cmd_fn = int(*)(const cl::ParsedArgs&);
    auto dispatch_sub = [](std::string_view parent,
                           const cl::ParsedArgs& parsed,
                           std::initializer_list<std::pair<std::string_view, cmd_fn>> table) -> int {
        if (!parsed.has_subcommand()) {
            std::string usage = std::format("`mcpp {}` requires a subcommand: ", parent);
            bool first = true;
            for (auto& [n, _] : table) {
                if (!first) usage += " / ";
                usage += n;
                first = false;
            }
            mcpp::ui::error(usage);
            return 2;
        }
        auto name    = parsed.subcommand_name();
        auto sub_ref = parsed.subcommand();
        if (!sub_ref) return 2;
        for (auto& [n, fn] : table) {
            if (n == name) return fn(sub_ref->get());
        }
        mcpp::ui::error(std::format("unknown `mcpp {} {}` subcommand", parent, name));
        return 2;
    };

    // ─── `--` passthrough for `mcpp run` / `mcpp test` ──────────────────
    // cmdline natively recognises `--` and dumps everything after it
    // into `parsed.positionals` (with the bare `--` token dropped). For
    // `mcpp run [target] -- args...` we need to distinguish the
    // optional `target` (a real positional) from the passthrough
    // tokens (which must reach the executed binary verbatim, even when
    // they look like `-x` / `--foo`). We split ourselves at the first
    // `--` and present cmdline only the pre-`--` slice; post-args go
    // into `passthrough` and are handed to the action helper directly.
    std::vector<std::string> passthrough;
    std::vector<char*> trimmed_argv(argv, argv + argc);
    {
        for (std::size_t i = 1; i < trimmed_argv.size(); ++i) {
            if (std::string_view(trimmed_argv[i]) != "--") continue;
            for (std::size_t j = i + 1; j < trimmed_argv.size(); ++j)
                passthrough.emplace_back(trimmed_argv[j]);
            trimmed_argv.resize(i);  // drop `--` and everything after
            break;
        }
    }
    int      trimmed_argc = static_cast<int>(trimmed_argv.size());
    char**   trimmed_argp = trimmed_argv.empty() ? nullptr : trimmed_argv.data();

    // ─── Build the top-level App ────────────────────────────────────────
    auto app = cl::App("mcpp")
        .version(std::string{mcpp::toolchain::MCPP_VERSION})
        .description("modern C++ build tool")
        .option(cl::Option("quiet").short_name('q')
            .help("Suppress status output").global())
        .option(cl::Option("verbose").short_name('v')
            .help("Show detailed progress on stderr").global())
        .option(cl::Option("no-color")
            .help("Disable colored output").global())

        // ─── project commands ──────────────────────────────────────────
        .subcommand(cl::App("new")
            .description("Create a new mcpp package skeleton")
            // not .required(): `--list-templates` runs without a name
            // (cmd_new validates presence for project creation itself).
            .arg(cl::Arg("name").help("Package directory name"))
            .option(cl::Option("template").short_name('t').takes_value().value_name("SPEC")
                .help("bin (default) | <pkg>[@ver][:<template>] — package-shipped template"))
            .option(cl::Option("list-templates").takes_value().value_name("PKG")
                .help("List the templates a package ships (PKG[@ver])"))
            .action(wrap_rc(cmd_new)))
        .subcommand(cl::App("build")
            .description("Build the current package")
            .option(cl::Option("print-fingerprint")
                .help("Show toolchain fingerprint and 10 inputs"))
            .option(cl::Option("no-cache")
                .help("Force-clear target/ before building"))
            .option(cl::Option("target").takes_value().help(
                "Build for <triple> (e.g. x86_64-linux-musl); looks up [target.<triple>] in mcpp.toml"))
            .option(cl::Option("static").help(
                "Force static linking (-static). On Linux, prefer pairing with --target <arch>-linux-musl"))
            .option(cl::Option("package").short_name('p').takes_value().value_name("NAME")
                .help("Build only the named workspace member"))
            .option(cl::Option("profile").takes_value().value_name("NAME")
                .help("Build profile: release (default) | dev | dist | <[profile.*] name>"))
            .option(cl::Option("features").takes_value().value_name("LIST")
                .help("Activate root-package features (comma-separated)"))
            .option(cl::Option("strict")
                .help("Treat manifest schema warnings (unknown feature/platform) as errors"))
            .action(wrap_rc(cmd_build)))
        .subcommand(cl::App("run")
            .description("Build + run a binary target (after `--`, args are passed to it)")
            .arg(cl::Arg("target").help("Binary target name (optional)"))
            .action(wrap_rc([&passthrough](const cl::ParsedArgs& p) {
                return cmd_run(p, std::span<const std::string>(passthrough));
            })))
        .subcommand(cl::App("test")
            .description("Build + run all tests/**/*.cpp (after `--`, args go to each test binary)")
            .option(cl::Option("profile").takes_value().value_name("NAME")
                .help("Build profile for the test build: release (default) | dev | dist | <[profile.*] name>"))
            .option(cl::Option("features").takes_value().value_name("LIST")
                .help("Activate root-package features for the test build (comma-separated)"))
            .option(cl::Option("strict")
                .help("Treat manifest schema warnings (unknown feature/platform) as errors"))
            .action(wrap_rc([&passthrough](const cl::ParsedArgs& p) {
                return cmd_test(p, std::span<const std::string>(passthrough));
            })))
        .subcommand(cl::App("clean")
            .description("Remove target/ (and optionally the global BMI cache)")
            .option(cl::Option("bmi-cache").help("Also wipe the global BMI cache"))
            .action(wrap_rc(cmd_clean)))
        .subcommand(cl::App("why")
            .description("Explain how the toolchain / runtime / deps were resolved")
            .arg(cl::Arg("topic").help("toolchain | runtime | deps (default: all)"))
            .action(wrap_rc(cmd_why)))
        .subcommand(cl::App("resolve")
            .description("Re-resolve the build plan and explain it")
            .option(cl::Option("explain").help("Print resolved toolchain / runtime / deps"))
            .action(wrap_rc(cmd_why)))
        .subcommand(cl::App("add")
            .description("Add a dependency to mcpp.toml")
            .arg(cl::Arg("pkg").help("Package spec, e.g. foo@1.0.0").required())
            .action(wrap_rc(mcpp::pm::commands::cmd_add)))
        .subcommand(cl::App("remove")
            .description("Remove a dependency from mcpp.toml")
            .arg(cl::Arg("pkg").help("Package name").required())
            .action(wrap_rc(mcpp::pm::commands::cmd_remove)))
        .subcommand(cl::App("update")
            .description("Re-resolve dependencies and rewrite mcpp.lock")
            .arg(cl::Arg("pkg").help("If given, update only that package"))
            .action(wrap_rc(mcpp::pm::commands::cmd_update)))
        .subcommand(cl::App("search")
            .description("Search packages in configured registries")
            .arg(cl::Arg("keyword").help("Search keyword (substring match)").required())
            .action(wrap_rc(cmd_search)))
        .subcommand(cl::App("publish")
            .description("Publish package to default registry")
            .option(cl::Option("dry-run").help("Print xpkg.lua without uploading"))
            .option(cl::Option("allow-dirty").help("Allow uncommitted changes"))
            .action(wrap_rc(cmd_publish)))
        .subcommand(cl::App("pack")
            .description("Build + bundle into a self-contained tarball")
            .option(cl::Option("mode").takes_value()
                .help("system | vendored (default) | self-contained | static"))
            .option(cl::Option("target").takes_value()
                .help("Triple, e.g. x86_64-linux-musl"))
            .option(cl::Option("format").takes_value()
                .help("tar (default) | dir"))
            .option(cl::Option("output").short_name('o').takes_value()
                .help("Override output path"))
            .action(wrap_rc(cmd_pack)))

        // ─── emit (one nested subcommand: xpkg) ────────────────────────
        .subcommand(cl::App("emit")
            .description("Generate package descriptor (xpkg)")
            .subcommand(cl::App("xpkg")
                .description("Generate xpkg Lua entry")
                .option(cl::Option("version").short_name('V').takes_value().value_name("VER")
                    .help("Override package version"))
                .option(cl::Option("output").short_name('o').takes_value().value_name("FILE")
                    .help("Write to file instead of stdout")))
            .action(wrap_rc([&dispatch_sub](const cl::ParsedArgs& p) {
                return dispatch_sub("emit", p, {{"xpkg", cmd_emit_xpkg}});
            })))

        // ─── resource management ───────────────────────────────────────
        .subcommand(cl::App("toolchain")
            .description("Install / list / select / remove C++ toolchains")
            .subcommand(cl::App("list").description("List installed toolchains"))
            .subcommand(cl::App("install")
                .description("Install a toolchain via mcpp's xlings")
                // Both `mcpp toolchain install gcc 16.1.0` and `mcpp toolchain
                // install gcc@16.1.0` are accepted, and the version may be
                // partial (`15`, `15.1`) — mcpp resolves to the highest match.
                .arg(cl::Arg("compiler").help("e.g. gcc, gcc@16.1.0, gcc@15-musl").required())
                .arg(cl::Arg("version").help("e.g. 16.1.0, 15, 15.1, 15.1.0-musl")))
            .subcommand(cl::App("default")
                .description("Set the default toolchain")
                // Same dual-form as `install`: `gcc@16.1.0` or `gcc 16.1.0`,
                // partial versions allowed.
                .arg(cl::Arg("spec").help("<compiler>[@<version>] (version may be partial)").required())
                .arg(cl::Arg("version").help("(optional, alternative to @-form)")))
            .subcommand(cl::App("remove")
                .description("Uninstall a toolchain")
                .arg(cl::Arg("spec").help("<compiler>@<version>").required()))
            .action(wrap_rc(cmd_toolchain)))
        .subcommand(cl::App("cache")
            .description("Inspect and manage the global BMI cache")
            .subcommand(cl::App("list")
                .description("List cache entries with size + last-access"))
            .subcommand(cl::App("info")
                .description("Show details for a cached package")
                .arg(cl::Arg("pkg").help("<pkg>@<ver>").required()))
            .subcommand(cl::App("prune")
                .description("Drop entries older than a threshold")
                .option(cl::Option("older-than").takes_value().value_name("N{s|m|h|d}")
                    .help("Age threshold (e.g. 30d)")))
            .subcommand(cl::App("clean")
                .description("Drop all dep cache entries (preserves std.gcm)"))
            .action(wrap_rc([&dispatch_sub](const cl::ParsedArgs& p) {
                return dispatch_sub("cache", p, {
                    {"list",  cmd_cache_list},
                    {"info",  cmd_cache_info},
                    {"prune", cmd_cache_prune},
                    {"clean", cmd_cache_clean},
                });
            })))
        .subcommand(cl::App("index")
            .description("Manage configured package registries")
            .subcommand(cl::App("list")
                .description("List configured registries"))
            .subcommand(cl::App("add")
                .description("Add a custom registry")
                .arg(cl::Arg("name").help("Registry name").required())
                .arg(cl::Arg("url").help("Registry URL").required()))
            .subcommand(cl::App("remove")
                .description("Remove a registry")
                .arg(cl::Arg("name").help("Registry name").required()))
            .subcommand(cl::App("update")
                .description("Refresh local registry clones")
                .arg(cl::Arg("name").help("If given, update only this index")))
            .subcommand(cl::App("pin")
                .description("Pin a custom index to a commit rev in mcpp.toml")
                .arg(cl::Arg("name").help("Index name").required())
                .arg(cl::Arg("rev").help("Commit sha (defaults to current lock rev)")))
            .subcommand(cl::App("unpin")
                .description("Remove rev pin from a custom index in mcpp.toml")
                .arg(cl::Arg("name").help("Index name").required()))
            .action(wrap_rc([&dispatch_sub](const cl::ParsedArgs& p) {
                return dispatch_sub("index", p, {
                    {"list",   cmd_index_list},
                    {"add",    cmd_index_add},
                    {"remove", cmd_index_remove},
                    {"update", cmd_index_update},
                    {"pin",    cmd_index_pin},
                    {"unpin",  cmd_index_unpin},
                });
            })))

        // ─── about mcpp itself ─────────────────────────────────────────
        .subcommand(cl::App("self")
            .description("Inspect and manage mcpp itself")
            .subcommand(cl::App("init")
                .description("Initialize or repair mcpp sandbox")
                .option(cl::Option("force")
                    .help("Delete registry and re-initialize from scratch")))
            .subcommand(cl::App("doctor")
                .description("Diagnose mcpp environment health"))
            .subcommand(cl::App("env")
                .description("Print mcpp paths and configuration"))
            .subcommand(cl::App("config")
                .description("Show or modify mcpp's private xlings configuration")
                .option(cl::Option("mirror").takes_value().value_name("CN|GLOBAL")
                    .help("Set xlings mirror for mcpp's private registry")))
            .subcommand(cl::App("version")
                .description("Show mcpp version"))
            .subcommand(cl::App("explain")
                .description("Show extended description for an error code")
                .arg(cl::Arg("code").help("Error code such as E0001").required()))
            .action(wrap_rc([&dispatch_sub](const cl::ParsedArgs& p) {
                return dispatch_sub("self", p, {
                    {"init",      cmd_self_init},
                    {"doctor",    cmd_doctor},
                    {"env",       cmd_env},
                    {"config",    cmd_self_config},
                    {"version",   cmd_self_version},
                    {"explain",   cmd_explain_action},
                });
            })))

        // ─── top-level explain alias ──────────────────────────────────
        // Preserves `mcpp explain E0001` as a shortcut for
        // `mcpp self explain E0001`.
        .subcommand(cl::App("explain")
            .description("Show extended description for an error code")
            .arg(cl::Arg("code").help("Error code such as E0001").required())
            .action(wrap_rc(cmd_explain_action)))

        // ─── bareword `version` alias ─────────────────────────────────
        // cmdline natively handles `--help`/`--version`/`-h`; the bareword
        // `mcpp help` is intercepted by the pre-scan above (prints the
        // canonical usage screen). `mcpp version` is wired through the
        // App so it shows up in the auto-generated subcommand list.
        .subcommand(cl::App("version")
            .description("Show mcpp version")
            .action(wrap_rc(cmd_self_version)))

        // ─── hidden / internal ─────────────────────────────────────────
        .subcommand(cl::App("dyndep")
            .description("(internal: invoked by ninja) Emit ninja dyndep file from .ddi inputs")
            .option(cl::Option("output").short_name('o').takes_value().value_name("PATH")
                .help("Path to write dyndep file"))
            .option(cl::Option("single").help("Single-file mode: one .ddi → one .dd"))
            .option(cl::Option("bmi-dir").takes_value().value_name("DIR")
                .help("BMI cache directory name (default: gcm.cache)"))
            .option(cl::Option("bmi-ext").takes_value().value_name("EXT")
                .help("BMI file extension (default: .gcm)"))
            .action(wrap_rc(cmd_dyndep)))
    ;

    // The bareword `mcpp help` and `mcpp` (no args) both print the
    // canonical help screen.
    if (argc <= 1) {
        print_usage();
        return 0;
    }
    if (std::string_view(argv[1]) == "help") {
        print_usage();
        return 0;
    }

    // Legacy `--explain CODE` form (still tested by e2e #22). cmdline
    // wouldn't naturally accept this as a top-level option taking a
    // value (it'd require declaring it on the root App, but then it'd
    // also need to coexist with the `explain` subcommand, which is
    // the form documented going forward). Special-case here before
    // invoking the App.
    if (std::string_view(argv[1]) == "--explain") {
        if (argc < 3) {
            std::println(stderr, "error: --explain requires an error code (e.g. E0001)");
            return 2;
        }
        return cmd_explain(argv[2]);
    }

    // Unknown-command pre-check. cmdline doesn't error on an unknown
    // top-level word — it just treats it as a positional and returns
    // 0 silently (since the root App has no top-level action). The
    // pre-existing CLI returned 127 for "unknown command", and e2e
    // #01 asserts that exit code, so reproduce it here.
    {
        std::string_view first = argv[1];
        if (!first.starts_with('-')) {
            static constexpr std::array<std::string_view, 21> known = {
                "new", "build", "run", "test", "clean", "add", "remove",
                "update", "search", "publish", "pack", "emit",
                "toolchain", "cache", "index", "self", "explain",
                "version", "dyndep", "why", "resolve",
            };
            bool ok = false;
            for (auto k : known) if (k == first) { ok = true; break; }
            if (!ok) {
                std::println(stderr, "error: unknown command '{}'", first);
                print_usage();
                return 127;
            }
        }
    }

    auto app_rc = app.run(trimmed_argc, trimmed_argp);
    if (app_rc != 0) return app_rc;
    return action_rc;
}

} // namespace mcpp::cli
