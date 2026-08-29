// mcpp.toolchain.registry — the two-axis toolchain identity model and its
// payload mapping.
//
// Identity (design §4.1–§4.3): a toolchain is `family@version` (family ∈
// gcc | llvm | msvc), a target is a canonical Triple (triple.cppm). The two
// axes are orthogonal: "cross", "musl" and "mingw" are NOT names — the
// variant lives in the target's env segment, and cross is the host≠target
// relation. Which xim PACKAGE serves a (family, version, target, host)
// combination is a data mapping below — that's the distribution layer, where
// names like `mingw-cross-gcc` are current identity (not legacy) and stay.
//
// Legacy spellings (musl-gcc, gcc@V-musl, mingw, mingw-cross, clang,
// <triple>-gcc) are normalized by mcpp.toolchain.compat before this module
// ever sees them; core code deals in canonical form only.

export module mcpp.toolchain.registry;

import std;
import mcpp.platform;
import mcpp.xlings;
import mcpp.toolchain.clang;
import mcpp.toolchain.compat;
import mcpp.toolchain.gcc;
import mcpp.toolchain.llvm;
import mcpp.toolchain.model;
import mcpp.toolchain.msvc;
import mcpp.toolchain.triple;

export namespace mcpp::toolchain {

// ⚠️ A FAMILY IS A COMPILER, AND `openkal-llvm` WAS NOT ONE.
//
// It named the same llvm payload as `Llvm` and existed to carry one fact: that
// a project's headers, C library, C++ runtime and platform implementation come
// from packages rather than from a payload beside the compiler. A toolchain
// family was the wrong object to carry it. That fact belongs to the dependency
// graph, is only knowable after the graph is resolved, and says nothing about
// which compiler is running — the same packages compiled by gcc are the
// intended second consumer, and expressing them through a family name would
// have required a second name for the same fact.
//
// `mcpp.targetside` resolves it per layer, from what packages declare, at the
// point where the graph exists. The enumerator is therefore gone and the
// SPELLING remains, normalised to `llvm` in `compat.cppm`, so a manifest or a
// config written against it still resolves.
//
// Keeping the enumerator had a visible cost beyond the dead branch: the
// available-toolchain listing enumerates families, so one payload under two
// family names appeared twice — and, since installation is recorded per family,
// the second copy was reported as NOT INSTALLED and offered for installation to
// users who already had it.
enum class Family { Gcc, Llvm, Msvc };


inline std::string_view family_name(Family f) {
    switch (f) {
        case Family::Gcc:  return "gcc";
        case Family::Llvm: return "llvm";
        case Family::Msvc: return "msvc";
    }
    return "?";
}

struct ToolchainSpec {
    Family          family = Family::Gcc;
    std::string     version;      // numeric (possibly partial), or "system"
    triple::Triple  target;       // empty = host
    // One-line canonical hint when the input used a legacy spelling
    // (compat.cppm); empty otherwise. Printed at most once per process by
    // print_compat_hint().
    std::string     compatHint;

    bool is_host_target() const { return target.empty(); }

    // "gcc@16.1.0" — the toolchain axis alone (config persistence, matching).
    std::string spec_str() const {
        return std::format("{}@{}", family_name(family), version);
    }

    // "gcc@16.1.0" or "gcc@16.1.0 → x86_64-windows-gnu" — user-facing.
    std::string display() const {
        if (target.empty()) return spec_str();
        return std::format("{} → {}", spec_str(), target.str());
    }
};

struct XimToolchainPackage {
    std::string                     ximName;
    std::string                     ximVersion;
    std::string                     displaySpec;   // canonical, from the spec
    std::vector<std::string>        frontendCandidates;
    bool                            needsGccPostInstallFixup = false;

    std::string target() const {
        return std::format("xim:{}@{}", ximName, ximVersion);
    }

    std::string display_spec() const { return displaySpec; }
};

std::expected<ToolchainSpec, std::string>
parse_toolchain_spec(std::string compilerArg,
                     std::string versionArg = {},
                     bool requireCompiler = true);

// Print the spec's compat hint (once per process; no-op for canonical input).
void print_compat_hint(const ToolchainSpec& spec);

// The (family, target, host) → xim package mapping — the distribution layer.
XimToolchainPackage to_xim_package(const ToolchainSpec& spec);

// #367: does a GCC spec aimed at the NATIVE Linux host resolve to the
// `musl-gcc` payload rather than the glibc `gcc` one?
//
// `xim:gcc` publishes x86_64 assets only (`archs = { "x86_64" }`); the GCC the
// ecosystem ships for other Linux architectures is `musl-gcc`, which does
// publish them (aarch64 included, verified against xlings-res). Asking for
// `gcc` on aarch64 404s.
//
// A free function taking the host arch rather than reading the compile-time
// constant, so the aarch64 answer is testable on an x86_64 machine — the whole
// point being an architecture the developer is not sitting in front of.
bool gcc_native_payload_is_musl(std::string_view hostArch, bool isLinux,
                                const triple::Triple& target);

ToolchainSpec with_resolved_xim_version(const ToolchainSpec& spec,
                                        std::string_view ximVersion);

std::filesystem::path toolchain_frontend(const std::filesystem::path& binDir,
                                         const XimToolchainPackage& pkg);

// The frontend inside an installed payload ROOT — for callers that have the
// root rather than a bin directory.
//
// Most families keep it in `bin/`, and for them this is `toolchain_frontend`
// on `root/bin`. MSVC does not: cl.exe sits four levels deeper, under
// `VC/Tools/MSVC/<version>/bin/Host<h>/<arch>/`.
//
// It exists because that difference had to be known in three places and was
// only handled in two. The third — `toolchain list`'s enumeration — asked
// `root/bin`, got nothing, and `continue`d, so an msvc toolset installed
// perfectly well and then did not appear in the list. Empty = no frontend
// here, which is the caller's cue to skip; a wrong LAYOUT and a missing
// PAYLOAD had been reporting the same way.
std::filesystem::path payload_frontend(const std::filesystem::path& payloadRoot,
                                       const XimToolchainPackage& pkg,
                                       Family family);

// Reverse mapping: an installed `xim-x-<name>` payload directory back to its
// (family, target) identity. nullopt for non-toolchain xpkgs (ninja, glibc,
// python, …) — list/doctor use this to filter what they enumerate.
struct PayloadIdentity {
    Family          family;
    triple::Triple  target;       // empty = host-target payload (gcc, llvm)
};
std::optional<PayloadIdentity> identify_xim_payload(std::string_view ximDirName);

// Does an installed payload row match the configured default (toolchain axis;
// version exact)? `msvc@system` names no version and so matches on family
// alone; a pinned toolset compares versions like every other family.
bool spec_matches_payload(const ToolchainSpec& def,
                          const PayloadIdentity& id,
                          std::string_view payloadVersion);

// System toolchains are located on the machine, never installed/removed by
// mcpp: `msvc@system` and bare `msvc`. A VERSIONED msvc spec is NOT one of
// them — `msvc@14.44.35207` is an xim payload mcpp installs and pins, the
// same shape as `gcc@16.1.0`.
//
// (The PATH-compiler escape hatch, `[toolchain] … = "system"`, is a separate
// and older mechanism.)
bool is_system_toolchain(const ToolchainSpec& spec);

// The same question as `is_system_toolchain`, asked as the AXIS it belongs to
// (mcpp.toolchain.model). Both spellings exist because the predicate reads
// better at a site that is deciding one thing, and the enum reads better at a
// site that dispatches — but there is one derivation, so they cannot drift.
Origin origin_of(const ToolchainSpec& spec);

// Resolve a MANAGED msvc payload to its installation record.
//
// `prepare` (a build) and `lifecycle` (install / default / remove) both need
// this, and both used to spell it out: derive the version directory from
// (store, name, version) rather than trusting the fetcher's `root` guess —
// which descends into a lone subdirectory and therefore lands one level too
// deep for an msvc payload, whose only entry is `VC/` — then call
// `installation_at`, then format the same error. Three copies of one rule,
// and the comment explaining WHY the fetcher's guess is wrong existed in only
// one of them.
//
// `identifyVersion = false` skips running cl.exe for its banner; callers that
// only need to know whether a toolset is there should pass false.
std::expected<msvc::MsvcInstallation, std::string>
resolve_managed_msvc(const mcpp::xlings::Env& env,
                     const XimToolchainPackage& pkg,
                     bool identifyVersion = true);

// Does installing a toolchain FOR THIS TARGET additionally need the Linux
// sysroot payloads (`xim:glibc` + `xim:linux-headers`)?
//
// THE SINGLE DERIVATION. It was two, and they were not equivalent while a
// comment on one of them said "mirrors the guard on the other":
//
//   lifecycle   !musl && !pe && !windows-host && !macos-host
//   prepare     !macos-host && !windows-host && !musl
//
// The PE term was missing from the second. It happens to be unreachable today
// (first-run never selects a PE target on Linux), which is what let the
// divergence sit there — a latent difference between two spellings of one
// rule is exactly the state that becomes a bug the moment either side moves.
//
// Decided by the TARGET, not the payload name: musl targets are
// self-contained, PE targets (native MinGW and the Linux-hosted cross alike)
// bring their own CRT, and a non-Linux host never needs a Linux sysroot at
// all.
bool needs_linux_sysroot_payloads(const triple::Triple& target);

// Can THIS host serve that target — is there an installable payload for the
// (host, target) pair? Empty target = host target, always serviceable.
//
// THE single derivation of that question. It used to be worked out twice and
// independently: here, when picking the xim payload, and again in
// lifecycle.cppm when deciding whether `toolchain list` may show a target as
// `available`. Two derivations of one decision is how adding a target turns
// into a build failure instead of a missing row — the pair had already drifted
// once (the payload side would happily resolve a windows-hosted musl package
// that the availability side declared impossible).
bool host_can_serve(const triple::Triple& target);

// xim index names to query for the Available section, with the family each
// one contributes versions to. Host-conditional: a host only lists payloads
// it can install.
struct AvailableIndex {
    std::string ximName;
    Family      family;
};
std::vector<AvailableIndex> available_toolchain_indexes();

std::filesystem::path derive_c_compiler(const Toolchain& tc);

// A binutils-family tool for THIS toolchain's TARGET, named in the GNU
// spelling ("ar", "strip", "objcopy").
//
// WHY ONE FUNCTION. Four families spell the same tool four ways — llvm-<n>
// beside clang, `<triple>-<n>` for a cross, a separate binutils payload for a
// glibc gcc — and until this existed only `ar` knew that. A second tool added
// by copying `archive_tool` would be the same decision derived twice, and the
// copy is the one that silently stops agreeing.
//
// EMPTY IS AN ANSWER, NOT ALWAYS A FAILURE. On MSVC there is no binutils and
// no need for one: PE/MSVC keeps debug information in a separate `.pdb`, so
// there is nothing in-band for `strip` to remove. Callers must distinguish
// "this format has nothing to strip" from "the tool this format needs is
// missing" — see mcpp.pack.strip, which refuses only the second.
std::filesystem::path binutils_tool(const Toolchain& tc, std::string_view name);

std::filesystem::path archive_tool(const Toolchain& tc);
std::filesystem::path link_tool(const Toolchain& tc);
std::filesystem::path staged_std_bmi_path(const Toolchain& tc,
                                          const std::filesystem::path& outputDir);
std::filesystem::path staged_std_compat_bmi_path(const Toolchain& tc,
                                                 const std::filesystem::path& outputDir);

} // namespace mcpp::toolchain

namespace mcpp::toolchain {

namespace {

bool ends_with(std::string_view s, std::string_view suf) {
    return s.size() >= suf.size()
        && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

std::filesystem::path derive_c_compiler_path(const std::filesystem::path& cxxPath) {
    auto stem = cxxPath.stem().string();
    auto parent = cxxPath.parent_path();
    auto ext = cxxPath.extension();

    std::string cc_stem;
    if (stem.ends_with("++")) {
        cc_stem = stem.substr(0, stem.size() - 2);
        if (cc_stem == "g" || cc_stem.ends_with("-g"))
            cc_stem += "cc";
    } else {
        cc_stem = stem;
    }
    return parent / (cc_stem + ext.string());
}

triple::Triple host_musl_triple() {
    return { std::string(mcpp::platform::host_arch), "linux", "musl" };
}

} // namespace

std::expected<ToolchainSpec, std::string>
parse_toolchain_spec(std::string compilerArg,
                     std::string versionArg,
                     bool requireCompiler) {
    if (auto at = compilerArg.find('@'); at != std::string::npos) {
        if (versionArg.empty()) versionArg = compilerArg.substr(at + 1);
        compilerArg = compilerArg.substr(0, at);
    }
    if (compilerArg.empty() && requireCompiler) {
        return std::unexpected("missing compiler name");
    }

    auto norm = compat::normalize_spec(compilerArg, versionArg);
    if (!norm) {
        return std::unexpected(std::format(
            "unknown toolchain '{}' (expected gcc | llvm | msvc, or a "
            "supported alias like mingw / musl-gcc)", compilerArg));
    }

    ToolchainSpec spec;
    if      (norm->family == "llvm") spec.family = Family::Llvm;
    else if (norm->family == "msvc") spec.family = Family::Msvc;
    else                             spec.family = Family::Gcc;
    spec.version = std::move(norm->version);
    spec.target  = std::move(norm->target);

    // `@system` IS NOT A GENERAL SPELLING, and refusing it here is the point.
    //
    // mcpp is built on xlings, a user-space OS, and the whole design drives
    // host dependencies to a minimum: a toolchain comes from a payload the
    // manifest names, so every machine compiles with the same compiler.
    // `msvc@system` is a concession to ONE platform — Visual Studio is very
    // often already installed and cannot always be redistributed — not a
    // capability the other families are missing.
    //
    // It used to parse and then fail somewhere else entirely, as
    // `xim:gcc@system` → "no such package", which sends the reader looking
    // for a version that was never going to exist. A spec that cannot mean
    // anything should be rejected where it is read, by name, with the two
    // things it might have meant spelled out.
    if (spec.version == "system" && spec.family != Family::Msvc) {
        return std::unexpected(std::format(
            "'{}@system' is not a toolchain spelling: only msvc has a system "
            "origin, because Visual Studio is often already installed and "
            "cannot always be redistributed.\n"
            "  mcpp installs every other toolchain itself, so that each "
            "machine builds with the same one:\n"
            "    {}@<version>   pin a payload   (mcpp toolchain list --available {})\n"
            "    system         the PATH compiler, whatever it is — an escape "
            "hatch, and it takes no family",
            family_name(spec.family), family_name(spec.family),
            family_name(spec.family)));
    }
    if (norm->changed) spec.compatHint = std::move(norm->hint);
    return spec;
}

Origin origin_of(const ToolchainSpec& spec) {
    return is_system_toolchain(spec) ? Origin::SystemMsvc : Origin::Managed;
}

std::expected<msvc::MsvcInstallation, std::string>
resolve_managed_msvc(const mcpp::xlings::Env& env,
                     const XimToolchainPackage& pkg,
                     bool identifyVersion) {
    // NOT the fetcher's `root`: that field is its guess at where the useful
    // tree starts, and it descends into a lone subdirectory when the version
    // dir has no bin/ include/ lib/. An msvc payload's only entry is `VC/`,
    // so the guess lands exactly one level too deep. (store, name, version)
    // is known — compose it.
    auto verDir = mcpp::xlings::paths::xim_tool(env, pkg.ximName, pkg.ximVersion);
    // The package version IS the toolset directory name, so cl.exe is derived
    // rather than searched for: nothing here can silently pick a different
    // toolset, which is the whole reason the version axis exists.
    if (auto inst = msvc::installation_at(verDir, pkg.ximVersion, identifyVersion))
        return *inst;
    return std::unexpected(std::format(
        "msvc payload at '{}' has no cl.exe under VC/Tools/MSVC/{}",
        verDir.string(), pkg.ximVersion));
}

void print_compat_hint(const ToolchainSpec& spec) {
    if (spec.compatHint.empty()) return;
    compat::print_hint_once(spec.compatHint);
}

bool gcc_native_payload_is_musl(std::string_view hostArch, bool isLinux,
                                const triple::Triple& target) {
    if (!isLinux) return false;
    if (hostArch == "x86_64") return false;      // the glibc payload exists here
    // "Native" = no explicit target, or one naming this same machine. A CROSS
    // target keeps its own payload rule (the `<triple>-gcc` packages above).
    return target.empty()
        || (target.os == "linux" && target.arch == hostArch);
}

XimToolchainPackage to_xim_package(const ToolchainSpec& spec) {
    XimToolchainPackage pkg;
    pkg.displaySpec = spec.display();
    pkg.ximVersion  = spec.version;

    if (spec.family == Family::Msvc) {
        // `xim:msvc@<toolset>`. Only reached for a VERSIONED spec —
        // `msvc@system` never gets here, because nothing about it is a
        // package (see is_system_toolchain).
        //
        // frontendCandidates is what the generic bin/-shaped resolution
        // looks for, and an msvc payload is not bin/-shaped: cl.exe lives at
        // VC/Tools/MSVC/<ver>/bin/Hostx64/x64/. The managed path therefore
        // resolves through msvc::installation_at() instead — the same code
        // that describes a system install. Keeping the candidate here means
        // a caller that does use the generic path gets nothing rather than
        // the wrong thing.
        pkg.ximName = "msvc";
        pkg.ximVersion = spec.version;
        pkg.frontendCandidates = {"cl.exe"};
        return pkg;
    }
    if (spec.family == Family::Llvm) {
        // ⭐ ONE PAYLOAD. The `openkal-llvm` spelling normalises to this family and
        // installs nothing of its own — it is a statement about where the
        // TARGET SIDE comes from, and the compiler is the llvm payload either
        // way. A user who has one has both.
        pkg.ximName = mcpp::toolchain::llvm::package_name();
        pkg.frontendCandidates = mcpp::toolchain::llvm::frontend_candidates();
        return pkg;
    }

    // Family::Gcc — the target decides the payload.
    const auto& t = spec.target;

    // ⚠️⚠️ `&& t.os == "linux"` — AND THE PARAGRAPH BELOW ALREADY SAID SO.
    //
    // "Canonical linux-musl triples coincide with the GNU tool spelling" is a
    // statement about linux-musl, and the condition asked only whether the C
    // library is musl. `x86_64-windows-musl` was added later and walked in.
    // Measured 2026-08-26 on a Linux x86_64 host:
    //
    //     $ mcpp build --target x86_64-windows-musl   ([toolchain] gcc@16.1.0)
    //       fetcher: resolve: target='xim:musl-gcc@16.1.0'
    //       error: toolchain payload 'xim:musl-gcc@16.1.0' has no known C++
    //              frontend in …/xim-x-musl-gcc/16.1.0/bin
    //
    // `native` read `t.arch == host_arch` and got true, so a PE target resolved
    // the host's ELF Linux payload. The message names a missing frontend, which
    // is true of that payload and says nothing about the decision that reached
    // for it — the same shape as the rest of this release.
    if (t.is_musl() && t.os == "linux") {
        // Same target, two payload shapes: the host-native `musl-gcc` package
        // (XLINGS_RES picks the host-matching asset) when target arch == host
        // arch, else the triple-named cross package. Canonical linux-musl
        // triples coincide with the GNU tool spelling, so `<triple>-g++` is
        // the frontend either way.
        bool native = mcpp::platform::is_linux
                   && t.arch == mcpp::platform::host_arch;
        pkg.ximName = native ? "musl-gcc" : t.str() + "-gcc";
        // Frontend candidates are resolved with filesystem::exists, so on a
        // Windows host the bare name never matches — the file on disk is
        // `<triple>-g++.exe`. The mingw branch below has carried the `.exe`
        // spelling since it shipped; this one had not, which made a
        // windows-hosted musl payload install fine and then be unusable.
        // `.exe` first: on a case-insensitive filesystem both would match, and
        // the executable is the one we want.
        if constexpr (mcpp::platform::is_windows) {
            pkg.frontendCandidates = { t.str() + "-g++.exe", t.str() + "-g++",
                                       "g++.exe", "g++" };
        } else {
            pkg.frontendCandidates = { t.str() + "-g++", "g++" };
        }
        return pkg;
    }

    if (t.is_windows_gnu()
        || (t.empty() && mcpp::platform::is_windows)) {
        // GCC targeting Windows PE (GNU CRT) — ONE user-facing identity,
        // host-split at the distribution layer only:
        //   Windows host → native winlibs UCRT build (PE frontend g++.exe)
        //   other hosts  → Linux-hosted MSVCRT cross (ELF frontend, triple-
        //                  prefixed so a cross build never silently falls
        //                  back to a native g++)
        if constexpr (mcpp::platform::is_windows) {
            pkg.ximName = "mingw-gcc";
            pkg.frontendCandidates = {"g++.exe", "g++"};
        } else {
            pkg.ximName = "mingw-cross-gcc";
            pkg.frontendCandidates = {"x86_64-w64-mingw32-g++"};
        }
        return pkg;
    }

    // Host target (or linux-gnu): the glibc gcc package — on x86_64.
    //
    // #367: `xim:gcc` declares `archs = { "x86_64" }` and publishes assets for
    // that arch only. The GCC the ecosystem ships for other Linux
    // architectures is `musl-gcc`, which does publish them (aarch64 included).
    // Asking for `gcc` on aarch64 therefore 404s — and because a `build.mcpp`
    // host compile resolves the toolchain spec with NO target injection, it
    // landed here and made every project whose graph contains a build program
    // unbuildable on aarch64, with `--target aarch64-linux-musl` fixing only
    // the target half.
    //
    // "Which payload backs this spec on this machine" is exactly the question
    // this function exists to answer — the musl branch above already answers
    // its half the same way ("same target, two payload shapes"). The knowledge
    // belongs here rather than in every manifest: a user should not have to
    // encode which architectures a toolchain package was built for.
    if (gcc_native_payload_is_musl(mcpp::platform::host_arch,
                                   mcpp::platform::is_linux, t)) {
        const auto mt = host_musl_triple();
        pkg.ximName = "musl-gcc";
        pkg.frontendCandidates = { mt.str() + "-g++", "g++" };
        return pkg;   // no glibc specs fixup: that payload is not glibc-linked
    }

    pkg.ximName = "gcc";
    pkg.frontendCandidates = {"g++"};
    pkg.needsGccPostInstallFixup = true;
    return pkg;
}

ToolchainSpec with_resolved_xim_version(const ToolchainSpec& spec,
                                        std::string_view ximVersion) {
    ToolchainSpec out = spec;
    out.version = std::string(ximVersion);
    return out;
}

std::filesystem::path toolchain_frontend(const std::filesystem::path& binDir,
                                         const XimToolchainPackage& pkg) {
    for (auto& cand : pkg.frontendCandidates) {
        auto p = binDir / cand;
        if (std::filesystem::exists(p)) return p;
    }
    return {};
}

std::filesystem::path payload_frontend(const std::filesystem::path& payloadRoot,
                                       const XimToolchainPackage& pkg,
                                       Family family) {
    if (family == Family::Msvc) {
        // Same resolution the install and build paths use, so the three
        // cannot disagree about where an msvc payload keeps its compiler.
        // below, which is the llvm payload's shape.)
        if (auto inst = mcpp::toolchain::msvc::installation_at(payloadRoot,
                                                              pkg.ximVersion))
            return inst->clPath;
        return {};
    }
    return toolchain_frontend(payloadRoot / "bin", pkg);
}

std::optional<PayloadIdentity> identify_xim_payload(std::string_view ximDirName) {
    if (ximDirName == "gcc")
        return PayloadIdentity{ Family::Gcc, {} };
    // A pinned toolset is an installed payload like any other, so it shows up
    // in `toolchain list` under its toolset version. Without this row the
    // install would succeed and then be invisible.
    //
    // Host-target (empty triple), like gcc and llvm: an msvc payload only
    // ever targets the machine it runs on, and a spec for it carries no
    // target axis either — so the two sides compare equal.
    if (ximDirName == "msvc")
        return PayloadIdentity{ Family::Msvc, {} };
    if (ximDirName == mcpp::toolchain::llvm::package_name())
        return PayloadIdentity{ Family::Llvm, {} };
    if (ximDirName == "musl-gcc")
        return PayloadIdentity{ Family::Gcc, host_musl_triple() };
    if (ximDirName == "mingw-gcc" || ximDirName == "mingw-cross-gcc")
        return PayloadIdentity{ Family::Gcc, { "x86_64", "windows", "gnu" } };
    if (ends_with(ximDirName, "-gcc")) {
        auto prefix = ximDirName.substr(0, ximDirName.size() - 4);
        if (auto t = triple::parse(prefix))
            return PayloadIdentity{ Family::Gcc, *t };
    }
    return std::nullopt;   // not a toolchain payload (ninja, glibc, …)
}

bool spec_matches_payload(const ToolchainSpec& def,
                          const PayloadIdentity& id,
                          std::string_view payloadVersion) {
    if (def.family != id.family) return false;
    // msvc@system names no version, so it matches on family alone. A pinned
    // toolset compares versions like every other family — that is the point
    // of pinning it.
    if (is_system_toolchain(def)) return true;
    return def.version == payloadVersion;
}

bool is_system_toolchain(const ToolchainSpec& spec) {
    // The VERSION axis decides, not the family. `msvc@system` (and bare
    // `msvc`) means "whatever this machine has"; `msvc@14.44.35207` names a
    // toolset mcpp installs and pins, exactly as `gcc@16.1.0` names a gcc.
    //
    // Before this split, every msvc spec was a system spec — so a manifest
    // could ask for a specific toolset and silently get a different one,
    // which is the defect this whole file's msvc handling exists to close.
    return spec.family == Family::Msvc
        && (spec.version.empty() || spec.version == "system");
}

bool needs_linux_sysroot_payloads(const triple::Triple& target) {
    if constexpr (!mcpp::platform::is_linux) return false;
    return !target.is_musl() && !target.is_pe();
}

bool host_can_serve(const triple::Triple& target) {
    if (target.empty()) return true;              // host target

    if (target.os == "linux") {
        if constexpr (mcpp::platform::is_linux) {
            // ⚠️⚠️ "SELF-CONTAINED" IS ABOUT THE PAYLOAD'S CONTENTS, NOT ABOUT
            // WHICH HOSTS IT IS PUBLISHED FOR — and this line read it as both.
            //
            // A musl payload really does carry its own sysroot, so no host-side
            // libc is needed. It still has to EXIST for the host running it,
            // and the cross packages are published per host arch:
            //
            //   x86_64-linux-musl-gcc    archs = { "x86_64" }
            //   aarch64-linux-musl-gcc   archs = { "x86_64", "aarch64" }
            //
            // Measured on ubuntu-24.04-arm: `--target x86_64-linux-musl` was
            // admitted, resolved a package with no aarch64 asset, and failed at
            // install — `mismatch / build-failed` in the target matrix, twice.
            //
            // ⭐ The native row stays reachable on every arch: `musl-gcc`
            // publishes both, which is why `aarch64-linux-musl` is `ok` there.
            const bool crossArch = target.arch != mcpp::platform::host_arch;
            if (target.is_musl())
                return !crossArch || mcpp::platform::host_arch == "x86_64";
            return !crossArch;
        }
        // Non-Linux host: only the self-contained musl payloads can work at
        // all (nothing else would find a C library). Today exactly one such
        // payload exists — the windows-hosted canadian cross, built per host
        // arch — so an arch-crossing combination stays unserviceable until one
        // is published. macOS has no Linux-targeting payload at all.
        return mcpp::platform::is_windows
            && target.is_musl()
            && target.arch == mcpp::platform::host_arch;
    }
    // ⚠️ THE mingw CROSS IS PUBLISHED FOR ONE HOST ARCH. `mingw-cross-gcc`
    // declares `archs = { "x86_64" }`, so a Linux host that is not x86_64
    // cannot obtain it — measured on ubuntu-24.04-arm, where the row was
    // listed and its refusal carried no reason at all (`unsupported / other`).
    if (target.is_windows_gnu()) {
        if constexpr (mcpp::platform::is_windows) return true;
        return mcpp::platform::is_linux
            && mcpp::platform::host_arch == "x86_64";
    }
    // ⚠️⚠️ PE + musl HAS NO PAYLOAD ON ANY HOST, INCLUDING WINDOWS.
    //
    // `triple::pin_is_capability()` already says so — no gcc emits a PE with a
    // musl C library, and LLVM cannot spell the triple — and chapter 16 states
    // it in those words: "A payload for it does not exist on any host; its
    // system can only come from a dependency graph."
    //
    // This line disagreed, on exactly one host. Measured on windows-2022,
    // payload system:
    //
    //     c-abi      musl      (payload)
    //     c++-abi    msvc-stl  (payload)
    //     lld-link: error: undefined symbol: __main
    //     lld-link: error: undefined symbol: __mingw_vfprintf
    //
    // — musl's C library, MSVC's STL and MinGW's CRT symbols in one link. On
    // Linux the same cell already answered `host-cannot-serve`, which is the
    // right answer everywhere.
    //
    // ⭐ The graph path is untouched: this refusal is held and released only
    // when nothing supplies the target's system, and `graph × windows-musl` is
    // `ok` on both hosts.
    if (target.is_pe() && target.is_musl()) return false;
    if (target.os == "windows") return bool(mcpp::platform::is_windows);
    if (target.os == "macos")   return bool(mcpp::platform::is_macos);

    // Bare metal: every host can serve it, and that is a property of the
    // toolchain rather than a claim about payload coverage. clang and lld are
    // cross-compilers by construction — one binary emits every target it was
    // built with — so a freestanding target needs NO per-host cross payload,
    // unlike every hosted case above, which needs a C library that only exists
    // for some (host, target) pairs.
    //
    // ⚠️ Serviceable is not the same as complete: a target with no C library
    // still links only `-nostdlib` programs. That gap belongs to the ecosystem
    // (a libc wrapper package), and saying `false` here would hide it behind
    // "this host cannot build it", which is the wrong diagnosis.
    if (target.is_freestanding()) return true;

    return false;
}

std::vector<AvailableIndex> available_toolchain_indexes() {
    // ⚠️⚠️ NOT EVERY FAMILY EXISTS FOR EVERY (OS, ARCH), AND THIS LIST USED TO
    // SAY OTHERWISE.
    //
    // The branches below are per-OS and there were none per-ARCH, so an aarch64
    // Linux host was told llvm could be installed. Measured 2026-08-26 against
    // the index and upstream:
    //
    //   xlings-res/llvm 20.1.7 / 22.1.8   no linux-aarch64 asset
    //   llvm/llvm-project 20.1.7, 21.1.0  no linux-aarch64 asset
    //   llvm/llvm-project 19.1.7          has one — too old for `import std`
    //
    // so `mcpp toolchain install llvm 22.1.8` there is a 404 that this list
    // promised would work. Same family as the rest of this release: a table
    // that answers a narrower question than the one it is asked.
    //
    // ⭐ THIS IS A POLICY STATEMENT, NOT A COPY OF THE INDEX. It says which
    // families mcpp SUPPORTS on this host — the same kind of statement `tier`
    // makes for a target row — and the plan that retires it is
    // `.agents/docs/2026-08-26-aarch64-linux-ecosystem-closure.md` §P1.
    //
    // ⚠️ AND ITS PREMISE IS ASSERTED IN CI, so it cannot outlive its reason.
    // `ci-target-matrix.yml`'s aarch64 job checks that no linux-aarch64 llvm
    // asset has appeared; the day one does, that step reds and names this
    // gate. A deferral nobody rechecks is indistinguishable from a defect.
    const bool linuxNonX86 =
        mcpp::platform::is_linux && mcpp::platform::host_arch != "x86_64";

    std::vector<AvailableIndex> out{
        { "gcc",      Family::Gcc },
        { "musl-gcc", Family::Gcc },
    };
    if (!linuxNonX86)
        out.push_back({ mcpp::toolchain::llvm::package_name(), Family::Llvm });
    // The Windows-PE gcc payload is host-split at the distribution layer
    // (§4.3); each host lists the package it would actually install.
    if constexpr (mcpp::platform::is_windows) {
        // Pinned MSVC toolsets. Listing them is what makes the managed origin
        // discoverable at all: without a row here `toolchain list --available`
        // says gcc and llvm can be pinned and msvc cannot, which stopped being
        // true. `msvc@system` is reported separately, as an installation.
        out.push_back({ "msvc", Family::Msvc });
        out.push_back({ "mingw-gcc", Family::Gcc });
        // The windows-hosted canadian cross to Linux. Named by triple, exactly
        // as to_xim_package() derives it (`<triple>-gcc`), so the Available
        // listing and the install path cannot disagree about the package name.
        out.push_back({ std::string(mcpp::platform::host_arch) + "-linux-musl-gcc",
                        Family::Gcc });
    } else if constexpr (mcpp::platform::is_linux) {
        // Same gate: `mingw-cross-gcc` publishes x86_64 only.
        if (!linuxNonX86) out.push_back({ "mingw-cross-gcc", Family::Gcc });
    }
    return out;
}

std::filesystem::path derive_c_compiler(const Toolchain& tc) {
    return derive_c_compiler_path(tc.binaryPath);
}

std::filesystem::path binutils_tool(const Toolchain& tc, std::string_view name) {
    // MSVC: no binutils, and none wanted — see the declaration.
    if (tc.compiler == CompilerId::MSVC) return {};

    std::error_code ec;
    auto dir = tc.binaryPath.parent_path();

    // Clang ships the whole family as `llvm-<name>` beside the frontend.
    if (is_clang(tc)) {
        auto llvmTool = dir / (std::string("llvm-") + std::string(name)
                               + std::string(mcpp::platform::exe_suffix));
        if (std::filesystem::exists(llvmTool, ec)) return llvmTool;
        return {};
    }

    // MinGW bundles its own binutils next to the frontend (self-contained,
    // like musl) — never an external binutils xpkg. Native (Windows-host) ships
    // `ar.exe`; the Linux-hosted cross ships the triple-prefixed ELF tool
    // `x86_64-w64-mingw32-ar`. Try the cross form first, then native.
    if (is_mingw_target(tc)) {
        if (!tc.targetTriple.empty()) {
            auto cross = dir / (tc.targetTriple + "-" + std::string(name));
            if (std::filesystem::exists(cross, ec)) return cross;
        }
        auto native = dir / (std::string(name) + ".exe");
        if (std::filesystem::exists(native, ec)) return native;
        return {};
    }

    if (!is_musl_target(tc)) {
        if (auto binutilsBin = mcpp::toolchain::gcc::find_binutils_bin(tc.binaryPath))
            return *binutilsBin / std::string(name);
    }

    // A musl tool is the triple-prefixed cross form (e.g. aarch64-linux-musl-ar),
    // sitting next to the frontend. Derive from the resolved target triple so
    // cross targets pick the matching tool instead of the x86_64 one.
    std::string crossName = (!tc.targetTriple.empty()
        ? tc.targetTriple : std::string("x86_64-linux-musl")) + "-" + std::string(name);
    // Same `.exe` reasoning as the frontend candidates above: a windows-hosted
    // musl cross payload ships `<triple>-ar.exe`. Try it first, then the bare
    // name (which is what every ELF host has).
    if constexpr (mcpp::platform::is_windows) {
        auto muslExe = dir / (crossName + ".exe");
        if (std::filesystem::exists(muslExe, ec)) return muslExe;
    }
    auto musl = dir / crossName;
    if (std::filesystem::exists(musl, ec)) return musl;
    return {};
}

std::filesystem::path archive_tool(const Toolchain& tc) {
    // The one spelling that is NOT a binutils name: MSVC archives with
    // LIB.EXE, which takes a different verb for every operation.
    if (tc.compiler == CompilerId::MSVC) {
        auto lib = tc.binaryPath.parent_path() / "lib.exe";
        std::error_code ec;
        if (std::filesystem::exists(lib, ec)) return lib;
        return {};
    }
    return binutils_tool(tc, "ar");
}

std::filesystem::path staged_std_bmi_path(const Toolchain& tc,
                                          const std::filesystem::path& outputDir) {
    if (tc.compiler == CompilerId::MSVC)
        return mcpp::toolchain::msvc::staged_std_bmi_path(outputDir);
    if (is_clang(tc)) return mcpp::toolchain::clang::staged_std_bmi_path(outputDir);
    return mcpp::toolchain::gcc::staged_std_bmi_path(outputDir);
}

std::filesystem::path staged_std_compat_bmi_path(const Toolchain& tc,
                                                 const std::filesystem::path& outputDir) {
    if (tc.compiler == CompilerId::MSVC)
        return mcpp::toolchain::msvc::staged_std_compat_bmi_path(outputDir);
    return mcpp::toolchain::clang::staged_std_compat_bmi_path(outputDir);
}

// Separate linker binary for SeparateLinker dialects (link.exe beside cl).
// Empty for driver-link toolchains.
std::filesystem::path link_tool(const Toolchain& tc) {
    if (tc.compiler != CompilerId::MSVC) return {};
    auto link = tc.binaryPath.parent_path() / "link.exe";
    std::error_code ec;
    if (std::filesystem::exists(link, ec)) return link;
    return {};
}

} // namespace mcpp::toolchain
