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
//
// WHAT IS LEFT HERE IS THE TEXT, AND THE COMPILE OF IT IS ELSEWHERE. The bundled
// module's source and the question "does this program import that module" stay;
// the text is written as a package of the build programs' graph by
// mcpp.build.engine_package, and compiled by that graph's plan (mcpp#797).

export module mcpp.build.hostprogram;

import std;

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
// Emit the executable as an ABSOLUTE path. A bare name resolves through
// PATH to a shim that dispatches against its OWNER home, which is not
// necessarily the home this build uses; measured in CI as
// `xlings: 'qemu-system-riscv64' is not installed` from a job where the same
// bare name had answered `--version` two steps earlier. `xpkg_dir()` is how a
// package finds the payload it declared.
//
// Exactly one dependency may supply this. Two board-support packages both
// claiming to know how to run the artifact is a configuration error, and mcpp
// reports it naming both rather than merging them.
inline void runner(const char* token)             { std::printf("mcpp:runner=%s\n", token); }
// A NAMED way of reaching the artefact. The engine learns the name from
// here and knows nothing else about it, so `flash`, `serve`, `deploy`,
// `submit` and `logcat` cost the same: nothing.
//
// One token per call, because argv is ordered and a single string cannot say
// where its boundaries are. The user reaches it with `mcpp run --runner <name>`.
inline void runner(const char* name, const char* token) {
    std::printf("mcpp:runner-named=%s:%s\n", name, token);
}
// This named runner has no natural end — a console monitor, a debug server.
// DECLARED RATHER THAN DERIVED FROM THE NAME: the engine has no list of
// names to derive it from, which is the point.
inline void runner_longlived(const char* name) {
    std::printf("mcpp:runner-longlived=%s\n", name);
}
inline void run_exclusive()                    { std::printf("mcpp:run-exclusive=1\n"); }

// Say something to the user and keep going.
//
// THIS IS THE ONLY WAY A BUILD PROGRAM CAN SUCCEED AND STILL BE HEARD.
// mcpp prints what it captured from a build program only when that program
// EXITS NON-ZERO, so a `std::printf` or `std::fprintf(stderr, ...)` note is
// invisible on precisely the successful builds that needed it.
//
// Use it for a condition the program handled correctly but the user would
// want to know about — most often "I could not find X, so I configured
// nothing that depends on it". Do not use it for an error: exit non-zero
// instead, and the output is printed already.
//
// It survives the build cache. A cached run does not re-execute the
// program, and an advisory that appeared once and then vanished would read as
// "resolved". mcpp replays it on every hit.
inline void warning(const char* message)          { std::printf("mcpp:warning=%s\n", message); }
// A structured diagnostic (#734 E11, protocol 14), rendered by the engine in
// its own form: the message, then `impact:` and `hint:` lines, in the JSON
// stream with the same fields, and replayed on a cached run as `warning` is.
// `severity` is "note", "warning" or "degraded" (a degradation fails the
// build under `--strict`). `warning(message)` stays, and equals a diagnostic
// with a message only.
struct diagnostic {
    const char* severity = "warning";
    const char* message  = "";
    const char* impact   = "";
    const char* hint     = "";
};
// Output goes through printf only: an inline function of this interface that
// names `stdout` or `fputc` carries `FILE` into the module, and GCC then
// rejects a build program that includes <cstdio> after `import mcpp;`.
inline void diagnostic_field_(const char* s, char end) {
    for (const char* p = s ? s : ""; *p; ++p)
        std::printf("%c", (*p == '\t' || *p == '\n' || *p == '\r') ? ' ' : *p);
    std::printf("%c", end);
}
inline void report(const diagnostic& d) {
    std::printf("mcpp:diagnostic=");
    diagnostic_field_(d.severity, '\t');
    diagnostic_field_(d.message,  '\t');
    diagnostic_field_(d.impact,   '\t');
    diagnostic_field_(d.hint,     '\n');
}

// ── The probe channel (mcpp 2026.9.5.2+) ────────────────────────────────────
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
// A fact is persisted with the program's other output and replayed on a
// cache hit. State what would change it -- `rerun_if_changed` on the library
// the version was read from -- or the fact outlives the machine it described.
inline void fact(const char* name, const char* version) {
    std::printf("mcpp:fact=%s=%s\n", name, version);
}
inline void floor(const char* spec)               { std::printf("mcpp:floor=%s\n", spec); }

// ── The distributable channel (mcpp 2026.9.11.1+) ───────────────────────────
//
// `mcpp pack --format <name>` dispatches to whichever package provides
// `<name>`, exactly as `--target` reaches a triple the engine did not have to
// know individually. This is how a package says which name it answers for:
//
//     mcpp::provides_pack_format("appimage");
//
// DECLARE UNCONDITIONALLY, SUBMIT CONDITIONALLY. The declaration must not be
// gated on `pack_format()`, and the reason is that the engine has to be able to
// answer a question the requesting build cannot: `mcpp pack --format bogus`
// names what IS available, and `--help` says "plus any format the resolved
// graph provides". Both read the set collected from this outlet, on a build
// that asked for nothing. A member that declared only when asked would still
// work for its author -- they always pass their own format -- and would make
// the set unknowable for everyone else.
//
// The work itself is the other half:
//
//     if (std::string_view(mcpp::pack_format()) == "appimage") { ... submit ... }
//
// Two names are reserved for the engine's own archive shapes and are refused
// here: `tar` and `dir`.
inline void provides_pack_format(const char* name) {
    std::printf("mcpp:pack-format=%s\n", name);
}

// The memory layout for a freestanding link. Reaches the CONSUMER's link line
// (like link_lib/link_search, unlike include_dir), because the package that
// knows a board's layout is not the package being built.
inline void link_script(const char* path)         { std::printf("mcpp:link-script=%s\n", path); }
// A linker flag this program COMPUTED. The outlet `link_lib` / `link_search` /
// `link_script` leave open: a generated version script, `--wrap`,
// `--exclude-libs`. Reaches the consumer's link line, as `[build] ldflags`
// already does -- see the table row for why a private form is not offered.
inline void link_flag(const char* flag)           { std::printf("mcpp:link-flag=%s\n", flag); }
// The symbol every image linked over this package is entered at (protocol
// 16): what a board's startup code defines. Reaches the consumer's link, as
// `link_script` does, and is checked there against the symbol the program or
// its entry states; an image is entered at one symbol.
inline void link_start(const char* symbol)        { std::printf("mcpp:link-start=%s\n", symbol); }
// THE PLATFORM'S LINK, STATED BY A PROGRAM (protocol 17, SPEC-013 §4.1): the
// same fields as `[platform.link]`, for the part a board or a platform plugin
// computes. Each reaches the consumer's link, as `link_script` does.
//
// `link_driver("lld")`: `ld.lld` itself links, with the flags `link_flag`
// states; "compiler" (the default) leaves the link to the compiler driver.
inline void link_driver(const char* driver)       { std::printf("mcpp:link-driver=%s\n", driver); }
// A file linked into every executable and shared library (assembled first when
// it is a `.S`/`.s`).
inline void link_input(const char* path)          { std::printf("mcpp:link-input=file:%s\n", path); }
// The standard output of a program, saved as `as` and linked in the same way;
// one token of its argv per call, in order (`mcpp::link_input_from("notes.S",
// "okld")`, then `"note-source"`).
inline void link_input_from(const char* as, const char* token) {
    std::printf("mcpp:link-input=from:%s:%s\n", as, token);
}
// How an artifact is named: `kind` is "exe" or "shared", `pattern` names it
// with `{name}` ("{name}.oke", "lib{name}.okso").
inline void artifact_name(const char* kind, const char* pattern) {
    std::printf("mcpp:artifact-name=%s:%s\n", kind, pattern);
}
// One token of seal step `step` (0, 1, ...): steps run in order on the linked
// file (`{}`) before it is published, `{needed}` naming the shared artifacts
// it needs. `mcpp::seal(0, "okld"); mcpp::seal(0, "check"); mcpp::seal(0, "{}");`
inline void seal(int step, const char* token)     { std::printf("mcpp:seal=%d:%s\n", step, token); }
// Where the runner runs the artifact: "same-arch" (a machine of its
// architecture) or "any" (an emulator or a probe reaches it from any machine).
inline void run_host(const char* where)           { std::printf("mcpp:run-host=%s\n", where); }
// The PE subsystem ("console" | "windows") and the entry function ("main" |
// "wmain" | "WinMain" | "wWinMain") of an executable target of THIS package,
// named by `target` (#618). The same fields as `[targets.<name>]
// windows_subsystem` / `windows_entry`: they reach that target's link and no
// other, and are inert on every target that is not PE.
inline void windows_subsystem(const char* target, const char* value) {
    std::printf("mcpp:windows-subsystem=%s:%s\n", target, value);
}
inline void windows_entry(const char* target, const char* value) {
    std::printf("mcpp:windows-entry=%s:%s\n", target, value);
}
// A file THIS PROGRAM produced or selected, placed beside the artifact at a
// path relative to the executable's directory (#622 A4) — the build-program
// form of `[runtime] deploy` (docs/04). `from` may be absolute (an action's
// own declared output; see mcpp::action) or relative to this package's root;
// `to` follows the manifest key's own rule: `/`-separated, no `..`
// component, and `"."` names the executable's own directory. TAB-separated
// on the wire because an absolute `from` is a Windows path on that platform,
// and `windows_subsystem`'s `:` splitter would misparse `C:\...`.
inline void deploy(const char* from, const char* to) {
    std::printf("mcpp:deploy=%s\t%s\n", from, to);
}
// The build-program form of `runtime_search_dirs` (docs/04 §2.11): a
// directory to search at LAUNCH time, for a dependency (a vcpkg prefix's
// `bin/`, a Qt SDK's `bin/`, a directory a `prepare` action populates) whose
// location this program learns rather than one an author can write into
// `mcpp.toml`. Reaches the consumer, joining the SAME `LinkIntent` field the
// manifest key populates directly -- not the retiring `[runtime]
// library_dirs` -- and gets the same treatment: RUNPATH/rpath on ELF and
// Mach-O, never `-L`, and `mcpp pack`'s closure search. Relative paths
// resolve against this package's root, like every other AbsPath directive.
// The directory need not exist when this program runs: a `prepare` action
// (below) may populate it later, at build time.
inline void runtime_search_dir(const char* dir) {
    std::printf("mcpp:runtime-search-dir=%s\n", dir);
}
// The five action roles (mcpp#702), as CONSTANTS rather than bare string
// literals: `a.role = mcpp::roles::prepare` fails to COMPILE on an engine
// whose bundled module has no such name, instead of reaching that engine's
// decoder as the plain string "prepare", which an engine older than protocol
// 12 reads as Source (see `decode_action`, modules/buildmcpp/src/
// directives.cppm). Spelled inside THIS module, not the engine's own headers,
// because these names have to be visible from a build.mcpp translation unit,
// which imports this module and nothing else of the engine's. SPEC-007 R3.6
// makes writing the constant the author's obligation; the engine still
// accepts the bare string from a hand-written frozen-surface program (protocol
// 0), and refuses one that names none of the five (`action_error`).
namespace roles {
    inline constexpr const char* source   = "source";
    inline constexpr const char* check    = "check";
    inline constexpr const char* object   = "object";
    inline constexpr const char* artifact = "artifact";
    // Construction whose file names are not known when this program runs:
    // installing a vcpkg manifest or a CMake subproject into a prefix,
    // unpacking an SDK. See `action::output_dir` below for its contract.
    inline constexpr const char* prepare  = "prepare";
}
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
    const char* role        = roles::source; // one of `roles::{source, check, object, artifact, prepare}`
    const char* description = "";
    bool        blocking    = false;      // check only: gate compilation on it
    // A Make-style dependency file the COMMAND writes as a side effect (gcc/
    // clang `-MD -MF`, glslangValidator `--depfile`, glslc `-MD -MF`, slangc
    // `-depfile`). Empty (the default) means the rule emits none, and the
    // action's re-run set is exactly its declared `inputs`, as before this
    // field existed. See BuildAction::depfile (modules/manifest/src/types.cppm)
    // for why `inputs` alone cannot express what this covers.
    const char* depfile     = "";
    action& input(const char* p)    { add(inputs_,  p); return *this; }
    action& output(const char* p)   { add(outputs_, p); return *this; }
    action& arg(const char* a)      { add(command_, a); return *this; }
    // Declare what a generated MODULE INTERFACE provides/imports. Same
    // "declare instead of discover" trade [modules].scan_overrides makes, and
    // what lets a generated .cppm exist as a graph node at all.
    action& provides(const char* n) { add(provides_, n); return *this; }
    action& imports(const char* n)  { add(imports_,  n); return *this; }
    // Object only: which link unit receives the outputs. Omit for "every image
    // this package produces" — which INCLUDES test binaries, and is what you
    // want: their names come from tests/*.cpp, so spelling one here breaks
    // plain `mcpp build`, where that link unit does not exist. An Artifact reads
    // its target out of ${mcpp.target_file:NAME}; an Object runs before the link
    // and has no such handle, so it has to say the name.
    action& target(const char* n)   { add(targets_,  n); return *this; }
    // `prepare` only: the directory the COMMAND populates, which the build
    // reads BY DIRECTORY (`include_dir`, `link_search`, `runtime_search_dir`)
    // rather than by naming files -- the whole reason this role exists (O1,
    // mcpp#702). One directory, not a list like `output()`: the post-condition
    // the engine checks after the command succeeds (does this directory now
    // exist, whether or not the command created it) is a single answer, and a
    // `prepare` action with no `output_dir` is refused (`action_error`) as a
    // `check` that forgot to declare what it built.
    action& output_dir(const char* p) { outputDir_ = p; return *this; }
    // An environment variable for the COMMAND (protocol 13, mcpp#708), added
    // to the environment the build already passes on. The command is an argv
    // with no shell, so `NAME=value cmd` is not available to write; this is.
    // Changing a value changes the edge's command, so the action re-runs.
    action& env(const char* name, const char* value) {
        add(env_, name, value);
        return *this;
    }
    // The directory the COMMAND runs in (protocol 13, mcpp#708). Relative to
    // this package's root; the default is the build directory. Declared
    // inputs and outputs are unaffected: they keep naming files the way they
    // always have.
    action& cwd(const char* dir) { cwd_ = dir; return *this; }
    void submit() const {
        std::printf("mcpp:action={\"id\":");        esc(id);
        std::printf(",\"role\":");                  esc(role);
        std::printf(",\"description\":");           esc(description);
        std::printf(",\"blocking\":%s", blocking ? "true" : "false");
        // Optional and omitted rather than sent empty: an action that never
        // sets this must serialise to the SAME bytes it did before the field
        // existed, because this payload is the cache key `apply()` stores
        // verbatim (see the comment there) — an unconditional `"depfile":""`
        // on every action would perturb the cache for every build.mcpp that
        // has nothing to do with depfiles. The decoder's default (empty
        // string) is identical either way, so omission costs nothing on read.
        if (depfile[0]) { std::printf(",\"depfile\":"); esc(depfile); }
        // Same omission rule as `depfile`, for the same reason: an action
        // that never calls `output_dir()` serialises to the same bytes it did
        // before the method existed.
        if (outputDir_[0]) { std::printf(",\"output_dir\":"); esc(outputDir_); }
        // Same omission rule again: an action that sets neither serialises to
        // the bytes it did before protocol 13.
        if (env_.len) std::printf(",\"env\":[%s]", env_.c_str());
        if (cwd_[0]) { std::printf(",\"cwd\":"); esc(cwd_); }
        // Set only when the process could not allocate memory for a list.
        // A declaration cut short would otherwise be INVALID rather than
        // obviously wrong -- the engine turns this marker into a diagnostic
        // that names the cause, instead of a generic "malformed action".
        if (overflow_) std::printf(",\"overflow\":true");
        std::printf(",\"inputs\":[%s]",   inputs_.c_str());
        std::printf(",\"outputs\":[%s]",  outputs_.c_str());
        std::printf(",\"command\":[%s]",  command_.c_str());
        std::printf(",\"provides\":[%s]", provides_.c_str());
        std::printf(",\"imports\":[%s]",  imports_.c_str());
        std::printf(",\"targets\":[%s]",  targets_.c_str());
        std::printf("}\n");
    }
private:
    // One list field, held already serialised (`"a","b"`) so submit() prints
    // it as it is. Owning and std-free, and both words are constraints this
    // module carries: it may be compiled before a std BMI exists, so it must
    // not `import std;`, and its exported interface must name no std type, so
    // `std::string` may not appear in a signature. Neither forbids the heap:
    // storage is `realloc` from the `<cstdlib>` already in the global module
    // fragment, and no exported signature mentions this type.
    //
    // An earlier revision held six fixed arrays (8192 bytes for `inputs` and
    // `outputs`, chosen for a protoc command line) and a declaration that did
    // not fit was refused. The bound was in bytes of serialised JSON, so a
    // consumer's checkout depth decided whether a resource list of 44 files
    // fit (HuxerUI#130 measured the margin at 45 bytes), and `outputs` is the
    // one list an author cannot shorten: an output the program does not name
    // cannot be built, and there is no depfile for outputs. See
    // .agents/docs/2026-09-13-four-upstream-asks-from-a-ui-framework.md.
    struct list {
        char* p = nullptr;
        unsigned long len = 0, cap = 0;
        list() = default;
        list(const list& o) { take(o); }
        list& operator=(const list& o) { if (this != &o) { len = 0; take(o); } return *this; }
        ~list() { std::free(p); }
        const char* c_str() const { return p ? p : ""; }
        // Grows by doubling. False only when the allocator refuses.
        bool reserve(unsigned long need) {
            if (need <= cap) return true;
            unsigned long c = cap ? cap : 256;
            while (c < need) c *= 2;
            void* q = std::realloc(p, c);
            if (!q) return false;
            p = static_cast<char*>(q);
            cap = c;
            return true;
        }
        bool put(char c) {
            if (!reserve(len + 2)) return false;
            p[len++] = c;
            p[len] = 0;
            return true;
        }
        void take(const list& o) {
            if (!o.len) { if (p) p[0] = 0; return; }
            if (!reserve(o.len + 1)) return;
            for (unsigned long i = 0; i <= o.len; ++i) p[i] = o.p[i];
            len = o.len;
        }
    };
    list inputs_, outputs_, command_, provides_, imports_, targets_, env_;
    // `prepare` only: see `output_dir()` above. A plain `const char*`, not a
    // `list`: it is one directory, never a JSON array.
    const char* outputDir_ = "";
    const char* cwd_ = "";
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
    // Appends one JSON string literal, with the escaping `esc` applies, so a
    // list entry and a scalar field are encoded by one rule. A payload that
    // decoded under the fixed-array revision is encoded to the same bytes
    // here: that revision escaped `"` and `\\` and passed control characters
    // through, and a control character passed through was not JSON, so no
    // payload the engine accepted contained one.
    // `value`, when given, is appended to `s` after an `=`, inside the same
    // string literal: one `env` entry is one `NAME=value` string.
    bool add(list& l, const char* s, const char* value = nullptr) {
        bool ok = true;
        if (l.len) ok = ok && l.put(',');
        ok = ok && l.put('"');
        auto body = [&](const char* text) {
            for (const char* p = text; ok && *p; ++p) {
                unsigned char c = (unsigned char)*p;
                if (c == '"' || c == '\\') { ok = l.put('\\') && l.put((char)c); continue; }
                if (c < 0x20) {
                    static const char hex[] = "0123456789abcdef";
                    ok = l.put('\\') && l.put('u') && l.put('0') && l.put('0')
                      && l.put(hex[c >> 4]) && l.put(hex[c & 0xf]);
                    continue;
                }
                ok = l.put((char)c);
            }
        };
        body(s);
        if (value) { ok = ok && l.put('='); body(value); }
        ok = ok && l.put('"');
        if (!ok) overflow_ = true;
        return ok;
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
// This exists so a package never has to DECLARE a toolchain. A package that
// needs headers the toolchain ships (libc++'s, for a freestanding standard
// library subset) previously had to put `xim:llvm` in `[xlings] deps`, which
// pinned it to one implementation — and the measured fact is that the same
// subset works over libstdc++'s freestanding mode too, so pinning was not
// merely inelegant, it closed a road. Asking here follows whatever
// `[toolchain]` actually resolved.
inline const char* toolchain_dir()                { return env_or("MCPP_TOOLCHAIN_DIR"); }

// The two flags mcpp passes to its own compiler: the `--sysroot` and the
// directory it names with `-B`. Either is empty when mcpp passes none.
//
// These are for A SECOND COMPILER — one this rule package runs and mcpp did
// not resolve. Such a compiler starts with no idea where anything is, and on a
// subos the C library is not at `/usr/include` and the assembler is not at
// `/usr/bin`; the first `#include` it reaches then fails on `features.h`.
// Forwarding these two makes it see what mcpp's own compiler sees.
//
// NOT `sysroot_dir()`, four lines down. That one answers a question about
// the TARGET's tier and is empty on a hosted target, which is exactly the case
// this pair exists for.
inline const char* toolchain_sysroot()            { return env_or("MCPP_TOOLCHAIN_SYSROOT"); }
inline const char* toolchain_binutils_dir()       { return env_or("MCPP_TOOLCHAIN_BINUTILS_DIR"); }

// The pkg-config search path of the payloads mcpp installed: the registry
// SubOS's `usr/lib/pkgconfig` and `usr/share/pkgconfig`, joined with the
// platform's path-list separator, for a build program that runs `pkg-config`
// over a library a payload provides (`PKG_CONFIG_LIBDIR=<this> pkg-config
// --cflags --libs gtk4`). Payload recipes declare their `.pc` files into that
// view, so it resolves a payload's whole pkg-config closure.
//
// AN ACCESSOR AND NOT AN ENVIRONMENT DEFAULT: a package that means the host's
// own pkg-config database keeps it, and one that means the payloads says so.
inline const char* pkg_config_libdir()            { return env_or("MCPP_PKG_CONFIG_LIBDIR"); }

// Which compiler resolved: "gcc", "clang", "msvc", or "" if none did.
//
// Ask this rather than inferring it from `toolchain_dir()`. The two questions
// a package has actually needed it for are which runtime library holds the
// routines the compiler emits calls to, and which spelling of a binutils tool
// exists beside the driver — and both have a different right answer per family
// rather than per version or per payload.
inline const char* compiler()                     { return env_or("MCPP_COMPILER"); }

// Which C++ standard library resolved: "libstdc++", "libc++", "msvc-stl", or
// "" if no toolchain did.
//
// A SEPARATE QUESTION FROM `compiler()`, and the reason this exists. clang
// links libc++ on one machine and libstdc++ on another and reports "clang"
// either way, while the two implementations differ in what they accept: a
// `unique_ptr` to an incomplete type destroyed in a header compiles under
// libstdc++ and does not under libc++. A package that must refuse such a
// configuration by name cannot ask `compiler()`, because that answer would
// also refuse the configuration that works.
//
// "cxx" is in the name deliberately. `MCPP_TARGET_LIBC` is the C library; this
// is the C++ one, and in an ecosystem that names glibc and musl constantly the
// two must not share a word.
inline const char* cxx_stdlib()                   { return env_or("MCPP_CXX_STDLIB"); }

// The root of the package of the graph that supplies the C++ library
// (`gcc.libstdcxx` beside llvm), "" when the compiler's payload supplies it.
// A rule package that drives a second compiler points it here, so the device
// half and the host half of one program use one C++ library
// (design 2026-10-26 §6.4).
inline const char* cxx_abi_root()                 { return env_or("MCPP_CXX_ABI_ROOT"); }

// Where the TARGET's C library lives, for targets that have one of their own
// (today: bare metal). Same argument one line up: the libc is a property of
// the target, mcpp resolves it from the target's own row, and a package that
// needs to name a FILE inside it (a linker script) asks rather than declares.
//
// Empty on a hosted target — there the libc comes with the compiler payload or
// through the runtime binding, and nothing has to look for it.
inline const char* sysroot_dir()                  { return env_or("MCPP_TARGET_SYSROOT"); }

// ── The build information of the resolved toolchain (#734 E2, protocol 14) ──
//
// Facts, never interpretations: which tools this row runs, which tools are the
// target ABI's own, the environment the engine runs them with, and the C++
// runtime contract the program compiles with. A plugin that drives CMake,
// vcpkg, Meson or make translates them for that system; mcpp knows none of
// them. Every value is empty when it does not apply, and a role is one of
// "cc", "cxx", "ld", "ar", "rc", "as", "mt".
inline const char* build_info_key_(const char* prefix, const char* role) {
    static char name[64];
    int n = 0;
    for (const char* p = prefix; *p && n < 48; ++p) name[n++] = *p;
    for (const char* p = role;   *p && n < 63; ++p)
        name[n++] = (*p >= 'a' && *p <= 'z') ? static_cast<char>(*p - 'a' + 'A') : *p;
    name[n] = '\0';
    return name;
}
// The row's tool for a role: the driver on GNU-style rows (it links and
// assembles), the toolset's own tools on the cl.exe row.
inline const char* tool(const char* role)          { return env_or(build_info_key_("MCPP_TOOL_", role)); }
// The target ABI's native tool: on the MSVC ABI `cl`, `link`, `lib`, `rc`,
// `ml64` and `mt` of the resolved toolset and SDK, whichever driver the row
// uses; elsewhere the same as `tool(role)`.
inline const char* abi_tool(const char* role)      { return env_or(build_info_key_("MCPP_ABI_TOOL_", role)); }
// The environment the engine runs the ABI's tools with, one KEY=value per
// line (INCLUDE, LIB, PATH, ... on the MSVC ABI); empty elsewhere.
inline const char* tool_env()                      { return env_or("MCPP_TOOL_ENV"); }
// A path-free identity of the toolset, for keying a cache by version:
// "msvc 14.44.35207; sdk 10.0.26100.0", "clang 22.1.8", "gcc 16.1.0".
inline const char* toolset_identity()              { return env_or("MCPP_TOOLSET_IDENTITY"); }
// The Visual Studio instance the MSVC toolset belongs to, when it came from
// one; empty for a managed toolset and off the MSVC ABI.
inline const char* msvc_instance_dir()             { return env_or("MCPP_MSVC_INSTANCE_DIR"); }
// The ninja mcpp itself runs, so a foreign build system needs none on PATH.
inline const char* ninja_program()                 { return env_or("MCPP_NINJA"); }
// The program's C++ runtime contract: "self-contained", "toolchain-coupled"
// or "host-coupled".
inline const char* cxx_runtime()                   { return env_or("MCPP_CXX_RUNTIME"); }
// On the MSVC ABI the CRT the program compiles with, "static" (/MT) or
// "dynamic" (/MD); empty elsewhere. The value `place-dlls --crt` reads.
inline const char* msvc_crt_linkage()              { return env_or("MCPP_MSVC_CRT_LINKAGE"); }

// ── Three answers a board-support package would otherwise hardcode ───────────
//
// The coupling these remove does not appear in any manifest. A board package
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
// THE PACKAGE THIS PROGRAM IS BUILDING, BY NAME.
//
// A rule package that generates a consumer-facing declaration has to name it,
// and every name it produces is derived from this one: the module a project
// imports, the namespace the accessors sit in, the symbols in a generated
// header. Before these existed the closest thing available was the leaf of
// `manifest_dir()`, which is a directory name rather than a package name --
// so a package called `vulkan-saxpy` in a directory called `app` generated
// `app.shaders`, and every `<something>/app/` in a workspace claimed it.
//
// Empty under an engine older than 2026.9.7.1, which a rule reads as "fall
// back to whatever you did before". That is what keeps an already-published
// rule package working unchanged.
inline const char* package_name()                 { return env_or("MCPP_PKG_NAME"); }
inline const char* package_namespace()            { return env_or("MCPP_PKG_NAMESPACE"); }

// THE REST OF `[package]`, BECAUSE A DISTRIBUTABLE CARRIES IT.
//
// `package_name()` above exists so a generated declaration can be named. These
// exist for the other member of the collection: every installer format states a
// version, and most state a description, a licence and a maintainer. A member
// without them has to ask the PROJECT to restate values mcpp has already
// parsed, in the member's own options, where the copy drifts from `[package]`
// and nothing can detect that it has.
//
// `package_authors()` is a ';'-separated list -- not ',', because an author is
// conventionally `Name <mail@host>` and a name may carry a comma.
//
// Empty under an engine older than 2026.9.11.1. A member that needs one must
// say so itself when it is empty, naming the value it wanted: only the member
// knows whether the absence is fatal.
inline const char* package_version()              { return env_or("MCPP_PKG_VERSION"); }
// THE PROJECT'S FLOOR FOR THIS TRIPLE, IN THE PLATFORM'S OWN WORDS (#622
// A11): `14.0` on macOS, `18.0` on iOS, an API level on Android, empty
// everywhere else. One function on the engine side, `min_platform_version`,
// already answers this for the compiler flag and the fingerprint slot; before
// this a member (`dist-apple`'s `minimum_system_version`, `dist-apk`'s
// `minSdkVersion`) had no channel to it and restated the value in its own
// options, where it silently drifted from the manifest's actual answer.
// Empty under an engine older than this, which a member reads as "restate the
// value yourself", the behaviour every consumer had before.
inline const char* min_platform_version()         { return env_or("MCPP_TARGET_MIN_PLATFORM_VERSION"); }
inline const char* package_description()          { return env_or("MCPP_PKG_DESCRIPTION"); }
inline const char* package_license()              { return env_or("MCPP_PKG_LICENSE"); }
inline const char* package_authors()              { return env_or("MCPP_PKG_AUTHORS"); }
inline const char* package_repo()                 { return env_or("MCPP_PKG_REPO"); }

// WHICH DISTRIBUTABLE THIS PASS WAS ASKED FOR, or "" for every ordinary build.
//
// The empty value is the one that carries the meaning: a member gates its
// submission on this, so `mcpp build` has the graph it always had and a dist
// edge exists only in the pass that wants one. See `provides_pack_format` for
// the half that must NOT be gated.
inline const char* pack_format()                  { return env_or("MCPP_PACK_FORMAT"); }

// WHERE `mcpp pack` HAS ALREADY STAGED THE CLOSURE, absolute; "" when this
// build is not packing.
//
// The tree is what `mcpp pack` computes and then, until this existed, threw
// away: the dependency closure after the strip policy, the debug-symbol split
// and `include`/`exclude`. It is a BUNDLE tree -- `bin/`, `lib/`, relocatable,
// rooted anywhere -- which is what an AppImage, a `.app` and an `.msi` want as
// it stands. A format that wants a root filesystem instead (`.deb`, `.rpm`)
// owns the re-layout, because which directory a file belongs in is that
// format's knowledge and not the engine's.
//
// READ IT HERE TO DECIDE, WRITE `${mcpp.stage_dir}` INTO THE ACTION. The
// directory exists while this program runs, so a member enumerates it to learn
// which of `bin/`, `lib/`, `share/` the tree actually has; the action's command
// then names it through the placeholder, so the path in the graph and the path
// this program read cannot disagree.
inline const char* pack_stage_dir()               { return env_or("MCPP_PACK_STAGE_DIR"); }

// WHETHER THIS PACKAGING PASS STRIPS: "1" or "0", and "" for every ordinary
// build (#649 E5).
//
// The engine strips the program, every shared library the graph built and the
// staged copy of the toolchain's runtime, and `--no-strip` turns all of it off.
// A member that stages libraries of its own -- an Android archive's native
// libraries, say -- asks here instead of deciding for itself, so one switch
// governs every file in the package. An engine older than this one leaves it
// empty, which a member reads as "decide as before".
inline const char* pack_strip()                   { return env_or("MCPP_PACK_STRIP"); }

// Where `--debug-symbols` sends the separated `*.debug` files, absolute; ""
// when they are discarded or this build is not packing (#649 E5).
inline const char* pack_debug_symbols_dir()       { return env_or("MCPP_PACK_DEBUG_SYMBOLS_DIR"); }

// THE RESOLVED DEPENDENCY GRAPH, as a JSON document; "" for a dependency's
// program and under an older engine (#647 E1).
//
// Offered to the ROOT package's program only, for the reason `dep_linkage` is:
// the root decides the graph, and when its program runs every input of that
// decision is final. `packages` lists every package, dependencies before the
// packages that request them, each with `package` (identity), `root`,
// `requested_by`, `link` (for a library), `manifest_dir` (absolute),
// `features`, `targets` and `metadata` (its `[package.metadata]`, verbatim).
// A path inside `metadata` is the reader's to resolve, against that entry's
// `manifest_dir`.
//
// Editing a package's `[package.metadata]` re-runs this program; editing its
// sources does not.
inline const char* graph_file()                   { return env_or("MCPP_GRAPH_FILE"); }
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
//
// #852: a name two visible dependencies answer to (two packages whose
// manifests both say `rules`) is no package's, and the engine publishes it as
// MCPP_DEP_AMBIGUOUS_<SANITIZED_NAME> with the qualified names instead. Asking
// for it ends the program, naming them: the answer otherwise would be one of
// the two packages' directories or none, and either is a wrong build.
inline const char* dep_dir(const char* name) {
    char buf[256] = "MCPP_DEP_";
    unsigned long o = 9;
    for (const char* p = name; *p && o + 5 < sizeof buf; ++p, ++o) {
        char c = *p;
        buf[o] = (c >= 'a' && c <= 'z') ? char(c - 'a' + 'A')
               : ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) ? c : '_';
    }
    char ambiguous[288] = "MCPP_DEP_AMBIGUOUS_";
    unsigned long a = 19;
    for (unsigned long i = 9; i < o && a + 1 < sizeof ambiguous; ++i, ++a) ambiguous[a] = buf[i];
    ambiguous[a] = 0;
    // Said on standard output, which the engine prints when the program exits
    // non-zero; `std::fprintf(stderr, ...)` here would bind `FILE` in this
    // module and collide with a build program's own <cstdio>.
    if (const char* which = std::getenv(ambiguous)) {
        std::printf("build.mcpp: dep_dir(\"%s\") names more than one dependency (%s); ask by one of "
                    "those names\n", name, which);
        std::exit(2);
    }
    buf[o++] = '_'; buf[o++] = 'D'; buf[o++] = 'I'; buf[o++] = 'R'; buf[o] = 0;
    return env_or(buf);
}
// #642 E2: the link form a declared dependency takes in this build, "static" or
// "shared", or "" when it has no library form or the name is unknown. Wraps
// MCPP_DEP_<SANITIZED_NAME>_LINKAGE, under the names dep_dir() answers for.
// Offered to the ROOT package's build program only: the root decides every
// dependency's form, and a dependency's program runs before every fact that
// decides it is known, so there it is always "".
inline const char* dep_linkage(const char* name) {
    char buf[256] = "MCPP_DEP_";
    unsigned long o = 9;
    for (const char* p = name; *p && o + 9 < sizeof buf; ++p, ++o) {
        char c = *p;
        buf[o] = (c >= 'a' && c <= 'z') ? char(c - 'a' + 'A')
               : ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) ? c : '_';
    }
    for (const char* suffix = "_LINKAGE"; *suffix; ++suffix) buf[o++] = *suffix;
    buf[o] = 0;
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
// `MCPP_XPKG_<NS>_<NAME>_<SUFFIX>`, spelled as `xpkg_dir` always spelled it.
inline const char* xpkg_value_(const char* ns, const char* name, const char* suffix) {
    char buf[256] = "MCPP_XPKG_";
    unsigned long o = 10;
    auto put = [&](const char* s) {
        for (const char* p = s; *p && o + 12 < sizeof buf; ++p, ++o) {
            char c = *p;
            buf[o] = (c >= 'a' && c <= 'z') ? char(c - 'a' + 'A')
                   : ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) ? c : '_';
        }
    };
    if (ns && *ns) { put(ns); if (o + 12 < sizeof buf) buf[o++] = '_'; }
    put(name);
    buf[o++] = '_';
    for (const char* p = suffix; *p && o + 1 < sizeof buf; ++p) buf[o++] = *p;
    buf[o] = 0;
    return env_or(buf);
}
inline const char* xpkg_dir(const char* ns, const char* name) { return xpkg_value_(ns, name, "DIR"); }
inline const char* xpkg_dir(const char* name) { return xpkg_dir("", name); }

// WHERE A DECLARED PAYLOAD COMES FROM (protocol 15, mcpp#755).
//
//   "payload"   installed from the registry; `xpkg_dir` names it
//   "override"  stated by `[xlings.overrides]`, `MCPP_XLINGS_OVERRIDE_<NS>_<NAME>`
//               or the global configuration; nothing was installed, and
//               `xpkg_dir` names the root the override implies
//   "pending"   declared `provision = "on-request"` and not installed yet;
//               `xpkg_request` asks for it
//   ""          not declared for this build, or not installed
inline const char* xpkg_source(const char* ns, const char* name) { return xpkg_value_(ns, name, "SOURCE"); }
inline const char* xpkg_source(const char* name) { return xpkg_source("", name); }
// The program an override named, when it named one (`program = "..."`, a
// path that is a file, or a name found on PATH); "" otherwise. A payload, and
// an override that named a root, leave the program to the plugin's layout.
inline const char* xpkg_program(const char* ns, const char* name) { return xpkg_value_(ns, name, "PROGRAM"); }
inline const char* xpkg_program(const char* name) { return xpkg_program("", name); }

inline bool& xpkg_pending_flag_() { static bool pending = false; return pending; }
// The directory of a payload this program needs, installing it on request.
//
// A payload declared `provision = "on-request"` is not installed before build
// programs run. This answers like `xpkg_dir` when the payload is installed or
// overridden. When it is pending, it asks the engine for it and answers "";
// `xpkg_pending()` is then true, and the program should return without
// configuring what needs the payload: the engine installs every payload asked
// for in one batch, discards this run, and runs the program again, which then
// receives the directory. A plugin that names its own tool never calls this,
// so its payload is never installed.
inline const char* xpkg_request(const char* ns, const char* name) {
    const char* dir = xpkg_dir(ns, name);
    if (*dir) return dir;
    const char* src = xpkg_source(ns, name);
    if (src[0] == 'p' && src[1] == 'e') {   // "pending"
        std::printf("mcpp:xpkg-request=%s:%s\n", (ns && *ns) ? ns : "xim", name);
        xpkg_pending_flag_() = true;
    }
    return "";
}
inline bool xpkg_pending() { return xpkg_pending_flag_(); }

// Which phase is running: "toolchain" while the root build program states the
// build toolchain (`[toolchain] <key> = { configure = "build.mcpp" }`), "build"
// otherwise. A program in the toolchain phase states only the toolchain.
inline const char* phase() {
    const char* p = env_or("MCPP_PHASE");
    return *p ? p : "build";
}

// THE SOURCE OF A TOOL A PLUGIN RUNS, recorded with the build's other sources.
//
// `subject` names the tool (`tool:<module>:<name>`), `from` how it was found
// (`choice` -- named by the build program; `env` -- by an environment variable
// the plugin reads; `override` -- by an engine override; `payload` -- the
// declared payload; `path` -- found on PATH), `value` the program, and
// `file`/`line` the statement that chose it when the build program did, and
// `payload` the declared payload (`<ns>:<name>`) the tool stands for, so an
// override or a payload is reported with that payload's own source. Fields
// must not contain a tab or a newline.
inline void decision(const char* subject, const char* from, const char* value,
                     const char* file = "", unsigned line = 0, const char* payload = "") {
    std::printf("mcpp:decision=%s\t%s\t%s\t%s\t%u\t%s\n", subject, from, value, file,
                line, payload);
}

// One key of the build toolchain, stated in the toolchain phase: `spec` (a
// managed spec such as "llvm@23.1.3"), or `path`, `prefix`, `sysroot`,
// `family`, `launcher`, `tool.<role>` -- the keys of a `[toolchain]` table --
// and `origin` (`<file>:<line>` of the statement).
inline void toolchain(const char* key, const char* value) {
    std::printf("mcpp:toolchain=%s=%s\n", key, value);
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


// The unit that makes `import mcpp.core;` and `import mcpp;` name one
// interface. Written beside `mcpp.cppm` and compiled after it, so either
// spelling -- or both in one program -- reaches the same symbols. Placeholders
// for the keywords, for the reason `kMcppModuleSource` gives: mcpp's own
// line-based scanner must not read this literal as a second module of this file.
inline constexpr std::string_view kMcppCoreAliasSource =
    "@MODULE@ mcpp.core;\n@EXPORT@ import mcpp;\n";

} // namespace mcpp::build
