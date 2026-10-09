// mcpp.cli.cmd_publish — CLI parsing + routing for publish / pack /
// emit xpkg. Implementations live in mcpp.publish.pipeline and mcpp.pack.pipeline.

module;
#include <cstdio>
#include <cstdlib>

export module mcpp.cli.cmd_publish;

import std;
import mcpp.build.advice;
import mcpplibs.cmdline;
import mcpp.build.prepare;   // profile_override_from_flags
import mcpp.build.progress;  // the build's report (build progress design 2026-09-29)
import mcpp.cli.selection;    // which members `pack` acts on, as `build` plans them
import mcpp.log;
import mcpp.libs.json;
import mcpp.pack;
import mcpp.pack.library_pipeline;
import mcpp.pack.pipeline;
import mcpp.pack.route;
import mcpp.platform.terminal;
import mcpp.project;          // resolve_member_dir, for `pack -p` (#734 E3)
import mcpp.manifest;
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

// `mcpp place-dlls --output <stamp> --depfile <d> <program> <dir>...`
// -- the edge that follows a Windows program's link when its plan has runtime
// search directories (mcpp.pack's `place_runtime_dlls`, SPEC-007 R4.3).
// Internal: only a generated build.ninja names it, and it runs on whatever
// host builds, because it reads the program's import table rather than asking
// a loader.
//
// The command line names no DLL. Which DLLs beside the program belong to
// another writer (a declared deploy, the toolchain's staged runtime) is
// decided below, from the program's directory, the stamp and the search
// directories, so that the command does not change when the plan's deploy set
// does (SPEC-007 R4.2/R4.3).
//
// The depfile names every DLL placed, so ninja runs the edge again when one of
// them changes in its directory; the stamp is the edge's only declared output,
// because the DLL names are not known when the graph is written, and it holds
// the names placed, one per line, for the next run.
export int cmd_place_dlls(const mcpplibs::cmdline::ParsedArgs& parsed) {
    const std::filesystem::path stamp{parsed.option_or_empty("output").value()};
    const std::filesystem::path depfile{parsed.option_or_empty("depfile").value()};
    if (stamp.empty() || depfile.empty() || parsed.positional_count() < 1) {
        std::println(stderr,
            "error: place-dlls requires --output, --depfile and a program");
        return 2;
    }
    const std::filesystem::path program{parsed.positional(0)};
    std::vector<std::filesystem::path> dirs;
    for (std::size_t i = 1; i < parsed.positional_count(); ++i)
        dirs.emplace_back(parsed.positional(i));

    // What the previous run placed, recorded in the stamp itself: those copies
    // are this edge's, not the program's, and resolve again from their
    // directories (see `place_runtime_dlls`).
    std::vector<std::string> placedBefore;
    {
        std::ifstream prev(stamp);
        for (std::string line; std::getline(prev, line);)
            if (!line.empty()) placedBefore.push_back(line);
    }
    // ONE DESTINATION, ONE WRITER (SPEC-007 R4.3, #723). A DLL already beside
    // the program that this edge did not place, and that a runtime search
    // directory also offers, is another writer's: a declared deploy or the
    // toolchain's staged runtime, both completed before the link this edge
    // follows. It is never overwritten; `place_runtime_dlls` compares it with
    // the directory's copy and warns on a difference. A DLL only the
    // program's directory holds (a library the project built there) is not
    // one this edge could write, and stays an ordinary member of the closure.
    // Decided here, from the directories, so the edge's command does not
    // change when the plan's deploy set does.
    std::vector<std::string> placedByOthers;
    {
        std::error_code dirEc;
        const auto here = program.has_parent_path() ? program.parent_path()
                                                    : std::filesystem::path(".");
        for (auto const& e : std::filesystem::directory_iterator(here, dirEc)) {
            if (!e.is_regular_file(dirEc)) continue;
            auto ext = e.path().extension().string();
            std::ranges::transform(ext, ext.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (ext != ".dll") continue;
            const auto name = e.path().filename().string();
            if (std::ranges::find(placedBefore, name) != placedBefore.end()) continue;
            const bool offered = std::ranges::any_of(dirs, [&](auto const& d) {
                std::error_code fe;
                return std::filesystem::is_regular_file(d / name, fe);
            });
            if (offered) placedByOthers.push_back(name);
        }
    }
    // The rule the plan's resolver applied to the MSVC C++ runtime's names
    // (mcpp.build.runtime_placement), and the toolset's runtime directory. A
    // graph written before these options existed passes neither, and gets
    // the search-order placement it was written for.
    mcpp::pack::RuntimeCrtRule crtRule;
    if (auto v = parsed.option_or_empty("crt").value(); !v.empty()) crtRule.policy = v;
    if (auto v = parsed.option_or_empty("toolset-crt").value(); !v.empty())
        crtRule.toolsetCrtDir = std::filesystem::path{v};
    auto placed = mcpp::pack::place_runtime_dlls(program, dirs, placedBefore, placedByOthers,
                                                 crtRule);
    if (!placed) {
        std::println(stderr, "error: {}", placed.error().message);
        return 1;
    }
    // What the placement has to say on success goes to the edge-advice
    // channel (mcpp.build.advice), which mcpp reports after a successful
    // build without `-v`; printed here, it reached nobody (WS3).
    {
        std::vector<mcpp::build::advice::Line> lines;
        for (auto const& n : placed->notes)
            lines.push_back({mcpp::build::advice::Kind::Note, n});
        for (auto const& w : placed->warnings)
            lines.push_back({mcpp::build::advice::Kind::Warning, w});
        mcpp::build::advice::write(stamp, lines);
    }

    // The depfile syntax ninja reads (`deps = gcc`): a space and `#` are
    // escaped with a backslash, and `$` is doubled.
    auto dep_word = [](const std::filesystem::path& p) {
        std::string out;
        for (char c : p.generic_string()) {
            if (c == ' ' || c == '#') out.push_back('\\');
            if (c == '$') out.push_back('$');
            out.push_back(c);
        }
        return out;
    };
    std::error_code ec;
    if (depfile.has_parent_path())
        std::filesystem::create_directories(depfile.parent_path(), ec);
    {
        std::ofstream d(depfile, std::ios::trunc);
        d << dep_word(stamp) << ':';
        for (auto const& src : placed->sources) d << " \\\n  " << dep_word(src);
        d << '\n';
        if (!d) {
            std::println(stderr, "error: cannot write '{}'", depfile.string());
            return 1;
        }
    }
    std::ofstream st(stamp, std::ios::trunc);
    for (auto const& n : placed->names) st << n << '\n';
    if (!st) {
        std::println(stderr, "error: cannot write '{}'", stamp.string());
        return 1;
    }
    return 0;
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
    if (messageFormat == "human") {
        // The build is reported as `mcpp build` reports it; `Finished` closes
        // the report before the pack's own lines.
        mcpp::build::progress::open(mcpp::log::is_verbose());
        return cmd_pack_body(parsed, nullptr, nullptr, nullptr);
    }

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
        // A pack of several members names each member that failed, which the
        // code above cannot: the others may have been packed.
        if (outcome.members.size() > 1)
            for (auto const& m : outcome.members)
                if (m.rc != 0)
                    env.diagnostics.push_back({"MCPP_PACK_FAILED", mcpp::wire::Severity::Error,
                        std::format("mcpp pack did not produce a package for member '{}'; "
                                    "the reason is on standard error", m.name)});
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
    nlohmann::json stages = nullptr;
    if (libraryRoute) {
        const auto fmt = parsed.value("format").value_or("tar");
        artifacts.push_back(artifact_json(library.artifact, fmt, library.targets));
    } else if (outcome.members.size() > 1) {
        // Several members (member selection design 2026-09-30, K1). Fields are
        // added and none is redefined (docs/50 §7): every member's artifacts
        // are listed, each naming its member, and `stage`, which names one
        // tree, stays null while `stages` names one per member.
        stages = nlohmann::json::array();
        for (auto const& m : outcome.members) {
            for (auto const& a : m.artifacts) {
                auto j = artifact_json(a, m.format, m.targets);
                j["member"] = m.name;
                artifacts.push_back(std::move(j));
            }
            if (!m.stageDir.empty())
                stages.push_back(nlohmann::json{
                    {"member", m.name},
                    {"dir", m.stageDir.lexically_normal().string()},
                    {"manifest", m.stageManifest.lexically_normal().string()},
                    {"closure", m.closure},
                });
        }
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
    if (!stages.is_null()) env.data["stages"] = std::move(stages);
    mcpp::wire::emit(env);
    return 0;
}

namespace {

// The members `mcpp pack` packs, from the selectors of its command line (member
// selection design 2026-09-30, K1).
//
// A command without a selector acts on the package of its directory, as it
// always has, and so does a selection of one member: `-p X` packs X as if the
// command ran in X's directory, which is what the command does. Several members
// are planned and built once, each in its own stage. For `pack`, "every member"
// means every member with a program target to pack: `--workspace` skips a member
// that has none, and `-p` names a member that has none in a refusal.
struct PackMembers {
    // Set when several members are packed; null for one, which is the package of
    // the directory the command runs in, entered when `-p` named it.
    std::optional<mcpp::pack::MemberPack> several;
    int                                   rc = 0;   // non-zero: refused, and reported
};

PackMembers pack_members(const mcpplibs::cmdline::ParsedArgs& parsed) {
    PackMembers out;
    const auto request = mcpp::cli::member_request(parsed);
    auto selection = mcpp::cli::select_members(request);
    if (!selection) {
        mcpp::ui::error(selection.error());
        out.rc = 2;
        return out;
    }
    if (!*selection) {
        // Outside a workspace there is nothing for `-p` to name.
        if (!request.packages.empty()) {
            auto root = mcpp::project::find_manifest_root(std::filesystem::current_path());
            if (!root) {
                mcpp::ui::error("-p needs a workspace; no mcpp.toml was found here or above");
            } else if (auto rm = mcpp::manifest::load(*root / "mcpp.toml"); !rm) {
                mcpp::ui::error(rm.error().format());
            } else {
                mcpp::ui::error(std::format("-p {}: {} is not a workspace",
                                            request.packages.front(), root->string()));
            }
            out.rc = 2;
        }
        return out;
    }
    const auto& sel = **selection;
    const bool selected = request.all || !request.packages.empty() || !request.excludes.empty();
    if (!selected) return out;

    // The members with a program to pack.
    auto ws = mcpp::manifest::load(sel.root / "mcpp.toml");
    if (!ws) {
        mcpp::ui::error(ws.error().format());
        out.rc = 2;
        return out;
    }
    std::vector<std::string> packable;
    for (auto const& mp : sel.members) {
        auto mm = mcpp::project::load_member_manifest(*ws, sel.root, mp);
        if (!mm) {
            mcpp::ui::error(mm.error());
            out.rc = 2;
            return out;
        }
        if (std::ranges::any_of(mm->targets, [](auto const& t) { return t.is_program(); })) {
            packable.push_back(mp);
            continue;
        }
        // One member named is packed as it always was, a library package
        // included; with several, a member that has no program is skipped when
        // the selection is every member, and refused by name when `-p` named it.
        if (sel.members.size() == 1) packable.push_back(mp);
        else if (sel.whole)
            mcpp::ui::info("Skipping", std::format(
                "{}: no program target to pack", mm->package.name));
        else {
            mcpp::ui::error(std::format(
                "member '{}' has no program target to pack: `mcpp pack` over several members "
                "packs programs.\n  Pack it alone with `-p {}`, or leave it out.",
                mm->package.name, mp));
            out.rc = 2;
            return out;
        }
    }
    if (packable.empty()) {
        mcpp::ui::error("no workspace member has a program target to pack");
        out.rc = 2;
        return out;
    }

    if (packable.size() == 1) {
        // One member: the package of its directory, as if the command ran there.
        // A relative `--output` keeps meaning the directory the user typed it in
        // (the caller resolves it before this changes the directory).
        std::error_code ec;
        std::filesystem::current_path(sel.root / packable.front(), ec);
        if (ec) {
            mcpp::ui::error(std::format("-p {}: cannot enter {}: {}", packable.front(),
                                        (sel.root / packable.front()).string(), ec.message()));
            out.rc = 2;
        }
        return out;
    }
    auto groups = mcpp::cli::workspace_groups(sel.root, packable, {});
    if (!groups) {
        mcpp::ui::error(groups.error());
        out.rc = 2;
        return out;
    }
    out.several = mcpp::pack::MemberPack{sel.root, std::move(*groups), {}, packable};
    return out;
}

int cmd_pack_body(const mcpplibs::cmdline::ParsedArgs& parsed,
                  mcpp::pack::PackOutcome* report,
                  mcpp::pack::LibraryPackReport* libraryReport,
                  bool* libraryRoute) {
    // `-p <member>` (#734 E3): the member is resolved by the resolver every
    // other `-p` uses, and the pack then runs in its directory, so the result
    // is by construction the one `mcpp pack` in that directory produces. A
    // relative `--output` keeps meaning the directory the user typed it in.
    std::optional<std::filesystem::path> typedOutput;
    if (auto v = parsed.value("output")) typedOutput = std::filesystem::absolute(*v);
    const auto startedIn = std::filesystem::current_path();
    auto members = pack_members(parsed);
    if (members.rc != 0) return members.rc;
    std::optional<std::filesystem::path> outputFromUser;
    if (typedOutput && std::filesystem::current_path() != startedIn)
        outputFromUser = typedOutput;
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
    if (auto v = parsed.value("output")) opts.output = outputFromUser ? outputFromUser->string() : *v;

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

    // ─── Several members ─────────────────────────────────────────────
    //
    // Each is refused before anything is planned, let alone compiled: the
    // command names one thing where several members are packed, and the
    // members cannot share it.
    if (members.several) {
        if (const auto name = parsed.positional(0); !name.empty()) {
            mcpp::ui::error(std::format(
                "a target name cannot be given when several members are packed: '{}' names "
                "a target of one package.\n"
                "  use: -p <member> {}, or leave the name out to pack each member's program",
                name, name));
            return 2;
        }
        if (triples.size() > 1) {
            mcpp::ui::error(
                "--target may be given once when several members are packed: each member's "
                "program is built for one target,\n"
                "  and a pack for several targets stages one member's legs into one tree.\n"
                "  use: pack that member alone with -p <member>");
            return 2;
        }
        opts.output.clear();
        if (typedOutput) {
            std::error_code ec;
            if (std::filesystem::exists(*typedOutput, ec)
                && !std::filesystem::is_directory(*typedOutput, ec)) {
                mcpp::ui::error(std::format(
                    "--output names a file, '{}', and several members are packed.\n"
                    "  Each member's archive or tree is written below the directory --output "
                    "names, under the name\n"
                    "  it has by default. use: --output <directory>", typedOutput->string()));
                return 2;
            }
            members.several->outputDir = *typedOutput;
        }
        auto out = mcpp::pack::build_and_pack_members(std::move(opts), modeFromUser,
                                                      *members.several);
        if (report) *report = out;
        return out.rc;
    }

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
