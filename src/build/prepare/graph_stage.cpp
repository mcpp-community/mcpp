// graph_stage.cpp -- P4b: what the multi-version rename (Level 1) writes: a
// package's sources staged with their module declarations rewritten, the files
// beside them carried across, and the modules a package declares. Split from
// graph.cpp at the boundary of the rename's files, when the program scopes of
// resolution (design 2026-11-19 §21 A1) took the file past its limit.

module mcpp.build.prepare;
import :state;

import std;
import mcpp.manifest;
import mcpp.source_kind;
import mcpp.modgraph.scanner;
import mcpp.pm.mangle;

namespace mcpp::build {

static std::expected<std::set<std::filesystem::path>, std::string> package_source_files(
        const std::filesystem::path& srcRoot,
        const mcpp::manifest::Manifest& depManifest)
{
        // Resolve the source globs against the original root, falling
        // back to the convention default if the manifest didn't set any.
        std::vector<std::string> globs = depManifest.modules.sources;
        if (globs.empty()) {
            // Was a fourth hand-written copy of the convention default, and it
            // had already drifted: all three assembly extensions were missing,
            // so staging a dependency with .S/.s/.asm silently dropped them.
            globs = mcpp::default_source_globs(
                mcpp::extension_table_for(depManifest.buildConfig.moduleExtensions,
                                          depManifest.buildConfig.deviceExtensions));
        }
        // As the scanner selects (`select_sources`).
        auto sourceFiles = mcpp::modgraph::select_sources(srcRoot, globs);
        if (sourceFiles.empty()) {
            return std::unexpected(std::format(
                "stage: no source files found under '{}' (globs={})",
                srcRoot.string(), globs.size()));
        }
        return sourceFiles;
}

    // Stage a dep's source files into a fresh directory, rewriting their
    // module / import declarations against `rename`. Used by the multi-
    // version mangling fallback (Level 1) so two cross-major copies of
    // the same package can coexist with distinct module names.
    //
    // Headers reached through `[build].include_dirs` are NOT staged — those
    // keep pointing at the original install dir via absolutized include paths.
    //
    // HEADERS BESIDE A SOURCE ARE A DIFFERENT CASE, AND THEY ARE STAGED.
    //
    // `#include "detail.h"` is resolved relative to the directory of the file
    // holding the directive, so moving the source moves the search. No
    // `include_dirs` entry is involved and absolutizing one cannot help: the
    // package never declared a path because it never needed one. Measured
    // before this, on a package whose `src/time.cpp` includes `src/sbi.h`:
    //
    //     target/.mangled/openkal-opensbi/__self__/src/time.cpp:44:10:
    //         fatal error: 'sbi.h' file not found
    //
    // THE DIAGNOSIS THIS PRODUCES POINTS AT THE WRONG THING. The path in it
    // is a staging directory the author never wrote, for a header sitting
    // exactly where the source expects it, and the build that triggered it
    // asked for nothing unusual — two majors of one dependency is a supported
    // arrangement, and this is its most ordinary consequence.
    //
    // What is copied is every file in a directory that contains a staged
    // source and is not itself staged, verbatim: rewriting applies to module
    // declarations, and a header has none. Directories with no staged source
    // are not visited, so this stays proportional to what is being staged.
std::expected<void, std::string> stage_with_rewrite(
        const std::filesystem::path& srcRoot,
        const std::filesystem::path& dstRoot,
        const mcpp::manifest::Manifest& depManifest,
        const std::map<std::string, std::string>& rename)
{
        std::error_code ec;
        std::filesystem::create_directories(dstRoot, ec);
        if (ec) return std::unexpected(std::format(
            "stage: cannot create '{}': {}", dstRoot.string(), ec.message()));

        auto sources = package_source_files(srcRoot, depManifest);
        if (!sources) return std::unexpected(sources.error());

        for (auto const& f : *sources) {
            auto rel = std::filesystem::relative(f, srcRoot, ec);
            if (ec) return std::unexpected(std::format(
                "stage: cannot relativize '{}': {}", f.string(), ec.message()));
            auto dst = dstRoot / rel;
            std::filesystem::create_directories(dst.parent_path(), ec);

            std::ifstream is(f);
            if (!is) return std::unexpected(std::format(
                "stage: cannot read '{}'", f.string()));
            std::stringstream buf; buf << is.rdbuf();
            std::string content = buf.str();

            std::string out = mcpp::pm::rewrite_module_decls(content, rename);
            std::ofstream os(dst);
            if (!os) return std::unexpected(std::format(
                "stage: cannot write '{}'", dst.string()));
            os << out;
        }

        // The files beside those sources, carried across unchanged so a quoted
        // include still finds what it named.
        std::set<std::filesystem::path> sourceDirs;
        for (auto const& f : *sources) sourceDirs.insert(f.parent_path());
        for (auto const& dir : sourceDirs) {
            for (auto const& entry : std::filesystem::directory_iterator(dir, ec)) {
                if (ec) break;
                if (!entry.is_regular_file()) continue;
                if (sources->contains(entry.path())) continue;
                auto rel = std::filesystem::relative(entry.path(), srcRoot, ec);
                if (ec) continue;
                auto dst = dstRoot / rel;
                std::filesystem::create_directories(dst.parent_path(), ec);
                std::filesystem::copy_file(
                    entry.path(), dst,
                    std::filesystem::copy_options::overwrite_existing, ec);
                if (ec) return std::unexpected(std::format(
                    "stage: cannot copy '{}': {}",
                    entry.path().string(), ec.message()));
            }
            ec.clear();
        }
        return {};
}

std::expected<std::vector<std::string>, std::string> declared_modules_for(
        const std::filesystem::path& srcRoot,
        const mcpp::manifest::Manifest& depManifest)
{
        auto sources = package_source_files(srcRoot, depManifest);
        if (!sources) return std::unexpected(sources.error());
        std::vector<std::string> modules;
        for (auto const& file : *sources) {
            std::ifstream is(file);
            if (!is) return std::unexpected(std::format(
                "mangle: cannot read '{}'", file.string()));
            std::stringstream buf; buf << is.rdbuf();
            for (auto& name : mcpp::pm::declared_module_roots(buf.str())) {
                if (std::ranges::find(modules, name) == modules.end())
                    modules.push_back(std::move(name));
            }
        }
        if (modules.empty()) return std::unexpected(std::format(
            "mangle: package '{}' declares no named C++ module to rewrite",
            depManifest.package.name));
        return modules;
}

} // namespace mcpp::build
