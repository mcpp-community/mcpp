// mcpp.toolchain.hostflags — the single producer of host-compile flags.
//
// One assembly, three consumers:
//   flags.cppm            → rendered with ninja `$` escaping into build.ninja
//   stdmod.cppm           → rendered with shell quoting into a std-module command
//   build_program.cppm    → used as argv tokens directly (build.mcpp execs, no shell)
//
// Before this module those three hand-wrote the same thing. The resolvers
// (linkmodel, clang driver model) were already shared; the ASSEMBLY was not,
// because the seam only produced strings and the argv consumer could not use
// it. Every bug in mcpp#331/PR#332's batch was an instance of that split —
// flags.cppm knew about quoting / the macOS deployment target / the MSVC
// dialect and the other two did not — and a 0.0.9x fix had already corrected
// the same file once for the same reason (musl→static re-derived).
//
// See .agents/docs/2026-08-02-host-compile-single-producer-design.md.
//
// Deliberately its own module rather than a helper inside build_program.cppm:
// that file's anonymous namespace has demonstrated (PR#332, clang 22.1.8 +
// C++20 modules + -O2) that adding a function to it can miscompile a
// NEIGHBOURING function — an unused `split_ws` was enough to corrupt a local
// vector in `contract_env`. Mechanism unknown, reproduction solid; the cheap
// response is to not grow that namespace. Design §6.2.

export module mcpp.toolchain.hostflags;

import std;
import mcpp.platform;
import mcpp.toolchain.model;
import mcpp.toolchain.linkmodel;
import mcpp.toolchain.registry;

export namespace mcpp::toolchain {

// The knobs below exist because the three consumers genuinely differ TODAY.
// Each one is a documented divergence, not a switch to preserve an accident:
// consolidating without them would silently change behaviour, and dropping
// the reasons would leave the next reader unable to tell which is which.
struct HostFlagOptions {
    // Whether to bypass clang's bundled `<driver>.cfg`.
    //
    //   Always    — the main build and the std module. The cfg is an
    //               install-time-generated, non-reproducible artifact, so
    //               everything it would provide is spelled out explicitly.
    //   LinuxOnly — the build.mcpp host helper. On macOS/Windows it keeps
    //               TRUSTING the cfg, because the macOS link additionally
    //               needs the libc++abi/unwind handling that the main build's
    //               needs_explicit_libcxx path owns; duplicating that for a
    //               host compile produced undefined __cxa_* /
    //               __gxx_personality_v0 (build_program.cppm, pre-existing).
    enum class CfgBypass { Always, LinuxOnly };
    CfgBypass cfgBypass = CfgBypass::Always;

    // binutils `-B` so the driver finds as/ld. A GCC/libstdc++ payload
    // concern only: musl and MinGW-w64 bundle their own, and Clang/MSVC never
    // take an external binutils. MinGW must NOT get the Linux binutils — its
    // PE/SEH output is only assemblable by x86_64-w64-mingw32-as.
    bool binutilsPrefix = false;

    // `-L` (plus `-Wl,-rpath` where the format has one) for the toolchain's
    // own runtime dirs, so the produced program can load private libs in
    // tree. The main build routes these through depRuntimeLibraryDirs
    // instead, so it leaves this off.
    bool runtimeLibDirs = false;

    // Emit `-stdlib=libc++` alongside the cfg bypass.
    //
    // ClangDriverModel deliberately leaves this to callers: flags.cppm's
    // string feeds C compiles too, and a C command must not carry it. The std
    // module build has no such constraint — it compiles exactly one C++ TU —
    // and states the stdlib selection explicitly.
    bool clangStdlibSelect = false;

    // Resolved value from platform::macos::deployment_target(); empty = omit.
    // Must agree across the std BMI and everything that imports it — clang
    // rejects a module built for a different deployment target outright.
    std::string macosDeploymentTarget;
};

// Host-compile flags as argv tokens, in the order the string channels have
// always emitted them (clang cfg → deployment target → C library), so
// rendering reproduces today's command lines byte for byte.
std::vector<std::string> host_compile_tokens(const Toolchain& tc,
                                             const HostFlagOptions& opt,
                                             const PathEscape& esc);

// Link-side tokens for a driver invocation that compiles AND links a host
// program in one step — which is what build.mcpp is. The main build keeps its
// own link assembly (it links target artifacts under a different linkage
// policy); this exists so the one-shot host case has a producer at all
// instead of hand-writing one.
std::vector<std::string> host_link_tokens(const Toolchain& tc,
                                          const HostFlagOptions& opt,
                                          const PathEscape& esc);

// A "use this BMI" flag as argv tokens.
//
// BmiTraits stores these for the ninja string channel, where the shape does
// not matter: `-fmodule-file=std=<p>` is one word but `/reference std=<p>` is
// two, and a string consumer never has to know. An argv consumer does — one
// element containing a space is a single argument with a space in it, which
// cl.exe rejects. Split at the prefix's last space, the same rule the ninja
// side's quoting uses.
std::vector<std::string> bmi_reference_tokens(std::string_view usePrefix,
                                              const std::filesystem::path& bmi);

} // namespace mcpp::toolchain

namespace mcpp::toolchain {

std::vector<std::string> host_compile_tokens(const Toolchain& tc,
                                             const HostFlagOptions& opt,
                                             const PathEscape& esc) {
    std::vector<std::string> out;

    // MSVC carries none of this on the command line: cl.exe and link.exe find
    // headers and import libraries through INCLUDE / LIB, which detection
    // synthesizes into tc.envOverrides. Emitting the GNU shapes below would
    // produce a string of unknown options and then LNK1181.
    if (tc.compiler == CompilerId::MSVC) return out;

    const auto dm = resolve_clang_driver(tc);
    const auto lm = resolve_link_model(tc);

    const bool bypassCfg =
        dm.hasCfg && (opt.cfgBypass == HostFlagOptions::CfgBypass::Always
                      || mcpp::platform::is_linux);

    // Trusting the cfg means contributing no include paths, stdlib selection
    // or runtime choices — it already carries them. It does NOT mean
    // contributing nothing: the deployment target still has to be stated (see
    // below), which is why this suppresses the two blocks rather than
    // returning early.
    const bool trustCfg = !bypassCfg && dm.hasCfg;

    if (bypassCfg) {
        for (auto& t : dm.compile_tokens(esc, opt.clangStdlibSelect))
            out.push_back(t);
    }

    // Unconditional on macOS, cfg or no cfg. clang refuses to load a module
    // built for a different deployment target, and this result feeds every
    // compile that touches one — the bundled mcpp module's precompile, its
    // object step, and the build.mcpp compile. Skipping it on the trust-cfg
    // path is exactly the mismatch e2e 181 catches: the std BMI is built for
    // 14.0 while the TU importing it is not.
    if (mcpp::platform::is_macos && !opt.macosDeploymentTarget.empty())
        out.push_back("-mmacosx-version-min=" + opt.macosDeploymentTarget);

    if (!trustCfg && (bypassCfg || lm.mode != CLibMode::None))
        for (auto& t : lm.compile_tokens(esc)) out.push_back(t);

    return out;
}

std::vector<std::string> bmi_reference_tokens(std::string_view usePrefix,
                                              const std::filesystem::path& bmi) {
    std::string_view p = usePrefix;
    while (!p.empty() && p.front() == ' ') p.remove_prefix(1);
    if (p.empty()) return {};
    auto sp = p.find_last_of(' ');
    if (sp == std::string_view::npos)
        return { std::string(p) + bmi.string() };
    return { std::string(p.substr(0, sp)),
             std::string(p.substr(sp + 1)) + bmi.string() };
}

std::vector<std::string> host_link_tokens(const Toolchain& tc,
                                          const HostFlagOptions& opt,
                                          const PathEscape& esc) {
    std::vector<std::string> out;
    if (tc.compiler == CompilerId::MSVC) return out;

    const auto dm = resolve_clang_driver(tc);
    const auto lm = resolve_link_model(tc);

    const bool bypassCfg =
        dm.hasCfg && (opt.cfgBypass == HostFlagOptions::CfgBypass::Always
                      || mcpp::platform::is_linux);

    if (bypassCfg) {
        for (auto& t : dm.link_tokens(esc)) out.push_back(t);
    } else if (dm.hasCfg) {
        return out;   // trusting the cfg: it already selects the linker/runtimes
    }

    for (auto& t : lm.link_tokens(esc)) out.push_back(t);

    if (opt.binutilsPrefix) {
        if (auto ar = archive_tool(tc); !ar.empty())
            out.push_back("-B" + esc(ar.parent_path()));
    }

    if (opt.runtimeLibDirs) {
        // -L is link-time and wanted everywhere; rpath is an ELF-only concept.
        // A PE target reaches here too, where the flag is inert and
        // self-containment comes from the static link instead (#299).
        for (auto& d : tc.linkRuntimeDirs) {
            out.push_back("-L" + esc(d));
            if constexpr (mcpp::platform::supports_rpath)
                out.push_back("-Wl,-rpath," + esc(d));
        }
    }

    return out;
}

} // namespace mcpp::toolchain
