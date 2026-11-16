// package_root.cpp -- P4b: a package's root, the snapshot of its build inputs
// (include directories, their private part, flags) the graph records for it.
// Split from graph.cpp at that boundary; graph.cpp, its steps and the phases
// after it take a root from here.

module mcpp.build.prepare;
import :state;

import mcpp.build.prepare_inputs;

import std;
import mcpp.diag;
import mcpp.manifest;
import mcpp.modgraph.glob;
import mcpp.modgraph.graph;

namespace mcpp::build {

static std::vector<std::filesystem::path> expandIncludeDirs(
        PrepareState& state,
        const std::filesystem::path& packageRoot,
        const mcpp::manifest::Manifest& manifest)
{
        std::vector<std::filesystem::path> dirs;
        for (auto const& inc : manifest.buildConfig.includeDirs) {
            if (inc.is_absolute()) {
                // Native spelling: a TOML `C:/SDL2/include` stays mixed on
                // MSVC and leaks into the CDB's -I otherwise. Direct
                // make_preferred — no generic_string round trip, which can
                // throw for names the ANSI codepage cannot spell (mcpp#230).
                auto n = inc;
                n.make_preferred();
                state.appendUniquePath(dirs, std::move(n));
                continue;
            }
            for (auto& dir : mcpp::modgraph::expand_dir_glob(
                     packageRoot, inc.generic_string())) {
                state.appendUniquePath(dirs, dir);
            }
        }
        return dirs;
}

    // #249: same glob expansion for `include_dirs_after` (the -idirafter
    // channel — searched after the toolchain's system dirs).
static std::vector<std::filesystem::path> expandIncludeDirsAfter(
        PrepareState& state,
        const std::filesystem::path& packageRoot,
        const mcpp::manifest::Manifest& manifest)
{
        std::vector<std::filesystem::path> dirs;
        for (auto const& inc : manifest.buildConfig.includeDirsAfter) {
            if (inc.is_absolute()) {
                auto n = inc;
                n.make_preferred();
                state.appendUniquePath(dirs, std::move(n));
                continue;
            }
            for (auto& dir : mcpp::modgraph::expand_dir_glob(
                     packageRoot, inc.generic_string())) {
                state.appendUniquePath(dirs, dir);
            }
        }
        return dirs;
}

    // The same expansion for `private_include_dirs`, so a private entry may be
    // a glob and still name exactly the directories it expands to.
static std::vector<std::filesystem::path> expandPrivateIncludeDirs(
        PrepareState& state,
        const std::filesystem::path& packageRoot,
        const mcpp::manifest::Manifest& manifest)
{
        std::vector<std::filesystem::path> dirs;
        for (auto const& inc : manifest.buildConfig.privateIncludeDirs) {
            if (inc.is_absolute()) {
                auto n = inc;
                n.make_preferred();
                state.appendUniquePath(dirs, std::move(n));
                continue;
            }
            for (auto& dir : mcpp::modgraph::expand_dir_glob(
                     packageRoot, inc.generic_string())) {
                state.appendUniquePath(dirs, dir);
            }
        }
        return dirs;
}

std::expected<mcpp::modgraph::PackageRoot, std::string> makePackageRoot(
        PrepareState& state,
        const std::filesystem::path& packageRoot,
        const mcpp::manifest::Manifest& manifest)
{
        // THE SNAPSHOT READS A NORMALISED MANIFEST; IT DOES NOT NORMALISE ONE.
        //
        // Every merge that feeds a package's build inputs (workspace
        // inheritance, the conditional `[target.<sel>.build]` sections) runs
        // at the package's LOAD site, and `fold_build_defines_into_flags` runs
        // after all of them. This lambda only captures the result.
        //
        // `[workspace.build]` inheritance used to run here (#539). The root
        // had already inherited at load time, so it received the workspace
        // entries twice; a member reached as a sibling's `path` dependency
        // inherited after its `defines` had been folded, so the workspace
        // `defines` never reached its compile lines (#690). Both follow from
        // performing a merge at the snapshot, and both are removed by
        // performing it at the load site, where the root already did.
        //
        // The post-condition below is what keeps it removed: a merge placed
        // after the fold leaves `defines` non-empty here, and the build stops
        // with an internal error instead of dropping the macros in silence.
        if (auto unfolded = unfolded_defines_error(manifest))
            return std::unexpected(*unfolded);

        mcpp::modgraph::PackageRoot pkg;
        pkg.root = packageRoot;
        pkg.manifest = manifest;
        pkg.usageResolved = true;

        pkg.privateBuild.includeDirs = expandIncludeDirs(state, packageRoot, manifest);
        pkg.privateBuild.includeDirsAfter = expandIncludeDirsAfter(state, packageRoot, manifest);
        pkg.privateBuild.cflags = manifest.buildConfig.cflags;
        pkg.privateBuild.cxxflags = manifest.buildConfig.cxxflags;
        // NOT `= privateBuild` ANY MORE — a package may now say which of
        // its include directories stop at its own boundary.
        //
        // This line took the whole set for as long as the two were the same
        // set, which they are for almost every package. The one shape where
        // they are not is a package that vendors a library with an internal
        // header overlay: musl's `src/include` adds `hidden`, `weak` and
        // `weak_alias` for musl's own sources, and publishing it hands those
        // names to every consumer. See BuildInputs::privateIncludeDirs.
        //
        // THE FILTER IS APPLIED AFTER GLOB EXPANSION, so a private entry may
        // itself be a glob and still name exactly the directories it expands
        // to. Comparing the unexpanded spellings would let `musl/src/*` be
        // published because it is not literally equal to `musl/src/include`.
        {
            const auto privateExpanded =
                expandPrivateIncludeDirs(state, packageRoot, manifest);
            for (auto const& d : pkg.privateBuild.includeDirs)
                if (std::ranges::find(privateExpanded, d) == privateExpanded.end())
                    pkg.publicUsage.includeDirs.push_back(d);

            // AN ENTRY THAT WITHHOLDS NOTHING IS REPORTED, because the way
            // it fails is the very defect this key exists to prevent: a
            // directory the author believes is private stays published, and
            // nothing about the build looks different until a consumer trips
            // over a name months later.
            //
            // A WARNING AND NOT AN ERROR, for consistency with `include_dirs`
            // itself: that key silently ignores a glob matching nothing, and a
            // conditional manifest can legitimately name a directory that
            // exists on one platform only. Refusing here would be stricter
            // than the list this one filters.
            for (auto const& want : privateExpanded) {
                if (std::ranges::find(pkg.privateBuild.includeDirs, want)
                    != pkg.privateBuild.includeDirs.end())
                    continue;
                mcpp::diag::warning("manifest", std::format(
                    "package '{}': `private_include_dirs` names '{}', which is "
                    "not among this package's `include_dirs`.\n"
                    "       It withholds nothing — `private_include_dirs` says "
                    "which entries OF `include_dirs`\n"
                    "       stop at this package's boundary, and an entry that "
                    "is not one of them is published\n"
                    "       exactly as before.",
                    manifest.package.name, want.generic_string()));
            }
        }
        pkg.publicUsage.includeDirsAfter = pkg.privateBuild.includeDirsAfter;
        pkg.linkUsage.ldflags = manifest.buildConfig.ldflags;
        return pkg;
}

} // namespace mcpp::build
