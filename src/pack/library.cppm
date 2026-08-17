// mcpp.pack.library — staging a LIBRARY package (interface + prebuilt
// artifacts), as opposed to mcpp.pack's application bundle.
//
// The two share a command and almost nothing else. An application bundle is
// "one executable plus the closure it needs at RUN time"; a library package is
// "the source a consumer must compile, plus the binaries it then links". The
// decision between them is not a flag: it is `[targets.<n>].kind`, so
// `mcpp pack <target>` reads the answer instead of asking for it.
//
// WHAT THIS FILE IS CAREFUL ABOUT
//
//  1. It never GLOBS for a built artifact. Every path comes from the build
//     this run just did. A `target/<triple>/` directory accumulates one
//     subdirectory per fingerprint, and picking "the first one" silently
//     selects a stale binary — measured while prototyping this feature, where
//     it produced an archive missing a translation unit and a consumer-side
//     `undefined reference` that pointed nowhere near the cause.
//
//  2. The archive members it deletes are the objects of the units it is
//     PUBLISHING AS SOURCE, computed by mcpp.pack.interface. Not "every
//     `.m.o`" — an implementation partition is a module unit too, and its
//     object holds the only copy of code nobody else has.
//
//  3. Digests are `fnv1a:` and not `sha256:`. The lock file already speaks
//     that vocabulary, it needs no external tool (`sha256sum` is not on a
//     Windows host), and the question it answers is "has this changed since
//     packaging" — for which a 64-bit content hash is evidence, not a
//     security boundary. The tarball's own `sha256` in an index entry is the
//     integrity check for transport.
//
// Design: .agents/docs/2026-08-17-library-distribution-design.md §2.3.

module;
#include <cstdio>

export module mcpp.pack.library;

import std;
import mcpp.pack.digest;
import mcpp.pack.manifest_emit;
import mcpp.pack.zip;
import mcpp.platform;

export namespace mcpp::pack {

// One target triple's build output, as this run produced it.
struct LibraryLeg {
    std::string           triple;       // canonical
    std::filesystem::path artifact;     // absolute; the .a / .so this build wrote
    std::filesystem::path archiveTool;  // `ar` for THIS leg's toolchain (empty = skip drop)
    std::string           abiTag;
    std::string           buildKey;
    std::string           linkName;     // the -l argument, e.g. "mathkit"
    bool                  shared = false;
};

struct LibraryPackPlan {
    std::filesystem::path projectRoot;
    std::filesystem::path stagingRoot;    // target/dist/<dirname>
    std::filesystem::path archivePath;    // …/<dirname>.tar.gz | .zip
    bool                  writeArchive = true;   // false = `--format dir`
    bool                  zip = false;           // PE targets get a .zip

    std::string namespace_, packageName, packageVersion, builtBy;
    std::string targetName;
    bool        targetShared = false;
    std::string cxxRuntime;
    std::vector<std::string> exportsModules;

    // Absolute paths from the module graph.
    std::vector<std::filesystem::path> interfaceSources;
    // Archive member names to delete (mcpp.pack.interface::published_object_names).
    std::vector<std::string> dropObjects;

    std::filesystem::path includeDir;     // absolute, or empty
    std::vector<std::pair<std::string, std::string>> dependencies;
    std::vector<std::filesystem::path> extras;   // README / LICENSE / [pack].include hits

    std::vector<LibraryLeg> legs;
};

// NOT `Error`. `mcpp.pack` already exports a `mcpp::pack::Error`, and a name
// can only be attached to one module — clang rejects the second outright
// ("cannot be attached to other modules"), and mcpp.pack.library_pipeline
// imports both. GCC accepted it, which is exactly why the Windows leg of CI
// is the one that found this.
struct LibraryPackError { std::string message; };

// Stage, drop, describe, archive. Returns the path a caller should report.
//
// The digests it records come from mcpp.pack.digest, which the CONSUMER also
// uses — one derivation, verified from both ends.
std::expected<std::filesystem::path, LibraryPackError> run_library_pack(const LibraryPackPlan& plan);

} // namespace mcpp::pack

namespace mcpp::pack {

namespace {

std::expected<void, LibraryPackError> copy_into(const std::filesystem::path& src,
                                     const std::filesystem::path& dst)
{
    std::error_code ec;
    std::filesystem::create_directories(dst.parent_path(), ec);
    std::filesystem::copy_file(src, dst,
        std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) return std::unexpected(LibraryPackError{ std::format(
        "cannot copy '{}' -> '{}': {}", src.string(), dst.string(), ec.message()) });
    return {};
}

// Every file under `root`, relative, sorted — deterministic archive order.
std::vector<std::filesystem::path> walk(const std::filesystem::path& root) {
    std::vector<std::filesystem::path> out;
    std::error_code ec;
    for (auto const& e : std::filesystem::recursive_directory_iterator(root, ec)) {
        if (!e.is_regular_file()) continue;
        out.push_back(std::filesystem::relative(e.path(), root, ec));
    }
    std::ranges::sort(out);
    return out;
}

} // namespace

std::expected<std::filesystem::path, LibraryPackError>
run_library_pack(const LibraryPackPlan& plan)
{
    std::error_code ec;
    std::filesystem::remove_all(plan.stagingRoot, ec);
    std::filesystem::create_directories(plan.stagingRoot, ec);
    if (ec) return std::unexpected(LibraryPackError{ std::format(
        "cannot create staging dir '{}': {}", plan.stagingRoot.string(), ec.message()) });

    // ── interface/ ────────────────────────────────────────────────────
    //
    // Flattened, because the names land in `sources` and a package's internal
    // directory layout is not something a consumer should inherit. A collision
    // is refused rather than resolved: silently renaming one of two files
    // called `api.cppm` would make the emitted `sources` point at the wrong
    // one, and the failure would surface in someone else's build.
    std::vector<std::string> interfaceNames;
    {
        std::map<std::string, std::filesystem::path> seen;
        for (auto const& src : plan.interfaceSources) {
            auto name = src.filename().string();
            if (auto it = seen.find(name); it != seen.end()) {
                return std::unexpected(LibraryPackError{ std::format(
                    "two interface units are both called '{}':\n"
                    "  {}\n  {}\n"
                    "A package's interface is published flat, so their names must differ.",
                    name, it->second.string(), src.string()) });
            }
            seen.emplace(name, src);
            if (auto r = copy_into(src, plan.stagingRoot / "interface" / name); !r)
                return std::unexpected(r.error());
            interfaceNames.push_back("interface/" + name);
        }
        std::ranges::sort(interfaceNames);
    }

    // ── include/ ──────────────────────────────────────────────────────
    //
    // Whole, never filtered. A source distribution of this package puts every
    // one of these on its consumers' include path (usage requirements), so
    // trimming here would give the same library a different public surface
    // depending on how it was delivered.
    if (!plan.includeDir.empty() && std::filesystem::is_directory(plan.includeDir, ec)) {
        for (auto const& rel : walk(plan.includeDir))
            if (auto r = copy_into(plan.includeDir / rel, plan.stagingRoot / "include" / rel); !r)
                return std::unexpected(r.error());
    }

    // ── lib/<triple>/ ─────────────────────────────────────────────────
    std::vector<PackageLeg> docLegs;
    for (auto const& leg : plan.legs) {
        if (!std::filesystem::exists(leg.artifact, ec)) {
            return std::unexpected(LibraryPackError{ std::format(
                "the build for '{}' produced no artifact at '{}'",
                leg.triple, leg.artifact.string()) });
        }
        auto name = leg.artifact.filename().string();
        auto dst  = plan.stagingRoot / "lib" / leg.triple / name;
        if (auto r = copy_into(leg.artifact, dst); !r) return std::unexpected(r.error());

        // Delete the objects of the units published as source. The consumer
        // compiles those itself; leaving them in the archive means two
        // definitions of the module initialiser, resolved by link order.
        if (!leg.shared && !plan.dropObjects.empty() && !leg.archiveTool.empty()) {
            std::string cmd = mcpp::platform::shell::quote(leg.archiveTool.string())
                            + " d " + mcpp::platform::shell::quote(dst.string());
            for (auto const& m : plan.dropObjects)
                cmd += " " + mcpp::platform::shell::quote(m);
            auto r = mcpp::platform::process::capture(cmd + " 2>&1");
            if (r.exit_code != 0) {
                return std::unexpected(LibraryPackError{ std::format(
                    "cannot drop published interface objects from '{}' (rc={}): {}",
                    dst.string(), r.exit_code, r.output) });
            }
        }

        docLegs.push_back(PackageLeg{
            .triple   = leg.triple,
            .libFile  = name,
            .linkName = leg.linkName,
            .abiTag   = leg.abiTag,
            .digest   = file_digest(dst),
            .buildKey = leg.buildKey,
            .shared   = leg.shared,
        });
    }

    // ── extras ────────────────────────────────────────────────────────
    for (auto const& x : plan.extras) {
        auto rel = std::filesystem::relative(x, plan.projectRoot, ec);
        if (ec || rel.empty() || rel.string().starts_with("..")) rel = x.filename();
        if (auto r = copy_into(x, plan.stagingRoot / rel); !r) return std::unexpected(r.error());
    }

    // ── mcpp.toml ─────────────────────────────────────────────────────
    {
        PackageDoc doc;
        doc.namespace_      = plan.namespace_;
        doc.name            = plan.packageName;
        doc.version         = plan.packageVersion;
        doc.builtBy         = plan.builtBy;
        doc.interfaceFiles  = interfaceNames;
        doc.hasIncludeDir   = std::filesystem::is_directory(plan.stagingRoot / "include", ec);
        doc.interfaceDigest = plan.interfaceSources.empty() && !doc.hasIncludeDir
            ? std::string{}
            : interface_set_digest(plan.interfaceSources.empty()
                ? [&] {
                      std::vector<std::filesystem::path> hdrs;
                      for (auto const& rel : walk(plan.stagingRoot / "include"))
                          hdrs.push_back(plan.stagingRoot / "include" / rel);
                      return hdrs;
                  }()
                : plan.interfaceSources);
        doc.cxxRuntime      = plan.cxxRuntime;
        doc.exportsModules  = plan.exportsModules;
        doc.targetName      = plan.targetName;
        doc.targetShared    = plan.targetShared;
        doc.legs            = std::move(docLegs);
        doc.dependencies    = plan.dependencies;

        std::ofstream os(plan.stagingRoot / "mcpp.toml", std::ios::binary);
        if (!os) return std::unexpected(LibraryPackError{ std::format(
            "cannot write '{}'", (plan.stagingRoot / "mcpp.toml").string()) });
        os << emit_package_manifest(doc);
    }

    if (!plan.writeArchive) return plan.stagingRoot;

    // ── archive ───────────────────────────────────────────────────────
    std::filesystem::create_directories(plan.archivePath.parent_path(), ec);
    if (plan.zip) {
        std::vector<zip::Entry> entries;
        const auto wrapper = plan.stagingRoot.filename().string();
        for (auto const& rel : walk(plan.stagingRoot)) {
            entries.push_back(zip::Entry{
                .name   = wrapper + "/" + rel.generic_string(),
                .source = plan.stagingRoot / rel,
            });
        }
        if (auto r = zip::write(plan.archivePath, entries); !r)
            return std::unexpected(LibraryPackError{ r.error() });
    } else {
        auto cmd = std::format("tar -czf {} -C {} {}",
            mcpp::platform::shell::quote(plan.archivePath.string()),
            mcpp::platform::shell::quote(plan.stagingRoot.parent_path().string()),
            mcpp::platform::shell::quote(plan.stagingRoot.filename().string()));
        auto r = mcpp::platform::process::capture(cmd + " 2>&1");
        if (r.exit_code != 0)
            return std::unexpected(LibraryPackError{ std::format(
                "tar failed (rc={}): {}", r.exit_code, r.output) });
    }
    return plan.archivePath;
}

} // namespace mcpp::pack
