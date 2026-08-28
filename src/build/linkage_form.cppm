// mcpp.build.linkage_form — static or shared is the CONSUMER's question.
//
// WHAT THIS DECIDES
//
// mcpp used to give a dependency exactly one shape, chosen by the package
// author: `kind = "lib"` merged its objects straight into the consumer's link,
// `kind = "shared"` built a real shared library. The consumer had no say. That
// is the wrong owner for the decision — whether a library should be a separate
// file at run time is a property of the PROGRAM being built, not of the source
// it is built from — and it is why two packages could quietly supply the same
// library in two different shapes with nothing to say so (issue #519).
//
// THE INVARIANT THIS SERVES
//
//   One library, one provider, one form.
//
// This module enforces it over what mcpp DECIDES. `mcpp.build.symbol_provision`
// enforces the same sentence over what the linker PRODUCES, which is the only
// altitude that can see a library mcpp never knew about. Two domains, one
// invariant.
//
// THREE LAYERS, AND THE FIRST ONE IS NEVER ASKED
//
//   Admissible   the set of forms a package can take here — DERIVED
//   Request      what the consumer wants — one new manifest key
//   DepLinkage   the answer — a total function of the two
//
// Every input to `resolve` except the request already existed in the manifest:
// `sources`, the `-L` in `ldflags`, `targets.*.kind`,
// `runtime.artifacts[].role`, the target format, the libc linkage. This axis
// does not add information to a manifest; it asks a question nobody was being
// asked.
//
// WHY THERE IS NO `Mechanism` LAYER
//
// Because there is nothing to write. A resolved `Shared` is materialised by
// setting the package's target kind, and every emitter mcpp already has —
// ELF soname and `$ORIGIN`, PE import library and auto-`.def`, Mach-O install
// name — then applies unchanged. Adding a fourth layer would mean writing a
// second copy of machinery that is already correct on three formats.
//
// Design: .agents/docs/2026-08-28-issue519-dependency-linkage-form.md §4.

export module mcpp.build.linkage_form;

import std;

export namespace mcpp::build::linkage_form {

// NOT `Form`. `mcpp.build.loader_contract` already exports
// `enum class Form { Executable, SharedLibrary, NotElf }`, and
// `runtime_validation` reads both — two spellings of nearly the same word
// meeting in one translation unit is how a reader stops trusting either. This
// is named after the key the user writes, so one concept has one word in the
// manifest, in the code and in the diagnostics.
enum class DepLinkage { Static, Shared };

std::string_view to_string(DepLinkage linkage);
std::optional<DepLinkage> parse(std::string_view value);

// What a package is permitted to be, HERE — the intersection of what it can
// be and what it is constrained to be.
//
// A SET rather than two layers. The first draft separated "capability" from
// "constraint" by analogy with mcpp.build.distribution's Contract/Mechanism
// split, but that split exists there because the two refusals say different
// things ("you asked X and got Y" versus "X has no mechanism on this
// platform"). Here both refusals are the same sentence, so the distinction
// bought a second traversal and nothing else.
struct Admissible {
    bool        staticOk = true;
    bool        sharedOk = false;
    // Why not, when `sharedOk` is false. Always populated in that case: a
    // refusal a user cannot act on is worse than no feature.
    std::string sharedRefusal;

    bool allows(DepLinkage linkage) const {
        return linkage == DepLinkage::Static ? staticOk : sharedOk;
    }
};

// Everything about ONE package that bears on the answer. All of it already
// exists in that package's manifest; this struct just names the subset.
struct PackageFacts {
    std::string label;              // "compat.zlib@1.3.2" — diagnostics only

    // mcpp compiles this package's own sources.
    bool hasSources = false;

    // The author wrote `kind = "shared"`. Read as a CONSTRAINT ("this must be
    // the only copy in the process"), because that is the only reason anyone
    // has ever written it — a library another library will `dlopen`.
    //
    // ⚠️ The mirror image is NOT true: `kind = "lib"` is the parser's DEFAULT,
    // written by 84 of 130 packages in mcpp-index as boilerplate. Reading it
    // as "must be static" would freeze the entire ecosystem out of this axis.
    // Absence of a constraint is not a constraint.
    bool declaredShared = false;

    // The package's resolved `ldflags` name link inputs mcpp did not compile
    // (see `carries_foreign_link_inputs`).
    bool carriesForeignLinkInputs = false;

    // A package produced by `mcpp pack`: its forms are the ones it SHIPS, and
    // no source exists to build another.
    bool isDistribution = false;
    bool shipsStatic = false;
    bool shipsShared = false;
};

// Everything about the TARGET that bears on the answer.
struct TargetFacts {
    // The target has a dynamic loader. False for a freestanding image, where
    // there is no shared-library rule at all — nothing loads anything.
    bool hasLoader = true;

    // The image links its C library statically (`-static`). A fully static
    // executable has no interpreter and cannot load a shared object, so the
    // libc axis and this one are NOT independent — a fact that is easy to
    // miss because they are separate keys, and one that reaches the most
    // common musl configuration, where `linkage = "static"` is the default.
    bool fullStaticLibc = false;
};

// Does this flag list bring link inputs that mcpp did not compile?
//
// `-L` is the marker, and it is exact rather than heuristic: a package that
// ships prebuilt archives has to point the linker at them, and a package that
// merely names a HOST library (`-lm`, `-lpthread`, `-lws2_32`) does not. Over
// mcpp-index, 31 packages carry ldflags and exactly 4 carry `-L`; those 4 are
// precisely the ones with prebuilt binaries inside them. Making such a package
// shared would wrap somebody else's non-PIC archive in a shared object.
bool carries_foreign_link_inputs(std::span<const std::string> ldflags);

Admissible admissible(const PackageFacts& package, const TargetFacts& target);

// What the consumer asked for.
struct Request {
    // `[build] dependency_linkage`, overridable by `[profile.*]`.
    DepLinkage whole = DepLinkage::Static;
    // Did a human write the whole-graph value, or is it just the default?
    // Decides whether a refusal SPEAKS: mcpp promised nothing when nobody
    // asked, and warning on every build about a default is noise.
    bool       wholeIsExplicit = false;
    // Per-package, from the dependency edge. Keyed by the same label as
    // `PackageFacts::label` and by the bare package name.
    std::map<std::string, DepLinkage, std::less<>> perPackage;
};

struct Resolution {
    DepLinkage  linkage = DepLinkage::Static;
    // Non-empty exactly when the answer differs from an EXPLICIT request.
    std::string diagnostic;
};

Resolution resolve(const PackageFacts& package, const Admissible& admissible,
                   const Request& request);

// The one derivation of "does this build need position-independent code".
//
// ⚠️ It used to be a scan of the finished plan for a shared link unit, in
// `flags.cppm`, and it was ABSENT FROM THE CACHE KEY. That was survivable
// while a package's form was fixed by its author; it stops being survivable
// the moment a consumer can ask for the shared form, because the same cache
// entry then serves non-PIC objects to a link that puts them in a shared
// object — a hard `relocation R_X86_64_32S ... can not be used when making a
// shared object` on an input nobody edited.
//
// Deciding it here, from the resolved forms, is what lets the key carry it.
bool needs_pic(std::span<const DepLinkage> resolved, bool anyOwnSharedTarget);

} // namespace mcpp::build::linkage_form

namespace mcpp::build::linkage_form {

std::string_view to_string(DepLinkage linkage) {
    return linkage == DepLinkage::Shared ? "shared" : "static";
}

std::optional<DepLinkage> parse(std::string_view value) {
    if (value == "static") return DepLinkage::Static;
    if (value == "shared") return DepLinkage::Shared;
    return std::nullopt;
}

bool carries_foreign_link_inputs(std::span<const std::string> ldflags) {
    // Three spellings, because all three reach the linker: `-Llib`, the
    // two-token `-L lib`, and `-Wl,-Llib`. A bare `-L` as the last element is
    // still an intent to add a search path even though its argument is
    // missing, so it counts.
    for (auto const& flag : ldflags) {
        if (flag.starts_with("-L")) return true;
        if (flag.starts_with("-Wl,-L")) return true;
        if (flag.starts_with("-Wl,--library-path")) return true;
        // MSVC-dialect spelling, for a package written against that ABI.
        if (flag.starts_with("/LIBPATH:")) return true;
    }
    return false;
}

Admissible admissible(const PackageFacts& package, const TargetFacts& target) {
    // Order matters and is the order of the diagnostic: a reason the user
    // could not have changed by editing the package comes first, because it
    // is not the package they need to look at.
    if (!target.hasLoader) {
        return Admissible{ .staticOk = true, .sharedOk = false,
            .sharedRefusal = "this target has no dynamic loader, so there is "
                             "nothing that could load a shared library" };
    }
    if (target.fullStaticLibc) {
        return Admissible{ .staticOk = true, .sharedOk = false,
            .sharedRefusal = "this image links its C library statically "
                             "(`linkage = \"static\"`), and a static "
                             "executable has no interpreter to load a shared "
                             "library with" };
    }

    if (package.isDistribution) {
        // A packaged library has no source to build the other form from. Its
        // admissible set is exactly what is inside it, which its own manifest
        // already records as `[[runtime.artifacts]] role`.
        Admissible out;
        out.staticOk = package.shipsStatic;
        out.sharedOk = package.shipsShared;
        if (!out.sharedOk)
            out.sharedRefusal = std::format(
                "{} is a packaged library and ships only a static leg",
                package.label);
        return out;
    }

    if (package.declaredShared)
        return Admissible{ .staticOk = false, .sharedOk = true };

    if (!package.hasSources) {
        return Admissible{ .staticOk = true, .sharedOk = false,
            .sharedRefusal = std::format(
                "{} builds none of its own sources, so mcpp has no objects to "
                "make a shared library from", package.label) };
    }
    if (package.carriesForeignLinkInputs) {
        return Admissible{ .staticOk = true, .sharedOk = false,
            .sharedRefusal = std::format(
                "{} brings its own prebuilt link inputs (its `ldflags` carry a "
                "`-L`), which mcpp cannot place inside a shared library it "
                "builds", package.label) };
    }
    return Admissible{ .staticOk = true, .sharedOk = true };
}

Resolution resolve(const PackageFacts& package, const Admissible& admissible,
                   const Request& request) {
    // A per-package request is always explicit — someone wrote it on the edge.
    bool explicitRequest = request.wholeIsExplicit;
    DepLinkage wanted = request.whole;
    if (auto it = request.perPackage.find(package.label);
        it != request.perPackage.end()) {
        wanted = it->second;
        explicitRequest = true;
    }

    if (admissible.allows(wanted)) return Resolution{ .linkage = wanted };

    // Not allowed. There is exactly one other form, and the admissible set is
    // never empty by construction — `staticOk` is false only for a package the
    // author constrained to shared, and that case allows Shared.
    const DepLinkage fallback = admissible.sharedOk ? DepLinkage::Shared
                                                    : DepLinkage::Static;
    Resolution out{ .linkage = fallback };
    // SPEAK ONLY FOR A BROKEN PROMISE. When the whole-graph value is mcpp's
    // own default, nobody asked for anything and there is nothing to report;
    // saying so on every build would put a warning on correct manifests that
    // their authors cannot act on. Same rule mcpp.build.distribution applies
    // to its contract defaults.
    if (explicitRequest && wanted != fallback) {
        out.diagnostic = std::format(
            "{} is linked as a {} library: {}",
            package.label, to_string(fallback),
            admissible.sharedRefusal.empty()
                ? std::string("the requested form is not available here")
                : admissible.sharedRefusal);
    }
    return out;
}

bool needs_pic(std::span<const DepLinkage> resolved, bool anyOwnSharedTarget) {
    if (anyOwnSharedTarget) return true;
    return std::ranges::any_of(resolved, [](DepLinkage linkage) {
        return linkage == DepLinkage::Shared;
    });
}

} // namespace mcpp::build::linkage_form
