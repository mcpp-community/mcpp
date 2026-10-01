// mcpp.pack.pipeline — pack orchestration: the plan of each configuration group
// of the members being packed (re-preparing for musl static when needed), what is
// refused before anything is compiled, one build per group, then for each member
// its program, its plan and the bundler, and one dispatch pass per group. The
// package of one directory is a group of one member, and runs the same steps.

module;
#include <cstdio>
#include <cstdlib>

export module mcpp.pack.pipeline;

import std;
import mcpp.build.runtime_placement;
import mcpp.build.prepare;
import mcpp.build.backend;
import mcpp.build.distribution;
import mcpp.build.flags;
import mcpp.build.ninja;
import mcpp.build.plan;
import mcpp.build.progress;
import mcpp.config;
import mcpp.fetcher.progress;
import mcpp.manifest;
import mcpp.pack;
import mcpp.pack.stage_tree;
import mcpp.pack.strip;
import mcpp.platform;
import mcpp.platform.env;
import mcpp.toolchain.model;
import mcpp.toolchain.probe;
import mcpp.toolchain.registry;
import mcpp.toolchain.triple;
import mcpp.log;
import mcpp.ui;
import mcpp.xlings;

namespace mcpp::pack {

// #622 A10: the CLI exit code, plus the artifact(s) this pass reported as
// "Packed" -- the staged tree or archive for a built-in format, or the
// distributable(s) a DISPATCHED format's provider submitted for THIS
// request. `mcpp run --format <name>` takes `artifacts.front()` as its run
// operand rather than re-deriving it: the reported path IS the thing rule 3
// ("declare unconditionally, submit conditionally") makes unambiguous, and a
// second derivation is a second place for it to disagree. Empty whenever
// `rc != 0`.
export struct PackOutcome {
    int                                 rc = 0;
    std::vector<std::filesystem::path> artifacts;
    // #649 E9: what `mcpp pack --message-format json` reports beside the
    // artifacts. All of it was already answered by the pass that produced
    // them; nothing here is derived for the report.
    //
    // `format` is the `--format` value as requested (`tar` when omitted);
    // `targets` the canonical triple of every leg, the primary first. The
    // stage fields are empty when no tree was staged; `closure` is the stage
    // manifest's own word, `walked` or `not-walked`.
    std::string                         format;
    std::vector<std::string>            targets;
    std::filesystem::path               stageDir;
    std::filesystem::path               stageManifest;
    std::string                         closure;
    // Whether a build program ran in this pack, for the envelope's `effects`.
    bool                                ranBuildPrograms = false;
    // What one packed member answered (member selection design 2026-09-30,
    // K1). A pack of several members reports each in member order, and the
    // fields above then describe the pack as a whole: `artifacts` holds every
    // member's, and the stage fields, which name one tree, are the first
    // member's. A pack of one member holds that member here as well.
    struct Member {
        std::string                         name;   // qualified package name
        int                                 rc = 0;
        std::vector<std::filesystem::path>  artifacts;
        std::string                         format;
        std::vector<std::string>            targets;
        std::filesystem::path               stageDir;
        std::filesystem::path               stageManifest;
        std::string                         closure;
    };
    std::vector<Member>                 members;
};

// #634 A3: the two directory lists an Android row's closure is read against,
// asked of the row's own driver rather than derived from the NDK's layout:
// where the link searches for a library (`-print-search-dirs`), and the
// directory the driver finds bionic's `libc.so` in, which is the API level's
// stub directory -- the set of names the device itself provides.
//
// A driver that answers neither leaves both lists empty; `pack::run` then
// reports that it cannot tell the device's libraries from the program's rather
// than staging stubs.
struct DriverLibraryDirs {
    std::vector<std::filesystem::path> search;
    std::vector<std::filesystem::path> platform;
};

DriverLibraryDirs driver_library_dirs(const mcpp::toolchain::Toolchain& tc)
{
    DriverLibraryDirs out;
    const auto base = std::format("{} {}", mcpp::xlings::shq(tc.binaryPath.string()),
                                  tc.crossTargetFlag);
    if (auto r = mcpp::toolchain::run_capture(std::format(
            "{} -print-search-dirs {}", base, mcpp::platform::null_redirect))) {
        std::istringstream is{*r};
        std::string line;
        const std::string_view key = "libraries: =";
        const auto sep = mcpp::platform::env::path_list_separator();
        while (std::getline(is, line)) {
            if (!line.starts_with(key)) continue;
            std::string rest = mcpp::toolchain::trim_line(line.substr(key.size()));
            std::size_t at = 0;
            while (at <= rest.size()) {
                auto next = rest.find(sep, at);
                if (next == std::string::npos) next = rest.size();
                if (next > at)
                    out.search.push_back(
                        std::filesystem::path(rest.substr(at, next - at)).lexically_normal());
                at = next + sep.size();
            }
        }
    }
    if (auto r = mcpp::toolchain::run_capture(std::format(
            "{} -print-file-name=libc.so {}", base, mcpp::platform::null_redirect))) {
        // A driver that cannot place the library echoes the bare name back.
        std::filesystem::path lib(mcpp::toolchain::trim_line(*r));
        std::error_code ec;
        if (lib.has_parent_path() && std::filesystem::is_regular_file(lib, ec))
            out.platform.push_back(lib.parent_path().lexically_normal());
    }
    return out;
}

// #630 A9: build every triple but the PRIMARY one for a `kind = "app"`
// target whose form is a shared object on every requested row (Android).
// Each leg is its own `prepare_build` + `ninja` build, the same shape
// `build_and_pack_library`'s leg loop uses and for the same reason: the
// artifact a leg contributes is the one THIS run made, never a glob over
// `target/` that could silently pick up a stale binary from an earlier
// build. The primary triple is not built here — it takes the ordinary
// single-triple `build_and_pack` path, which is what stages the tree,
// deploys the declared files once and runs the one dispatch pass; this
// function only produces the OTHER legs' bytes and where they belong.
//
// Returns `nullopt` on the first leg that fails, having already printed the
// error — the same contract `build_and_pack_library` and `build_and_pack`
// use, so `cmd_pack` only has to check the outcome and return.
export std::optional<std::vector<SharedLeg>>
build_extra_android_legs(const std::string& targetName,
                         std::span<const std::string> triples,
                         const std::string& profile,
                         const std::string& features)
{
    std::vector<SharedLeg> out;
    for (auto const& triple : triples) {
        mcpp::build::BuildOverrides ov;
        ov.target_triple    = triple;
        // A packaged artifact leaves this machine — same fallback as every
        // other pack leg (`build_and_pack_library`, and the primary leg
        // below).
        ov.profile          = profile;
        ov.profile_fallback = "release";
        ov.features         = features;
        auto ctx = mcpp::build::prepare_build(false, false, {}, ov);
        if (!ctx) { mcpp::ui::error(ctx.error()); return std::nullopt; }

        auto be = mcpp::build::make_ninja_backend();
        mcpp::build::progress::programs_done();
        mcpp::build::BuildOptions bo;
        if (auto br = be->build(ctx->plan, bo); !br) {
            mcpp::build::report_failed_drive(br.error());
            return std::nullopt;
        }
        mcpp::build::populate_dependency_cache(*ctx);

        // FROM THE PLAN, never a glob — see the function comment. A
        // dependency's own `shared` target also contributes a `SharedLibrary`
        // link unit to this plan, and `dependencyOwned` is what excludes it
        // (the same exclusion `pipeline.cppm`'s `is_program_link_unit` makes
        // for the primary leg).
        const mcpp::build::LinkUnit* lu = nullptr;
        for (auto const& u : ctx->plan.linkUnits)
            if (u.targetName == targetName
                && u.kind == mcpp::build::LinkUnit::SharedLibrary
                && !u.dependencyOwned) { lu = &u; break; }
        if (!lu) {
            mcpp::ui::error(std::format(
                "target '{}' produced no shared-object artifact for {}.\n"
                "  Every leg of a several-`--target` app pack must resolve to "
                "the same shared-object\n"
                "  form the primary `--target` does; this row did not.",
                targetName, triple));
            return std::nullopt;
        }

        auto t = mcpp::toolchain::triple::parse(triple);
        auto canonical = t ? t->str() : triple;
        auto abi = t ? mcpp::toolchain::triple::android_abi(*t) : triple;
        mcpp::ui::status("Packed leg", std::format("{}  [{}]", canonical, abi));
        // #634 A3: where THIS triple's closure is read from -- the same
        // channels the primary leg's plan names (`build_and_pack` below), from
        // this leg's own plan and driver.
        SharedLeg leg;
        leg.abi      = std::move(abi);
        leg.triple   = canonical;
        leg.artifact = ctx->outputDir / lu->output;
        // #649 E5: what THIS leg's graph built, which pack strips as it strips
        // the program; see `Plan::graphSharedLibraries`.
        for (auto const& u : ctx->plan.linkUnits)
            if (u.kind == mcpp::build::LinkUnit::SharedLibrary)
                leg.graphSharedLibraries.push_back(ctx->outputDir / u.output);
        leg.searchDirs.push_back(leg.artifact.parent_path());
        for (auto const& d : ctx->plan.runtimeLibraryDirs) leg.searchDirs.push_back(d);
        for (auto const& d : ctx->plan.linkIntent.runtimeSearchDirs)
            leg.searchDirs.push_back(d);
        for (auto const& d : ctx->plan.linkIntent.linkLibraryDirs)
            leg.searchDirs.push_back(d);
        for (auto const& d : ctx->plan.linkIntent.transitiveNeededDirs)
            leg.searchDirs.push_back(d);
        auto dirs = driver_library_dirs(ctx->tc);
        for (auto const& d : dirs.search) leg.searchDirs.push_back(d);
        leg.platformDirs = std::move(dirs.platform);
        out.push_back(std::move(leg));
    }
    return out;
}

// The human report of what a dispatched format produced. A format may submit
// one output per file of a distribution tree (1,309 for one program of the
// validation project), and a line per file then buries the rest of the pass.
// Up to `kListedOutputs` outputs are named one per line; more are named by the
// entry each lies in below their common parent, with a count, and --verbose
// names every one. `--message-format json` lists every output in either case.
constexpr std::size_t kListedOutputs = 8;

void report_packed(const std::vector<std::filesystem::path>& outputs,
                   const mcpp::ui::PathContext& pathCtx) {
    if (outputs.size() <= kListedOutputs || mcpp::log::is_verbose()) {
        for (auto const& o : outputs)
            mcpp::ui::status("Packed", mcpp::ui::shorten_path(o, pathCtx));
        return;
    }
    auto parent = outputs.front().parent_path();
    for (auto const& o : outputs) {
        auto a = parent.begin(), b = o.begin();
        std::filesystem::path common;
        for (; a != parent.end() && b != o.end() && *a == *b; ++a, ++b) common /= *a;
        parent = common;
    }
    // Each entry, in the order first reached, with the outputs below it and
    // whether it is itself an output (then named alone).
    struct Entry { std::filesystem::path path; std::size_t outputs = 0; bool isOutput = false; };
    std::vector<Entry> entries;
    for (auto const& o : outputs) {
        // Outputs with no common parent (two drives) are named one by one.
        const auto rel = parent.empty() ? std::filesystem::path{} : o.lexically_relative(parent);
        const auto path = rel.empty() || rel == "." ? o : parent / *rel.begin();
        auto it = std::ranges::find(entries, path, &Entry::path);
        if (it == entries.end()) it = entries.insert(entries.end(), Entry{path});
        ++it->outputs;
        it->isOutput = it->isOutput || path == o;
    }
    for (auto const& e : entries)
        mcpp::ui::status("Packed", e.outputs == 1 && e.isOutput
            ? mcpp::ui::shorten_path(e.path, pathCtx)
            : std::format("{} ({} {})", mcpp::ui::shorten_path(e.path, pathCtx), e.outputs,
                          e.outputs == 1 ? "file" : "files"));
}

// What `mcpp pack` over several workspace members packs (member selection
// design 2026-09-30, K1), as the command selected it.
//
// The members are planned as the selection `mcpp build` plans: once per
// configuration group, one graph and one build for the members of a group, so
// a package that several members reach is compiled once and its build program
// runs once. Packing each member alone would plan the graph, run the build
// programs and start the build again for each (#749).
export struct MemberPack {
    // The workspace's root.
    std::filesystem::path                 root;
    // The packed members, each as `[workspace] members` spells it, by
    // configuration group: members whose root-position values are equal are
    // planned together. Every member has a program target to pack.
    std::vector<std::vector<std::string>> groups;
    // `--output` when several members are packed: the directory each member's
    // archive or tree is written below, under the name it would have by
    // default. Empty: each member's own `target/dist`.
    std::filesystem::path                 outputDir;
    // Every packed member, as `[workspace] members` spells it, in that order:
    // the order the members are reported in, whichever group each is in.
    std::vector<std::string>              order;
};

// One member of a pack, and what each step learned of it. A project that is not
// a workspace member has one, which is the plan's own package.
struct MemberJob {
    // The member's qualified package name.
    std::string                     name;
    // The member as `[workspace] members` spells it; empty outside a workspace.
    std::string                     path;
    // Whether the plan holds workspace members, and the member is read through
    // `with_member`. False for a plain project, whose plan is its own view.
    bool                            inWorkspace = false;
    // This member's request: the command's, with the mode the member's manifest
    // and the command line resolved to.
    Options                         request;
    bool                            modeFromUser = false;
    // The request with what the plan answered added to it (`member_plan`).
    Options                         opts;
    // The program the member packs, from the member's link units.
    std::filesystem::path           mainBinary;
    bool                            programIsSharedObject = false;
    std::optional<Plan>             plan;
    // Set only when NO tree exists at all; see `stage_member`.
    std::string                     stageFailure;
    ClosureStatus                   closure;
    mcpp::ui::PathContext           pathCtx;
    bool                            ranBuildPrograms = false;
    // What the member's pack answered: its status, artifacts and stage.
    PackOutcome::Member             result;
};

// One configuration group: the plan of its members, what produced the plan,
// and the members. A plan of one package, or of the member a command runs in,
// is a group of one member.
struct GroupJob {
    // The members as `[workspace] members` spells them; empty for the package
    // of the directory the command runs in.
    std::vector<std::string>                  paths;
    // What produced `ctx`. It stays the record of it, because the dispatch pass
    // prepares again with the same overrides plus the packaging pass's values.
    mcpp::build::BuildOverrides               ov;
    std::optional<mcpp::build::BuildContext>  ctx;
    std::vector<MemberJob>                    members;
    // The group failed as a whole: its plan, or its build.
    int                                       rc = 0;
};

// What the command asked for, and what every group of it shares.
struct PackRun {
    Options                                   opts;
    bool                                      modeFromUser = false;
    std::string                               wantTarget;
    std::vector<SharedLeg>                    extraLegs;
    const MemberPack*                         selection = nullptr;
    // More than one member is packed, so a line about one names it.
    bool                                      several = false;
    // The value of a dispatched `--format` is not known to be one until prepare
    // has run; see `prepare_group`.
    bool                                      quietUntilValidated = false;
    // The command reports its build (`mcpp pack` in human output): the drives
    // are stated as `mcpp build` states one (#753).
    bool                                      reporting = false;
    std::optional<mcpp::config::GlobalConfig> cfg;
};

// A refusal: what the command exits with, and the message. The message is
// reported where the refusal is made.
struct Refusal {
    int         rc = 0;
    std::string message;
};

// A line about a member names it when several are packed, and stays as it has
// always been for one.
std::string about(const PackRun& run, const MemberJob& m, std::string_view text) {
    return run.several ? std::format("member '{}': {}", m.name, text) : std::string(text);
}

// Does the member's closure carry this program shipped through `artifacts`? In a
// plan of several members each program a member ships is placed in that member's
// product directory (`BuildPlan::LinkGroup::placements`), and the member's own
// files are the ones that directory holds.
bool member_carries(const mcpp::build::BuildContext& ctx, const MemberJob& m,
                    const mcpp::build::LinkUnit& u) {
    if (!m.inWorkspace || ctx.workspaceMembers.size() < 2) return true;
    for (auto const& g : ctx.plan.linkGroups) {
        if (g.linkOnly || g.member != m.name) continue;
        if (u.output.parent_path() == g.productDir) return true;
        return std::ranges::any_of(g.placements,
            [&](auto const& pl) { return pl.source == u.output; });
    }
    return false;
}

// The program a member packs, and everything the member's pack reads from the
// plan: its request resolved against the flags, and the pack plan. It reads the
// plan and no built file but the program's format (`make_plan`), so it is asked
// before the build of a pack of several members, for what it refuses and for
// where it writes, and again after it, for the plan `pack::run` executes.
//
// Called inside the member's view (`with_member`): `ctx.manifest`,
// `ctx.projectRoot` and the plan's link group are the member's.
std::optional<Refusal> member_plan(PackRun& run, GroupJob& g, MemberJob& m) {
    auto& ctx = *g.ctx;
    const bool sharedPlan = ctx.workspaceMembers.size() > 1;
    Options opts = m.request;

    // ─── Pick the main binary target ─────────────────────────────────
    //
    // An explicitly named target wins over the package-name convention: the
    // user said which one, and guessing past that is how `mcpp pack app2`
    // would produce app1's bundle under app2's name.
    //
    // #622 A3/A10: an `app` whose form on THIS row is a shared object still
    // links as `LinkUnit::SharedLibrary` (`mcpp.build.plan`), so "is this
    // link unit the program" cannot ask for `Binary` alone any more without
    // reintroducing the refusal `Target::is_program()` exists to remove
    // everywhere else this record sweeps (§2.3). `dependencyOwned` is
    // excluded: a dependency's own `shared` target contributes a link unit to
    // this plan too, and it is never the package being packed.
    //
    // In a plan of several members the link units are every member's, and a
    // member packs the ones `memberOf` names it for.
    auto is_program_link_unit = [&](const mcpp::build::LinkUnit& lu) {
        if (sharedPlan && lu.memberOf != m.name) return false;
        // A dependency's program shipped beside this one (mcpp#711) is staged
        // as a file, never packed as the program.
        if (lu.kind == mcpp::build::LinkUnit::Binary) return lu.artifactOf.empty();
        if (lu.kind != mcpp::build::LinkUnit::SharedLibrary || lu.dependencyOwned)
            return false;
        for (auto const& t : ctx.manifest.targets)
            if (t.name == lu.targetName)
                return t.kind == mcpp::manifest::Target::Application;
        return false;
    };
    std::filesystem::path mainBinary;
    const mcpp::build::LinkUnit* chosenLu = nullptr;
    if (!run.wantTarget.empty()) {
        for (auto& lu : ctx.plan.linkUnits) {
            if (is_program_link_unit(lu) && lu.targetName == run.wantTarget) {
                mainBinary = ctx.outputDir / lu.output;
                chosenLu = &lu;
                break;
            }
        }
        if (mainBinary.empty())
            return Refusal{2, about(run, m, std::format(
                "target '{}' is not a program in this build", run.wantTarget))};
    }
    for (auto& lu : ctx.plan.linkUnits) {
        if (!mainBinary.empty()) break;
        if (is_program_link_unit(lu) && lu.targetName == ctx.manifest.package.name) {
            mainBinary = ctx.outputDir / lu.output;
            chosenLu = &lu;
            break;
        }
    }
    if (mainBinary.empty()) {
        // Fall back to the first binary target if package.name doesn't match.
        for (auto& lu : ctx.plan.linkUnits) {
            if (is_program_link_unit(lu)) {
                mainBinary = ctx.outputDir / lu.output;
                chosenLu = &lu;
                break;
            }
        }
    }
    if (mainBinary.empty())
        return Refusal{1, about(run, m, "no binary target to pack")};
    // Passed to `make_plan` rather than re-derived from the file: `make_plan`
    // has only `mainBinary` and would otherwise have to ask the triple and
    // the manifest the same question a second time.
    const bool programIsSharedObject =
        chosenLu && chosenLu->kind == mcpp::build::LinkUnit::SharedLibrary;

    if (!run.cfg) {
        auto cfg = mcpp::config::load_or_init(/*quiet=*/false,
            mcpp::fetcher::make_bootstrap_progress_callback());
        if (!cfg) return Refusal{4, cfg.error().message};
        run.cfg = std::move(*cfg);
    }

    // ─── What the build promised, and where its runtime lives ────────
    //
    // The C++ runtime contract has been resolved since the flags were
    // computed; `pack` simply had no way to see it (design §4.3), so on PE
    // nothing enforced it and on ELF the `ldd` closure agreed with it by
    // luck. Reading the RESOLVED value rather than the manifest string is the
    // point: a request that was downgraded (a per-role self-contained on
    // /MD, say) must not make the package behave as though it had been
    // honoured.
    {
        const auto flags = mcpp::build::compute_flags(ctx.plan);
        opts.carryToolchainRuntime =
            flags.contractByRole[static_cast<std::size_t>(
                mcpp::build::dist::Role::Distributable)]
            == mcpp::build::dist::Contract::ToolchainCoupled;
        // AN EXPLICIT `--mode system` OUTRANKS A DEFAULTED CONTRACT (#718,
        // §7.3): the MSVC-ABI default is toolchain-coupled, and asking for
        // `--mode system` on a project that never wrote `cxx_runtime` down is
        // an explicit choice for host-coupled, not a contradiction — the
        // contradiction is reserved for a manifest that SAID
        // toolchain-coupled and a mode that bundles nothing (checked below,
        // unchanged).
        if (opts.mode == mcpp::pack::Mode::None
            && opts.carryToolchainRuntime
            && !flags.programCxxRuntimeStated) {
            opts.carryToolchainRuntime = false;
        }
        opts.toolchainRuntimeDirs = ctx.plan.toolchain.linkRuntimeDirs;
        if (!ctx.plan.toolchain.msvcRedistDir.empty())
            opts.toolchainRuntimeDirs.push_back(ctx.plan.toolchain.msvcRedistDir);
        // Where a third-party dependency's shared library may be found. Both
        // channels, because they answer for different things: the runtime
        // library dirs are what `mcpp run` puts on the loader's path, and the
        // link intent's search dirs are what a dependency package declared.
        opts.depSearchDirs = ctx.plan.runtimeLibraryDirs;
        for (auto const& d : ctx.plan.linkIntent.runtimeSearchDirs)
            opts.depSearchDirs.push_back(d);
        // What the build placed relative to the executable (#615): the
        // runtime placement resolver's answer (`CompileFlags::runtimeDeploy`),
        // not the plan's candidates, so the package carries the files the
        // build placed and no other. The destinations are `bin/<to>/<file>`,
        // and the executable is in `bin/`.
        //
        // The MSVC C++ runtime's names are left to the closure below: in a
        // mode that carries the toolchain's runtime it resolves them beside
        // the program, where the build placed the chosen set; otherwise they
        // are the host's (`hostProvidedLibs`), whichever directory offers a
        // copy. A copy found in a dependency's directory never enters a
        // package that the contract says the host serves.
        namespace rp = mcpp::build::runtime_placement;
        const bool msvcAbi = mcpp::toolchain::is_msvc_target(ctx.plan.toolchain);
        // Relative to the program's directory: `bin`, or a workspace member's
        // product directory (§15 of the 2026-09-29 workspace design).
        const auto& productDir = ctx.plan.productDir;
        for (auto const& d : flags.runtimeDeploy) {
            if (msvcAbi && d.dest.parent_path() == productDir
                && rp::is_msvc_crt_name(d.dest.filename().string()))
                continue;
            opts.runtimeFiles.push_back(d.dest.lexically_relative(productDir));
        }
        if (msvcAbi && !opts.carryToolchainRuntime && flags.runtimeCrtPolicy != "static")
            for (auto n : rp::kMsvcCrtNames) opts.hostProvidedLibs.emplace_back(n);
        // A dependency's program the manifest ships with this one (mcpp#711,
        // `artifacts = [...]`) is linked into `bin/` beside the executable, so
        // it is staged the way a deployed file is.
        // In a workspace member's product directory it is the placed copy.
        for (auto const& u : ctx.plan.linkUnits)
            if (!u.artifactOf.empty() && member_carries(ctx, m, u))
                opts.runtimeFiles.push_back(productDir == "bin"
                    ? u.output.lexically_relative("bin")
                    : std::filesystem::path(u.output.filename()));
        // #634 A3: the Android row reads its closure against the directories
        // its link declared -- a prebuilt library named through `[runtime]
        // link_library_dirs` is a file the link used and the device does not
        // have -- and against its driver's; see `driver_library_dirs`.
        if (programIsSharedObject) {
            for (auto const& d : ctx.plan.linkIntent.linkLibraryDirs)
                opts.depSearchDirs.push_back(d);
            for (auto const& d : ctx.plan.linkIntent.transitiveNeededDirs)
                opts.depSearchDirs.push_back(d);
            auto dirs = driver_library_dirs(ctx.tc);
            opts.toolchainLibraryDirs = std::move(dirs.search);
            opts.platformLibraryDirs  = std::move(dirs.platform);
        }
    }

    // ─── Build the plan ──────────────────────────────────────────────
    auto plan = mcpp::pack::make_plan(ctx.manifest, *run.cfg, opts,
        mainBinary, ctx.projectRoot, ctx.tc.targetTriple,
        // From the RESOLVED graph. `mcpp why runtime` on a real imgui project
        // lists `capability:opengl.glx.driver <- compat.glfw@3.4` — none of
        // which appears in the project's own manifest.
        ctx.plan.runtimeRequirements, programIsSharedObject);
    if (!plan) return Refusal{1, about(run, m, plan.error().message)};
    // `--output <dir>` with several members: each member's archive, or its tree
    // for `--format dir`, is written below the directory under the name it would
    // have by default. The staging tree of an archive, and of a dispatched
    // format, stays in the member's `target/dist`, as it does for `-o <file>`.
    if (run.selection && !run.selection->outputDir.empty()) {
        plan->archivePath = run.selection->outputDir / plan->archivePath.filename();
        if (opts.format == mcpp::pack::Format::Dir)
            plan->stagingRoot = run.selection->outputDir / plan->stagingRoot.filename();
    }
    m.opts                  = std::move(opts);
    m.mainBinary            = std::move(mainBinary);
    m.programIsSharedObject = programIsSharedObject;
    m.plan                  = std::move(*plan);
    return std::nullopt;
}

// Prepares one group of the pack: the plan of its members, and what is decided
// from it alone.
std::optional<Refusal> prepare_group(PackRun& run, GroupJob& g) {
    // QUIET FOR A DISPATCHED FORMAT, AND ONLY UNTIL THE VALUE IS VALIDATED.
    //
    // `mcpp pack --format bogus` must write NOTHING to stdout and exit 2 --
    // the machine-output contract, asserted by
    // tests/e2e/202_machine_output_contract.sh, because this is the path a
    // client hits when it probes an mcpp for a capability. The set of valid
    // values is a property of the resolved graph, so the refusal cannot be
    // decided until prepare has run, and prepare narrates what it resolves.
    //
    // Nothing is lost when the value IS valid: the dispatch pass prepares a
    // second time and prints the same lines, so a successful
    // `pack --format <name>` narrates once rather than twice.
    auto prepare = [&]() {
        if (run.quietUntilValidated) mcpp::ui::set_quiet(true);
        auto ctx = mcpp::build::prepare_build(/*print_fp=*/false, /*includeDevDeps=*/false,
                                              /*extraTargets=*/{}, g.ov);
        if (run.quietUntilValidated) mcpp::ui::set_quiet(false);
        return ctx;
    };
    auto ctx = prepare();
    if (!ctx) return Refusal{2, ctx.error()};
    g.ctx = std::move(*ctx);

    // The members of the group: the workspace members of its plan, in
    // selection order, or the plan's own package.
    if (g.ctx->workspaceMembers.empty()) {
        MemberJob m;
        m.name = mcpp::build::qualified_package_name(g.ctx->manifest);
        g.members.push_back(std::move(m));
    } else {
        for (auto const& wm : g.ctx->workspaceMembers) {
            MemberJob m;
            m.name = wm.name;
            m.path = wm.memberPath;
            m.inWorkspace = true;
            g.members.push_back(std::move(m));
        }
    }
    auto manifest_of = [&](const MemberJob& m) -> const mcpp::manifest::Manifest& {
        if (!m.inWorkspace) return g.ctx->manifest;
        for (auto const& wm : g.ctx->workspaceMembers)
            if (wm.name == m.name) return wm.manifest;
        return g.ctx->manifest;
    };
    // Manifest may override mode only when neither --mode nor an
    // equivalent flag (--target *-musl → static) was given.
    // A workspace plan's subject is its selected member (§15).
    for (auto& m : g.members) {
        m.request = run.opts;
        m.modeFromUser = run.modeFromUser;
        const auto& mf = manifest_of(m);
        if (!m.modeFromUser && !mf.packConfig.defaultMode.empty()) {
            if (auto mode = mcpp::pack::parse_mode(mf.packConfig.defaultMode))
                m.request.mode = *mode;
        }
    }

    // Re-derive target triple: if mode is Static we force the musl
    // triple even when the manifest's [pack].default_mode bumped us
    // here after `prepare_build` ran with the host toolchain.
    //
    // ...but NOT over a target the user asked for. `--mode static` on its own
    // has always meant "the musl-static ELF", and that stays; `--mode static
    // --target x86_64-windows-gnu` used to silently become a Linux build,
    // which was invisible while PE packaging did not exist and is a wrong
    // answer now that it does. An explicit `--target` is an instruction.
    //
    // One group is one target, so the members of a group that want the static
    // row must all want it.
    const bool anyStatic = std::ranges::any_of(g.members, [](const MemberJob& m) {
        return m.request.mode == mcpp::pack::Mode::Static; });
    if (anyStatic
        && run.opts.targetTriple.empty()
        && g.ctx->tc.targetTriple.find("-musl") == std::string::npos) {
        if (auto other = std::ranges::find_if(g.members, [](const MemberJob& m) {
                return m.request.mode != mcpp::pack::Mode::Static; });
            other != g.members.end()) {
            auto first = std::ranges::find_if(g.members, [](const MemberJob& m) {
                return m.request.mode == mcpp::pack::Mode::Static; });
            return Refusal{2, std::format(
                "member '{}' packs as --mode static, which links for the musl target, and "
                "member '{}' does not, and the two share one build.\n"
                "  use: --mode <mode> for both, or pack them separately",
                first->name, other->name)};
        }
        // Need to re-prepare the build with the musl target.
        //
        // `ov` IS MUTATED RATHER THAN SHADOWED. It has to stay the record of
        // what produced `ctx`, because the dispatch pass below re-enters
        // prepare with the same overrides plus two fields -- and a second
        // overrides object left behind here would make that pass differ from
        // this build in a way nothing states.
        g.ov.target_triple = "x86_64-linux-musl";
        // Quiet on the same grounds as the first prepare: this one also runs
        // before `--format` has been validated.
        auto ctx2 = prepare();
        if (!ctx2) return Refusal{2, ctx2.error()};
        g.ctx = std::move(*ctx2);
    }
    return std::nullopt;
}

// Is the requested format one the group provides, and for which members? The
// refusals of a dispatched `--format`, before the build.
std::optional<Refusal> check_format(PackRun& run, GroupJob& g) {
    auto const& ctx = *g.ctx;
    const auto& formatName = run.opts.formatName;
    // ─── Is the requested format one anything provides? ──────────────
    //
    // BEFORE THE BUILD, because a refusal that arrives after a full compile is
    // a worse refusal, and because this is the earliest point at which it can be
    // exact: build programs have now run and declared what they provide.
    //
    // The set is read from a pass that asked for NOTHING. That is what the
    // "declare unconditionally, submit conditionally" rule buys -- a member
    // that declared only when asked would leave this list empty exactly when a
    // user names a format, and the refusal would name nothing.
    if (run.opts.format == mcpp::pack::Format::Dispatched) {
        auto const& provided = ctx.plan.providedPackFormats;
        if (std::ranges::find(provided, formatName) == provided.end()) {
            std::string avail;
            for (auto b : mcpp::pack::kBuiltinPackFormats)
                avail += (avail.empty() ? "" : ", ") + std::string(b);
            for (auto const& f : provided) {
                if (mcpp::pack::is_builtin_pack_format(f)) continue;
                avail += ", " + f;
            }
            return Refusal{2, std::format(
                "unknown --format '{}'.\n"
                "  available in this build: {}\n"
                "  A format past `tar` and `dir` comes from a package in the "
                "resolved graph, which declares\n"
                "  it with `mcpp::provides_pack_format(\"<name>\")` in its build "
                "program. Add the package\n"
                "  that provides '{}' to [build-dependencies] and activate its "
                "feature.",
                formatName, avail, formatName)};
        }
        // A member is packed into its own staged tree, by a provider that acts
        // for it: the member's own build program, or a package that only that
        // member reaches. A package several members reach has no one tree to
        // name, and a member served by no provider would be reported as a pack
        // that produced nothing once the whole build had been paid for.
        if (g.members.size() > 1) {
            std::string missing;
            bool anyShared = false;
            for (auto const& m : g.members) {
                bool served = false;
                std::string sharedProvider;
                if (auto it = ctx.packFormatProviders.find(formatName);
                    it != ctx.packFormatProviders.end())
                    for (auto const& p : it->second) {
                        auto reach = ctx.packReach.find(p);
                        if (reach == ctx.packReach.end()) continue;
                        const auto owner = mcpp::build::pack_owner(p, reach->second);
                        if (owner == m.name) served = true;
                        else if (owner.empty() && sharedProvider.empty()
                                 && std::ranges::find(reach->second, m.name)
                                        != reach->second.end())
                            sharedProvider = p;
                    }
                if (served) continue;
                anyShared = anyShared || !sharedProvider.empty();
                missing += std::format("\n  member '{}' has no package providing it{}", m.name,
                    sharedProvider.empty() ? std::string{}
                    : std::format(" ('{}' provides it, and other packed members reach it too)",
                                  sharedProvider));
            }
            if (!missing.empty())
                return Refusal{2, std::format(
                    "--format '{}' is not provided for every packed member.{}\n"
                    "  A member is packed into its own staged tree by a provider in its own build "
                    "program, or in a\n"
                    "  package that only that member reaches.{}",
                    formatName, missing,
                    anyShared
                        ? "\n  A package that several packed members reach acts for none of them: "
                          "one run of its program\n"
                          "  serves them all, and it is given no staged tree. Provide the format from "
                          "each member's own\n"
                          "  build program, or pack the members one at a time."
                        : std::string{})};
        }
    }

    // A package claiming a built-in name is silently unreachable, since the
    // parser resolves `tar` and `dir` before consulting the graph at all.
    //
    // OUTSIDE THE DISPATCH BRANCH ABOVE, because the mistake is in the PACKAGE
    // and does not depend on what this invocation asked for. Reported on every
    // pack, so the author hears it on the plain `mcpp pack` they are most
    // likely to run.
    for (auto const& f : ctx.plan.providedPackFormats)
        if (mcpp::pack::is_builtin_pack_format(f))
            mcpp::ui::warning(std::format(
                "a package in this graph declares `mcpp:pack-format={}`, which "
                "is one of the archive shapes `mcpp pack` owns; `--format {}` "
                "will always select the built-in and never that package", f, f));
    return std::nullopt;
}

// Stages one member from the build of its group: the pack plan, the tree, and
// for a built-in format the product.
//
// Called inside the member's view (`with_member`).
std::optional<Refusal> stage_member(PackRun& run, GroupJob& g, MemberJob& m) {
    auto& ctx = *g.ctx;
    if (auto refused = member_plan(run, g, m)) return refused;
    auto& opts = m.opts;
    auto& plan = m.plan;
    const auto& cfg = *run.cfg;

    // The RESOLVED debug-information decision. On the plan, not in Options:
    // Options is the request, this is what it came out as once the manifest
    // and the toolchain had their say. Tools come from the build's own
    // toolchain so a cross bundle is stripped by the cross tool.
    // #630 A9: see the field comment on `Plan::extraSharedLegs` and the
    // parameter comment on `extraLegs` above. A no-op (default-constructed,
    // empty) for every caller before this item.
    plan->extraSharedLegs = std::move(run.extraLegs);
    // #649 E5: the shared libraries this graph built, primary leg. From the
    // plan's link units, never from a directory listing: a vendor library
    // deployed beside the program is not one of them and stays as shipped.
    for (auto const& u : ctx.plan.linkUnits)
        if (u.kind == mcpp::build::LinkUnit::SharedLibrary
            && (ctx.workspaceMembers.size() < 2 || u.memberOf.empty() || u.memberOf == m.name))
            plan->graphSharedLibraries.push_back(ctx.outputDir / u.output);

    plan->strip     = mcpp::pack::resolve_strip(opts, ctx.manifest.packConfig);
    plan->debugDir  = mcpp::pack::resolve_debug_dir(opts, ctx.manifest.packConfig,
                                                    ctx.projectRoot);
    plan->stripTools = mcpp::pack::StripTools{
        .strip   = mcpp::toolchain::binutils_tool(ctx.tc, "strip"),
        .objcopy = mcpp::toolchain::binutils_tool(ctx.tc, "objcopy"),
        // The CANONICAL triple, resolved the same way the library packer
        // resolves it: an empty `targetTriple` means "this host", and asking
        // the empty string would answer "in-band" for macOS and MSVC alike.
        .inBandDebugInfo = mcpp::pack::debug_info_is_in_band(
            ctx.tc.targetTriple.empty()
                ? mcpp::toolchain::triple::host_triple().str()
                : [&] {
                      auto t = mcpp::toolchain::triple::parse(ctx.tc.targetTriple);
                      return t ? t->str() : ctx.tc.targetTriple;
                  }()),
    };

    // "stripped" STATES WHAT THIS ROW DOES, NOT WHAT WAS REQUESTED (#649 E5).
    // It used to print the decision alone, so a Mach-O row, whose debug
    // information is not in the image and is never stripped, and the Android
    // row, which did not reach the strip step at all, both said "stripped".
    mcpp::ui::info("Packing", std::format("{} v{} ({}{})",
        plan->packageName, plan->packageVersion,
        mcpp::pack::mode_cli_name(plan->opts.mode),
        mcpp::pack::strips_on_this_row(*plan) ? ", stripped" : ""));

    // STAGING IS A SERVICE TO THE PROVIDER, NOT A PRECONDITION FOR DISPATCH.
    //
    // For `--format tar` and `--format dir` the staged tree IS the product, so
    // a staging failure -- no tree at all -- is the command failing. For a
    // DISPATCHED format a MISSING tree is still failing the same way (a
    // genuine I/O error staging steps 1-3), but an UNAVAILABLE CLOSURE is
    // not: `pack::run` now stages the program and its declared runtime files
    // before it asks whether this host can walk the artifact's dependency
    // closure, so a dispatched format receives that tree regardless of
    // whether the closure could be resolved. See §3 of
    // .agents/docs/2026-09-13-630-what-a-framework-still-hits-in-the-engine.md.
    //
    // Measured on macos-15 with mcpp 2026.9.11.1: `mcpp pack --format app`
    // never reached the dispatch, because `pack::run` refused a Mach-O PROGRAM
    // outright -- the built-in closure walk is `LD_TRACE_LOADED_OBJECTS`, which
    // is glibc's, and dyld ignores it and runs the program instead. That
    // refusal was correct about the built-in archive and said nothing about
    // whether a `.app` bundler could work, since a bundler that names one
    // program needs no closure walk at all. The engine was answering a
    // question the provider had not been asked.
    //
    // So a missing tree is REPORTED AND CARRIED rather than swallowed: the
    // reason is printed as a warning, the stage directory stays empty, and
    // `${mcpp.stage_dir}` then refuses at expansion naming that reason. A
    // provider that reads the tree gets a precise diagnostic; one that does not
    // proceeds. Nothing is silently degraded -- what changes is who decides.
    if (run.selection && !run.selection->outputDir.empty()) {
        std::error_code dirEc;
        std::filesystem::create_directories(plan->archivePath.parent_path(), dirEc);
        if (opts.format == mcpp::pack::Format::Dir)
            std::filesystem::create_directories(plan->stagingRoot.parent_path(), dirEc);
    }
    if (auto r = mcpp::pack::run(*plan, cfg); !r) {
        if (opts.format != mcpp::pack::Format::Dispatched)
            return Refusal{1, about(run, m, r.error().message)};
        m.stageFailure = r.error().message;
        mcpp::ui::warning(about(run, m, std::format(
            "no staged tree for --format {}: {}\n"
            "  A format that consumes ${{mcpp.stage_dir}} cannot be produced "
            "here; one that names a\n"
            "  built file with ${{mcpp.target_file:<name>}} is unaffected.",
            opts.formatName, m.stageFailure)));
    } else if (!r->walked) {
        // The tree exists; only its dependency closure does not. Distinct
        // warning text -- "staged" is true here, unlike the branch above.
        m.closure = mcpp::pack::ClosureStatus{false, r->reason, r->needs};
        mcpp::ui::warning(about(run, m, std::format(
            "staged without its dependency closure: {}\n"
            "  A format that consumes ${{mcpp.stage_dir}} sees the program and its "
            "declared\n"
            "  runtime files but not its discovered dependencies; one that names a "
            "built file\n"
            "  with ${{mcpp.target_file:<name>}} is unaffected.",
            m.closure.reason)));
    } else {
        m.closure.needs = r->needs;
    }

    // The staged tree is now on disk and final -- past the closure (walked or
    // not), the `$ORIGIN` rewriting, the strip and the debug split. Describe
    // it, so an action that consumes it has something whose CONTENT changes
    // when the staged set does, and so a provider can read whether the
    // closure was walked. Best-effort: see write_stage_manifest. Skipped when
    // no tree exists, so no manifest describes a tree that is not there.
    if (m.stageFailure.empty()) mcpp::pack::write_stage_manifest(plan->stagingRoot, m.closure);

    // #649 E9: the outcome a machine reader receives, from values this pass
    // has already answered. Every return goes through it.
    auto& out = m.result;
    out.name = m.name;
    out.format = opts.format == mcpp::pack::Format::Tar ? std::string("tar")
               : opts.format == mcpp::pack::Format::Dir ? std::string("dir")
               : opts.formatName;
    const auto canonical = [](std::string const& t) {
        if (t.empty()) return mcpp::toolchain::triple::host_triple().str();
        auto parsed = mcpp::toolchain::triple::parse(t);
        return parsed ? parsed->str() : t;
    };
    out.targets.push_back(canonical(plan->triple));
    for (auto const& leg : plan->extraSharedLegs) out.targets.push_back(leg.triple);
    if (m.stageFailure.empty()) {
        out.stageDir      = plan->stagingRoot;
        out.stageManifest = mcpp::pack::stage_manifest_path(plan->stagingRoot);
        out.closure       = m.closure.walked ? "walked" : "not-walked";
    }
    std::error_code bec;
    m.ranBuildPrograms = std::filesystem::exists(ctx.projectRoot / "build.mcpp", bec)
                      || !ctx.manifest.buildConfig.ruleModules.empty();
    for (auto const& sp : ctx.sourcePackages)
        if (std::filesystem::exists(sp.root / "build.mcpp", bec)) m.ranBuildPrograms = true;
    // Paths are shown from the directory the command was typed in: a workspace's
    // root when several members are packed, and otherwise the package's own.
    m.pathCtx = mcpp::fetcher::make_path_ctx(
        &cfg, run.selection ? run.selection->root : ctx.projectRoot);

    if (opts.format != mcpp::pack::Format::Dispatched) {
        auto outPath = (opts.format == mcpp::pack::Format::Tar)
            ? plan->archivePath : plan->stagingRoot;
        mcpp::ui::status("Packed", mcpp::ui::shorten_path(outPath, m.pathCtx));
        out.artifacts.push_back(std::move(outPath));
    }
    return std::nullopt;
}

// ─── The dispatch pass ───────────────────────────────────────────────
//
// A `role = "artifact"` action is a ninja edge, and the staged tree is
// produced here, in C++, AFTER ninja has finished. So an artifact action
// cannot depend on the staged tree in the pass that built it, and a
// single-pass `--format <name>` is not expressible. Two passes are, and
// every value this one needs was answered by the first:
//
//   `ov`                  the overrides that produced the build above
//   `plan->stagingRoot`   from make_plan, which resolved the triple
//   `opts.formatName`     the request, already checked against the graph
//
// NOTHING IS RE-DERIVED, and that is the whole discipline of this block.
// `stagingRoot` is a function of the package name, the version, the resolved
// triple and the mode; the resolved triple is not known until a prepare has
// run, so computing it a second time before prepare -- from the host triple,
// say -- is the shape where two derivations of one value agree on every
// machine the author has and disagree on one they do not.
//
// One pass serves every member of the group: each member has its own stage,
// the programs act for the member they belong to (`BuildOverrides::
// pack_stages`), and one ninja drive builds every member's edges.
std::optional<Refusal> dispatch_group(PackRun& run, GroupJob& g, mcpp::build::Backend& be) {
    // WHICH ARTIFACT ACTIONS THIS BUILD ALREADY HAD, before a format was
    // requested. The dispatch below reports what the REQUEST introduced,
    // and this is the other half of that subtraction.
    std::set<std::pair<std::string, std::string>> preexistingArtifacts;
    for (auto const& a : g.ctx->plan.actions)
        if (a.role == mcpp::manifest::BuildAction::Role::Artifact)
            preexistingArtifacts.emplace(a.packageName, a.id);

    // Nothing was staged, so there is nothing to hand a provider.
    if (std::ranges::none_of(g.members, [](const MemberJob& m) { return m.result.rc == 0; }))
        return std::nullopt;

    g.ov.pack_format = run.opts.formatName;
    auto stage_members = [&] {
        g.ov.pack_stages.clear();
        for (auto const& m : g.members) {
            if (m.result.rc != 0) continue;
            mcpp::build::BuildOverrides::PackStage stage;
            // Empty when staging was refused, which is what makes
            // `${mcpp.stage_dir}` refuse with the reason attached rather than
            // expand to a directory that does not exist.
            if (m.stageFailure.empty()) stage.dir = m.plan->stagingRoot;
            stage.reason = m.stageFailure;
            // #649 E5: the RESOLVED strip decision and debug directory, for a
            // member that stages libraries of its own and must follow the same
            // switch `--no-strip` and `--debug-symbols` set for this tree.
            stage.strip           = m.plan->strip ? "1" : "0";
            stage.debugSymbolsDir = m.plan->debugDir;
            g.ov.pack_stages[m.name] = std::move(stage);
        }
    };
    stage_members();
    // THE DISPATCH PASS IS REPORTED AS A BUILD (#753). `Finished` closed the
    // report of the build above; this pass reopens it for its own planning,
    // its build programs and its drive, and closes it before the members'
    // results.
    if (run.reporting) mcpp::build::progress::open(mcpp::log::is_verbose());
    struct CloseReport {
        bool on;
        ~CloseReport() { if (on) mcpp::build::progress::close(); }
    } closeReport{run.reporting};
    auto distCtx = mcpp::build::prepare_build(false, false, {}, g.ov);
    // A MEMBER WITHOUT A TREE FAILS ALONE (P3). Its provider may name only a
    // built file and proceed, which is why the member is kept in the pass; when
    // the pass is refused instead -- a provider read `${mcpp.stage_dir}` -- the
    // members without a tree are failed by name and the pass is prepared again
    // for the others.
    if (!distCtx && run.several
        && std::ranges::any_of(g.members, [](const MemberJob& m) {
               return m.result.rc == 0 && !m.stageFailure.empty(); })) {
        for (auto& m : g.members) {
            if (m.result.rc != 0 || m.stageFailure.empty()) continue;
            mcpp::ui::error(about(run, m, std::format(
                "no staged tree for --format {}: {}", run.opts.formatName, m.stageFailure)));
            m.result.rc = 1;
        }
        if (std::ranges::none_of(g.members, [](const MemberJob& m) { return m.result.rc == 0; }))
            return std::nullopt;
        stage_members();
        distCtx = mcpp::build::prepare_build(false, false, {}, g.ov);
    }
    if (!distCtx) return Refusal{2, distCtx.error()};

    // WHICH ACTIONS ARE THE DISTRIBUTABLE: the artifact actions the REQUEST
    // INTRODUCED. An action present in both passes existed before anyone
    // asked for a format -- a codesign stamp, a size budget -- and
    // reporting one as the package would be a wrong answer that looks like a
    // right one.
    //
    // THE FIRST VERSION ASKED A NARROWER QUESTION AND GOT IT WRONG. It
    // collected only actions naming `${mcpp.stage_dir}`, on the assumption
    // that a distributable consumes the staged closure. Not every format
    // does: an `.msi` built from ONE named program takes
    // `${mcpp.target_file:<name>}` and never looks at the tree, which is
    // the shape section 6 of the design record recommends -- "name the
    // input, do not harvest a directory", after a bind path that resolved
    // to nothing produced a valid, empty, 52 KB installer. So the member
    // that followed the guidance was the member the check refused, and the
    // workaround was to name the placeholder as an unused input purely to
    // satisfy it. Presence-in-this-pass is the property actually wanted,
    // and it needs nothing of the member.
    //
    // Identity is (package, id): an id is unique within the package that
    // declared it and nothing more.
    //
    // AN ACTION BELONGS TO THE MEMBER THE PACKAGE THAT SUBMITTED IT ACTS FOR:
    // the member whose own build program submitted it, or the one member whose
    // closure reaches the package (`pack_owner`). A plan of one member has no
    // other to belong to.
    struct Submitted {
        std::vector<std::string> outputs;
        // THE DISTRIBUTABLE IS THE TERMINAL ARTIFACT. A provider may submit a
        // chain (`dist-apk`: link, add libraries, align, sign); every output
        // is verified below, but the thing a user installs, and the operand
        // `mcpp run --format` hands the runner, is an output no other
        // introduced action consumes. Measured 2026-09-12: with the first
        // output taken as the operand, `adb-run` received the unsigned
        // `base.apk` and `adb install` refused it.
        std::vector<std::string> inputs;
    };
    std::map<std::string, Submitted> submitted;
    const bool sharedPlan = distCtx->workspaceMembers.size() > 1;
    for (auto const& a : distCtx->plan.actions) {
        if (a.role != mcpp::manifest::BuildAction::Role::Artifact) continue;
        if (preexistingArtifacts.contains({a.packageName, a.id})) continue;
        std::string member = g.members.front().name;
        if (sharedPlan) {
            auto reach = distCtx->packReach.find(a.packageName);
            member = reach == distCtx->packReach.end() ? std::string{}
                   : mcpp::build::pack_owner(a.packageName, reach->second);
        }
        if (member.empty()) continue;
        auto& s = submitted[member];
        for (auto const& o : a.outputs) s.outputs.push_back(o);
        for (auto const& i : a.inputs)  s.inputs.push_back(i);
    }
    auto absolute_of = [&](std::string const& p) {
        auto q = std::filesystem::path(p).is_absolute()
               ? std::filesystem::path(p) : distCtx->plan.outputDir / p;
        return q.lexically_normal();
    };
    // Two members' providers writing one file are refused, naming both: one
    // edge declares an output once, and the second declaration would be ninja's
    // refusal of the whole group.
    {
        std::map<std::filesystem::path, std::string> writer;
        for (auto const& m : g.members) {
            auto it = submitted.find(m.name);
            if (m.result.rc != 0 || it == submitted.end()) continue;
            for (auto const& o : it->second.outputs) {
                auto [at, fresh] = writer.try_emplace(absolute_of(o), m.name);
                if (!fresh && at->second != m.name)
                    return Refusal{1, std::format(
                        "members '{}' and '{}' would both write {} for --format '{}'.\n"
                        "  A file has one producer: write each member's output below a "
                        "directory of its own,\n"
                        "  for example under `mcpp::out_dir()`, which is the member's.",
                        at->second, m.name, at->first.string(), run.opts.formatName)};
            }
        }
    }

    // DECLARED AND THEN SUBMITTED NOTHING. The half of the contract a
    // member is most likely to get wrong is the gate, and a member whose
    // gate never opens leaves a pass that succeeds and produces no
    // package. Refused by name rather than reported as success.
    //
    // A MEMBER THAT GOT NOTHING FAILS ALONE: the members that were claimed
    // continue (P3).
    bool any = false;
    for (auto& m : g.members) {
        if (m.result.rc != 0) continue;
        auto it = submitted.find(m.name);
        if (it != submitted.end() && !it->second.outputs.empty()) { any = true; continue; }
        mcpp::ui::error(about(run, m, std::format(
            "no action claimed --format '{}'.\n"
            "  A package declared it provides this format, and no build "
            "program submitted a new\n"
            "  `role = \"artifact\"` action when it was asked for.\n"
            "  The provider must gate on the request and not on anything "
            "else:\n"
            "      mcpp::provides_pack_format(\"{}\");                     "
            "// always\n"
            "      if (std::string_view(mcpp::pack_format()) == \"{}\") ..."
            "   // then submit",
            run.opts.formatName, run.opts.formatName, run.opts.formatName)));
        m.result.rc = 1;
    }
    if (!any) return std::nullopt;

    for (auto const& m : g.members)
        if (m.result.rc == 0)
            mcpp::ui::info("Distributing", std::format("{} v{} (--format {})",
                m.plan->packageName, m.plan->packageVersion, run.opts.formatName));

    // NO EXPLICIT GOALS. Everything but the dist edges is already up to
    // date from the build above, so a full drive costs a graph scan and
    // nothing else -- and an explicit goal set is how the 0.0.104 soname
    // aliases went missing, because an edge reachable only through
    // `default` is skipped under one.
    //
    // SEVERAL MEMBERS KEEP GOING (P3): one member's distribution step that
    // fails must not stop another member's, so the drive continues past a
    // failure, and each member is then judged by its own files below.
    //
    // A drive that keeps going and fails cannot say which member's step
    // failed, and a file a previous pack left in place would then read as this
    // pack's product. So the members' declared products are removed before the
    // drive, and a file present after it is one this drive made.
    if (run.several) {
        std::error_code rmEc;
        for (auto const& [name, s] : submitted)
            for (auto const& o : s.outputs) std::filesystem::remove_all(absolute_of(o), rmEc);
    }
    mcpp::build::progress::programs_done();
    mcpp::build::BuildOptions dbo;
    dbo.keepGoing = run.several;
    auto dr = be.build(distCtx->plan, dbo);
    const bool driveFailed = !dr.has_value();
    if (!dr) {
        mcpp::build::report_failed_drive(dr.error());
        if (!run.several) return Refusal{1, {}};
    }
    if (run.reporting) mcpp::build::progress::close();

    // THE CRITERION IS THE FILE, NOT THE EXIT CODE. A cached build program
    // replaying the first pass's answer, or a tool that writes nothing and
    // exits 0, both leave ninja reporting success -- and section 2's
    // measured failure was a packaging step that succeeded while carrying
    // nothing.
    for (auto& m : g.members) {
        if (m.result.rc != 0) continue;
        const auto& s = submitted.at(m.name);
        std::set<std::filesystem::path> consumed;
        for (auto const& i : s.inputs) consumed.insert(absolute_of(i));
        std::error_code ec;
        std::vector<std::filesystem::path> reported;
        std::vector<std::filesystem::path> intermediate;
        bool produced = true;
        for (auto const& o : s.outputs) {
            auto abs = absolute_of(o);
            if (!std::filesystem::is_regular_file(abs, ec)
                && !std::filesystem::is_directory(abs, ec)) {
                // A drive that failed says why above; its missing file is
                // the member's failure, not a success that carried nothing.
                mcpp::ui::error(about(run, m, driveFailed
                    ? std::format("--format {} did not produce {}: its step failed",
                                  run.opts.formatName, abs.string())
                    : std::format("--format {} reported success and produced nothing at {}",
                                  run.opts.formatName, abs.string())));
                m.result.rc = 1;
                produced = false;
                break;
            }
            if (consumed.contains(abs)) { intermediate.push_back(std::move(abs)); continue; }
            reported.push_back(std::move(abs));
        }
        if (!produced) continue;
        report_packed(reported, m.pathCtx);
        // Every output consumed by another: a cycle a provider should not
        // write, reported as all outputs rather than as nothing.
        if (reported.empty()) reported = std::move(intermediate);
        m.result.artifacts = std::move(reported);
    }
    return std::nullopt;
}

// The refusals that need only the plans of the groups, before anything is
// compiled, for a pack of several members: what each member would write, and
// that no two write one destination.
std::optional<Refusal> check_destinations(PackRun& run, std::vector<GroupJob>& groups) {
    std::map<std::filesystem::path, std::string> writer;
    for (auto& g : groups) {
        if (g.rc != 0) continue;
        for (auto& m : g.members) {
            std::optional<Refusal> refused;
            mcpp::build::with_member(*g.ctx, m.inWorkspace ? m.name : std::string_view{}, [&] {
                refused = member_plan(run, g, m);
            });
            if (refused) return refused;
            // The staging tree and the product: a tree for `--format dir`, an
            // archive for `tar`; a dispatched format's products are its
            // provider's, and are compared when they are submitted.
            std::vector<std::filesystem::path> writes{m.plan->stagingRoot};
            if (m.opts.format == mcpp::pack::Format::Tar) writes.push_back(m.plan->archivePath);
            for (auto const& w : writes) {
                auto [at, fresh] = writer.try_emplace(w.lexically_normal(), m.name);
                if (!fresh)
                    return Refusal{2, std::format(
                        "members '{}' and '{}' would both write {}.\n"
                        "  Two packages of one name, version and target share an archive name: "
                        "pack them one at\n"
                        "  a time, or without --output, which writes each below its own "
                        "`target/dist`.",
                        at->second, m.name, at->first.string())};
            }
            m.plan.reset();
        }
    }
    return std::nullopt;
}

// The pack: the plan of each configuration group and what is refused from it,
// then for each group the build, each member's staging and the one dispatch
// pass. `selection` is null for the package of the directory the command runs
// in, which is one member; a single member is the same steps with one member.
PackOutcome run_pack(PackRun run) {
    auto be = mcpp::build::make_ninja_backend();
    run.reporting = mcpp::build::progress::is_open();

    // ─── Build first (pack implies a fresh build) ────────────────────
    mcpp::build::BuildOverrides base;
    if (run.opts.mode == mcpp::pack::Mode::Static && run.opts.targetTriple.empty())
        base.target_triple = "x86_64-linux-musl";
    else
        base.target_triple = run.opts.targetTriple;
    // A bundled program leaves this machine: release is the fallback, not dev.
    // `[build] default-profile` still decides when the project states one.
    base.profile          = run.opts.profile;
    base.profile_fallback = "release";
    // The same features on this pass and on the dispatched format's second
    // pass below, which reuses `ov`.
    base.features         = run.opts.features;

    std::vector<GroupJob> groups;
    if (run.selection) {
        std::vector<std::string> request;
        for (auto const& members : run.selection->groups)
            for (auto const& mp : members) request.push_back(mp);
        for (auto const& members : run.selection->groups) {
            GroupJob g;
            g.paths = members;
            g.ov = base;
            g.ov.package_filter.clear();
            g.ov.project_root      = run.selection->root;
            g.ov.workspace_members = members;
            g.ov.workspace_request = request;
            groups.push_back(std::move(g));
        }
    } else {
        GroupJob g;
        g.ov = base;
        groups.push_back(std::move(g));
    }
    std::size_t memberCount = 0;
    run.quietUntilValidated =
        run.opts.format == mcpp::pack::Format::Dispatched && !mcpp::ui::is_quiet();

    // ─── The plans, and what is refused before anything is compiled ──
    for (auto& g : groups) {
        if (auto refused = prepare_group(run, g)) {
            mcpp::ui::error(refused->message);
            if (groups.size() == 1) return PackOutcome{refused->rc};
            // A configuration that cannot be planned fails for its members alone,
            // as a member that fails to build does (P3): the members are named
            // as the command named them, and the other groups are packed.
            g.rc = refused->rc;
            g.members.clear();
            for (auto const& mp : g.paths) g.members.push_back(MemberJob{.name = mp, .path = mp});
        }
        memberCount += g.members.size();
    }
    run.several = memberCount > 1;
    for (auto& g : groups)
        if (g.rc == 0)
            if (auto refused = check_format(run, g)) {
                mcpp::ui::error(refused->message);
                return PackOutcome{refused->rc};
            }
    if (run.several)
        if (auto refused = check_destinations(run, groups)) {
            mcpp::ui::error(refused->message);
            return PackOutcome{refused->rc};
        }

    // ─── One build per group, then one `Finished` ────────────────────
    //
    // THE PACK STATES ITS BUILD AS `mcpp build` STATES ONE (#753): each
    // package that does work has its line, the status row counts the build,
    // and `Finished` closes the report before the first `Packing` line. Every
    // group is built before any is staged, so a pack over several
    // configurations writes one `Finished`, as `mcpp build` does; a group
    // whose build fails still fails alone (P3).
    mcpp::build::progress::programs_done();
    std::size_t building = 0;
    for (auto const& g : groups) if (g.rc == 0) ++building;
    if (run.reporting && building > 1) {
        mcpp::build::progress::configurations(building);
        mcpp::build::progress::defer_finished();
    }
    bool everyGroupBuilt = true;
    for (auto& g : groups) {
        if (g.rc != 0) continue;
        mcpp::build::BuildOptions bo;
        auto br = be->build(g.ctx->plan, bo);
        if (!br) {
            mcpp::build::report_failed_drive(br.error());
            g.rc = 1;
            everyGroupBuilt = false;
            if (!run.several) return PackOutcome{1};
            continue;
        }
        mcpp::build::populate_dependency_cache(*g.ctx);
        if (run.reporting)
            mcpp::build::progress::finished(
                g.ctx->profile, mcpp::build::profile_descriptor(g.ctx->plan.manifest.buildConfig));
    }
    if (run.reporting && building > 1 && everyGroupBuilt)
        mcpp::build::progress::finish_deferred();
    // A report not closed by `Finished` (a group failed) is closed here, before
    // the members' own lines.
    mcpp::build::progress::close();

    // ─── Each member staged, one dispatch per group ──────────────────
    for (auto& g : groups) {
        if (g.rc != 0) continue;
        // Everything below reads the package being packed: in a workspace plan,
        // each selected member in turn (workspace design 2026-09-29 §15).
        for (auto& m : g.members) {
            std::optional<Refusal> refused;
            mcpp::build::with_member(*g.ctx, m.inWorkspace ? m.name : std::string_view{}, [&] {
                refused = stage_member(run, g, m);
            });
            if (!refused) continue;
            mcpp::ui::error(refused->message);
            m.result.name = m.name;
            m.result.rc = refused->rc;
            if (!run.several) return PackOutcome{refused->rc};
        }
        if (run.opts.format != mcpp::pack::Format::Dispatched) continue;
        if (auto refused = dispatch_group(run, g, *be)) {
            // A failed drive was reported where it failed.
            if (!refused->message.empty()) mcpp::ui::error(refused->message);
            for (auto& m : g.members)
                if (m.result.rc == 0) m.result.rc = refused->rc;
            if (!run.several) return PackOutcome{refused->rc};
        }
    }

    // ─── What was packed ─────────────────────────────────────────────
    //
    // A failing member is reported and the others continue (P3): the status is
    // that of the first member, in member order, that failed.
    //
    // The members are reported in `[workspace] members` order, which the groups,
    // each in that order, keep only within themselves.
    std::vector<MemberJob*> reported;
    for (auto& g : groups)
        for (auto& m : g.members) {
            m.result.name = m.name;
            if (g.rc != 0 && m.result.rc == 0) m.result.rc = g.rc;
            reported.push_back(&m);
        }
    if (run.selection && !run.selection->order.empty()) {
        auto rank = [&](const MemberJob* m) {
            auto it = std::ranges::find(run.selection->order, m->path);
            return static_cast<std::size_t>(it - run.selection->order.begin());
        };
        std::ranges::stable_sort(reported, {}, rank);
    }
    PackOutcome out;
    for (auto* m : reported) {
        if (m->result.rc != 0 && out.rc == 0) out.rc = m->result.rc;
        out.ranBuildPrograms = out.ranBuildPrograms || m->ranBuildPrograms;
        out.members.push_back(m->result);
    }
    if (out.rc != 0) return out;
    for (auto const& m : out.members)
        out.artifacts.insert(out.artifacts.end(), m.artifacts.begin(), m.artifacts.end());
    const auto& first = out.members.front();
    out.format        = first.format;
    out.targets       = first.targets;
    out.stageDir      = first.stageDir;
    out.stageManifest = first.stageManifest;
    out.closure       = first.closure;
    return out;
}

// Everything after CLI option parsing for `mcpp pack`, for the package of the
// directory the command runs in (a workspace member included).
//
// `wantTarget` is the target NAME the user asked for, empty when they did not.
// It exists because `mcpp pack <name>` now routes on `[targets.<name>].kind`:
// a name that resolves to a program has to reach the binary selection below,
// or a project with two `bin` targets would accept `mcpp pack app2` and
// silently bundle app1 — the shape where the command succeeds and the answer
// is wrong.
//
// `extraLegs` (#630 A9) is every OTHER triple a several-`--target` app-pack
// request named, already built by `build_extra_android_legs` above. This call
// still does exactly one build — of `opts.targetTriple`, the PRIMARY leg — and
// stages the primary's own artifact and the declared deploy files as it always
// has; `extraLegs`, when non-empty, only changes WHERE the primary's shared
// object lands (`Plan::extraSharedLegs`, read by `run_shared_program`) and adds
// the other legs, each with its own closure, beside it in the same staged tree,
// before the one dispatch pass runs. Empty for a single-`--target` pack.
export PackOutcome build_and_pack(Options opts, bool modeFromUser,
                          const std::string& wantTarget = {},
                          std::vector<SharedLeg> extraLegs = {}) {
    // `--target *-linux-musl` without an explicit `--mode` implies
    // `--mode static` — packaging a musl-static ELF as bundle-project
    // would feed patchelf a static binary and crash. The docs treat
    // this pair as equivalent; surface it in the code path too.
    if (!modeFromUser && opts.targetTriple.find("-musl") != std::string::npos) {
        opts.mode     = mcpp::pack::Mode::Static;
        modeFromUser  = true;   // user-equivalent intent — block manifest override
    }
    PackRun run;
    run.opts         = std::move(opts);
    run.modeFromUser = modeFromUser;
    run.wantTarget   = wantTarget;
    run.extraLegs    = std::move(extraLegs);
    return run_pack(std::move(run));
}

// `mcpp pack` over several workspace members: the same pipeline, with each
// configuration group planned once and built once (member selection design
// 2026-09-30, K1). Each member is staged from the group's build, read through
// its own view of the plan, and the members of a group are dispatched by one
// second pass.
export PackOutcome build_and_pack_members(Options opts, bool modeFromUser,
                                          const MemberPack& selection) {
    if (!modeFromUser && opts.targetTriple.find("-musl") != std::string::npos) {
        opts.mode     = mcpp::pack::Mode::Static;
        modeFromUser  = true;
    }
    PackRun run;
    run.opts         = std::move(opts);
    run.modeFromUser = modeFromUser;
    run.selection    = &selection;
    return run_pack(std::move(run));
}

} // namespace mcpp::pack
