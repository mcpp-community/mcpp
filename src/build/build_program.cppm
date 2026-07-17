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
import mcpp.toolchain.fingerprint;   // hash_file / hash_string (FNV-1a, 16 hex)
import mcpp.toolchain.linkmodel;     // shared C-library / clang-cfg-bypass model
import mcpp.toolchain.model;         // Toolchain, PayloadPaths, is_clang/is_musl_target
import mcpp.toolchain.registry;      // archive_tool
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
    std::string_view cppStandard,
    const BuildProgramEnv& env);

} // namespace mcpp::build

namespace mcpp::build {

namespace {

namespace fs = std::filesystem;

// Parsed directives in apply order. Stored verbatim in the cache so a cache hit
// reapplies the exact same edits without re-running the program.
struct Directives {
    std::vector<std::string> cxxflags;      // -> buildConfig.cxxflags
    std::vector<std::string> cflags;        // -> buildConfig.cflags
    std::vector<std::string> ldflags;       // -> buildConfig.ldflags (already -l/-L)
    std::vector<std::string> defines;       // cfg=  -> -D, into BOTH c/cxx flags
    std::vector<std::string> generated;     // relative source paths
    std::vector<std::string> rerunFiles;    // declared file inputs
    std::vector<std::string> rerunEnv;      // declared env-var inputs
};

std::string trim(std::string_view s) {
    std::size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) --e;
    return std::string(s.substr(b, e - b));
}

// Resolve a possibly-relative path against the project root, returning an
// absolute lexically-normal path (no filesystem touch, so it works for dirs that
// the program is about to create as well as existing ones).
std::string abs_against_root(const fs::path& root, std::string_view p) {
    fs::path pp(p);
    if (pp.is_relative()) pp = root / pp;
    return pp.lexically_normal().string();
}

// Parse one stdout line. Returns true if it was a recognized (or unknown-but-
// `mcpp:`) directive; false for ordinary program chatter.
bool parse_line(const fs::path& root, std::string_view raw, Directives& d) {
    std::string line = trim(raw);
    constexpr std::string_view kPfx = "mcpp:";
    if (!line.starts_with(kPfx)) return false;
    std::string_view body = std::string_view(line).substr(kPfx.size());
    auto eq = body.find('=');
    std::string key = std::string(body.substr(0, eq));
    std::string val = eq == std::string_view::npos ? std::string() : std::string(body.substr(eq + 1));

    if (key == "cxxflag")            d.cxxflags.push_back(val);
    else if (key == "cflag")         d.cflags.push_back(val);
    else if (key == "link-lib")      d.ldflags.push_back("-l" + val);
    else if (key == "link-search")   d.ldflags.push_back("-L" + abs_against_root(root, val));
    else if (key == "cfg")           d.defines.push_back("-D" + val);
    else if (key == "generated")     d.generated.push_back(val);
    else if (key == "rerun-if-changed")     d.rerunFiles.push_back(val);
    else if (key == "rerun-if-env-changed") d.rerunEnv.push_back(val);
    else mcpp::ui::warning(std::format("build.mcpp: ignoring unknown directive 'mcpp:{}'", key));
    return true;
}

void parse_output(const fs::path& root, std::string_view out, Directives& d) {
    std::size_t pos = 0;
    while (pos <= out.size()) {
        std::size_t nl = out.find('\n', pos);
        std::string_view ln = out.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
        parse_line(root, ln, d);
        if (nl == std::string_view::npos) break;
        pos = nl + 1;
    }
}

std::string env_value(const std::string& name) {
    const char* v = std::getenv(name.c_str());
    return v ? std::string(v) : std::string();
}

// The host subset of flags.cppm's sysroot/runtime handling — enough to compile +
// link a freestanding host program on a fresh sandbox (where bare `g++ file -o x`
// can't find crt/libc). build.mcpp is host-only (skipped under cross), so we need
// only the native cases; these are passed as separate argv tokens (no shell).
std::vector<std::string> host_base_flags(const mcpp::toolchain::Toolchain& tc) {
    std::vector<std::string> f;
    const auto lm = mcpp::toolchain::resolve_link_model(tc);

    // Clang with a bundled cfg on LINUX: bypass it (--no-default-config) and
    // provide everything explicitly, same as the main build — the cfg is an
    // install-time-generated artifact, so trusting it here while bypassing
    // it in the main build meant two different toolchains for one project.
    // On macOS/Windows keep trusting the cfg: the macOS link additionally
    // needs the platform's libc++abi/unwind handling that the main build's
    // needs_explicit_libcxx path owns (duplicating it for a host compile
    // produced undefined __cxa_*/__gxx_personality_v0), and the fixup
    // pipeline regenerates the cfg deterministically anyway.
    if (mcpp::toolchain::is_clang(tc)) {
        if constexpr (!mcpp::platform::is_linux) return f;
        const auto dm = mcpp::toolchain::resolve_clang_driver(tc);
        if (dm.hasCfg) {
            f.push_back("--no-default-config");
            f.push_back("-nostdinc++");
            f.push_back("-stdlib=libc++");
            for (auto& inc : dm.cxxIncludes) f.push_back("-isystem" + inc.string());
            f.push_back("-fuse-ld=lld");
            f.push_back("--rtlib=compiler-rt");
            f.push_back("--unwindlib=libunwind");
            for (auto& d : dm.libDirs) {
                f.push_back("-L" + d.string());
                f.push_back("-Wl,-rpath," + d.string());
            }
        }
        if (lm.mode == mcpp::toolchain::CLibMode::Sysroot) {
            f.push_back("--sysroot=" + lm.sysroot.string());
        } else if (lm.mode == mcpp::toolchain::CLibMode::PayloadFirst) {
            for (auto& inc : lm.systemIncludes) f.push_back("-isystem" + inc.string());
            f.push_back("-B" + lm.crtDir.string());   // Scrt1.o/crti.o discovery
            for (auto& d : lm.libDirs) {
                f.push_back("-L" + d.string());
                f.push_back("-Wl,-rpath," + d.string());
            }
            if (!lm.loader.empty())
                f.push_back("-Wl,--dynamic-linker=" + lm.loader.string());
        }
        // Runtime lib dirs so the produced program can load private libs in-tree.
        for (auto& d : tc.linkRuntimeDirs) {
            f.push_back("-L" + d.string());
            f.push_back("-Wl,-rpath," + d.string());
        }
        return f;
    }

    // GCC: a fresh sandbox g++ needs --sysroot to find the C library + the
    // include-fixed headers; without a sysroot, wire the glibc payload directly.
    if (lm.mode == mcpp::toolchain::CLibMode::Sysroot) {
        f.push_back("--sysroot=" + lm.sysroot.string());
    } else if (lm.mode == mcpp::toolchain::CLibMode::PayloadFirst) {
        for (auto& inc : lm.systemIncludes) {
            f.push_back("-idirafter"); f.push_back(inc.string());
        }
        f.push_back("-B" + lm.crtDir.string());          // crt1.o/crti.o discovery
        for (auto& d : lm.libDirs) f.push_back("-L" + d.string());  // -lc/-lm
    }
    // binutils -B so the driver finds ld/as (GCC, non-musl; musl ships its own).
    if (!mcpp::toolchain::is_musl_target(tc)) {
        auto ar = mcpp::toolchain::archive_tool(tc);
        if (!ar.empty()) f.push_back("-B" + ar.parent_path().string());
    }
    // Runtime lib dirs so the produced program can load private libs in-tree.
    for (auto& d : tc.linkRuntimeDirs) {
        f.push_back("-L" + d.string());
        f.push_back("-Wl,-rpath," + d.string());
    }
    return f;
}

// The bundled `mcpp` build module — a typed API over the stdout wire protocol so
// build.mcpp can `import mcpp;` (no `#include`, no `import std;`). I/O uses
// C-level primitives in the global module fragment, so the module needs no std
// module BMI. The functions mirror the directive set 1:1; they just print the
// `mcpp:` lines the engine already parses. Embedded in the binary (not shipped as
// a file) so it always matches this mcpp's protocol.
// NOTE: the module declaration line uses a `@MODULE@` placeholder (substituted
// with `export module` when written) so mcpp's own line-based module scanner does
// not mistake this embedded string for build_program.cppm exporting a 2nd module.
constexpr std::string_view kMcppModuleSource = R"CPP(module;
#include <cstdio>
#include <cstdlib>
@MODULE@ mcpp;
export namespace mcpp {
inline void cxxflag(const char* flag)             { std::printf("mcpp:cxxflag=%s\n", flag); }
inline void cflag(const char* flag)               { std::printf("mcpp:cflag=%s\n", flag); }
inline void link_lib(const char* name)            { std::printf("mcpp:link-lib=%s\n", name); }
inline void link_search(const char* dir)          { std::printf("mcpp:link-search=%s\n", dir); }
inline void define(const char* name)              { std::printf("mcpp:cfg=%s\n", name); }
inline void generated(const char* path)           { std::printf("mcpp:generated=%s\n", path); }
inline void rerun_if_changed(const char* path)    { std::printf("mcpp:rerun-if-changed=%s\n", path); }
inline void rerun_if_env_changed(const char* var) { std::printf("mcpp:rerun-if-env-changed=%s\n", var); }
// ── environment contract (read side; values injected by the engine) ─────
inline const char* env_or(const char* n)          { const char* v = std::getenv(n); return v ? v : ""; }
inline const char* target()                       { return env_or("MCPP_TARGET"); }
inline const char* host()                         { return env_or("MCPP_HOST"); }
inline const char* profile()                      { return env_or("MCPP_PROFILE"); }
inline const char* out_dir()                      { return env_or("MCPP_OUT_DIR"); }
inline const char* manifest_dir()                 { return env_or("MCPP_MANIFEST_DIR"); }
inline bool has_feature(const char* name) {
    char buf[256] = "MCPP_FEATURE_";
    unsigned long o = 13;
    for (const char* p = name; *p && o + 1 < sizeof buf; ++p, ++o) {
        char c = *p;
        buf[o] = (c >= 'a' && c <= 'z') ? char(c - 'a' + 'A')
               : ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) ? c : '_';
    }
    buf[o] = 0;
    return std::getenv(buf) != nullptr;
}
}
)CPP";

// Compile the bundled `mcpp` module into `bdir` and return the extra flags the
// build.mcpp compile needs to import it (the object `mcpp.o` is linked alongside).
//   GCC   : -fmodules → gcm.cache/mcpp.gcm + mcpp.o; build.mcpp compiles from
//           `bdir` (cwd) so GCC finds gcm.cache/mcpp.gcm.
//   Clang : --precompile → mcpp.pcm, then -c → mcpp.o; pass -fmodule-file=mcpp=<pcm>.
std::expected<std::vector<std::string>, std::string>
build_mcpp_module(const fs::path& bdir, const fs::path& compiler,
                  const std::vector<std::string>& base, const std::string& stdFlag,
                  bool isClang) {
    std::error_code ec;
    fs::path cppm = bdir / "mcpp.cppm";
    std::string moduleSrc(kMcppModuleSource);
    if (auto p = moduleSrc.find("@MODULE@"); p != std::string::npos)
        moduleSrc.replace(p, std::string_view("@MODULE@").size(), "export module");
    { std::ofstream os(cppm, std::ios::trunc);
      os << moduleSrc;
      if (!os) return std::unexpected(std::string("could not write mcpp module source")); }

    auto run = [&](std::vector<std::string> argv, const char* what)
        -> std::expected<void, std::string> {
        auto r = mcpp::platform::process::capture_exec(argv, {}, bdir.string());
        if (r.exit_code != 0)
            return std::unexpected(std::format("mcpp module {} failed (exit {}):\n{}",
                                               what, r.exit_code, r.output));
        return {};
    };
    auto with_base = [&](std::vector<std::string> head) {
        for (auto& b : base) head.push_back(b);
        return head;
    };

    std::vector<std::string> extra;
    if (isClang) {
        if (auto r = run(with_base({compiler.string(), stdFlag, "--precompile",
                                    "mcpp.cppm", "-o", "mcpp.pcm"}), "precompile"); !r)
            return std::unexpected(r.error());
        if (auto r = run(with_base({compiler.string(), stdFlag, "-c",
                                    "mcpp.pcm", "-o", "mcpp.o"}), "object"); !r)
            return std::unexpected(r.error());
        extra.push_back("-fmodule-file=mcpp=" + (bdir / "mcpp.pcm").string());
    } else {
        if (auto r = run(with_base({compiler.string(), stdFlag, "-fmodules", "-c",
                                    "mcpp.cppm", "-o", "mcpp.o"}), "compile"); !r)
            return std::unexpected(r.error());
        extra.push_back("-fmodules");
    }
    return extra;
}

// ── Cache (line-based; one record per line, internal format) ───────────────
// program <hash>
// compiler <hash>
// in <contenthash> <path>
// env <valuehash> <NAME>
// d cxxflag|cflag|ldflag|define|generated <verbatim value to end of line>
// The leading program/compiler/in/env lines are the re-run key; the `d` lines
// are the directives to reapply on a hit.

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
    os << "program " << programHash << '\n';
    os << "compiler " << compilerHash << '\n';
    os << "ctx " << ctxHash << '\n';
    for (auto const& f : d.rerunFiles)
        os << "in " << mcpp::toolchain::hash_file(abs_against_root(root, f)) << ' ' << f << '\n';
    for (auto const& e : d.rerunEnv)
        os << "env " << mcpp::toolchain::hash_string(env_value(e)) << ' ' << e << '\n';
    auto emit = [&](std::string_view kind, const std::vector<std::string>& v) {
        for (auto const& x : v) os << "d " << kind << ' ' << x << '\n';
    };
    emit("cxxflag", d.cxxflags);
    emit("cflag", d.cflags);
    emit("ldflag", d.ldflags);
    emit("define", d.defines);
    emit("generated", d.generated);
}

struct CacheRecord {
    std::string programHash;
    std::string compilerHash;
    std::string ctxHash;   // contract env (target/profile/features/out-dir)
    std::vector<std::pair<std::string, std::string>> inputs;  // (hash, path)
    std::vector<std::pair<std::string, std::string>> envs;    // (hash, name)
    Directives directives;
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
        if (tag == "program") r.programHash = rest;
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
            if (kind == "cxxflag") r.directives.cxxflags.push_back(val);
            else if (kind == "cflag") r.directives.cflags.push_back(val);
            else if (kind == "ldflag") r.directives.ldflags.push_back(val);
            else if (kind == "define") r.directives.defines.push_back(val);
            else if (kind == "generated") r.directives.generated.push_back(val);
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
    if (c.programHash != programHash) return false;
    if (c.compilerHash != compilerHash) return false;
    if (c.ctxHash != ctxHash) return false;   // pre-G3 caches (no ctx line) rerun once
    for (auto const& [h, path] : c.inputs)
        if (mcpp::toolchain::hash_file(abs_against_root(root, path)) != h) return false;
    for (auto const& [h, name] : c.envs)
        if (mcpp::toolchain::hash_string(env_value(name)) != h) return false;
    // A declared generated output that vanished invalidates the cache.
    for (auto const& g : c.directives.generated)
        if (!fs::exists(abs_against_root(root, g))) return false;
    return true;
}

void apply(mcpp::manifest::Manifest& m, const Directives& d) {
    auto& bc = m.buildConfig;
    bc.cxxflags.insert(bc.cxxflags.end(), d.cxxflags.begin(), d.cxxflags.end());
    bc.cflags.insert(bc.cflags.end(), d.cflags.begin(), d.cflags.end());
    bc.ldflags.insert(bc.ldflags.end(), d.ldflags.begin(), d.ldflags.end());
    // cfg defines apply to both C and C++ translation units.
    bc.cflags.insert(bc.cflags.end(), d.defines.begin(), d.defines.end());
    bc.cxxflags.insert(bc.cxxflags.end(), d.defines.begin(), d.defines.end());
    // Generated sources join the source set. BOTH lists: the scanner walks the
    // legacy modules.sources mirror — pushing only bc.sources left a generated
    // file outside the base globs invisible to the scan (latent since L3).
    for (auto const& g : d.generated) {
        bc.sources.push_back(g);
        m.modules.sources.push_back(g);
    }
}

} // namespace

std::expected<void, std::string> run_build_program(
    mcpp::manifest::Manifest& m,
    const fs::path& root,
    const fs::path& hostCompiler,
    const mcpp::toolchain::Toolchain& tc,
    std::string_view cppStandard,
    const BuildProgramEnv& env) {

    fs::path src = root / "build.mcpp";
    std::error_code ec;
    if (!fs::exists(src, ec)) return {};  // no build program — nothing to do

    fs::path bdir = build_dir(root, env);
    fs::path outDir = bdir / "out";
    auto childEnv = contract_env(root, outDir, env);
    std::string ctxHash = contract_hash(childEnv);

    std::string programHash  = mcpp::toolchain::hash_file(src);
    std::string compilerHash = mcpp::toolchain::hash_string(hostCompiler.string());

    // Fast path: declared inputs + contract unchanged → reapply cached
    // directives, no run.
    CacheRecord cache = read_cache(bdir);
    if (cache_fresh(root, cache, programHash, compilerHash, ctxHash)) {
        apply(m, cache.directives);
        mcpp::ui::info("build.mcpp", "up to date (cached)");
        return {};
    }

    fs::create_directories(outDir, ec);   // creates bdir too
    fs::path bin = bdir / "build.mcpp.bin";

    // ── Compile build.mcpp with the host toolchain ──────────────────────────
    std::string std_flag = "-std=" + std::string(cppStandard.empty() ? "c++23" : cppStandard);
    auto base = host_base_flags(tc);

    // Only wire the bundled `mcpp` module when build.mcpp actually imports it —
    // so the common `#include`-based program compiles exactly as before (no
    // -fmodules, cwd = project root). When it does `import mcpp;`, compile the
    // module, link its object, and run the build.mcpp compile from `bdir` so GCC
    // finds gcm.cache/mcpp.gcm.
    std::string srcText;
    { std::ifstream is(src); std::ostringstream ss; ss << is.rdbuf(); srcText = ss.str(); }
    bool usesModule = srcText.find("import mcpp") != std::string::npos;

    std::vector<std::string> moduleFlags;
    if (usesModule) {
        auto mf = build_mcpp_module(bdir, hostCompiler, base, std_flag,
                                    mcpp::toolchain::is_clang(tc));
        if (!mf) return std::unexpected(mf.error());
        moduleFlags = std::move(*mf);
    }

    // `-x c++` is required: the `.mcpp` extension is unknown to the compiler, so
    // without it the driver hands build.mcpp to the linker as a linker script.
    std::vector<std::string> compileArgv = { hostCompiler.string(), std_flag, "-O0" };
    for (auto& bf : base)        compileArgv.push_back(bf);
    for (auto& mf : moduleFlags) compileArgv.push_back(mf);
    compileArgv.push_back("-x"); compileArgv.push_back("c++");
    compileArgv.push_back(src.string());
    if (usesModule) {
        // Link the module object (reset the input language first so the .o isn't
        // treated as C++ source).
        compileArgv.push_back("-x"); compileArgv.push_back("none");
        compileArgv.push_back((bdir / "mcpp.o").string());
    }
    compileArgv.push_back("-o"); compileArgv.push_back(bin.string());
    mcpp::ui::info("build.mcpp", "compiling");
    // GCC resolves `import mcpp;` via gcm.cache/ relative to the compile cwd, so
    // run the module-using compile from bdir; otherwise the project root is fine.
    std::string compileCwd = usesModule ? bdir.string() : root.string();
    auto cres = mcpp::platform::process::capture_exec(compileArgv, {}, compileCwd);
    if (cres.exit_code != 0) {
        return std::unexpected(std::format(
            "build.mcpp failed to compile (exit {}):\n{}", cres.exit_code, cres.output));
    }

    // ── Run it; capture stdout(+stderr) and parse directives ────────────────
    // Run with cwd = package root so the program's relative file writes (e.g.
    // mcpp:generated sources) land in the project, not in mcpp's invocation
    // dir. The MCPP_* contract env is injected into the CHILD only.
    mcpp::ui::info("build.mcpp", "running");
    auto rres = mcpp::platform::process::capture_exec({bin.string()}, childEnv, root.string());
    if (rres.exit_code != 0) {
        return std::unexpected(std::format(
            "build.mcpp exited with {} (build aborted):\n{}", rres.exit_code, rres.output));
    }

    Directives d;
    parse_output(root, rres.output, d);

    // Dependency mode (genBase set): relative `generated=` paths resolve
    // against OUT_DIR-style genBase, not the (possibly read-only, shared)
    // package root — rewrite them to absolute before validation/apply/cache.
    if (!env.genBase.empty()) {
        for (auto& g : d.generated) {
            fs::path gp(g);
            if (gp.is_relative()) g = (env.genBase / gp).lexically_normal().string();
        }
    }

    // Missing declared generated outputs are a hard error (declared-output contract).
    for (auto const& g : d.generated) {
        if (!fs::exists(abs_against_root(root, g))) {
            return std::unexpected(std::format(
                "build.mcpp declared generated source '{}' but it does not exist after the run", g));
        }
    }

    apply(m, d);
    write_cache(bdir, root, programHash, compilerHash, ctxHash, d);
    return {};
}

} // namespace mcpp::build
