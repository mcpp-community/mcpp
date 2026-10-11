// graph_closures.cpp -- P4b: the lookups the phases after the graph ask of it
// (which providers a consumer sees, which package a user may edit, how a
// deprecated reading is reported). Defined once the graph exists; split from
// graph.cpp at that boundary.

module mcpp.build.prepare;
import :state;

import mcpp.build.prepare_inputs;

import std;
import mcpp.diag;
import mcpp.log;
import mcpp.home;
import mcpp.manifest;
import mcpp.modgraph.scanner;
import mcpp.build.plan;         // path_is_under_any, qualified_package_name
import mcpp.build.directives;   // directive table: mark / fold_private_tail
import mcpp.pm.compat;

namespace mcpp::build {

void step4b_define_lookup_closures(PrepareState& state) {
    state.bareBindingsFor = [&](std::size_t consumer) {
        std::vector<std::string> fqns;
        if (consumer < state.provisionGraph.visible.size())
            for (auto const& pr : state.provisionGraph.visible[consumer]) {
                if (pr.provider >= state.packages.size()) continue;
                auto const& n = state.packages[pr.provider].manifest.package.name;
                if (std::find(fqns.begin(), fqns.end(), n) == fqns.end())
                    fqns.push_back(n);
            }
        return prov::bind_bare_names(fqns);
    };
    // THE NAMES UNDER WHICH ONE PROVIDER IS PUBLISHED TO ONE CONSUMER, derived
    // once for every channel (#647 E4.3). The manifest's `name`, the qualified
    // `namespace.name` when the manifest writes the two apart, and the bare
    // tail where the namespace ladder binds it to this provider for this
    // consumer. `dep_dir`/`dep_linkage` and `dep_bin` used to derive this list
    // separately; #642 added the qualified spelling to the first and the second
    // kept publishing `MCPP_DEP_INSTALLER_BIN_*` alone for a package written
    // `namespace = "spike"`, `name = "installer"`, so
    // `dep_bin("spike.installer", ...)` read nothing.
    state.publishedNamesFor =
        [&](std::size_t provider,
            const std::map<std::string, prov::BareBinding>& bind) {
        std::vector<std::string> out;
        auto const& manifest = state.packages[provider].manifest;
        auto const& canon = manifest.package.name;
        out.push_back(canon);
        if (auto qualified = mcpp::build::qualified_package_name(manifest);
            qualified != canon)
            out.push_back(std::move(qualified));
        if (auto tail = prov::tail_of(canon); tail != canon) {
            auto it = bind.find(tail);
            if (it != bind.end() && it->second.owner == canon)
                out.push_back(std::move(tail));
        }
        return out;
    };

    // A package whose DECLARED targets are all programs (#649 E6). See the
    // worklist, where such a package is not walked into a consumer's graph.
    state.isProgramOnlyPackage = [](const mcpp::manifest::Manifest& pm) {
        if (pm.targetsInferred || pm.targets.empty()) return false;
        return std::ranges::none_of(pm.targets, [](const mcpp::manifest::Target& t) {
            return t.kind == mcpp::manifest::Target::Library
                || t.kind == mcpp::manifest::Target::SharedLibrary;
        });
    };
    // A package some edge asked for programs to SHIP (mcpp#711). Its programs
    // are linked in this plan, so it is scanned and configured here like any
    // library dependency, even when every target it declares is a program.
    state.isArtifactPackage = [&](std::size_t i) {
        return std::ranges::any_of(state.dependencyEdges, [&](const DependencyEdge& e) {
            return e.dependencyPackageIndex == i && !e.requestedArtifacts.empty();
        });
    };
    // Compiled in this plan: not a package of programs, or one whose programs
    // this plan ships.
    state.isWorkspaceMemberPackage = [&](std::size_t i) {
        if (i == 0 || i >= state.packages.size() || !state.workspacePlan()) return false;
        if (state.packages[i].selectedMember) return true;
        const auto& r = state.packages[i].root;
        if (r.lexically_normal() == state.runtimeWorkspaceRoot.lexically_normal()) return true;
        return !workspace_member_of(state, r).empty();
    };
    // A selected workspace member is compiled here whatever its targets are
    // (workspace design 2026-09-29 §15).
    //
    // NOT a package with no target instance (mcpp#797): one reached only
    // through `[build-dependencies]` is compiled in the build program's graph,
    // not here. `targetInstance` is decided by `step6_instances`; until then
    // every package counts.
    state.compilesHere = [&](std::size_t i) {
        if (i < state.packages.size() && i > 0 && state.packages[i].buildTimeOnly)
            return false;
        return i == 0 || !state.isProgramOnlyPackage(state.packages[i].manifest)
            || state.isArtifactPackage(i) || state.packages[i].selectedMember;
    };
    state.editablePackage = [&](std::size_t i) {
        if (i == 0 || i >= state.packages.size()) return true;
        auto const& pkg = state.packages[i];
        if (pkg.selectedMember || state.isWorkspaceMemberPackage(i)) return true;
        // A package fetched into a store is not; a `path` package is, wherever
        // it lies (Cargo draws the same line for its lints).
        return !mcpp::build::path_is_under_any(pkg.root, state.storeRoots)
            && !mcpp::build::path_is_under_any(pkg.root, std::vector{mcpp::home::root()});
    };
    state.reportCompatUse = [&](std::string_view id, std::size_t i, std::string detail) {
        auto const* d = mcpp::pm::compat::find_deprecation(id);
        if (!d) return;
        const bool warn = state.editablePackage(i);
        const auto where = i < state.packages.size()
            ? (state.packages[i].root / "mcpp.toml").string() : std::string{};
        mcpp::pm::compat::record_use(id, where, detail, warn);
        mcpp::log::info("compat", std::format("{} {}: {}{}", id, where, detail,
                                              warn ? "" : " (not editable here; not warned)"));
        if (!warn) return;
        mcpp::diag::warning(d->domain, std::format(
            "{}: {} is {}. Write {} instead.", where, detail, mcpp::pm::compat::deprecation_clause(*d),
            d->canonical));
    };


    state.appendUniquePath =
        [](std::vector<std::filesystem::path>& dirs,
           const std::filesystem::path& dir) -> bool
    {
        if (std::find(dirs.begin(), dirs.end(), dir) != dirs.end()) return false;
        dirs.push_back(dir);
        return true;
    };

    state.appendUniquePaths =
        [&](std::vector<std::filesystem::path>& dirs,
            const std::vector<std::filesystem::path>& additions) -> bool
    {
        bool changed = false;
        for (auto const& dir : additions) {
            changed = state.appendUniquePath(dirs, dir) || changed;
        }
        return changed;
    };

    // "Which compile-visible channels a build.mcpp directive lands in" is a
    // property of the DIRECTIVE TABLE, not of this call site, so both the mark
    // and the fold now live with the table in mcpp.build.directives. This pair
    // used to be defined here and was already incomplete — the comment it
    // replaced admitted that link/source residues stayed at the call sites,
    // which is the #242 two-derivations shape.
    //
    // The fold is PRIVATE by design (Cargo discipline — a build-time program
    // must not widen the package's public interface): privateBuild only, never
    // publicUsage. The after-dirs ride the typed #249 channel, which owns the
    // per-dialect degradations (cl.exe /I, NASM -I).
    using DirectiveMark = mcpp::build::directives::Mark;
    state.markDirectiveTail = [](const mcpp::manifest::Manifest& mm) {
        return mcpp::build::directives::mark(mm);
    };
    state.foldDirectiveTailIntoPrivateBuild =
        [](mcpp::modgraph::PackageRoot& pkg, const mcpp::manifest::Manifest& ran,
           const DirectiveMark& t)
    {
        mcpp::build::directives::fold_private_tail(pkg.privateBuild, ran, t);
    };

    // mcpp#241: the (name → dir) pairs a package's build.mcpp receives as
    // MCPP_DEP_<NAME>_DIR. ONE owner: the dependency loop and the root call
    // site had drifted into two near-identical copies of this, and #355 was
    // about to add a third. Each dependency is emitted under BOTH its
    // canonical name and its namespace-stripped tail, so
    // `mcpp::dep_dir("compat.zlib")` and `mcpp::dep_dir("zlib")` both resolve
    // regardless of which spelling the author used in `deps`.
    //
    // #359: the set is now the consumer's VISIBLE provisions rather than its
    // direct edges, so a re-exported dependency's directory reaches it too.
    // That is what makes a rule package able to find data files belonging to a
    // dependency the user never declared — protoc's well-known .proto files
    // are exactly such a directory, and `grpcgen` reads them through dep_dir.
    //
    // The bare tail is emitted only when the namespace ladder binds it here.
    // Emitting it unconditionally was safe while only the root's own
    // declarations reached build.mcpp; with re-export, two packages that never
    // heard of each other can share a tail and the later emplace_back would
    // silently win.
    // The xlings half of fillDepDirs. Same question ("where did my declared
    // dependency's payload land"), different namespace and store layout, so it
    // cannot ride the mcpp dependency channel — but it must be an INTERFACE on
    // the build.mcpp side for the same reason that one is: a program that
    // reconstructs the store path is coupled to internals mcpp is free to
    // change. See mcpp::build::hostprogram::xpkg_dir.

}

}  // namespace mcpp::build
