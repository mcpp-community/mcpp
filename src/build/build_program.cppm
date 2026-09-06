// mcpp.build.build_program — L3 `build.mcpp`: a project-local native imperative
// build program (Zig's build.zig / Cargo's build.rs model, but in C++ so it
// dogfoods mcpp). Compiled with the HOST toolchain and run BEFORE the main build;
// it emits stdout `mcpp:` directives that augment the main build (extra flags,
// link libraries/search dirs, defines, generated sources). A declared-input cache
// (Discipline 2) re-runs it only when its source, a declared input, or a declared
// env var changes — the documented replacement for the bare `.mcpp_ok` marker.
//
// See .agents/docs/2026-06-30-l3-build-mcpp-implementation-design.md.

module;

export module mcpp.build.build_program;

import std;
import mcpp.manifest;
import mcpp.platform;
import mcpp.platform.process;
import mcpp.toolchain.cppfly;        // std_flag (dialect- and c++fly-aware -std= spelling)
import mcpp.toolchain.dialect;       // CommandDialect — gnu vs cl.exe spellings
import mcpp.toolchain.fingerprint;   // hash_file / hash_string (FNV-1a, 16 hex)
import mcpp.build.directives;        // the directive definition table (own module: see its header)
import mcpp.build.refusal;           // the machine-readable identity of a refusal
import mcpp.build.hostprogram;       // bundled `mcpp` module compile (own module: see its header)
import mcpp.toolchain.hostflags;     // the shared host-compile flag producer
import mcpp.toolchain.linkmodel;     // shared C-library / clang-cfg-bypass model
import mcpp.toolchain.model;         // Toolchain, PayloadPaths, is_clang/is_musl_target/is_mingw_target
import mcpp.toolchain.registry;      // archive_tool
import mcpp.toolchain.stdmod;        // ensure_built — the SAME std BMI the main build uses
import mcpp.toolchain.triple;        // host_triple (MCPP_HOST contract value)
import mcpp.ui;
import mcpp.version;         // MCPP_VERSION — the hint names the engine the reader is on

export namespace mcpp::build {

// Build-program environment contract (G3) — what the running build.mcpp can
// see, mirroring Cargo's env family. Injected as MCPP_* variables into the
// child ONLY (never the calling process), and folded into the cache key so a
// target/profile/feature change re-runs the program.
struct BuildProgramEnv {
    std::string targetTriple;               // resolved canonical triple; "" = host
    // The resolved toolchain's payload root and the target's own C library
    // root. Both exist so a package can ASK instead of DECLARE — see
    // hostprogram::toolchain_dir / sysroot_dir for why declaring was wrong.
    std::string toolchainDir;
    std::string targetSysroot;
    // THE TWO ANSWERS A SECOND COMPILER NEEDS AND CANNOT DERIVE.
    //
    // `toolchainSysroot` is the `--sysroot` mcpp passes to its own compiler and
    // `toolchainBinutilsDir` the directory it names with `-B`; either is empty
    // when mcpp passes none. They are not the same question as
    // `targetSysroot`, which is a TIER fact (a bare-metal target's own C
    // library payload, empty on a hosted target) — these two are ENVIRONMENT
    // facts, and on a hosted subos both are non-empty precisely because the C
    // library is not at `/usr/include` and the assembler is not at `/usr/bin`.
    //
    // Measured 2026-09-05 on the CUDA example. `nvcc` refuses a libc++ host
    // compiler and fails on GCC 16's `<type_traits>`, so its rule package
    // resolves a second host compiler from a declared payload. That compiler
    // is not one mcpp resolved, so nothing tells it where anything is, and the
    // first `#include` in NVIDIA's own `crt/host_config.h` fails:
    //
    //     host_config.h:218: fatal error: features.h: No such file or directory
    //
    // Every rule package driving a compiler mcpp did not resolve has the same
    // gap — `hipcc`, `-fsycl-host-compiler`, a generator that compiles what it
    // emits — so the answer belongs to the engine and is stated once here.
    std::string toolchainSysroot;
    std::string toolchainBinutilsDir;
    // WHICH COMPILER RESOLVED — "gcc" | "clang" | "msvc" | "".
    //
    // A package should never have to guess this, and until this field existed
    // the only way to was to look at `toolchainDir` and recognise a directory
    // name. The question is real and recurring: the routines a compiler emits
    // calls to and no C library defines live in `libgcc.a` under one and in
    // compiler-rt under another, and the tool that turns a `.def` into an
    // import library is `dlltool` under one and `llvm-dlltool` under another.
    //
    // Measured 2026-08-22, both on the same day and both from the same
    // missing answer: `openkal-musl` naming `-lgcc` on a link whose compiler was
    // clang (`unable to find library -lgcc`), and `openkal-windows` running
    // `llvm-dlltool` under a GCC toolchain (`sh: 1: llvm-dlltool: not found`).
    // Each package had made the assumption its author's toolchain made true.
    std::string compilerId;
    // WHICH C++ STANDARD LIBRARY RESOLVED — "libstdc++" | "libc++" |
    // "msvc-stl" | "".
    //
    // `compilerId` does not answer this. clang links libc++ on one machine and
    // libstdc++ on another, both reporting "clang", and the two differ in what
    // they accept: llama.cpp-m's Vulkan backend does not compile under libc++
    // because upstream destroys a `unique_ptr` to an incomplete type, which
    // libstdc++ accepts and libc++ rejects. A package that wants to refuse
    // early, by name, has no other way to ask -- and refusing on the compiler
    // name would also refuse clang with libstdc++, which works.
    //
    // The engine has resolved this value for a long time: it is in the cache
    // key, the ABI tag, the toolchain fingerprint and `resolution.json`. It was
    // simply never handed to the layer that had to decide on it.
    std::string cxxStdlib;
    // Three more answers a board-support package would otherwise hardcode.
    //
    // THE COUPLING THESE REMOVE IS INVISIBLE IN A MANIFEST. `riscv-virt-rt`
    // declares no dependency on LLVM or on picolibc — #459 removed those — and
    // yet it named `clang_rt.builtins-riscv64` (a compiler-rt fact, `libgcc`
    // under GCC) and `rv64gc/lp64d` (picolibc's multilib convention). A
    // declared dependency is visible; a hardcoded name is not, and it fails
    // only when something is swapped.
    //
    // The division of labour is the same one the layering already uses:
    // location is a target fact, selection is a board fact. Which builtins
    // library exists is decided by the compiler, and no board chooses to go
    // without one; where a profile's libraries live is the C library's
    // convention. Both belong to the engine, which knows them already.
    std::string targetBuiltinsLib;          // "clang_rt.builtins-riscv64" | "gcc" | ""
    std::string targetLibcProfile;          // "rv64gc/lp64d" | ""
    std::string targetLibc;                 // "picolibc-riscv" | "" (zero-libc tier)
    std::string profile;                    // effective profile name (dev/release/…)
    std::vector<std::string> features;      // active feature closure of the package
    // The device axis of this build, in the wire form `mcpp.pack.abi_tag`
    // reads (`cuda12.9+{sm_89} ptx>=89`); empty when the build asks for no
    // accelerator. Already resolved -- `--accel` / `--no-accel` over
    // `[build] accel` -- so a rule package derives its own spelling
    // (`-gencode`, `--offload-arch`) from here and the architecture set is
    // written once, in the manifest, and never again in a build program.
    std::string accel;
    // The device-kind sources (`.cu`, `.hip`, ...) this package's effective
    // source set matches, package-root-relative with `/` separators, one per
    // line. The engine has no compile rule for them and hands the list to the
    // build program, where the rule package the package imports turns each
    // one into an `mcpp::action`. Already narrowed: a glob whose `accel`
    // constraint the build does not satisfy contributes nothing.
    std::vector<std::string> deviceSources;
    // Artifact home (bin/cache/out). Empty → <root>/target/.build-mcpp (the
    // root-project default). Dependencies MUST point this into the CONSUMING
    // project's tree — a registry package root is shared and may be read-only.
    std::filesystem::path artifactsDir;
    // Base for resolving relative `mcpp:generated=` paths. Empty → root (the
    // root-project contract, unchanged). Dependencies point this at OUT_DIR so
    // a shared package root is never written to.
    std::filesystem::path genBase;
    // mcpp#241: this package's resolved dependencies, as (name → dir) pairs.
    // The caller emits each dep under BOTH its canonical package name and its
    // short (namespace-stripped) name, so a build.mcpp can locate a dependency's
    // payload (e.g. a data-asset package) via either spelling. Emitted as
    // MCPP_DEP_<SANITIZED_NAME>_DIR (same sanitizer as MCPP_FEATURE_) instead of
    // reverse-engineering the store layout.
    std::vector<std::pair<std::string, std::filesystem::path>> depDirs;
    // Payload dirs of the packages this build declared in `[xlings] deps`,
    // as (env var name → dir). Resolved by the caller through the xlings path
    // helpers, so a build.mcpp asks `xpkg_dir("xim", "picolibc-riscv")`
    // instead of reconstructing `<home>/data/xpkgs/<ns>-x-<name>/<version>`
    // — the same reason depDirs exists for mcpp dependencies.
    std::vector<std::pair<std::string, std::string>> xpkgDirs;
    // #355: HOST tools this package asked its dependencies for, as
    // (env var name → absolute path to the executable) pairs. The caller has
    // already resolved them (built, taken from the store, or an override), so
    // this is purely the delivery channel. Rides the same contract env, hence
    // the same re-run key: a rebuilt tool re-runs the program that uses it,
    // with no `rerun-if-changed` needed from the author.
    std::vector<std::pair<std::string, std::string>> toolPaths;
    // THE `bin` OF THIS PROJECT'S OWN SubOS, AT THE FRONT OF THE CHILD'S
    // `PATH`. Empty for a project that has not declared one, and an empty
    // value means the child's `PATH` is left exactly as mcpp received it.
    //
    // A build program that needs a tool has, until this field, had to ask
    // `PATH` the way a shell script would — and `PATH` answers about the
    // MACHINE, not about this build. Measured 2026-08-25: a probe for
    // `qemu-system-riscv64` found one that answers, when executed,
    //
    //     [error] qemu-system-riscv64 is not installed in this subos (_)
    //
    // — present, and unable to run — while the copy the project had declared
    // sat in its own payload directory, reachable only by a path the program
    // would have had to construct itself.
    //
    // THE PROJECT'S SubOS, NEVER A GLOBAL ONE. An earlier draft put this
    // build system's shared `subos/default/bin` in front, which makes what a
    // build sees depend on what else has been installed on the machine — two
    // projects on one machine would agree with each other, and the same
    // project on two machines would not. A declared `[xlings].subos` is a
    // directory that belongs to the project and travels with it.
    //
    // WHO DECIDES IS NOT DECIDED HERE. `mcpp::xlings::runtime` is the sole
    // project runtime-selection policy and `RuntimeBinding::subosDir` is its
    // resolved answer; this field carries that answer to the child. Deriving
    // it a second time — from the manifest, from `[xlings] deps`, from the
    // config — is how a build ends up with two subos and no way to say which
    // one it used.
    //
    // PREPENDED, NOT SUBSTITUTED. A build program legitimately calls `git`,
    // `python3` or a shell, none of which arrive this way.
    std::string toolsBin;
    // #355 step 5: dependency-provided modules to compile FOR THE HOST and make
    // importable from this build.mcpp — reusable build rules distributed as
    // ordinary mcpp packages (`import mcpp.rules.protobuf;`) instead of a
    // second, non-C++ rule DSL.
    //
    // Compiled with the SAME flags as build.mcpp itself, in the same directory,
    // which is what makes the BMI usable at all — see DependencySpec::hostModule.
    //
    // ORDER IS LOAD-BEARING. The compile loop accumulates `moduleFlags` as it
    // goes, so every entry sees the BMIs of the entries before it. That is the
    // whole mechanism by which a rule can import another rule: the caller
    // topologically sorts this list, and nothing else is required.
    struct HostModuleRef {
        std::string           logical;
        std::filesystem::path interface;
        // False when this module is present ONLY because another rule imports
        // it. Its BMI must exist for that rule to compile, but this build.mcpp
        // never declared it, and a package's build-time provisions cross one
        // further edge only on a `reexport = true` edge. GCC cannot enforce
        // that — its BMIs are implicit under gcm.cache and reachable by name
        // whatever the flags say — so mcpp enforces it, and does so on every
        // platform rather than leaving the rule true only where the compiler
        // happens to help.
        bool                  importable = true;
    };
    std::vector<HostModuleRef> hostModules;
};

// The env-var name `hostprogram::xpkg_dir` reads back. One spelling of the
// sanitizer, shared by both sides — the two drifting apart would make the
// interface answer "" for a package that is right there.
inline std::string xpkg_env_var(std::string_view ns, std::string_view name) {
    std::string out = "MCPP_XPKG_";
    auto put = [&](std::string_view s) {
        for (char c : s)
            out += (c >= 'a' && c <= 'z') ? char(c - 'a' + 'A')
                 : ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) ? c : '_';
    };
    if (!ns.empty()) { put(ns); out += '_'; }
    put(name);
    out += "_DIR";
    return out;
}

// Does a compiler's output say the program asked for something the bundled
// `mcpp` module does not have?
//
// This is the ONLY place an engine-too-old situation can be caught for the
// TYPED api, and it exists because the in-language probe does not:
//
//     if constexpr (requires { mcpp::runner("x"); })      // ← hard error,
//         mcpp::runner("x");                              //   measured
//
// A requires-expression over a qualified name that does not exist is
// ill-formed, not `false`, so a package CANNOT degrade gracefully across mcpp
// versions the way it could across, say, a header's feature macro. The wire
// protocol has its own answer for this (protocol_error() names `mcpp self
// update` for an unknown `mcpp:` key), but that answer needs the program to
// have COMPILED — and a package written against a newer mcpp does not get
// that far. So the raw compiler error is the message, and on its own it says
// only `'runner' is not a member of 'mcpp'`, which reads like the author's
// typo instead of the reader's out-of-date engine.
//
// Deliberately spelling-based, and deliberately broad across the three
// frontends (they phrase it three ways). A false positive costs one extra
// hint line under a genuine typo; a false negative costs a user an afternoon.
bool mentions_missing_mcpp_api(std::string_view compilerOutput);

// Compile + run `<root>/build.mcpp` (if present) with `hostCompiler` (the resolved
// HOST frontend — under a cross --target the caller resolves a host toolchain;
// the program always compiles AND runs on the host) and apply its directives to
// `m.buildConfig`. `tc` supplies the sysroot / runtime flags a fresh sandbox
// needs to compile + link a freestanding host program. No-op when absent.
std::expected<void, std::string> run_build_program(
    mcpp::manifest::Manifest& m,
    const std::filesystem::path& root,
    const std::filesystem::path& hostCompiler,
    const mcpp::toolchain::Toolchain& tc,
    const mcpp::manifest::CppStandardConfig& cppStandard,
    const BuildProgramEnv& env);

// Has any recorded build-program input changed since its build.mcpp cache was
// written: a glob's path SET (#359), a declared file's CONTENT, or a declared
// environment variable's value?
//
// The project-level fast path skips prepare_build entirely when no source is
// newer than build.ninja, and prepare is where the build.mcpp cache is
// normally consulted. A glob input is precisely an input whose change leaves
// every existing file's mtime alone — adding a .proto — so without this ask,
// the fast path would report "Finished dev in 0.00s" and the new file would
// never be generated. That is the same gap the fast path already closes for
// the build.mcpp source itself; a glob is one more kind of build-program input,
// so it belongs to the same question.
//
// A DECLARED FILE IS THE SAME QUESTION AND WAS NOT ASKED (2026.9.5.4). The
// mtime sweep that guards the fast path walks SOURCES, so a data file a build
// program reads -- `rerun_if_changed("data/table.csv")` -- is invisible to it,
// and this function used to skip a cache record that carried no glob at all.
// The result was that editing such a file left the generated header from the
// previous build in place: `Finished dev in 0.00s`, and the program compiled
// the previous bytes. `mcpp.tools.embed` is the case that found it. Contents,
// not mtime, exactly as the cache records them.
//
// Scans the caches under `<projectRoot>/target/.build-mcpp` (the root's own and
// each dependency's). Each cache records the root its entries were relative to,
// so a dependency's input is evaluated against the dependency's tree.
bool program_inputs_stale(const std::filesystem::path& projectRoot);

} // namespace mcpp::build

namespace mcpp::build {

// See the declaration for why this exists at all.
//
// Three frontends, three spellings of the same fact — and MSVC's does not even
// contain the word "member" in the same order, so each is matched literally
// rather than by a shared substring:
//
//   gcc    error: 'runner' is not a member of 'mcpp'
//   clang  error: no member named 'runner' in namespace 'mcpp'
//   cl.exe error C2039: 'runner': is not a member of 'mcpp'
//
// The trailing `'mcpp'` is what keeps this off unrelated failures: a package's
// own missing symbol names its own namespace, not ours.
bool mentions_missing_mcpp_api(std::string_view out) {
    static constexpr std::string_view kNeedles[] = {
        "is not a member of 'mcpp'",       // gcc, and cl.exe's tail
        "in namespace 'mcpp'",             // clang
    };
    for (auto n : kNeedles)
        if (out.find(n) != std::string_view::npos) return true;
    return false;
}

namespace {

namespace fs = std::filesystem;

namespace dirs = mcpp::build::directives;

// The directive model — what a directive IS, how it parses, how it is cached
// and applied — lives in mcpp.build.directives as a single table. This file
// only orchestrates: compile, run, cache, validate. See that module's header
// for why it is separate (both the nine-sites problem and the clang 22
// anonymous-namespace miscompile that forbids growing this one).
using Directives = dirs::Directives;
using dirs::Slot;

// Resolve a possibly-relative path against the project root, returning an
// absolute lexically-normal path (no filesystem touch, so it works for dirs that
// the program is about to create as well as existing ones).
std::string abs_against_root(const fs::path& root, std::string_view p) {
    return dirs::abs_against(root, p);
}

std::string env_value(const std::string& name) {
    const char* v = std::getenv(name.c_str());
    return v ? std::string(v) : std::string();
}

// The host subset of flags.cppm's sysroot/runtime handling — enough to compile +
// link a freestanding host program on a fresh sandbox (where bare `g++ file -o x`
// can't find crt/libc). `tc` is always a HOST-targeting toolchain: under a cross
// --target, prepare.cppm resolves the spec a second time without the target axis
// (host_tc_for_build_program) and passes that here, so the native cases are the
// only ones needed. Passed as separate argv tokens (no shell).
std::vector<std::string> host_base_flags(const mcpp::toolchain::Toolchain& tc,
                                         std::string_view macosDeploymentTarget) {
    // One driver invocation compiles AND links build.mcpp, so it needs both
    // sides. Both come from mcpp.toolchain.hostflags — the same producer
    // flags.cppm and the std module build use. This function used to hand-write
    // the whole assembly, which is how it kept missing what the main build
    // already knew (quoting, the macOS deployment target, the MSVC dialect);
    // see 2026-08-02-host-compile-single-producer-design.md.
    mcpp::toolchain::HostFlagOptions opt;
    // The host helper keeps TRUSTING clang's cfg on macOS/Windows: the macOS
    // link needs the libc++abi/unwind handling the main build's
    // needs_explicit_libcxx path owns, and duplicating it here produced
    // undefined __cxa_* / __gxx_personality_v0.
    opt.cfgBypass = mcpp::toolchain::HostFlagOptions::CfgBypass::LinuxOnly;
    opt.clangStdlibSelect = true;
    // `cAbiPrebuilt` is left at its default (true), and that is a statement
    // rather than an omission: `tc` here is always HOST-targeting (see this
    // function's header), so the helper's C library is the payload's whatever
    // the project's target side turns out to be.
    //
    // It also corrects a latent defect. The predicate this replaced was
    // `!tc.crossTargetFlag.empty()`, and a host toolchain resolved for a cross
    // build could carry one — in which case the helper lost the payload's own
    // headers for a reason that had nothing to do with it.
    // binutils -B so the driver finds ld/as (GCC; musl and MinGW ship their own).
    opt.binutilsPrefix = !mcpp::toolchain::is_musl_target(tc)
                      && !mcpp::toolchain::is_mingw_target(tc);
    // The helper is exec'd outside anything mcpp controls, so it must be able
    // to find the toolchain's private runtime libs itself.
    opt.runtimeLibDirs = true;
    opt.macosDeploymentTarget = std::string(macosDeploymentTarget);

    const mcpp::toolchain::PathEscape plain = mcpp::toolchain::no_escape;
    auto f = mcpp::toolchain::host_compile_tokens(tc, opt, plain);
    for (auto& t : mcpp::toolchain::host_link_tokens(tc, opt, plain))
        f.push_back(t);
    return f;
}

// ── Cache (line-based; one record per line, internal format) ───────────────
// epoch <n>
// program <hash>
// compiler <hash>
// ctx <hash>
// in <contenthash> <path>
// env <valuehash> <NAME>
// d <tag> <verbatim value to end of line>
// The leading epoch/program/compiler/ctx/in/env lines are the re-run key; the
// `d` lines are the directives to reapply on a hit. The `d` tag vocabulary is
// owned by mcpp.build.directives::kTable and is NOT spelled here — that list
// used to be duplicated in four places and drifted.

// build.mcpp artifacts live under target/ (the build output tree), not in the
// project: target/.build-mcpp/{build.mcpp.bin, build.mcpp.cache}. A stable subdir
// (not the fingerprint-keyed one — build.mcpp runs before the fingerprint exists)
// so the binary + cache survive across builds and aren't rebuilt needlessly.
// A dependency's artifacts are redirected into the CONSUMING project's tree
// via BuildProgramEnv::artifactsDir (a registry root may be read-only).
fs::path build_dir(const fs::path& root, const BuildProgramEnv& env) {
    return env.artifactsDir.empty() ? root / "target" / ".build-mcpp"
                                    : env.artifactsDir;
}

std::string cache_path(const fs::path& bdir) {
    return (bdir / "build.mcpp.cache").string();
}

// MCPP_FEATURE_<NAME> spelling — same sanitizer as the compile-side
// -DMCPP_FEATURE_ macro (prepare.cppm): uppercase, non-alnum → '_'.
std::string sanitize_feature_env(std::string f) {
    for (auto& c : f)
        c = std::isalnum(static_cast<unsigned char>(c))
          ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : '_';
    return f;
}

// The injected contract values, as (NAME, value) pairs for the child process.
std::vector<std::pair<std::string, std::string>>
contract_env(const fs::path& root, const fs::path& outDir, const BuildProgramEnv& env) {
    std::vector<std::pair<std::string, std::string>> e;
    auto hostT = mcpp::toolchain::triple::host_triple().str();
    e.emplace_back("MCPP_TARGET", env.targetTriple.empty() ? hostT : env.targetTriple);
    // THE SAME VALUE UNFILLED — EMPTY WHEN NOBODY NAMED A TARGET.
    //
    // `MCPP_TARGET` above answers "which machine is this for", and filling it
    // in with the host is right for that question. It cannot answer a different
    // one that a platform package has to ask: **was this build POINTED at a
    // target**, or is it an ordinary native build?
    //
    // The two are not the same even when the triples are equal. `mcpp build
    // --target aarch64-macos` on an arm64 Mac names the same machine the host
    // is, and yet it is the graph that supplies the target side — so this tool
    // puts no system SDK on the link, and the package that knows the system is
    // the only thing that can name one. A native build on the same machine gets
    // the SDK and needs nothing from the package.
    //
    // Measured 2026-08-23, `openkal-macos` trying to decide this from what
    // was available. From the host: right for the cross, wrong for
    // `--target aarch64-macos` ON a Mac (`library not found for -lSystem`).
    // From `MCPP_TARGET`: right for the cross, wrong for the native build,
    // because it is never empty (`undefined symbol: wcslen`, `strtoul`, … —
    // the package's three-name stub had shadowed the vendor's complete one).
    //
    // An older mcpp sets neither, and that is the correct answer for it:
    // it has no graph-supplied target side, so the system is always on the
    // link and a package should supply nothing.
    e.emplace_back("MCPP_TARGET_REQUESTED", env.targetTriple);
    // Convenience splits of the resolved target (Cargo CARGO_CFG_TARGET_*
    // parity): parsed ONCE here through the canonical triple parser so every
    // build.mcpp stops hand-splitting MCPP_TARGET. MCPP_TARGET_ENV is "" when
    // the triple has no env segment (macOS); all three are "" for an
    // escape-hatch triple outside the canonical vocabulary. They ride the
    // same env vector, so contract_hash folds them into the re-run key.
    {
        mcpp::toolchain::triple::Triple t{};
        if (env.targetTriple.empty()) t = mcpp::toolchain::triple::host_triple();
        else if (auto p = mcpp::toolchain::triple::parse(env.targetTriple)) t = *p;
        e.emplace_back("MCPP_TARGET_OS", t.os);
        e.emplace_back("MCPP_TARGET_ARCH", t.arch);
        e.emplace_back("MCPP_TARGET_ENV", t.env);
    }
    e.emplace_back("MCPP_HOST", hostT);
    // Always emitted, empty when they do not apply: a build program reads
    // these through `env_or`, which cannot tell "absent" from "empty", and an
    // absent variable would make the answer depend on whatever the parent
    // process happened to export.
    e.emplace_back("MCPP_TOOLCHAIN_DIR", env.toolchainDir);
    e.emplace_back("MCPP_TOOLCHAIN_SYSROOT", env.toolchainSysroot);
    e.emplace_back("MCPP_TOOLCHAIN_BINUTILS_DIR", env.toolchainBinutilsDir);
    e.emplace_back("MCPP_COMPILER", env.compilerId);
    e.emplace_back("MCPP_CXX_STDLIB", env.cxxStdlib);
    e.emplace_back("MCPP_TARGET_SYSROOT", env.targetSysroot);
    e.emplace_back("MCPP_TARGET_BUILTINS_LIB", env.targetBuiltinsLib);
    e.emplace_back("MCPP_TARGET_LIBC_PROFILE", env.targetLibcProfile);
    e.emplace_back("MCPP_TARGET_LIBC", env.targetLibc);
    e.emplace_back("MCPP_PROFILE", env.profile);
    e.emplace_back("MCPP_ACCEL", env.accel);
    {
        std::string joined;
        for (auto const& d : env.deviceSources) {
            if (!joined.empty()) joined += '\n';
            joined += d;
        }
        e.emplace_back("MCPP_DEVICE_SOURCES", joined);
    }
    e.emplace_back("MCPP_OUT_DIR", outDir.string());
    e.emplace_back("MCPP_MANIFEST_DIR", root.string());
    std::string csv;
    for (auto const& f : env.features) {
        if (!csv.empty()) csv += ',';
        csv += f;
        e.emplace_back("MCPP_FEATURE_" + sanitize_feature_env(f), "1");
    }
    e.emplace_back("MCPP_FEATURES", csv);
    // mcpp#241: per-dependency payload dir, under MCPP_DEP_<SANITIZED_NAME>_DIR
    // (same sanitizer as MCPP_FEATURE_ — predictable from the manifest, not
    // store internals). Two distinct dep names can sanitize to the same var
    // (e.g. `foo.bar` vs `foo-bar`, or a bare `zlib` vs another dep's short
    // `zlib`); guard so a silent last-wins can't hand one dep another's dir —
    // keep the first and warn on a conflicting value.
    std::map<std::string, std::string> depVarValue;
    for (auto const& [name, dir] : env.depDirs) {
        auto var = "MCPP_DEP_" + sanitize_feature_env(name) + "_DIR";
        auto [it, inserted] = depVarValue.try_emplace(var, dir.string());
        if (inserted) {
            e.emplace_back(var, dir.string());
        } else if (it->second != dir.string()) {
            mcpp::ui::warning(std::format(
                "build.mcpp: dependency name collides on {} (kept '{}', ignored "
                "'{}') — rename one dependency to disambiguate", var,
                it->second, dir.string()));
        }
    }
    // The xlings side of the same question. Each declared entry is emitted
    // under BOTH its namespaced and its bare spelling, because a manifest may
    // write either and the build.mcpp asking should not have to know which
    // one the author chose. Namespaced entries are emitted first, so a bare
    // name that two namespaces claim resolves to the first DECLARED one rather
    // than to whichever was seen last.
    for (auto const& [var, dir] : env.xpkgDirs) {
        auto [it, inserted] = depVarValue.try_emplace(var, dir);
        if (inserted) e.emplace_back(var, dir);
    }
    // #355: MCPP_DEP_<PKG>_BIN_<TOOL> — absolute path to a host tool the
    // consumer declared via `tools = [...]`. A PATH rather than a directory:
    // the store keys an entry per (package, target), the typed reader can
    // append the platform's exe suffix itself, and a tool's adjacent DATA
    // (protoc's well-known .proto files, say) lives in the package tree, which
    // dep_dir() already exposes.
    // ── The child's PATH, with the project's own SubOS in front ────────────
    //
    // See `BuildProgramEnv::toolsBin` for why this exists, why it is a prefix
    // rather than a replacement, and why the decision is not made here.
    if (!env.toolsBin.empty()) {
        std::string path = env.toolsBin;
        // THE INHERITED VALUE IS READ HERE AND NOT ASSUMED. `extraEnv`
        // replaces a variable outright in the child, so writing only the
        // project's own directory would silently be the substitution this
        // deliberately is not.
        if (const char* inherited = std::getenv("PATH"); inherited && *inherited) {
            path += mcpp::platform::env::path_list_separator();
            path += inherited;
        }
        e.emplace_back("PATH", path);
    }

    for (auto const& [var, path] : env.toolPaths) {
        auto [it, inserted] = depVarValue.try_emplace(var, path);
        if (inserted) {
            e.emplace_back(var, path);
        } else if (it->second != path) {
            mcpp::ui::warning(std::format(
                "build.mcpp: tool name collides on {} (kept '{}', ignored '{}')",
                var, it->second, path));
        }
    }
    return e;
}

// The contract values are part of the re-run key UNCONDITIONALLY — a target /
// profile / feature change must re-run the program; that correctness cannot
// depend on the author remembering rerun-if-env-changed.
std::string contract_hash(const std::vector<std::pair<std::string, std::string>>& e) {
    std::string s;
    for (auto const& [k, v] : e) { s += k; s += '='; s += v; s += '\n'; }
    return mcpp::toolchain::hash_string(s);
}

// The project-relative name of the build output tree ("target" by default), so
// a glob input never walks into what a previous run produced. Empty when the
// output lives outside the project, in which case nothing needs excluding.
std::string output_dir_name(const fs::path& root, const fs::path& bdir) {
    std::error_code ec;
    auto rel = bdir.lexically_relative(root);
    if (rel.empty()) return {};
    auto first = rel.begin();
    if (first == rel.end()) return {};
    auto name = first->string();
    if (name == "..") return {};   // outside the project
    return name;
}

void write_cache(const fs::path& bdir, const fs::path& root,
                 const std::string& programHash,
                 const std::string& compilerHash, const std::string& ctxHash,
                 const Directives& d) {
    std::ofstream os(cache_path(bdir), std::ios::trunc);
    if (!os) return;  // best-effort: a failed cache write only loses the optimization
    // The epoch guards against a semantics change: the `d` lines below are
    // replayed verbatim on a hit, so if this mcpp interprets a directive
    // differently than the one that wrote them, the entry must not be reused.
    os << "epoch " << dirs::kCacheEpoch << '\n';
    os << "program " << programHash << '\n';
    os << "compiler " << compilerHash << '\n';
    os << "ctx " << ctxHash << '\n';
    // The root the relative entries below are resolved against. Recorded so a
    // reader that is not prepare_build — the fast-path glob check — can
    // evaluate a DEPENDENCY's cache against the dependency's own tree.
    os << "root " << root.string() << '\n';
    for (auto const& f : d.at(Slot::RerunFiles))
        os << "in " << mcpp::toolchain::hash_file(abs_against_root(root, f)) << ' ' << f << '\n';
    for (auto const& e : d.at(Slot::RerunEnv))
        os << "env " << mcpp::toolchain::hash_string(env_value(e)) << ' ' << e << '\n';
    // #359: a glob input's fingerprint is the SET of matching paths. Same
    // record shape as `in`/`env`; the value is computed by the table's owner.
    {
        auto outName = output_dir_name(root, bdir);
        for (auto const& g : d.at(Slot::RerunGlobs))
            os << "glob " << dirs::glob_fingerprint(root, g, outName) << ' '
               << g << '\n';
    }
    dirs::serialize(os, d);
}

struct CacheRecord {
    int         epoch = 0;   // 0 = pre-epoch entry (written before this guard existed)
    std::string programHash;
    std::string compilerHash;
    std::string ctxHash;   // contract env (target/profile/features/out-dir)
    std::string rootPath;  // what relative entries are resolved against
    std::vector<std::pair<std::string, std::string>> inputs;  // (hash, path)
    std::vector<std::pair<std::string, std::string>> envs;    // (hash, name)
    std::vector<std::pair<std::string, std::string>> globs;   // (hash, pattern)
    Directives directives;
    // A `d` record whose tag this mcpp does not know — the entry was written
    // by a newer mcpp. Replaying the rest would apply a strict subset of what
    // the program asked for, so the whole entry is discarded instead.
    bool unknownRecord = false;
    bool loaded = false;
};

CacheRecord read_cache(const fs::path& bdir) {
    CacheRecord r;
    std::ifstream is(cache_path(bdir));
    if (!is) return r;
    std::string line;
    while (std::getline(is, line)) {
        if (line.empty()) continue;
        auto sp = line.find(' ');
        if (sp == std::string::npos) continue;
        std::string tag = line.substr(0, sp);
        std::string rest = line.substr(sp + 1);
        if (tag == "epoch") {
            int n = 0;
            if (std::from_chars(rest.data(), rest.data() + rest.size(), n).ec == std::errc{})
                r.epoch = n;
        }
        else if (tag == "program") r.programHash = rest;
        else if (tag == "compiler") r.compilerHash = rest;
        else if (tag == "ctx") r.ctxHash = rest;
        else if (tag == "root") r.rootPath = rest;
        else if (tag == "in" || tag == "env" || tag == "glob") {
            auto sp2 = rest.find(' ');
            if (sp2 == std::string::npos) continue;
            std::string h = rest.substr(0, sp2), name = rest.substr(sp2 + 1);
            (tag == "in" ? r.inputs : tag == "env" ? r.envs : r.globs)
                .emplace_back(h, name);
        } else if (tag == "d") {
            auto sp2 = rest.find(' ');
            if (sp2 == std::string::npos) continue;
            std::string kind = rest.substr(0, sp2), val = rest.substr(sp2 + 1);
            if (!dirs::accept_cache_record(r.directives, kind, val))
                r.unknownRecord = true;
        }
    }
    r.loaded = true;
    return r;
}

// Decide whether the cached run is still valid (so we can skip recompiling/running).
bool cache_fresh(const fs::path& root, const fs::path& bdir, const CacheRecord& c,
                 const std::string& programHash, const std::string& compilerHash,
                 const std::string& ctxHash) {
    if (!c.loaded) return false;
    if (c.epoch != dirs::kCacheEpoch) return false;  // pre-epoch entries rerun once
    if (c.unknownRecord) return false;
    if (c.programHash != programHash) return false;
    if (c.compilerHash != compilerHash) return false;
    if (c.ctxHash != ctxHash) return false;   // pre-G3 caches (no ctx line) rerun once
    for (auto const& [h, path] : c.inputs)
        if (mcpp::toolchain::hash_file(abs_against_root(root, path)) != h) return false;
    for (auto const& [h, name] : c.envs)
        if (mcpp::toolchain::hash_string(env_value(name)) != h) return false;
    // #359: the path SET behind each declared glob. A file appearing or
    // disappearing changes it; editing one does not (that is what the `in`
    // entries above are for).
    if (!c.globs.empty()) {
        auto outName = output_dir_name(root, bdir);
        for (auto const& [h, pattern] : c.globs)
            if (dirs::glob_fingerprint(root, pattern, outName) != h) return false;
    }
    // A declared output that vanished invalidates the cache. Driven off the
    // table's mustExistAfterRun so a future output-shaped directive is covered
    // without editing this function.
    for (auto const& def : dirs::kTable) {
        if (!def.mustExistAfterRun) continue;
        for (auto const& p : c.directives.at(def.slot))
            if (!fs::exists(abs_against_root(root, p))) return false;
    }
    return true;
}

} // namespace

std::expected<void, std::string> run_build_program(
    mcpp::manifest::Manifest& m,
    const fs::path& root,
    const fs::path& hostCompiler,
    const mcpp::toolchain::Toolchain& tc,
    const mcpp::manifest::CppStandardConfig& cppStandard,
    const BuildProgramEnv& env) {

    fs::path src = root / "build.mcpp";
    std::error_code ec;
    if (!fs::exists(src, ec)) return {};  // no build program — nothing to do

    fs::path bdir = build_dir(root, env);
    fs::path outDir = bdir / "out";
    auto childEnv = contract_env(root, outDir, env);
    std::string ctxHash = contract_hash(childEnv);

    // ── Helper self-containment (the single decision point) ─────────────────
    // The compiled helper is exec'd by the host OS, outside anything mcpp
    // controls — no wrapper, no injected LD_LIBRARY_PATH, no PATH guarantee.
    // Making it runnable is a per-platform mechanism, and only two of the four
    // need a static link:
    //   Linux + glibc payload — host_base_flags bakes absolute payload paths
    //     into -Wl,-rpath; that already closes it, so leave it dynamic.
    //   Linux + musl (#295)   — rpath cannot reach PT_INTERP, an absolute
    //     /lib/ld-musl-<arch>.so.1 that no payload installs and glibc distros
    //     do not ship. Only a static link removes the interpreter entirely.
    //   Windows PE (#299)     — PE has no rpath, so a dynamic helper resolves
    //     libstdc++-6 / libgcc_s / libwinpthread through the process PATH and
    //     dies with STATUS_DLL_NOT_FOUND unless the user put the toolchain bin
    //     there by hand. A static link is the only PATH-independent answer.
    //   macOS                 — the system libc++ is always present.
    // Keep this the ONLY place that decides; the flag is appended to the final
    // link argv further down, never to `base`.
    const bool muslStaticHelper  = mcpp::platform::supports_full_static
                                && mcpp::toolchain::is_musl_target(tc);
    const bool mingwStaticHelper = mcpp::toolchain::is_mingw_target(tc);
    const bool staticHostHelper  = muslStaticHelper || mingwStaticHelper;

    // Fold the policy into the compiler identity: a helper produced under an
    // older link policy must be rebuilt, not reused from the cache.
    std::string compilerIdentity = hostCompiler.string();
    // Host modules change what the helper links, so they belong in the identity
    // the cache keys on — otherwise adding or removing a rule package would
    // replay a cached run compiled without it.
    for (auto const& hm : env.hostModules) {
        compilerIdentity += "\nhost-module=";
        compilerIdentity += hm.logical;
        compilerIdentity += "@";
        compilerIdentity += mcpp::toolchain::hash_file(hm.interface);
    }
    compilerIdentity += "\nbuild-program-link=";
    compilerIdentity += muslStaticHelper  ? "musl-static-v1"
                      : mingwStaticHelper ? "mingw-static-v1"
                                          : "default-v2";   // v2: DT_RPATH on Linux
    std::string programHash  = mcpp::toolchain::hash_file(src);
    std::string compilerHash = mcpp::toolchain::hash_string(compilerIdentity);

    // Read once, here, because the check below has to run BEFORE the cache
    // fast path returns.
    std::string srcText;
    { std::ifstream is(src); std::ostringstream ss; ss << is.rdbuf(); srcText = ss.str(); }

    // A module that is present only as another rule's prerequisite is not part
    // of this package's declared surface. Refusing the import rather than
    // letting it work is the difference between a rule that travels and one
    // that happens to build on whoever's machine: the same source stops
    // compiling the moment the intermediate rule stops depending on it, or the
    // moment a Clang user tries it, since only GCC makes the BMI reachable
    // without the flags.
    //
    // AHEAD OF THE FAST PATH ON PURPOSE. Withdrawing `reexport = true` from
    // an edge leaves the module SET and every interface hash identical, so
    // nothing in `compilerIdentity` moves. Measured: the replay is prevented
    // today only because `ctxHash` happens to change too — an incidental
    // coupling, and the shape this codebase keeps paying for. Asking the
    // question before the cache is consulted needs no key at all.
    for (auto const& hm : env.hostModules) {
        if (hm.importable) continue;
        if (!imports_module(srcText, hm.logical)) continue;
        return std::unexpected(std::format(
            "build.mcpp imports '{}', which reaches this build only as a "
            "prerequisite of another build rule.\n"
            "       A rule's build-time provisions cross one further edge only "
            "when that edge says so.\n"
            "       Either depend on the package providing '{}' directly, or "
            "have the rule that owns it re-export it:\n"
            "         <that rule's package> = {{ ..., host-module = true, "
            "reexport = true }}",
            hm.logical, hm.logical));
    }

    // Fast path: declared inputs + contract unchanged → reapply cached
    // directives, no run.
    CacheRecord cache = read_cache(bdir);
    if (cache_fresh(root, bdir, cache, programHash, compilerHash, ctxHash)) {
        dirs::apply(m, cache.directives);
        // ONE OF TWO SITES, AND THE ONE THAT IS EASY TO FORGET.
        //
        // A cache hit does not re-run the program, so an advisory emitted only
        // on the run path below would appear on the first build of a project
        // and never again — which reads as "the condition went away". The
        // wording comes from dirs::advisories so the two sites cannot drift;
        // what they must not drift on is WHETHER they emit, and e2e 139
        // asserts the second build, not the first.
        for (auto const& a : dirs::advisories(m.package.name, cache.directives))
            mcpp::ui::warning(a);
        mcpp::ui::info("build.mcpp", "up to date (cached)");
        return {};
    }

    fs::create_directories(outDir, ec);   // creates bdir too
    // #230: on Windows the capture_exec shell is cmd.exe, which can only launch
    // a PE by an executable extension — a bare `.bin` is not in PATHEXT and
    // fails to run. Name the compiled program `.exe` there; other platforms keep
    // `.bin` (cosmetic — bdir is separate from the `.mcpp` source). Surfaces
    // once a workspace build.mcpp member is reached on Windows, after the
    // scanner symlink-escape crash fix (df985df) stops masking it as exit 127.
    fs::path bin = bdir / (mcpp::platform::is_windows
                               ? "build.mcpp.exe" : "build.mcpp.bin");

    // ── Compile build.mcpp with the host toolchain ──────────────────────────
    // Spelled by the dialect layer, not concatenated here: the canonical of
    // `standard = "c++fly"` / `"c++latest"` is not a valid -std= spelling, so
    // the old `"-std=" + canonical` produced `-std=c++fly` and the host compile
    // died on an unknown dialect. cppfly::std_flag resolves those against the
    // toolchain that will actually run the compile — the host one, here.
    std::string std_flag = mcpp::toolchain::cppfly::std_flag(
        tc, cppStandard.canonical.empty() ? std::string_view("c++23")
                                          : std::string_view(cppStandard.canonical),
        cppStandard.level);
    // One resolution of the deployment target, used by every compile below
    // and by the std module it asks stdmod to build — they must agree or
    // clang rejects the BMI.
    const std::string macosDeploymentTarget =
        mcpp::platform::macos::deployment_target(
            m.buildConfig.macosDeploymentTarget);
    auto base = host_base_flags(tc, macosDeploymentTarget);

    // The host compile has always been spelled in GNU driver syntax with no
    // dialect branch at all — `grep -i msvc` over this file used to hit only
    // comments. Under cl.exe every one of `-O0` / `-x c++` / `-static` / `-o`
    // is wrong, so the whole build.mcpp path was unusable on a native MSVC
    // toolchain regardless of what else was fixed.
    const auto& dial = mcpp::toolchain::dialect_for(tc);
    const bool msvcHost = dial.id == std::string_view("msvc");

    // Only wire the bundled `mcpp` module when build.mcpp actually imports it —
    // so the common `#include`-based program compiles exactly as before (no
    // -fmodules, cwd = project root). When it does `import mcpp;`, compile the
    // module, link its object, and run the build.mcpp compile from `bdir` so GCC
    // finds gcm.cache/mcpp.gcm.
    // `srcText` was read above the cache fast path, which the prerequisite
    // check needs to run ahead of.
    bool usesModule    = srcText.find("import mcpp") != std::string::npos;
    bool usesStdCompat = imports_module(srcText, "std.compat");
    bool usesStd       = usesStdCompat || imports_module(srcText, "std");

    // A rule package's interface is compiled by this same function, so what IT
    // imports decides what has to be built just as much as what build.mcpp
    // imports. Scanning only build.mcpp made a rule that said `import std;`
    // fail with `module 'std' not found` — the std module was never built,
    // because the program that triggers the build did not mention it.
    for (auto const& hm : env.hostModules) {
        std::ifstream is(hm.interface);
        if (!is) continue;  // a missing interface is diagnosed by build_host_module
        std::ostringstream ss; ss << is.rdbuf();
        const std::string t = ss.str();
        if (t.find("import mcpp") != std::string::npos) usesModule = true;
        if (imports_module(t, "std.compat")) usesStdCompat = true;
        if (imports_module(t, "std"))        usesStd       = true;
    }

    usesStd = usesStd || usesStdCompat;

    // The toolchain's own environment (MSVC's INCLUDE / LIB / VSLANG, which
    // detection synthesized from the located VC tools + Windows SDK). Needed
    // by every compile below, the module precompile included.
    std::vector<std::pair<std::string, std::string>> compileEnv;
    for (auto const& ev : tc.envOverrides)
        compileEnv.emplace_back(ev.key, ev.value);

    // Named modules dispatch on the same BmiTraits/CommandDialect rows the
    // main build uses, so there is no per-family gate here: cl.exe's
    // .ifc + /reference works because the table already describes it, not
    // because build.mcpp grew a second implementation of it.
    std::vector<std::string> moduleFlags;
    fs::path mcppModuleObject;
    if (usesModule) {
        auto mf = build_mcpp_module(bdir, hostCompiler, base, std_flag, tc,
                                    compileEnv);
        if (!mf) return std::unexpected(mf.error());
        moduleFlags = std::move(mf->useFlags);
        mcppModuleObject = std::move(mf->object);
    }

    // ── `import std;` in build.mcpp ─────────────────────────────────────────
    //
    // mcpp asks projects to `import std;` everywhere and then made their build
    // script fall back to `#include` — the bundled `mcpp` module even says so
    // in its own header comment. The std module the main build already uses is
    // reusable verbatim: stdmod::ensure_built caches on
    // (toolchain × standard × dialect), so for a native build this is a cache
    // HIT on the very artifact the project's own TUs import. Only a cross
    // build pays for a second one, which is unavoidable — see below.
    //
    // `tc` here is the HOST toolchain: prepare.cppm's
    // host_tc_for_build_program() resolves the spec WITHOUT the --target axis
    // and hands it in. That is load-bearing. build.mcpp is compiled AND run on
    // the machine doing the build, so a std BMI built for the target would
    // produce a helper that cannot execute — the same host≠target mistake the
    // mingw-cross work had to fix in four separate places.
    std::vector<std::string> stdFlags;
    std::vector<std::string> stdObjects;
    // GCC finds staged BMIs by cwd; Clang/MSVC get an explicit path flag.
    bool stdStagedInBdir = false;
    if (usesStd) {
        if (!tc.hasImportStd) {
            return std::unexpected(std::format(
                "build.mcpp uses `import std;` but the host toolchain ({}) "
                "ships no std module.\n"
                "       Use #include in build.mcpp, or switch to a toolchain "
                "that provides one.", tc.label()));
        }
        auto sm = mcpp::toolchain::ensure_built(
            tc, cppStandard.canonical, std_flag, macosDeploymentTarget);
        if (!sm) {
            // The second branch of the same refusal, and it is named for the
            // same reason: both `run_build_program` call sites in prepare.cppm
            // return `std::unexpected` unconditionally, so this error always
            // reaches the layer that reads the code. An unnamed one here would
            // reproduce, for the host std module, exactly the gap the target
            // std module had.
            refusal::record(refusal::Code::StdModulePrecompile);
            return std::unexpected(std::format(
                "build.mcpp uses `import std;` but the std module could not be "
                "built for the host toolchain: {}", sm.error().message));
        }

        auto traits = mcpp::toolchain::bmi_traits(tc);
        if (traits.stdBmiUsePrefix.empty()) {
            // GCC: BMIs are found implicitly under <cwd>/gcm.cache, so stage
            // the cached ones where the compile will look. Copy rather than
            // symlink — this mirrors the main build's staging edge, and a
            // stale copy is caught by ensure_built's own cache key.
            std::error_code ec;
            fs::path gcmDir = bdir / traits.bmiDir;
            fs::create_directories(gcmDir, ec);
            auto stage = [&](const fs::path& from, std::string_view name)
                -> std::expected<void, std::string> {
                if (from.empty() || !fs::exists(from)) return {};
                fs::path to = gcmDir / std::format("{}{}", name, traits.bmiExt);
                fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
                if (ec) return std::unexpected(std::format(
                    "staging {} for build.mcpp failed: {}", name, ec.message()));
                return {};
            };
            if (auto r = stage(sm->bmiPath, "std"); !r)
                return std::unexpected(r.error());
            if (usesStdCompat) {
                if (auto r = stage(sm->compatBmiPath, "std.compat"); !r)
                    return std::unexpected(r.error());
            }
            // -fmodules may already be present from the `mcpp` module path;
            // GCC tolerates the repeat, but keep the argv honest.
            if (!usesModule) stdFlags.push_back("-fmodules");
            stdStagedInBdir = true;
        } else {
            // Through bmi_reference_tokens, not string concatenation: the
            // traits spell these for the ninja STRING channel, where
            // `-fmodule-file=std=<p>` (one word) and `/reference std=<p>`
            // (two) are indistinguishable. Concatenating produced a single
            // argv element with a space inside it, and cl answered
            // "C2230: could not find module 'std'".
            for (auto& t : mcpp::toolchain::bmi_reference_tokens(
                     traits.stdBmiUsePrefix, sm->bmiPath))
                stdFlags.push_back(t);
            if (usesStdCompat && !sm->compatBmiPath.empty())
                for (auto& t : mcpp::toolchain::bmi_reference_tokens(
                         traits.stdCompatBmiUsePrefix, sm->compatBmiPath))
                    stdFlags.push_back(t);
        }
        if (!sm->objectPath.empty() && fs::exists(sm->objectPath))
            stdObjects.push_back(sm->objectPath.string());
        if (usesStdCompat && !sm->compatObjectPath.empty()
            && fs::exists(sm->compatObjectPath))
            stdObjects.push_back(sm->compatObjectPath.string());
    }

    // #355 step 5: dependency-provided host modules (reusable build rules
    // shipped as ordinary packages). Compiled HERE, with `base` and `std_flag`
    // — the same flags the build.mcpp compile below gets — because a BMI is
    // only usable by a compile that agrees with it. Doing this in a separate
    // sub-build would leave that agreement to chance, and disagreement shows
    // up as `module X CRC mismatch`, not as a clear error.
    //
    // AFTER the std block, and that ordering is load-bearing: a rule may
    // `import std;` just as build.mcpp may, and it can only do so once the std
    // BMI exists and `stdFlags` names it. Compiling rules first — which is what
    // 2026.8.5.1 did — handed them an empty `stdFlags` and failed with
    // `module 'std' not found`.
    std::vector<fs::path> hostModuleObjects;
    for (auto const& ref : env.hostModules) {
        std::vector<std::string> use = moduleFlags;
        use.insert(use.end(), stdFlags.begin(), stdFlags.end());
        auto hm = build_host_module(bdir, hostCompiler, base, std_flag, tc,
                                    compileEnv, ref.logical, ref.interface, use);
        if (!hm) return std::unexpected(hm.error());
        for (auto& f : hm->useFlags) {
            // GCC's marker is just `-fmodules`, already present when the
            // bundled module was built; repeating it is harmless but noisy.
            if (std::find(moduleFlags.begin(), moduleFlags.end(), f)
                == moduleFlags.end())
                moduleFlags.push_back(f);
        }
        hostModuleObjects.push_back(std::move(hm->object));
    }

    // `-x c++` is required: the `.mcpp` extension is unknown to the compiler, so
    // without it the driver hands build.mcpp to the linker as a linker script.
    std::vector<std::string> compileArgv = { hostCompiler.string() };
    if (msvcHost) {
        // /nologo /EHsc /utf-8 — cl.exe needs these to behave like the other
        // two drivers do by default (quiet, exceptions on, UTF-8 sources).
        for (auto f : dial.alwaysFlagsArgv) compileArgv.emplace_back(f);
    }
    compileArgv.push_back(std_flag);
    // No optimization: this program runs once per build and its compile time
    // is on the critical path. MSVC spells "off" /Od, not /O0.
    compileArgv.push_back(msvcHost ? std::string("/Od")
                                   : std::string(dial.optPrefix) + "0");
    for (auto& bf : base)        compileArgv.push_back(bf);
    for (auto& mf : moduleFlags) compileArgv.push_back(mf);
    for (auto& sf : stdFlags)    compileArgv.push_back(sf);
    // The `.mcpp` extension is unknown to every driver, so without this the
    // file is handed to the linker as a linker script.
    // Per-file where the driver has that form (cl's /Tp), positional
    // otherwise. Object files follow on this same command line, and cl's
    // global /TP would compile them as C++ source.
    if (!dial.perFileCxxPrefix.empty()) {
        compileArgv.push_back(std::string(dial.perFileCxxPrefix) + src.string());
    } else {
        for (auto f : dial.forceCxxLangArgv) compileArgv.emplace_back(f);
        compileArgv.push_back(src.string());
    }
    if (usesModule || !stdObjects.empty() || !hostModuleObjects.empty()) {
        // Link the module objects. GNU drivers need the input language reset
        // first, or the .o that follows `-x c++` is handed to the frontend as
        // C++ source; cl.exe has no `-x` at all and infers from the extension.
        // This used to be unconditional and was only harmless while MSVC could
        // not reach it — removing that gate made the dead branch live, and cl
        // answered with `D9002: ignoring unknown option '-x'`.
        if (!msvcHost) { compileArgv.push_back("-x"); compileArgv.push_back("none"); }
        if (usesModule) compileArgv.push_back(mcppModuleObject.string());
        for (auto& hmo : hostModuleObjects) compileArgv.push_back(hmo.string());
        for (auto& so : stdObjects) compileArgv.push_back(so);
    }
    // Self-contained helper link — see the staticHostHelper doctrine above.
    // Deliberately NOT in `base`: that also feeds the bundled module's
    // compile/precompile commands, where a link flag has no business (and for
    // Clang would perturb the default PIC/PIE codegen of mcpp.o).
    if (staticHostHelper) compileArgv.push_back(std::string(dial.staticRuntime));
    // A DYNAMIC HELPER ON LINUX GETS `DT_RPATH`, NOT `DT_RUNPATH`.
    //
    // The driver's default is the new tag, and a runpath is consulted only for
    // the helper's OWN needed libraries. A build program that opens a host
    // library at run time -- a rule package reading a driver's version through
    // the driver itself -- then fails one hop later, because that library's
    // own dependencies (`libdl.so.2`, `libpthread.so.0`) are looked up without
    // the helper's search path and the payload loader has no default that
    // reaches them. Measured: `dlopen("<sentinel>/lib/libcuda.so.1")` from a
    // build.mcpp answered `libdl.so.2: cannot open shared object file` while
    // the very same directories sat in the helper's RUNPATH. The artifacts
    // mcpp links carry DT_RPATH for this reason (loader_contract's Rpath tag);
    // the helper now does too. Driver-only spelling: the helper is always
    // linked through the compiler driver, never through the linker directly.
    if (!staticHostHelper && !msvcHost
        && !mcpp::platform::is_windows && !mcpp::platform::is_macos)
        compileArgv.push_back("-Wl,--disable-new-dtags");
    if (msvcHost) {
        // /Fe: takes its value attached, not as a separate argv token.
        compileArgv.push_back(std::string(dial.outputExePrefix) + bin.string());
    } else {
        compileArgv.push_back("-o"); compileArgv.push_back(bin.string());
    }
    mcpp::ui::info("build.mcpp", "compiling");
    // GCC resolves imported BMIs via gcm.cache/ relative to the compile cwd, so
    // any compile that imports a module — `mcpp`, `std`, or both — has to run
    // from bdir, where they were staged. One condition, not two: a build.mcpp
    // that imports only std needs exactly the same cwd as one that imports
    // only mcpp. Otherwise the project root is fine.
    const bool needsBmiCwd = usesModule || stdStagedInBdir;
    std::string compileCwd = needsBmiCwd ? bdir.string() : root.string();
    auto cres = mcpp::platform::process::capture_exec(compileArgv, compileEnv,
                                                     compileCwd);
    if (cres.exit_code != 0) {
        std::string msg = std::format("build.mcpp failed to compile (exit {}):\n{}",
                                      cres.exit_code, cres.output);
        if (mentions_missing_mcpp_api(cres.output)) {
            msg += std::format(
                "\n       The `mcpp` build module this engine bundles does not have "
                "that name.\n"
                "       Either the package was written for a newer mcpp (try "
                "`mcpp self update`;\n"
                "       this is mcpp {}), or the name is misspelled — the compiler "
                "cannot tell\n"
                "       the two apart, because the module is generated by whichever "
                "mcpp is running.",
                mcpp::MCPP_VERSION);
        }
        return std::unexpected(std::move(msg));
    }

    // ── Run it; capture stdout(+stderr) and parse directives ────────────────
    // Run with cwd = package root so the program's relative file writes (e.g.
    // mcpp:generated sources) land in the project, not in mcpp's invocation
    // dir. The MCPP_* contract env is injected into the CHILD only.
    //
    // Bounded, unlike the compile above. The asymmetry is deliberate and the
    // same one `mcpp test` settled on: a COMPILE that runs long is usually
    // legitimate (a first-run std module build is minutes), and killing it
    // produces a baffling failure; a build PROGRAM that runs long is usually
    // stuck — waiting on a network read or spinning — and without a bound the
    // whole build hangs with no diagnostic at all.
    mcpp::ui::info("build.mcpp", "running");
    bool timedOut = false;
    // The bound comes from THIS package's manifest — a dependency's generator
    // is bounded by the dependency's own declaration, because its author is
    // the one who knows how long it takes.
    const auto deadline =
        dirs::run_timeout_for(m.buildConfig.buildProgramTimeoutSecs);
    auto rres = mcpp::platform::process::capture_exec_deadline(
        {bin.string()}, childEnv, deadline, &timedOut, root.string());
    if (timedOut) {
        // Name the manifest to edit. Without this the user edits their OWN
        // mcpp.toml when a dependency's build program is what timed out, and
        // nothing changes — which is how issue #410 reads from the outside.
        auto ownManifest = (root / "mcpp.toml").string();
        return std::unexpected(std::format(
            "build.mcpp for '{}' exceeded its {}s time limit and was killed.\n"
            "       Raise it in that package's own manifest:\n"
            "         {}   ->  [build] build_program_timeout = <seconds>\n"
            "       Or for this invocation only:\n"
            "         MCPP_BUILD_PROGRAM_TIMEOUT=<seconds>   (0 = no limit)\n"
            "       Output so far:\n{}",
            m.package.name.empty() ? std::string("<unnamed package>")
                                   : m.package.name,
            deadline.count() / 1000, ownManifest, rres.output));
    }
    if (rres.exit_code != 0) {
        return std::unexpected(std::format(
            "build.mcpp exited with {} (build aborted):\n{}", rres.exit_code, rres.output));
    }

    Directives d;
    dirs::accept_output(d, dial, root, rres.output);

    // Protocol gate (S1). A program that announced a version this mcpp cannot
    // speak, or that emitted an unknown directive INSIDE a version both sides
    // speak, is a hard error — "warn and ignore" would turn a missing
    // directive into a silently different build. A program that announced
    // nothing is a hand-written printf program (the frozen surface) and keeps
    // the historical warn-and-ignore behaviour.
    if (auto perr = dirs::protocol_error(d)) {
        return std::unexpected(*perr);
    }
    // Refuse a malformed action BEFORE applying anything: a half-applied
    // action set is worse than none, and an action that silently does not
    // exist surfaces as a missing generated source three edges away.
    if (auto aerr = dirs::action_error(d); !aerr.empty()) {
        return std::unexpected(aerr);
    }
    if (d.protocol == 0) {
        for (auto const& k : d.unknownKeys)
            mcpp::ui::warning(std::format(
                "build.mcpp: ignoring unknown directive 'mcpp:{}'", k));
    }

    // Dependency mode (genBase set): relative `generated=` paths resolve
    // against OUT_DIR-style genBase, not the (possibly read-only, shared)
    // package root — rewrite them to absolute before validation/apply/cache.
    // `source=` paths are NOT rewritten: they name pre-existing files in the
    // package tree (MCPP_MANIFEST_DIR-relative), never OUT_DIR products.
    if (!env.genBase.empty()) {
        for (auto& g : d.at(Slot::Generated)) {
            fs::path gp(g);
            if (gp.is_relative()) g = (env.genBase / gp).lexically_normal().string();
        }
    }

    // Declared-output contract, driven off the table's mustExistAfterRun:
    //   generated= — the program said it wrote this file; if it did not, the
    //                build would fail far away as a glob no-match.
    //   source=    — the program SELECTED a pre-existing file (payload /
    //                vendored tree); a missing one is a typo or a broken
    //                payload, surfaced now.
    for (auto const& def : dirs::kTable) {
        if (!def.mustExistAfterRun) continue;
        for (auto const& p : d.at(def.slot)) {
            if (fs::exists(abs_against_root(root, p))) continue;
            return std::unexpected(std::format("build.mcpp {} '{}' {}",
                                               def.missingPrefix, p, def.missingSuffix));
        }
    }

    dirs::apply(m, d);
    // The second of the two sites. See the note on the cache-hit path above.
    for (auto const& a : dirs::advisories(m.package.name, d))
        mcpp::ui::warning(a);
    write_cache(bdir, root, programHash, compilerHash, ctxHash, d);
    return {};
}

bool program_inputs_stale(const fs::path& projectRoot) {
    std::error_code ec;
    const fs::path base = projectRoot / "target" / ".build-mcpp";
    if (!fs::exists(base, ec)) return false;

    // Depth-bounded: the root's cache sits at depth 0 and a dependency's at
    // `deps/<pkg>/` (depth 2). Bounding it keeps this off the generated-output
    // tree under `out/`, which can hold thousands of files and never holds a
    // cache.
    fs::recursive_directory_iterator it(
        base, fs::directory_options::skip_permission_denied, ec);
    if (ec) return false;
    for (; it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (it.depth() >= 3) { it.disable_recursion_pending(); continue; }
        if (it->path().filename() != "build.mcpp.cache") continue;
        auto rec = read_cache(it->path().parent_path());
        if (!rec.loaded || rec.rootPath.empty()) continue;
        fs::path recRoot{rec.rootPath};
        auto outName = output_dir_name(recRoot, it->path().parent_path());
        for (auto const& [h, pattern] : rec.globs)
            if (dirs::glob_fingerprint(recRoot, pattern, outName) != h) return true;
        // The same comparison `cache_fresh` makes when prepare_build runs. It
        // is repeated here rather than shared because the fast path has no
        // manifest, no toolchain and no context hash -- only the recorded
        // entries and the tree they were measured against.
        for (auto const& [h, rel] : rec.inputs)
            if (mcpp::toolchain::hash_file(abs_against_root(recRoot, rel)) != h) return true;
        for (auto const& [h, name] : rec.envs)
            if (mcpp::toolchain::hash_string(env_value(name)) != h) return true;
    }
    return false;
}

} // namespace mcpp::build
