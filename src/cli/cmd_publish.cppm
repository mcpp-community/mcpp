// mcpp.cli.cmd_publish — CLI parsing + routing for publish / pack /
// emit xpkg. Implementations live in mcpp.publish.pipeline and mcpp.pack.pipeline.

module;
#include <cstdio>
#include <cstdlib>

export module mcpp.cli.cmd_publish;

import std;
import mcpplibs.cmdline;
import mcpp.pack;
import mcpp.pack.library_pipeline;
import mcpp.pack.pipeline;
import mcpp.pack.route;
import mcpp.publish.pipeline;
import mcpp.ui;

namespace mcpp::cli {

// `mcpp emit xpkg ...` — only one subcommand defined, so the action sits
// directly on the `emit xpkg` nested subcommand and receives its ParsedArgs.
export int cmd_emit_xpkg(const mcpplibs::cmdline::ParsedArgs& parsed) {
    return mcpp::publish::emit_xpkg_to(
        parsed.option_or_empty("version").value(),
        std::filesystem::path{parsed.option_or_empty("output").value()},
        parsed.option_or_empty("namespace").value());
}

export int cmd_publish(const mcpplibs::cmdline::ParsedArgs& parsed) {
    return mcpp::publish::publish_package(
        parsed.is_flag_set("dry-run"), parsed.is_flag_set("allow-dirty"));
}

export int cmd_pack(const mcpplibs::cmdline::ParsedArgs& parsed) {
    // ─── Resolve mode ────────────────────────────────────────────────
    mcpp::pack::Options opts;
    bool modeFromUser = false;
    if (auto v = parsed.value("mode")) {
        auto m = mcpp::pack::parse_mode(*v);
        if (!m) {
            mcpp::ui::error(std::format(
                "invalid --mode '{}'; expected: system | vendored | self-contained | static "
                "(aliases: bundle-project=vendored, bundle-all=self-contained)", *v));
            return 2;
        }
        opts.mode = *m;
        modeFromUser = true;
    }
    if (auto v = parsed.value("format")) {
        if (*v == "tar")      opts.format = mcpp::pack::Format::Tar;
        else if (*v == "dir") opts.format = mcpp::pack::Format::Dir;
        else {
            mcpp::ui::error(std::format(
                "invalid --format '{}'; expected tar | dir", *v));
            return 2;
        }
    }
    if (auto v = parsed.value("output")) opts.output = *v;

    // `value()`, not `option_or_empty()`, for `profile` — and NOT for
    // anything whose name a positional shares. `ParsedArgs::value()` falls back
    // to a same-named positional when the option is unset, which is how
    // `mcpp run q` once became `--target=q`. `pack`'s positional is `target`,
    // so `--target` is read through `option()` above and stays unaffected;
    // `profile` and `debug-symbols` have no positional twin.
    if (auto v = parsed.value("profile")) opts.profile = *v;
    if (parsed.is_flag_set("no-strip")) opts.strip = false;
    if (auto v = parsed.value("debug-symbols")) opts.debugSymbols = *v;

    // `--target` is repeatable: one leg per triple, which is how a library
    // package ships for several targets at once. The application path has
    // always taken exactly one, and still does — packing one executable for
    // several triples would need several executables.
    std::vector<std::string> triples;
    if (auto o = parsed.option("target")) triples = o->get().values;
    if (!triples.empty()) opts.targetTriple = triples.back();

    // ─── Which target, and therefore which kind of package ───────────
    //
    // The positional is a target NAME. Its `kind` decides everything: a
    // program becomes an application bundle (the four --mode depths), a
    // library becomes a library package. Reading the answer out of the
    // manifest is the whole reason there is no --lib flag.
    auto route = mcpp::pack::route_pack_target(parsed.positional(0));
    if (!route) { mcpp::ui::error(route.error()); return 2; }
    if (route->library) {
        if (modeFromUser) {
            mcpp::ui::warning(std::format(
                "--mode is an application-bundle depth and does not apply to the "
                "library target '{}' yet; ignoring it", route->targetName));
        }
        return mcpp::pack::build_and_pack_library(route->targetName, triples, opts);
    }
    if (triples.size() > 1) {
        mcpp::ui::error(
            "--target may be given once when packing a program: an application "
            "bundle wraps one executable, and one executable has one target.");
        return 2;
    }
    return mcpp::pack::build_and_pack(std::move(opts), modeFromUser, route->targetName);
}

} // namespace mcpp::cli
