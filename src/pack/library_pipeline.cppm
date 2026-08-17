// mcpp.pack.library_pipeline — everything `mcpp pack <target>` does when the
// named target is a library.
//
// The sibling of mcpp.pack.pipeline (which packs an application). They are
// separate files rather than one branch because they share almost nothing:
// an application bundle asks "what does this executable need at RUN time",
// a library package asks "what must a consumer compile, and what may it link".
//
// WHICH ONE RUNS IS NOT A FLAG. `[targets.<n>].kind` already says whether a
// target is a program or a library, so the command reads that answer instead
// of asking for it again — there is no `--lib`, and no `--artifact static`.
// A project that publishes both forms declares both targets, which is also
// what it must do for `mcpp build` to produce both.
//
// Design: .agents/docs/2026-08-17-library-distribution-design.md §2.

export module mcpp.pack.library_pipeline;

import std;
import mcpp.build.backend;
import mcpp.build.ninja;
import mcpp.build.plan;
import mcpp.build.prepare;
import mcpp.manifest;
import mcpp.modgraph.graph;
import mcpp.modgraph.scanner;
import mcpp.pack;
import mcpp.pack.abi_tag;
import mcpp.pack.interface;
import mcpp.pack.library;
import mcpp.platform;
import mcpp.toolchain.dialect;
import mcpp.toolchain.registry;
import mcpp.toolchain.triple;
import mcpp.ui;
import mcpp.version;

namespace mcpp::pack {

namespace {

// The lib-root's own unit.
//
// Both facts the packer needs come from here, and taking them from the SAME
// unit is the point: the module name (mcpp has not required it to match the
// package name since 0.0.10 — the author names the module, the scanner detects
// it) and the package name the scanner stamped on every unit. Recomputing the
// latter from the manifest would be a second derivation of a value the graph
// already carries, and the closure compares against it by string.
const mcpp::modgraph::SourceUnit* root_unit_of(const mcpp::modgraph::Graph& g,
                                               const std::filesystem::path& libRoot)
{
    std::error_code ec;
    for (auto const& u : g.units) {
        if (!u.provides) continue;
        if (std::filesystem::equivalent(u.path, libRoot, ec)) return &u;
    }
    return nullptr;
}

// Dependencies the package can honestly carry downstream.
//
// The same rule `mcpp emit xpkg` applies, and for the same reason: a `path`
// or `git` dependency addresses the PRODUCER's disk or a revision only they
// can resolve, so republishing it hands the consumer an address that means
// something else (or nothing) on their machine. Version dependencies are the
// only kind that survive the trip.
std::vector<std::pair<std::string, std::string>>
publishable_dependencies(const mcpp::manifest::Manifest& m)
{
    std::vector<std::pair<std::string, std::string>> out;
    for (auto const& [k, v] : m.dependencies) {
        if (v.isPath() || v.isGit() || v.version.empty()) continue;
        out.emplace_back(k, v.version);
    }
    return out;
}

std::vector<std::filesystem::path> extras_of(const mcpp::manifest::Manifest& m,
                                             const std::filesystem::path& root)
{
    std::vector<std::filesystem::path> out;
    std::error_code ec;
    for (auto const* name : { "README.md", "README", "LICENSE", "LICENSE.txt", "COPYING" })
        if (std::filesystem::is_regular_file(root / name, ec)) out.push_back(root / name);
    // `[pack].include` keeps the meaning it has for an application bundle:
    // EXTRA files to ship. It has never reached the interface or the headers,
    // and it must not start to — those two sets are computed, and letting a
    // glob trim them would give the same library a different public surface
    // depending on how it was delivered.
    for (auto const& glob : m.packConfig.include)
        for (auto const& hit : mcpp::modgraph::expand_glob(root, glob))
            if (std::filesystem::is_regular_file(hit, ec)) out.push_back(hit);
    return out;
}

} // namespace

// `mcpp pack <target>` for a `kind = "lib"` / `"shared"` target.
//
// `triples` is the `--target` list; empty means "this host". Each entry gets
// its own prepare+build, so the artifacts really are the ones this run made —
// the packer never searches `target/` for something that looks right.
export int build_and_pack_library(const std::string& targetName,
                                  const std::vector<std::string>& triples,
                                  const mcpp::pack::Options& opts)
{
    std::vector<std::string> legs = triples;
    if (legs.empty()) legs.push_back({});   // one leg, this host

    LibraryPackPlan plan;
    plan.builtBy      = std::string(mcpp::MCPP_VERSION);
    plan.writeArchive = opts.format == mcpp::pack::Format::Tar;

    InterfaceClosure closure;
    bool             haveClosure = false;
    std::string      firstTriple;
    // `[package] platforms` — the support CLAIM, read once. Checked against the
    // legs after the loop; see there for why only two of the four comparisons
    // are printable.
    std::vector<std::string> declaredPlatforms;

    for (auto const& want : legs) {
        mcpp::build::BuildOverrides ov;
        ov.target_triple = want;
        auto ctx = mcpp::build::prepare_build(false, /*includeDevDeps=*/false, {}, ov);
        if (!ctx) { mcpp::ui::error(ctx.error()); return 2; }

        // ── the target, and what its kind means here ──────────────────
        const mcpp::manifest::Target* target = nullptr;
        for (auto const& t : ctx->manifest.targets)
            if (t.name == targetName) { target = &t; break; }
        if (!target) {
            std::string names;
            for (auto const& t : ctx->manifest.targets) {
                if (!names.empty()) names += ", ";
                names += t.name;
            }
            mcpp::ui::error(std::format(
                "no target named '{}' in this package{}{}",
                targetName, names.empty() ? "" : "; available: ", names));
            return 2;
        }
        const bool shared = target->kind == mcpp::manifest::Target::SharedLibrary;

        const auto triple = ctx->tc.targetTriple.empty()
            ? mcpp::toolchain::triple::host_triple().str()
            : [&] {
                  auto t = mcpp::toolchain::triple::parse(ctx->tc.targetTriple);
                  return t ? t->str() : ctx->tc.targetTriple;
              }();

        // ── build, then take the artifact FROM THE PLAN ───────────────
        //
        // Never a glob over `target/`: that directory holds one subtree per
        // fingerprint, and picking one by name or by mtime silently selects a
        // stale binary. The link unit knows its own output.
        auto be = mcpp::build::make_ninja_backend();
        mcpp::build::BuildOptions bo;
        if (auto br = be->build(ctx->plan, bo); !br) {
            mcpp::ui::error(br.error().message);
            return 1;
        }
        std::filesystem::path artifact;
        std::filesystem::path importLib;
        for (auto const& lu : ctx->plan.linkUnits) {
            if (lu.targetName != targetName) continue;
            if (lu.kind != mcpp::build::LinkUnit::StaticLibrary
                && lu.kind != mcpp::build::LinkUnit::SharedLibrary) continue;
            artifact = ctx->outputDir / lu.output;
            // Also from the plan, for the same reason: the link unit knows
            // whether it produced an import library and where.
            if (!lu.importLibrary.empty()) importLib = ctx->outputDir / lu.importLibrary;
            break;
        }
        if (artifact.empty()) {
            mcpp::ui::error(std::format(
                "target '{}' produced no library artifact for {}", targetName, triple));
            return 1;
        }

        // ── the interface closure ─────────────────────────────────────
        // PROBING form: the convention is `src/<tail>.<module-interface-ext>`, and
        // which extension that is belongs to the project. A package whose
        // interfaces are `.ixx` used to resolve to a `src/<tail>.cppm` that does
        // not exist, and the packer then published nothing and tagged the
        // result as a C surface — both silently.
        auto libRoot = ctx->projectRoot
                     / mcpp::manifest::resolve_lib_root_path(ctx->manifest, ctx->projectRoot);
        std::error_code ec;
        InterfaceClosure here;
        std::string qualifiedPackage;
        if (std::filesystem::is_regular_file(libRoot, ec)) {
            auto const* rootUnit = root_unit_of(ctx->graph, libRoot);
            if (!rootUnit) {
                mcpp::ui::error(std::format(
                    "'{}' is the lib root but provides no module interface",
                    libRoot.string()));
                return 1;
            }
            qualifiedPackage = rootUnit->packageName;
            auto c = interface_closure(ctx->graph, qualifiedPackage,
                                       rootUnit->provides->logicalName);
            if (!c) { mcpp::ui::error(c.error()); return 1; }
            here = std::move(*c);
        }
        // else: a header-only package. `sources = []` in the emitted manifest
        // says so explicitly, which is a thing a manifest can say now.

        if (!here.unresolvedImports.empty()) {
            std::string list;
            for (auto const& m : here.unresolvedImports) {
                if (!list.empty()) list += ", ";
                list += m;
            }
            mcpp::ui::error(std::format(
                "the published interface imports {} , which no unit in this build "
                "provides.\n"
                "  A consumer compiling the published interface would fail with\n"
                "  \"failed to read compiled module\". Either publish that unit (make it\n"
                "  an `export module` partition) or keep it out of the interface's purview.",
                list));
            return 1;
        }

        // Publishing an implementation partition is legal — the consumer cannot
        // build the interface's BMI without it — but for a closed-source
        // library it is the one thing nobody wants by accident, and nothing in
        // the source looks unusual: `import :detail;` reads like any other
        // import. So say it, with the file named.
        for (auto const& p : here.publishedImplementationPartitions) {
            mcpp::ui::warning(std::format(
                "{} is an implementation partition, and the published interface "
                "reaches it — so its SOURCE is being published.\n"
                "  A consumer compiling the interface cannot produce a BMI "
                "without it. If that source should stay private, move what the "
                "interface needs into an `export module` partition and keep the "
                "rest out of the interface's purview.",
                p.filename().string()));
        }

        // The same disclosure, one step worse: mcpp does not know which kind of
        // partition it just published. A `[scan_overrides."<glob>"]` entry names
        // the modules a file provides and has nowhere to say whether the
        // declaration carries `export`, and a P1689 scanner may omit
        // `is-interface`. The author is the only one who can answer, so ask them
        // rather than guessing — the guess used to be "interface", which is the
        // one that says nothing.
        for (auto const& p : here.publishedUndeterminedPartitions) {
            mcpp::ui::warning(std::format(
                "{} provides a module PARTITION and mcpp cannot tell which kind: "
                "the unit is declared in `[scan_overrides]`, which has nowhere to "
                "say whether the declaration carries `export`, or a P1689 scanner "
                "omitted `is-interface`.\n"
                "  Its SOURCE is being published either way. If the declaration "
                "has no `export` and that source should stay private, keep it out "
                "of the published interface's purview.",
                p.filename().string()));
        }

        if (!haveClosure) {
            closure           = std::move(here);
            haveClosure       = true;
            firstTriple       = triple;
            plan.projectRoot  = ctx->projectRoot;
            plan.namespace_   = ctx->manifest.package.namespace_;
            plan.packageName  = ctx->manifest.package.name;
            plan.packageVersion = ctx->manifest.package.version;
            plan.targetName   = targetName;
            plan.targetShared = shared;
            plan.cxxRuntime   = ctx->manifest.buildConfig.cxxRuntime;
            declaredPlatforms = ctx->manifest.package.platforms;
            plan.dependencies = publishable_dependencies(ctx->manifest);
            plan.extras       = extras_of(ctx->manifest, ctx->projectRoot);
            plan.interfaceSources = closure.published;
            plan.dropObjects  = published_object_names(closure);
            for (auto const& d : ctx->manifest.buildConfig.includeDirs) {
                auto abs = d.is_absolute() ? d : ctx->projectRoot / d;
                if (std::filesystem::is_directory(abs, ec)) { plan.includeDir = abs; break; }
            }
            for (auto const& u : ctx->graph.units)
                if (u.provides && u.provides->logicalName.find(':') == std::string::npos
                    && u.packageName == qualifiedPackage)
                    plan.exportsModules.push_back(u.provides->logicalName);
            std::ranges::sort(plan.exportsModules);
            plan.exportsModules.erase(
                std::ranges::unique(plan.exportsModules).begin(), plan.exportsModules.end());
        } else if (closure.published != here.published) {
            // One package, one `sources` list. If a conditional source glob
            // makes the INTERFACE differ per target, the package cannot
            // describe itself, and quietly publishing the first leg's answer
            // would ship an interface that does not match some of the binaries.
            mcpp::ui::error(std::format(
                "the published interface differs between {} and {}.\n"
                "  A package has one `sources` list, so its interface must be the same\n"
                "  for every target it ships. Move the per-target difference behind\n"
                "  the implementation, or publish one package per target.",
                firstTriple, triple));
            return 1;
        }

        // ── the tag ───────────────────────────────────────────────────
        //
        // A package whose interface is only headers constrains the libc ABI
        // and not the C++ one, so it publishes the shorter tag and links into
        // any compiler. The SHAPE is the statement; there is no flag for it.
        const bool cxxSurface = !closure.published.empty();
        auto tag = cxxSurface
            ? cxx_surface_tag(ctx->tc, triple, ctx->manifest.cppStandard.level)
            : c_surface_tag(triple);

        plan.legs.push_back(LibraryLeg{
            .triple      = triple,
            .artifact    = artifact,
            .archiveTool = shared ? std::filesystem::path{}
                                  : mcpp::toolchain::archive_tool(ctx->tc),
            .abiTag      = tag.str(),
            .buildKey    = ctx->fp.hex,
            .linkName    = targetName,
            .removeArg   = std::string(
                mcpp::toolchain::dialect_for(ctx->tc).archiveRemoveArg),
            .removeArchiveFirst =
                mcpp::toolchain::dialect_for(ctx->tc).archiveRemoveTakesArchiveFirst,
            .soname      = target->soname,
            .shared      = shared,
            .importLibrary = importLib,
        });
        mcpp::ui::status("Packed leg", std::format("{}  [{}]", triple, tag.str()));
    }

    // ── does the package cover what it claims? ─────────────────────────
    //
    // `[package] platforms` is a support claim, and `mcpp pack` is where it
    // first becomes checkable: the legs in this package are the platforms it can
    // actually serve. Two of the four comparisons are worth printing, and
    // printing the other two would make the check worse than absent —
    //
    //   packed, not declared   always actionable: either the claim is stale, or
    //                          this package now ships a binary for a platform
    //                          nobody said it supports.
    //   declared, not packed   actionable ONLY IF THIS HOST COULD HAVE BUILT IT.
    //                          The normal flow is one `mcpp pack` per platform in
    //                          CI, so a Linux runner producing no macos leg is
    //                          not an omission — it is every single run, and a
    //                          warning that fires on every run is one nobody
    //                          reads. `host_can_serve` is the same function that
    //                          decides which `--target` values are accepted at
    //                          all, so the warning can only ever name something
    //                          the author is able to do on this machine.
    if (!declaredPlatforms.empty()) {
        auto joined = [&] {
            std::string s;
            for (auto const& p : declaredPlatforms) { if (!s.empty()) s += ", "; s += p; }
            return s;
        }();

        std::set<std::string> packedOs;
        for (auto const& leg : plan.legs) {
            if (auto t = mcpp::toolchain::triple::parse(leg.triple)) packedOs.insert(t->os);
        }

        for (auto const& os : packedOs) {
            if (std::ranges::find(declaredPlatforms, os) != declaredPlatforms.end()) continue;
            mcpp::ui::warning(std::format(
                "this package ships a {} binary, and `[package] platforms` does "
                "not list {} (it lists: {}).\n"
                "  Add it if the support is real; drop the leg if it is not. As "
                "it stands the manifest disclaims a platform the package serves.",
                os, os, joined));
        }

        // Could this host have built a leg for `platform` at all?
        auto servable_here = [](std::string_view platform) {
            using mcpp::toolchain::triple::Triple;
            const std::string arch{ mcpp::platform::host_arch };
            std::vector<Triple> candidates;
            if (platform == "linux") {
                candidates.push_back(Triple{ arch, "linux", "gnu" });
                candidates.push_back(Triple{ arch, "linux", "musl" });
            } else if (platform == "windows") {
                candidates.push_back(Triple{ arch, "windows", "msvc" });
                candidates.push_back(Triple{ arch, "windows", "gnu" });
            } else if (platform == "macos") {
                candidates.push_back(Triple{ arch, "macos", "" });
            }
            for (auto const& c : candidates)
                if (mcpp::toolchain::host_can_serve(c)) return true;
            return false;
        };

        for (auto const& want : declaredPlatforms) {
            if (packedOs.contains(want)) continue;
            if (!servable_here(want)) continue;   // not this host's job to fix
            mcpp::ui::warning(std::format(
                "`[package] platforms` claims {}, and this host can build for it, "
                "but no {} leg was packed.\n"
                "  Consumers on {} will resolve this package and find no artifact "
                "for their target. Add `--target <{}-triple>`, or publish a "
                "separate package from a {} runner.",
                want, want, want, want, want));
        }
    }

    // ── where it lands ────────────────────────────────────────────────
    const bool zip = plan.legs.size() == 1
                  && plan.legs[0].triple.find("windows") != std::string::npos;
    auto dirName = plan.legs.size() == 1
        ? std::format("{}-{}-{}", plan.packageName, plan.packageVersion, plan.legs[0].abiTag)
        : std::format("{}-{}", plan.packageName, plan.packageVersion);
    plan.zip         = zip;
    plan.stagingRoot = plan.projectRoot / "target" / "dist" / dirName;
    plan.archivePath = plan.projectRoot / "target" / "dist"
                     / (dirName + (zip ? ".zip" : ".tar.gz"));
    if (!opts.output.empty()) {
        auto o = std::filesystem::path(opts.output);
        plan.archivePath = o.has_parent_path() ? o
                         : plan.projectRoot / "target" / "dist" / o;
    }

    // ── say what travels, and what does not ───────────────────────────
    //
    // Both lists, always. "What you are publishing" is only half of what the
    // author of a closed-source library needs to see before uploading.
    {
        std::string pub, held;
        for (auto const& p : closure.published) {
            if (!pub.empty()) pub += ", ";
            pub += p.filename().string();
        }
        for (auto const& p : closure.withheld) {
            if (!held.empty()) held += ", ";
            held += p.filename().string();
        }
        mcpp::ui::status("Interface", pub.empty() ? "(headers only)" : pub);
        mcpp::ui::status("Withheld",  held.empty() ? "(nothing)" : held);
    }

    auto out = run_library_pack(plan);
    if (!out) { mcpp::ui::error(out.error().message); return 1; }
    mcpp::ui::status("Packed", out->string());
    return 0;
}

} // namespace mcpp::pack
