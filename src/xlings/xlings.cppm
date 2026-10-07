// mcpp.xlings — unified abstraction layer for all xlings (external package
// manager) interactions. Consolidates NDJSON event parsing, subprocess
// command building, path helpers, and bootstrap progress types that were
// previously scattered across config.cppm, package_fetcher.cppm, cli.cppm,
// flags.cppm, ninja_backend.cppm, and stdmod.cppm.
//
// This module is a LEAF dependency: it only imports `std` and
// `mcpp.pm.compat`. It must NOT import mcpp.config or any other mcpp module.

module;
#include <cstdio>      // stderr
#include <cstdlib>

export module mcpp.xlings;

import std;
import mcpp.pm.compat;
import mcpp.pm.index_contract;
import mcpp.pm.index_snapshot;
import mcpp.platform;
import mcpp.log;
import mcpp.ui;                 // closing notices of the refresh guard
import mcpp.home;
import mcpp.xpkg_version;
import mcpp.libs.json;

export namespace mcpp::xlings {

// ─── Env: resolved xlings binary + home directory ───────────────────

struct Env {
    std::filesystem::path binary;      // xlings binary path
    std::filesystem::path home;        // XLINGS_HOME directory
    std::filesystem::path projectDir;  // XLINGS_PROJECT_DIR (empty = global mode)
};

// ─── Pinned version constants ───────────────────────────────────────

namespace pinned {
    inline constexpr std::string_view kPatchelfVersion = "0.18.0";
    inline constexpr std::string_view kNinjaVersion    = "1.12.1";
    // The xlings a release bundles at <install>/registry/bin/xlings, and the
    // version `mcpp self env` reports.
    //
    // This is the SOURCE OF TRUTH for every xlings pin in .github/, and
    // `.github/tools/check_version_pins.sh` enforces that — CI fails if any
    // pin disagrees. This comment used to instead name three files to keep
    // in lock-step by hand; that list was already missing both composite
    // actions, which is how CI's sandbox sat on 0.4.30 unnoticed while
    // everything else had moved on. Don't reintroduce a hand-maintained list.
    //
    // THIS IS A FLOOR, not just the current pick, and it has been raised
    // twice for reasons that both still hold.
    //
    // First, at 2026.8.27.5. Below 2026.8.27.2 the bundled xlings takes a
    // subos's runtime binding from a compiled-in constant, so a home can
    // declare one glibc and install another the moment the index publishes a
    // packaging revision -- and mcpp is the
    // party that notices, because select_glibc_payload_lib looks up the
    // payload directory by the binding's exact version and refuses to fall
    // back:
    //
    //   error: selected RuntimeBinding glibc@2.44 requires payload
    //          '<store>/xim-x-glibc/2.44', but it is not installed
    //
    // Measured across four xlings versions against an index carrying both
    // 2.44 and 2.44.2: every client whose binding is a constant mismatched;
    // 2026.8.27.4 and .5, which read the index, stayed consistent. .5 also
    // makes the declaration outrank the index during resolution, so it holds
    // even when `latest` is not the highest entry in the table.
    //
    // Second, at 2026.8.30.2 (mcpp#533). Below it, xlings answered "is this
    // package already installed" from the xvm version database keyed on the
    // BARE SHORT NAME, so a package skipped its own `install()` whenever any
    // other namespace held the same `<name>@<version>` —
    // `compat:libdrm@2.4.123` against the
    // `xim:libdrm@2.4.123` that Mesa pulls in. The descriptor's tree was never
    // built, and mcpp is again the party that notices, four layers later:
    //
    //   /bin/sh: 1: -shared: not found
    //
    // mcpp now refuses that link unit by name and withholds `.mcpp_ok` from a
    // directory holding nothing the package installed, so the failure is
    // legible on any client. Only a client at or above this floor INSTALLS
    // correctly — which is why this is a floor and not a preference, and why
    // an index must still not publish a deliberately colliding
    // `<name>@<version>` (see .agents/docs/2026-08-30-cross-repo-fix-plan §1).
    //
    // Third, at 2026.9.14.1 (mcpp#636). Below it, a package without an
    // `install()` received the whole download directory rather than its own
    // archive: on a host store, openkal 0.8.0 held 1.6 G of other packages'
    // downloads. A registry that already holds such a payload is repaired by
    // `XLINGS_HOME=<registry> xlings self doctor --fix`.
    //
    // Fourth, at 2026.9.16.1 (openxlings/xlings#601). Below it, the region
    // object mcpp writes for an index artifact (`{"GLOBAL": …, "CN": …}`,
    // src/config.cppm) resolved to one base, so under `mirror = CN` a 403 from
    // GitCode fell back to a git clone from GitHub instead of trying GLOBAL;
    // and an index refresh had no bound on a connection that went silent
    // after its handshake.
    //
    // Fifth, at 2026.9.20.1 (openxlings/xlings#610). Below it, the global
    // workspace could be read out of a project's subos, and the derived shim
    // table then removed every global entry a project did not declare; an
    // install that failed to download could still print `installed`. mcpp
    // drives xlings from inside project and sandbox subos, which is the
    // position where the first misread applied.
    //
    // Sixth, at 2026.9.26.2 (openxlings/xlings#613). Below it, xlings.exe ran
    // in the system's ANSI code page and its `main` had no exception boundary:
    // a working directory outside that code page ended it with 0xC0000409 and
    // no output (mcpp#693), and under an MCPP_HOME outside it the xlings mcpp
    // vendors could not initialise its sandbox. It now declares the UTF-8 code
    // page, as mcpp.exe does.
    // Metadata and install share the process ABI context from 2026.10.8.1;
    // ARM64 runtime exports must be correct before dependency resolution.
    inline constexpr std::string_view kXlingsVersion   = "2026.10.8.1";
    inline constexpr std::string_view kNasmVersion     = "3.02";
}

// ─── Path helpers (pure functions, no subprocess) ───────────────────

namespace paths {

    // xpkgs base: env.home / "data" / "xpkgs"
    std::filesystem::path xpkgs_base(const Env& env);

    // sandbox bin: env.home / "subos" / "default" / "bin"
    std::filesystem::path sandbox_bin(const Env& env);

    // sandbox sysroot: env.home / "subos" / "default"
    std::filesystem::path sysroot(const Env& env);

    // xim tool root: xpkgs_base / "xim-x-<tool>"
    std::filesystem::path xim_tool_root(const Env& env, std::string_view tool);

    // xim tool versioned: xpkgs_base / "xim-x-<tool>" / "<version>"
    std::filesystem::path xim_tool(const Env& env, std::string_view tool,
                                   std::string_view version);

    // ── A declared `[xlings] deps` entry, and where its payload landed ──────
    //
    // The two helpers above hardcode the `xim` namespace, which was right
    // while the only callers were mcpp's own tools (ninja, nasm). A manifest
    // may name any namespace — `local:qemu-riscv` during development,
    // `scode:…` for a source-built package — and the store spells an installed
    // package `<namespace>-x-<name>/<version>`.
    //
    // Here rather than in the build layer because THIS module already owns
    // that layout (`xpkgs_base`, `xim_tool`). A second place deriving it is
    // the shape this codebase has paid for repeatedly: it does not fail when
    // you add it, it fails later, somewhere else.
    struct XpkgRef {
        std::string ns;       // "xim" when the spec omitted one
        std::string name;
        std::string version;  // empty = unpinned
    };

    // Parse `[<ns>:]<name>[@<version>]` exactly as a manifest writes it.
    XpkgRef parse_xpkg_ref(std::string_view spec);

    // Where that package's payload is, or nullopt if it is not installed.
    //
    // The answer is the one xlings gave when it resolved the address. A
    // recorded resolution (below) answers first; without one, the request
    // selects among the installed directories under xlings' own version
    // grammar (mcpp.xpkg_version): a literal directory, then written-prefix
    // equality for three or more segments (1.8.12 matches 1.8.12.4 and never
    // 1.9.0), a prefix range for one or two (1.7 matches 1.7.0.1, #712), and
    // ranges for operators. An unpinned ref takes the highest version present.
    std::optional<std::filesystem::path>
    xpkg_payload(const Env& env, const XpkgRef& ref);

    // The grammar half of that resolution against an explicit xpkgs base,
    // without the record. Exists so the rule is testable without a home.
    std::optional<std::filesystem::path>
    xpkg_payload_at(const std::filesystem::path& xpkgsBase, const XpkgRef& ref);

    // ── What xlings resolved each requested address to ──────────────────
    //
    // xlings >= 2026.9.27.1 reports, for every address an install was asked
    // for, the version it selected and where the payload is (the
    // `install_targets` interface event, protocol 1.1), on every path
    // including "everything was already installed". mcpp keeps one record per
    // (xpkgs base, address) under <MCPP_HOME>/provisioned/resolved/, so the
    // question "which payload did `libglvnd@1.7` select" has xlings' answer
    // wherever it is asked -- by the root, by a member, by a dependency's build
    // program. The grammar above answers only when no record exists: an older
    // xlings, or a payload installed outside mcpp.
    //
    // The capability is detected by the event's presence, not by a version
    // number: an xlings that does not emit it simply leaves no record.
    struct ResolvedTarget {
        std::string           request;    // the address as it was sent
        std::string           ns;
        std::string           name;
        std::string           version;    // what the request resolved to
        int                   revision = 0;
        std::string           status;     // installed | already_present | failed
        std::filesystem::path payloadDir;
    };

    // The targets of one `install_targets` payload (`{"targets":[...]}`).
    std::vector<ResolvedTarget> parse_install_targets(std::string_view payloadJson);

    // Record every resolved target whose payload exists. Written through a
    // temporary file and a rename, so concurrent builds never read half a
    // record.
    void record_resolutions(const Env& env, std::span<const ResolvedTarget> targets);

    // The recorded payload for that address, if the record exists and its
    // directory is still the package's payload under this env's store.
    std::optional<std::filesystem::path>
    recorded_payload(const Env& env, const XpkgRef& ref);

    // The packaging revision xlings recorded for the payload in `payloadDir`
    // (`.xpkg-install.json`, xlings 2026.9.27.1+; openxlings/xlings#620), and
    // 0 when it recorded none -- the reading xlings itself gives a record that
    // predates the field. `nullopt` when there is no record at all.
    std::optional<int> installed_revision(const std::filesystem::path& payloadDir);

    // From compiler binary, climb parent dirs to find "xpkgs" directory.
    // Replaces 3 duplicate implementations in flags.cppm, ninja_backend.cppm,
    // stdmod.cppm.
    std::optional<std::filesystem::path>
    xpkgs_from_compiler(const std::filesystem::path& compilerBin);

    // The subos a TOOLCHAIN belongs to (mcpp#352).
    //
    // Derived from the compiler rather than from a global: a build already
    // knows which home its toolchain came from, and asking a second source
    // would let the two disagree — the "one question, several answerers" shape
    // that the same investigation found four times over on the xlings side.
    //
    // A PURE legacy ownership derivation, with no environment override in it.
    // Project runtime selection no longer calls this helper: it goes through
    // RuntimeSelection -> RuntimeBinding, which is rooted in mcpp.toml and the
    // configured xlings home. This remains useful for locating payload-adjacent
    // tools owned by a compiler installation.
    //
    // Empty when the toolchain is not sandbox-resident — a system compiler is
    // the user's explicit choice of the host world, and there is no subos
    // speaking for it.
    std::optional<std::filesystem::path>
    subos_dir_of(const std::filesystem::path& compilerBin);

    // Find a sibling xim tool relative to a compiler binary.
    // e.g. find_sibling_tool(gcc_bin, "binutils") returns highest version
    // dir of xim-x-binutils.
    std::optional<std::filesystem::path>
    find_sibling_tool(const std::filesystem::path& compilerBin,
                      std::string_view tool);

    // Find a binary inside a sibling tool (e.g. binutils/bin/ar,
    // ninja/ninja).
    std::optional<std::filesystem::path>
    find_sibling_binary(const std::filesystem::path& compilerBin,
                        std::string_view tool,
                        std::string_view binaryRelPath);

    // Find a sibling package across all index prefixes.
    // e.g. find_sibling_package(gcc_bin, "linux-headers") searches for
    // xim-x-linux-headers, scode-x-linux-headers, etc.
    // Metadata-only dirs (.xim-installed/.xpkg.lua husks left by delegating
    // index packages) never qualify; when requiredRelPath is given, only a
    // version dir containing it qualifies (the payload may live under a
    // different prefix than the husk — issue #120).
    std::optional<std::filesystem::path>
    find_sibling_package(const std::filesystem::path& compilerBin,
                         std::string_view packageName,
                         std::string_view requiredRelPath = {});

    // xpkgs root of the ACTIVE mcpp home ($MCPP_HOME or ~/.mcpp). Payload
    // discovery consults this in addition to compiler siblings: an
    // inherited/symlinked compiler resolves into its owner home, while the
    // active home may own (or have just installed) the sysroot payloads.
    std::optional<std::filesystem::path> active_home_xpkgs();

    // Like find_sibling_tool, but anchored at the active home's xpkgs.
    // Searches across index prefixes (xim-x-, scode-x-, …) with the same
    // husk/requiredRelPath rules as find_sibling_package.
    std::optional<std::filesystem::path>
    find_home_tool(std::string_view tool,
                   std::string_view requiredRelPath = {});

    // index data root: env.home / "data"
    std::filesystem::path index_data(const Env& env);

    // sandbox init marker: env.home / "subos" / "default" / ".xlings.json"
    std::filesystem::path sandbox_init_marker(const Env& env);

} // namespace paths

// ─── Shell quoting ──────────────────────────────────────────────────

// Shell-escape (single-quote) a string for the command line.
std::string shq(std::string_view s);

// `shq` for an argument that may carry a shell metacharacter (JSON, `>=`).
// See `shell::quote_windows_through_cmd`.
std::string shq_meta(std::string_view s);

// ─── Shell command builders ─────────────────────────────────────────

// Build the standard xlings command prefix with proper env vars.
// cd '<home>' && env -u XLINGS_PROJECT_DIR PATH=<sandbox_bin>:"$PATH"
//     XLINGS_HOME='<home>' '<binary>'
std::string build_command_prefix(const Env& env);

// The Windows spelling of that prefix, compiled on every host so that it is
// tested from any: `cd /d "<home>" && "<binary>"`. The variables are applied
// by ScopedInvocationEnv instead.
//
// THE WORKING DIRECTORY IS THE HOME ON BOTH PLATFORMS (#726). xlings enters
// project mode by walking up from its working directory to a `.xlings.json`.
// Started from a project that has one -- a project whose own mcpp is pinned
// there -- the registry's xlings adopted that project and wrote the shims of
// mcpp's toolchain and payloads (`cl`, `link`, `cmake`, ...) into the
// project's SubOS, whose `bin` is on PATH wherever the project's shell is.
std::string windows_command_prefix(const Env& env);

// THE ENVIRONMENT OF ONE XLINGS INVOCATION (#614), decided once. Each entry is
// a variable, its value, and whether it is present at all. Global mode is an
// absent XLINGS_PROJECT_DIR, because xlings resolves its subos scope from that
// variable. POSIX renders the decision into the command prefix (`env -u` and
// `K=V`); Windows applies it to the process through ScopedInvocationEnv.
//
// XLINGS_ACTIVE_SUBOS IS ALWAYS ABSENT. A shell that ran `xlings subos use
// <name>` exports it, and xlings ranks it above the home's own `activeSubos`.
// It names a SubOS of the shell's xlings home; mcpp's registry is a different
// home, whose paths mcpp derives from `subos/default`. Inherited, it made the
// registry install mcpp's tools and a project's payloads into a SubOS of the
// shell's name, where mcpp does not look (measured on 2026.9.14.2: a fresh
// home's `ninja` and `patchelf` landed in `registry/subos/<name>/bin`, and the
// pkg-config view `mcpp::pkg_config_libdir()` names stayed empty).
struct InvocationVar {
    std::string name;
    std::string value;
    bool        present = true;
};
std::vector<InvocationVar> invocation_env(const Env& env);

// Applies `invocation_env` and the sandbox's `bin` in front of PATH to this
// process for the guard's lifetime on Windows, and restores every prior value
// when the guard ends. On POSIX the command prefix carries them and the guard
// does nothing. Every function that runs a command built by
// `build_command_prefix` holds one while the command runs.
//
// NOTHING OUTLIVES THE INVOCATION (#726). The process environment after an
// xlings invocation is the one before it, so a build that installed a payload
// starts ninja with the environment a build that installed nothing does. The
// PATH prefix used to stay: the sandbox's `bin` holds the shims `xim:llvm`
// registers on Windows (`cl`, `link`, `lib`, `rc`), and every action of a
// first build then met them in front of MSVC's tools -- vcpkg's compiler
// detection failed there while the second build passed.
class ScopedInvocationEnv {
public:
    explicit ScopedInvocationEnv(const Env& env);
    ~ScopedInvocationEnv();
    ScopedInvocationEnv(const ScopedInvocationEnv&) = delete;
    ScopedInvocationEnv& operator=(const ScopedInvocationEnv&) = delete;

private:
    // One entry per scope variable the guard applied: its name and the value
    // it had before, if any. Plain members, not `std::optional<std::string>`,
    // which clang on the MSVC ABI does not copy inside a vector element.
    struct Saved {
        std::string name;
        bool        hadPrevious = false;
        std::string previous;
    };
    std::vector<Saved> saved_;
};

// Build full xlings interface command.
// <prefix> interface <capability> --args '<argsJson>' 2>/dev/null
std::string build_interface_command(const Env& env,
                                    std::string_view capability,
                                    std::string_view argsJson);

// ─── NDJSON event types ─────────────────────────────────────────────

struct ProgressEvent {
    std::string  phase;      // "download", "extract", "configure", ...
    int          percent;    // 0..100
    std::string  message;
};

struct LogEvent {
    std::string  level;      // "debug" | "info" | "warn" | "error"
    std::string  message;
};

struct DataEvent {
    std::string  dataKind;   // "install_plan", "styled_list", ...
    std::string  payloadJson;// raw JSON (let caller parse)
};

struct ErrorEvent {
    std::string  code;
    std::string  message;
    std::string  hint;
    bool         recoverable = false;
};

struct ResultEvent {
    int          exitCode = 0;
    std::string  dataJson;   // additional payload, may be empty
};

using Event = std::variant<ProgressEvent, LogEvent, DataEvent,
                           ErrorEvent, ResultEvent>;

// Parse one NDJSON line into an Event.
std::optional<Event> parse_event_line(std::string_view line);

// ─── JSON extraction helpers (for NDJSON parsing) ───────────────────

std::string extract_string(std::string_view text, std::string_view key);
std::optional<long long> extract_int(std::string_view text, std::string_view key);
std::optional<bool> extract_bool(std::string_view text, std::string_view key);
std::string extract_object(std::string_view text, std::string_view key);

// ─── Subprocess call ────────────────────────────────────────────────

struct CallResult {
    int                          exitCode = 0;
    std::vector<DataEvent>       dataEvents;
    std::optional<ErrorEvent>    error;
    std::string                  resultJson;
    // xlings' own error-level lines from its stderr, the last few, kept only
    // when the call failed (#614). The NDJSON error event carries a summary
    // ("config hook failed"); the line that says WHICH binding was rejected and
    // why is a log line on stderr, which this call used to discard.
    std::vector<std::string>     stderrTail;
};

// The error-level lines of an xlings invocation's stderr, the last `limit` of
// them (#614). A line is kept when it contains "error" in any case, contains
// `E_`, or starts with `[xim]`; a trailing carriage return is dropped. Exported
// so the selection is stated as a unit test rather than through a failing
// install.
std::vector<std::string> stderr_error_tail(std::string_view text, std::size_t limit = 20);

struct EventHandler {
    virtual ~EventHandler() = default;
    virtual void on_progress(const ProgressEvent&) {}
    virtual void on_log     (const LogEvent&)     {}
    virtual void on_data    (const DataEvent&)    {}
    virtual void on_error   (const ErrorEvent&)   {}
    virtual void on_result  (const ResultEvent&)  {}
};

std::expected<CallResult, std::string>
call(const Env& env, std::string_view capability,
     std::string_view argsJson, EventHandler* handler = nullptr);

// ─── Bootstrap progress types ───────────────────────────────────────

struct BootstrapFile {
    std::string  name;             // xim package id, e.g. "xim:patchelf@0.18.0"
    double       downloadedBytes = 0;
    double       totalBytes      = 0;
    bool         started         = false;
    bool         finished        = false;
};

struct BootstrapProgress {
    std::vector<BootstrapFile>  files;
    double                      elapsedSec = 0;
};

using BootstrapProgressCallback = std::function<void(const BootstrapProgress&)>;

// Run xlings install with progress callback (used by bootstrap functions).
// When not `quiet` and stderr is a TTY, an elapsed-time spinner is shown
// during the (otherwise silent) direct install so first-run doesn't look
// frozen.
int install_with_progress(const Env& env, std::string_view target,
                          const BootstrapProgressCallback& cb,
                          bool quiet = false);

// Run direct `xlings install <target> -y`.
// Used as a fallback when the NDJSON interface install path fails.
int install_direct(const Env& env, std::string_view target, bool quiet = false);

// ─── Sandbox lifecycle ──────────────────────────────────────────────

// Write .xlings.json seed file.
//
// The `mirror` default is "auto": xlings' own adaptive mirror module
// (xlings.core.mirror.adaptive — latency-probed with per-download failover and
// failure penalisation) then picks the best reachable host per download. This
// replaces the historic hardcoded "CN", which FORCED the CN mirror and disabled
// that mechanism — stranding overseas users and GitHub-hosted CI on a
// slow/unreachable gitcode. An explicit `mcpp self config --mirror CN|GLOBAL`
// still writes that fixed value (config priority). Mirror selection is xlings'
// responsibility; mcpp just declines to override it by default.
// The project's build-environment payload (L-1), materialized 1:1 into the
// `.xlings.json` keys xlings already understands. Plain types (no manifest
// dependency); the caller fills it from a manifest's [xlings] section.
struct ProjectEnv {
    std::vector<std::string>                          deps;       // → "deps"
    std::vector<std::pair<std::string,std::string>>   workspace;  // → "workspace"
    std::string                                       subos;      // → "subos"
    // No `envs`: the key was materialised here and read by nothing. See
    // .agents/docs/2026-09-03-xlings-workspace-as-the-one-table.md §4.
    bool empty() const {
        return deps.empty() && workspace.empty() && subos.empty();
    }
};

// One index_repos entry for seed_xlings_json. `artifact`/`source` are the
// xlings >= 0.4.68 per-repo artifact-sync fields (#269); empty means "do not
// emit the key", which keeps pre-artifact output byte-identical and matches
// xlings' "undeclared = plain git" semantics. Older xlings ignores both keys.
struct SeedRepo {
    std::string name;
    std::string url;
    std::string artifact;   // artifact source base, e.g. https://github.com/xlings-res/mcpp-index
    std::string source;     // "auto" | "artifact" | "git"
    // Non-empty: `artifact` is written as the region object
    // {"GLOBAL": artifact, "CN": artifactCn} (xlings #377).
    std::string artifactCn;
};

void seed_xlings_json(const Env& env,
                      std::span<const SeedRepo> repos,
                      std::string_view mirror = "auto",
                      const ProjectEnv& penv = {});

// Persist the xlings mirror selection in .xlings.json via xlings itself.
int config_show(const Env& env);
int config_set_mirror(const Env& env, std::string_view mirror, bool quiet = false);

// Run xlings self init.
void ensure_init(const Env& env, bool quiet);

// Ensure patchelf is installed.
void ensure_patchelf(const Env& env, bool quiet,
                     const BootstrapProgressCallback& cb);

// Ensure ninja is installed.
void ensure_ninja(const Env& env, bool quiet,
                  const BootstrapProgressCallback& cb);

// Fast, side-effect-free probe for a usable nasm (>= 2.16): PATH first (CI
// images ship one), the mcpp sandbox second. NEVER triggers an install —
// pure lookup. A build that needs to *provision* nasm goes through the
// toolchain's synchronous fetcher gate
// (mcpp::pm::Fetcher::resolve_xpkg_path("xim:nasm@<version>", autoInstall,
// ...)) from mcpp.build.prepare, the same gate the compiler toolchain uses:
// this module is a LEAF dependency and must not import mcpp.config /
// mcpp.fetcher (#232 — the old bespoke `ensure_nasm` install path skipped
// the index refresh and downgraded install failure to a warning).
std::optional<std::filesystem::path> find_usable_nasm(const Env& env);

// Whether `find_usable_nasm` answered with the host's assembler rather than
// the sandbox's. A host tool that reaches a build is named in the report.
bool nasm_is_from_host(const Env& env, const std::filesystem::path& bin);

// Locate an already-installed nasm inside the mcpp sandbox
// ($XLINGS_HOME/data/xpkgs/xim-x-nasm/<version>/...). Pure lookup: no PATH
// probe, no install. Called lazily — only when a build plan actually
// contains .asm units — after a provisioning gate has landed the package;
// the caller HARD-FAILS on nullopt, never silently skips assembly sources.
std::optional<std::filesystem::path> find_sandbox_nasm(const Env& env);

// ─── Index freshness ────────────────────────────────────────────────

// How long a just-completed index sync suppresses the next one.
//
// Re-running a multi-repo sync because a SECOND package is also missing cannot
// help: the first sync already fetched everything upstream had, so a package
// still absent afterwards is absent upstream. Lives here, in the leaf module,
// because both users need it and the policy layer (mcpp.pm.index_refresh)
// imports this module — the reverse would be a cycle.
inline constexpr std::int64_t kIndexRefreshDebounceSeconds = 120;

// ─── Bounds on the xlings children mcpp starts (#648 A3) ───────────────
//
// Every xlings invocation on the planning path runs through
// `run_streaming_bounded`: the child owns a process group (a job object on
// Windows) that dies with mcpp, and it is bounded. Before, an `xlings update`
// on a connection that never answered held `emit build-database` for eleven
// minutes, and outlived the mcpp its caller had killed.
//
// Which bound fits depends on what the child says while it works:
//   - the index refresh prints little and is small: a TOTAL bound, configured
//     by `[index] refresh_timeout`. A timed-out refresh is a failed refresh,
//     which the build already survives when the local index can answer;
//   - an install through the NDJSON interface emits a heartbeat after five
//     seconds of silence (xlings interface.cpp), so no output for
//     kInterfaceIdleTimeout means the xlings process itself is wedged, while a
//     slow download that makes progress is never cut short: an IDLE bound;
//   - a direct `xlings install -y` has its output sent to the null device, so
//     only a total bound can apply, and it is generous: a toolchain archive on a
//     slow link is not a hang;
//   - a local command (the one-time sandbox init) gets a total bound.
inline constexpr std::chrono::seconds kDefaultIndexRefreshTimeout{120};
inline constexpr std::chrono::seconds kInterfaceIdleTimeout{300};
inline constexpr std::chrono::seconds kDirectInstallTimeout{3 * 3600};
inline constexpr std::chrono::seconds kLocalCommandTimeout{600};

// Set once from `[index] refresh_timeout` when the configuration is loaded;
// zero or negative restores the default.
void set_index_refresh_timeout(std::chrono::seconds timeout);
std::chrono::seconds index_refresh_timeout();

// Check whether the default mcpplibs index data exists and is fresh
// (within ttlSeconds).
// Returns true if index is present and fresh, false otherwise.
bool is_index_fresh(const Env& env, std::int64_t ttlSeconds);

// Check whether xlings' official xim index data exists and is fresh.
// This is separate from mcpp's default mcpplibs index because xlings
// toolchains live in xim-pkgindex, while modular libraries live in mcpplibs.
bool is_official_index_fresh(const Env& env, std::int64_t ttlSeconds);

// Check whether a specific package file exists in xlings' official xim index
// and the index is fresh. This catches restored CI caches that have an index
// directory and marker but predate a package added later.
bool is_official_package_index_fresh(const Env& env,
                                     std::string_view packageName,
                                     std::int64_t ttlSeconds);

// Run `xlings update` to refresh all index repos. Streams output to stderr.
// Returns the xlings exit code.
int update_index(const Env& env, bool quiet = false);

// Ensure the local index is present and fresh. Runs `xlings update` if
// the index is missing or older than ttlSeconds. Idempotent and quiet
// when no update is needed.
void ensure_index_fresh(const Env& env, std::int64_t ttlSeconds, bool quiet = false);

// Whether xlings' official xim index on disk carries a descriptor for
// `packageName`. Offline and read-only. Whether a miss may refresh the index is
// not decided here: that is mcpp.pm.refresh_policy's `decide_for_miss`, so
// `[index] auto_refresh`, `--offline` and the debounce apply to it as they do
// to every other refresh (mcpp-community/mcpp#648 A5).
bool official_package_present(const Env& env, std::string_view packageName);

// ─── Index status (read-only, offline) ──────────────────────────────
// Snapshot of a local index directory — computed without touching the
// network, for `mcpp index status`.
struct IndexStatus {
    std::filesystem::path      dir;        // on-disk index directory
    bool                       present;    // pkgs/ tree exists locally
    bool                       fresh;      // refreshed within ttlSeconds
    std::int64_t               ageSeconds; // since last refresh marker, -1 if unknown
    std::optional<std::string> rev;        // index CONTENT identity, nullopt if absent
};
IndexStatus default_index_status(const Env& env, std::int64_t ttlSeconds);
IndexStatus official_index_status(const Env& env, std::int64_t ttlSeconds);

// The index's own content identity, as written by xlings into
// `<indexDir>/.xlings-index-version` when it materializes the tree.
//
// OPAQUE BY CONTRACT. Observed values are a 7-char short sha for the artifact-
// distributed indexes (`mcpp-index-8d67478.tar.gz` → `8d67478`) but a DATE
// VERSION for the sub-indexes (`xim-index-awesome-2026.7.30.1.tar.gz` →
// `2026.7.30.1`). Never parse it, never assume a length — only compare it for
// equality and print it. Returns nullopt when the file is missing (a local
// `path` index has none) or blank; callers must degrade, never hard-fail.
std::optional<std::string> index_revision(const std::filesystem::path& indexDir);

// ─── run_capture utility ────────────────────────────────────────────

std::expected<std::string, std::string> run_capture(const std::string& cmd);

} // namespace mcpp::xlings

// ═══════════════════════════════════════════════════════════════════════
// Implementation
// ═══════════════════════════════════════════════════════════════════════

namespace mcpp::xlings {

namespace {

// Right-pad a verb to 12 columns for bootstrap status lines, on standard
// error: the stream mcpp.ui narrates on.
void print_status(std::string_view verb, std::string_view msg) {
    constexpr std::size_t W = 12;
    if (verb.size() >= W) {
        std::println(stderr, "{} {}", verb, msg);
    } else {
        std::println(stderr, "{}{} {}", std::string(W - verb.size(), ' '), verb, msg);
    }
}

std::filesystem::path default_index_dir(const Env& env) {
    return paths::index_data(env) / "mcpplibs";
}

std::filesystem::path official_index_dir(const Env& env) {
    return paths::index_data(env) / "xim-pkgindex";
}

std::filesystem::path index_pkgs_dir(const std::filesystem::path& indexDir) {
    return indexDir / "pkgs";
}

std::filesystem::path index_refresh_marker(const std::filesystem::path& indexDir) {
    return indexDir / ".mcpp-index-updated";
}

std::filesystem::path index_version_file(const std::filesystem::path& indexDir) {
    return indexDir / ".xlings-index-version";
}

std::filesystem::path official_package_file(const Env& env, std::string_view packageName) {
    if (packageName.empty()) return {};
    std::string name(packageName);
    return official_index_dir(env) / "pkgs" / std::string(1, name[0]) / (name + ".lua");
}

std::string json_escaped_path_probe(std::filesystem::path path) {
    auto value = path.string();
    std::string escaped;
    escaped.reserve(value.size() * 2);
    for (char c : value) {
        if (c == '\\') escaped += "\\\\";
        else escaped.push_back(c);
    }
    return escaped;
}

bool official_index_cache_matches_package_file(const Env& env,
                                               std::string_view packageName) {
    auto cache = official_index_dir(env) / ".xlings-index-cache.json";
    if (!std::filesystem::exists(cache)) return true;

    auto pkg = official_package_file(env, packageName);
    if (pkg.empty()) return false;

    std::ifstream is(cache);
    if (!is) return false;
    std::string body((std::istreambuf_iterator<char>(is)), {});
    auto rawPath = pkg.string();
    return body.find(rawPath) != std::string::npos
        || body.find(json_escaped_path_probe(pkg)) != std::string::npos;
}

void mark_index_refreshed(const std::filesystem::path& indexDir) {
    if (!std::filesystem::exists(index_pkgs_dir(indexDir))) return;
    std::error_code ec;
    std::filesystem::create_directories(indexDir, ec);
    auto marker = index_refresh_marker(indexDir);
    {
        std::ofstream os(marker, std::ios::trunc);
        if (!os) return;
        os << "ok\n";
    }
    std::filesystem::last_write_time(
        marker, std::filesystem::file_time_type::clock::now(), ec);
}

void mark_known_indexes_refreshed(const Env& env) {
    // Project-scoped envs used to return early here, which left the global
    // indexes unmarked whenever a project with custom `[indices]` triggered the
    // sync — so `mcpp index status` reported "unknown" forever on exactly the
    // machines that refresh most. The marker is advisory (it debounces and it
    // dates the status line; it is NOT what decides whether a refresh is
    // needed), so marking it from either env is both safe and more accurate.
    mark_index_refreshed(default_index_dir(env));
    mark_index_refreshed(official_index_dir(env));
}

bool is_index_dir_fresh(const std::filesystem::path& indexDir, std::int64_t ttlSeconds) {
    std::error_code ec;
    if (!std::filesystem::exists(index_pkgs_dir(indexDir))) return false;

    auto marker = index_refresh_marker(indexDir);
    if (!std::filesystem::exists(marker)) return false;

    auto newest = std::filesystem::last_write_time(marker, ec);
    if (ec) return false;

    auto now = std::filesystem::file_time_type::clock::now();
    auto age = std::chrono::duration_cast<std::chrono::seconds>(now - newest);
    // A marker stamped in the FUTURE yields a negative age, which the plain
    // `age < ttl` test read as "fresh" — and stayed fresh until the wall clock
    // caught up, potentially for years. Future timestamps are routine: clock
    // skew in containers/VMs, a tar that preserved mtimes, a restored CI cache.
    // An unusable timestamp means "unknown", and unknown must mean stale.
    if (age.count() < 0) return false;
    return age.count() < ttlSeconds;
}

// Seconds since the index's refresh marker was last touched, or -1 if the
// marker is missing/unreadable. Read-only — no network, no side effects.
std::int64_t index_age_seconds(const std::filesystem::path& indexDir) {
    std::error_code ec;
    auto marker = index_refresh_marker(indexDir);
    auto newest = std::filesystem::last_write_time(marker, ec);
    if (ec) return -1;
    auto now = std::filesystem::file_time_type::clock::now();
    auto age = std::chrono::duration_cast<std::chrono::seconds>(now - newest).count();
    return age < 0 ? -1 : age;   // future stamp → "unknown", same rule as above
}

// Defined inside the anonymous namespace so `index_status_for` (also here) can
// use it; the exported `index_revision` below forwards to it.
std::optional<std::string> read_index_revision(const std::filesystem::path& indexDir) {
    std::ifstream is(index_version_file(indexDir), std::ios::binary);
    if (!is) return std::nullopt;
    std::string body((std::istreambuf_iterator<char>(is)), {});
    // The observed files carry no trailing newline, but trim anyway: this value
    // is compared for equality and printed, and a stray \r from a Windows-side
    // writer would otherwise turn "same rev" into "changed rev" every run.
    auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!body.empty() && isSpace(static_cast<unsigned char>(body.back())))
        body.pop_back();
    std::size_t b = 0;
    while (b < body.size() && isSpace(static_cast<unsigned char>(body[b]))) ++b;
    body.erase(0, b);
    if (body.empty()) return std::nullopt;
    return body;
}

IndexStatus index_status_for(const std::filesystem::path& indexDir,
                             std::int64_t ttlSeconds) {
    std::error_code ec;
    bool present = std::filesystem::exists(index_pkgs_dir(indexDir), ec) && !ec;
    return IndexStatus{
        .dir        = indexDir,
        .present    = present,
        .fresh      = is_index_dir_fresh(indexDir, ttlSeconds),
        .ageSeconds = index_age_seconds(indexDir),
        .rev        = read_index_revision(indexDir),
    };
}

void write_file(const std::filesystem::path& p, std::string_view content) {
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream os(p);
    os << content;
}

std::string json_escape(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (unsigned char ch : value) {
        switch (ch) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (ch < 0x20) {
                    out += std::format("\\u{:04x}", static_cast<unsigned>(ch));
                } else {
                    out.push_back(static_cast<char>(ch));
                }
        }
    }
    return out;
}

// LineScan: cheap field extraction for bootstrap install progress lines.
// Handles flat JSON; no nested array/object — the keys we extract are
// all leaves.
struct LineScan {
    std::string_view s;
    std::string find_str(std::string_view key) const {
        std::string n = std::format("\"{}\":\"", key);
        auto p = s.find(n);
        if (p == std::string_view::npos) return "";
        p += n.size();
        std::string out;
        while (p < s.size() && s[p] != '"') {
            if (s[p] == '\\' && p + 1 < s.size()) {
                out.push_back(s[p+1]); p += 2; continue;
            }
            out.push_back(s[p++]);
        }
        return out;
    }
    double find_num(std::string_view key) const {
        std::string n = std::format("\"{}\":", key);
        auto p = s.find(n);
        if (p == std::string_view::npos) return 0;
        p += n.size();
        auto e = p;
        while (e < s.size()
            && (std::isdigit(static_cast<unsigned char>(s[e]))
                || s[e] == '.' || s[e] == '-' || s[e] == '+'
                || s[e] == 'e' || s[e] == 'E')) ++e;
        try { return std::stod(std::string(s.substr(p, e - p))); }
        catch (...) { return 0; }
    }
    bool find_bool(std::string_view key) const {
        std::string n = std::format("\"{}\":", key);
        auto p = s.find(n);
        if (p == std::string_view::npos) return false;
        p += n.size();
        return s.size() - p >= 4 && s.substr(p, 4) == "true";
    }
};

} // anonymous namespace

// ─── Index identity ─────────────────────────────────────────────────

std::optional<std::string> index_revision(const std::filesystem::path& indexDir) {
    return read_index_revision(indexDir);
}

// ─── run_capture ────────────────────────────────────────────────────

std::expected<std::string, std::string> run_capture(const std::string& cmd) {
    auto r = mcpp::platform::process::capture(cmd);
    if (r.exit_code != 0 && r.output.empty())
        return std::unexpected("command failed: " + cmd);
    return r.output;
}

// ─── Shell quoting ──────────────────────────────────────────────────

std::string shq(std::string_view s) {
    return mcpp::platform::shell::quote(s);
}

// The same, for an argument that may carry a shell metacharacter -- which the
// JSON payloads below do, and which a version constraint spelled `>=` does.
// See `quote_windows_through_cmd` for what `shq` alone leaves unprotected.
std::string shq_meta(std::string_view s) {
    return mcpp::platform::shell::quote_through_shell(s);
}

// ─── Path helpers ───────────────────────────────────────────────────

namespace paths {

std::filesystem::path xpkgs_base(const Env& env) {
    return env.home / "data" / "xpkgs";
}

std::filesystem::path sandbox_bin(const Env& env) {
    return env.home / "subos" / "default" / "bin";
}

std::filesystem::path sysroot(const Env& env) {
    return env.home / "subos" / "default";
}

std::filesystem::path xim_tool_root(const Env& env, std::string_view tool) {
    return xpkgs_base(env) / std::format("xim-x-{}", tool);
}

std::filesystem::path xim_tool(const Env& env, std::string_view tool,
                               std::string_view version) {
    return xpkgs_base(env) / std::format("xim-x-{}", tool) / std::string(version);
}

// Parse `[<ns>:]<name>[@<version>]` exactly as a manifest writes it.
XpkgRef parse_xpkg_ref(std::string_view spec) {
    XpkgRef r;
    if (auto colon = spec.find(':'); colon != std::string_view::npos) {
        r.ns = std::string(spec.substr(0, colon));
        spec = spec.substr(colon + 1);
    } else {
        // The manifest's own default, spelled once here rather than at each
        // call site: `deps = ["ninja"]` and `deps = ["xim:ninja"]` name the
        // same package.
        r.ns = "xim";
    }
    if (auto at = spec.find('@'); at != std::string_view::npos) {
        r.name    = std::string(spec.substr(0, at));
        r.version = std::string(spec.substr(at + 1));
    } else {
        r.name = std::string(spec);
    }
    return r;
}

// Where that package's payload is, or nullopt if it is not installed.
//
// THE ANSWER IS XLINGS' ANSWER. The address was resolved by xlings, so the
// payload it selected is found with xlings' grammar (mcpp.xpkg_version), not
// with the Cargo grammar mcpp reads its own dependencies with. They disagree
// where it matters (#712): `libglvnd@1.7` installed `1.7.0.1`, and the Cargo
// reading of `1.7` could not see a four-segment directory at all, so
// `mcpp::xpkg_dir` answered "" for a payload that was on disk.
//
// The request selects among the installed directories exactly as xlings would
// among index keys (`select_installed`): a literal name first, then a bare
// version of three or more segments as written-prefix equality (1.8.12 matches
// 1.8.12.x and never 1.9.0), one or two segments as a prefix range, operators
// as ranges; with no version, the highest installed one.
std::optional<std::filesystem::path>
xpkg_payload(const Env& env, const XpkgRef& ref) {
    if (auto recorded = recorded_payload(env, ref)) return recorded;
    return xpkg_payload_at(xpkgs_base(env), ref);
}

std::optional<std::filesystem::path>
xpkg_payload_at(const std::filesystem::path& xpkgsBase, const XpkgRef& ref) {
    if (ref.name.empty()) return std::nullopt;
    const auto root = xpkgsBase / std::format("{}-x-{}", ref.ns, ref.name);
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) return std::nullopt;
    std::vector<std::string> installed;
    for (auto const& e : std::filesystem::directory_iterator(root, ec))
        if (e.is_directory(ec)) installed.push_back(e.path().filename().string());
    auto pick = mcpp::xpkg_version::select_installed(installed, ref.version);
    if (!pick) return std::nullopt;
    return root / *pick;
}


// ─── Resolution records ────────────────────────────────────────────

namespace {

// One file per (xpkgs base, address). The address is canonicalised through
// parse_xpkg_ref, so `libglvnd@1.7` and `xim:libglvnd@1.7` share a record, and
// the base is part of the key, so two homes never answer for each other.
std::filesystem::path record_path(const Env& env, const XpkgRef& ref) {
    std::uint64_t h = 1469598103934665603ull;   // FNV-1a
    const auto key = std::format("{}\n{}:{}@{}",
        xpkgs_base(env).generic_string(), ref.ns, ref.name, ref.version);
    for (unsigned char ch : key) { h ^= ch; h *= 1099511628211ull; }
    return mcpp::home::root() / "provisioned" / "resolved"
         / std::format("{:016x}.json", h);
}

} // namespace

std::vector<ResolvedTarget> parse_install_targets(std::string_view payloadJson) {
    std::vector<ResolvedTarget> out;
    auto j = nlohmann::json::parse(payloadJson, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object() || !j.contains("targets") || !j["targets"].is_array()) return out;
    auto str = [](const nlohmann::json& o, const char* k) {
        return o.contains(k) && o[k].is_string() ? o[k].get<std::string>() : std::string{};
    };
    for (auto const& t : j["targets"]) {
        if (!t.is_object()) continue;
        ResolvedTarget r;
        r.request    = str(t, "request");
        r.ns         = str(t, "namespace");
        r.name       = str(t, "name");
        r.version    = str(t, "version");
        r.status     = str(t, "status");
        r.payloadDir = str(t, "payload_dir");
        if (t.contains("revision") && t["revision"].is_number_integer())
            r.revision = t["revision"].get<int>();
        if (!r.request.empty()) out.push_back(std::move(r));
    }
    return out;
}

void record_resolutions(const Env& env, std::span<const ResolvedTarget> targets) {
    for (auto const& t : targets) {
        if (t.status == "failed" || t.payloadDir.empty()) continue;
        std::error_code ec;
        if (!std::filesystem::is_directory(t.payloadDir, ec)) continue;
        const auto path = record_path(env, parse_xpkg_ref(t.request));
        std::filesystem::create_directories(path.parent_path(), ec);
        nlohmann::json j;
        j["request"]     = t.request;
        j["namespace"]   = t.ns;
        j["name"]        = t.name;
        j["version"]     = t.version;
        j["revision"]    = t.revision;
        j["payload_dir"] = t.payloadDir.generic_string();
        auto tmp = path;
        tmp += std::format(".{}.tmp",
            std::chrono::steady_clock::now().time_since_epoch().count());
        {
            std::ofstream o{tmp, std::ios::binary | std::ios::trunc};
            if (!o) continue;
            o << j.dump();
        }
        std::filesystem::rename(tmp, path, ec);
        if (ec) std::filesystem::remove(tmp, ec);
    }
}

std::optional<int> installed_revision(const std::filesystem::path& payloadDir) {
    std::ifstream in{payloadDir / ".xpkg-install.json", std::ios::binary};
    if (!in) return std::nullopt;
    std::string text{std::istreambuf_iterator<char>(in), {}};
    auto j = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object()) return std::nullopt;
    if (!j.contains("revision") || !j["revision"].is_number_integer()) return 0;
    return j["revision"].get<int>();
}

std::optional<std::filesystem::path>
recorded_payload(const Env& env, const XpkgRef& ref) {
    if (ref.name.empty()) return std::nullopt;
    std::ifstream in{record_path(env, ref), std::ios::binary};
    if (!in) return std::nullopt;
    std::string text{std::istreambuf_iterator<char>(in), {}};
    auto j = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object() || !j.contains("payload_dir") || !j["payload_dir"].is_string())
        return std::nullopt;
    const std::filesystem::path dir = j["payload_dir"].get<std::string>();
    // A record answers only for a payload that is still there and still this
    // package's, in this env's store. Anything else -- the payload was
    // removed, the home moved -- falls through to the grammar, and the
    // provisioning check treats "no answer" as not installed (#716).
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return std::nullopt;
    const auto root = xpkgs_base(env) / std::format("{}-x-{}", ref.ns, ref.name);
    if (dir.parent_path().lexically_normal() != root.lexically_normal()) return std::nullopt;
    return dir;
}

std::optional<std::filesystem::path>
xpkgs_from_compiler(const std::filesystem::path& compilerBin) {
    for (auto p = compilerBin.parent_path();
         p.has_parent_path() && p != p.root_path();
         p = p.parent_path()) {
        if (p.filename() == "xpkgs") return p;
    }
    return std::nullopt;
}

std::optional<std::filesystem::path>
subos_dir_of(const std::filesystem::path& compilerBin) {
    auto xpkgs = xpkgs_from_compiler(compilerBin);
    if (!xpkgs) return std::nullopt;
    // <home>/data/xpkgs → <home>/subos/default. Spelled from the xpkgs dir
    // rather than from Env so a toolchain inherited from ANOTHER home resolves
    // to that home's subos, which is the one whose payloads the binary was
    // actually linked against.
    auto home = xpkgs->parent_path().parent_path();
    if (home.empty()) return std::nullopt;
    return home / "subos" / "default";
}

std::optional<std::filesystem::path>
find_sibling_tool(const std::filesystem::path& compilerBin,
                  std::string_view tool) {
    auto xpkgs = xpkgs_from_compiler(compilerBin);
    if (!xpkgs) return std::nullopt;

    auto root = *xpkgs / std::format("xim-x-{}", tool);
    std::error_code ec;
    if (!std::filesystem::exists(root, ec)) return std::nullopt;

    // The first version dir readdir yields. ORDER IS UNDEFINED -- this does
    // NOT return the highest, and the comment that said so was wrong for as
    // long as it stood. With two versions installed the answer varies by
    // filesystem, so a caller that needs a SPECIFIC version must name it;
    // see probe.cppm, where the version comes from the resolved runtime
    // binding rather than from this scan.
    //
    // A test asserting the old "highest" claim was attempted and removed: it
    // passed or failed by directory order, which makes both outcomes
    // uninformative.
    for (auto& v : std::filesystem::directory_iterator(root, ec)) {
        if (v.is_directory(ec)) return v.path();
    }
    return std::nullopt;
}

std::optional<std::filesystem::path> active_home_xpkgs() {
    // `mcpp::home::root()`, NOT A FOURTH DERIVATION OF IT.
    //
    // This function used to resolve the home itself — `$MCPP_HOME`, else
    // `$HOME/.mcpp` — which is two of the three answers `mcpp.home` gives. The
    // one it left out is SELF-CONTAINED MODE: a release tarball or
    // `xlings install mcpp` puts the binary at `<root>/bin/mcpp` and the
    // unpacked tree IS the home. On such an install this function answered
    // `$HOME/.mcpp`, so payload discovery reached a DIFFERENT home than
    // everything else in the process — and what payload discovery produces is
    // `-isystem` rows on every compile command.
    //
    // `mcpp.home`'s own header opens with this: "Every path under the mcpp
    // home must be derived from here. Before #311 this logic existed in three
    // places … the copies drifted." This was the fourth copy, and it drifted
    // the same way.
    auto xpkgs = mcpp::home::root() / "registry" / "data" / "xpkgs";
    std::error_code ec;
    if (!std::filesystem::exists(xpkgs, ec)) return std::nullopt;
    return xpkgs;
}

namespace {

// A version dir qualifies as a payload only if it has real content —
// dot-prefixed entries (.xim-installed, .xpkg.lua) are install metadata,
// and a dir holding nothing else is the husk a delegating index package
// leaves behind (the payload lives under another prefix; issue #120).
// When requiredRelPath is given, the dir must also contain that path.
bool payload_dir_qualifies(const std::filesystem::path& versionDir,
                           std::string_view requiredRelPath) {
    std::error_code ec;
    bool hasContent = false;
    for (auto& f : std::filesystem::directory_iterator(versionDir, ec)) {
        if (!f.path().filename().string().starts_with(".")) {
            hasContent = true;
            break;
        }
    }
    if (!hasContent) return false;
    if (!requiredRelPath.empty()
        && !std::filesystem::exists(versionDir / requiredRelPath, ec))
        return false;
    return true;
}

// Scan an xpkgs root across index prefixes (xim-x-, scode-x-, compat-x-, …)
// for the first qualifying version dir of `packageName`.
std::optional<std::filesystem::path>
find_package_in_xpkgs(const std::filesystem::path& xpkgs,
                      std::string_view packageName,
                      std::string_view requiredRelPath) {
    std::error_code ec;
    std::string suffix = std::format("-x-{}", packageName);
    for (auto& entry : std::filesystem::directory_iterator(xpkgs, ec)) {
        if (!entry.is_directory(ec)) continue;
        auto name = entry.path().filename().string();
        if (!name.ends_with(suffix)) continue;
        for (auto& v : std::filesystem::directory_iterator(entry.path(), ec)) {
            if (!v.is_directory(ec)) continue;
            if (payload_dir_qualifies(v.path(), requiredRelPath))
                return v.path();
        }
    }
    return std::nullopt;
}

} // namespace

std::optional<std::filesystem::path>
find_home_tool(std::string_view tool, std::string_view requiredRelPath) {
    auto xpkgs = active_home_xpkgs();
    if (!xpkgs) return std::nullopt;
    return find_package_in_xpkgs(*xpkgs, tool, requiredRelPath);
}

std::optional<std::filesystem::path>
find_sibling_binary(const std::filesystem::path& compilerBin,
                    std::string_view tool,
                    std::string_view binaryRelPath) {
    auto xpkgs = xpkgs_from_compiler(compilerBin);
    if (!xpkgs) return std::nullopt;

    auto root = *xpkgs / std::format("xim-x-{}", tool);
    std::error_code ec;
    if (!std::filesystem::exists(root, ec)) return std::nullopt;

    for (auto& v : std::filesystem::directory_iterator(root, ec)) {
        auto candidate = v.path() / std::string(binaryRelPath);
        if (std::filesystem::exists(candidate, ec))
            return candidate;
    }
    return std::nullopt;
}

std::optional<std::filesystem::path>
find_sibling_package(const std::filesystem::path& compilerBin,
                     std::string_view packageName,
                     std::string_view requiredRelPath) {
    auto xpkgs = xpkgs_from_compiler(compilerBin);
    if (!xpkgs) return std::nullopt;

    // Search across index prefixes: xim-x-, scode-x-, compat-x-, etc.
    if (auto found = find_package_in_xpkgs(*xpkgs, packageName, requiredRelPath))
        return found;

    // THE `~/.xlings` FALLBACK IS GONE, AND ITS REMOVAL IS THE POINT.
    //
    // It used to read: "Also check ~/.xlings/data/xpkgs/ (xlings global home)
    // as fallback." That was written when one machine had one home. It means
    // that a build whose `MCPP_HOME` names one tree can take a payload out of
    // ANOTHER, and what this function's callers do with the result is put it on
    // every compile command:
    //
    //     probe.cppm:448   linux-headers  →  -isystem <other home>/…/include
    //
    // ⇒ a hermetic build reaching outside its own sandbox for headers, with no
    // diagnostic, and with the resulting objects looking exactly like objects
    // built from the payload that was actually pinned. Reported as the second
    // half of mcpp#514, where a project's dependency units carried `-isystem`
    // rows naming a home the build was not using.
    //
    // THE DIRECTION IS THE SAFE ONE. Not finding a payload is reported —
    // `probe.cppm` already has the verbose branch for it, and a glibc build
    // that then fails at `<linux/limits.h>` says so at the first compile.
    // Finding the WRONG one says nothing at all, and this codebase has paid
    // for that shape before (`e2e-inherit-toolchain-corrupts-real-payloads`,
    // `dev-overlay-poisons-a-released-version`).
    return std::nullopt;
}

std::filesystem::path index_data(const Env& env) {
    return env.home / "data";
}

std::filesystem::path sandbox_init_marker(const Env& env) {
    return env.home / "subos" / "default" / ".xlings.json";
}

} // namespace paths

// ─── Shell command builders ─────────────────────────────────────────

std::vector<InvocationVar> invocation_env(const Env& env) {
    return {
        {"XLINGS_HOME", env.home.string(), true},
        {"XLINGS_PROJECT_DIR", env.projectDir.string(), !env.projectDir.empty()},
        {"XLINGS_ACTIVE_SUBOS", "", false},
    };
}

ScopedInvocationEnv::ScopedInvocationEnv(const Env& env) {
    if constexpr (mcpp::platform::is_windows) {
        auto save = [this](const std::string& name) {
            Saved s;
            s.name = name;
            if (auto prior = mcpp::platform::env::get(name)) {
                s.hadPrevious = true;
                s.previous = *prior;
            }
            saved_.push_back(s);
        };
        for (auto const& var : invocation_env(env)) {
            save(var.name);
            if (var.present) mcpp::platform::env::set(var.name, var.value);
            else             mcpp::platform::env::unset(var.name);
        }
        save("PATH");
        mcpp::platform::windows::prepend_path(paths::sandbox_bin(env).string());
    }
}

ScopedInvocationEnv::~ScopedInvocationEnv() {
    // Newest first, so a variable saved twice ends at its oldest value.
    for (auto it = saved_.rbegin(); it != saved_.rend(); ++it) {
        if (it->hadPrevious) mcpp::platform::env::set(it->name, it->previous);
        else                 mcpp::platform::env::unset(it->name);
    }
}

std::string windows_command_prefix(const Env& env) {
    return std::format("cd /d {} && {}",
        mcpp::platform::shell::quote_windows(env.home.string()),
        mcpp::platform::shell::quote_windows(env.binary.string()));
}

std::string build_command_prefix(const Env& env) {
    auto xvmBin = paths::sandbox_bin(env).string();
    if constexpr (mcpp::platform::is_windows) {
        // The environment is applied by the caller's ScopedInvocationEnv and
        // restored after the command; building a command changes nothing.
        return windows_command_prefix(env);
    } else {
        // `env` takes its `-u` operands before its assignments.
        std::string unset, assign;
        for (auto const& var : invocation_env(env)) {
            if (var.present) assign += std::format(" {}={}", var.name, shq(var.value));
            else             unset  += std::format(" -u {}", var.name);
        }
        return std::format("cd {} && env{} PATH={}:\"$PATH\"{} {}",
            shq(env.home.string()), unset, shq(xvmBin), assign,
            shq(env.binary.string()));
    }
}

std::string build_interface_command(const Env& env,
                                    std::string_view capability,
                                    std::string_view argsJson) {
    return std::format("{} interface {} --args {} {}",
        build_command_prefix(env), capability, shq_meta(argsJson),
        mcpp::platform::null_redirect);
}

// ─── JSON extraction helpers ────────────────────────────────────────

std::string extract_string(std::string_view text, std::string_view key) {
    auto needle = std::string{"\""} + std::string(key) + "\":\"";
    auto p = text.find(needle);
    if (p == std::string_view::npos) return "";
    p += needle.size();
    std::string out;
    while (p < text.size()) {
        char c = text[p++];
        if (c == '\\' && p < text.size()) {
            char nc = text[p++];
            switch (nc) {
                case 'n': out.push_back('\n'); break;
                case 't': out.push_back('\t'); break;
                case 'r': out.push_back('\r'); break;
                case '"': out.push_back('"');  break;
                case '\\': out.push_back('\\'); break;
                default: out.push_back(nc);
            }
        } else if (c == '"') {
            return out;
        } else {
            out.push_back(c);
        }
    }
    return out;
}

std::optional<long long> extract_int(std::string_view text, std::string_view key) {
    auto needle = std::string{"\""} + std::string(key) + "\":";
    auto p = text.find(needle);
    if (p == std::string_view::npos) return std::nullopt;
    p += needle.size();
    while (p < text.size() && text[p] == ' ') ++p;
    bool neg = false;
    if (p < text.size() && text[p] == '-') { neg = true; ++p; }
    long long n = 0;
    bool any = false;
    while (p < text.size() && std::isdigit(static_cast<unsigned char>(text[p]))) {
        n = n * 10 + (text[p++] - '0');
        any = true;
    }
    if (!any) return std::nullopt;
    return neg ? -n : n;
}

std::optional<bool> extract_bool(std::string_view text, std::string_view key) {
    auto needle = std::string{"\""} + std::string(key) + "\":";
    auto p = text.find(needle);
    if (p == std::string_view::npos) return std::nullopt;
    p += needle.size();
    while (p < text.size() && text[p] == ' ') ++p;
    if (text.substr(p, 4) == "true")  return true;
    if (text.substr(p, 5) == "false") return false;
    return std::nullopt;
}

std::string extract_object(std::string_view text, std::string_view key) {
    auto needle = std::string{"\""} + std::string(key) + "\":";
    auto p = text.find(needle);
    if (p == std::string_view::npos) return "";
    p += needle.size();
    while (p < text.size() && text[p] == ' ') ++p;
    if (p >= text.size() || (text[p] != '{' && text[p] != '[')) return "";
    char open  = text[p];
    char close = (open == '{') ? '}' : ']';
    int depth = 0;
    std::size_t start = p;
    bool in_string = false;
    while (p < text.size()) {
        char c = text[p];
        if (in_string) {
            if (c == '\\' && p + 1 < text.size()) { p += 2; continue; }
            if (c == '"') in_string = false;
            ++p; continue;
        }
        if (c == '"') { in_string = true; ++p; continue; }
        if (c == open)  { ++depth; }
        else if (c == close) {
            --depth;
            if (depth == 0) return std::string(text.substr(start, p - start + 1));
        }
        ++p;
    }
    return "";
}

// ─── NDJSON event parser ────────────────────────────────────────────

std::optional<Event> parse_event_line(std::string_view line) {
    auto kind = extract_string(line, "kind");
    if (kind == "progress") {
        ProgressEvent e;
        e.phase   = extract_string(line, "phase");
        e.percent = static_cast<int>(extract_int(line, "percent").value_or(0));
        e.message = extract_string(line, "message");
        return e;
    }
    if (kind == "log") {
        LogEvent e;
        e.level   = extract_string(line, "level");
        e.message = extract_string(line, "message");
        return e;
    }
    if (kind == "data") {
        DataEvent e;
        e.dataKind   = extract_string(line, "dataKind");
        e.payloadJson= extract_object(line, "payload");
        return e;
    }
    if (kind == "error") {
        ErrorEvent e;
        e.code        = extract_string(line, "code");
        e.message     = extract_string(line, "message");
        e.hint        = extract_string(line, "hint");
        e.recoverable = extract_bool(line, "recoverable").value_or(false);
        return e;
    }
    if (kind == "result") {
        ResultEvent e;
        e.exitCode = static_cast<int>(extract_int(line, "exitCode").value_or(0));
        return e;
    }
    // heartbeat and unknown kinds
    return std::nullopt;
}

// ─── Subprocess call ────────────────────────────────────────────────

std::expected<CallResult, std::string>
call(const Env& env, std::string_view capability,
     std::string_view argsJson, EventHandler* handler)
{
    ScopedInvocationEnv scope(env);   // #614
    // STDERR GOES TO A FILE, NOT TO THE NULL DEVICE AND NOT INTO STDOUT (#614).
    // Stdout is parsed line by line as NDJSON, so merging stderr into it could
    // split an event; discarding it lost the one line that names a rejection.
    // The file is read only when the call fails, and removed either way.
    const auto stderrFile = std::filesystem::temp_directory_path()
        / std::format("mcpp-xlings-{}.stderr",
                      std::chrono::steady_clock::now().time_since_epoch().count());
    auto cmd = std::format("{} interface {} --args {} 2>{}",
        build_command_prefix(env), capability, shq_meta(argsJson),
        mcpp::platform::is_windows
            ? std::format("\"{}\"", stderrFile.string())
            : shq(stderrFile.string()));

    // #238: under MCPP_VERBOSE=1 surface the exact xlings invocation so a
    // failing install_packages can be reproduced/inspected by hand. The
    // NDJSON child's own error/warn lines are surfaced by the caller's
    // EventHandler (e.g. InstallProgressHandler).
    mcpp::log::verbose("xlings",
        std::format("interface {} exec: {}", capability, cmd));

    // An install reaches the network; the query capabilities read local state.
    if (capability == "install_packages" || capability == "update_packages")
        mcpp::platform::env::note_network_access();

    CallResult result;
    bool timedOut = false;
    int rc = mcpp::platform::process::run_streaming_bounded(cmd,
        [&](std::string_view line) {
            if (line.empty()) return;

            auto ev = parse_event_line(line);
            if (!ev) return;

            std::visit([&](auto&& e) {
                using T = std::decay_t<decltype(e)>;
                if constexpr (std::is_same_v<T, ProgressEvent>) {
                    if (handler) handler->on_progress(e);
                } else if constexpr (std::is_same_v<T, LogEvent>) {
                    if (handler) handler->on_log(e);
                } else if constexpr (std::is_same_v<T, DataEvent>) {
                    result.dataEvents.push_back(e);
                    if (handler) handler->on_data(e);
                } else if constexpr (std::is_same_v<T, ErrorEvent>) {
                    result.error = e;
                    if (handler) handler->on_error(e);
                } else if constexpr (std::is_same_v<T, ResultEvent>) {
                    result.exitCode = e.exitCode;
                    result.resultJson = e.dataJson;
                    if (handler) handler->on_result(e);
                }
            }, *ev);
        },
        std::chrono::milliseconds{0},
        std::chrono::duration_cast<std::chrono::milliseconds>(kInterfaceIdleTimeout),
        &timedOut);
    if (timedOut) {
        result.exitCode = result.exitCode != 0 ? result.exitCode : 124;
        result.stderrTail.push_back(std::format(
            "xlings interface {} wrote nothing for {} seconds (not even its "
            "heartbeat) and was stopped", capability, kInterfaceIdleTimeout.count()));
    }
    if (rc != 0 && result.exitCode == 0) result.exitCode = rc;
    if (result.exitCode != 0) {
        // Error-level lines only, the last 20: enough to name a rejection, and
        // bounded so a verbose child cannot bury the diagnostic it is attached
        // to.
        std::ifstream in(stderrFile);
        std::stringstream text;
        text << in.rdbuf();
        result.stderrTail = stderr_error_tail(text.str());
    }
    std::error_code rmEc;
    std::filesystem::remove(stderrFile, rmEc);
    return result;
}

std::vector<std::string> stderr_error_tail(std::string_view text, std::size_t limit) {
    std::deque<std::string> tail;
    std::size_t pos = 0;
    while (pos < text.size()) {
        const auto nl = text.find('\n', pos);
        std::string line(text.substr(pos, nl == std::string_view::npos
                                              ? std::string_view::npos : nl - pos));
        pos = nl == std::string_view::npos ? text.size() : nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string lower = line;
        std::ranges::transform(lower, lower.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lower.find("error") == std::string::npos
            && line.find("E_") == std::string::npos
            && !line.starts_with("[xim]"))
            continue;
        tail.push_back(std::move(line));
        if (tail.size() > limit) tail.pop_front();
    }
    return {tail.begin(), tail.end()};
}

namespace {
// The `files[]` of one NDJSON `download_progress` data event, in the order
// xlings reports them; nullopt when the line carries none. Shared by the
// sandbox bootstrap and the index refresh, which render the same event.
std::optional<BootstrapProgress> download_progress_of(std::string_view line) {
    LineScan ls{line};
    auto p = line.find("\"files\":[");
    if (p == std::string_view::npos) return std::nullopt;
    p += 9;

    BootstrapProgress prog;
    prog.elapsedSec = ls.find_num("elapsedSec");

    while (p < line.size()) {
        while (p < line.size() && (line[p] == ' ' || line[p] == '\n'
                                   || line[p] == ',')) ++p;
        if (p >= line.size() || line[p] == ']') break;
        if (line[p] != '{') break;
        int depth = 0;
        auto start = p;
        bool in_string = false;
        for (; p < line.size(); ++p) {
            char c = line[p];
            if (in_string) {
                if (c == '\\' && p + 1 < line.size()) { ++p; continue; }
                if (c == '"') in_string = false;
                continue;
            }
            if (c == '"')      in_string = true;
            else if (c == '{') ++depth;
            else if (c == '}') { if (--depth == 0) { ++p; break; } }
        }
        LineScan fl{line.substr(start, p - start)};
        BootstrapFile f;
        f.name            = fl.find_str("name");
        f.downloadedBytes = fl.find_num("downloadedBytes");
        f.totalBytes      = fl.find_num("totalBytes");
        f.started         = fl.find_bool("started");
        f.finished        = fl.find_bool("finished");
        if (!f.name.empty()) prog.files.push_back(std::move(f));
    }
    if (prog.files.empty()) return std::nullopt;
    return prog;
}

// The events of one `xlings interface update_packages` run, rendered with the
// renderer every other acquisition uses (mcpp::ui::ProgressBar and
// DownloadProgress). A `progress` event carries a phase and a percentage;
// one bar is drawn per phase (xlings reports `index_sync`, one event per
// repository, and `index_rebuild`, one event per descriptor file), so a
// refresh prints a few lines off a terminal rather than one per file. A
// `download_progress` data event is an index artifact being fetched. A line
// that is not an event is not rendered: an xlings that predates structured
// index progress printed its own terminal text on this stream, and the bar
// is what replaces it.
class IndexRefreshRenderer {
public:
    void line(std::string_view text) {
        LineScan ls{text};
        const auto kind = ls.find_str("kind");
        if (kind == "result") {
            resultExit_ = static_cast<int>(ls.find_num("exitCode"));
        } else if (kind == "progress") {
            const auto phase = ls.find_str("phase");
            if (phase.empty()) return;
            // A sync step names its repository at the end of its message
            // (`syncing index repo 2/5: mcpplibs`); each repository is its
            // own bar, labelled with that name.
            std::string subject;
            if (phase == "index_sync") {
                const auto message = ls.find_str("message");
                if (auto colon = message.rfind(": "); colon != std::string::npos)
                    subject = message.substr(colon + 2);
            }
            const auto key = subject.empty() ? phase : phase + "/" + subject;
            if (!bar_ || key != label_) {
                if (bar_) bar_->finish();
                shown_ = subject.empty() ? phase_label(phase)
                                         : std::format("package index {}", subject);
                bar_.emplace("Updating", shown_);
                label_ = key;
            }
            const auto pct = std::clamp(ls.find_num("percent"), 0.0, 100.0);
            bar_->update(static_cast<std::size_t>(pct));
        } else if (kind == "data" && ls.find_str("dataKind") == "download_progress") {
            auto prog = download_progress_of(text);
            if (!prog) return;
            if (bar_) { bar_->finish(); bar_.reset(); label_.clear(); }
            std::vector<mcpp::ui::DownloadFile> files;
            for (auto const& f : prog->files)
                files.push_back({f.name,
                                 static_cast<std::size_t>(f.downloadedBytes),
                                 static_cast<std::size_t>(f.totalBytes),
                                 f.started, f.finished});
            download_.update(files, prog->elapsedSec);
        }
    }
    // The exit code the result event carried, or -1 when none arrived.
    int result_exit() const { return resultExit_; }
    // The run failed or was stopped: the open bar says the step did not
    // complete rather than reporting it done.
    void fail() { failed_ = true; }
    ~IndexRefreshRenderer() {
        if (bar_) failed_ ? bar_->finish_failed(shown_) : bar_->finish();
        failed_ ? download_.finish_failed() : download_.finish();
    }

private:
    static std::string phase_label(std::string_view phase) {
        if (phase == "index_sync")    return "package index (sync)";
        if (phase == "index_rebuild") return "package index (rebuild)";
        return std::format("package index ({})", phase);
    }

    std::optional<mcpp::ui::ProgressBar> bar_;
    std::string                          label_;   // the phase (and repository) drawn
    std::string                          shown_;   // the bar's label
    bool                                 failed_ = false;
    mcpp::ui::DownloadProgress           download_;
    int                                  resultExit_ = -1;
};
} // namespace

// ─── install_with_progress ──────────────────────────────────────────

int install_with_progress(const Env& env, std::string_view target,
                          const BootstrapProgressCallback& cb,
                          bool quiet)
{
    ScopedInvocationEnv scope(env);   // #614
    auto argsJson = std::format(
        R"({{"targets":["{}"],"yes":true}})", target);

    // All platforms: try direct `xlings install ... -y` first.
    // The direct command is more reliable for large packages (e.g. LLVM
    // ~800MB) because:
    //   - it doesn't pipe through NDJSON interface (simpler subprocess chain)
    //   - xlings manages its own stdin/stdout/stderr
    //   - extraction subprocess coordination works normally
    // The NDJSON interface path is kept as a fallback for progress reporting.
    {
        auto directCmd = build_command_prefix(env) +
            std::format(" install {} -y {}", target, mcpp::platform::shell::silent_redirect);
        // Seal stdin so xlings and any grandchildren (curl, git, 7z, xcode-select
        // / brew prompts, etc.) cannot block on a terminal read. Without it,
        // first-run toolchain install pauses waiting for the user to press Enter
        // — first seen on Windows, now also reproduced on macOS (the "需要回车
        // 才能继续" first-run report). The old "POSIX must keep stdin open for
        // subprocess coordination" claim never held in practice; `-y` already
        // makes the install non-interactive, so the redirect only removes the
        // spurious terminal blocking. Windows uses <NUL, POSIX </dev/null.
        if constexpr (mcpp::platform::is_windows) {
            directCmd += " <NUL";
        } else {
            directCmd += " </dev/null";
        }

        // The direct install redirects all output to the null device, so it
        // produces zero feedback — on a slow/network-bound first run this
        // looks frozen. Run the blocking command on a worker thread and draw
        // an elapsed-time bar while it runs, with the renderer every other
        // acquisition uses (W11): redrawn in place on a terminal, one start
        // and one finish line otherwise, nothing under --quiet.
        const bool showSpinner = !quiet;

        mcpp::platform::env::note_network_access();
        std::atomic<bool> done{false};
        int directRc = 0;
        std::thread worker([&] {
            bool timedOut = false;
            directRc = mcpp::platform::process::run_streaming_bounded(
                directCmd, [](std::string_view) {},
                std::chrono::duration_cast<std::chrono::milliseconds>(kDirectInstallTimeout),
                std::chrono::milliseconds{0}, &timedOut);
            if (timedOut)
                mcpp::log::warn("xlings", std::format(
                    "`xlings install {}` did not finish within {} hours and was stopped",
                    target, kDirectInstallTimeout.count() / 3600));
            done.store(true, std::memory_order_release);
        });

        if (showSpinner) {
            mcpp::ui::ProgressBar bar("Installing", target);
            const auto start = std::chrono::steady_clock::now();
            while (!done.load(std::memory_order_acquire)) {
                bar.update_indeterminate(0, std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - start).count());
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
            worker.join();
            if (directRc == 0) bar.finish();
            else               bar.finish_failed(target);
            if (directRc == 0) return 0;
        } else {
            worker.join();
            if (directRc == 0) return 0;
        }
    }

    // Fallback: NDJSON interface path (provides progress callbacks).
    // Seal stdin (same rationale as the direct path above) so the install can't
    // block on a terminal read. The protocol is NDJSON-over-stdout + "yes":true,
    // so nothing here needs the terminal.
    // The same decision as the direct path above: the fallback used to spell
    // global mode by hand on POSIX whatever `env.projectDir` said (#614).
    auto cmd = std::format("{} interface install_packages --args {} {} {}",
        build_command_prefix(env), shq_meta(argsJson), mcpp::platform::null_redirect,
        mcpp::platform::is_windows ? "<NUL" : "</dev/null");

    int resultExitCode = -1;

    auto handle_line = [&](std::string_view line) {
        LineScan ls{line};
        auto kind = ls.find_str("kind");
        if (kind == "result") {
            resultExitCode = static_cast<int>(ls.find_num("exitCode"));
            return;
        }
        if (kind != "data") return;
        if (ls.find_str("dataKind") != "download_progress") return;
        if (!cb) return;

        if (auto prog = download_progress_of(line)) cb(*prog);
    };

    bool idleTimedOut = false;
    int closeRc = mcpp::platform::process::run_streaming_bounded(
        cmd, handle_line, std::chrono::milliseconds{0},
        std::chrono::duration_cast<std::chrono::milliseconds>(kInterfaceIdleTimeout),
        &idleTimedOut);
    if (idleTimedOut) {
        mcpp::log::warn("xlings", std::format(
            "xlings interface install_packages wrote nothing for {} seconds and was stopped",
            kInterfaceIdleTimeout.count()));
        return resultExitCode > 0 ? resultExitCode : 124;
    }
    return (resultExitCode != -1) ? resultExitCode : closeRc;
}

int install_direct(const Env& env, std::string_view target, bool quiet) {
    ScopedInvocationEnv scope(env);   // #614
    auto cmd = build_command_prefix(env)
        + std::format(" install {} -y", shq(target));
    if (quiet) {
        cmd += " ";
        cmd += std::string(mcpp::platform::shell::silent_redirect);
    }
    mcpp::platform::env::note_network_access();
    bool timedOut = false;
    // The streaming runner seals stdin itself. Lines are passed through as the
    // inherited terminal showed them before, unless the caller asked for quiet.
    // They are xlings' own progress text, narration: standard error.
    int rc = mcpp::platform::process::run_streaming_bounded(cmd,
        [quiet](std::string_view line) { if (!quiet) std::println(stderr, "{}", line); },
        std::chrono::duration_cast<std::chrono::milliseconds>(kDirectInstallTimeout),
        std::chrono::milliseconds{0}, &timedOut);
    if (timedOut)
        mcpp::log::warn("xlings", std::format(
            "`xlings install {}` did not finish within {} hours and was stopped",
            target, kDirectInstallTimeout.count() / 3600));
    return rc;
}

// ─── Sandbox lifecycle ──────────────────────────────────────────────

void seed_xlings_json(const Env& env,
                      std::span<const SeedRepo> repos,
                      std::string_view mirror,
                      const ProjectEnv& penv)
{
    auto path = env.home / ".xlings.json";
    std::string json = "{\n";
    json += "  \"index_repos\": [\n";
    for (std::size_t i = 0; i < repos.size(); ++i) {
        json += std::format("    {{ \"name\": \"{}\", \"url\": \"{}\"",
                            json_escape(repos[i].name),
                            json_escape(repos[i].url));
        if (!repos[i].artifact.empty() && !repos[i].artifactCn.empty())
            json += std::format(", \"artifact\": {{ \"GLOBAL\": \"{}\", \"CN\": \"{}\" }}",
                                json_escape(repos[i].artifact),
                                json_escape(repos[i].artifactCn));
        else if (!repos[i].artifact.empty())
            json += std::format(", \"artifact\": \"{}\"",
                                json_escape(repos[i].artifact));
        if (!repos[i].source.empty())
            json += std::format(", \"source\": \"{}\"",
                                json_escape(repos[i].source));
        json += std::format(" }}{}\n", i + 1 == repos.size() ? "" : ",");
    }
    json += "  ],\n";
    // [xlings] build environment (L-1): materialize deps/workspace/subos/envs
    // verbatim into the keys xlings reads. Each is emitted only when non-empty.
    auto emit_obj = [&](std::string_view key,
                        std::span<const std::pair<std::string,std::string>> kv) {
        json += std::format("  \"{}\": {{\n", key);
        for (std::size_t i = 0; i < kv.size(); ++i)
            json += std::format("    \"{}\": \"{}\"{}\n",
                                json_escape(kv[i].first), json_escape(kv[i].second),
                                i + 1 == kv.size() ? "" : ",");
        json += "  },\n";
    };
    if (!penv.deps.empty()) {
        json += "  \"deps\": [";
        for (std::size_t i = 0; i < penv.deps.size(); ++i)
            json += std::format("{}\"{}\"", i ? ", " : "", json_escape(penv.deps[i]));
        json += "],\n";
    }
    if (!penv.workspace.empty()) emit_obj("workspace", penv.workspace);
    if (!penv.subos.empty())
        json += std::format("  \"subos\": \"{}\",\n", json_escape(penv.subos));
    json += "  \"lang\": \"en\",\n";
    json += std::format("  \"mirror\": \"{}\"\n", json_escape(mirror));
    json += "}\n";
    write_file(path, json);
}

int config_show(const Env& env) {
    ScopedInvocationEnv scope(env);   // #614
    auto cmd = std::format("{} config", build_command_prefix(env));
    return mcpp::platform::process::run_silent(cmd);
}

int config_set_mirror(const Env& env, std::string_view mirror, bool quiet) {
    ScopedInvocationEnv scope(env);   // #614
    if (mirror.empty()) return 0;
    auto cmd = std::format(
        "{} config --mirror {} {}",
        build_command_prefix(env),
        shq(mirror),
        quiet ? mcpp::platform::shell::silent_redirect : "");
    return mcpp::platform::process::run_silent(cmd);
}

void ensure_init(const Env& env, bool quiet) {
    auto marker = paths::sandbox_init_marker(env);
    if (std::filesystem::exists(marker)) return;

    // Ensure the home directory exists before cd'ing into it.
    std::error_code ec;
    std::filesystem::create_directories(env.home, ec);

    if (!quiet)
        print_status("Initialize", "mcpp sandbox layout (one-time)");
    mcpp::log::ScopedTimer _t_init("init", "sandbox layout (xlings self init)");
    // `self init` initialises the global sandbox, so it runs in global mode
    // whatever the caller's project directory is.
    Env globalEnv = env;
    globalEnv.projectDir.clear();
    ScopedInvocationEnv scope(globalEnv);   // #614
    std::string cmd = build_command_prefix(globalEnv) + " self init "
        + std::string(mcpp::platform::shell::silent_redirect);
    bool initTimedOut = false;
    int rc = mcpp::platform::process::run_streaming_bounded(
        cmd, [](std::string_view) {},
        std::chrono::duration_cast<std::chrono::milliseconds>(kLocalCommandTimeout),
        std::chrono::milliseconds{0}, &initTimedOut);
    if (rc != 0 && !quiet) {
        std::println(stderr,
            "warning: `xlings self init` failed for sandbox at '{}'",
            env.home.string());
    }

    // The first real `xlings install` (the next bootstrap step) triggers
    // xlings to fetch its package index — a one-time, network-bound step
    // with no output of its own. Announce it so the user knows the silent
    // wait that follows is expected. This runs once: ensure_init is gated
    // by the sandbox-init marker above.
    if (!quiet)
        print_status("Fetching", "package index (one-time)");
}

void ensure_patchelf(const Env& env, bool quiet,
                     const BootstrapProgressCallback& cb)
{
    auto toolDir = paths::xim_tool(env, "patchelf", pinned::kPatchelfVersion);
    auto binary  = toolDir / "bin" / "patchelf";
    if (std::filesystem::exists(binary)) return;

    // Clean up incomplete installation residue (e.g. from Ctrl+C interrupt).
    if (std::filesystem::exists(toolDir)) {
        if (!quiet)
            print_status("Repairing", "patchelf (incomplete installation, cleaning up)");
        std::error_code ec;
        std::filesystem::remove_all(toolDir, ec);
    }

    if (!quiet)
        print_status("Bootstrap", "patchelf into mcpp sandbox (one-time)");
    mcpp::log::ScopedTimer _t_pe("init", std::format("bootstrap patchelf@{}", pinned::kPatchelfVersion));
    int rc = install_with_progress(env,
        std::format("xim:patchelf@{}", pinned::kPatchelfVersion), cb, quiet);
    if (rc != 0 && !quiet) {
        std::println(stderr,
            "warning: failed to bootstrap patchelf into mcpp sandbox; "
            "subsequent xim installs may skip ELF rewriting");
    }
}

void ensure_ninja(const Env& env, bool quiet,
                  const BootstrapProgressCallback& cb)
{
    auto root = paths::xim_tool_root(env, "ninja");
    auto ninja_name = std::string("ninja") + std::string(mcpp::platform::exe_suffix);
    if (std::filesystem::exists(root)) {
        std::error_code ec;
        for (auto& v : std::filesystem::directory_iterator(root, ec)) {
            if (std::filesystem::exists(v.path() / ninja_name)) return;
        }
        // Directory exists but no version has a working binary — residue.
        if (!quiet)
            print_status("Repairing", "ninja (incomplete installation, cleaning up)");
        std::filesystem::remove_all(root, ec);
    }
    if (!quiet)
        print_status("Bootstrap", "ninja into mcpp sandbox (one-time)");
    mcpp::log::ScopedTimer _t_ninja("init", std::format("bootstrap ninja@{}", pinned::kNinjaVersion));
    int rc = install_with_progress(env,
        std::format("xim:ninja@{}", pinned::kNinjaVersion), cb, quiet);
    if (rc != 0 && !quiet) {
        std::println(stderr,
            "warning: failed to bootstrap ninja into mcpp sandbox (exit {})",
            rc);
    }
}

namespace {

// "NASM version 2.16.03 compiled on ..." → true iff ≥ 2.16. A binary that
// fails to run or reports something unparseable is unusable — false.
bool nasm_version_ok(const std::filesystem::path& bin) {
    auto r = mcpp::platform::process::capture_exec({bin.string(), "-v"});
    if (r.exit_code != 0) return false;
    auto pos = r.output.find("version ");
    if (pos == std::string::npos) return false;
    int major = 0, minor = 0;
    const char* p = r.output.c_str() + pos + 8;
    while (*p >= '0' && *p <= '9') major = major * 10 + (*p++ - '0');
    if (*p++ != '.') return false;
    while (*p >= '0' && *p <= '9') minor = minor * 10 + (*p++ - '0');
    return major > 2 || (major == 2 && minor >= 16);
}

} // namespace

std::optional<std::filesystem::path> find_sandbox_nasm(const Env& env) {
    auto root = paths::xim_tool_root(env, "nasm");
    auto nasm_name = std::string("nasm") + std::string(mcpp::platform::exe_suffix);
    std::error_code ec;
    if (!std::filesystem::exists(root, ec)) return std::nullopt;
    for (auto& v : std::filesystem::directory_iterator(root, ec)) {
        for (auto cand : { v.path() / nasm_name, v.path() / "bin" / nasm_name }) {
            if (std::filesystem::exists(cand, ec) && nasm_version_ok(cand))
                return cand;
        }
    }
    return std::nullopt;
}

// THE ECOSYSTEM COPY IS TRIED FIRST, AND IT USED TO BE TRIED SECOND.
//
// This function answered `which("nasm")` before looking in the sandbox, so a
// machine with an assembler on PATH assembled with that one and a machine
// without downloaded the pinned `xim:nasm`. Three machines could produce three
// different objects from one source tree with nothing in the build saying so,
// which is the property every other tool in this engine is arranged to avoid:
// the compiler, the linker, ninja and patchelf all come from the graph or the
// sandbox, and none of them asks PATH first.
//
// The host copy is kept, and only as a last resort: a machine that is offline
// and already has a usable assembler can still build, which is the one case
// the sandbox route cannot serve. When it is the one used, the caller says so
// in the build report rather than leaving it silent --- `nasm_is_from_host`
// below is how the caller tells the two apart.
std::optional<std::filesystem::path> find_usable_nasm(const Env& env) {
    if (auto sandboxed = find_sandbox_nasm(env)) return sandboxed;
    auto nasm_name = std::string("nasm") + std::string(mcpp::platform::exe_suffix);
    if (auto sys = mcpp::platform::fs::which(nasm_name);
        sys && nasm_version_ok(*sys)) {
        return sys;
    }
    return std::nullopt;
}

// Whether a path `find_usable_nasm` returned is the host's rather than the
// sandbox's. Asked by the caller so that a host tool reaching a build is
// NAMED there; a host tool that reaches a build silently is the defect, not
// the host tool.
bool nasm_is_from_host(const Env& env, const std::filesystem::path& bin) {
    auto root = paths::xim_tool_root(env, "nasm");
    std::error_code ec;
    auto canonRoot = std::filesystem::weakly_canonical(root, ec);
    auto canonBin  = std::filesystem::weakly_canonical(bin, ec);
    if (canonRoot.empty() || canonBin.empty()) return true;
    auto r = canonRoot.string();
    auto b = canonBin.string();
    return b.rfind(r, 0) != 0;
}

// ─── Index freshness ────────────────────────────────────────────────

bool is_index_fresh(const Env& env, std::int64_t ttlSeconds) {
    return is_index_dir_fresh(default_index_dir(env), ttlSeconds);
}

bool is_official_index_fresh(const Env& env, std::int64_t ttlSeconds) {
    return is_index_dir_fresh(official_index_dir(env), ttlSeconds);
}

IndexStatus default_index_status(const Env& env, std::int64_t ttlSeconds) {
    return index_status_for(default_index_dir(env), ttlSeconds);
}

IndexStatus official_index_status(const Env& env, std::int64_t ttlSeconds) {
    return index_status_for(official_index_dir(env), ttlSeconds);
}

bool is_official_package_index_fresh(const Env& env,
                                     std::string_view packageName,
                                     std::int64_t ttlSeconds) {
    if (!is_official_index_fresh(env, ttlSeconds)) return false;
    auto pkg = official_package_file(env, packageName);
    return !pkg.empty()
        && std::filesystem::exists(pkg)
        && official_index_cache_matches_package_file(env, packageName);
}

namespace {
// The raw sync. Everything that makes a refresh SAFE lives in the wrapper
// below; this function's only job is to run `xlings update` and report.
int update_index_unguarded(const Env& env, bool quiet);
} // namespace

// Guarded entry point — the ONE place every refresh in this process passes
// through, which is why the monotonicity guarantee is installed here rather
// than at the seven call sites.
//
//   an index-side change must never take mcpp from "works" to "does not work"
//
// A published index can raise its client-version floor (index.toml min_mcpp).
// `xlings update` rewrites the tree in place, so before this guard existed a
// floor bump replaced a readable index with an unreadable one and left no way
// back — the refresh itself was the thing that broke the machine. See
// mcpp.pm.index_snapshot for why the shape is archive/judge/restore rather
// than the stage-and-swap the original design assumed.
namespace {
std::atomic<long long> g_index_refresh_timeout_s{kDefaultIndexRefreshTimeout.count()};
}

void set_index_refresh_timeout(std::chrono::seconds timeout) {
    g_index_refresh_timeout_s.store(
        timeout.count() > 0 ? timeout.count() : kDefaultIndexRefreshTimeout.count(),
        std::memory_order_relaxed);
}

std::chrono::seconds index_refresh_timeout() {
    return std::chrono::seconds{g_index_refresh_timeout_s.load(std::memory_order_relaxed)};
}

int update_index(const Env& env, bool quiet) {
    namespace snap = mcpp::pm::index_snapshot;
    const auto dataRoot = paths::index_data(env);

    snap::GuardOutcome out;
    int rc = snap::guarded_refresh(dataRoot,
        [&] { return update_index_unguarded(env, quiet); }, out);

    // Report ONLY when the guard had to act. The common path — refresh keeps
    // the index readable — must stay silent, or the notice becomes noise that
    // users learn to skip past, which is the same as not printing it.
    //
    // A floor is not an error of the run (an index is data; mcpp is the
    // program). Each case is one closing notice, printed after the command's
    // own output (mcpp::ui::add_closing_notice), and only because this run
    // refreshed the index. `quiet` governs the refresh's own narration, not
    // these: they are the one thing the refresh has to say.
    (void)quiet;
    auto advice = [&](const std::filesystem::path& dir) {
        auto it = out.requiredMcpp.find(dir);
        return mcpp::pm::index_floor_upgrade_advice(
            it == out.requiredMcpp.end() ? std::string_view{} : std::string_view{it->second});
    };
    for (auto& dir : out.rolledBack) {
        mcpp::ui::add_closing_notice("MCPP_INDEX_REQUIRES_NEWER_MCPP", std::format(
            "the refreshed package index `{}` requires a newer mcpp; this run "
            "used the previous index. {}", dir.filename().string(), advice(dir)));
    }
    for (auto& dir : out.recovered) {
        mcpp::ui::add_closing_notice("MCPP_INDEX_REQUIRES_NEWER_MCPP", std::format(
            "the package index `{}` was restored from a local snapshot this mcpp "
            "can read; the published index requires a newer mcpp. {}",
            dir.filename().string(), advice(dir)));
    }
    for (auto& dir : out.stillUnusable) {
        mcpp::ui::add_closing_notice("MCPP_INDEX_REQUIRES_NEWER_MCPP", std::format(
            "the package index `{}` requires a newer mcpp and no earlier copy is "
            "usable; packages it serves cannot be resolved. {}",
            dir.filename().string(), advice(dir)));
    }
    return rc;
}

namespace {
int update_index_unguarded(const Env& env, bool quiet) {
    ScopedInvocationEnv scope(env);   // #614
    // Offline is absolute: no caller gets to reach the network by going around
    // the decision layer. Reported as success so a build that can still resolve
    // everything locally proceeds — the caller that genuinely needed the data
    // fails on its own missing answer, with a message that says so.
    if (mcpp::platform::env::offline_mode()) {
        mcpp::log::verbose("index", "offline mode: skipping index update");
        return 0;
    }

    // Cross-process mutex. The sync rewrites a tree several concurrent mcpp
    // processes read (parallel CI jobs on one MCPP_HOME, two terminals, a
    // workspace fan-out), and it also writes the refresh markers. The BMI cache
    // has had this guard for a while; the index never did.
    //
    // NON-BLOCKING BY DESIGN: whoever holds the lock is already doing the exact
    // work we wanted, so waiting buys nothing and a queue of stalled builds is
    // precisely the symptom this whole change exists to remove. Skipping is
    // also why this must not be reported as failure.
    std::error_code lockEc;
    std::filesystem::create_directories(paths::index_data(env), lockEc);
    auto lock = mcpp::platform::fs::FileLock::try_acquire(paths::index_data(env));
    if (!lock) {
        mcpp::log::verbose("index",
            "another process is refreshing the index — skipping this one");
        return 0;
    }

    // Through the NDJSON interface, as installs are (W11): the refresh reports
    // its steps as events and is drawn by the same renderer, instead of the
    // bare CLI's terminal text, which was either reprinted verbatim or, for
    // the automatic refresh, discarded, so a refresh of many seconds showed
    // nothing. The interface's `update_packages` and the CLI's `update` are
    // one function in xlings (xim::cmd_update).
    std::string cmd = std::format("{} interface update_packages --args {} {} {}",
        build_command_prefix(env), shq_meta("{}"), mcpp::platform::null_redirect,
        mcpp::platform::is_windows ? "<NUL" : "</dev/null");
    mcpp::platform::env::note_network_access();
    // The index sync is a network git operation; a single transient blip (DNS,
    // TLS reset, a mirror hiccup) otherwise fails a cold `mcpp self env` /
    // first-run init outright (e.g. CI's index/sandbox bootstrap). Retry with
    // linear backoff. The success path returns on the FIRST attempt — zero
    // added latency in steady state; only a genuine failure pays the backoff.
    constexpr int kMaxAttempts = 3;
    int rc = 0;
    const auto refreshBound = index_refresh_timeout();
    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
        bool timedOut = false;
        {
            IndexRefreshRenderer renderer;
            rc = mcpp::platform::process::run_streaming_bounded(cmd,
                [&renderer](std::string_view line) { renderer.line(line); },
                std::chrono::duration_cast<std::chrono::milliseconds>(refreshBound),
                std::chrono::milliseconds{0}, &timedOut);
            if (rc == 0 && renderer.result_exit() > 0) rc = renderer.result_exit();
            if (rc != 0 || timedOut) renderer.fail();
        }
        if (rc == 0 && !timedOut) { mark_known_indexes_refreshed(env); return 0; }
        // A refresh that exceeded its bound is not retried: the retries exist for
        // a transient failure that ends, and a connection that never answers
        // would only be waited on three times. The caller treats the refresh as
        // failed and resolves from the local index, as it does for any failure.
        if (timedOut) {
            std::println(stderr,
                "warning: the package index refresh did not finish within {} seconds and was "
                "stopped; continuing with the local index "
                "(the bound is [index] refresh_timeout in mcpp's config.toml)",
                refreshBound.count());
            return rc != 0 ? rc : 124;
        }
        if (attempt < kMaxAttempts) {
            int delay = attempt * 2;  // 2s, then 4s
            mcpp::log::verbose("index", std::format(
                "index update attempt {}/{} failed (rc {}); retrying in {}s",
                attempt, kMaxAttempts, rc, delay));
            std::this_thread::sleep_for(std::chrono::seconds(delay));
        }
    }
    // Said, not only logged, to a caller that refreshes on its own (`quiet`:
    // the TTL refresh before a build), which continues with the local index;
    // `mcpp index update` reports the failure itself.
    if (quiet)
        std::println(stderr,
            "warning: the package index refresh failed after {} attempts (exit {}); "
            "continuing with the local index", kMaxAttempts, rc);
    return rc;
}
} // namespace

void ensure_index_fresh(const Env& env, std::int64_t ttlSeconds, bool quiet) {
    if (is_index_fresh(env, ttlSeconds)) return;
    if (!quiet)
        print_status("Updating", "package index (auto-refresh)");
    update_index(env, /*quiet=*/true);
}

bool official_package_present(const Env& env, std::string_view packageName) {
    auto pkg = official_package_file(env, packageName);
    std::error_code ec;
    return !pkg.empty() && std::filesystem::exists(pkg, ec);
}

} // namespace mcpp::xlings
