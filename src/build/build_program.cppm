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
import mcpp.diag;   // structured diagnostics of build programs (#734 E11)
import mcpp.home;      // cache_root — the global home of the engine's compiled module (#748)
import mcpp.log;       // the compile of a build program is timestamped in the verbose log
import mcpp.manifest;
import mcpp.effects;
import mcpp.runtime.launch_env;
import mcpp.run;
import mcpp.pm.mangle;        // imported_module_names -- what build.mcpp asks for
import mcpp.toolchain.cppfly;        // std_flag (dialect- and c++fly-aware -std= spelling)
import mcpp.toolchain.dialect;       // CommandDialect — gnu vs cl.exe spellings
import mcpp.toolchain.fingerprint;   // hash_file / hash_string (FNV-1a, 16 hex)
import mcpp.build.cmdlimits;
import mcpp.build.directives;        // the directive definition table (own module: see its header)
import mcpp.build.progress;          // the program's line (build progress design 2026-09-29)
import mcpp.build.refusal;           // the machine-readable identity of a refusal
import mcpp.build.hostprogram;       // the bundled `mcpp` module's text (own module: see its header)
import mcpp.build.resources;         // compile_utf8_manifest — the build program speaks UTF-8 (#693)
import mcpp.toolchain.hostflags;     // the shared host-compile flag producer
import mcpp.toolchain.linkmodel;     // shared C-library / clang-cfg-bypass model
import mcpp.toolchain.model;         // Toolchain, PayloadPaths, is_clang/is_musl_target/is_mingw_target
import mcpp.toolchain.registry;      // archive_tool
import mcpp.toolchain.stdmod;        // ensure_built — the SAME std BMI the main build uses
import mcpp.toolchain.triple;        // host_triple (MCPP_HOST contract value)
import mcpp.ui;
import mcpp.version;         // MCPP_VERSION — the hint names the engine the reader is on
import mcpp.invocation;

namespace {
// #734 E11: a build program's structured diagnostics, in the engine's own form.
// Called at both sites that report advisories -- the run path and the cache
// hit -- for the reason stated there: a replayed record must say what the run
// said.
void report_stated_diagnostics(std::string_view packageName,
                               const mcpp::build::directives::Directives& d) {
    namespace dirs = mcpp::build::directives;
    for (auto const& sd : dirs::stated_diagnostics(d)) {
        const auto sev = sd.severity == "note"     ? mcpp::diag::Severity::Note
                       : sd.severity == "degraded" ? mcpp::diag::Severity::Degraded
                                                   : mcpp::diag::Severity::Warning;
        mcpp::diag::report(sev, std::format("build.mcpp/{}", packageName),
                           packageName.empty() ? sd.message
                                               : std::format("{}: {}", packageName, sd.message),
                           sd.impact, sd.hint);
    }
}
} // namespace

export namespace mcpp::build {

// Build-program environment contract (G3) — what the running build.mcpp can
// see, mirroring Cargo's env family. Injected as MCPP_* variables into the
// child ONLY (never the calling process), and folded into the cache key so a
// target/profile/feature change re-runs the program.
// #734 E2: the build information a build program reads through
// `mcpp::tool`, `mcpp::abi_tool`, `mcpp::tool_env`, `mcpp::toolset_identity`,
// `mcpp::msvc_instance_dir`, `mcpp::ninja_program`, `mcpp::cxx_runtime` and
// `mcpp::msvc_crt_linkage`. The roles are cc, cxx, ld, ar, rc, as, mt.
inline constexpr std::string_view kBuildInformationKeys[] = {
    "MCPP_TOOL_CC", "MCPP_TOOL_CXX", "MCPP_TOOL_LD", "MCPP_TOOL_AR",
    "MCPP_TOOL_RC", "MCPP_TOOL_AS", "MCPP_TOOL_MT",
    "MCPP_ABI_TOOL_CC", "MCPP_ABI_TOOL_CXX", "MCPP_ABI_TOOL_LD", "MCPP_ABI_TOOL_AR",
    "MCPP_ABI_TOOL_RC", "MCPP_ABI_TOOL_AS", "MCPP_ABI_TOOL_MT",
    "MCPP_TOOL_ENV", "MCPP_TOOLSET_IDENTITY", "MCPP_MSVC_INSTANCE_DIR",
    "MCPP_NINJA", "MCPP_CXX_RUNTIME", "MCPP_MSVC_CRT_LINKAGE",
};

struct BuildProgramEnv {
    // The program belongs to a package the command was asked to build (the
    // root, or a selected member): its line is listed, and the programs of
    // every other package are folded into one (build progress design
    // 2026-09-29, §4.3).
    bool requested = false;
    // The program's package is a registry or git dependency, whose tree sits in
    // the package store: a `rerun_if_changed_glob` pattern leaving it is
    // refused (#766).
    bool sealedPackage = false;
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
    // #734 E2: the build information of the resolved toolchain, keyed by the
    // variable names of `kBuildInformationKeys`. Filled by prepare from the
    // same producers the engine's own command lines read; a key absent here is
    // emitted empty.
    std::map<std::string, std::string> buildInfo;
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
    // The root of the package of the graph that supplies the `c++-abi` layer
    // (`gcc.libstdcxx` beside an llvm compiler), empty when the compiler's own
    // payload supplies it (design 2026-10-26 §6.4). A rule package that drives
    // a second compiler -- a SYCL device compiler -- points it at the same C++
    // library through this, so one process holds one C++ runtime.
    std::string cxxAbiRoot;
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
    // THE PROJECT'S FLOOR FOR THIS TRIPLE, IN THE PLATFORM'S OWN WORDS (#622
    // A11): `14.0` on macOS, `18.0` on iOS, an API level on Android, empty
    // elsewhere. The same `min_platform_version` (prepare.cppm) already
    // computes for the compiler's `--target` flag and the fingerprint slot --
    // this is that answer, handed to the build program instead of restated in
    // a member's own options, where `dist/apple.cppm:140-143` measured it
    // drifting.
    std::string minPlatformVersion;
    // #634, A7: the pkg-config directories of the registry SubOS, the view
    // payloads declare their `.pc` files into. Joined with the platform's
    // path-list separator; empty when no registry was loaded.
    std::string pkgConfigLibdir;
    std::string profile;                    // effective profile name (dev/release/…)
    std::vector<std::string> features;      // active feature closure of the package
    // The device axis of this build, in the wire form `mcpp.pack.abi_tag`
    // reads (`cuda12.9+{sm_89} ptx>=89`); empty when the build asks for no
    // accelerator. Already resolved -- `--accel` / `--no-accel` over
    // `[build] accel` -- so a rule package derives its own spelling
    // (`-gencode`, `--offload-arch`) from here and the architecture set is
    // written once, in the manifest, and never again in a build program.
    std::string accel;
    // The package this program is building, from `[package]`. Reported because
    // every name a rule generates is derived from it -- the module a consumer
    // imports, the namespace its accessors sit in -- and a build program had no
    // way to ask. See hostprogram::package_name for what it replaced.
    std::string packageName;
    std::string packageNamespace;
    // THE REST OF `[package]`, FOR THE MEMBER OF THE COLLECTION THAT NEEDS IT.
    //
    // A rule generates a declaration and needs the package's NAME. A member
    // that produces a DISTRIBUTABLE needs more: every installer format carries
    // a version, and most carry a description, a licence and a maintainer.
    // Without these a project has to restate them in the member's options,
    // where they can drift from `[package]` with nothing able to detect it --
    // the second copy of a value whose first copy mcpp has already parsed.
    //
    // `packageAuthors` is joined with ';' rather than ',' because an author
    // entry is conventionally `Name <mail@host>` and a name may carry a comma.
    // Empty under an engine that predates these, which a member reads as "fall
    // back to whatever you did before".
    std::string packageVersion;
    std::string packageDescription;
    std::string packageLicense;
    std::string packageAuthors;
    std::string packageRepo;
    // ── The packaging pass this build is part of (mcpp 2026.9.11.1+) ────────
    //
    // Empty for every ordinary build, and that is the value that carries the
    // meaning: a member which produces a distributable SUBMITS NOTHING unless
    // the format it provides was asked for. `mcpp build` therefore has the
    // graph it always had, and the dist edge exists only in the pass that
    // wants it.
    //
    // The value is the `--format` argument verbatim -- `tar`, `dir`, or a name
    // a package provides. It rides the same env vector as everything else here,
    // so `contract_hash` folds it into the build program's re-run key: the
    // second pass re-runs exactly the programs whose answer this changes.
    std::string packFormat;
    // Where `mcpp pack` has ALREADY STAGED the closure, absolute. Non-empty
    // only in the second pass, and only then because a staged tree is produced
    // by mcpp after the link -- so a graph generated before the link cannot
    // name a directory that does not exist yet.
    //
    // This is the value `${mcpp.stage_dir}` expands to. A member reads it to
    // decide the shape of the work (which of `bin/`, `lib/`, `share/` the tree
    // actually has) and writes the placeholder into the action, so the two
    // never disagree.
    std::filesystem::path packStageDir;
    // The packaging pass's RESOLVED strip decision, "1" or "0", and where the
    // separated debug files go, absolute; both empty for every ordinary build
    // (#649 E5). A member that stages libraries of its own follows them, so
    // `--no-strip` and `--debug-symbols` reach its files as they reach the
    // engine's.
    std::string packStrip;
    std::filesystem::path packDebugSymbolsDir;
    // #647 E1: the graph document offered to the ROOT package's program, and
    // the digest of its text. Empty for a dependency's program. The digest
    // joins the re-run key (see `run_build_program`), so a change to what the
    // document says re-runs the program and a change it does not state does
    // not.
    std::filesystem::path graphFile;
    std::string graphDigest;
    // Whether this package builds C++ modules (`[language] modules`).
    //
    // Reported because a rule package that GENERATES a consumer-facing
    // declaration has to choose between a module interface and a header, and
    // the project has already stated which it uses. Deriving it any other way
    // would be a second spelling of one decision. A rule that reads it can make
    // the module surface its default without any project declaring anything,
    // and an engine older than this one leaves the variable absent -- which a
    // rule reads as "header", the behaviour every consumer had before.
    bool languageModules = true;
    // The rule modules a synthesised build program imports, from
    // `BuildConfig::ruleModules`. When this package has no `build.mcpp` and
    // this list is not empty, mcpp writes the program these entries describe.
    // The program is the one the project would have written by hand, which is
    // what makes the declaration a LAYER above `build.mcpp` rather than a
    // second way of doing the same thing.
    std::vector<std::string> ruleModules;
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
    // The program's output directory, `mcpp::out_dir()` (D7). Empty →
    // <artifactsDir>/out, a directory every configuration shares, which is
    // what a caller with no configuration (a test) gets.
    std::filesystem::path outDir;
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
    // #642 E2: the link form ("static" or "shared") each dependency takes in
    // this build, under the same names as `depDirs`, emitted as
    // MCPP_DEP_<SANITIZED_NAME>_LINKAGE. Filled for the ROOT package's program
    // only: the root decides every dependency's form, and a dependency's
    // program runs before facts that decide it are known (see prepare.cppm).
    std::vector<std::pair<std::string, std::string>> depLinkages;
    // #852: a name that more than one visible dependency answers to (two
    // packages whose manifests both say `rules`), with the qualified names of
    // those packages, comma-separated. Such a name is in neither list above:
    // no package can be it, so none is published under it, and the program
    // reads MCPP_DEP_AMBIGUOUS_<SANITIZED_NAME> to say which names to use.
    std::vector<std::pair<std::string, std::string>> depAmbiguous;
    // Payload dirs of the packages this build declared in `[xlings] deps`,
    // as (env var name → dir). Resolved by the caller through the xlings path
    // helpers, so a build.mcpp asks `xpkg_dir("xim", "picolibc-riscv")`
    // instead of reconstructing `<home>/data/xpkgs/<ns>-x-<name>/<version>`
    // — the same reason depDirs exists for mcpp dependencies.
    std::vector<std::pair<std::string, std::string>> xpkgDirs;
    // THE PHASE THIS RUN BELONGS TO (mcpp#755). Empty for every ordinary run,
    // and then `MCPP_PHASE` is not set, so the contract of a program that
    // never asks is unchanged. "toolchain" for the root program's toolchain
    // phase, whose only accepted statements are the build toolchain, re-run
    // keys and messages.
    std::string phase;
    // A run that asked for a payload (`mcpp:xpkg-request=`) is normally
    // DISCARDED: the requests are handed back in `xpkgRequests`, nothing else
    // is applied and nothing is cached, because the engine installs the
    // payloads and runs the program again. Under `plan_only` no payload is
    // installed, so the run is kept -- applied, reported, and still not cached,
    // so the next build asks again.
    bool keepRequestingRun = false;
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
    // WHO DECIDES IS NOT DECIDED HERE. `mcpp::provider::runtime` is the sole
    // project runtime-selection policy and `RuntimeBinding::subosDir` is its
    // resolved answer; this field carries that answer to the child. Deriving
    // it a second time — from the manifest, from `[xlings] deps`, from the
    // config — is how a build ends up with two subos and no way to say which
    // one it used.
    //
    // PREPENDED, NOT SUBSTITUTED. A build program legitimately calls `git`,
    // `python3` or a shell, none of which arrive this way.
    std::string toolsBin;
    // THE PROGRAM, COMPILED (mcpp#797). The build programs' graph compiles
    // every build program of a build in one plan, with its libraries, and
    // hands each run its binary and the identity it was compiled from; the
    // run cache keys on the identity, so a program whose graph changed runs
    // again. `programCompile` is the compile's duration when this build made
    // it. `programError` is a compile that failed, reported at the program's
    // turn so that the first failure in run order is the one reported.
    std::filesystem::path     programBinary;
    std::string               programIdentity;
    std::chrono::milliseconds programCompile{0};
    std::string               programError;
    // THE ARGV THAT RUNS IT, `{}` the program: the host graph's platform's
    // runner when the program is one of its images (SPEC-013 §6, `okrun {}`);
    // empty: the program is run directly.
    std::vector<std::string>  programRunner;
};

// The env-var name `hostprogram::xpkg_dir` reads back. One spelling of the
// sanitizer, shared by both sides — the two drifting apart would make the
// interface answer "" for a package that is right there.
//
// The suffix selects the fact: `DIR` (the payload directory), `SOURCE` and
// `PROGRAM` (where it came from, mcpp#755), read back by `xpkg_source` and
// `xpkg_program`.
std::string response_file_body(std::span<const std::string> args, bool gnuQuoting) {
    std::string body;
    for (auto const& a : args) {
        if (gnuQuoting) {
            // GNU TOKENIZATION TREATS A BACKSLASH AS AN ESCAPE, INCLUDING INSIDE
            // QUOTES -- which is where this differs from a POSIX shell, and where
            // the first attempt at this function was wrong. A Windows path
            // written plainly arrives with its separators eaten: clang read
            // `D:\a\mcpp-plugins\...` back as `D:amcpp-plugins...` and
            // reported `no such file or directory`, and single-quoting it changed
            // nothing. Measured with clang 22.1.8: a response file holding
            // `'-DX=a\b'` yields `X=ab`, and one holding `-DX=a\\b` yields
            // `X=a\b`. So every backslash is doubled and every quote escaped,
            // and whitespace is handled by quoting the whole argument.
            const bool quote = a.find_first_of(" \t") != std::string::npos;
            if (quote) body.push_back('"');
            for (char c : a) {
                if (c == '\\' || c == '"') body.push_back('\\');
                body.push_back(c);
            }
            if (quote) body.push_back('"');
            body.push_back('\n');
            continue;
        }
        // Windows tokenization (cl, clang-cl): a backslash is literal except
        // before a quote, so only whitespace and quotes need handling.
        if (a.find_first_of(" \t\"") == std::string::npos) {
            body += a;
            body.push_back('\n');
            continue;
        }
        body.push_back('"');
        std::size_t slashes = 0;
        for (char c : a) {
            if (c == '\\') { ++slashes; body.push_back(c); continue; }
            if (c == '"') { body.append(slashes, '\\'); body += "\\\""; }
            else body.push_back(c);
            slashes = 0;
        }
        // A run of backslashes that ends the argument would escape the closing
        // quote, so it is doubled.
        body.append(slashes, '\\');
        body += "\"\n";
    }
    return body;
}

inline std::string xpkg_env_var(std::string_view ns, std::string_view name,
                                std::string_view suffix = "DIR") {
    std::string out = "MCPP_XPKG_";
    auto put = [&](std::string_view s) {
        for (char c : s)
            out += (c >= 'a' && c <= 'z') ? char(c - 'a' + 'A')
                 : ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) ? c : '_';
    };
    if (!ns.empty()) { put(ns); out += '_'; }
    put(name);
    out += '_';
    out += suffix;
    return out;
}

// THE CONTENT OF A RESPONSE FILE CARRYING `args`, one argument per line.
//
// Every compiler driver mcpp supports reads `@file`, but not with one grammar:
// clang and GCC tokenize it the GNU way, where a backslash escapes the next
// character, and cl and clang-cl tokenize it the Windows way, where a backslash
// is literal. `gnuQuoting` picks between them -- single quotes, inside which
// nothing is special, or Windows quoting of whitespace and quotes.
//
// Exported because its quoting is the part worth testing, and the command it
// serves cannot be run on a host whose limit it does not cross.
std::string response_file_body(std::span<const std::string> args, bool gnuQuoting);

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
// THE PART OF A BUILD PROGRAM'S ENVIRONMENT AN INSTALL HOOK ALSO RECEIVES
// (#613): `MCPP_COMPILER`, `MCPP_CXX_STDLIB`, `MCPP_TARGET` and its three
// segments, in that order. One function computes them for both, so a hook and a
// build program cannot be told different things about one build. Every value is
// present, and empty when it does not apply; `MCPP_TARGET` is the host triple
// when `env.targetTriple` is empty.
std::vector<std::pair<std::string, std::string>>
install_hook_env(const BuildProgramEnv& env);

// The program its rules describe, for a package with device sources and no
// `build.mcpp` of its own: it imports each rule module and calls its
// `compile()`. Written by the build programs' graph (mcpp#797).
std::string synthesised_rule_program(const std::vector<std::string>& modules);

// Runs the build program of the package at `root` -- `env.programBinary`, as
// the build programs' graph compiled it -- or replays its cached directives
// when nothing it depends on changed, and applies them to `m`.
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

// The inputs the build programs under `<workRoot>/target/.build-mcpp` declared
// on their last run, read from the same caches program_inputs_stale compares:
// declared files (absolute), glob patterns (relative to `root`) and environment
// variable names. The build database lists them as inputs that change the plan.
struct DeclaredProgramInputs {
    std::filesystem::path              root;
    std::vector<std::filesystem::path> files;
    std::vector<std::string>           globs;
    std::vector<std::string>           envs;
};
std::vector<DeclaredProgramInputs>
declared_program_inputs(const std::filesystem::path& workRoot);

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
    return mcpp::invocation::get(name).value_or(std::string());
}

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
// `warn` is false in the concurrent compile phase (#748, B2): the values are
// computed again at the program's turn, which states each warning once.
std::vector<std::pair<std::string, std::string>>
contract_env(const fs::path& root, const fs::path& outDir, const BuildProgramEnv& env,
             bool warn = true) {
    std::vector<std::pair<std::string, std::string>> e;
    auto hostT = mcpp::toolchain::triple::host_triple().str();
    // The toolchain and target names an install hook also receives, from the
    // one function that computes them for both (#613). Emitted in the order
    // they always had, so the re-run key of an existing program is unchanged.
    const auto shared = install_hook_env(env);
    auto shared_value = [&](std::string_view key) {
        for (auto const& [k, v] : shared)
            if (k == key) return v;
        return std::string{};
    };
    e.emplace_back("MCPP_TARGET", shared_value("MCPP_TARGET"));
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
    // parity): parsed ONCE, in install_hook_env, through the canonical triple parser so every
    // build.mcpp stops hand-splitting MCPP_TARGET. MCPP_TARGET_ENV is "" when
    // the triple has no env segment (macOS); all three are "" for an
    // escape-hatch triple outside the canonical vocabulary. They ride the
    // same env vector, so contract_hash folds them into the re-run key.
    e.emplace_back("MCPP_TARGET_OS", shared_value("MCPP_TARGET_OS"));
    e.emplace_back("MCPP_TARGET_ARCH", shared_value("MCPP_TARGET_ARCH"));
    e.emplace_back("MCPP_TARGET_ENV", shared_value("MCPP_TARGET_ENV"));
    e.emplace_back("MCPP_HOST", hostT);
    // Always emitted, empty when they do not apply: a build program reads
    // these through `env_or`, which cannot tell "absent" from "empty", and an
    // absent variable would make the answer depend on whatever the parent
    // process happened to export.
    e.emplace_back("MCPP_TOOLCHAIN_DIR", env.toolchainDir);
    e.emplace_back("MCPP_TOOLCHAIN_SYSROOT", env.toolchainSysroot);
    e.emplace_back("MCPP_TOOLCHAIN_BINUTILS_DIR", env.toolchainBinutilsDir);
    e.emplace_back("MCPP_COMPILER", shared_value("MCPP_COMPILER"));
    e.emplace_back("MCPP_CXX_STDLIB", shared_value("MCPP_CXX_STDLIB"));
    e.emplace_back("MCPP_CXX_ABI_ROOT", env.cxxAbiRoot);
    e.emplace_back("MCPP_TARGET_SYSROOT", env.targetSysroot);
    // #734 E2. Every key always, empty when it does not apply, for the reason
    // the lines above give.
    for (auto key : kBuildInformationKeys) {
        auto it = env.buildInfo.find(std::string(key));
        e.emplace_back(std::string(key), it == env.buildInfo.end() ? std::string{} : it->second);
    }
    e.emplace_back("MCPP_TARGET_BUILTINS_LIB", env.targetBuiltinsLib);
    e.emplace_back("MCPP_TARGET_LIBC_PROFILE", env.targetLibcProfile);
    e.emplace_back("MCPP_TARGET_LIBC", env.targetLibc);
    // #622 A11. Always emitted, empty when `min_platform_version` returned
    // empty (every non-Apple, non-Android target) — the same "absent and
    // empty must not be the same observation" reason every other always-on
    // contract value here is. Rides this vector, so it joins the re-run key
    // like every other value contract_hash folds in.
    e.emplace_back("MCPP_TARGET_MIN_PLATFORM_VERSION", env.minPlatformVersion);
    e.emplace_back("MCPP_PKG_CONFIG_LIBDIR", env.pkgConfigLibdir);
    e.emplace_back("MCPP_PROFILE", env.profile);
    e.emplace_back("MCPP_ACCEL", env.accel);
    e.emplace_back("MCPP_LANGUAGE_MODULES", env.languageModules ? "1" : "0");
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
    e.emplace_back("MCPP_PKG_NAME", env.packageName);
    e.emplace_back("MCPP_PKG_NAMESPACE", env.packageNamespace);
    e.emplace_back("MCPP_PKG_VERSION", env.packageVersion);
    e.emplace_back("MCPP_PKG_DESCRIPTION", env.packageDescription);
    e.emplace_back("MCPP_PKG_LICENSE", env.packageLicense);
    e.emplace_back("MCPP_PKG_AUTHORS", env.packageAuthors);
    e.emplace_back("MCPP_PKG_REPO", env.packageRepo);
    e.emplace_back("MCPP_PACK_FORMAT", env.packFormat);
    e.emplace_back("MCPP_PACK_STAGE_DIR", env.packStageDir.string());
    e.emplace_back("MCPP_PACK_STRIP", env.packStrip);
    e.emplace_back("MCPP_PACK_DEBUG_SYMBOLS_DIR", env.packDebugSymbolsDir.string());
    e.emplace_back("MCPP_GRAPH_FILE", env.graphFile.string());
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
            if (warn) mcpp::ui::warning(std::format(
                "build.mcpp: dependency name collides on {} (kept '{}', ignored "
                "'{}') — rename one dependency to disambiguate", var,
                it->second, dir.string()));
        }
    }
    for (auto const& [name, which] : env.depAmbiguous)
        e.emplace_back("MCPP_DEP_AMBIGUOUS_" + sanitize_feature_env(name), which);
    // #642 E2: the form each dependency takes, beside its directory and under
    // the same sanitised name. Its own collision map, because the variable
    // names differ from the `_DIR` ones; the rule is the same: keep the first,
    // warn on a conflicting second.
    std::map<std::string, std::string> linkageVarValue;
    for (auto const& [name, form] : env.depLinkages) {
        auto var = "MCPP_DEP_" + sanitize_feature_env(name) + "_LINKAGE";
        auto [it, inserted] = linkageVarValue.try_emplace(var, form);
        if (inserted) {
            e.emplace_back(var, form);
        } else if (it->second != form) {
            if (warn) mcpp::ui::warning(std::format(
                "build.mcpp: dependency name collides on {} (kept '{}', ignored "
                "'{}') — rename one dependency to disambiguate", var,
                it->second, form));
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
    if (!env.phase.empty()) e.emplace_back("MCPP_PHASE", env.phase);
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
        if (auto const& inherited = mcpp::runtime::launch_env::inherited_path(); !inherited.empty()) {
            path += mcpp::runtime::launch_env::path_list_separator();
            path += inherited;
        }
        e.emplace_back("PATH", path);
    }

    for (auto const& [var, path] : env.toolPaths) {
        auto [it, inserted] = depVarValue.try_emplace(var, path);
        if (inserted) {
            e.emplace_back(var, path);
        } else if (it->second != path) {
            if (warn) mcpp::ui::warning(std::format(
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

// THE CONTRACT, KEY BY KEY, beside the run record: `contract_hash` keeps one
// digest, and "the environment contract changed" does not say which variable
// moved. The record names each key with the digest of its value, and the log
// names the keys that differ when a run is not replayed.
std::string contract_keys_path(const fs::path& bdir) {
    return (bdir / "build.mcpp.ctx").string();
}

void write_contract_keys(const fs::path& bdir,
                         const std::vector<std::pair<std::string, std::string>>& e) {
    std::ofstream os(contract_keys_path(bdir), std::ios::trunc);
    for (auto const& [k, v] : e) os << k << '\t' << mcpp::toolchain::hash_string(v) << '\n';
}

std::string changed_contract_keys(const fs::path& bdir,
                                  const std::vector<std::pair<std::string, std::string>>& e) {
    std::map<std::string, std::string> before;
    std::ifstream in(contract_keys_path(bdir));
    for (std::string line; std::getline(in, line);)
        if (auto tab = line.find('\t'); tab != std::string::npos)
            before[line.substr(0, tab)] = line.substr(tab + 1);
    std::string out;
    std::set<std::string> seen;
    for (auto const& [k, v] : e) {
        seen.insert(k);
        auto it = before.find(k);
        if (it == before.end() || it->second != mcpp::toolchain::hash_string(v))
            out += (out.empty() ? "" : ", ") + k;
    }
    for (auto const& [k, h] : before)
        if (!seen.contains(k)) out += (out.empty() ? "" : ", ") + k;
    return out;
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

// The first value `vanished` answers for.
template <class F>
std::optional<std::string> first_vanished(const std::vector<std::string>& values, F&& vanished) {
    for (auto const& v : values)
        if (auto path = vanished(v)) return path;
    return std::nullopt;
}

// Why the cached run is not valid, for the log; empty when it is.
std::string cache_stale_reason(const fs::path& root, const fs::path& bdir, const CacheRecord& c,
                               const std::string& programHash, const std::string& compilerHash,
                               const std::string& ctxHash, const fs::path& outDir = {}) {
    if (!c.loaded) return "no record";
    if (c.epoch != dirs::kCacheEpoch) return "record epoch";  // pre-epoch entries rerun once
    if (c.unknownRecord) return "unknown record line";
    if (c.programHash != programHash) return "program identity " + c.programHash + " -> " + programHash;
    if (c.compilerHash != compilerHash) return "record shape";
    if (c.ctxHash != ctxHash) return "environment contract";   // pre-G3 caches rerun once
    for (auto const& [h, path] : c.inputs)
        if (mcpp::toolchain::hash_file(abs_against_root(root, path)) != h) return "input " + path;
    for (auto const& [h, name] : c.envs)
        if (mcpp::toolchain::hash_string(env_value(name)) != h) return "environment variable " + name;
    // #359: the path SET behind each declared glob. A file appearing or
    // disappearing changes it; editing one does not (that is what the `in`
    // entries above are for).
    if (!c.globs.empty()) {
        auto outName = output_dir_name(root, bdir);
        for (auto const& [h, pattern] : c.globs)
            if (dirs::glob_fingerprint(root, pattern, outName) != h) return "glob " + pattern;
    }
    // A declared output that vanished invalidates the cache. Driven off the
    // table's mustExistAfterRun so a future output-shaped directive is covered
    // without editing this function.
    for (auto const& def : dirs::kTable) {
        if (!def.mustExistAfterRun) continue;
        for (auto const& p : c.directives.at(def.slot))
            if (!fs::exists(abs_against_root(root, p))) return "output " + p;
    }
    // WHAT THE PROGRAM WROTE INTO ITS OUTPUT DIRECTORY AND NAMED: an include
    // directory, a library directory, a file to deploy. A directive that names
    // a path below `MCPP_OUT_DIR` which is no longer there was written by a
    // run whose output was deleted since (`rm -rf target/.build-mcpp/out`),
    // and replaying it hands the build a directory with nothing in it.
    if (!outDir.empty()) {
        const std::array<std::string, 2> spellings{ outDir.generic_string(), outDir.string() };
        // The path a value names below the output directory, when it is gone.
        auto vanished = [&](const std::string& v) -> std::optional<std::string> {
            std::error_code ec;
            for (auto const& text : spellings) {
                const auto at = v.find(text);
                if (at == std::string::npos) continue;
                const auto end = v.find_first_of("\t\"", at);
                auto path = v.substr(at, end == std::string::npos ? std::string::npos : end - at);
                if (!fs::exists(path, ec)) return path;
            }
            return std::nullopt;
        };
        // Only what reaches a compile, a link or a run: a warning or a claim
        // that mentions a path is text, and an action's outputs are the
        // build's products, made again by the build whenever they are missing.
        for (auto const& def : dirs::kTable) {
            if (def.scope == dirs::Scope::Advisory || def.scope == dirs::Scope::Claim
                || def.scope == dirs::Scope::RerunKey || def.scope == dirs::Scope::GraphNode)
                continue;
            if (auto path = first_vanished(c.directives.at(def.slot), vanished))
                return "output " + *path;
        }
    }
    return {};
}

// Decide whether the cached run is still valid (so we can skip recompiling/running).
bool cache_fresh(const fs::path& root, const fs::path& bdir, const CacheRecord& c,
                 const std::string& programHash, const std::string& compilerHash,
                 const std::string& ctxHash) {
    return cache_stale_reason(root, bdir, c, programHash, compilerHash, ctxHash).empty();
}


// The program a set of rule modules describes.
//
// It is the program a project writes by hand for the same rules, and that is
// the whole contract: the declaration is a layer ABOVE `build.mcpp`, not a
// second mechanism beside it, so a project that outgrows it copies this file
// into its root and edits it. Synthesis then stops, because a project that has
// its own program keeps it.
//
// The namespace comes from the module name with `.` exchanged for `::`, which
// is the convention `mcpp.rules.<x>` already follows and the one a third-party
// rule opts into by naming its module. Nothing here knows what any rule does.
std::string synthesised_rule_program_impl(const std::vector<std::string>& modules) {
    std::string s =
        "// Generated by mcpp from the build rules this package's dependencies\n"
        "// declare. Do not edit. To take it over, copy this file to `build.mcpp`\n"
        "// in the project root; mcpp synthesises nothing once a project has one.\n"
        "import std;\n"
        "import mcpp;\n";
    for (auto const& m : modules) s += "import " + m + ";\n";
    s += "\nint main() {\n    bool ok = true;\n";
    for (auto const& m : modules) {
        std::string ns;
        for (char c : m) { if (c == '.') ns += "::"; else ns += c; }
        // `&&` would stop at the first refusal, and a project with two rules
        // wants both diagnostics rather than one and then silence.
        s += "    ok = " + ns + "::compile() && ok;\n";
    }
    s += "    return ok ? 0 : 1;\n}\n";
    return s;
}

} // namespace

std::vector<std::pair<std::string, std::string>>
install_hook_env(const BuildProgramEnv& env) {
    mcpp::toolchain::triple::Triple t{};
    std::string target;
    if (env.targetTriple.empty()) {
        t = mcpp::toolchain::triple::host_triple();
        target = t.str();
    } else {
        target = env.targetTriple;
        if (auto p = mcpp::toolchain::triple::parse(env.targetTriple)) t = *p;
    }
    return {
        {"MCPP_COMPILER", env.compilerId},
        {"MCPP_CXX_STDLIB", env.cxxStdlib},
        {"MCPP_TARGET", target},
        {"MCPP_TARGET_OS", t.os},
        {"MCPP_TARGET_ARCH", t.arch},
        {"MCPP_TARGET_ENV", t.env},
    };
}

std::expected<void, std::string> run_build_program_impl(
    mcpp::manifest::Manifest& m,
    const fs::path& root,
    const mcpp::toolchain::Toolchain& tc,
    const BuildProgramEnv& env) {

    // A package with neither a `build.mcpp` nor rules that describe one has no
    // program (the synthesised one is written by the build programs' graph).
    std::error_code ec;
    if (!fs::exists(root / "build.mcpp", ec) && env.ruleModules.empty()) return {};

    fs::path bdir = build_dir(root, env);
    fs::path outDir = env.outDir.empty() ? bdir / "out" : env.outDir;
    auto childEnv = contract_env(root, outDir, env);
    std::string ctxHash = contract_hash(childEnv);
    // What ctxHash is the digest of: the environment and the graph document.
    auto contractEnv = childEnv;
    contractEnv.emplace_back("(graph document)", env.graphDigest);
    // THE GRAPH DOCUMENT'S CONTENT, NOT ITS PATH. The path is the same on
    // every run; what the document says is what the program's answer depends
    // on (#647 E1).
    if (!env.graphDigest.empty())
        ctxHash = mcpp::toolchain::hash_string(ctxHash + "\ngraph=" + env.graphDigest);

    // A compile that failed is reported at the program's turn, so the failure
    // that reaches the user is the first in the order the programs run.
    if (!env.programError.empty()) return std::unexpected(env.programError);
    if (env.programBinary.empty())
        return std::unexpected(std::string(
            "the build program was not compiled: no binary reached its run "
            "(an mcpp defect; please report it with this line)"));

    // THE IDENTITY THE PROGRAM WAS COMPILED FROM (mcpp#797) keys the run
    // cache: the program's text, the engine, the toolchain, and every package
    // of its graph with the content of each tree that can change in place. A
    // program whose graph changed is compiled again and run again; one whose
    // graph stands is replayed. The second field is the record's own shape.
    const std::string programHash  = env.programIdentity;
    const std::string compilerHash = mcpp::toolchain::hash_string("build-programs-graph-v1");

    // The package the status lines name: several programs run in one
    // workspace plan, and a line without its package cannot be attributed.
    const std::string who = m.package.namespace_.empty()
        ? m.package.name : m.package.namespace_ + "." + m.package.name;

    // Fast path: declared inputs + contract unchanged → reapply cached
    // directives, no run.
    CacheRecord cache = read_cache(bdir);
    const auto stale = cache_stale_reason(root, bdir, cache, programHash, compilerHash, ctxHash,
                                          outDir);
    if (!stale.empty()) {
        std::string keys;
        if (stale == "environment contract") keys = changed_contract_keys(bdir, contractEnv);
        mcpp::log::verbose("build-program", std::format("{}: runs ({}{}{})", who, stale,
                                                        keys.empty() ? "" : ": ", keys));
    }
    if (stale.empty()) {
        if (auto terr = dirs::target_directive_error(m, cache.directives); !terr.empty())
            return std::unexpected(terr);
        // #622 A4. Checked on the cache-hit path too, so a `deploy` a fresh
        // run would refuse is refused again on a replay, never silently
        // applied because the check itself was never re-run.
        if (auto derr = dirs::deploy_directive_error(m, cache.directives); !derr.empty())
            return std::unexpected(derr);
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
        report_stated_diagnostics(m.package.name, cache.directives);
        mcpp::build::progress::program_finished(
            who, env.requested, mcpp::build::progress::ProgramOutcome::Cached, {}, {});
        return {};
    }

    // From here on the program is run, and every return reports its outcome:
    // `ran` when the directives were applied, `failed` otherwise.
    using ProgressClock = std::chrono::steady_clock;
    struct ProgramReport {
        const std::string& who;
        bool requested;
        ProgressClock::time_point runStart{};
        std::chrono::milliseconds compile{0};
        bool ran = false;
        bool reported = false;
        ~ProgramReport() { report(); }
        void report() {
            if (reported) return;
            reported = true;
            using namespace std::chrono;
            const auto zero = ProgressClock::time_point{};
            const auto run = runStart == zero ? milliseconds{0}
                : duration_cast<milliseconds>(ProgressClock::now() - runStart);
            mcpp::build::progress::program_finished(
                who, requested,
                ran ? mcpp::build::progress::ProgramOutcome::Ran
                    : mcpp::build::progress::ProgramOutcome::Failed,
                compile, run, /*compiledAside=*/compile.count() > 0);
        }
    } programReport{who, env.requested};
    programReport.compile = env.programCompile;

    // Both: the output directory is per configuration and need not lie
    // below `bdir`, where the program's cache is (D7).
    fs::create_directories(bdir, ec);
    fs::create_directories(outDir, ec);
    const fs::path bin = env.programBinary;
    // The driver's dialect decides how the program's output is read
    // (`accept_output`).
    const auto& dial = mcpp::toolchain::dialect_for(tc);

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
    // `--deny exec-build-script` (design 2026-10-26 §5.5): the effect is
    // running code from the workspace, so the refusal is here, where a program
    // would start -- a program whose answer is current runs nothing and is
    // not refused.
    if (mcpp::effects::effect_denied("exec-build-script")) {
        mcpp::effects::note_effect_refused("exec-build-script", std::format("{} build program", who));
        return std::unexpected(std::format(
            "effect denied: exec-build-script -- {}'s build program would run, and the caller "
            "denied running code from the workspace (--deny exec-build-script)", who));
    }
    mcpp::build::progress::program_running(who, env.requested);
    programReport.runStart = ProgressClock::now();
    bool timedOut = false;
    // The bound comes from THIS package's manifest — a dependency's generator
    // is bounded by the dependency's own declaration, because its author is
    // the one who knows how long it takes.
    const auto deadline =
        dirs::run_timeout_for(m.buildConfig.buildProgramTimeoutSecs);
    std::vector<std::string> argv{bin.string()};
    if (!env.programRunner.empty()) {
        argv.clear();
        bool placed = false;
        for (auto const& a : env.programRunner) {
            if (a == "{}") { argv.push_back(bin.string()); placed = true; }
            else argv.push_back(a);
        }
        if (!placed) argv.push_back(bin.string());
    }
    auto rres = mcpp::run::capture_exec_deadline(
        argv, childEnv, deadline, &timedOut, root.string());
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
    // A directive in another encoding names a different file (#693). Refused
    // before anything is applied, for the same reason as the checks below.
    if (auto eerr = dirs::encoding_error(d)) {
        return std::unexpected(*eerr);
    }
    // Refuse a malformed action BEFORE applying anything: a half-applied
    // action set is worse than none, and an action that silently does not
    // exist surfaces as a missing generated source three edges away.
    if (auto aerr = dirs::action_error(d); !aerr.empty()) {
        return std::unexpected(aerr);
    }
    // A directive that names a target is checked against the manifest before
    // anything is applied, for the same reason.
    if (auto terr = dirs::target_directive_error(m, d); !terr.empty()) {
        return std::unexpected(terr);
    }
    // #622 A4: a `deploy` directive's `to` is checked before anything is
    // applied, for the same reason.
    if (auto derr = dirs::deploy_directive_error(m, d); !derr.empty()) {
        return std::unexpected(derr);
    }
    // GLOB INPUTS OUTSIDE THE PACKAGE (#766). A registry or git dependency's
    // tree sits in the package store, whose other contents depend on what
    // else is installed: a pattern leaving that package would make its re-run
    // key a fact about this machine. Refused, naming the pattern. Elsewhere it
    // is honoured; a pattern that no walk can enter is reported, since it can
    // never re-run the program.
    for (auto const& g : d.at(Slot::RerunGlobs)) {
        if (env.sealedPackage && dirs::glob_leaves_package(g))
            return std::unexpected(std::format(
                "build.mcpp of '{}' watches '{}', which leaves the package: a registry or git "
                "dependency may only watch files inside its own tree",
                m.package.name, g));
        if (auto why = dirs::glob_unwatchable(root, g, output_dir_name(root, bdir)))
            mcpp::ui::warning(std::format(
                "build.mcpp of '{}': rerun_if_changed_glob(\"{}\") can never re-run it: {}. "
                "To watch one file's contents, use rerun_if_changed",
                m.package.name, g, *why));
    }
    if (d.protocol == 0) {
        for (auto const& k : d.unknownKeys)
            mcpp::ui::warning(std::format(
                "build.mcpp: ignoring unknown directive 'mcpp:{}'", k));
    }
    // THE TOOLCHAIN PHASE STATES THE TOOLCHAIN AND NOTHING ELSE (mcpp#755).
    // It runs before the dependency graph is resolved, so a flag, a source or
    // an action stated here would describe a build that does not exist yet; a
    // payload request cannot be answered either, because the payloads are
    // declared by a graph that has not been read. Refused rather than
    // dropped, naming the first directive that does not belong.
    if (env.phase == "toolchain") {
        for (auto const& def : dirs::kTable) {
            switch (def.slot) {
                case Slot::ToolchainStatement: case Slot::RerunFiles:
                case Slot::RerunEnv: case Slot::RerunGlobs:
                case Slot::Warnings: case Slot::Diagnostics:
                    continue;
                default: break;
            }
            if (d.at(def.slot).empty()) continue;
            return std::unexpected(std::format(
                "build.mcpp stated `mcpp:{}=` in its toolchain phase.\n"
                "       That phase runs before the dependency graph is resolved, and\n"
                "       it states the build toolchain only (`mcpp::toolchain`,\n"
                "       `mcpp::plugins::toolchain::use`). Return from main() after\n"
                "       stating it: `mcpp::phase()` is \"toolchain\" there, and\n"
                "       `mcpp::plugins::toolchain::configure` returns true.",
                def.wire));
        }
    } else if (env.phase == "platform") {
        // THE PLATFORM PHASE STATES THE PLATFORM'S BUILD, LINK AND RUNNER
        // FIELDS, AND THE PACKAGE FORMATS IT PROVIDES (SPEC-013 §4.2, SPEC-014
        // §4): it describes a class of targets, not a package of sources, and
        // what it states reaches every unit and every link of the target. A
        // format is provided as any package provides one -- `pack-format` and
        // the `artifact` actions that produce it from the staged tree. Refused
        // rather than dropped, naming the first directive that does not belong.
        for (auto const& def : dirs::kTable) {
            switch (def.slot) {
                case Slot::CxxFlags: case Slot::CFlags: case Slot::LdFlags:
                case Slot::Runner: case Slot::NamedRunner: case Slot::RunnerLongLived:
                case Slot::RunExclusive: case Slot::LinkStart: case Slot::LinkDriver:
                case Slot::LinkInputs: case Slot::ArtifactNames: case Slot::Seal:
                case Slot::RunHost: case Slot::RerunFiles: case Slot::RerunEnv:
                case Slot::RerunGlobs: case Slot::Warnings: case Slot::Diagnostics:
                case Slot::PackFormats:
                    continue;
                default: break;
            }
            if (d.at(def.slot).empty()) continue;
            if (def.slot == Slot::Actions
                && std::ranges::all_of(d.at(def.slot), [](std::string const& payload) {
                       return dirs::action_role_of(payload).value_or("") == "artifact";
                   }))
                continue;
            mcpp::build::refusal::record(mcpp::build::refusal::Code::PlatformPhaseDirective);
            return std::unexpected(std::format(
                "build.mcpp stated `mcpp:{}=` in its platform phase.\n"
                "       A platform's plugin states the platform's build, link and runner\n"
                "       fields (compile and link flags, the link driver, inputs, entry,\n"
                "       artifact names, seal steps, runners) and the package formats it\n"
                "       provides (`pack-format`, `artifact` actions); `mcpp::phase()` is\n"
                "       \"platform\" there (SPEC-013 §4.2).",
                def.wire));
        }
        if (!d.at(Slot::ToolchainStatement).empty())
            return std::unexpected(std::string(
                "build.mcpp stated `mcpp:toolchain=` in its platform phase; a platform's "
                "toolchain is its `[platform] toolchain`"));
    } else if (!d.at(Slot::ToolchainStatement).empty()) {
        return std::unexpected(std::string(
            "build.mcpp stated `mcpp:toolchain=` outside the toolchain phase.\n"
            "       The build toolchain is stated by the root build program of a\n"
            "       project whose manifest says\n"
            "         [toolchain]\n"
            "         default = { configure = \"build.mcpp\" }\n"
            "       and only while `mcpp::phase()` is \"toolchain\"."));
    }
    // A PAYLOAD REQUEST (mcpp#755). The payloads are installed by the caller,
    // in one batch with every other program's, and this program runs again
    // with their directories -- so this run is discarded: nothing applied,
    // nothing cached, and no line of its own (the run that answers reports).
    if (!d.at(Slot::XpkgRequests).empty() && !env.keepRequestingRun) {
        m.buildConfig.xpkgRequests = d.at(Slot::XpkgRequests);
        programReport.reported = true;
        return {};
    }
    const bool cacheThisRun = d.at(Slot::XpkgRequests).empty();

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
    // The program's line comes before what it has to say.
    programReport.ran = true;
    programReport.report();
    // The second of the two sites. See the note on the cache-hit path above.
    for (auto const& a : dirs::advisories(m.package.name, d))
        mcpp::ui::warning(a);
    report_stated_diagnostics(m.package.name, d);
    // A run kept although it asked for a payload (`plan_only`) is never
    // cached: replaying it would keep the build from ever asking again.
    if (cacheThisRun) {
        write_cache(bdir, root, programHash, compilerHash, ctxHash, d);
        write_contract_keys(bdir, contractEnv);
    }
    return {};
}

std::string synthesised_rule_program(const std::vector<std::string>& modules) {
    return synthesised_rule_program_impl(modules);
}

std::expected<void, std::string> run_build_program(
    mcpp::manifest::Manifest& m,
    const fs::path& root,
    const fs::path& /*hostCompiler*/,
    const mcpp::toolchain::Toolchain& tc,
    const mcpp::manifest::CppStandardConfig& /*cppStandard*/,
    const BuildProgramEnv& env) {
    return run_build_program_impl(m, root, tc, env);
}

std::vector<DeclaredProgramInputs>
declared_program_inputs(const fs::path& workRoot) {
    std::vector<DeclaredProgramInputs> out;
    std::error_code ec;
    const fs::path base = workRoot / "target" / ".build-mcpp";
    if (!fs::exists(base, ec)) return out;
    // The walk program_inputs_stale makes, bounded the same way.
    fs::recursive_directory_iterator it(
        base, fs::directory_options::skip_permission_denied, ec);
    if (ec) return out;
    for (; it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (it.depth() >= 3) { it.disable_recursion_pending(); continue; }
        if (it->path().filename() != "build.mcpp.cache") continue;
        auto rec = read_cache(it->path().parent_path());
        if (!rec.loaded || rec.rootPath.empty()) continue;
        DeclaredProgramInputs d;
        d.root = fs::path{rec.rootPath};
        for (auto const& [h, rel] : rec.inputs)
            d.files.emplace_back(abs_against_root(d.root, rel));
        for (auto const& [h, pattern] : rec.globs) d.globs.push_back(pattern);
        for (auto const& [h, name] : rec.envs) d.envs.push_back(name);
        out.push_back(std::move(d));
    }
    std::ranges::sort(out, {}, [](const DeclaredProgramInputs& d) {
        return d.root.generic_string();
    });
    return out;
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
