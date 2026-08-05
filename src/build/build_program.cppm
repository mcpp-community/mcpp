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
import mcpp.build.hostprogram;       // bundled `mcpp` module compile (own module: see its header)
import mcpp.toolchain.hostflags;     // the shared host-compile flag producer
import mcpp.toolchain.linkmodel;     // shared C-library / clang-cfg-bypass model
import mcpp.toolchain.model;         // Toolchain, PayloadPaths, is_clang/is_musl_target/is_mingw_target
import mcpp.toolchain.registry;      // archive_tool
import mcpp.toolchain.stdmod;        // ensure_built — the SAME std BMI the main build uses
import mcpp.toolchain.triple;        // host_triple (MCPP_HOST contract value)
import mcpp.ui;

export namespace mcpp::build {

// Build-program environment contract (G3) — what the running build.mcpp can
// see, mirroring Cargo's env family. Injected as MCPP_* variables into the
// child ONLY (never the calling process), and folded into the cache key so a
// target/profile/feature change re-runs the program.
struct BuildProgramEnv {
    std::string targetTriple;               // resolved canonical triple; "" = host
    std::string profile;                    // effective profile name (dev/release/…)
    std::vector<std::string> features;      // active feature closure of the package
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
    // #355: HOST tools this package asked its dependencies for, as
    // (env var name → absolute path to the executable) pairs. The caller has
    // already resolved them (built, taken from the store, or an override), so
    // this is purely the delivery channel. Rides the same contract env, hence
    // the same re-run key: a rebuilt tool re-runs the program that uses it,
    // with no `rerun-if-changed` needed from the author.
    std::vector<std::pair<std::string, std::string>> toolPaths;
    // #355 step 5: dependency-provided modules to compile FOR THE HOST and make
    // importable from this build.mcpp — reusable build rules distributed as
    // ordinary mcpp packages (`import mcpp.rules.protobuf;`) instead of a
    // second, non-C++ rule DSL.
    //
    // (logical module name, absolute path to its interface unit). Compiled with
    // the SAME flags as build.mcpp itself, in the same directory, which is what
    // makes the BMI usable at all — see DependencySpec::hostModule.
    std::vector<std::pair<std::string, std::filesystem::path>> hostModules;
};

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

} // namespace mcpp::build

namespace mcpp::build {

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
    e.emplace_back("MCPP_PROFILE", env.profile);
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
    // #355: MCPP_DEP_<PKG>_BIN_<TOOL> — absolute path to a host tool the
    // consumer declared via `tools = [...]`. A PATH rather than a directory:
    // the store keys an entry per (package, target), the typed reader can
    // append the platform's exe suffix itself, and a tool's adjacent DATA
    // (protoc's well-known .proto files, say) lives in the package tree, which
    // dep_dir() already exposes.
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
    for (auto const& f : d.at(Slot::RerunFiles))
        os << "in " << mcpp::toolchain::hash_file(abs_against_root(root, f)) << ' ' << f << '\n';
    for (auto const& e : d.at(Slot::RerunEnv))
        os << "env " << mcpp::toolchain::hash_string(env_value(e)) << ' ' << e << '\n';
    dirs::serialize(os, d);
}

struct CacheRecord {
    int         epoch = 0;   // 0 = pre-epoch entry (written before this guard existed)
    std::string programHash;
    std::string compilerHash;
    std::string ctxHash;   // contract env (target/profile/features/out-dir)
    std::vector<std::pair<std::string, std::string>> inputs;  // (hash, path)
    std::vector<std::pair<std::string, std::string>> envs;    // (hash, name)
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
        else if (tag == "in" || tag == "env") {
            auto sp2 = rest.find(' ');
            if (sp2 == std::string::npos) continue;
            std::string h = rest.substr(0, sp2), name = rest.substr(sp2 + 1);
            (tag == "in" ? r.inputs : r.envs).emplace_back(h, name);
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
bool cache_fresh(const fs::path& root, const CacheRecord& c,
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
    for (auto const& [logical, ifacePath] : env.hostModules) {
        compilerIdentity += "\nhost-module=";
        compilerIdentity += logical;
        compilerIdentity += "@";
        compilerIdentity += mcpp::toolchain::hash_file(ifacePath);
    }
    compilerIdentity += "\nbuild-program-link=";
    compilerIdentity += muslStaticHelper  ? "musl-static-v1"
                      : mingwStaticHelper ? "mingw-static-v1"
                                          : "default-v1";
    std::string programHash  = mcpp::toolchain::hash_file(src);
    std::string compilerHash = mcpp::toolchain::hash_string(compilerIdentity);

    // Fast path: declared inputs + contract unchanged → reapply cached
    // directives, no run.
    CacheRecord cache = read_cache(bdir);
    if (cache_fresh(root, cache, programHash, compilerHash, ctxHash)) {
        dirs::apply(m, cache.directives);
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
    std::string srcText;
    { std::ifstream is(src); std::ostringstream ss; ss << is.rdbuf(); srcText = ss.str(); }
    bool usesModule    = srcText.find("import mcpp") != std::string::npos;
    bool usesStdCompat = imports_module(srcText, "std.compat");
    bool usesStd       = usesStdCompat || imports_module(srcText, "std");

    // A rule package's interface is compiled by this same function, so what IT
    // imports decides what has to be built just as much as what build.mcpp
    // imports. Scanning only build.mcpp made a rule that said `import std;`
    // fail with `module 'std' not found` — the std module was never built,
    // because the program that triggers the build did not mention it.
    for (auto const& [logical, ifacePath] : env.hostModules) {
        std::ifstream is(ifacePath);
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
    for (auto const& [logical, ifacePath] : env.hostModules) {
        std::vector<std::string> use = moduleFlags;
        use.insert(use.end(), stdFlags.begin(), stdFlags.end());
        auto hm = build_host_module(bdir, hostCompiler, base, std_flag, tc,
                                    compileEnv, logical, ifacePath, use);
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
        return std::unexpected(std::format(
            "build.mcpp failed to compile (exit {}):\n{}", cres.exit_code, cres.output));
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
    auto rres = mcpp::platform::process::capture_exec_deadline(
        {bin.string()}, childEnv, dirs::run_timeout(), &timedOut, root.string());
    if (timedOut) {
        return std::unexpected(std::format(
            "build.mcpp for '{}' exceeded its {}s time limit and was killed.\n"
            "       Raise or disable it with MCPP_BUILD_PROGRAM_TIMEOUT=<seconds> "
            "(0 = no limit).\n"
            "       Output so far:\n{}",
            m.package.name.empty() ? std::string("<unnamed package>")
                                   : m.package.name,
            dirs::run_timeout().count() / 1000, rres.output));
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
    write_cache(bdir, root, programHash, compilerHash, ctxHash, d);
    return {};
}

} // namespace mcpp::build
