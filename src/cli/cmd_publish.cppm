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
    // THE VALUE IS NOT VALIDATED HERE, AND THAT IS THE CHANGE.
    //
    // `tar` and `dir` are the archive shapes the engine owns. Everything else
    // is a name a package provides, and which names those are is a property of
    // the RESOLVED GRAPH -- so a refusal written here could only compare
    // against a constant, which is exactly the coupling this whole mechanism
    // exists to remove. The refusal moves to `build_and_pack`, after build
    // programs have declared what they provide and before anything is
    // compiled, where it can name what IS available instead of a fixed list.
    if (auto v = parsed.value("format")) {
        if (*v == "tar")      opts.format = mcpp::pack::Format::Tar;
        else if (*v == "dir") opts.format = mcpp::pack::Format::Dir;
        else if (v->empty()) {
            mcpp::ui::error("--format needs a value: tar | dir | a format the "
                            "resolved graph provides");
            return 2;
        } else {
            opts.format     = mcpp::pack::Format::Dispatched;
            opts.formatName = *v;
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
        // A DISPATCHED FORMAT IS AN APPLICATION-BUNDLE OUTPUT, and a library
        // package has no bundle: `mcpp pack <lib>` produces an interface plus
        // prebuilt binaries for one or more triples, and there is no single
        // staged tree for a member to turn into an installer. Refused rather
        // than ignored, because ignoring it would report `Packed` and hand back
        // a library package while the user asked for an installer.
        if (opts.format == mcpp::pack::Format::Dispatched) {
            mcpp::ui::error(std::format(
                "--format {} is a distributable produced from a program's staged "
                "bundle, and '{}' is a library target.\n"
                "  A library package ships an interface plus prebuilt binaries "
                "per triple; there is no\n"
                "  single staged tree to hand a distribution format.\n"
                "  use: --format tar | dir, or name a program target",
                opts.formatName, route->targetName));
            return 2;
        }
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
    // #622 A10: `build_and_pack` now reports the artifact(s) it packed, for
    // `mcpp run --format` to take as its operand -- `mcpp pack` itself only
    // ever needed the exit code.
    return mcpp::pack::build_and_pack(std::move(opts), modeFromUser, route->targetName).rc;
}

} // namespace mcpp::cli
