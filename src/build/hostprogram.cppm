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
// One argv token of the command that EXECUTES this build's artifact, when the
// host cannot run it itself (a freestanding image: wrong ISA, no loader).
//
// Called once per token, in order — argv is an ordered list and a directive
// carries one value per line. The artifact path is appended by mcpp, or
// substituted for a `{}` token if one is present.
//
// ⚠️ Emit the executable as an ABSOLUTE path. A bare name resolves through
// PATH to a shim that dispatches against its OWNER home, which is not
// necessarily the home this build uses; measured in CI as
// `xlings: 'qemu-system-riscv64' is not installed` from a job where the same
// bare name had answered `--version` two steps earlier. `xpkg_dir()` is how a
// package finds the payload it declared.
//
// ⚠️ Exactly one dependency may supply this. Two board-support packages both
// claiming to know how to run the artifact is a configuration error, and mcpp
// reports it naming both rather than merging them.
inline void runner(const char* token)             { std::printf("mcpp:runner=%s\n", token); }
// ⭐ A NAMED way of reaching the artefact. The engine learns the name from
// here and knows nothing else about it, so `flash`, `serve`, `deploy`,
// `submit` and `logcat` cost the same: nothing.
//
// One token per call, because argv is ordered and a single string cannot say
// where its boundaries are. The user reaches it with `mcpp run --runner <name>`.
inline void runner(const char* name, const char* token) {
    std::printf("mcpp:runner-named=%s:%s\n", name, token);
}
// This named runner has no natural end — a console monitor, a debug server.
// ⚠️ DECLARED RATHER THAN DERIVED FROM THE NAME: the engine has no list of
// names to derive it from, which is the point.
inline void runner_longlived(const char* name) {
    std::printf("mcpp:runner-longlived=%s\n", name);
}
inline void run_exclusive()                    { std::printf("mcpp:run-exclusive=1\n"); }

// Say something to the user and keep going.
//
// ⚠️ THIS IS THE ONLY WAY A BUILD PROGRAM CAN SUCCEED AND STILL BE HEARD.
// mcpp prints what it captured from a build program only when that program
// EXITS NON-ZERO, so a `std::printf` or `std::fprintf(stderr, ...)` note is
// invisible on precisely the successful builds that needed it.
//
// Use it for a condition the program handled correctly but the user would
// want to know about — most often "I could not find X, so I configured
// nothing that depends on it". Do not use it for an error: exit non-zero
// instead, and the output is printed already.
//
// ⚠️ It survives the build cache. A cached run does not re-execute the
// program, and an advisory that appeared once and then vanished would read as
// "resolved". mcpp replays it on every hit.
inline void warning(const char* message)          { std::printf("mcpp:warning=%s\n", message); }

// ── The probe channel (mcpp 2026.9.6+) ────────────────────────────────────
//
// A rule package is the thing that knows how to ask a machine what it has --
// which library to open, which function to call -- and the engine is the
// thing that must not. So the package MEASURES and the engine COMPARES:
//
//     mcpp::fact("cuda.driver", "12.4");        // what this machine has
//     mcpp::floor("cuda.driver >= 12.0");       // what this package needs
//
// Before anything is compiled the engine refuses when a floor is unmet and
// names both values (`mcpp why toolchain --format json` reports the reason
// `version-floor-unmet`). A floor nobody stated a fact for is silent: not
// knowing is not failing.
//
// ⚠️ A fact is persisted with the program's other output and replayed on a
// cache hit. State what would change it -- `rerun_if_changed` on the library
// the version was read from -- or the fact outlives the machine it described.
inline void fact(const char* name, const char* version) {
    std::printf("mcpp:fact=%s=%s\n", name, version);
}
inline void floor(const char* spec)               { std::printf("mcpp:floor=%s\n", spec); }

// The memory layout for a freestanding link. Reaches the CONSUMER's link line
// (like link_lib/link_search, unlike include_dir), because the package that
// knows a board's layout is not the package being built.
inline void link_script(const char* path)         { std::printf("mcpp:link-script=%s\n", path); }
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
    const char* role        = "source";   // "source" | "check" | "object" | "artifact"
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
    // Object only: which link unit receives the outputs. Omit for "every image
    // this package produces" — which INCLUDES test binaries, and is what you
    // want: their names come from tests/*.cpp, so spelling one here breaks
    // plain `mcpp build`, where that link unit does not exist. An Artifact reads
    // its target out of ${mcpp.target_file:NAME}; an Object runs before the link
    // and has no such handle, so it has to say the name.
    action& target(const char* n)   { add(targets_,  sizeof targets_,  n); return *this; }
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
        std::printf(",\"targets\":[%s]",  targets_);
        std::printf("}\n");
    }
private:
    // Fixed buffers because this module must stay buildable BEFORE a std BMI
    // exists (it is what a build.mcpp imports, and it may be compiled first) —
    // so no std::string. Sizes chosen for real generator invocations: a protoc
    // command line with many -I paths runs long.
    char inputs_[8192]{}, outputs_[8192]{}, command_[16384]{},
         provides_[2048]{}, imports_[2048]{}, targets_[1024]{};
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
// mcpp#359: re-run when the SET of files matching `pattern` changes — a file
// appearing or disappearing, not its contents (declare those with
// rerun_if_changed). `pattern` is relative to the manifest directory and uses
// the same `*` / `**` grammar as `sources = [...]`, e.g. "proto/**/*.proto".
//
// Without this a build program that globs is structurally unsafe: adding a
// .proto changes no declared file's hash, so the program does not re-run and
// the new file is silently never generated. The build output tree and .git are
// never part of the set, so watching a wide pattern cannot create a re-run
// loop with the program's own outputs.
inline void rerun_if_changed_glob(const char* pattern) {
    std::printf("mcpp:rerun-if-changed-glob=%s\n", pattern);
}
inline void rerun_if_env_changed(const char* var) { std::printf("mcpp:rerun-if-env-changed=%s\n", var); }
// ── environment contract (read side; values injected by the engine) ─────
inline const char* env_or(const char* n)          { const char* v = std::getenv(n); return v ? v : ""; }
inline const char* target()                       { return env_or("MCPP_TARGET"); }
inline const char* target_os()                    { return env_or("MCPP_TARGET_OS"); }
inline const char* target_arch()                  { return env_or("MCPP_TARGET_ARCH"); }
inline const char* target_env()                   { return env_or("MCPP_TARGET_ENV"); }
inline const char* host()                         { return env_or("MCPP_HOST"); }
inline const char* profile()                      { return env_or("MCPP_PROFILE"); }
// The device axis of this build: `cuda12.9+{sm_89} ptx>=89`, or "" when the
// build asks for no accelerator. Already resolved (`--accel` / `--no-accel`
// over `[build] accel`), so a rule package derives its architecture flags
// from HERE and the set is written once, in the manifest. What the string
// means beyond "backend, version, architectures, floor" is the package's
// business: the engine never learns what `sm_89` is.
inline const char* accel()                        { return env_or("MCPP_ACCEL"); }
// The device-kind sources (`.cu`, `.hip`, ...) this package's `sources` match
// under the current accel, package-root-relative, one per line, "" when there
// are none. The engine compiles none of them; the rule package this program
// imports turns each into an `mcpp::action`. Already narrowed: a glob written
// as `{ glob = "...", accel = "..." }` whose constraint the build does not
// satisfy contributes nothing, so `--no-accel` yields an empty list.
inline const char* device_sources()               { return env_or("MCPP_DEVICE_SOURCES"); }
inline const char* out_dir()                      { return env_or("MCPP_OUT_DIR"); }

// Where the TOOLCHAIN mcpp resolved for this build lives — the payload root,
// the directory whose `bin/` holds the driver.
//
// ⚠️ This exists so a package never has to DECLARE a toolchain. A package that
// needs headers the toolchain ships (libc++'s, for a freestanding standard
// library subset) previously had to put `xim:llvm` in `[xlings] deps`, which
// pinned it to one implementation — and the measured fact is that the same
// subset works over libstdc++'s freestanding mode too, so pinning was not
// merely inelegant, it closed a road. Asking here follows whatever
// `[toolchain]` actually resolved.
inline const char* toolchain_dir()                { return env_or("MCPP_TOOLCHAIN_DIR"); }

// Which compiler resolved: "gcc", "clang", "msvc", or "" if none did.
//
// ⭐ Ask this rather than inferring it from `toolchain_dir()`. The two questions
// a package has actually needed it for are which runtime library holds the
// routines the compiler emits calls to, and which spelling of a binutils tool
// exists beside the driver — and both have a different right answer per family
// rather than per version or per payload.
inline const char* compiler()                     { return env_or("MCPP_COMPILER"); }

// Where the TARGET's C library lives, for targets that have one of their own
// (today: bare metal). Same argument one line up: the libc is a property of
// the target, mcpp resolves it from the target's own row, and a package that
// needs to name a FILE inside it (a linker script) asks rather than declares.
//
// Empty on a hosted target — there the libc comes with the compiler payload or
// through the runtime binding, and nothing has to look for it.
inline const char* sysroot_dir()                  { return env_or("MCPP_TARGET_SYSROOT"); }

// ── Three answers a board-support package would otherwise hardcode ───────────
//
// ⚠️ The coupling these remove does not appear in any manifest. A board package
// can declare no dependency on LLVM and none on picolibc — and still be unable
// to serve a second toolchain or a second C library, because it wrote their
// names into its `build.mcpp`. A declared dependency is visible and reviewable;
// a hardcoded name fails only when something is swapped, which is exactly when
// nobody is looking for it.
//
// The rule that decides what belongs here is the one the layering already
// uses: LOCATION IS A TARGET FACT, SELECTION IS A BOARD FACT.

// The compiler's builtins library, by bare name: `clang_rt.builtins-riscv64`
// for an LLVM payload, `gcc` for a GCC one.
//
// A board does not choose whether to have builtins — every freestanding link
// needs them, and on rv64 the trigger is picolibc's printf doing 128-bit
// shifts, which the ISA has no instruction for. What varies is only which
// implementation the resolved toolchain ships, and that is not a board fact.
// Empty on a hosted target, where the driver links them without being asked.
inline const char* target_builtins_lib()          { return env_or("MCPP_TARGET_BUILTINS_LIB"); }

// The C library's sub-directory for this target's ISA profile, e.g.
// `rv64gc/lp64d`. It is the multilib convention of whichever C library the
// target resolved, with no board input at all — a board that wanted a
// different layout would be using a different C library.
//
// Empty when the target has no C library of its own (the zero-libc tier, or a
// hosted target).
inline const char* target_libc_profile()          { return env_or("MCPP_TARGET_LIBC_PROFILE"); }

// The C library's package name, e.g. `picolibc-riscv`; empty on the zero-libc
// tier and on hosted targets.
//
// This one does NOT remove a coupling — it makes one visible. A board package
// that genuinely must differ between picolibc and newlib (the crt0 object is
// named differently, and that IS a board choice) can branch on this instead of
// assuming. An explicit branch can be read and can be extended; an assumption
// baked into a string literal can be neither.
inline const char* target_libc()                  { return env_or("MCPP_TARGET_LIBC"); }

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
// The payload directory of a package declared in `[xlings] deps`.
//
// An INTERFACE, not a naming convention. `dep_dir` answers for mcpp
// dependencies and cannot answer for xlings ones: they are a different
// namespace with a different store layout, and a build.mcpp that reconstructed
// `<home>/data/xpkgs/<ns>-x-<name>/<version>` itself would be encoding store
// internals that mcpp is free to change — the exact thing `dep_dir` exists to
// avoid ("instead of reverse-engineering the store layout").
//
// Ask with the spelling the manifest used:
//
//     deps = ["xim:picolibc-riscv@1.8.12"]
//     xpkg_dir("xim", "picolibc-riscv")   // exact, and preferred
//     xpkg_dir("picolibc-riscv")          // bare name
//
// The namespaced form is tried first and answers only for a package declared
// under that namespace. The bare form is a convenience for the common single
// declaration; when two namespaces declare the same name, only the namespaced
// form can say which one is meant, and the bare one answers for the first
// declared. Returns "" when the package was not declared or is not installed —
// a caller that needs it should say so itself, because only it knows whether
// the absence is fatal.
inline const char* xpkg_dir(const char* ns, const char* name) {
    char buf[256] = "MCPP_XPKG_";
    unsigned long o = 10;
    auto put = [&](const char* s) {
        for (const char* p = s; *p && o + 6 < sizeof buf; ++p, ++o) {
            char c = *p;
            buf[o] = (c >= 'a' && c <= 'z') ? char(c - 'a' + 'A')
                   : ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) ? c : '_';
        }
    };
    if (ns && *ns) { put(ns); if (o + 6 < sizeof buf) buf[o++] = '_'; }
    put(name);
    buf[o++] = '_'; buf[o++] = 'D'; buf[o++] = 'I'; buf[o++] = 'R'; buf[o] = 0;
    return env_or(buf);
}
inline const char* xpkg_dir(const char* name) { return xpkg_dir("", name); }

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

    // The interface's LANGUAGE, stated rather than inferred from its extension.
    //
    // ⚠️ Measured on macOS CI: a rule package whose lib root is `rulepkg.ixx`
    // made `clang++ --precompile rulepkg.ixx -o rulepkg.pcm` EXIT 0 AND WRITE
    // NOTHING — clang's driver does not recognise `.ixx`, so it treated the file
    // as a linker input, warned that it was unused, and succeeded. The failure
    // surfaced one step later as `no such file or directory: …/rulepkg.pcm`,
    // naming an output rather than the input that was never read.
    //
    // Every other module compile in mcpp already says this (BmiTraits::
    // moduleInterfaceLangFlag — `/interface /TP`, `-x c++-module`, `-x c++`);
    // the host-module path was the one place that still let the driver guess.
    // It is positional on GNU-style drivers, so it goes immediately before the
    // input.
    std::vector<std::string> langArgv;
    {
        std::string_view lang = traits.moduleInterfaceLangFlag;
        for (std::size_t i = 0; i < lang.size(); ) {
            while (i < lang.size() && lang[i] == ' ') ++i;
            auto j = lang.find(' ', i);
            if (j == std::string_view::npos) j = lang.size();
            if (j > i) langArgv.emplace_back(lang.substr(i, j - i));
            i = j;
        }
    }

    if (mcpp::toolchain::is_clang(tc)) {
        fs::path pcm = bdir / (stem + std::string(traits.bmiExt));
        std::vector<std::string> pre{compiler.string(), stdFlag, "--precompile"};
        for (auto const& l : langArgv) pre.push_back(l);
        pre.push_back(interfacePath.string());
        pre.push_back("-o"); pre.push_back(pcm.string());
        if (auto r = run(with_base(std::move(pre)), "precompile"); !r)
            return std::unexpected(r.error());
        // The precompile can succeed and write nothing when the driver ignored
        // the input, which is exactly what happened above. Checked here so the
        // diagnostic names the interface rather than a missing output.
        if (!fs::exists(pcm, ec)) {
            return std::unexpected(std::format(
                "host module '{}': the compiler accepted '{}' and produced no "
                "BMI.\n"
                "       The interface's language is passed explicitly, so this "
                "is not an extension\n"
                "       the driver failed to recognise — check that the file "
                "really is a module interface.",
                logicalName, interfacePath.string()));
        }
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
    std::vector<std::string> gccArgv{compiler.string(), stdFlag, "-fmodules", "-c"};
    for (auto const& l : langArgv) gccArgv.push_back(l);
    gccArgv.push_back(interfacePath.string());
    gccArgv.push_back("-o"); gccArgv.push_back(out.object.string());
    if (auto r = run(with_base(std::move(gccArgv)), "compile"); !r)
        return std::unexpected(r.error());
    out.useFlags = {"-fmodules"};
    return out;
}

} // namespace mcpp::build
