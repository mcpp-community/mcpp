// mcpp.toolchain.registry — the two-axis toolchain identity model and its
// payload mapping.
//
// Identity (design §4.1–§4.3): a toolchain is `family@version` (family ∈
// gcc | llvm | msvc | emsdk | android-ndk), a target is a canonical Triple
// (triple.cppm). The two
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
import mcpp.libs.json;
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

// A FAMILY IS A COMPILER, AND `openkal-llvm` WAS NOT ONE.
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
    // WHICH PAYLOAD ANSWERED, NOT ONLY WHICH FAMILY.
    //
    // `emsdk@6.0.9` normalises to the llvm family, because `em++` IS clang and
    // a fourth family value would be a false claim about the compiler. The
    // consequence was a display line reading `Resolved llvm@6.0.9`, which is
    // indistinguishable from the real `xim:llvm` and is not what the user
    // typed. `mcpp toolchain list` has the same problem, and the matrix scan
    // takes one toolchain per family, so two llvm-family payloads on one host
    // could not both be enumerated.
    //
    // The family and the payload are two questions:
    //
    //     family   what flag vocabulary does this compiler speak?   llvm
    //     payload  which archive provides it?                       emsdk
    //
    // `to_xim_package` already answers the second from the target; this is the
    // field that lets it be SAID. Empty means the family's own payload, which
    // is every row but these two, so nothing else's output moves.
    std::string payloadName;

    std::string spec_str() const {
        return std::format("{}@{}",
                           payloadName.empty() ? family_name(family)
                                               : std::string_view(payloadName),
                           version);
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
    // WHERE THE FRONTEND LIVES, RELATIVE TO THE PAYLOAD ROOT.
    //
    // `bin` for every payload that grew up here, and that was hardcoded at the
    // one place which composed a root with a bin directory -- with MSVC as a
    // named exception four levels deeper. A third and fourth shape make the
    // exception list the wrong structure: emsdk keeps `em++` in
    // `emscripten/`, and the NDK keeps `clang++` in
    // `toolchains/llvm/prebuilt/<host>/bin/`. Neither is unusual; what was
    // unusual was asking the FAMILY where a PAYLOAD keeps its compiler.
    //
    // MSVC stays a branch rather than a subdirectory because its path carries
    // the toolset version and the host/target arch pair, which is a lookup and
    // not a constant.
    std::string                     frontendSubdir = "bin";
    // THE FAMILY, CARRIED RATHER THAN PASSED ALONGSIDE. `payload_frontend`
    // took it as a second argument, which every caller had to source from a
    // differently-named local; five of them composed `payload->binDir`
    // themselves instead and so could not see `frontendSubdir` at all. The
    // package is built from a spec that has a family, so carrying it is free
    // and removes an argument that could disagree with `pkg`.
    Family                          family = Family::Gcc;
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
// A MALFORMED DESCRIPTOR IS AN ERROR AND A MISSING FRONTEND IS NOT, which is
// why this returns `expected` over a path that may still be empty. Empty =
// nothing found where we looked, the caller's long-standing cue to skip or to
// refuse in its own words. `unexpected` = the payload described itself and the
// description does not parse, which no caller can express as "not found".
std::expected<std::filesystem::path, std::string>
payload_frontend(const std::filesystem::path& payloadRoot,
                 const XimToolchainPackage& pkg);

// ─── A payload describes itself ──────────────────────────────────────────────
//
// Three payload-specific facts used to live in this engine: the NDK's internal
// `toolchains/llvm/prebuilt/<host>/bin` layout, its API floor in
// `meta/platforms.json`, and the `-D__BIONIC_CTYPE_INLINE=` its libc++ module
// surface needs. Each is a fact the INSTALLING RECIPE already computes for its
// own probes, and re-deriving all three here is why adding a second such SDK
// meant editing the engine instead of publishing a package.
//
// So the recipe writes one file beside the payload and the engine reads it:
//
//     <payload root>/.mcpp-toolchain.json
//     {
//       "schema": 1,
//       "frontend": "toolchains/llvm/prebuilt/linux-x86_64/bin/clang++",
//       "platform_floor": "21",
//       "std_module_defines": ["__BIONIC_CTYPE_INLINE="]
//     }
//
// It is NOT a general flag channel. Three keys, each answering a question this
// engine already asks; a payload that could inject arbitrary flags would be a
// package changing a build it does not own, and `[build]` in a manifest is the
// project's to write.
struct PayloadDescriptor {
    int                      schema = 0;
    // Relative to the payload root, already host-resolved by the recipe. The
    // engine stops computing a host tag; `frontendSubdir` stays as the answer
    // for a payload that ships no descriptor.
    std::string              frontend;
    // The string `llvm_triple(param)` already takes -- the payload's answer
    // rather than a constant compiled in here.
    std::string              platformFloor;
    // These reach the std module's own command assembly, which is a separate
    // channel from the compile flags, and they enter the build fingerprint
    // because they change what the module compiles to.
    std::vector<std::string> stdModuleDefines;
};

// The descriptor's file name, so a message and a test name the same string.
inline constexpr std::string_view payload_descriptor_filename =
    ".mcpp-toolchain.json";

// Read `<payloadRoot>/.mcpp-toolchain.json`.
//
// THREE OUTCOMES, AND THE MIDDLE ONE IS THE REASON THIS RETURNS `expected`.
//
//   nullopt         no descriptor -- today's behaviour exactly, so a released
//                   payload keeps working and there is no flag day.
//   a descriptor    the payload answered.
//   unexpected      PRESENT AND MALFORMED. Refused, naming the file. Silently
//                   falling back would make a typo read as "an older payload"
//                   and the engine would use a hardcoded path for a layout
//                   that has moved -- which is the failure mode this
//                   repository records most often: the lookup is repaired and
//                   the message is not.
std::expected<std::optional<PayloadDescriptor>, std::string>
read_payload_descriptor(const std::filesystem::path& payloadRoot);

// The descriptor for the payload a COMPILER belongs to.
//
// Two of the three answers are needed where only the compiler path is in
// hand, and the payload root is some number of directories above it -- five
// for the NDK, two for emsdk and for llvm. Found by walking up, not by
// counting components, because the count is exactly the kind of fact that
// changes silently when a layout does (`ndk_min_api_level` walks for the same
// reason). The walk is BOUNDED so that a compiler outside any payload cannot
// reach a descriptor belonging to a directory that is not its payload.
std::expected<std::optional<PayloadDescriptor>, std::string>
payload_descriptor_for_compiler(const std::filesystem::path& compilerPath);

// The DIRECTORY `payload_frontend` searched, for a message that has to name it.
//
// The five "has no known C++ frontend in <dir>" refusals printed
// `payload->binDir`, which was the directory they had composed themselves. Once
// the package decides where its frontend lives, a message naming `bin` would
// be naming a directory nothing looked in -- and that is the failure this
// codebase records most often: the lookup is fixed and the message is not.
std::filesystem::path payload_frontend_dir(const std::filesystem::path& payloadRoot,
                                           const XimToolchainPackage& pkg);

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

// The NDK's own declared minimum API level, read from the installed payload's
// `meta/platforms.json`. 0 when it cannot be read. Definition and the reason
// the number is not a constant are below.
int ndk_min_api_level(const std::filesystem::path& compilerPath);

// xim index names to query for the Available section, with the family each
// one contributes versions to. Host-conditional: a host only lists payloads
// it can install.
struct AvailableIndex {
    std::string ximName;
    Family      family;
};
std::vector<AvailableIndex> available_toolchain_indexes();

std::filesystem::path derive_c_compiler(const Toolchain& tc);

// The same derivation as a pure function of the path, so that the mapping can
// be stated once and asserted without a toolchain. Exported because it is the
// whole of what can be wrong in it: every frontend this engine resolves has a
// C driver beside it, and the rule for naming it differs by driver.
std::filesystem::path derive_c_compiler_path(const std::filesystem::path& cxxPath);

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


triple::Triple host_musl_triple() {
    return { std::string(mcpp::platform::host_arch), "linux", "musl" };
}

} // namespace

// THE C COMPILER BESIDE A C++ ONE, AND DROPPING `++` IS NOT THE RULE.
//
// It is the rule for clang and it is not for the others, which is why `g` was
// already a special case here: `g++`'s C compiler is `gcc` and not `g`. A
// second driver with the same shape arrived and the special case did not
// cover it.
//
// Measured 2026-09-11, compiling the conformance suite's one C translation
// unit for `wasm32-emscripten`:
//
//   /bin/sh: 1: .../xim-x-emsdk/6.0.9/emscripten/em: not found
//
// `em++` became `em`, which is not a program. The C compiler is `emcc`.
//
// A TABLE RATHER THAN A THIRD `if`, because the property being encoded is "this
// driver names its C compiler with a different word", and a table can be read
// as the list of drivers for which that is true. The fallthrough -- drop the
// `++` -- stays correct for clang, for `<triple>-clang++`, and for every
// frontend candidate this engine resolves that is not in the table.
std::filesystem::path derive_c_compiler_path(const std::filesystem::path& cxxPath) {
    auto stem = cxxPath.stem().string();
    auto parent = cxxPath.parent_path();
    auto ext = cxxPath.extension();

    // Each row is a C++ driver stem and the C driver beside it. Prefixed forms
    // (`x86_64-w64-mingw32-g++`) match on the suffix, which is what keeps one
    // row per driver rather than one per target triple.
    struct Row { std::string_view cxx, c; };
    static constexpr Row kNamed[] = {
        { "g++",  "gcc"  },   // GCC, native and triple-prefixed
        { "em++", "emcc" },   // Emscripten
    };

    std::string cc_stem;
    bool named = false;
    for (auto const& row : kNamed) {
        if (stem == row.cxx) {
            cc_stem = std::string(row.c);
            named = true;
            break;
        }
        // `<prefix>-g++` -> `<prefix>-gcc`. The separator is required, so
        // `clang++` does not match the `g++` row by ending in it.
        const std::string suffix = "-" + std::string(row.cxx);
        if (stem.size() > suffix.size() && ends_with(stem, suffix)) {
            cc_stem = stem.substr(0, stem.size() - row.cxx.size())
                    + std::string(row.c);
            named = true;
            break;
        }
    }
    if (!named) {
        cc_stem = stem.ends_with("++")
            ? stem.substr(0, stem.size() - 2)
            : stem;
    }
    return parent / (cc_stem + ext.string());
}

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
            "unknown toolchain '{}' (expected gcc | llvm | msvc | emsdk | "
            "android-ndk, or a supported alias like mingw / musl-gcc)",
            compilerArg));
    }

    ToolchainSpec spec;
    if      (norm->family == "llvm") spec.family = Family::Llvm;
    else if (norm->family == "msvc") spec.family = Family::Msvc;
    else                             spec.family = Family::Gcc;
    spec.version     = std::move(norm->version);
    spec.target      = std::move(norm->target);
    spec.payloadName = std::move(norm->payload);

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

// The NDK's own name for the HOST it runs on, which is the directory component
// under `toolchains/llvm/prebuilt/`. Upstream ships `linux-x86_64`,
// `darwin-x86_64` (a universal binary, so Apple silicon reads it too) and
// `windows-x86_64`. Not the target -- a Linux x86_64 machine building for
// aarch64 still reads `linux-x86_64`.
std::string ndk_host_tag() {
    if constexpr (mcpp::platform::is_windows) return "windows-x86_64";
    else if constexpr (mcpp::platform::is_macos) return "darwin-x86_64";
    else return "linux-x86_64";
}

// THE NDK'S OWN MINIMUM API LEVEL, READ FROM THE PAYLOAD.
//
// Android's API level is NOT OPTIONAL and mcpp cannot leave it out. bionic's
// own <sys/cdefs.h> stops the build:
//
//     sys/cdefs.h:365:2: error: Unversioned target triples are not supported!
//
// So a project that declares no `min_api_level` still needs a level, and the
// question is where the number comes from. Not from a constant compiled in
// here: this repository has recorded more than once that a version written
// into a comment becomes a version written into a diagnostic and then into
// somebody's install command, and the NDK's floor moves with the NDK. The
// payload answers for itself -- `meta/platforms.json` is upstream's own
// declaration of the range it supports, `{"min": 21, "max": 37}` for r30 --
// and reading it means a newer NDK changes the default by being installed
// rather than by being edited into this file.
//
// Returns 0 when the file is absent or unreadable, which the caller turns into
// a refusal naming `min_api_level`. A guessed level would be worse than the
// refusal: it selects which bionic symbols exist, so guessing produces an
// artefact that links here and fails to load on a device.
int ndk_min_api_level(const std::filesystem::path& compilerPath) {
    // `<ndk>/toolchains/llvm/prebuilt/<host>/bin/clang++` -- walk up rather
    // than counting components, because the count is exactly the kind of fact
    // that changes silently when a layout does.
    std::error_code ec;
    for (auto dir = compilerPath.parent_path();
         !dir.empty() && dir != dir.parent_path();
         dir = dir.parent_path()) {
        auto meta = dir / "meta" / "platforms.json";
        if (!std::filesystem::exists(meta, ec)) continue;
        std::ifstream in(meta);
        if (!in) return 0;
        try {
            auto j = nlohmann::json::parse(in, nullptr, false);
            if (j.is_discarded() || !j.contains("min")) return 0;
            auto min = j["min"];
            if (!min.is_number_integer()) return 0;
            auto level = min.get<int>();
            return level > 0 ? level : 0;
        } catch (...) {
            return 0;
        }
    }
    return 0;
}

XimToolchainPackage to_xim_package(const ToolchainSpec& spec) {
    XimToolchainPackage pkg;
    pkg.displaySpec = spec.display();
    pkg.ximVersion  = spec.version;
    pkg.family      = spec.family;

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
        // THE TARGET DECIDES THE PAYLOAD HERE TOO, and it did not used to.
        //
        // This returned the generic llvm payload unconditionally, which is
        // right for every target llvm itself serves and wrong for the two that
        // arrive with their own clang. `em++` and the NDK's `clang++` ARE
        // clang -- same family, same flag vocabulary, same `import std` path
        // -- and each is a clang whose target is fixed by its payload, which
        // is the shape `src/toolchain/hostflags.cppm` already describes:
        // "every hosted cross this build tool could do was served by a payload
        // whose driver had exactly one target". So the family stays `Llvm` and
        // no fourth value is invented; what changes is which package answers.
        const auto& lt = spec.target;

        if (lt.os == "emscripten") {
            // `em++` is a `#!/bin/sh` wrapper beside the Python it execs, in
            // `emscripten/` rather than `bin/` -- `bin/` holds the raw clang,
            // which would compile for wasm and then link like an ordinary
            // clang, producing a `.wasm` with no JavaScript and none of
            // Emscripten's own glue. Naming the wrapper is the whole point.
            pkg.ximName = "emsdk";
            pkg.frontendSubdir = "emscripten";
            pkg.frontendCandidates = { "em++", "emcc" };
            return pkg;
        }

        if (lt.is_android()) {
            // One NDK payload serves every Android arch and API level: the
            // arch arrives as `--target=<arch>-linux-android<api>` on the
            // command line, not as a different package. The host tuple in the
            // path is the HOST's, not the target's -- a Linux x86_64 machine
            // cross-compiling for aarch64 still reads
            // `prebuilt/linux-x86_64/`.
            pkg.ximName = "android-ndk";
            pkg.frontendSubdir = std::format("toolchains/llvm/prebuilt/{}/bin",
                                             ndk_host_tag());
            if constexpr (mcpp::platform::is_windows) {
                pkg.frontendCandidates = { "clang++.exe", "clang++" };
            } else {
                pkg.frontendCandidates = { "clang++", "clang" };
            }
            return pkg;
        }

        // ONE PAYLOAD for everything else. The `openkal-llvm` spelling
        // normalises to this family and installs nothing of its own — it is a
        // statement about where the TARGET SIDE comes from, and the compiler is
        // the llvm payload either way. A user who has one has both.
        pkg.ximName = mcpp::toolchain::llvm::package_name();
        pkg.frontendCandidates = mcpp::toolchain::llvm::frontend_candidates();
        return pkg;
    }

    // Family::Gcc — the target decides the payload.
    const auto& t = spec.target;

    // `&& t.os == "linux"` — AND THE PARAGRAPH BELOW ALREADY SAID SO.
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

std::expected<std::optional<PayloadDescriptor>, std::string>
read_payload_descriptor(const std::filesystem::path& payloadRoot) {
    std::error_code ec;
    auto file = payloadRoot / payload_descriptor_filename;
    if (!std::filesystem::exists(file, ec)) return std::nullopt;

    // EVERY REFUSAL NAMES THE FILE. A message that says only "invalid
    // descriptor" sends the reader to the documentation; one that names the
    // path is one edit away from a fix, and the recipe that wrote it is the
    // thing that has to change.
    auto refuse = [&](std::string_view what) {
        return std::unexpected(std::format("{}: {}", file.string(), what));
    };

    std::ifstream in(file);
    if (!in) return refuse("cannot be read");
    auto j = nlohmann::json::parse(in, nullptr, false);
    if (j.is_discarded()) return refuse("is not valid JSON");
    if (!j.is_object())   return refuse("is not a JSON object");

    PayloadDescriptor d;
    if (!j.contains("schema") || !j["schema"].is_number_integer())
        return refuse("has no integer \"schema\"");
    d.schema = j["schema"].get<int>();
    // A SCHEMA THIS ENGINE DOES NOT IMPLEMENT IS REFUSED, NOT IGNORED. The
    // descriptor exists because the built-in guess is wrong for this payload;
    // a newer schema means the payload is describing something this engine
    // cannot read, and falling back to the guess is the one outcome the
    // descriptor was added to prevent. The obligation is therefore on the
    // recipe: a schema bump comes with a floor on the mcpp version that reads
    // it, the same way an index descriptor's syntax does.
    if (d.schema != 1)
        return refuse(std::format(
            "declares schema {}, which this mcpp does not implement "
            "(expected 1); upgrade mcpp to build with this payload",
            d.schema));

    if (j.contains("frontend")) {
        if (!j["frontend"].is_string())
            return refuse("\"frontend\" is not a string");
        d.frontend = j["frontend"].get<std::string>();
        if (d.frontend.empty()) return refuse("\"frontend\" is empty");
        // IT CANNOT LEAVE THE PAYLOAD. A descriptor naming `/usr/bin/g++` or
        // `../../..` would make a package choose a host compiler, which is
        // the one thing a payload must not be able to do: mcpp's hermeticity
        // is a property of the payload boundary, not of the recipe's good
        // manners.
        //
        // VALIDATED AS A STRING AND NOT THROUGH `std::filesystem::path`,
        // BECAUSE THAT TYPE'S ANSWERS DIFFER BY HOST AND THE DESCRIPTOR DOES
        // NOT.
        //
        // Measured 2026-09-11 on a Windows runner: `path("/usr/bin/g++")
        // .is_absolute()` is FALSE there -- the path has a root directory and
        // no root NAME, which Windows calls root-relative -- so the check
        // passed, and `payloadRoot / "/usr/bin/g++"` then resolves to
        // `C:/usr/bin/g++`. A host compiler, chosen by a package, on the one
        // host where the guard did not look.
        //
        // The descriptor's `frontend` is one shape on every host: relative,
        // `/`-separated, plain components. Asserting that positively is
        // host-independent by construction; asking a path type whether it is
        // absolute is asking a question whose meaning the host supplies.
        if (d.frontend.find('\\') != std::string::npos)
            return refuse("\"frontend\" contains a backslash; the separator "
                          "is `/` on every host");
        if (d.frontend.front() == '/')
            return refuse("\"frontend\" is absolute; it is relative to the "
                          "payload root");
        if (d.frontend.find(':') != std::string::npos)
            return refuse("\"frontend\" names a drive or a scheme; it is a "
                          "path relative to the payload root");
        for (std::size_t i = 0, n = 0; i <= d.frontend.size(); ++i) {
            if (i != d.frontend.size() && d.frontend[i] != '/') { ++n; continue; }
            const auto part = d.frontend.substr(i - n, n);
            n = 0;
            if (part.empty())
                return refuse("\"frontend\" has an empty path component");
            if (part == "." || part == "..")
                return refuse("\"frontend\" leaves the payload root");
        }
    }

    if (j.contains("platform_floor")) {
        // ONE SPELLING. `llvm_triple` takes the string, the manifest's
        // `min_api_level` is a string, and a number here would be a second
        // spelling for the same value -- which this engine has recorded as
        // the shape that makes two mechanisms out of one question. The
        // recipe's own tests assert this file's content, so the refusal is
        // caught where the file is written rather than where it is read.
        if (!j["platform_floor"].is_string())
            return refuse("\"platform_floor\" is not a string (write \"21\", "
                          "not 21)");
        d.platformFloor = j["platform_floor"].get<std::string>();
        if (d.platformFloor.empty())
            return refuse("\"platform_floor\" is empty");
        if (d.platformFloor.find_first_not_of("0123456789.") != std::string::npos)
            return refuse("\"platform_floor\" is not a version");
    }

    if (j.contains("std_module_defines")) {
        if (!j["std_module_defines"].is_array())
            return refuse("\"std_module_defines\" is not an array");
        for (auto const& e : j["std_module_defines"]) {
            if (!e.is_string())
                return refuse("\"std_module_defines\" holds a non-string");
            auto def = e.get<std::string>();
            // THIS IS THE KEY THAT COULD BECOME A FLAG CHANNEL, so it is the
            // key with a shape. A define starts with an identifier character;
            // anything that could begin an option is refused, and whitespace
            // is refused because a value needing quotes crosses two parsers
            // and this engine has paid for that twice.
            if (def.empty())
                return refuse("\"std_module_defines\" holds an empty entry");
            const char c0 = def.front();
            if (!(std::isalpha(static_cast<unsigned char>(c0)) || c0 == '_'))
                return refuse(std::format(
                    "\"std_module_defines\" entry '{}' is not a define name; "
                    "this is not a flag channel", def));
            if (def.find_first_of(" \t\n\r\"'") != std::string::npos)
                return refuse(std::format(
                    "\"std_module_defines\" entry '{}' contains whitespace "
                    "or a quote", def));
            d.stdModuleDefines.push_back(std::move(def));
        }
    }
    return d;
}

std::expected<std::optional<PayloadDescriptor>, std::string>
payload_descriptor_for_compiler(const std::filesystem::path& compilerPath) {
    // Eight levels covers every payload layout this engine resolves --
    // `toolchains/llvm/prebuilt/<host>/bin/clang++` is the deepest at five --
    // with room for one that is deeper, and stops well short of a machine's
    // root.
    constexpr int kMaxLevels = 8;
    std::error_code ec;
    auto dir = compilerPath.parent_path();
    for (int i = 0; i < kMaxLevels && !dir.empty() && dir != dir.parent_path();
         ++i, dir = dir.parent_path()) {
        if (std::filesystem::exists(dir / payload_descriptor_filename, ec))
            return read_payload_descriptor(dir);
    }
    return std::nullopt;
}

std::filesystem::path payload_frontend_dir(const std::filesystem::path& payloadRoot,
                                           const XimToolchainPackage& pkg) {
    if (pkg.family == Family::Msvc) {
        // The lookup's own answer, so the message names the toolset directory
        // rather than a path this code would have guessed.
        if (auto inst = mcpp::toolchain::msvc::installation_at(payloadRoot,
                                                              pkg.ximVersion))
            return inst->clPath.parent_path();
        return payloadRoot / "VC" / "Tools" / "MSVC" / pkg.ximVersion;
    }
    // The payload's own answer when it has one -- otherwise this message
    // would name the directory the guess would have searched while the
    // lookup searched another.
    //
    // A malformed descriptor is not refused here: this function exists to
    // NAME A DIRECTORY IN A MESSAGE, and `payload_frontend` has already
    // refused by the time any caller composes one.
    if (auto d = read_payload_descriptor(payloadRoot); d && *d && !(*d)->frontend.empty())
        return (payloadRoot / (*d)->frontend).parent_path();
    return payloadRoot / pkg.frontendSubdir;
}

std::expected<std::filesystem::path, std::string>
payload_frontend(const std::filesystem::path& payloadRoot,
                 const XimToolchainPackage& pkg) {
    // THE PAYLOAD IS ASKED FIRST, AND ITS ANSWER IS NOT SECOND-GUESSED. If a
    // descriptor names a frontend that is not there, the result is empty --
    // "nothing found where we looked" -- and the message names the directory
    // the descriptor pointed at, because that is where the search happened.
    auto desc = read_payload_descriptor(payloadRoot);
    if (!desc) return std::unexpected(desc.error());
    if (*desc && !(*desc)->frontend.empty()) {
        auto named = payloadRoot / (*desc)->frontend;
        if (std::filesystem::exists(named)) return named;
        return std::filesystem::path{};
    }
    if (pkg.family == Family::Msvc) {
        // Same resolution the install and build paths use, so the three
        // cannot disagree about where an msvc payload keeps its compiler.
        // below, which is the llvm payload's shape.)
        if (auto inst = mcpp::toolchain::msvc::installation_at(payloadRoot,
                                                              pkg.ximVersion))
            return inst->clPath;
        return std::filesystem::path{};
    }
    return toolchain_frontend(payloadRoot / pkg.frontendSubdir, pkg);
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

    // AN SDK THAT SHIPS ITS OWN SYSROOT IS SERVED WHERE THE SDK IS PUBLISHED,
    // AND THE ARCH IN THE TRIPLE IS THE GUEST'S.
    //
    // Every branch below reasons about a cross payload per host arch, because
    // that is how a compiler targeting another Linux or another Windows is
    // published here. An Emscripten or Android SDK is published per HOST and
    // serves every guest arch from one archive: one `xim:emsdk` compiles for
    // wasm32 regardless of the machine's arch, and one NDK serves both Android
    // arches from a single `--target=<arch>-linux-android`.
    //
    // Without this the wasm row's own pin was not enough. Measured: with
    // `emsdk@6.0.9` in the table, `mcpp build --target wasm32-emscripten`
    // still answered "No toolchain payload here produces it" and listed
    // seventeen servable targets -- because `target.os` is neither "linux" nor
    // a Windows form, so control reached a `return false` written for triples
    // nobody publishes a payload for.
    //
    // "WHEREVER" WAS TOO BROAD ONCE, AND THE TARGET MATRIX IS WHAT CAUGHT IT.
    // The first version returned true unconditionally, which is the same
    // mistake as the branches it sits above: a predicate correct about the
    // objects its author had in mind. It was then narrowed to
    // `mcpp::platform::is_linux`, because `xim:emsdk` and `xim:android-ndk`
    // both declared ONLY an `xpm.linux` table -- so on macOS or Windows there
    // was no payload to install and the honest answer was the same
    // `host-cannot-serve` every other unpublished combination gets. That
    // comment named its own expiry: "when a darwin or windows NDK lands in the
    // index -- upstream publishes both -- this is the one line that changes."
    //
    // IT HAS LANDED, SO THIS IS THAT LINE. Both packages now declare
    // `xpm.linux`, `xpm.macosx` and `xpm.windows`, and the index's own
    // per-host install jobs are the measurement rather than the declaration:
    // on macOS and Windows each payload downloads, extracts, passes its
    // recipe's compiler probe and registers its shims. Two host assumptions
    // inside those recipes were found by exactly those jobs and fixed there,
    // which is where a host-shaped packaging defect belongs -- not here.
    //
    // Keyed on the target and no longer on the host, because these payloads
    // are published per host OS and carry every guest arch: one `xim:emsdk`
    // compiles for wasm32 regardless of the machine's arch, and one NDK serves
    // both Android arches. The remaining per-host question is whether the
    // payload EXISTS, and that is the index's answer to give, not a constant
    // compiled into the engine. A row whose pin the index cannot satisfy on
    // this host fails at install with the package's own diagnostic, which
    // names the payload -- strictly better than this function silently
    // deleting the row from `toolchain list`, which reported a target mcpp
    // knows as one it has never heard of.
    if (target.has_own_sysroot()) return true;

    if (target.os == "linux") {
        if constexpr (mcpp::platform::is_linux) {
            // "SELF-CONTAINED" IS ABOUT THE PAYLOAD'S CONTENTS, NOT ABOUT
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
            // The native row stays reachable on every arch: `musl-gcc`
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
    // THE mingw CROSS IS PUBLISHED FOR ONE HOST ARCH. `mingw-cross-gcc`
    // declares `archs = { "x86_64" }`, so a Linux host that is not x86_64
    // cannot obtain it — measured on ubuntu-24.04-arm, where the row was
    // listed and its refusal carried no reason at all (`unsupported / other`).
    if (target.is_windows_gnu()) {
        if constexpr (mcpp::platform::is_windows) return true;
        return mcpp::platform::is_linux
            && mcpp::platform::host_arch == "x86_64";
    }
    // PE + musl HAS NO PAYLOAD ON ANY HOST, INCLUDING WINDOWS.
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
    // The graph path is untouched: this refusal is held and released only
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
    // Serviceable is not the same as complete: a target with no C library
    // still links only `-nostdlib` programs. That gap belongs to the ecosystem
    // (a libc wrapper package), and saying `false` here would hide it behind
    // "this host cannot build it", which is the wrong diagnosis.
    if (target.is_freestanding()) return true;

    return false;
}

std::vector<AvailableIndex> available_toolchain_indexes() {
    // NOT EVERY FAMILY EXISTS FOR EVERY (OS, ARCH), AND THIS LIST USED TO
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
    // THIS IS A POLICY STATEMENT, NOT A COPY OF THE INDEX. It says which
    // families mcpp SUPPORTS on this host — the same kind of statement `tier`
    // makes for a target row — and the plan that retires it is
    // `.agents/docs/2026-08-26-aarch64-linux-ecosystem-closure.md` §P1.
    //
    // AND ITS PREMISE IS ASSERTED IN CI, so it cannot outlive its reason.
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
