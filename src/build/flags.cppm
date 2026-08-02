// mcpp.build.flags — shared compile/link flag computation.
//
// Extracts all flag logic from ninja_backend.cppm into a single point
// of truth so both the ninja backend and compile_commands.json emitter
// (and future backends) share identical flag sets.
//
// See .agents/docs/2026-05-12-compile-commands-design.md.

module;
#include <cstdlib>

export module mcpp.build.flags;

import std;
import mcpp.build.distribution;
import mcpp.build.plan;
import mcpp.modgraph.scanner;
import mcpp.platform;
import mcpp.toolchain.clang;
import mcpp.toolchain.detect;
import mcpp.toolchain.dialect;
import mcpp.toolchain.hostflags;
import mcpp.toolchain.linkmodel;
import mcpp.toolchain.model;
import mcpp.toolchain.provider;
import mcpp.toolchain.registry;

export namespace mcpp::build {

struct CompileFlags {
    std::string cxx;                  // full cxxflags string
    std::string cc;                   // full cflags string
    std::string as;                   // asm-safe subset for .S/.s via the C driver
    std::string nasm;                 // NASM global flags (.asm; own spelling)
    std::string ld;                   // ldflags string
    std::filesystem::path cxxBinary;  // g++ / clang++ / cl.exe
    std::filesystem::path ccBinary;   // gcc / clang (derived; cl.exe = same)
    std::filesystem::path arBinary;   // ar / llvm-ar / lib.exe (empty → PATH)
    std::filesystem::path ldBinary;   // link.exe (SeparateLinker dialects only)
    std::string sysroot;              // --sysroot=... (for ninja ldflags)
    std::string bFlag;                // -B<binutils> (for ninja ldflags)
    bool staticStdlib = true;
    std::string linkage;  // "static" or ""
    // Per-link-unit C++ runtime flags, indexed by dist::Role. EVERY platform
    // routes through here now (`-static-libstdc++`, MinGW's `-static`, macOS's
    // `-load_hidden` archives): the channel has to be per-unit because two
    // roles in one build may hold different contracts, which is precisely what
    // `static_stdlib = false` could not express for test binaries before #336.
    // Produced by exactly one call to `dist::resolve` per role.
    std::array<std::string, 3> ldStdlibByRole{};
    // The contract each role actually got (after any degradation).
    std::array<mcpp::build::dist::Contract, 3> contractByRole{};
    // macOS + self-contained: link units need the initializer-ordering shim
    // object prepended to their inputs (issue #336).
    bool needsStreamInitShim = false;
    // Non-empty when a requested contract could not be honored. The caller
    // MUST surface these — a silent downgrade is the failure mode this whole
    // model exists to prevent. Emitted once by the backend, not here, because
    // compute_flags runs twice per build (ninja + compile_commands).
    std::vector<std::string> diagnostics;

    const std::string& ldStdlibFor(mcpp::build::dist::Role r) const {
        return ldStdlibByRole[static_cast<std::size_t>(r)];
    }
};

CompileFlags compute_flags(const BuildPlan& plan);

// The kind → role map. One line of policy, in one place: a test binary runs on
// the build machine and is then thrown away; an archive embeds no runtime at
// all; everything else leaves this machine. Backends ask this, never the kind.
constexpr mcpp::build::dist::Role role_of(LinkUnit::Kind k) {
    switch (k) {
        case LinkUnit::TestBinary:    return mcpp::build::dist::Role::Test;
        case LinkUnit::StaticLibrary: return mcpp::build::dist::Role::Intermediate;
        case LinkUnit::Binary:
        case LinkUnit::SharedLibrary: break;
    }
    return mcpp::build::dist::Role::Distributable;
}

// Return the linker flag that pulls in libatomic, or "" when it should be
// omitted. libatomic carries the out-of-line __atomic_* libcalls that
// 16-byte / oversized std::atomic lowers to (a GCC runtime lib — LLVM ships
// no equivalent, and compiler drivers don't auto-link it), so a genuine
// atomic user otherwise fails at link with `undefined __atomic_*`. We guard
// it with --as-needed so binaries that don't use it get no dependency. But
// --as-needed does NOT skip a missing library (the linker still has to open
// it), so the flag is emitted ONLY when a link-resolvable libatomic actually
// exists on one of the toolchain's link dirs — otherwise it would break
// toolchains that ship no libatomic at all. `staticLink` (a `-static` build,
// e.g. musl targets) narrows the resolvable form to `libatomic.a`; a dynamic
// link also accepts `libatomic.so`.
std::string atomic_link_flag(const std::vector<std::filesystem::path>& linkDirs,
                             bool staticLink);

// mcpp#234: quote a single flag-vector token for safe embedding in a shell
// command line. Every element of a flags `vector<string>` is already one
// argv token (e.g. `apply_glob_flags` pushes `"-D" + d`, so a define like
// `T=long long` arrives as the single element `-DT=long long`) — but the
// emission choke points (`join_flags` in ninja_backend.cppm, and the global
// blob assembly below) historically joined tokens with a bare space and no
// quoting, so a token containing a space silently split into two shell
// words once ninja handed the resolved command line to the shell. Only
// tokens that actually contain whitespace or a shell-significant character
// are quoted — plain framework flags (`-std=c++23`, `-O2`, `-I/abs/path`)
// come back unchanged, byte-for-byte. POSIX: wrap in single quotes (embedded
// `'` escaped as `'\''`). Windows: wrap in double quotes (embedded `"`
// escaped as `\"`) — cmd.exe/CreateProcess argv convention.
std::string shell_quote_arg(std::string_view arg);

// One include-directory token, fully prepared for a ninja command line:
// dialect prefix, ninja `$` escaping, and shell quoting — in that order.
//
// #331: the same manifest `[build] include_dirs` reaches the compiler through
// two channels — the global blob assembled below, and the per-translation-unit
// `$local_includes` emitted by ninja_backend. Only the first one quoted, so an
// include dir containing a space (`C:\Program Files\...`, or `/home/my dir` on
// Linux) survived one path and split into separate shell words on the other.
// Both channels call this now; adding a third one and forgetting to quote is
// how the bug happened, and a shared helper is the only fix that also covers
// the fourth.
//
// `prefixOverride` replaces `d.includePrefix` for the callers that need a
// different flag for the same kind of path (`-idirafter` for #249's
// after-dirs, plain `-I` for NASM units which would parse `-idirafter<p>` as
// `-i dirafter<p>`).
//
// `form` picks the separator, and the two channels genuinely need different
// ones (#261): tokens that stay on the command line keep native separators,
// while tokens ninja copies into a RESPONSE FILE must be forward-slashed,
// because the drivers tokenize response files GNU-style — there a backslash
// is an ESCAPE character and `C:\src\inc` loses its separators. Quoting
// alone does not save it; the escape happens inside quotes too.
enum class PathForm {
    Native,   // command line — a backslash is just a character
    Generic,  // response file — forward slashes, see above
};

std::string include_token(const mcpp::toolchain::CommandDialect& d,
                          const std::filesystem::path& dir,
                          std::string_view prefixOverride = {},
                          PathForm form = PathForm::Native);

}  // namespace mcpp::build

namespace mcpp::build {

namespace {

std::filesystem::path staged_std_bmi_path(const BuildPlan& plan) {
    return mcpp::toolchain::staged_std_bmi_path(plan.toolchain, plan.outputDir);
}

// Escape a string for embedding in ninja rule strings. Takes the text, not a
// path: round-tripping through std::filesystem::path would re-normalize the
// separators on Windows, which silently undoes a caller that deliberately
// chose generic_string() for a response-file token (#261).
std::string escape_ninja_chars(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == ' ' || c == '$' || c == ':')
            out.push_back('$');
        out.push_back(c);
    }
    return out;
}

// Escape a path for embedding in ninja rule strings (native separators).
std::string escape_path(const std::filesystem::path& p) {
    return escape_ninja_chars(p.string());
}

std::string normalize_ldflag(const std::filesystem::path& root, const std::string& flag) {
    auto absolute_path = [&](std::string_view raw) {
        std::filesystem::path p{std::string(raw)};
        if (p.is_absolute() || raw.starts_with("$")) return p;
        return root / p;
    };

    if (flag.starts_with("-L") && flag.size() > 2) {
        return "-L" + escape_path(absolute_path(std::string_view(flag).substr(2)));
    }

    constexpr std::string_view rpathPrefix = "-Wl,-rpath,";
    if (flag.starts_with(rpathPrefix) && flag.size() > rpathPrefix.size()) {
        return std::string(rpathPrefix)
             + escape_path(absolute_path(std::string_view(flag).substr(rpathPrefix.size())));
    }

    return flag;
}

}  // namespace

std::string atomic_link_flag(const std::vector<std::filesystem::path>& linkDirs,
                             bool staticLink) {
    for (auto& dir : linkDirs) {
        std::error_code ec;
        if (std::filesystem::exists(dir / "libatomic.a", ec)
            || (!staticLink && std::filesystem::exists(dir / "libatomic.so", ec))) {
            return " -Wl,--push-state,--as-needed -latomic -Wl,--pop-state";
        }
    }
    return {};
}

std::string include_token(const mcpp::toolchain::CommandDialect& d,
                          const std::filesystem::path& dir,
                          std::string_view prefixOverride,
                          PathForm form) {
    std::string_view prefix =
        prefixOverride.empty() ? d.includePrefix : prefixOverride;
    std::string path = form == PathForm::Generic ? dir.generic_string()
                                                 : dir.string();
    // Prefix first, then escape+quote the whole token: the prefix and the
    // path are ONE argv word, so quoting them separately would put the
    // opening quote in the wrong place and re-split exactly what we came to
    // join. `escape_path` only adds ninja's `$` escapes and never touches
    // separators, so the form chosen above survives it.
    return shell_quote_arg(escape_ninja_chars(std::string(prefix) + path));
}

std::string shell_quote_arg(std::string_view arg) {
    // Characters that split/alter a word when unquoted in POSIX sh or
    // cmd.exe: whitespace plus the common shell metacharacters. Anything
    // NOT in this set (e.g. `-std=c++23`, `-O2`, `-I/abs/path`, `-DFOO=1`)
    // returns untouched — no quoting where none is needed.
    constexpr std::string_view kNeedsQuote = " \t\n\"'\\$`;&|<>()*?[]#~!{}";
    if (arg.find_first_of(kNeedsQuote) == std::string_view::npos)
        return std::string(arg);

    if constexpr (mcpp::platform::is_windows) {
        // cmd.exe / CreateProcess argv convention: wrap in double quotes,
        // escape embedded `"` as `\"`.
        std::string out = "\"";
        for (char c : arg) {
            if (c == '"') out += "\\\"";
            else out.push_back(c);
        }
        out += "\"";
        return out;
    } else {
        // POSIX sh: wrap in single quotes (nothing is special inside single
        // quotes except `'` itself), escaping an embedded `'` as `'\''`
        // (close quote, literal quote, reopen quote).
        std::string out = "'";
        for (char c : arg) {
            if (c == '\'') out += "'\\''";
            else out.push_back(c);
        }
        out += "'";
        return out;
    }
}

CompileFlags compute_flags(const BuildPlan& plan) {
    CompileFlags f;

    // Central query points for per-toolchain decisions — prefer these over
    // ad-hoc is_clang()/is_gcc() calls:
    //   caps   — what the toolchain can do (scan-deps, stdlib id, …)
    //   d      — how a flag is SPELT (GNU "-I" vs MSVC "/I")
    //   traits — BMI mechanics + module-flag spellings
    auto caps = mcpp::toolchain::capabilities_for(plan.toolchain);
    const auto& d = mcpp::toolchain::dialect_for(plan.toolchain);

    // macOS minimum supported OS version for produced binaries.
    // Precedence: MACOSX_DEPLOYMENT_TARGET env (explicit per-invocation
    // override, the convention cargo/rustc/cc honor) > the manifest's
    // [build] macos_deployment_target (project default, SwiftPM-style) >
    // empty (toolchain/SDK default).
    std::string macosDeploymentTarget = mcpp::platform::macos::deployment_target(
        plan.manifest.buildConfig.macosDeploymentTarget);

    f.cxxBinary = plan.toolchain.binaryPath;
    f.ccBinary = mcpp::toolchain::derive_c_compiler(plan.toolchain);

    const bool isMsvcDialect = (d.id == "msvc");

    // PIC? (GNU-only concept; PE code is position independent by design.)
    bool need_pic = false;
    for (auto& lu : plan.linkUnits) {
        if (lu.kind == LinkUnit::SharedLibrary) {
            need_pic = true;
            break;
        }
    }
    std::string pic_flag = (need_pic && !isMsvcDialect) ? " -fPIC" : "";

    // Include dirs — this is the TYPED PATH channel (bare paths from the
    // manifest; the dialect prefix is applied here at emission), not the
    // FLAG-STRING channel that `normalize_include_flags` serves (cflags/
    // cxxflags, where the -I/-iquote/... prefix is already embedded in the
    // string by the scanner). `normalize_include_flags`'s prefix table only
    // knows GNU spellings, so routing dialect-prefixed tokens through it
    // silently no-ops under MSVC (`/Iinclude` matches nothing and is never
    // rewritten against plan.projectRoot — but ninja runs with cwd = output
    // dir, so a relative include dir stops resolving). Absolutize the path
    // directly instead (dialect-agnostic), then prepend the prefix, then
    // ninja-$-escape and shell-quote per token (#234) so an include dir
    // whose name contains a space can't silently split into two shell words
    // once ninja hands the resolved command line to the shell.
    std::vector<std::string> includeTokens;
    for (auto& inc : plan.manifest.buildConfig.includeDirs) {
        std::filesystem::path p = inc.has_root_path() ? inc : (plan.projectRoot / inc);
        includeTokens.push_back(include_token(d, p));
    }
    // #249: `[build] include_dirs_after` — searched AFTER the toolchain's
    // system dirs via -idirafter (gcc+clang), so entries can't shadow
    // standard headers. cl.exe has no -idirafter; under the msvc dialect
    // they degrade to regular /I appended at the END of the include list
    // (documented degradation; clang-MSVC uses the gnu dialect).
    const bool msvcInclude = d.includePrefix == std::string_view("/I");
    for (auto& inc : plan.manifest.buildConfig.includeDirsAfter) {
        std::filesystem::path ip(inc);
        std::filesystem::path p = ip.has_root_path() ? ip : (plan.projectRoot / ip);
        includeTokens.push_back(
            include_token(d, p, msvcInclude ? "/I" : "-idirafter"));
    }
    std::string include_flags;
    for (auto& t : includeTokens) {
        include_flags += ' ';
        include_flags += t;   // already prefixed, escaped and quoted
    }

    // Sysroot / payload paths — resolved ONCE by the toolchain link model
    // (mcpp.toolchain.linkmodel, the single source of truth shared with
    // stdmod / build_program / the cfg fixup; see
    // .agents/docs/2026-07-07-hermetic-toolchain-link-model-design.md).
    // Payload-first, --sysroot fallback; for Clang with a cfg file we bypass
    // the (install-time-generated, non-reproducible) cfg with
    // --no-default-config and provide everything explicitly.
    const auto dm = mcpp::toolchain::resolve_clang_driver(plan.toolchain);
    const auto lm = mcpp::toolchain::resolve_link_model(plan.toolchain);
    const mcpp::toolchain::PathEscape ninjaEsc =
        [](const std::filesystem::path& p) { return escape_path(p); };

    std::string compile_toolchain_flags;
    std::string link_toolchain_flags;
    const bool isClangWithCfg = dm.hasCfg;
    // LLVM root of a clang-with-cfg toolchain — used by the macOS link
    // path below to locate libc++.a/libc++abi.a for staticStdlib.
    std::filesystem::path llvmRootForStdlib;

    // Compile side: the shared producer (mcpp.toolchain.hostflags), which the
    // std module build and the build.mcpp host compile also use. It emits
    // clang-cfg bypass → macOS deployment target → C library headers, the
    // order this function has always used.
    //
    // The macOS deployment target is on the command line rather than left to
    // the environment so (a) the ninja commands don't depend on env
    // propagation and (b) the value participates in the BMI fingerprint via
    // canonical flags — mixing targets in one sandbox otherwise reuses a
    // std.pcm built for a different arm64-apple-macosxNN triple and dies with
    // a config mismatch (observed on macos CI). The link side is added to
    // f.ld below (the macOS link path doesn't consume link_toolchain_flags).
    //
    // binutilsPrefix / runtimeLibDirs stay off here: this function computes
    // -B separately into f.bFlag, and routes runtime dirs through
    // depRuntimeLibraryDirs.
    {
        mcpp::toolchain::HostFlagOptions hopt;
        hopt.cfgBypass = mcpp::toolchain::HostFlagOptions::CfgBypass::Always;
        hopt.macosDeploymentTarget = macosDeploymentTarget;
        compile_toolchain_flags = mcpp::toolchain::render_tokens(
            mcpp::toolchain::host_compile_tokens(plan.toolchain, hopt, ninjaEsc));
    }
    if (isClangWithCfg) {
        llvmRootForStdlib = dm.llvmRoot;
        // Linker flags that cfg normally provides. The payload C-runtime
        // flags (-B/-L/loader) are appended via payload_ld below.
        link_toolchain_flags = " --no-default-config";
        if (lm.mode == mcpp::toolchain::CLibMode::Sysroot)
            link_toolchain_flags += lm.link_flags(ninjaEsc);
        link_toolchain_flags +=
            mcpp::toolchain::ClangDriverModel::kLinkDriverFlags;
        f.sysroot = link_toolchain_flags;
    } else if (lm.mode != mcpp::toolchain::CLibMode::None) {
        // GCC (or Clang without cfg): --sysroot from probe, or the payload
        // headers + C runtime (-B for crt discovery, -L for -lc/-lm).
        link_toolchain_flags = lm.link_flags(ninjaEsc);
        f.sysroot = link_toolchain_flags;
    }

    // Binutils -B flag — a GCC/libstdc++ payload concern (musl and MinGW-w64
    // cross both bundle their own as/ld; Clang and MSVC never take an external
    // binutils). MinGW must not get the Linux binutils -B — its PE/SEH output
    // is only assemblable by its own x86_64-w64-mingw32-as.
    bool isMuslTc  = mcpp::toolchain::is_musl_target(plan.toolchain);
    bool isMingwTc = mcpp::toolchain::is_mingw_target(plan.toolchain);
    std::filesystem::path binutilsBin;
    if (!isMuslTc && !isMingwTc && caps.stdlib_id == "libstdc++") {
        auto ar = mcpp::toolchain::archive_tool(plan.toolchain);
        if (!ar.empty())
            binutilsBin = ar.parent_path();
    }
    std::string b_flag;
    if (!binutilsBin.empty()) {
        b_flag = " -B" + escape_path(binutilsBin);
        f.bFlag = b_flag;
    }

    // AR binary
    f.arBinary = mcpp::toolchain::archive_tool(plan.toolchain);

    // Opt level + debug come from the resolved build profile
    // ([profile.<name>] → buildConfig). musl keeps -Og as an ICE workaround
    // unless the profile pins -O0.
    auto& prof = plan.manifest.buildConfig;
    std::string opt_flag = isMuslTc && prof.optLevel != "0"
        ? " -Og"
        : (isMsvcDialect && prof.optLevel == "0")
        ? " /Od"    // MSVC's no-opt spelling (there is no /O0)
        : std::format(" {}{}", d.optPrefix, prof.optLevel);
    if (prof.debug) opt_flag += std::format(" {}", d.debugFlags);
    if (prof.lto && !isMsvcDialect) opt_flag += " -flto";

    // MSVC baseline: /nologo /EHsc /utf-8 (dialect alwaysFlags) + the CRT
    // model — /MD default, /MT under static linkage (portable-by-default is
    // impossible on MSVC-ABI; /MT at least removes the vcruntime DLL dep).
    std::string msvc_base;
    if (isMsvcDialect) {
        msvc_base = std::format(" {}", d.alwaysFlags);
        msvc_base += (plan.manifest.buildConfig.linkage == "static") ? " /MT" : " /MD";
    }

    // User link flags
    std::string user_ldflags;
    for (auto const& flag : plan.manifest.buildConfig.ldflags) {
        user_ldflags += ' ';
        user_ldflags += normalize_ldflag(plan.projectRoot, flag);
    }

    // C standard
    std::string c_std =
        plan.manifest.buildConfig.cStandard.empty() ? "c11" : plan.manifest.buildConfig.cStandard;

    // Assemble
    // Module-flag spellings come from BmiTraits: GCC needs -fmodules on every
    // TU (BMIs implicit); Clang/MSVC reference the staged std BMI and a BMI
    // search dir explicitly (spelled -fmodule-file=/-fprebuilt-module-path vs
    // /reference//ifcSearchDir).
    auto traits = mcpp::toolchain::bmi_traits(plan.toolchain);
    std::string module_flag{traits.compileModulesFlag};
    // A BMI flag and its path are ONE shell word, so the quotes have to wrap
    // both — a build under `/Users/me/my work dir/…` otherwise hands the
    // shell `-fmodule-file=std=/Users/me/my`, `work`, `dir/…` and the compile
    // dies on "no such file or directory: 'work'" with nothing naming the
    // flag that split. The BmiTraits prefixes carry a leading space for this
    // string channel, and MSVC's is itself two words (`/reference std=`), so
    // split at the LAST space: everything before it stays outside the quotes.
    auto bmi_flag = [](std::string_view prefix, const std::filesystem::path& p) {
        auto sp = prefix.find_last_of(' ');
        std::string_view lead = sp == std::string_view::npos
                              ? std::string_view{} : prefix.substr(0, sp + 1);
        std::string_view body = sp == std::string_view::npos
                              ? prefix : prefix.substr(sp + 1);
        return std::string(lead)
             + shell_quote_arg(escape_ninja_chars(std::string(body) + p.string()));
    };
    std::string std_module_flag;
    if (!traits.stdBmiUsePrefix.empty() && !plan.stdBmiPath.empty()) {
        std_module_flag = bmi_flag(traits.stdBmiUsePrefix,
                                   staged_std_bmi_path(plan));
    }
    std::string std_compat_module_flag;
    if (!traits.stdCompatBmiUsePrefix.empty() && !plan.stdCompatBmiPath.empty()) {
        auto compatDst = mcpp::toolchain::staged_std_compat_bmi_path(
            plan.toolchain, plan.outputDir);
        std_compat_module_flag = bmi_flag(traits.stdCompatBmiUsePrefix, compatDst);
    }
    std::string prebuilt_module_flag;
    if (traits.needsPrebuiltModulePath) {
        // Absolute path: a bare `pcm.cache` / `gcm.cache` works at ninja
        // time because ninja runs commands with cwd = outputDir, but the
        // same flag ends up verbatim in `compile_commands.json` whose
        // `directory` field is the project root. clangd does `cd directory`
        // before resolving the flag, so a bare relative path points at
        // `<projectRoot>/pcm.cache` (which doesn't exist) and `import`
        // resolution fails with `module 'X' not found`. The other
        // `-fmodule-file=` flags in this block are already escape_path'd
        // (absolute) for the same reason — this one was a leftover.
        prebuilt_module_flag = bmi_flag(traits.bmiSearchPrefix,
                                        plan.outputDir / traits.bmiDir);
    }
    std::string cxx_std_flag =
        plan.cppStandardFlag.empty()
            ? std::format("{}c++23", d.stdPrefix) : plan.cppStandardFlag;
    // plan.dialectFlags rides right behind -std= (issue #210): module-graph-
    // global dialect flags reach every TU (deps included) via this global
    // cxxflags string, exactly like the standard flag itself.
    f.cxx = std::format("{}{}{}{}{}{}{}{}{}{}{}{}", cxx_std_flag, plan.dialectFlags,
                        msvc_base, module_flag, std_module_flag,
                        std_compat_module_flag, prebuilt_module_flag,
                        opt_flag, pic_flag, compile_toolchain_flags, b_flag, include_flags);
    // MSVC compiles C with cl.exe too; /std: for C uses cN spellings — skip
    // the C standard flag there (cl defaults are fine for the C entry TUs).
    f.cc = isMsvcDialect
        ? std::format("{}{}{}{}{}", msvc_base, opt_flag, compile_toolchain_flags,
                      b_flag, include_flags)
        : std::format("{}{}{}{}{}{}{}", d.stdPrefix, c_std, opt_flag, pic_flag,
                      compile_toolchain_flags, b_flag, include_flags);

    // GAS assembly (.S/.s via the C driver): the asm-safe subset — no -std
    // (C-only) and no -O (meaningless), but PIC stays (.S sources gate on
    // __PIC__), -g is fine, and the toolchain-location flags must come along
    // (hermetic link model: never fall back to a host `as`). MSVC dialect has
    // no GAS path — prepare hard-errors before these flags are consumed.
    f.as = std::format("{}{}{}{}{}",
                       prof.debug ? " -g" : "", pic_flag,
                       compile_toolchain_flags, b_flag, include_flags);

    // NASM (.asm): fixed GNU-ish spelling of its own — include dirs are
    // re-spelt with -I regardless of dialect (nasm ≥2.14 inserts a missing
    // path separator itself); DWARF debug info exists on ELF only.
    if (!plan.nasmPath.empty()) {
        std::string nasm_includes;
        for (auto& inc : plan.manifest.buildConfig.includeDirs) {
            auto abs = inc.is_absolute() ? inc : (plan.projectRoot / inc);
            nasm_includes += " -I" + escape_path(abs);
        }
        // #249: nasm has no system header dirs to defer to — after-dirs
        // degrade to plain -I appended at the end.
        for (auto& inc : plan.manifest.buildConfig.includeDirsAfter) {
            std::filesystem::path ip(inc);
            auto abs = ip.is_absolute() ? ip : (plan.projectRoot / ip);
            nasm_includes += " -I" + escape_path(abs);
        }
        std::string nasm_debug;
        if (prof.debug && plan.nasmFormat.starts_with("elf"))
            nasm_debug = " -g -F dwarf";
        f.nasm = nasm_debug + nasm_includes;
    }

    // Link flags
    f.staticStdlib = plan.manifest.buildConfig.staticStdlib;
    f.linkage = plan.manifest.buildConfig.linkage;
    // Whether the ARTIFACT can be fully static is a property of the target,
    // not of this machine. Reading the host constant directly here dropped
    // `-static` from every Windows→Linux cross build, silently turning the
    // musl targets into something they are not. The host constant is still
    // the right answer for a host-target build, so it is threaded in as the
    // fallback rather than discarded.
    const bool full_static_ok = mcpp::toolchain::target_supports_full_static(
        plan.toolchain.targetTriple, mcpp::platform::supports_full_static);
    std::string full_static = (full_static_ok && f.linkage == "static") ? " -static" : "";

    // ---- C++ runtime distribution contract (issue #336) -------------------
    //
    // THE single derivation of "does this artifact carry its own C++ runtime",
    // for every role and every platform. `-static-libstdc++`, MinGW's
    // `-static` and macOS's `-load_hidden` archives all come out of here now;
    // nothing below re-decides it. The flags land in the PER-UNIT channel
    // (`unit_ldflags`) rather than the global one because two roles in the
    // same build may hold different contracts.
    {
        namespace dist = mcpp::build::dist;
        auto const& bc = plan.manifest.buildConfig;

        // `static_stdlib` is a faithful alias of the two ends of the contract:
        // its documented meaning has always been exactly self-contained vs the
        // dynamic system runtime. An explicit `cxx_runtime` wins.
        const dist::Contract base =
            dist::parse_contract(bc.cxxRuntime).value_or(
                bc.staticStdlib ? dist::Contract::SelfContained
                                : dist::Contract::HostCoupled);
        const dist::Contract testsContract =
            dist::parse_contract(bc.cxxRuntimeTests).value_or(base);

        // Archive lookup. LLVM lays these out either directly under lib/ (the
        // macOS packages) or under lib/<llvm-triple>/ (the Linux ones), so try
        // both rather than hard-coding one layout. Sorted so the choice cannot
        // depend on directory iteration order.
        auto find_archive = [&](std::string_view name) -> std::filesystem::path {
            if (llvmRootForStdlib.empty()) return {};
            std::error_code ec;
            auto libDir = llvmRootForStdlib / "lib";
            auto direct = libDir / name;
            if (std::filesystem::exists(direct, ec)) return direct;
            std::vector<std::filesystem::path> hits;
            for (auto& e : std::filesystem::directory_iterator(libDir, ec)) {
                std::error_code de;
                if (!e.is_directory(de)) continue;
                auto p = e.path() / name;
                if (std::filesystem::exists(p, de)) hits.push_back(p);
            }
            std::ranges::sort(hits);
            return hits.empty() ? std::filesystem::path{} : hits.front();
        };

        // Does this archive define a given symbol? Answered by scanning the
        // ranlib symbol index, which a BSD/Mach-O archive keeps in its FIRST
        // member — so a bounded read of the head is enough and no toolchain
        // subprocess is involved. Used for exactly one thing: refusing to
        // generate the macOS ordering shim against a libc++ that does not
        // export the symbol it binds. That failure mode is not hypothetical —
        // the first CI round of #336 turned every macOS link into `undefined
        // symbol` — and a check here cannot fail the build the way a bad
        // reference in the generated TU can.
        auto archive_defines = [](const std::filesystem::path& p,
                                  std::string_view sym) -> bool {
            if (p.empty()) return false;
            std::ifstream is(p, std::ios::binary);
            if (!is) return false;
            constexpr std::streamsize kHead = 8 << 20;
            std::string head(static_cast<std::size_t>(kHead), '\0');
            is.read(head.data(), kHead);
            head.resize(static_cast<std::size_t>(is.gcount()));
            return head.find(sym) != std::string::npos;
        };

        dist::MechanismInput mi;
        mi.stdlibId       = caps.stdlib_id;
        mi.hostIsWindows  = mcpp::platform::is_windows;
        mi.fullStaticLibc = (f.linkage == "static");
        mi.mingw          = isMingwTc;
        mi.macosFloor     = !macosDeploymentTarget.empty();
        if constexpr (mcpp::platform::needs_explicit_libcxx) {
            mi.format = dist::Format::MachO;
        } else if constexpr (mcpp::platform::is_windows) {
            mi.format = dist::Format::Pe;
        }
        // Target-keyed, not host-keyed: a Linux-hosted MinGW cross build
        // produces a PE and must take the PE mechanism.
        if (isMingwTc) mi.format = dist::Format::Pe;

        const bool wantsArchives =
            (base == dist::Contract::SelfContained
             || testsContract == dist::Contract::SelfContained)
            && caps.stdlib_id == "libc++";
        if (wantsArchives) {
            auto libcxxA    = find_archive("libc++.a");
            auto libcxxAbiA = find_archive("libc++abi.a");
            mi.libcxxArchive    = libcxxA.empty()    ? std::string{} : escape_path(libcxxA);
            mi.libcxxAbiArchive = libcxxAbiA.empty() ? std::string{} : escape_path(libcxxAbiA);
            // ELF only: without it the "self-contained" binary still pulls
            // libunwind.so.1. Mach-O's libc++abi.a carries its own unwinder.
            if (mi.format == dist::Format::Elf) {
                auto unwindA = find_archive("libunwind.a");
                if (!unwindA.empty()) mi.libunwindArchive = escape_path(unwindA);
            } else {
                // Searched without the leading underscore so the same needle
                // matches the ELF (`_ZN...`) and Mach-O (`__ZN...`) spellings.
                mi.streamInitSymbolPresent =
                    archive_defines(libcxxA, "ZNSt3__18ios_base4InitC1Ev");
            }
        }

        // "Explicit" = a human wrote it down. `static_stdlib = false` counts:
        // nobody sets a flag to its default to get non-default behavior.
        const bool explicitBase  = !bc.cxxRuntime.empty() || !bc.staticStdlib;
        const bool explicitTests = explicitBase || !bc.cxxRuntimeTests.empty();

        for (auto [role, requested, wasAsked] : {
                 std::tuple{dist::Role::Distributable, base,          explicitBase},
                 std::tuple{dist::Role::Test,          testsContract, explicitTests},
                 std::tuple{dist::Role::Intermediate,  base,          explicitBase}}) {
            mi.role            = role;
            mi.requested       = requested;
            mi.explicitRequest = wasAsked;
            auto r = dist::resolve(mi);
            auto i = static_cast<std::size_t>(role);
            f.ldStdlibByRole[i] = r.unitFlags;
            f.contractByRole[i] = r.effective;
            if (r.streamInitShim) f.needsStreamInitShim = true;
            if (!r.diagnostic.empty())
                f.diagnostics.push_back(std::format(
                    "{} target: {}", dist::to_string(role), r.diagnostic));
        }
        // Two roles usually share a contract, so they usually share a
        // complaint; report each distinct one once.
        std::ranges::sort(f.diagnostics);
        f.diagnostics.erase(std::ranges::unique(f.diagnostics).begin(),
                            f.diagnostics.end());
    }

    std::string runtime_dirs;
    if constexpr (mcpp::platform::supports_rpath) {
        // Toolchain runtime dirs (glibc/gcc) as before...
        for (auto& dir : plan.toolchain.linkRuntimeDirs) {
            runtime_dirs += " -L" + escape_path(dir);
            runtime_dirs += " -Wl,-rpath," + escape_path(dir);
        }
        // ...plus dependency packages' [runtime] library_dirs (e.g.
        // compat.glx-runtime's host-GL passthrough), so dlopen()'d host libs
        // (libGL/libGLX) are reachable at run time. Only the dep dirs — NOT the
        // glibc payload dir — so static/musl links stay clean.
        for (auto& dir : plan.depRuntimeLibraryDirs) {
            runtime_dirs += " -L" + escape_path(dir);
            runtime_dirs += " -Wl,-rpath," + escape_path(dir);
        }
    }

    // For Clang with payload paths: the payload C runtime — -B so the driver
    // resolves Scrt1.o/crti.o/crtn.o inside the payload (the driver never
    // consults -L for CRT objects; without -B it silently falls back to the
    // host's /lib or, on hosts without a system toolchain, passes bare names
    // that lld cannot open — issue #195), -L/-rpath for -lc/-lm, and the
    // payload's dynamic linker.
    std::string payload_ld;
    if (isClangWithCfg && lm.mode == mcpp::toolchain::CLibMode::PayloadFirst)
        payload_ld = lm.link_flags(ninjaEsc);

    std::string link_extra;
    if (prof.lto)   link_extra += " -flto";
    if (prof.strip) link_extra += " -s";

    // MinGW PE link — keyed on the TARGET (is_mingw_target), NOT the host: a
    // Linux-hosted cross build produces exactly the same PE link as a native
    // Windows MinGW build (host≠target). No rpath/loader/payload model. Static
    // + libstdc++exp (std::print's __open_terminal/__write_to_terminal live in
    // libstdc++exp.a, not plain libstdc++). Self-contained binutils → no -B.
    if (isMingwTc) {
        // `-static` / `-static-libstdc++` now come from the contract table via
        // unit_ldflags (dist::Format::Pe) — the whole-link `-static` is what
        // "self-contained" means here, since the piecemeal recipe still leaves
        // libwinpthread-1.dll behind.
        std::string mingw_stdexp;
        if (caps.stdlib_id == "libstdc++")
            mingw_stdexp = " -lstdc++exp";
        f.ld = std::format("{}{}{}", user_ldflags, mingw_stdexp, link_extra);
        return f;
    }

    if constexpr (mcpp::platform::is_windows) {
        if (isMsvcDialect) {
            // Native cl.exe: link.exe does the link (SeparateLinker). Search
            // paths for dependency runtime import libs via /LIBPATH; user
            // ldflags pass through verbatim; GNU link_extra (-flto/-s) does
            // not apply.
            f.ldBinary = mcpp::toolchain::link_tool(plan.toolchain);
            std::string libpaths;
            for (auto& dir : plan.depRuntimeLibraryDirs)
                libpaths += " /LIBPATH:" + escape_path(dir);
            f.ld = libpaths + user_ldflags;
            return f;
        }
        // PE link, MSVC-ABI Clang (native MinGW is handled by the target-keyed
        // branch above and has already returned): no rpath/loader/payload —
        // MSVC STL/SDK come via the driver, nothing extra needed.
        f.ld = std::format("{}{}", user_ldflags, link_extra);
    } else if constexpr (mcpp::platform::needs_explicit_libcxx) {
        // macOS. The C++ runtime itself is decided by the contract table above
        // (dist::Format::MachO) and rides unit_ldflags; what is left here is
        // the rest of the macOS link:
        //
        // 1. deployment target — mirror MACOSX_DEPLOYMENT_TARGET onto the
        //    link command line so it doesn't depend on env propagation. The
        //    static-libc++ mechanism exists to make this floor REAL: `-lc++`
        //    resolves to the SYSTEM /usr/lib/libc++.1.dylib, which caps the
        //    runnable version at the build host's OS (std::print's
        //    __is_posix_terminal support symbol only exists in macOS 15's
        //    libc++, so a minos-14 binary died at launch on macos-14 CI).
        // 2. linker — use LLVM's own lld (same as the Linux clang path)
        //    instead of Xcode's ld: the system ld's version floats with
        //    the host Xcode (observed: Xcode 15.4's ld aborting at launch
        //    on macos-14 CI when its libc++ resolution was diverted), and
        //    lld ships with the exact toolchain doing the compile.
        //
        // TODO(macos-floor-11): the official LLVM archives are built for
        // macOS 14; supporting 11-13 needs a custom libc++ build shipped
        // via xlings-res (data-only change — swap the archive source).
        // Tracked in xlings
        // .agents/docs/2026-06-05-macos-min-version-support.md §5.
        std::string version_min;
        if (!macosDeploymentTarget.empty()) {
            version_min = " -mmacosx-version-min=" + macosDeploymentTarget;
        }
        // Pass the macOS SDK to the LINKER explicitly. The link otherwise relies
        // on clang's implicit SDK detection (xcrun/SDKROOT → ld64 -syslibroot)
        // to resolve -lSystem and friends. On a clean Xcode (CI) that works, so
        // the gap is latent; but on a machine where that detection fails —
        // misconfigured `xcode-select`, Command-Line-Tools-only, or a freshly
        // installed bundled clang — ld64.lld dies with "library not found for
        // -lSystem". -isysroot makes it deterministic regardless of the host's
        // developer-tools state. (compile side already gets --sysroot above.)
        std::string macos_sdk;
        if (auto sdk = mcpp::platform::macos::sdk_path())
            macos_sdk = " -isysroot " + escape_path(*sdk);
        f.ld = std::format("{}{}{} -fuse-ld=lld{}{}{}", full_static,
                           b_flag, macos_sdk, version_min, user_ldflags, link_extra);
    } else {
        // libatomic: 16-byte / oversized std::atomic needs the out-of-line
        // __atomic_* libcalls from libatomic, which the driver won't add on
        // its own. Inject `-latomic` (under --as-needed) after runtime_dirs
        // so its -L entries are on the search path; self-guards on the lib
        // actually being present (see atomic_link_flag).
        std::string atomic_ld = atomic_link_flag(plan.toolchain.linkRuntimeDirs,
                                                 !full_static.empty());
        f.ld = std::format("{}{}{}{}{}{}{}{}", full_static, link_toolchain_flags, b_flag,
                           runtime_dirs, atomic_ld, payload_ld, user_ldflags, link_extra);
    }

    return f;
}

}  // namespace mcpp::build
