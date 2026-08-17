// mcpp.pack.prebuilt — the consumer's half of a library package.
//
// A package produced by `mcpp pack` is an ordinary mcpp package: it carries a
// normal `mcpp.toml`, and mcpp's existing "the payload has its own manifest"
// route builds against it with no new code. That is the design, and it is why
// an mcpp too old to know about any of this still WORKS with these packages.
//
// What this module adds is the part an old mcpp cannot do: check that the
// binaries in the package were built for the toolchain about to link them, and
// that the interface sitting next to them is still the one they were built
// from. Both failures are otherwise silent.
//
// THE SECOND ONE IS THE DANGEROUS ONE. Measured on a real build: change one
// line of a shipped interface — swap two `int` members of a struct, which the
// Itanium ABI does not mangle — and the consumer compiles, links, runs, and
// prints transposed data. No diagnostic at any stage. A digest cannot prevent
// a producer from shipping a mismatched pair in the first place (only atomic
// production does that), but it does catch the pair coming apart afterwards,
// which is the case a path dependency or an extracted store is exposed to.
//
// WHAT MARKS A PACKAGE. `provenance` beginning with `mcpp-pack` on any runtime
// artifact. No new key, no new section: the marker is a value in a field that
// has existed since runtime artifacts did.
//
// Design: .agents/docs/2026-08-17-library-distribution-design.md §3.2.

export module mcpp.pack.prebuilt;

import std;
import mcpp.manifest;
import mcpp.pack.abi_tag;
import mcpp.pack.digest;

export namespace mcpp::pack {

inline constexpr std::string_view kPackProvenancePrefix = "mcpp-pack";

// Was this manifest written by `mcpp pack`?
bool is_distribution_package(const mcpp::manifest::Manifest& m);

struct PrebuiltCheck {
    std::filesystem::path packageRoot;
    std::string           packageLabel;   // "acme.mathkit@0.1.0", for diagnostics
    AbiTag                current;        // the tag THIS build would publish
};

// Refuse, with a message the reader can act on, or accept.
//
// Order matters and is the order of the diagnostic: a package that is for
// another architecture entirely should say so before it complains about a
// digest, because the digest is not what the user has to fix.
std::expected<void, std::string>
check_prebuilt(const mcpp::manifest::Manifest& m, const PrebuiltCheck& in);

} // namespace mcpp::pack

namespace mcpp::pack {

namespace {

bool is_library_role(std::string_view role) {
    return role == "static-library" || role == "shared-library";
}

} // namespace

bool is_distribution_package(const mcpp::manifest::Manifest& m) {
    for (auto const& a : m.runtimeConfig.artifacts)
        if (a.provenance.starts_with(kPackProvenancePrefix)) return true;
    return false;
}

std::expected<void, std::string>
check_prebuilt(const mcpp::manifest::Manifest& m, const PrebuiltCheck& in)
{
    std::error_code ec;

    // ── 1. the artifacts are where the manifest says ──────────────────
    //
    // A package whose `lib/` was trimmed in transit links against nothing and
    // fails at the linker, naming a path nobody recognises.
    for (auto const& a : m.runtimeConfig.artifacts) {
        if (!a.provenance.starts_with(kPackProvenancePrefix)) continue;
        auto abs = a.path.is_absolute() ? a.path : in.packageRoot / a.path;
        if (!std::filesystem::exists(abs, ec)) {
            return std::unexpected(std::format(
                "{}: the package declares an artifact at '{}', and it is not there.\n"
                "  The package is incomplete — re-download or re-pack it.",
                in.packageLabel, a.path.string()));
        }
    }

    // ── 2. one of the published tags accepts this toolchain ───────────
    std::vector<std::string> publishedTags;
    bool sawLibrary = false, accepted = false;
    std::vector<TagMismatch> bestRefusal;
    std::string bestRefusalTag;

    for (auto const& a : m.runtimeConfig.artifacts) {
        if (!a.provenance.starts_with(kPackProvenancePrefix)) continue;
        if (!is_library_role(a.role)) continue;
        sawLibrary = true;
        if (a.abi.empty()) {          // nothing declared → nothing to enforce
            accepted = true;
            continue;
        }
        publishedTags.push_back(a.abi);
        auto published = parse_abi_tag(a.abi);
        if (!published) { accepted = true; continue; }   // unreadable → lenient
        auto bad = tag_check(*published, in.current);
        if (bad.empty()) { accepted = true; break; }
        // Keep the CLOSEST refusal to show: the one that disagrees least is
        // the one the user is most likely able to act on.
        if (bestRefusal.empty() || bad.size() < bestRefusal.size()) {
            bestRefusal    = bad;
            bestRefusalTag = a.abi;
        }
    }

    if (sawLibrary && !accepted) {
        std::string tags;
        for (auto const& t : publishedTags) tags += std::format("\n                   {}", t);
        std::string why;
        for (auto const& b : bestRefusal)
            why += std::format("\n    {:<9} needs {}, this build has {}", b.dimension, b.need, b.got);
        return std::unexpected(std::format(
            "{}: no prebuilt artifact matches this toolchain.\n"
            "  your toolchain : {}\n"
            "  published tags :{}\n"
            "  closest is {}, and it differs on:{}\n"
            "  fix: ask the publisher for a build matching your toolchain, or pin\n"
            "       [toolchain] to the one the package was built with.",
            in.packageLabel, in.current.str(), tags, bestRefusalTag, why));
    }

    // ── 3. the interface is the one the binaries were built from ──────
    for (auto const& a : m.runtimeConfig.artifacts) {
        if (!a.provenance.starts_with(kPackProvenancePrefix)) continue;
        if (a.role != "interface" || a.digest.empty()) continue;

        auto dir = a.path.is_absolute() ? a.path : in.packageRoot / a.path;
        std::vector<std::filesystem::path> files;
        if (std::filesystem::is_directory(dir, ec)) {
            for (auto const& e : std::filesystem::recursive_directory_iterator(dir, ec))
                if (e.is_regular_file()) files.push_back(e.path());
        } else if (std::filesystem::is_regular_file(dir, ec)) {
            files.push_back(dir);
        }
        auto now = interface_set_digest(files);
        if (now != a.digest) {
            return std::unexpected(std::format(
                "{}: '{}' does not match what was packaged.\n"
                "  recorded {}\n"
                "  found    {}\n"
                "  The published interface and the prebuilt binaries are produced\n"
                "  together and are not separately replaceable: an edited interface\n"
                "  compiles and links against binaries that no longer agree with it,\n"
                "  and the result is wrong at run time with no diagnostic.\n"
                "  fix: restore the package from its original archive.",
                in.packageLabel, a.path.string(), a.digest, now));
        }
    }
    return {};
}

} // namespace mcpp::pack
