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
import mcpp.toolchain.triple;

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

    // ⭐⭐ THE TRIPLE, SAID OUT LOUD, WHEN NOTHING ELSE SAYS IT.
    //
    // Every hosted cross this build tool could do was served by a payload whose
    // driver had exactly one target — `x86_64-w64-mingw32-g++` needs no
    // `--target` because it has no choice. So nothing emitted one outside the
    // freestanding path, and the assumption "the driver knows" was true.
    //
    // It stops being true the moment the TARGET SIDE comes from the dependency
    // graph instead of from a payload. Then the compiler is an ordinary clang,
    // which emits every format it was built with, and which will emit for THIS
    // machine unless told otherwise.
    //
    // ⚠️ Measured 2026-08-23. A build for `aarch64-macos` with an explicit
    // `[target.aarch64-macos] toolchain = "llvm@…"` resolved the whole graph,
    // took the C library's aarch64 headers, and compiled with no `--target` —
    // host code generation, target declarations. It was caught by an assertion
    // the C library port wrote for precisely this situation:
    //
    //     the C library and the compiler disagree about LDBL_DIG ('33 == 18')
    //
    // 33 is aarch64's binary128 and 18 is x87: two machines in one command.
    //
    // The decision itself is not made here — see Toolchain::crossTargetFlag,
    // which is set where both the request and the compiler are known. This
    // reads it.
    if (!tc.crossTargetFlag.empty()) out.push_back(tc.crossTargetFlag);

    // ⭐⭐ AND WHAT A `throw` AND A `thread_local` COMPILE INTO, WHICH IS A
    // PROPERTY OF THE GRAPH AND NOT OF ANY ONE PACKAGE — see
    // `graph_runtime_compile_flags` for what and why.
    //
    // ⚠️ IT WAS DECLARED PER-PACKAGE, WHICH IS EXACTLY AS FAR AS IT REACHED.
    // `openkal-llvm-runtime` set `-fdwarf-exceptions` in its own `[build]`, so
    // its objects agreed with each other and nothing else did. Measured
    // 2026-08-23 — every object compiled, and the link said:
    //
    //     ld.lld: error: undefined symbol: __gxx_personality_seh0
    //
    // referenced from the CONSUMER's `main.o`, which had a `try` block and no
    // reason to know any of this. A user cannot be asked to write a flag whose
    // necessity is a fact about their dependencies.
    for (auto& f : graph_runtime_compile_flags(tc)) out.push_back(f);

    const bool bypassCfg =
        dm.hasCfg && (opt.cfgBypass == HostFlagOptions::CfgBypass::Always
                      || mcpp::platform::is_linux);

    // Trusting the cfg means contributing no include paths, stdlib selection
    // or runtime choices — it already carries them. It does NOT mean
    // contributing nothing: the deployment target still has to be stated (see
    // below), which is why this suppresses the two blocks rather than
    // returning early.
    const bool trustCfg = !bypassCfg && dm.hasCfg;

    // ⚠️ AND NOT WHEN THE TARGET SIDE COMES FROM THE GRAPH — the compile-side
    // counterpart of the replacement `flags.cppm` makes on the link line.
    //
    // These tokens are the payload's: `-isystem <payload>/include/c++/v1` and
    // the C library beside it. For an openkal target the C++ runtime and the C
    // library are packages, and the payload's copies are built for the machine
    // doing the building.
    //
    // ⚠️ `-nostdinc++` DOES NOT REMOVE THEM, which is what makes this its own
    // fix rather than a flag. That option suppresses the DRIVER's own C++
    // search; a path put there explicitly with `-isystem` stays. Measured
    // 2026-08-23, cross-compiling openkal-windows — a package that uses no C++
    // standard library at all — with `-nostdinc++` on the command line:
    //
    //     winnt.h:16 → …/xim-x-llvm/…/include/c++/v1/ctype.h
    //                → __config:13 '__config_site' file not found
    //
    // mingw's own header asked for `<ctype.h>`, and the payload's libc++ was
    // still ahead of the sysroot that had just been pointed at the right place.
    const bool graphSuppliesTarget = !tc.crossTargetFlag.empty();

    if (bypassCfg && !graphSuppliesTarget) {
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

    if (!trustCfg && !graphSuppliesTarget
        && (bypassCfg || lm.mode != CLibMode::None))
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
        // Trusting the cfg — with ONE exception, and it is not a preference.
        //
        // The cfg picks runtimes; it does not pick a linker, so on macOS the
        // default is Xcode's /usr/bin/ld. That binary is itself a C++ Mach-O
        // linked against libc++, and it runs inside the same DYLD_* the
        // payload toolchain sets up, so dyld resolves ITS libc++ to the
        // payload's. When that copy lacks a symbol Apple's ld needs, ld
        // aborts before it links anything:
        //
        //   dyld: Symbol not found: __ZdaPv        (operator delete[])
        //     Referenced from: .../XcodeDefault.xctoolchain/usr/bin/ld
        //     Expected in:     .../xim-x-llvm/22.1.8/lib/libc++.1.0.dylib
        //
        // The MAIN build already refuses to use Xcode's ld for exactly this,
        // and says so at flags.cppm's macOS branch: "Xcode 15.4's ld aborting
        // at launch on macos-14 CI when its libc++ resolution was diverted".
        // The host helper links in the same environment, so it cannot be
        // allowed to differ — a toolchain that builds the project but not its
        // build.mcpp is not a working toolchain (mcpp#437).
        //
        // lld ships with the very toolchain doing the compile, so it cannot
        // be diverted to a libc++ it was not built against.
        if constexpr (mcpp::platform::is_macos) out.push_back("-fuse-ld=lld");
        return out;
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
