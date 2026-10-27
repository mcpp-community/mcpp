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

export module mcpp.provider.xlings;

import std;
import mcpp.provider;
import mcpp.pm.compat;
import mcpp.pm.index_contract;
import mcpplibs.os.platform;
import mcpp.invocation;
import mcpp.effects;
import mcpp.build.command_line;
import mcpp.home.files;
import mcpp.log;
import mcpp.ui;                 // closing notices of the refresh guard
import mcpp.home;
import mcpp.xpkg_version;
import mcpp.libs.json;
import mcpplibs.os.fs;

export namespace mcpp::provider::xlings {

// ─── Env: resolved xlings binary + home directory ───────────────────

using Env = mcpp::provider::Location;

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
    //
    // Seventh, at 2026.10.10.2 (openxlings/xlings#651). Below it, a payload's
    // hard-link entry failed the whole extraction where link(2) is refused --
    // Android's app sandbox, so every Termux install of the managed glibc --
    // every local write failure was reported as E_DISK_FULL, the packages that
    // depended on a failed one ran their hooks anyway, and an extraction said
    // nothing until it ended. mcpp reads the failure's code for its hint and
    // the `extract` and `hook` progress events for its `Installing` lines.
    //
    // Eighth, at 2026.10.16.1 (openxlings/xlings#676). Below it, an index's
    // data and payloads could only live in xlings' own home, `update` fetched
    // every index including the ones mcpp refreshes itself, a home could not
    // install from another by link, and nothing answered which payloads were
    // complete. mcpp now places its indices and store in its home (`dir`,
    // `store`), stages each refresh (`sync_index`), names upstreams, reads
    // `payload_status`, and checks all of it through `interface --version`.
    //
    // Ninth, at 2026.10.16.2 (openxlings/xlings#677). Below it, inside a Luban
    // system mcpp's registry could not build its catalog (its recipes need a
    // worker bwrap cannot start there), and was layered on the system's home
    // besides its upstream, so a borrowed payload had no entry in the store
    // mcpp reads. A runtime's own install now makes the home's runtime links;
    // `pin_runtime` still asks for them after it (idempotent).
    //
    // Tenth, at 2026.10.17.1 (openxlings/xlings#678). Below it, `payload_status`
    // called a payload of an older packaging revision incomplete, so a build
    // replaced llvm, gcc and glibc after every revision the index raised, and
    // a replacement was refused for good by another package's dangling record
    // in the home. It now reports integrity, `usable` and `current` apart
    // (`payload_currency`), drops dangling records in every transaction and
    // says so (`scope_repaired`), and summarizes the binding state
    // (`binding_state`) for `mcpp self doctor` (design 2026-10-17).
    // Eleventh, at 2026.10.19.1 (openxlings/xlings#684). Resource probes and
    // download socket budgets share the provider's actual transport. Read-only
    // queries do not adopt a home or create an operation journal; older custom
    // providers are queried for metadata only in an isolated temporary home.
    //
    // Twelfth, at 2026.10.24.1 (openxlings/xlings#699, released through #701).
    // Below it, a script package's alias was `xlings script <path>` unquoted,
    // and the shim split it at whitespace: under a home whose path holds a
    // space a script package's hook could not load (mcpp#832). The index
    // works around it for gcc (openxlings/xim-pkgindex#1035); this floor
    // closes it for every script package.
    //
    // Thirteenth, at 2026.10.26.1 (openxlings/xlings#707). An interface call
    // ends when its capability does (`payload_status`, which every query
    // reads, took 517 ms of fixed waits); an upgraded home's projection links
    // are proven by its registrations; `--home` selects one run's home; and a
    // hook in a project's store finds `xlings` in its bin directory, which
    // `gcc.libstdcxx` (an `xim:gcc` edge) needs.
    inline constexpr std::string_view kXlingsVersion   = "2026.10.26.1";
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
    using XpkgRef = mcpp::provider::PackageRef;

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

    // ── Where each index is, and where its payloads go (design 2026-10-16
    // §5): an `index_repos` entry may name its own data directory and store
    // (xlings 2026.10.16.1, X1/X2); mcpp names them for the indices it owns.
    struct DeclaredIndex {
        std::string           name;
        std::filesystem::path dir;     // where its data is (absolute)
        std::filesystem::path store;   // where its payloads go (absolute; empty: the home's)
        bool                  owned = false;   // a `dir` was declared: its owner refreshes it
    };
    // The entries of this env's configuration (a project's, then the home's
    // when the env has a project).
    std::vector<DeclaredIndex> declared_indices(const Env& env);
    // Where the index `name` keeps its data here.
    std::filesystem::path index_dir_of(const Env& env, std::string_view name);
    // Every store a payload of this env may be in: the declared ones, then
    // the home's. A payload mcpp installed into the home's store before its
    // index had a store of its own is found there (COMPAT(internal:legacy-registry-store)).
    std::vector<std::filesystem::path> stores(const Env& env);
    // Every directory an index of this env's scope (its project's when it
    // has one, else the home's) keeps its data in, sorted.
    std::vector<std::filesystem::path> index_roots(const Env& env);
    // The same of a provider home (the global one, or a project's), read
    // from its configuration alone: its index directories, its stores.
    std::vector<std::filesystem::path> index_roots_of(const std::filesystem::path& home);
    std::vector<std::filesystem::path> stores_of(const std::filesystem::path& home);
    // The indexes a provider home serves: xim, then its configuration's
    // `index_repos`; empty when it has no configuration.
    std::vector<std::string> indices_of(const std::filesystem::path& home);
    // Record where the indices mcpp owns keep their data and payloads in
    // this env's configuration, and move data an index kept at the home's
    // own place there, once (design §5.3).
    void own_indices(const Env& env, std::span<const mcpp::provider::SeedRepo> owned);

    // sandbox init marker: env.home / "subos" / "default" / ".xlings.json"
    std::filesystem::path sandbox_init_marker(const Env& env);

} // namespace paths

// ─── Shell quoting ──────────────────────────────────────────────────

// Shell-escape (single-quote) a string for the command line.
std::string shq(std::string_view s);

// `shq` for an argument that may carry a shell metacharacter (JSON, `>=`).
// See `shell::quote_windows_through_cmd`.
std::string shq_meta(std::string_view s);

// ─── One xlings invocation ──────────────────────────────────────────

// The command line a person runs to repeat what mcpp ran (diagnostics, the
// commands a message names), POSIX:
// cd '<home>' && env -u XLINGS_PROJECT_DIR PATH=<sandbox_bin>:"$PATH"
//     XLINGS_HOME='<home>' '<binary>'
std::string build_command_prefix(const Env& env);

// The Windows spelling of that prefix, compiled on every host so that it is
// tested from any: `cd /d "<home>" && "<binary>"`.
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
// variable. The started xlings receives the decision (`invocation`); the
// displayed command spells it (`env -u` and `K=V`).
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

// The runtime the home's `abi` link selects (`xlings config --runtime`).
void select_runtime(const Env& env, std::string_view runtimeId);

// Build full xlings interface command.
// <prefix> interface <capability> --args '<argsJson>' 2>/dev/null
std::string build_interface_command(const Env& env,
                                    std::string_view capability,
                                    std::string_view argsJson);

// ─── NDJSON event types ─────────────────────────────────────────────

using ProgressEvent = mcpp::provider::ProgressEvent;

using LogEvent = mcpp::provider::LogEvent;

using DataEvent = mcpp::provider::DataEvent;

using ErrorEvent = mcpp::provider::ErrorEvent;

using ResultEvent = mcpp::provider::ResultEvent;

using Event = mcpp::provider::Event;

// Parse one NDJSON line into an Event.
std::optional<Event> parse_event_line(std::string_view line);

// ─── JSON extraction helpers (for NDJSON parsing) ───────────────────

std::string extract_string(std::string_view text, std::string_view key);
std::optional<long long> extract_int(std::string_view text, std::string_view key);
std::optional<bool> extract_bool(std::string_view text, std::string_view key);
std::string extract_object(std::string_view text, std::string_view key);

// ─── Subprocess call ────────────────────────────────────────────────

using CallResult = mcpp::provider::CallResult;

// The error-level lines of an xlings invocation's stderr, the last `limit` of
// them (#614). A line is kept when it contains "error" in any case, contains
// `E_`, or starts with `[xim]`; a trailing carriage return is dropped. Exported
// so the selection is stated as a unit test rather than through a failing
// install.
std::vector<std::string> stderr_error_tail(std::string_view text, std::size_t limit = 20);

using EventHandler = mcpp::provider::EventHandler;

std::expected<CallResult, std::string>
call(const Env& env, std::string_view capability,
     std::string_view argsJson, EventHandler* handler = nullptr);

// ─── Bootstrap progress types ───────────────────────────────────────

using BootstrapFile = mcpp::provider::BootstrapFile;

using BootstrapProgress = mcpp::provider::BootstrapProgress;

using BootstrapProgressCallback = mcpp::provider::BootstrapProgressCallback;

// Parent-owned byte progress protection, independent of provider heartbeats
// and socket reads (DNS or a trickle of headers may produce neither body bytes
// nor a socket timeout). Time is injectable for the state-machine tests.
class DownloadWatchdog {
    struct File {
        double bytes = 0;
        int attempt = 0;
        bool finished = false;
        std::chrono::steady_clock::time_point progress;
    };
    std::map<std::string, File> files_;
    std::chrono::milliseconds idle_;
public:
    explicit DownloadWatchdog(std::chrono::milliseconds idle = std::chrono::seconds(30)) : idle_(idle) {}
    void observe(const BootstrapProgress& progress,
                 std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) {
        for (const auto& f : progress.files) {
            auto [it, added] = files_.try_emplace(f.name);
            auto& state = it->second;
            if (added || f.sourceAttempt != state.attempt || f.downloadedBytes > state.bytes)
                state.progress = now;
            state.bytes = f.downloadedBytes;
            state.attempt = f.sourceAttempt;
            state.finished = f.finished || !f.started;
        }
    }
    bool expired(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) const {
        return std::ranges::any_of(files_, [&](const auto& f) {
            return !f.second.finished && now - f.second.progress >= idle_;
        });
    }
};

class InstallDeadline {
    DownloadWatchdog downloads_;
    std::chrono::steady_clock::time_point started_;
    std::chrono::milliseconds preparation_;
    bool downloadSeen_ = false, localWork_ = false;
public:
    explicit InstallDeadline(std::chrono::milliseconds preparation = std::chrono::seconds(120),
        std::chrono::milliseconds idle = std::chrono::seconds(30),
        std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now())
        : downloads_(idle), started_(start), preparation_(preparation) {}
    void observe(std::string_view line,
        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
    bool expired(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) const {
        return downloads_.expired(now)
            || (!downloadSeen_ && !localWork_ && now - started_ >= preparation_);
    }
};

mcpp::provider::NetworkReport network_check(const Env& env, std::chrono::milliseconds budget);

// One structured install with native source/byte progress, an optional
// callback (otherwise the common download renderer), and parent-owned bounds.
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
using ProjectEnv = mcpp::provider::ProjectEnv;

// One index_repos entry for seed_xlings_json. `artifact`/`source` are the
// xlings >= 0.4.68 per-repo artifact-sync fields (#269); empty means "do not
// emit the key", which keeps pre-artifact output byte-identical and matches
// xlings' "undeclared = plain git" semantics. Older xlings ignores both keys.
using SeedRepo = mcpp::provider::SeedRepo;

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
// (mcpp::pm::Fetcher::resolve_xpkg_path("xim:nasm@<version>", PayloadIntent::Use,
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
inline constexpr std::int64_t kIndexRefreshDebounceSeconds = mcpp::provider::kIndexRefreshDebounceSeconds;

// ─── Bounds on the xlings children mcpp starts (#648 A3) ───────────────
//
// Every xlings invocation on the planning path is started by mcpp.run in a
// unit of its own (a process group, a job object on Windows) that ends with
// mcpp, and it is bounded. Before, an `xlings update`
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
inline constexpr std::chrono::seconds kDefaultIndexRefreshTimeout = mcpp::provider::kDefaultIndexRefreshTimeout;
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
using IndexStatus = mcpp::provider::IndexStatus;
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

// What the xlings at `binary` is (`xlings interface --version`, protocol
// 1.10+): its version, protocol, capabilities, features and home layouts;
// `answered` false for an xlings before the handshake.
mcpp::provider::ProviderInfo describe(const std::filesystem::path& binary,
    std::chrono::milliseconds budget = std::chrono::seconds(3));

// Why that xlings cannot serve this mcpp (design 2026-10-16 §12): empty when
// it can. The pinned version, and every feature mcpp relies on.
std::string unmet(const mcpp::provider::ProviderInfo& info);

} // namespace mcpp::provider::xlings
