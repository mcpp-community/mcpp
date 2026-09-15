// mcpp.cli.cmd_publish — CLI parsing + routing for publish / pack /
// emit xpkg. Implementations live in mcpp.publish.pipeline and mcpp.pack.pipeline.

module;
#include <cstdio>
#include <cstdlib>

export module mcpp.cli.cmd_publish;

import std;
import mcpplibs.cmdline;
import mcpp.build.prepare;   // profile_override_from_flags
import mcpp.libs.json;
import mcpp.pack;
import mcpp.pack.library_pipeline;
import mcpp.pack.pipeline;
import mcpp.pack.route;
import mcpp.platform.terminal;
import mcpp.publish.pipeline;
import mcpp.ui;
import mcpp.wire;

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

namespace {

int cmd_pack_body(const mcpplibs::cmdline::ParsedArgs& parsed,
                  mcpp::pack::PackOutcome* report,
                  mcpp::pack::LibraryPackReport* libraryReport,
                  bool* libraryRoute);

} // namespace

// `mcpp pack --message-format json` (#649 E9).
//
// ONE ENVELOPE ON STDOUT, AND EVERYTHING ELSE ON STDERR. `--format` names the
// PACKAGE format on this command, so machine output is asked for the way
// `mcpp test` asks for it, and the document is printed after the command has
// finished, alone: planning, building and packing narrate, and so do the
// programs they start, which is why the whole run is under `StdoutToStderr`,
// as `emit build-database`'s planning is.
//
// `data.artifacts` holds what the human `Packed` lines name, as absolute paths:
// the archive or tree of a built-in format, the terminal outputs of a
// dispatched one, or the library package. A leg is a `targets` entry of the
// artifact it went into, not an artifact of its own.
export int cmd_pack(const mcpplibs::cmdline::ParsedArgs& parsed) {
    std::string messageFormat = "human";
    if (auto mf = parsed.value("message-format")) messageFormat = *mf;
    if (messageFormat != "human" && messageFormat != "json") {
        mcpp::ui::error(std::format("unknown --message-format '{}' (human|json)",
                                    messageFormat));
        return 2;
    }
    if (messageFormat == "human")
        return cmd_pack_body(parsed, nullptr, nullptr, nullptr);

    mcpp::pack::PackOutcome outcome;
    mcpp::pack::LibraryPackReport library;
    bool libraryRoute = false;
    int rc = 0;
    {
        mcpp::platform::terminal::StdoutToStderr narration;
        rc = cmd_pack_body(parsed, &outcome, &library, &libraryRoute);
    }

    using mcpp::wire::Effect;
    std::vector<Effect> effects{Effect::ReadProject, Effect::WriteProject,
                                Effect::WriteGlobalCache};
    if (outcome.ranBuildPrograms) effects.push_back(Effect::ExecBuildScript);
    mcpp::wire::Envelope env{ .kind = "mcpp.pack", .effects = std::move(effects) };
    if (rc != 0) {
        env.data = nullptr;
        env.diagnostics.push_back({"MCPP_PACK_FAILED", mcpp::wire::Severity::Error,
            "mcpp pack did not produce a package; the reason is on standard error"});
        mcpp::wire::emit(env);
        return rc;
    }

    auto artifact_json = [](const std::filesystem::path& p, std::string const& format,
                            std::vector<std::string> const& targets) {
        std::error_code ec;
        nlohmann::json t = nlohmann::json::array();
        for (auto const& x : targets) t.push_back(x);
        return nlohmann::json{
            {"path", std::filesystem::absolute(p, ec).lexically_normal().string()},
            {"type", std::filesystem::is_directory(p, ec) ? "directory" : "file"},
            {"format", format},
            {"targets", std::move(t)},
        };
    };
    nlohmann::json artifacts = nlohmann::json::array();
    nlohmann::json stage = nullptr;
    if (libraryRoute) {
        const auto fmt = parsed.value("format").value_or("tar");
        artifacts.push_back(artifact_json(library.artifact, fmt, library.targets));
    } else {
        for (auto const& a : outcome.artifacts)
            artifacts.push_back(artifact_json(a, outcome.format, outcome.targets));
        if (!outcome.stageDir.empty())
            stage = nlohmann::json{
                {"dir", outcome.stageDir.lexically_normal().string()},
                {"manifest", outcome.stageManifest.lexically_normal().string()},
                {"closure", outcome.closure},
            };
    }
    env.data = nlohmann::json{{"artifacts", std::move(artifacts)}, {"stage", std::move(stage)}};
    mcpp::wire::emit(env);
    return 0;
}

namespace {

int cmd_pack_body(const mcpplibs::cmdline::ParsedArgs& parsed,
                  mcpp::pack::PackOutcome* report,
                  mcpp::pack::LibraryPackReport* libraryReport,
                  bool* libraryRoute) {
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
    // `--profile` > `--release` / `--dev`, the rule `build` and `run` follow.
    opts.profile = mcpp::build::profile_override_from_flags(
        parsed.value("profile").value_or(""),
        parsed.is_flag_set("release"), parsed.is_flag_set("dev"));
    if (parsed.is_flag_set("no-strip")) opts.strip = false;
    if (auto v = parsed.value("debug-symbols")) opts.debugSymbols = *v;
    if (auto v = parsed.value("features")) opts.features = *v;

    // `--target` is repeatable: one leg per triple, which is how a library
    // package ships for several targets at once. The application path takes
    // exactly one triple UNLESS the target is a `kind = "app"` whose form is
    // a shared object on every requested row (#630 A9, below) — packing an
    // executable for several triples would need several executables, and
    // that refusal still stands for a `bin` target and for any row whose
    // form is not a shared object.
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
        if (libraryRoute) *libraryRoute = true;
        return mcpp::pack::build_and_pack_library(route->targetName, triples, opts,
                                                  libraryReport);
    }
    if (triples.size() > 1) {
        // #630 A9: THE ROUTE IS CHOSEN BY THE ARTIFACT'S FORM, NOT BY THE
        // TARGET'S KIND. An `app` whose resolved link form is a shared
        // object on EVERY requested row (Android) takes the several-triple
        // path a library target already has: the other legs are built first
        // (`build_extra_android_legs`, the same leg-loop shape
        // `build_and_pack_library` uses) and staged as `lib/<abi>/` beside
        // the primary leg's own `lib/<abi>/`, into ONE tree, behind ONE
        // dispatch. A `bin` target, or any row whose form is an executable,
        // keeps today's refusal — packing an executable for several triples
        // would need several executables, and there is no "universal
        // executable" mechanism the way there is a universal shared object
        // (that is `lipo`, a different tool, and out of scope here).
        if (!mcpp::pack::accepts_several_targets(*route, triples)) {
            mcpp::ui::error(
                "--target may be given once when packing a program: an application "
                "bundle wraps one executable, and one executable has one target.");
            return 2;
        }
        auto extraLegs = mcpp::pack::build_extra_android_legs(
            route->targetName,
            std::span<const std::string>(triples).first(triples.size() - 1),
            opts.profile, opts.features);
        if (!extraLegs) return 1;   // build_extra_android_legs already printed why
        // #622 A10: `build_and_pack` now reports the artifact(s) it packed, for
        // `mcpp run --format` to take as its operand -- `mcpp pack` itself only
        // ever needed the exit code.
        auto out = mcpp::pack::build_and_pack(std::move(opts), modeFromUser,
                                              route->targetName, std::move(*extraLegs));
        if (report) *report = out;
        return out.rc;
    }
    // #622 A10: `build_and_pack` now reports the artifact(s) it packed, for
    // `mcpp run --format` to take as its operand -- `mcpp pack` itself only
    // ever needed the exit code.
    auto out = mcpp::pack::build_and_pack(std::move(opts), modeFromUser, route->targetName);
    if (report) *report = out;
    return out.rc;
}

} // namespace

} // namespace mcpp::cli
