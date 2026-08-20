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
import mcpp.pack.relocate;
import mcpp.pack.strip;
import mcpp.source_kind;   // builtin_extension_table — what needs no declaring
import mcpp.pack.zip;
import mcpp.platform;
import mcpp.ui;

export namespace mcpp::pack {

// One target triple's build output, as this run produced it.
struct LibraryLeg {
    std::string           triple;       // canonical
    std::filesystem::path artifact;     // absolute; the .a / .so this build wrote
    std::filesystem::path archiveTool;  // `ar` for THIS leg's toolchain (empty = skip drop)
    std::string           abiTag;
    std::string           buildKey;
    std::string           linkName;     // the -l argument, e.g. "mathkit"
    // How THIS leg's archiver spells "delete a member", from
    // mcpp.toolchain.dialect. `ar` and `llvm-ar` take `d <archive> <member>…`;
    // LIB.EXE takes `/REMOVE:<member>` per member with the archive last. The
    // packer must not pick one — see the dialect's note.
    std::string           removeArg;    // "d" | "/REMOVE:{}"
    bool                  removeArchiveFirst = true;
    // The SONAME the artifact declares, when it declares one. A shared library
    // is FOUND at run time by this name and LINKED by `lib<linkName>.so`, and
    // those are two different filenames — so a package that ships only the
    // built file links fine and then cannot start.
    std::string           soname;
    bool                  shared = false;
    // The IMPORT LIBRARY, on PE only — absolute, empty everywhere else.
    //
    // A PE shared library is two files: the `.dll` the loader opens and an
    // archive of stubs the LINKER consumes. Shipping only the `.dll` gives a
    // package that no linker can use, so the package carries both and the
    // emitted manifest points consumers at this one.
    std::filesystem::path importLibrary;
    // Debug-information removal for THIS leg's toolchain, resolved by the
    // caller from `mcpp::toolchain::binutils_tool`. Per leg for the same
    // reason `archiveTool` is: a fat package's aarch64 leg must not be
    // stripped by the x86_64 host's tool.
    mcpp::pack::StripTools stripTools;
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

    // Remove debug information from the shipped artifacts (mcpp.pack.resolve_strip).
    bool                  strip = true;
    // Where the separated `*.debug` files go, absolute. Empty = do not separate.
    std::filesystem::path debugDir;

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

// The command that deletes `members` from `archive`, spelled for whichever
// archiver `tool` is.
//
// Exported ONLY so it can be tested. mcpp's Windows CI archives with clang's
// `llvm-ar`, which takes the GNU spelling, so the MSVC branch below is never
// executed by any job — and it is the branch that differs in both directions:
//
//   ar        one verb, then the archive, then every member
//               ar d libmathkit.a mathkit.m.o api.m.o
//   lib.exe   one FLAG PER MEMBER, and the archive comes LAST
//               lib.exe /REMOVE:mathkit.m.o /REMOVE:api.m.o mathkit.lib
//
// Pinning the two constants in dialect.cppm is not enough: the order they are
// assembled in is a third fact, and it lives here.
std::string archive_remove_command(const std::filesystem::path& tool,
                                   const std::filesystem::path& archive,
                                   const std::vector<std::string>& members,
                                   std::string_view removeArg,
                                   bool archiveFirst);

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

std::string archive_remove_command(const std::filesystem::path& tool,
                                   const std::filesystem::path& archive,
                                   const std::vector<std::string>& members,
                                   std::string_view removeArg,
                                   bool archiveFirst)
{
    std::string cmd = mcpp::platform::shell::quote(tool.string());
    // `{}` in removeArg means one flag per member (lib.exe /REMOVE:<m>); its
    // absence means one verb followed by every member (ar d <archive> <m>...).
    const bool perMember = removeArg.find("{}") != std::string_view::npos;
    auto member_words = [&] {
        std::string w;
        for (auto const& m : members) {
            if (!perMember) { w += " " + mcpp::platform::shell::quote(m); continue; }
            std::string arg{ removeArg };
            arg.replace(arg.find("{}"), 2, m);
            w += " " + mcpp::platform::shell::quote(arg);
        }
        return w;
    };
    if (archiveFirst) {
        if (!perMember) cmd += " " + std::string(removeArg);
        cmd += " " + mcpp::platform::shell::quote(archive.string());
        cmd += member_words();
    } else {
        cmd += member_words();
        cmd += " " + mcpp::platform::shell::quote(archive.string());
    }
    return cmd;
}

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

        // The import library travels beside the .dll, and it is what the emitted
        // manifest names as the link input. Refused rather than skipped when
        // missing: a PE shared package without one links for nobody, and finding
        // that out at the consumer's link step points at the consumer.
        std::string linkFile = name;
        if (!leg.importLibrary.empty()) {
            if (!std::filesystem::exists(leg.importLibrary, ec)) {
                return std::unexpected(LibraryPackError{ std::format(
                    "the build for '{}' produced no import library at '{}'.\n"
                    "  A PE shared library is two files, and consumers link the "
                    "second one — shipping only the .dll gives a package no "
                    "linker can use.",
                    leg.triple, leg.importLibrary.string()) });
            }
            linkFile = leg.importLibrary.filename().string();
            if (auto r = copy_into(leg.importLibrary, dst.parent_path() / linkFile); !r)
                return std::unexpected(r.error());
        }

        // Delete the objects of the units published as source. The consumer
        // compiles those itself; leaving them in the archive means two
        // definitions of the module initialiser, resolved by link order.
        // No archiver but objects to drop would leave the published interface's
        // objects inside the archive — two definitions of the module
        // initialiser, resolved by link order. Skipping that quietly is the
        // exact failure class this feature exists to remove, so it is refused.
        if (!leg.shared && !plan.dropObjects.empty() && leg.archiveTool.empty()) {
            return std::unexpected(LibraryPackError{ std::format(
                "no archiver was resolved for {}, so the published interface's "
                "objects cannot be removed from '{}'.\n"
                "  Shipping them leaves two definitions of each published "
                "module's initialiser in the consumer's link.",
                leg.triple, name) });
        }
        if (!leg.shared && !plan.dropObjects.empty()) {
            const auto cmd = archive_remove_command(
                leg.archiveTool, dst, plan.dropObjects,
                leg.removeArg, leg.removeArchiveFirst);
            auto r = mcpp::platform::process::capture(cmd + " 2>&1");
            if (r.exit_code != 0) {
                return std::unexpected(LibraryPackError{ std::format(
                    "cannot drop published interface objects from '{}' (rc={}).\n"
                    "  command: {}\n"
                    "  output : {}\n"
                    "  Leaving them in would give the consumer two definitions of "
                    "each published module's initialiser.",
                    dst.string(), r.exit_code, cmd, r.output) });
            }
        }

        // ── THE ORDER, AND IT IS NOT FREE ─────────────────────────────
        //
        // Four steps change the artifact's bytes and one records them. They
        // are written here, once, because every one of them is a way to ship
        // a package whose manifest describes a file that is not the one in the
        // archive:
        //
        //   1. copy            (above)
        //   2. drop objects    (above) — changes the archive's members
        //   3. relocate        — remove the build machine's loader paths
        //   4. strip           — remove debug info (and separate it)
        //   5. soname alias    — a symlink to, or a COPY OF, the FINAL file
        //   6. digest          — the package's evidence, over the final bytes
        //
        // 5 after 3–4 is the one that used to be wrong in a way nothing could
        // see: the alias' copy fallback read `leg.artifact` (the BUILD tree's
        // file), which was byte-identical only because nothing here modified
        // anything. With 3 and 4 in place it would ship an unrelocated,
        // unstripped library under the exact name the loader asks for — and
        // only on the machines where `create_symlink` fails, which is where
        // nobody looks.

        // 3. The build machine does not travel. See mcpp.pack.relocate for why
        //    this removes the tag rather than rewriting it to `$ORIGIN`.
        {
            auto r = mcpp::pack::relocate::strip_search_paths(dst);
            if (!r) return std::unexpected(LibraryPackError{ std::format(
                "cannot make '{}' relocatable: {}", dst.string(), r.error()) });
            using O = mcpp::pack::relocate::Outcome;
            if (r->outcome == O::Removed) {
                std::string what;
                for (auto const& p : r->paths) { if (!what.empty()) what += " "; what += p; }
                mcpp::ui::status("Relocated",
                    std::format("{} ({} dropped from the loader search path)",
                                name, what.empty() ? std::string("build-machine paths") : what));
            } else if (r->outcome == O::Reported && !r->paths.empty()) {
                // Mach-O: read, not rewritten. Saying nothing here would let a
                // `.dylib` carry the publisher's LC_RPATH into a package while
                // the ELF leg beside it is clean.
                std::string what;
                for (auto const& p : r->paths) { if (!what.empty()) what += ", "; what += p; }
                mcpp::ui::warning(std::format(
                    "{} carries LC_RPATH entries that mcpp does not yet rewrite: {}\n"
                    "  If any of them names a directory on THIS machine, the package is "
                    "not relocatable. Remove it with `install_name_tool -delete_rpath "
                    "<path> <file>` before publishing.", name, what));
            } else if (r->outcome == O::Unanalysed) {
                mcpp::ui::warning(std::format(
                    "{} could not be checked for build-machine loader paths: {}",
                    name, r->note));
            }
        }

        // 4. Debug information does not travel either — unless asked.
        if (plan.strip) {
            const auto shape = leg.shared ? mcpp::pack::ArtifactShape::SharedLibrary
                                          : mcpp::pack::ArtifactShape::StaticArchive;
            // PER LEG, mirroring `lib/<triple>/`. A fat package's legs share an
            // artifact NAME — `libmathkit-shared.so` for both the gnu and the
            // musl leg is the normal case, not a corner one — so a flat debug
            // directory would have the second leg overwrite the first, and the
            // first artifact's `.gnu_debuglink` would then resolve to the other
            // target's symbols. Silently.
            const auto legDebugDir = plan.debugDir.empty()
                ? std::filesystem::path{} : plan.debugDir / leg.triple;
            auto r = mcpp::pack::strip_artifact(dst, shape, leg.stripTools, legDebugDir);
            if (!r) return std::unexpected(LibraryPackError{ r.error() });
            if (r->outcome == mcpp::pack::StripOutcome::Stripped) {
                mcpp::ui::status("Stripped", std::format("{}  {} → {} bytes{}",
                    name, r->before, r->after,
                    r->debugFile.empty() ? std::string{}
                                         : std::format("  (debug: {})",
                                                       r->debugFile.filename().string())));
            }
            // The IMPORT LIBRARY is deliberately not stripped: it is an archive
            // of linker stubs with no debug information to remove, and dh_strip
            // makes the same exclusion.
        }

        // 5. A shared library needs BOTH of its names present.
        //
        // `-lmathkit-shared` resolves `libmathkit-shared.so` at link time, but
        // the object records `SONAME libmathkit.so.1`, and that is the name the
        // loader asks for. Ship only the built file and the consumer links,
        // then fails to start — mcpp's own runtime-closure check reports
        // "libmathkit.so.1 not found on the search path this artifact will
        // actually use", which is how this was caught.
        //
        // A symlink is what a distribution ships; a copy is the fallback for
        // filesystems (and archives) that cannot carry one — and it copies
        // `dst`, never `leg.artifact`. See the order note above.
        if (leg.shared && !leg.soname.empty() && leg.soname != name) {
            auto alias = dst.parent_path() / leg.soname;
            std::error_code linkEc;
            std::filesystem::remove(alias, linkEc);
            std::filesystem::create_symlink(name, alias, linkEc);
            if (linkEc)
                if (auto r = copy_into(dst, alias); !r) return std::unexpected(r.error());
        }

        // 6. Evidence, over the bytes that actually ship.
        docLegs.push_back(PackageLeg{
            .triple   = leg.triple,
            .libFile  = name,
            .linkFile = linkFile,
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
        // Whatever the published set uses beyond the built-in `.cppm`.
        // Computed from the FILES, so it cannot disagree with `sources` above.
        {
            const auto builtin = mcpp::builtin_extension_table();
            std::set<std::string> extras;
            for (auto const& src : plan.interfaceSources) {
                auto ext = src.extension().string();
                if (ext.empty()) continue;
                if (std::ranges::find(builtin.moduleInterface, ext)
                    != builtin.moduleInterface.end()) continue;
                extras.insert(ext);
            }
            doc.moduleExtensions.assign(extras.begin(), extras.end());
        }
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
        // `--force-local` on Windows, and it is not optional there: GNU tar
        // reads `C:/path/x.tar.gz` as the rsh form `host:path`, tries to resolve
        // a machine called `C`, and fails with
        //   tar (child): Cannot connect to C: resolve failed
        // which names neither tar's argument nor the drive letter as the cause.
        //
        // Reached only by a package with more than one leg: a single PE leg is
        // written as a zip, so a Windows host never ran this path until a fat
        // package existed. Same fix, same reason, in pack.cppm's make_tarball.
        auto cmd = std::format("tar {}-czf {} -C {} {}",
            mcpp::platform::is_windows ? "--force-local " : "",
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
