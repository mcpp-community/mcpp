// mcpp.build.hostprogram — compiling the bundled `mcpp` module for build.mcpp.
//
// Split out of build_program.cppm for a blunt reason: that file's anonymous
// namespace miscompiles its own neighbours under clang 22.1.8 + C++20 modules
// + -O2. PR#332 established it (an UNUSED `split_ws` was enough to corrupt a
// local vector in `contract_env`), and growing `build_mcpp_module` in place
// reproduced it again — `Segmentation fault: 11` on every macOS build.mcpp
// e2e, in code this change never touched. Mechanism unknown, reproduction
// solid, and the cheap response is to stop growing that namespace.
//
// See .agents/docs/2026-08-02-host-compile-single-producer-design.md §6.2.

export module mcpp.build.hostprogram;

import std;
import mcpp.build.directives;   // kProtocolVersion — the announced value has ONE source
import mcpp.platform;
import mcpp.platform.process;
import mcpp.toolchain.dialect;
import mcpp.toolchain.hostflags;
import mcpp.toolchain.model;

export namespace mcpp::build {

namespace fs = std::filesystem;

// The bundled `mcpp` build module — a typed API over the stdout wire protocol
// so build.mcpp can `import mcpp;` instead of `#include`. Its own I/O uses
// C-level primitives in the global module fragment, so the module itself
// needs no std BMI and stays buildable before one exists. (That was once also
// a limit on build.mcpp; it no longer is — a build.mcpp may `import std;` and
// the engine stages the same std module the main build uses.)
// The functions mirror the directive set 1:1; they just print the
// `mcpp:` lines the engine already parses. Embedded in the binary (not shipped as
// a file) so it always matches this mcpp's protocol.
// NOTE: the module declaration line uses a `@MODULE@` placeholder (substituted
// with `export module` when written) so mcpp's own line-based module scanner does
// not mistake this embedded string for build_program.cppm exporting a 2nd module.
inline constexpr std::string_view kMcppModuleSource = R"CPP(module;
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
inline void source(const char* path)              { std::printf("mcpp:source=%s\n", path); }
inline void include_dir(const char* dir)          { std::printf("mcpp:include-dir=%s\n", dir); }
inline void include_dir_after(const char* dir)    { std::printf("mcpp:include-dir-after=%s\n", dir); }
// ── Build-graph nodes (mcpp 2026.8.5.1+) ────────────────────────────────
// Declare WORK instead of doing it. A build program is a good place to decide
// what the build looks like and a bad place to perform it: work done here is
// serial, whole-set, and reported as "build.mcpp exited 1". Declared as a node
// it becomes an edge in the build graph — incremental, parallel, attributable.
//
// You must name the OUTPUT FILES. mcpp fixes the source set, the fingerprint
// and the module graph during prepare, so an output whose name is unknown
// cannot be built. Content may arrive later; names may not.
struct action {
    const char* id          = "";
    const char* role        = "source";   // "source" | "check" | "artifact"
    const char* description = "";
    bool        blocking    = false;      // check only: gate compilation on it
    action& input(const char* p)    { add(inputs_,  sizeof inputs_,  p); return *this; }
    action& output(const char* p)   { add(outputs_, sizeof outputs_, p); return *this; }
    action& arg(const char* a)      { add(command_, sizeof command_, a); return *this; }
    // Declare what a generated MODULE INTERFACE provides/imports. Same
    // "declare instead of discover" trade [modules].scan_overrides makes, and
    // what lets a generated .cppm exist as a graph node at all.
    action& provides(const char* n) { add(provides_, sizeof provides_, n); return *this; }
    action& imports(const char* n)  { add(imports_,  sizeof imports_,  n); return *this; }
    void submit() const {
        std::printf("mcpp:action={\"id\":");        esc(id);
        std::printf(",\"role\":");                  esc(role);
        std::printf(",\"description\":");           esc(description);
        std::printf(",\"blocking\":%s", blocking ? "true" : "false");
        // A truncated argv would otherwise be INVALID rather than obviously
        // wrong — the engine turns this marker into a diagnostic that names
        // the limit, instead of a generic "malformed action".
        if (overflow_) std::printf(",\"overflow\":true");
        std::printf(",\"inputs\":[%s]",   inputs_);
        std::printf(",\"outputs\":[%s]",  outputs_);
        std::printf(",\"command\":[%s]",  command_);
        std::printf(",\"provides\":[%s]", provides_);
        std::printf(",\"imports\":[%s]",  imports_);
        std::printf("}\n");
    }
private:
    // Fixed buffers because this module must stay buildable BEFORE a std BMI
    // exists (it is what a build.mcpp imports, and it may be compiled first) —
    // so no std::string. Sizes chosen for real generator invocations: a protoc
    // command line with many -I paths runs long.
    char inputs_[8192]{}, outputs_[8192]{}, command_[16384]{},
         provides_[2048]{}, imports_[2048]{};
    mutable bool overflow_ = false;
    static void esc(const char* s) {
        std::putchar('"');
        for (const char* p = s; *p; ++p) {
            unsigned char c = (unsigned char)*p;
            if (c == '"' || c == '\\') { std::putchar('\\'); std::putchar(c); continue; }
            // Any control character has to be escaped or the payload is not
            // JSON at all. \n was handled before; \t and \r reach this code
            // through ordinary Windows paths and log text.
            if (c < 0x20) { std::printf("\\u%04x", c); continue; }
            std::putchar(c);
        }
        std::putchar('"');
    }
    // Capacity is a PARAMETER. The previous revision hardcoded 4096 while the
    // smallest buffer here was 1024 — a bound living somewhere other than next
    // to the array it bounds is exactly the shape that overflows.
    bool add(char* buf, unsigned long cap, const char* s) {
        unsigned long o = 0; while (buf[o]) ++o;
        if (o + 4 >= cap) { overflow_ = true; return false; }
        if (o) buf[o++] = ',';
        buf[o++] = '"';
        for (const char* p = s; *p; ++p) {
            if (o + 3 >= cap) { buf[o] = 0; overflow_ = true; return false; }
            if (*p == '"' || *p == '\\') buf[o++] = '\\';
            buf[o++] = *p;
        }
        buf[o++] = '"';
        buf[o] = 0;
        return true;
    }
};
inline void rerun_if_changed(const char* path)    { std::printf("mcpp:rerun-if-changed=%s\n", path); }
inline void rerun_if_env_changed(const char* var) { std::printf("mcpp:rerun-if-env-changed=%s\n", var); }
// ── environment contract (read side; values injected by the engine) ─────
inline const char* env_or(const char* n)          { const char* v = std::getenv(n); return v ? v : ""; }
inline const char* target()                       { return env_or("MCPP_TARGET"); }
inline const char* target_os()                    { return env_or("MCPP_TARGET_OS"); }
inline const char* target_arch()                  { return env_or("MCPP_TARGET_ARCH"); }
inline const char* target_env()                   { return env_or("MCPP_TARGET_ENV"); }
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
// mcpp#241: resolved install dir of a declared dependency (by its package
// name), or "" if not found. Same sanitize as has_feature; wraps
// MCPP_DEP_<SANITIZED_NAME>_DIR.
inline const char* dep_dir(const char* name) {
    char buf[256] = "MCPP_DEP_";
    unsigned long o = 9;
    for (const char* p = name; *p && o + 5 < sizeof buf; ++p, ++o) {
        char c = *p;
        buf[o] = (c >= 'a' && c <= 'z') ? char(c - 'a' + 'A')
               : ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) ? c : '_';
    }
    buf[o++] = '_'; buf[o++] = 'D'; buf[o++] = 'I'; buf[o++] = 'R'; buf[o] = 0;
    return env_or(buf);
}
// mcpp#355: absolute path to a HOST tool built by a dependency — the binary
// behind one of its `kind = "bin"` targets. Returns "" unless the consumer
// declared it:  <dep> = { version = "…", tools = ["protoc"] }
// The path already carries the platform's executable suffix.
inline const char* dep_bin(const char* pkg, const char* tool) {
    char buf[256] = "MCPP_DEP_";
    unsigned long o = 9;
    auto put = [&](const char* s) {
        for (const char* p = s; *p && o + 8 < sizeof buf; ++p, ++o) {
            char c = *p;
            buf[o] = (c >= 'a' && c <= 'z') ? char(c - 'a' + 'A')
                   : ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) ? c : '_';
        }
    };
    put(pkg);
    buf[o++] = '_'; buf[o++] = 'B'; buf[o++] = 'I'; buf[o++] = 'N'; buf[o++] = '_';
    put(tool);
    buf[o] = 0;
    return env_or(buf);
}
}
// ── Protocol announcement ───────────────────────────────────────────────
// Emitted before main() runs, so a program that uses `import mcpp;` never has
// to remember to declare anything. The engine uses it two ways: it refuses a
// program that speaks a NEWER protocol than it understands, and — because the
// two sides then provably agree — it treats an unrecognized directive as an
// error rather than warning and silently dropping it.
//
// A hand-written `printf("mcpp:...")` program emits no announcement, which is
// exactly right: that surface is frozen at protocol 1 and keeps the historical
// warn-and-ignore behaviour.
//
// Namespace-scope `static` in the module purview: internal linkage, one object
// in mcpp.o, whose dynamic initializer runs from .init_array. mcpp.o is always
// on the link line, so it always fires.
namespace mcpp_detail {
struct ProtocolAnnouncer {
    ProtocolAnnouncer() { std::printf("mcpp:protocol=%d\n", @PROTOCOL@); }
};
static ProtocolAnnouncer mcpp_protocol_announcer;
}
)CPP";

// Compile the bundled `mcpp` module into `bdir` and return the extra flags the
// build.mcpp compile needs to import it (the object `mcpp.o` is linked alongside).
//   GCC   : -fmodules → gcm.cache/mcpp.gcm + mcpp.o; build.mcpp compiles from
//           `bdir` (cwd) so GCC finds gcm.cache/mcpp.gcm.
//   Clang : --precompile → mcpp.pcm, then -c → mcpp.o; pass -fmodule-file=mcpp=<pcm>.
// Does the source contain `import <name>;`?
//
// A plain substring search is not enough here: "import std" is a prefix of
// "import std.compat", so the naive test reports both for a program that
// only imports the latter, and mcpp would build a std BMI nobody asked for.
// Match the whole module name and require the terminating `;`, tolerating
// the whitespace the grammar allows. Occurrences inside comments or string
// literals still match — over-detection costs one cached BMI lookup, never
// a wrong build, and that is the same trade the `import mcpp` check has
// always made.
bool imports_module(std::string_view src, std::string_view name) {
    constexpr std::string_view kImport = "import";
    std::size_t pos = 0;
    while ((pos = src.find(kImport, pos)) != std::string_view::npos) {
        std::size_t i = pos + kImport.size();
        // `importfoo` is not an import.
        if (i >= src.size() || (src[i] != ' ' && src[i] != '\t')) { ++pos; continue; }
        while (i < src.size() && (src[i] == ' ' || src[i] == '\t')) ++i;
        if (src.compare(i, name.size(), name) == 0) {
            std::size_t j = i + name.size();
            while (j < src.size() && (src[j] == ' ' || src[j] == '\t')) ++j;
            if (j < src.size() && src[j] == ';') return true;
        }
        ++pos;
    }
    return false;
}


// What the bundled `mcpp` module contributes to the build.mcpp compile.
struct McppModule {
    std::vector<std::string> useFlags;   // how the consumer names the BMI
    fs::path                 object;     // linked alongside build.mcpp
};

// Compile ONE dependency-provided module interface for the host, into `bdir`,
// with the SAME flags build.mcpp itself gets. Returns how to name its BMI plus
// the object to link.
//
// Shares build_mcpp_module's per-family dispatch deliberately: a BMI is only
// usable by a compile that agrees with it on standard, dialect and compiler
// identity, and the cheapest way to guarantee that is to produce both from one
// set of flags rather than to check afterwards.
//
// Limitation, stated rather than hidden: the interface is compiled ALONE, so it
// may import `std` and the bundled `mcpp` module but not a third package. A
// rule package is a leaf by construction; a transitive host module graph would
// need the sub-build machinery and its own BMI-agreement story.
std::expected<McppModule, std::string>
build_host_module(const fs::path& bdir, const fs::path& compiler,
                  const std::vector<std::string>& base, const std::string& stdFlag,
                  const mcpp::toolchain::Toolchain& tc,
                  const std::vector<std::pair<std::string, std::string>>& env,
                  std::string_view logicalName, const fs::path& interfacePath,
                  const std::vector<std::string>& extraUseFlags);

std::expected<McppModule, std::string>
build_mcpp_module(const fs::path& bdir, const fs::path& compiler,
                  const std::vector<std::string>& base, const std::string& stdFlag,
                  const mcpp::toolchain::Toolchain& tc,
                  const std::vector<std::pair<std::string, std::string>>& env) {
    std::error_code ec;
    fs::path cppm = bdir / "mcpp.cppm";
    std::string moduleSrc(kMcppModuleSource);
    if (auto p = moduleSrc.find("@MODULE@"); p != std::string::npos)
        moduleSrc.replace(p, std::string_view("@MODULE@").size(), "export module");
    // Substituted rather than hardcoded so the announced version can never
    // drift from the one the engine checks against.
    if (auto p = moduleSrc.find("@PROTOCOL@"); p != std::string::npos)
        moduleSrc.replace(p, std::string_view("@PROTOCOL@").size(),
                          std::to_string(mcpp::build::directives::kProtocolVersion));
    { std::ofstream os(cppm, std::ios::trunc);
      os << moduleSrc;
      if (!os) return std::unexpected(std::string("could not write mcpp module source")); }

    auto run = [&](std::vector<std::string> argv, const char* what)
        -> std::expected<void, std::string> {
        auto r = mcpp::platform::process::capture_exec(argv, env, bdir.string());
        if (r.exit_code != 0)
            return std::unexpected(std::format("mcpp module {} failed (exit {}):\n{}",
                                               what, r.exit_code, r.output));
        return {};
    };
    auto with_base = [&](std::vector<std::string> head) {
        for (auto& b : base) head.push_back(b);
        return head;
    };

    // Dispatch on the SAME module table the main build uses (BmiTraits +
    // CommandDialect), not on a local is_clang/else. That is what makes a
    // toolchain family work here as soon as it works there — adding cl.exe
    // needed no new pipeline, only this row.
    const auto traits = mcpp::toolchain::bmi_traits(tc);
    const auto& dial  = mcpp::toolchain::dialect_for(tc);
    McppModule out;

    if (tc.compiler == mcpp::toolchain::CompilerId::MSVC) {
        // cl produces the .ifc and the .obj in one step.
        fs::path ifc = bdir / ("mcpp" + std::string(traits.bmiExt));
        out.object = bdir / ("mcpp" + std::string(dial.objExt));
        std::vector<std::string> argv{compiler.string()};
        for (auto f : dial.alwaysFlagsArgv) argv.emplace_back(f);
        argv.push_back(stdFlag);
        argv.push_back("/interface");
        for (auto f : dial.forceCxxLangArgv) argv.emplace_back(f);
        argv.push_back(dial.compileOnly == std::string_view("/c") ? "/c" : "-c");
        argv.push_back("mcpp.cppm");
        argv.push_back("/ifcOutput"); argv.push_back(ifc.string());
        argv.push_back(std::string(dial.outputObjPrefix) + out.object.string());
        if (auto r = run(with_base(std::move(argv)), "compile"); !r)
            return std::unexpected(r.error());
        out.useFlags = mcpp::toolchain::bmi_reference_tokens(" /reference mcpp=", ifc);
        return out;
    }

    out.object = bdir / ("mcpp" + std::string(dial.objExt));
    if (mcpp::toolchain::is_clang(tc)) {
        fs::path pcm = bdir / ("mcpp" + std::string(traits.bmiExt));
        if (auto r = run(with_base({compiler.string(), stdFlag, "--precompile",
                                    "mcpp.cppm", "-o", pcm.string()}), "precompile"); !r)
            return std::unexpected(r.error());
        if (auto r = run(with_base({compiler.string(), stdFlag, "-c",
                                    pcm.string(), "-o", out.object.string()}), "object"); !r)
            return std::unexpected(r.error());
        out.useFlags = mcpp::toolchain::bmi_reference_tokens("-fmodule-file=mcpp=", pcm);
        return out;
    }

    // GCC: BMIs are implicit under <cwd>/gcm.cache, so nothing to name.
    if (auto r = run(with_base({compiler.string(), stdFlag,
                                std::string(mcpp::toolchain::bmi_traits(tc).compileModulesFlag).empty()
                                    ? "-fmodules" : "-fmodules",
                                "-c", "mcpp.cppm", "-o", out.object.string()}), "compile"); !r)
        return std::unexpected(r.error());
    out.useFlags = {"-fmodules"};
    return out;
}


} // namespace mcpp::build

namespace mcpp::build {

std::expected<McppModule, std::string>
build_host_module(const fs::path& bdir, const fs::path& compiler,
                  const std::vector<std::string>& base, const std::string& stdFlag,
                  const mcpp::toolchain::Toolchain& tc,
                  const std::vector<std::pair<std::string, std::string>>& env,
                  std::string_view logicalName, const fs::path& interfacePath,
                  const std::vector<std::string>& extraUseFlags) {
    std::error_code ec;
    if (!fs::exists(interfacePath, ec)) {
        return std::unexpected(std::format(
            "host module '{}': no interface unit at {}\n"
            "       A package offering build rules must have a lib root "
            "(src/<name>.cppm or [lib] path).",
            logicalName, interfacePath.string()));
    }
    // A filesystem-safe stem. Partition separators and any path separator that
    // sneaks into a logical name would otherwise create directories that do
    // not exist. Dots are left ALONE on purpose: `a.b.rules.o` is a legal
    // filename, GCC's own gcm.cache uses the dotted module name verbatim, and
    // rewriting them would make the object name disagree with the BMI name for
    // no gain.
    std::string stem(logicalName);
    for (auto& c : stem) if (c == ':' || c == '/' || c == '\\') c = '-';

    auto run = [&](std::vector<std::string> argv, const char* what)
        -> std::expected<void, std::string> {
        auto r = mcpp::platform::process::capture_exec(argv, env, bdir.string());
        if (r.exit_code != 0)
            return std::unexpected(std::format(
                "host module '{}' {} failed (exit {}):\n{}",
                logicalName, what, r.exit_code, r.output));
        return {};
    };
    auto with_base = [&](std::vector<std::string> head) {
        for (auto& b : base)          head.push_back(b);
        for (auto& f : extraUseFlags) head.push_back(f);
        return head;
    };

    const auto traits = mcpp::toolchain::bmi_traits(tc);
    const auto& dial  = mcpp::toolchain::dialect_for(tc);
    McppModule out;
    out.object = bdir / (stem + std::string(dial.objExt));

    if (tc.compiler == mcpp::toolchain::CompilerId::MSVC) {
        fs::path ifc = bdir / (stem + std::string(traits.bmiExt));
        std::vector<std::string> argv{compiler.string()};
        for (auto f : dial.alwaysFlagsArgv) argv.emplace_back(f);
        argv.push_back(stdFlag);
        argv.push_back("/interface");
        for (auto f : dial.forceCxxLangArgv) argv.emplace_back(f);
        argv.push_back("/c");
        argv.push_back(interfacePath.string());
        argv.push_back("/ifcOutput"); argv.push_back(ifc.string());
        argv.push_back(std::string(dial.outputObjPrefix) + out.object.string());
        if (auto r = run(with_base(std::move(argv)), "compile"); !r)
            return std::unexpected(r.error());
        out.useFlags = mcpp::toolchain::bmi_reference_tokens(
            std::format(" /reference {}=", logicalName), ifc);
        return out;
    }

    if (mcpp::toolchain::is_clang(tc)) {
        fs::path pcm = bdir / (stem + std::string(traits.bmiExt));
        if (auto r = run(with_base({compiler.string(), stdFlag, "--precompile",
                                    interfacePath.string(), "-o", pcm.string()}),
                         "precompile"); !r)
            return std::unexpected(r.error());
        if (auto r = run(with_base({compiler.string(), stdFlag, "-c",
                                    pcm.string(), "-o", out.object.string()}),
                         "object"); !r)
            return std::unexpected(r.error());
        out.useFlags = mcpp::toolchain::bmi_reference_tokens(
            std::format("-fmodule-file={}=", logicalName), pcm);
        return out;
    }

    // GCC: BMIs are implicit under <cwd>/gcm.cache, so nothing to name — which
    // is also why the compile has to happen in bdir (it already does).
    if (auto r = run(with_base({compiler.string(), stdFlag, "-fmodules", "-c",
                                interfacePath.string(), "-o", out.object.string()}),
                     "compile"); !r)
        return std::unexpected(r.error());
    out.useFlags = {"-fmodules"};
    return out;
}

} // namespace mcpp::build
