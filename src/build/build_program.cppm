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
import mcpp.platform.process;
import mcpp.toolchain.fingerprint;   // hash_file / hash_string (FNV-1a, 16 hex)
import mcpp.toolchain.model;         // Toolchain, PayloadPaths, is_clang/is_musl_target
import mcpp.toolchain.registry;      // archive_tool
import mcpp.ui;

export namespace mcpp::build {

// Compile + run `<root>/build.mcpp` (if present) with `hostCompiler` (the resolved
// host frontend) and apply its directives to `m.buildConfig`. `tc` supplies the
// sysroot / runtime flags a fresh sandbox needs to compile + link a freestanding
// host program. No-op when the file is absent. `isCross` skips execution (a host
// build program can't run when compiled for another target).
std::expected<void, std::string> run_build_program(
    mcpp::manifest::Manifest& m,
    const std::filesystem::path& root,
    const std::filesystem::path& hostCompiler,
    const mcpp::toolchain::Toolchain& tc,
    std::string_view cppStandard,
    bool isCross);

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
    // Clang reads its sibling `<clang>.cfg` by default, which wires libc++ + the
    // sysroot. A simple host compile trusts it (the main build bypasses the cfg
    // for reproducibility; here correctness on a fresh box is all we need).
    if (mcpp::toolchain::is_clang(tc)) return f;

    // GCC: a fresh sandbox g++ needs --sysroot to find the C library + the
    // include-fixed headers; without a sysroot, wire the glibc payload directly.
    if (!tc.sysroot.empty()) {
        f.push_back("--sysroot=" + tc.sysroot.string());
    } else if (tc.payloadPaths) {
        auto& pp = *tc.payloadPaths;
        f.push_back("-idirafter"); f.push_back(pp.glibcInclude.string());
        if (!pp.linuxInclude.empty()) { f.push_back("-idirafter"); f.push_back(pp.linuxInclude.string()); }
        f.push_back("-B" + pp.glibcLib.string());   // crt1.o/crti.o discovery
        f.push_back("-L" + pp.glibcLib.string());   // -lc/-lm resolution
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
fs::path build_dir(const fs::path& root) { return root / "target" / ".build-mcpp"; }

std::string cache_path(const fs::path& root) {
    return (build_dir(root) / "build.mcpp.cache").string();
}

void write_cache(const fs::path& root, const std::string& programHash,
                 const std::string& compilerHash, const Directives& d) {
    std::ofstream os(cache_path(root), std::ios::trunc);
    if (!os) return;  // best-effort: a failed cache write only loses the optimization
    os << "program " << programHash << '\n';
    os << "compiler " << compilerHash << '\n';
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
    std::vector<std::pair<std::string, std::string>> inputs;  // (hash, path)
    std::vector<std::pair<std::string, std::string>> envs;    // (hash, name)
    Directives directives;
    bool loaded = false;
};

CacheRecord read_cache(const fs::path& root) {
    CacheRecord r;
    std::ifstream is(cache_path(root));
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
                 const std::string& programHash, const std::string& compilerHash) {
    if (!c.loaded) return false;
    if (c.programHash != programHash) return false;
    if (c.compilerHash != compilerHash) return false;
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
    // Generated sources join the source glob set so the modgraph scanner finds them.
    for (auto const& g : d.generated) bc.sources.push_back(g);
}

} // namespace

std::expected<void, std::string> run_build_program(
    mcpp::manifest::Manifest& m,
    const fs::path& root,
    const fs::path& hostCompiler,
    const mcpp::toolchain::Toolchain& tc,
    std::string_view cppStandard,
    bool isCross) {

    fs::path src = root / "build.mcpp";
    std::error_code ec;
    if (!fs::exists(src, ec)) return {};  // no build program — nothing to do

    if (isCross) {
        mcpp::ui::warning(
            "build.mcpp present but skipped under a cross --target build "
            "(it compiles and runs on the host; host-toolchain-for-cross is a follow-up)");
        return {};
    }

    std::string programHash  = mcpp::toolchain::hash_file(src);
    std::string compilerHash = mcpp::toolchain::hash_string(hostCompiler.string());

    // Fast path: declared inputs unchanged → reapply cached directives, no run.
    CacheRecord cache = read_cache(root);
    if (cache_fresh(root, cache, programHash, compilerHash)) {
        apply(m, cache.directives);
        mcpp::ui::info("build.mcpp", "up to date (cached)");
        return {};
    }

    fs::create_directories(build_dir(root), ec);
    fs::path bin = build_dir(root) / "build.mcpp.bin";

    // ── Compile build.mcpp with the host toolchain ──────────────────────────
    std::string std_flag = "-std=" + std::string(cppStandard.empty() ? "c++23" : cppStandard);
    // `-x c++` is required: the `.mcpp` extension is unknown to the compiler, so
    // without it the driver hands build.mcpp to the linker as a linker script.
    std::vector<std::string> compileArgv = { hostCompiler.string(), std_flag, "-O0" };
    for (auto& bf : host_base_flags(tc)) compileArgv.push_back(bf);
    compileArgv.push_back("-x"); compileArgv.push_back("c++");
    compileArgv.push_back(src.string());
    compileArgv.push_back("-o"); compileArgv.push_back(bin.string());
    mcpp::ui::info("build.mcpp", "compiling");
    auto cres = mcpp::platform::process::capture_exec(compileArgv);
    if (cres.exit_code != 0) {
        return std::unexpected(std::format(
            "build.mcpp failed to compile (exit {}):\n{}", cres.exit_code, cres.output));
    }

    // ── Run it; capture stdout(+stderr) and parse directives ────────────────
    mcpp::ui::info("build.mcpp", "running");
    auto rres = mcpp::platform::process::capture_exec({bin.string()});
    if (rres.exit_code != 0) {
        return std::unexpected(std::format(
            "build.mcpp exited with {} (build aborted):\n{}", rres.exit_code, rres.output));
    }

    Directives d;
    parse_output(root, rres.output, d);

    // Missing declared generated outputs are a hard error (declared-output contract).
    for (auto const& g : d.generated) {
        if (!fs::exists(abs_against_root(root, g))) {
            return std::unexpected(std::format(
                "build.mcpp declared generated source '{}' but it does not exist after the run", g));
        }
    }

    apply(m, d);
    write_cache(root, programHash, compilerHash, d);
    return {};
}

} // namespace mcpp::build
