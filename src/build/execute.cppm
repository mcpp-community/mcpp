// mcpp.build.execute — drives a prepared BuildContext: ninja execution,
// build cache + fast-path rebuilds, and the run/test/clean pipelines.
// Bodies moved verbatim from the CLI layer. Zero behavior change.

module;
#include <cstdio>
#include <cstdlib>
#include <cerrno>      // ENOENT/EACCES/ENOEXEC — the launcher status band

export module mcpp.build.execute;

import std;
import mcpp.build.advice;
import mcpp.build.build_program;   // #359 glob inputs the mtime sweep cannot see
import mcpp.build.compile_commands; // C1: the fast path restores a deleted root CDB
import mcpp.build.prepare;
import mcpp.pack;                  // #622 A10: mcpp::pack::Options / Format
import mcpp.pack.pipeline;         // #622 A10: build_and_pack, for `run --format`
import mcpp.build.test_targets;
import mcpp.diag;
import mcpp.build.plan;
import mcpp.toolchain.triple;
import mcpp.freestanding.runner;
import mcpp.build.runner_lookup;    // #544: where the runner's program is
import mcpp.home;                   // config.toml, for the machine's default toolchain
import mcpp.libs.toml;
import mcpp.toolchain.registry;     // a payload's own runner (PayloadDescriptor::runner)
import mcpp.build.directives;      // the device-slot table: run / flash / monitor / debug
import mcpp.freestanding.linkline;
import mcpp.build.graph_shape;    // #407: which mode wrote this build.ninja
import mcpp.build.backend;
import mcpp.build.ninja;
import mcpp.build.flags;       // profile_descriptor — one answer for the level (#694)
import mcpp.build.runtime_validation;
import mcpp.bmi_cache;
import mcpp.bmi_cache.maintenance;  // dir_size + human_bytes, for `clean --stale`
import mcpp.manifest;
import mcpp.source_kind;
import mcpp.modgraph.scanner;
import mcpp.toolchain.post_install;
import mcpp.toolchain.stdmod;
import mcpp.version;               // MCPP_VERSION — the engine a build record names
import mcpp.xlings;
import mcpp.xlings.subos_info;
import mcpp.runtime.binding;
import mcpp.log;
import mcpp.platform;
import mcpp.platform.capacity;
import mcpp.build.schedule.policy;   // resolve_jobs — one answer to "how many at once"
import mcpp.fetcher.progress;
import mcpp.project;
import mcpp.ui;
import mcpp.build.progress;

namespace mcpp::build {

// ─── The exit status of a run ────────────────────────────────────────
//
// `mcpp run` REPORTS THE PROGRAM'S OWN EXIT STATUS, AND NOTHING BELOW 125
// BELONGS TO mcpp.
//
// It used to fold every non-zero status to 1, so that 2 could mean "could not
// start" as distinct from "ran and failed". The distinction was worth keeping;
// the price was not. Measured before the change: a program whose `main` returns
// 3 made `mcpp run` exit 1, and a bare-metal image that qemu reported as 3
// arrived as 1 as well. A command that cannot report a status is one nobody can
// use in a script, and this ecosystem tells people that running on a device is
// like running hosted.
//
// The band is the one `env`, `timeout` and `nice` already use and that shells
// document, so 126 and 127 arrive with their usual meanings rather than as
// numbers this project allocated:
//
//   127  the program was not found
//   126  it was found and could not be executed (permission, wrong format)
//   125  the launcher itself failed for some other reason
//
// A program that legitimately exits 125-127 is indistinguishable from these,
// which is the residual cost and the reason the message on stderr is not
// optional: a launcher failure always prints why, a program's own status never
// does.
int launcher_status(int spawnErrno) {
    switch (spawnErrno) {
        case ENOENT:
        case ESRCH:    return 127;
        case EACCES:
        case EPERM:
        case ENOEXEC:
        case EISDIR:   return 126;
        default:       return 125;
    }
}

// THE STATUS OF A `run` WHOSE PLANNING OR BUILD FAILED (output streams plan
// 2026-10-01, D7, R2).
//
// It used to be the build's own status, 1 or 2, and a program that returns 1
// reads the same: a script that runs `mcpp run -q` could not tell a compile
// error from a program that failed. 101 is the status Cargo gives a failed
// `cargo run` build, and an established convention. It is also rare among the
// statuses programs return themselves, which is what makes it tell the two
// apart; it does not collide with the launcher's band above. Only `run` uses
// it: `build`, `test` and `pack` keep their statuses, and so does a program's
// own, which passes through unchanged.
//
// With a runner (`--runner`, `[target.<triple>].runner`) the status that passes
// through is the runner's, and a runner that itself returns 101 reads as a
// failed build. That is accepted, as in Cargo, and documented.
constexpr int kRunBuildFailed = 101;

// ─── P0: build cache for fast-path rebuilds ─────────────────────────

constexpr std::string_view kBuildCacheFile = "target/.build_cache";
// P3: LRU capacity. Entries are keyed by (target triple, profile), so the
// working set is now targets × profiles rather than targets alone — 4 was
// enough for one profile, not for a dev/release/dist rotation across a host
// and a cross target.
constexpr int kBuildCacheMaxEntries = 32;

// P3: one entry per (target, fingerprint) pair.
// THE INPUTS THAT CHOOSE A TOOLCHAIN AND ARE NOT IN THE MANIFEST.
//
// The fast path replays a recorded build when the request matches the entry
// that recorded it. `--toolchain` (arriving as MCPP_TOOLCHAIN) and the
// machine's default (`mcpp toolchain default`, stored in config.toml) both
// choose the compiler, and neither was compared. Measured 2026-09-12: after
// `mcpp build` with gcc, `mcpp build --toolchain llvm@22.1.8` printed
// `Finished dev in 0.00s` and left the gcc artefact in place, skipping every
// resolution-time check with it. The manifest's own `[toolchain]` needs no
// entry here: the freshness check already declines when mcpp.toml is newer
// than the recorded build.
//
// THE NAMED SET. A recorded build is replayed only for the same target triple,
// profile, cache mode, requested features and toolchain request. The other
// global options change how a resolution is fetched (`--offline`), checked
// (`--locked`) or executed (`--jobs`), not what it chooses, and are not
// compared.
std::string toolchain_request_identity() {
    std::string cli;
    if (const char* e = std::getenv("MCPP_TOOLCHAIN"); e) cli = e;
    std::string machineDefault;
    std::error_code ec;
    const auto configFile = mcpp::home::root() / "config.toml";
    if (std::filesystem::exists(configFile, ec)) {
        if (auto doc = mcpp::libs::toml::parse_file(configFile))
            machineDefault = doc->get_string("toolchain.default").value_or("");
    }
    return std::format("cli={};default={}", cli, machineDefault);
}

// THE ENGINE THAT WROTE A BUILD RECORD.
//
// build.ninja names the engine that wrote it by absolute path: the `$mcpp`
// rules (dyndep, stage, the BMI schedule), and the `__action` wrapper of an
// action that declares an environment or a stamp, all start that executable.
// The emitter said a version change regenerates the file, because the version
// is a fingerprint input, and the fast paths never computed a fingerprint: they
// matched an entry by target, profile, cache mode, features and toolchain
// request, and compared its recorded fingerprint with its own directory's name.
// After an upgrade that removed the previous install the entry was replayed, and
// every action started a program that no longer existed (#757). When the old
// executable still exists the failure is silent instead: a newer front end
// drives the actions, and the graph, of an older engine.
//
// So the record carries the engine itself, and the one admission predicate
// below compares it. The version and the executable's path are both kept: the
// same version at another path (a moved or reinstalled binary) leaves the same
// stale text in the graph that a new version leaves, and only the path says so.
export struct EngineIdentity {
    std::string version;   // MCPP_VERSION
    std::string exe;       // mcpp_exe_path(), the spelling the graph uses
    bool operator==(const EngineIdentity&) const = default;
};

// The engine this process is: the one that would write a record now, and the
// one that would replay it.
export EngineIdentity running_engine() {
    return {std::string(mcpp::MCPP_VERSION), mcpp_exe_path().string()};
}

// Why a record written by `recorded` is not replayed by `running`; nullopt when
// they are the same engine. `recorded` is empty for an entry written before the
// field existed, which is not the same answer as an engine that happens to
// match -- the entry then declines once and the next write records it, the
// discipline of every field of the entry below.
export std::optional<std::string>
engine_declined_because(const std::optional<EngineIdentity>& recorded,
                        const EngineIdentity& running) {
    if (!recorded) return std::string("the recorded build predates the engine identity");
    if (recorded->version != running.version)
        return std::format("the recorded build was written by mcpp {}, and this is mcpp {}",
                           recorded->version, running.version);
    if (recorded->exe != running.exe)
        return std::format("the recorded build was written by the engine at {}, and this one is at {}",
                           recorded->exe, running.exe);
    return std::nullopt;
}

export struct BuildCacheEntry {
    std::string targetTriple;    // "" for default target
    std::string outputDir;
    std::string ninjaProgram;
    std::string fingerprint;     // outputDir basename
    std::string runtimeEnvKey;   // "-" means intentionally empty; "" means old cache
    std::string runtimeEnvValue;
    // mcpp#225 (E2): resolved binary run-targets, cached alongside the
    // fingerprint so `mcpp run` can skip prepare_build (toolchain
    // resolution + modgraph scan) on a cache hit — see build_run_target's
    // fast path. name -> exe path relative to outputDir. Caches written
    // before this field existed leave it empty, which the run fast-path
    // treats as a miss (falls back to prepare_build once, never crashes).
    std::vector<std::pair<std::string, std::string>> runTargets;
    // The process environment needed to exec those targets (e.g.
    // LD_LIBRARY_PATH for dep .so's not covered by the exe's own RUNPATH),
    // cached the same way as runtimeEnvKey/Value above but for RUNNING the
    // binary rather than invoking the toolchain. "" (default-constructed)
    // means old cache / not yet resolved — the run fast-path exec's with no
    // extra env in that case, matching prepare_build's behavior when
    // plan.runtimeLibraryDirs is empty.
    std::string runEnvKey;
    std::string runEnvValue;
    // The subos this build's toolchain belongs to (mcpp#352). The DIRECTORY,
    // never the resolved variables: the environment is the subos's property
    // and must be re-read on every run, while WHICH subos is the build's
    // property and would otherwise be unknowable on the fast path -- which
    // has no toolchain to derive it from.
    std::string subosDir;
    // Was the line present at all? An EMPTY subosDir is a legitimate answer
    // (a system toolchain outside the xpkgs store has no subos), so it cannot
    // stand in for "this cache predates the field" -- and those two need
    // opposite treatment: the first runs, the second must rebuild once.
    bool        subosRecorded = false;
    // The resolved profile this entry was built for. Entries used to be keyed
    // by target triple alone, and the fast paths only refuse to run when an
    // EXPLICIT --profile/--dev/--release is passed — so a bare `mcpp build`
    // after `mcpp build --release` took the fast path against the release
    // build.ninja and reported success without ever rebuilding at -O0 -g.
    // Empty means "cache predates this field" and is treated as a miss (a
    // bare rebuild once, never a wrong artifact).
    std::string profile;
    // The global-cache mode this build.ninja was generated under. A graph built
    // under `global` contains stage_file edges reading the cache; replaying it
    // for a request that asked for `local` would use the cache the manifest just
    // said not to use — and ruling the cache out is `local`'s entire purpose.
    // Same back-compat contract as `profile`: empty ⇒ miss.
    std::string cacheMode;
    // Exact immutable snapshot used by the build. Optional distinguishes a
    // current cache from one written by an older mcpp (or a corrupt payload).
    std::optional<mcpp::platform::runtime::RuntimeBinding> runtimeBinding;
    // Source trees outside `projectRoot` that this build read — `path`
    // dependencies, which is what workspace members are to each other. The
    // staleness sweep has to cover them or a NEW FILE appearing in one is
    // invisible: ninja has no edge for a file that did not exist when
    // build.ninja was written, so `mcpp build` replays the stale graph and
    // reports success. See BuildContext::depSourceRoots.
    //
    // Each root carries the extension tables of its own package, which is what
    // the sweep classifies the root's files with (#756): the consumer's table
    // says nothing about a provider's `.ixx`. Written as `depSources=`; the
    // block of an engine that recorded the paths alone (`depSourceRoots=`) is
    // read past and counts as absent, since a root without its tables cannot
    // be swept.
    std::vector<DepSourceRoot> depSourceRoots;
    // Was the block present at all? An EMPTY list is a legitimate answer — a
    // project with no path dependencies has none — so it cannot stand in for
    // "this cache predates the field", and the two need opposite treatment:
    // the first takes the fast path, the second must fall through once so the
    // list gets written. Same discipline as `subosRecorded` above, and for the
    // same reason.
    bool depSourceRootsRecorded = false;
    // Did the build this entry records have a runner declared for its target
    // (#544)? The run fast path executes the artifact bare and has no manifest
    // to read a template from, so an entry with a runner is a miss for it —
    // the prepare path then consults choose_runner as the first `mcpp run`
    // did. Absent on caches written before the field: false, which is the
    // pre-#544 behaviour and correct for every entry such a cache could hold
    // (a hosted runner was never consulted then, so none was ever used).
    bool runnerDeclared = false;
    // Did the graph declare a tool on the `when = "run"` tier that this build
    // did NOT provision?
    //
    // A BUILD INSTALLS LESS THAN A RUN NEEDS, WHICH IS THE POINT OF THE
    // TIER AND ALSO ITS ONE HAZARD. `mcpp build` requests the build tier; a
    // later `mcpp run` requests more. The fast path exists precisely to skip
    // the pass that would install the difference, so an entry written by a
    // build that saw run-tier entries is a miss for it — exactly as an entry
    // recording a runner is.
    //
    // Absent on caches written before the field: false, which is correct for
    // every entry such a cache could hold, because no manifest could express
    // the tier.
    bool runTierPending = false;
    // THE FEATURE SET THIS ENTRY'S ARTEFACTS WERE BUILT WITH.
    //
    // The entry is keyed on (target, profile, cache mode) and was matched on
    // those three alone, while the OUTPUT DIRECTORY is keyed on a fingerprint
    // that includes the features. So `mcpp build --features loud` wrote an
    // entry pointing at the loud output directory, and the next plain
    // `mcpp build` matched it and reported success in 0.00s — serving the
    // featured artefact to a request that asked for none.
    //
    // Measured before this field existed: three builds of one project printed
    // `quiet`, `LOUD`, `LOUD`. The third had no feature on.
    //
    // Absent on caches written before the field: empty, which reads as "no
    // features" — correct for every entry such a cache could hold whose
    // request also has none, and a miss otherwise, which is the safe direction.
    std::string features;
    // The toolchain request this entry was built for; see
    // toolchain_request_identity. Recorded is kept apart from the value
    // because a cache written before the field existed must decline once,
    // not match a request whose inputs it never saw.
    std::string toolchainRequest;
    bool        toolchainRecorded = false;
    // The payload directory of every `[xlings]` address the build resolved
    // (#716). The fast path skips the pass that notices a removed payload --
    // `xlings remove`, a pruned cache -- so it checks the directories still
    // exist and declines otherwise; the full path then re-provisions or, with
    // auto-install off, refuses naming what is missing. Recorded apart from
    // the list for the reason `depSourceRootsRecorded` is.
    std::vector<std::string> xlingsPayloads;
    bool                     xlingsPayloadsRecorded = false;
    // The workspace selection of the command that wrote the entry, and the
    // members of the entry's own plan, each joined by a unit separator
    // (workspace design 2026-09-29 §15). Empty outside a workspace. One
    // command on a workspace writes one entry per configuration group, and a
    // fast path matches every entry of its selection.
    std::string              selection;
    std::string              group;
    // The engine that wrote the graph this entry records, absent for an entry
    // written before the field (see EngineIdentity). Optional rather than an
    // empty identity, because an engine whose version is the empty string does
    // not exist and "no record" must not be representable as a value.
    std::optional<EngineIdentity> engine;
};

// One list of extensions as a line of the record carries it: joined by a unit
// separator. An extension is a dot and a name, so it holds neither that
// character nor a tab or a newline, which the line's own structure relies on.
// The two directions are kept together so that the reader and the writer cannot
// disagree about the separator.
constexpr char kExtensionSeparator = '\x1f';

std::vector<std::string> split_extensions(std::string_view joined) {
    std::vector<std::string> out;
    while (!joined.empty()) {
        const auto at = joined.find(kExtensionSeparator);
        out.emplace_back(joined.substr(0, at));
        if (at == std::string_view::npos) break;
        joined.remove_prefix(at + 1);
    }
    return out;
}

std::string join_extensions(const std::vector<std::string>& extensions) {
    std::string out;
    for (auto const& x : extensions) {
        if (!out.empty()) out += kExtensionSeparator;
        out += x;
    }
    return out;
}

export std::vector<BuildCacheEntry> read_build_cache(const std::filesystem::path& projectRoot) {
    auto path = projectRoot / kBuildCacheFile;
    std::ifstream f(path);
    if (!f) return {};

    std::string firstLine;
    if (!std::getline(f, firstLine) || firstLine.empty()) return {};

    // Detect legacy format (first line is an absolute path, not "[target=...]").
    if (firstLine[0] != '[') {
        // Legacy 4-line format: outputDir, ninjaProgram, target, fingerprint.
        BuildCacheEntry e;
        e.outputDir = firstLine;
        std::getline(f, e.ninjaProgram);
        std::getline(f, e.targetTriple);
        std::getline(f, e.fingerprint);
        if (e.outputDir.empty() || e.ninjaProgram.empty()) return {};
        return {e};
    }

    // P3 multi-entry format: sections of [target=<triple>] + 3 mandatory
    // lines, plus optional runtime-env lines added after toolenv moved out of
    // build.ninja. Old cache entries omit them and are treated as stale.
    std::vector<BuildCacheEntry> entries;
    std::string line = firstLine;
    while (true) {
        // Parse [target=<triple>]
        if (line.size() < 9 || !line.starts_with("[target=") || line.back() != ']')
            break;
        BuildCacheEntry e;
        e.targetTriple = line.substr(8, line.size() - 9);
        if (!std::getline(f, e.outputDir) || e.outputDir.empty()) break;
        if (!std::getline(f, e.ninjaProgram) || e.ninjaProgram.empty()) break;
        std::getline(f, e.fingerprint);
        bool haveNextLine = static_cast<bool>(std::getline(f, line));
        if (haveNextLine && !line.starts_with("[target=")
                         && !line.starts_with("runTargets=")) {
            e.runtimeEnvKey = line;
            std::getline(f, e.runtimeEnvValue);
            haveNextLine = static_cast<bool>(std::getline(f, line));
        }
        // mcpp#225 (E2): optional runTargets block. Absent on caches written
        // before this field existed (or truncated/corrupt mid-block) — in
        // either case e.runTargets stays empty, which the run fast-path
        // treats as a miss, never a crash.
        if (haveNextLine && line.starts_with("runTargets=")) {
            std::size_t n = 0;
            try { n = std::stoul(line.substr(11)); } catch (...) { n = 0; }
            for (std::size_t i = 0; i < n && std::getline(f, line); ++i) {
                auto tab = line.find('\t');
                if (tab == std::string::npos) continue;
                e.runTargets.emplace_back(line.substr(0, tab), line.substr(tab + 1));
            }
            haveNextLine = static_cast<bool>(std::getline(f, line));
        }
        // mcpp#225 (E2): optional run-env block (the process env needed to
        // exec a cached run-target, e.g. LD_LIBRARY_PATH). Same back-compat
        // contract as runTargets above.
        if (haveNextLine && line.starts_with("runEnv=")) {
            e.runEnvKey = line.substr(7);
            std::getline(f, e.runEnvValue);
            haveNextLine = static_cast<bool>(std::getline(f, line));
        }
        // Optional subos line. Same back-compat contract: absent ⇒ empty ⇒
        // the run fast path treats the entry as a miss, exactly as it already
        // does for a cache written before runtimeEnvKey existed. Running with
        // a DIFFERENT environment than the full path would be worse than not
        // using the cache at all -- the program would work once and then
        // silently stop finding its runtime data.
        if (haveNextLine && line.starts_with("subos=")) {
            e.subosDir      = line.substr(6);
            e.subosRecorded = true;
            haveNextLine = static_cast<bool>(std::getline(f, line));
        }
        if (haveNextLine && line.starts_with("runtimeBinding=")) {
            auto decoded = mcpp::platform::runtime::deserialize_runtime_binding(
                line.substr(15));
            if (decoded) e.runtimeBinding = std::move(*decoded);
            haveNextLine = static_cast<bool>(std::getline(f, line));
        }
        // Optional profile line. Same back-compat contract as the two blocks
        // above: absent ⇒ e.profile stays empty ⇒ every fast path treats the
        // entry as a miss and falls through to prepare_build.
        if (haveNextLine && line.starts_with("profile=")) {
            e.profile = line.substr(8);
            haveNextLine = static_cast<bool>(std::getline(f, line));
        }
        if (haveNextLine && line.starts_with("cacheMode=")) {
            e.cacheMode = line.substr(10);
            haveNextLine = static_cast<bool>(std::getline(f, line));
        }
        // Count-prefixed, like `runTargets=` above and for the same reason: a
        // zero-length list and an absent block must not read the same. Absent
        // means the cache predates the field, and the fast path then declines
        // once so the next write records it.
        //
        // The block that listed the paths alone is read past and left
        // unrecorded: its roots have no tables, so the entry declines once and
        // the next write records both.
        if (haveNextLine && line.starts_with("depSourceRoots=")) {
            std::size_t n = 0;
            try { n = std::stoul(line.substr(15)); } catch (...) { n = 0; }
            for (std::size_t i = 0; i < n && std::getline(f, line); ++i) {}
            haveNextLine = static_cast<bool>(std::getline(f, line));
        }
        // `depSources=<n>`, then one line per root: the path, the module
        // extensions and the device extensions of the root's package, separated
        // by a tab, the extensions of one list by a unit separator. A line that
        // is not of that shape makes the whole block unrecorded rather than
        // read with a table nobody wrote.
        if (haveNextLine && line.starts_with("depSources=")) {
            std::size_t n = 0;
            try { n = std::stoul(line.substr(11)); } catch (...) { n = 0; }
            bool wellFormed = true;
            std::vector<DepSourceRoot> roots;
            for (std::size_t i = 0; i < n && std::getline(f, line); ++i) {
                const auto t1 = line.find('\t');
                const auto t2 = t1 == std::string::npos ? t1 : line.find('\t', t1 + 1);
                if (t2 == std::string::npos) { wellFormed = false; continue; }
                roots.push_back({std::filesystem::path(line.substr(0, t1)),
                                 split_extensions(std::string_view(line).substr(t1 + 1, t2 - t1 - 1)),
                                 split_extensions(std::string_view(line).substr(t2 + 1))});
            }
            if (wellFormed && roots.size() == n) {
                e.depSourceRoots = std::move(roots);
                e.depSourceRootsRecorded = true;
            }
            haveNextLine = static_cast<bool>(std::getline(f, line));
        }
        // Optional `runner=0|1` (#544). Absent ⇒ false; see the field.
        if (haveNextLine && line.starts_with("runner=")) {
            e.runnerDeclared = (line.substr(7) == "1");
            haveNextLine = static_cast<bool>(std::getline(f, line));
        }
        // Optional `runtier=0|1`. Absent ⇒ false; see the field.
        if (haveNextLine && line.starts_with("runtier=")) {
            e.runTierPending = (line.substr(8) == "1");
            haveNextLine = static_cast<bool>(std::getline(f, line));
        }
        // Optional `features=<list>`. Absent ⇒ empty; see the field.
        if (haveNextLine && line.starts_with("features=")) {
            e.features = line.substr(9);
            haveNextLine = static_cast<bool>(std::getline(f, line));
        }
        // Optional `toolchain=<request>`. Absent means the entry predates the
        // field, and every fast path declines it once; see the field.
        if (haveNextLine && line.starts_with("toolchain=")) {
            e.toolchainRequest  = line.substr(10);
            e.toolchainRecorded = true;
            haveNextLine = static_cast<bool>(std::getline(f, line));
        }
        // Count-prefixed, as `depSourceRoots=` is: absent means the entry
        // predates the field and the fast paths decline it once.
        if (haveNextLine && line.starts_with("xlingsPayloads=")) {
            std::size_t n = 0;
            try { n = std::stoul(line.substr(15)); } catch (...) { n = 0; }
            for (std::size_t i = 0; i < n && std::getline(f, line); ++i)
                e.xlingsPayloads.push_back(line);
            e.xlingsPayloadsRecorded = true;
            haveNextLine = static_cast<bool>(std::getline(f, line));
        }
        // Optional `selection=` and `group=`; absent outside a workspace.
        if (haveNextLine && line.starts_with("selection=")) {
            e.selection = line.substr(10);
            haveNextLine = static_cast<bool>(std::getline(f, line));
        }
        if (haveNextLine && line.starts_with("group=")) {
            e.group = line.substr(6);
            haveNextLine = static_cast<bool>(std::getline(f, line));
        }
        // `engine=<version>`, a tab, and the executable's path; see
        // EngineIdentity. The version holds no tab, so the path is everything
        // after the first one. Absent, or without the tab, the entry declines
        // once.
        if (haveNextLine && line.starts_with("engine=")) {
            if (const auto tab = line.find('\t'); tab != std::string::npos)
                e.engine = EngineIdentity{line.substr(7, tab - 7), line.substr(tab + 1)};
            haveNextLine = static_cast<bool>(std::getline(f, line));
        }
        entries.push_back(std::move(e));
        if (!haveNextLine || line.empty()) break;
    }
    return entries;
}

// Serialize the P3 format. Declared ahead of its single caller so the reader
// and the writer of this file sit next to each other.
export void write_build_cache_entries(const std::filesystem::path& path,
                                      const std::vector<BuildCacheEntry>& entries);

// `a, b` and `b a` are one request. Normalised on both sides of the comparison
// — the entry stores this form and the fast path computes it — so a cache hit
// depends on the SET rather than on how it was typed.
std::string normalize_features(std::string_view raw) {
    std::vector<std::string> toks;
    for (std::size_t i = 0; i < raw.size();) {
        auto c = raw.find_first_of(", ", i);
        auto t = raw.substr(i, c == std::string_view::npos ? c : c - i);
        if (!t.empty()) toks.emplace_back(t);
        if (c == std::string_view::npos) break;
        i = c + 1;
    }
    std::ranges::sort(toks);
    toks.erase(std::unique(toks.begin(), toks.end()), toks.end());
    std::string out;
    for (auto const& t : toks) { if (!out.empty()) out += ','; out += t; }
    return out;
}

void write_build_cache(const std::filesystem::path& projectRoot,
                       const std::filesystem::path& outputDir,
                       const std::string& ninjaProgram,
                       const std::string& targetTriple,
                       const std::string& fingerprintHex = "",
                       const std::string& runtimeEnvKey = "-",
                       const std::string& runtimeEnvValue = "",
                       std::vector<std::pair<std::string, std::string>> runTargets = {},
                       const std::string& runEnvKey = "",
                       const std::string& runEnvValue = "",
                       const std::string& profile = "",
                       const std::string& cacheMode = "",
                       const mcpp::platform::runtime::RuntimeBinding& runtimeBinding = {},
                       std::vector<DepSourceRoot> depSourceRoots = {},
                       bool runnerDeclared = false,
                       bool runTierPending = false,
                       const std::string& features = {},
                       const std::string& toolchainRequest = {},
                       std::vector<std::string> xlingsPayloads = {},
                       const std::string& selection = {},
                       const std::string& group = {}) {
    // The configuration groups of one workspace command build at the same
    // time (workspace design 2026-09-29 §6) and record into one file; the
    // read, the edit and the write are one step.
    static std::mutex cacheWrite;
    std::lock_guard lock(cacheWrite);
    auto path = projectRoot / kBuildCacheFile;
    auto entries = read_build_cache(projectRoot);

    // Remove the existing entry for this (target, profile) pair. Keying on the
    // triple alone made a release build evict the dev entry and vice versa, so
    // switching profiles back and forth could never be incremental AND the
    // surviving entry pointed at the other profile's build dir.
    std::erase_if(entries, [&](const BuildCacheEntry& e) {
        return e.targetTriple == targetTriple && e.profile == profile
            && e.selection == selection && e.group == group;
    });

    // Insert at front (MRU).
    BuildCacheEntry newEntry{targetTriple, outputDir.string(), ninjaProgram, fingerprintHex,
                             runtimeEnvKey, runtimeEnvValue, std::move(runTargets),
                             runEnvKey, runEnvValue, runtimeBinding.subosDir.string(),
                             /*subosRecorded=*/true,
                             profile, cacheMode};
    newEntry.runtimeBinding = runtimeBinding;
    newEntry.depSourceRoots = std::move(depSourceRoots);
    newEntry.depSourceRootsRecorded = true;
    newEntry.runnerDeclared = runnerDeclared;
    newEntry.runTierPending = runTierPending;
    newEntry.features = features;
    newEntry.toolchainRequest  = toolchainRequest;
    newEntry.toolchainRecorded = true;
    newEntry.xlingsPayloads = std::move(xlingsPayloads);
    newEntry.xlingsPayloadsRecorded = true;
    newEntry.selection = selection;
    newEntry.group = group;
    // The engine that wrote the graph is the one writing the record: both
    // happen in this process, after the graph was emitted.
    newEntry.engine = running_engine();
    entries.insert(entries.begin(), std::move(newEntry));

    // Trim to LRU capacity.
    if ((int)entries.size() > kBuildCacheMaxEntries)
        entries.resize(kBuildCacheMaxEntries);

    write_build_cache_entries(path, entries);
}

void write_build_cache_entries(const std::filesystem::path& path,
                               const std::vector<BuildCacheEntry>& entries) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream f(path, std::ios::trunc);
    if (!f) return;
    for (auto& e : entries) {
        f << "[target=" << e.targetTriple << "]\n";
        f << e.outputDir << '\n';
        f << e.ninjaProgram << '\n';
        f << e.fingerprint << '\n';
        f << (e.runtimeEnvKey.empty() ? "-" : e.runtimeEnvKey) << '\n';
        f << e.runtimeEnvValue << '\n';
        // mcpp#225 (E2): run-targets + their exec env, always written (even
        // when empty) so a reader never has to guess whether a missing
        // block means "no targets" vs "cache predates this field" — the
        // count-prefixed block is unambiguous either way, and back-compat
        // for OLD caches (no such block at all) is handled on the read side.
        f << "runTargets=" << e.runTargets.size() << '\n';
        for (auto& [name, exe] : e.runTargets) f << name << '\t' << exe << '\n';
        f << "runEnv=" << e.runEnvKey << '\n';
        f << e.runEnvValue << '\n';
        f << "subos=" << e.subosDir << '\n';
        if (e.runtimeBinding)
            f << "runtimeBinding="
              << mcpp::platform::runtime::serialize_runtime_binding(*e.runtimeBinding)
              << '\n';
        f << "profile=" << e.profile << '\n';
        f << "cacheMode=" << e.cacheMode << '\n';
        // A root whose path holds a tab or a line break cannot be written in
        // this line format: read back, the path would be cut at the tab, and a
        // directory that happened to exist under the shorter name would be
        // swept with the wrong table. Such an entry records no roots, which
        // reads as "predates the list" and declines the fast path, the safe
        // direction.
        const bool writable = std::ranges::none_of(e.depSourceRoots, [](auto const& r) {
            return r.root.generic_string().find_first_of("\t\n\r") != std::string::npos;
        });
        if (writable) {
            f << "depSources=" << e.depSourceRoots.size() << '\n';
            for (auto& r : e.depSourceRoots)
                f << r.root.generic_string() << '\t' << join_extensions(r.moduleExtensions)
                  << '\t' << join_extensions(r.deviceExtensions) << '\n';
        }
        f << "runner=" << (e.runnerDeclared ? 1 : 0) << '\n';
        f << "runtier=" << (e.runTierPending ? 1 : 0) << '\n';
        f << "features=" << e.features << '\n';
        f << "toolchain=" << e.toolchainRequest << '\n';
        f << "xlingsPayloads=" << e.xlingsPayloads.size() << '\n';
        for (auto& p : e.xlingsPayloads) f << p << '\n';
        if (!e.selection.empty() || !e.group.empty()) {
            f << "selection=" << e.selection << '\n';
            f << "group=" << e.group << '\n';
        }
        if (e.engine)
            f << "engine=" << e.engine->version << '\t' << e.engine->exe << '\n';
    }
}

std::vector<std::string> read_ninja_command_prefixes(const std::filesystem::path& ninjaPath) {
    std::ifstream f(ninjaPath);
    if (!f) return {};

    std::vector<std::string> prefixes;
    std::string line;
    while (std::getline(f, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        auto key = line.substr(0, eq);
        while (!key.empty() && std::isspace(static_cast<unsigned char>(key.back())))
            key.pop_back();
        // `mcpp` drives the dyndep + stage_file rules; treating it as a command
        // prefix filters the echoed command line while keeping the diagnostic
        // mcpp itself printed (#311).
        if (key != "cxx" && key != "cc" && key != "ar" && key != "scan_deps"
            && key != "mcpp")
            continue;

        std::string value = line.substr(eq + 1);
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
            value.erase(value.begin());
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
            value.pop_back();
        if (!value.empty())
            prefixes.push_back(std::move(value));
    }
    return prefixes;
}

bool is_stale_ninja_failure(std::string_view output) {
    return output.find("loading 'build.ninja'") != std::string_view::npos
        || output.find("loading build.ninja") != std::string_view::npos
        || output.find("unknown target") != std::string_view::npos
        || output.find("manifest 'build.ninja' still dirty") != std::string_view::npos
        // A cached build.ninja can reference an input (e.g. a dependency
        // source under the registry) that moved or was reinstalled since the
        // graph was generated — the build fingerprint does not yet cover
        // registry dep state, so the stale graph is reused. Ninja then aborts
        // with this signature. Treat it as stale → drop to a full regen
        // instead of hard-failing and forcing the user to `mcpp clean`.
        || output.find("missing and no known rule to make") != std::string_view::npos;
}

// mcpp#225 (E2): the (name, exe-path-relative-to-outputDir) pairs for every
// binary link unit in a resolved plan, cached alongside the build
// fingerprint so `mcpp run` can locate an executable without re-running
// prepare_build (see BuildCacheEntry::runTargets / try_fast_run below).
// TestBinary/library link units never run via `mcpp run`, so only Binary
// link units are collected.
std::vector<std::pair<std::string, std::string>>
compute_run_targets(const mcpp::build::BuildPlan& plan) {
    std::vector<std::pair<std::string, std::string>> out;
    for (auto& lu : plan.linkUnits) {
        if (lu.kind != mcpp::build::LinkUnit::Binary) continue;
        // A dependency's program shipped with this one (mcpp#711) is not a
        // program of this package, and `mcpp run` does not choose it.
        if (!lu.artifactOf.empty()) continue;
        out.emplace_back(lu.targetName, lu.output.generic_string());
    }
    return out;
}

// mcpp#225 (E2): the process env needed to exec a run-target (e.g.
// LD_LIBRARY_PATH for dep .so's not covered by the exe's own RUNPATH).
// Shared between build_run_target's normal (prepare_build) path and its
// cached fast path so both derive the same env from the same source.
std::pair<std::string, std::string>
compute_run_env(const mcpp::build::BuildPlan& plan) {
    auto key = mcpp::platform::env::runtime_library_path_key();
    auto value = mcpp::platform::env::prepend_path_list(key, plan.runtimeLibraryDirs);
    if (key.empty() || value.empty()) return {"", ""};
    return {key, value};
}

// The environment captured by this build's selected RuntimeBinding (mcpp#352).
//
// A GL application needs three things and mcpp only ever supplied two: the
// binary links (bootstrap), it finds its libraries (RPATH), and then it has to
// be told which driver module to load and which GL vendors exist. That third
// one is a set of environment variables, xlings's graphics packages declare
// them into the subos, and until now nothing carried them to a program mcpp
// launched — `xlings subos use` applied them, `mcpp run` did not. Hence a
// binary that links fine and exits 255 with no output.
//
// The declarations are snapshotted with the build and serialized into the
// fast-path cache. A changed SubOS manifest invalidates that cache and causes a
// fresh prepare; no invocation mixes newly read run state with old objects.
//
// mcpp does not know what any of these variables MEAN, and that is the design:
// when the ecosystem gains a Vulkan loader or a new driver bridge, the
// declaration changes and this code does not.
std::vector<std::pair<std::string, std::string>>
compute_subos_env(const mcpp::build::BuildPlan& plan) {
    return mcpp::platform::runtime::resolve_runtime_environment(
        plan.runtimeBinding,
        [](std::string_view v) -> std::optional<std::string> {
            if (const char* e = std::getenv(std::string(v).c_str()))
                return std::string(e);
            return std::nullopt;
        });
}

// THE FILES A RUNNER HAS TO CARRY WITH THE ARTIFACT (#634 A6).
//
// A runner receives the artifact's path and nothing else, and for a runner
// that executes the artifact on this machine that is enough: the files beside
// it are beside it. A runner that moves the artifact -- `adb-run` pushes the
// program to a device -- moved only the program, and a test reading its
// deployed data then failed on the emulator with `open failed:
// /data/local/tmp/data/data.txt` while passing on the host and on the iOS
// simulator, which reads the host's filesystem.
//
// THE LIST IS WHAT THE ARTIFACT LOADS OR READS FROM ITS OWN DIRECTORY, AS THE
// BUILD LAID IT OUT: every `[runtime] deploy` and `deploy_files` entry, and
// every shared library the plan links, which consumers find beside them
// through `$ORIGIN` or `@loader_path`. A test of a package whose dependency is
// shared on a row (#634 A1) needs that library on the device as much as its
// data. The staged copy in the output tree is named, not the declared source:
// it is the file the artifact reads when it runs here.
//
// One line per file: the destination relative to the artifact's directory,
// with `/` separators, a TAB, and the absolute path of the file. A TAB,
// because a Windows user directory commonly contains a space. A destination
// begins with `../` when the artifact sits below the tree's root, as a test
// discovered in a subdirectory does. The file exists for every runner
// invocation and is empty when there is nothing to carry, so a runner can
// tell an engine that states "nothing" from one that predates the variable.
constexpr std::string_view kRuntimeFilesEnv = "MCPP_RUNTIME_FILES";

// `owner`, for a plan of several workspace members, is the member whose
// artifact this is: the shared libraries another member's targets link are not
// this artifact's files.
std::vector<std::pair<std::string, std::filesystem::path>>
runtime_files_for(const mcpp::build::BuildContext& ctx,
                  const std::filesystem::path& artifact,
                  std::string_view owner = {}) {
    std::vector<std::pair<std::string, std::filesystem::path>> out;
    const auto artifactDir = artifact.parent_path().lexically_normal();
    const auto artifactNorm = artifact.lexically_normal();
    std::set<std::string> seen;
    auto add = [&](const std::filesystem::path& relToOutputDir) {
        const auto staged = (ctx.outputDir / relToOutputDir).lexically_normal();
        if (staged == artifactNorm) return;
        auto dest = staged.lexically_relative(artifactDir).generic_string();
        if (dest.empty() || !seen.insert(dest).second) return;
        out.emplace_back(std::move(dest), staged);
    };
    // The resolver's answer, not the plan's candidates: a runtime search
    // directory's copy of a name that another kind outranks is not placed,
    // and a list naming it would name a file that is not there.
    for (auto const& d : mcpp::build::compute_flags(ctx.plan).runtimeDeploy) add(d.dest);
    for (auto const& lu : ctx.plan.linkUnits) {
        if (lu.kind != mcpp::build::LinkUnit::SharedLibrary) continue;
        if (!owner.empty() && !lu.memberOf.empty() && lu.memberOf != owner) continue;
        add(lu.output);
        for (auto const& alias : lu.runtimeAliases) add(alias);
    }
    return out;
}

// Writes the list for `artifact` under the output tree and returns its path.
// `carried` is empty for a distributable, which holds its own files.
std::expected<std::filesystem::path, std::string>
write_runtime_files_list(
    const mcpp::build::BuildContext& ctx,
    const std::filesystem::path& artifact,
    const std::vector<std::pair<std::string, std::filesystem::path>>& carried) {
    namespace fs = std::filesystem;
    std::error_code ec;
    auto rel = artifact.lexically_normal().lexically_relative(
        ctx.outputDir.lexically_normal());
    if (rel.empty() || *rel.begin() == "..")
        rel = fs::path("distributable") / artifact.filename();
    auto listPath = ctx.outputDir / ".mcpp-runtime-files" / rel;
    listPath += ".tsv";
    fs::create_directories(listPath.parent_path(), ec);
    std::ofstream os(listPath, std::ios::binary | std::ios::trunc);
    for (auto const& [dest, source] : carried)
        os << dest << '\t' << source.string() << '\n';
    os.flush();
    if (!os)
        return std::unexpected(std::format(
            "could not write the runtime-files list '{}' a runner receives as {}",
            listPath.string(), kRuntimeFilesEnv));
    return listPath;
}

// Compile a prepared BuildContext. Shared between `mcpp build` and `mcpp run`
// so the latter doesn't call prepare_build twice (and re-print the toolchain
// resolution banner).
// How many compiles to run at once.
// Concurrency and the module-edge schedule are resolved together in
// mcpp.build.schedule.policy and stamped onto the plan, so this reads one value
// instead of re-deriving it. `scheduleNinjaJobs` is NOT the compiler cap under
// detach-codegen: a detached compiler stops holding a ninja slot, so ninja is
// handed a larger number on purpose.
// THE read point for "how is this artifact executed".
//
// One function, two callers (`mcpp run` and `mcpp test`). Deriving it twice is
// the shape this codebase has paid for repeatedly (#233/#240/#242/#344): it
// does not fail when you add the second derivation, it fails later, when one
// of them gains a rule the other does not.
//
// Returns an empty argv when nothing is declared — the caller runs the
// artifact directly.
//
// Read for EVERY target (#544). The freestanding predicate used to gate this
// read, which is how a runner declared under a hosted cross triple
// (`[target.aarch64-linux-musl].runner` on an x86_64 host) was parsed,
// validated, documented and never consulted: `mcpp run` exec'd the artifact
// bare, the kernel refused it with ENOEXEC, and nothing said so. The
// predicate now decides exactly one thing — whether an absent runner is fatal
// before any spawn is attempted — and `RunnerChoice::freestanding` keeps that
// meaning. Whether THIS host can execute a hosted artifact is not predicted
// here or anywhere: mcpp does what the project declared, or attempts the
// launch and reports what the kernel answered (design §3, P1).
struct RunnerChoice {
    std::vector<std::string> tmpl;      // empty = execute the artifact directly
    bool freestanding = false;          // an EMPTY tmpl is fatal when true
    bool fromManifest = false;          // the consumer overrode a dependency's
    bool ignored = false;               // --no-runner dropped a declared template
    bool longLived = false;             // declared by the package; no natural end
    bool fromPayload = false;           // the toolchain payload's runner, nothing declared
    // The spelling that names this target in the manifest: the canonical form,
    // which is also the output directory's name and the key every
    // `[target.<triple>]` reader resolves. Every diagnostic below prints this
    // rather than `tc.targetTriple`, because each one either names the key the
    // author wrote or prints a key to paste, and the driver's own spelling is
    // neither — on macOS it is `arm64-apple-darwin24.6.0`, which no
    // `[target.…]` lookup matches. Derived here, once, so the lookup and the
    // message it produces cannot disagree about which target they mean.
    std::string tripleKey;
};

// ONE READER FOR FOUR SLOTS, PARAMETERISED BY THE SLOT.
//
// `run`, `flash`, `monitor` and `debug` resolve identically: a dependency
// supplies a template, the project may override it on the same axis, the
// override is reported, and the canonical triple spelling is the lookup key.
// Every one of those four facts was learned the hard way for `runner` alone
// (#544, and the macOS `arm64-apple-darwin24.6.0` mismatch measured on CI).
// Copying the function three times would copy the four facts three times and
// let them drift apart — the shape this file's own header warns about.
//
// `which` selects the slot; everything else is shared.
RunnerChoice choose_device_action(const BuildContext& ctx,
                                  std::string_view which,
                                  bool noRunner = false) {
    RunnerChoice c;
    const bool isDefault = which.empty();
    const auto ft = mcpp::toolchain::triple::parse(ctx.tc.targetTriple);
    if (ft) c.freestanding = ft->is_freestanding();
    c.tripleKey = ft ? ft->str() : ctx.tc.targetTriple;
    // The graph's answer: the default runner, or a named one a package supplied.
    if (isDefault) {
        c.tmpl = ctx.manifest.buildConfig.runner;
    } else if (auto it = ctx.manifest.buildConfig.namedRunners.find(std::string(which));
               it != ctx.manifest.buildConfig.namedRunners.end()) {
        c.tmpl      = it->second.argv;
        c.longLived = it->second.longLived;
    }
    // The row is found as every other `[target.<triple>]` reader finds it
    // (`find_target_entry`), whichever spelling the section and the toolchain
    // use. The toolchain's own `targetTriple` is what the driver reported,
    // which on macOS is `arm64-apple-darwin24.6.0`; an exact lookup of it
    // matched on Linux and never on macOS (measured on CI, 2026-09-02), and an
    // exact lookup of the canonical spelling missed a section written
    // `[target.x86-windows-msvc]`. The raw form is kept as a fallback for a
    // triple the parser does not know.
    auto with_slot = [&](const mcpp::manifest::TargetEntry* e) {
        const mcpp::manifest::TargetEntry* none = nullptr;
        if (!e) return none;
        if (isDefault) return e->runner.empty() ? none : e;
        auto nr = e->namedRunners.find(std::string(which));
        return (nr != e->namedRunners.end() && !nr->second.empty()) ? e : none;
    };
    const mcpp::manifest::TargetEntry* entry = nullptr;
    if (ft) {
        entry = with_slot(mcpp::build::find_target_entry(ctx.manifest, *ft));
    } else if (auto it = ctx.manifest.targetOverrides.find(ctx.tc.targetTriple);
               it != ctx.manifest.targetOverrides.end()) {
        entry = with_slot(&it->second);
    }
    if (entry) {
        if (isDefault) {
            c.fromManifest = !ctx.manifest.buildConfig.runner.empty();
            c.tmpl = entry->runner;
        } else {
            c.fromManifest = ctx.manifest.buildConfig.namedRunners.contains(
                std::string(which));
            c.tmpl = entry->namedRunners.at(std::string(which));
        }
    }
    // AND THE TOOLCHAIN'S OWN ANSWER, WHEN THE PROJECT AND ITS GRAPH GAVE NONE.
    //
    // The third source and the last. A project's `[target.<triple>] runner`
    // and a package's `mcpp::runner(...)` both outrank it, because they are
    // statements about THIS program; the payload's is a statement about
    // everything its compiler produces. Measured 2026-09-12 in a sandbox with
    // no `node` on PATH: an Emscripten artefact built correctly and then
    // stopped at `#!/usr/bin/env node`, while the node the payload had
    // declared sat in its store. See `PayloadDescriptor::runner`.
    //
    // The run slot only. `flash`, `monitor` and `debug` name actions a board
    // package owns, and a compiler has no opinion about them.
    if (isDefault && c.tmpl.empty()) {
        c.tmpl = mcpp::toolchain::payload_default_runner(ctx.tc.binaryPath);
        c.fromPayload = !c.tmpl.empty();
    }
    // `--no-runner` is the operator on THIS host stating a host fact the
    // manifest cannot carry: the triple is native here. On a freestanding
    // target that leaves nothing to execute, and the caller's existing
    // no-runner error is the correct answer.
    if (noRunner && !c.tmpl.empty()) { c.tmpl.clear(); c.ignored = true; }
    return c;
}

RunnerChoice choose_runner(const BuildContext& ctx, bool noRunner) {
    return choose_device_action(ctx, std::string_view{}, noRunner);
}
RunnerChoice choose_runner(const BuildContext& ctx) {
    return choose_device_action(ctx, std::string_view{}, false);
}

// The capacity number, printed because capacity is the constraint.
//
// After `Finished`, not instead of it: the build succeeded either way, and a
// size line that replaced the outcome would be a different kind of message.
// Silent on every hosted target and whenever the tool is absent — an
// informational line has no standing to fail a build.
void report_freestanding_size(const BuildContext& ctx) {
    auto ft = mcpp::toolchain::triple::parse(ctx.tc.targetTriple);
    if (!ft || !ft->is_freestanding()) return;
    auto tool = mcpp::freestanding::resolve_size_tool(ctx.tc.binaryPath);
    if (tool.empty()) return;
    for (auto const& lu : ctx.plan.linkUnits) {
        if (lu.kind != mcpp::build::LinkUnit::Binary) continue;
        auto art = ctx.outputDir / lu.output;
        std::error_code ec;
        if (!std::filesystem::exists(art, ec)) continue;
        // An argument vector rather than a `2>/dev/null` command string,
        // which cmd.exe cannot open on a Windows host.
        auto out = mcpp::platform::process::capture_stdout(
            {tool.string(), art.string()});
        if (out.exit_code != 0 && out.output.empty()) continue;
        auto s = mcpp::freestanding::parse_size_output(out.output);
        if (!s) continue;
        mcpp::ui::info("Size", std::format(
            "{}  text {}  data {}  bss {}  total {}",
            lu.targetName, s->text, s->data, s->bss,
            mcpp::freestanding::size_total(*s)));
    }
}

export int run_build_plan(BuildContext& ctx, bool verbose, bool no_cache,
                   std::string_view targetOverride = "") {
    // `--cache=off` means a cold build: no global cache, and target/ cleared —
    // which is exactly what `--no-cache` has always done, hence the alias.
    const bool coldBuild = no_cache || ctx.cacheMode == CacheMode::Off;
    if (coldBuild) {
        std::error_code ec;
        std::filesystem::remove_all(ctx.outputDir, ec);
    }

    // The generated `*link:` spec lives in the output directory, and the line
    // above is allowed to delete that directory. prepare wrote the file before
    // this point, so a cold build reached ninja with a link command naming a
    // spec that no longer existed -- `g++: fatal error: cannot read spec file`,
    // on every `--no-cache` build with gcc.
    //
    // Regenerating here rather than reordering: the invariant worth holding is
    // "the spec exists when ninja runs", and stating it as an invariant
    // survives the next thing that clears target/ (a user with `rm -rf`, for
    // one). The write is idempotent, so the warm path costs one stat.
    if (!ctx.plan.gccCleanSpecs.empty()) {
        std::error_code ec;
        if (!std::filesystem::exists(ctx.plan.gccCleanSpecs, ec))
            ctx.plan.gccCleanSpecs = mcpp::toolchain::write_clean_link_specs(
                ctx.tc.binaryPath, ctx.outputDir);
    }

    auto be = mcpp::build::make_ninja_backend();

    // M5.0: print "Inferred" banner when defaults / target inference fired.
    for (auto& note : ctx.manifest.inferredNotes) {
        mcpp::ui::status("Inferred", note);
    }
    // A plan of one workspace member reports what was inferred about the
    // member, as its own build did (§15).
    if (ctx.workspaceMembers.size() == 1)
        for (auto& note : ctx.workspaceMembers.front().manifest.inferredNotes)
            mcpp::ui::status("Inferred", note);

    // The packages are no longer announced before ninja runs: each package's
    // line is written when its outcome is known, with that outcome (build
    // progress design 2026-09-29, §4.2). The plan names them
    // (`BuildPlan::packages`), and a dependency served from the global cache
    // states how many units that saved, as the `Cached` line did: a number
    // that has to match the edges ninja actually skips cannot be quietly
    // wrong, where the bare word was printed for three months over full
    // recompiles.
    mcpp::build::progress::programs_done();

    // The job count, the stale-token reclaim and the report are the backend's
    // (pack drive and selection design 2026-10-01, A1, A2): every command's
    // drive takes them from the plan and the open report.
    mcpp::build::BuildOptions opts;
    opts.verbose = verbose;
    auto r = be->build(ctx.plan, opts);
    if (!r) {
        // A failed step was reported when it failed; what follows is the
        // advice that reads the whole output.
        if (!r.error().reported) mcpp::ui::error(r.error().message);
        mcpp::ui::block(r.error().diagnosticOutput);
        return 1;
    }

    // Populate the global cache for deps that did NOT hit.
    mcpp::build::populate_dependency_cache(ctx);

    // P1.5: warn if fingerprint changed from last build (explains full rebuild).
    // Compared against the entry for the SAME profile: the profile is now a
    // fingerprint input, so a dev↔release switch always changes the fp. That is
    // exactly what the user asked for, and warning about it turns a useful
    // signal ("something you didn't expect invalidated your build dir") into
    // noise on every profile switch.
    {
        auto entries = read_build_cache(ctx.projectRoot);
        for (auto& e : entries) {
            if (e.targetTriple == targetOverride && e.profile == ctx.profile
                && e.cacheMode == cache_mode_name(ctx.cacheMode)
                && e.selection == ctx.workspaceRequest && e.group == ctx.workspaceGroup
                && !e.fingerprint.empty()) {
                auto newFp = ctx.outputDir.filename().string();
                if (e.fingerprint != newFp) {
                    mcpp::ui::warning(std::format(
                        "fingerprint changed ({} → {}), full rebuild; "
                        "`mcpp clean --stale` drops the directories no build still uses",
                        e.fingerprint, newFp));
                }
                break;
            }
        }
    }

    // P0: save build cache for fast-path on next invocation.
    if (!coldBuild && !r->ninjaProgram.empty()) {
        auto fpHex = ctx.outputDir.filename().string();
        auto runTargets = compute_run_targets(ctx.plan);
        auto [runEnvKey, runEnvValue] = compute_run_env(ctx.plan);
        write_build_cache(ctx.projectRoot, ctx.outputDir, r->ninjaProgram,
                          std::string(targetOverride), fpHex,
                          r->runtimeEnvKey.empty() ? "-" : r->runtimeEnvKey,
                          r->runtimeEnvValue,
                          std::move(runTargets), runEnvKey, runEnvValue,
                          ctx.profile, std::string(cache_mode_name(ctx.cacheMode)),
                          ctx.plan.runtimeBinding,
                          ctx.depSourceRoots,
                          // #544: the run fast path declines an entry whose
                          // target has a runner declared — see the field.
                          !choose_runner(ctx).tmpl.empty(),
                          // …and one written by a build that left a run-tier
                          // tool uninstalled, for the same reason.
                          ctx.runTierPending,
                          // The feature set these artefacts were built with:
                          // the entry is matched on it, because the output
                          // directory is keyed on a fingerprint that includes
                          // it and the entry was not.
                          normalize_features(ctx.activeFeatureRequest),
                          // The toolchain request, so a later `--toolchain` or
                          // a changed machine default declines the fast path.
                          toolchain_request_identity(),
                          // The xlings payloads it read, so a removed one
                          // declines the fast path (#716).
                          [&] {
                              std::vector<std::string> v;
                              for (auto const& p : ctx.xlingsPayloads)
                                  v.push_back(p.generic_string());
                              return v;
                          }(),
                          ctx.workspaceRequest, ctx.workspaceGroup);
    }

    // The one place the --strict policy is settled. Degradations reported by
    // the backend (e.g. a toolchain/platform combination that cannot emit a
    // depfile, #257) are discovered during emission, so this has to come
    // after the build rather than at the end of prepare_build. Without this
    // call the whole diag channel would report and then be ignored — the
    // exact failure mode it exists to prevent.
    if (!mcpp::diag::flush(ctx.strict)) return 1;

    // The descriptor reads the level the compile realised, from the one
    // function `compute_flags` spells it from (#694), and from the plan's
    // manifest, which `compute_flags` reads. It once read the declared level
    // while the compile used another, and said `[optimized]` over `-Og`. The
    // step record's header carries the same value for the fast path.
    mcpp::build::progress::note_sources(ctx.plan.outputDir);
    mcpp::build::progress::finished(
        ctx.profile, mcpp::build::profile_descriptor(ctx.plan.manifest.buildConfig));
    report_freestanding_size(ctx);
    return 0;
}

// ─── P0 fast-path: skip prepare_build when build.ninja is fresh ──────
//
// On a successful build, we write `target/.build_cache` containing the
// outputDir path. On the next invocation, if build.ninja in that dir
// is newer than all source files and mcpp.toml, we invoke ninja directly
// without re-running the scanner, make_plan, or emit phases.
//
// This reduces no-change builds from ~10s to <0.5s.

// mcpp#225: is any tracked source file under `projectRoot` newer than
// `ninjaTime`? Shared by try_fast_build's and try_fast_run's freshness
// gates. Uses expand_glob's bounded ("src" prefix) + vcs/build-dir-excluded
// walk instead of a hand-rolled recursive_directory_iterator — the OLD
// staleness check here walked ALL of src/ unfiltered (harmless when src/ is
// the whole tree, but wasteful/wrong the moment a huge unrelated directory
// lives elsewhere under the project root and gets swept in by some other
// caller's broader glob; and it's the same choke-point fix as expand_glob
// itself, see scanner.cppm).
// `extTable` has no default ON PURPOSE. A default would let a future caller
// sweep with the built-in table while the project classifies with a wider one
// — the exact shape of the bug this converge is removing, reintroduced as a
// parameter default. Callers must say where their table came from.
bool sources_newer_than(const std::filesystem::path& projectRoot,
                        std::filesystem::file_time_type ninjaTime,
                        const std::vector<std::filesystem::path>& resourceScripts,
                        const mcpp::ExtensionTable& extTable) {
    std::error_code ec;
    // The root build.mcpp is a build input too — its directives shape
    // build.ninja (flags, generated/selected sources). A changed program must
    // abandon the fast path and fall through to prepare_build, where the
    // declared-input cache decides whether it actually re-runs. Without this
    // the documented "re-runs when the build.mcpp source itself changes" was
    // unreachable behind a fresh build.ninja.
    if (auto bp = projectRoot / "build.mcpp"; std::filesystem::exists(bp, ec)) {
        auto bt = std::filesystem::last_write_time(bp, ec);
        if (ec || bt > ninjaTime) return true;
    }
    // #359: a GLOB input changes without any existing file's mtime changing —
    // a new .proto appears and every timestamp below is unmoved. The mtime
    // sweep therefore cannot see it, and the fast path would report
    // "Finished dev in 0.00s" while the new file is never generated. Same
    // question as the build.mcpp check above, different kind of input.
    //
    // 2026.9.5.4: the same call now also compares a declared FILE's content and
    // a declared environment variable's value. The sweep below walks sources,
    // so a data file a build program reads is invisible to it for the same
    // reason a new .proto is.
    if (mcpp::build::program_inputs_stale(projectRoot)) return true;
    // mcpp#365: an author-written `.rc` is a third input of the same kind. It
    // is not under src/ and has no C++ extension, so the sweep below cannot see
    // it — and unlike the icon or a header the script includes, editing it can
    // change WHAT THE GRAPH SHOULD BE: the implicit-input set comes from
    // scanning the script, and the "your VERSIONINFO is named by string"
    // diagnostic is produced while scanning. Both happen in prepare_build, so a
    // fresh build.ninja made the edit invisible — the resource itself rebuilt
    // (ninja tracks it), but a newly added `#include "ids.h"` went untracked and
    // the diagnostic never fired again after the first build.
    //
    // Only `files` is swept. `icon` and `extra-inputs` are already ninja
    // implicit inputs and changing them cannot change the shape of the graph,
    // so forcing a full prepare on every icon tweak would buy nothing.
    for (auto const& f : resourceScripts) {
        auto p = f.is_absolute() ? f : (projectRoot / f);
        auto ft = std::filesystem::last_write_time(p, ec);
        if (ec) { ec.clear(); continue; }   // missing → prepare_build reports it
        if (ft > ninjaTime) return true;
    }
    // The one place classification is legitimately re-derived: this runs
    // BEFORE prepare, so there is no plan to read a kind from. It uses the
    // SAME table the scanner will use (this project's manifest), so the two
    // cannot drift — which is exactly what the old hand-written list did.
    //
    // The question here is NOT "did a file change" (ninja answers that) but
    // "could the SHAPE of the graph have changed". A `.ixx` missing from the
    // old list meant a new `import` inside one never invalidated the fast
    // path: ninja recompiled the object, the dyndep edges stayed stale, and
    // nothing reported anything.
    for (auto& f : mcpp::modgraph::expand_glob(projectRoot, "src/**/*")) {
        if (!mcpp::affects_graph_shape(mcpp::classify(f, extTable))) continue;
        auto ft = std::filesystem::last_write_time(f, ec);
        if (ec || ft > ninjaTime) return true;
    }
    return false;
}

// The same question, asked of a source tree that is not the project's own.
//
// WHY THIS IS NOT COVERED BY THE SWEEP ABOVE, AND WHY NINJA DOES NOT COVER IT
// EITHER. A `path` dependency's translation units are compiled by edges in the
// SAME build.ninja, so an EDIT to one of its existing files is caught: ninja
// rebuilds the object, the link output moves, and `artifact_snapshot_unchanged`
// abandons the fast path afterwards. A NEW FILE has no edge at all. Nothing in
// the graph mentions it, every recorded timestamp is unmoved, and the fast path
// replays a build.ninja that predates it — measured, `mcpp build` printed
// `Finished dev in 0.00s` and the module was never compiled. That is #359's
// shape ("a GLOB input changes without any existing file's mtime changing")
// applied to a directory the original fix did not reach.
//
// It matters most exactly where it is hardest to notice: members of a workspace
// depend on one another by `path`, so for a workspace this is not an edge case
// but the ordinary arrangement.
//
// The dependency's own manifest is swept too. A member that gains a target, a
// `[modules] sources` glob or a `[build]` flag changes what the graph SHOULD
// be, and none of that is visible from its source files' timestamps.
//
// EACH ROOT IS CLASSIFIED BY ITS OWN PACKAGE (#756). Which of a tree's files can
// change the graph depends on the extensions the package that owns the tree
// declares: a provider's `.ixx` is a module interface because the provider says
// so, whatever the consumer declares, and a consumer with only `.cpp` sources
// has no reason to. The sweep used the consumer's table for every root, so an
// edit to the provider's host module was classified as a file of no interest and
// replayed as "no work". The tables are the ones prepare recorded with the root.
export bool dep_sources_newer_than(const std::vector<DepSourceRoot>& depSourceRoots,
                                   std::filesystem::file_time_type ninjaTime) {
    std::error_code ec;
    for (auto const& dep : depSourceRoots) {
        const std::filesystem::path& depRoot = dep.root;
        const auto extTable = mcpp::extension_table_for(dep.moduleExtensions,
                                                        dep.deviceExtensions);
        // A dependency directory that has gone away is a resolution question,
        // not a staleness one: fall through to prepare_build, which reports it
        // with the dependency's name instead of a bare missing path.
        if (!std::filesystem::is_directory(depRoot, ec)) { ec.clear(); return true; }
        auto tomlTime = std::filesystem::last_write_time(depRoot / "mcpp.toml", ec);
        if (ec) { ec.clear(); return true; }
        if (tomlTime > ninjaTime) return true;
        // THE WHOLE TREE, NOT `src/`. A dependency's units outside `src/` --
        // a feature's `rules/x.cppm`, a `[build] sources` glob elsewhere --
        // are as much its sources as those under it, and a host module among
        // them is compiled into the consumer's BUILD PROGRAM, which no edge of
        // this build.ninja names: an edit to one was replayed as "no work"
        // (#734, measured on mcpp-plugins' `deps/vcpkg.cppm`). Version-control,
        // hidden and build-output directories are skipped, and so is a nested
        // package (a directory with its own mcpp.toml), whose files are its
        // own package's sources, not this one's.
        auto it = std::filesystem::recursive_directory_iterator(
            depRoot, std::filesystem::directory_options::skip_permission_denied, ec);
        if (ec) { ec.clear(); return true; }
        for (; it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) { ec.clear(); return true; }
            const auto& path = it->path();
            if (it->is_directory(ec)) {
                const auto name = path.filename().string();
                if (name.starts_with(".") || name == "target"
                    || std::filesystem::exists(path / "mcpp.toml", ec))
                    it.disable_recursion_pending();
                ec.clear();
                continue;
            }
            if (!mcpp::affects_graph_shape(mcpp::classify(path, extTable))) continue;
            auto ft = std::filesystem::last_write_time(path, ec);
            if (ec || ft > ninjaTime) return true;
        }
    }
    return false;
}

// mcpp#225: run ninja quietly against an already-verified-fresh build.ninja.
// Shared by try_fast_build (which just reports "Finished" on success) and
// try_fast_run (which goes on to locate + exec a binary). Returns nullopt
// when ninja's failure looks like a stale-graph signature — the caller
// should abandon the fast path and fall back to a full prepare_build — or
// an exit code otherwise (0 success; 1 hard failure, diagnostics already
// printed to stderr).
std::optional<int> run_ninja_fast(const std::string& ninjaProgram,
                                  const std::filesystem::path& outputDir,
                                  const std::filesystem::path& ninjaPath,
                                  bool verbose,
                                  const std::string& runtimeEnvKey,
                                  const std::string& runtimeEnvValue,
                                  std::chrono::milliseconds* elapsedOut = nullptr) {
    // Read as it runs, into this directory's report, unless nothing is to be
    // shown (build progress design 2026-09-29, §6.1).
    const bool reporting = !mcpp::ui::is_quiet();
    std::vector<std::string> argv{ninjaProgram};
    if (!verbose && !reporting) argv.push_back("--quiet");
    argv.push_back("-C");
    argv.push_back(outputDir.string());
    if (verbose) argv.push_back("-v");
    // MCPP_NINJA_DEBUG: append ninja's own `-d` topics. The one instrument that
    // works on a process nobody can attach to.
    //
    // A ninja that spins with no command running has been observed three times
    // in fresh sandboxes, and neither gdb nor perf can reach it there
    // (ptrace_scope=1, perf_event_paranoid=4); making gdb its parent stops the
    // spin happening at all. `-d explain` prints the dirtiness decision as it
    // is made, so a ninja re-deciding the same edge names that edge in its own
    // output — which is readable from the log the runner already captures.
    //
    // Unset by default and unset in CI. It changes nothing but ninja's
    // verbosity, and it is spelled as a topic list rather than a boolean so
    // that `-d stats` and `-d keeprsp` are reachable without another variable.
    if (const char* topics = std::getenv("MCPP_NINJA_DEBUG"); topics && *topics) {
        argv.push_back("-d");
        argv.push_back(topics);
    }

    std::vector<std::pair<std::string, std::string>> childEnv;
    if (runtimeEnvKey == "@env") {
        // Multi-var encoding (MSVC INCLUDE/LIB/PATH/VSLANG + optional runtime
        // pair): \x1f-separated k=v records in the single value slot.
        std::string_view rest = runtimeEnvValue;
        while (!rest.empty()) {
            auto sep = rest.find('\x1f');
            auto rec = rest.substr(0, sep);
            if (auto eq = rec.find('='); eq != std::string_view::npos && eq > 0)
                childEnv.emplace_back(std::string(rec.substr(0, eq)),
                                      std::string(rec.substr(eq + 1)));
            if (sep == std::string_view::npos) break;
            rest.remove_prefix(sep + 1);
        }
    } else if (runtimeEnvKey != "-" && !runtimeEnvValue.empty()) {
        childEnv.emplace_back(runtimeEnvKey, runtimeEnvValue);
    }

    auto t0 = std::chrono::steady_clock::now();
    // capture_exec merges stderr into the captured output (replacing `2>&1`),
    // so is_stale_ninja_failure / filter_ninja_output still see ninja errors.
    std::string out;
    int status = 0;
    bool reported = false;
    const auto prefixes = read_ninja_command_prefixes(ninjaPath);
    // The scan pass comes first here as on the full path (build wall-time
    // plan, W2): the same passes, so the same counts.
    std::optional<std::vector<std::string>> scanArgv;
    {
        std::ifstream in(ninjaPath, std::ios::binary);
        std::string text{std::istreambuf_iterator<char>(in), {}};
        if (text.find("\nbuild " + std::string(mcpp::build::kScannedGoal) + " : phony")
            != std::string::npos) {
            scanArgv = argv;
            scanArgv->push_back(std::string(mcpp::build::kScannedGoal));
        }
    }
    if (reporting) {
        mcpp::build::progress::Build report(outputDir);
        mcpp::build::NinjaRun run;
        if (scanArgv)
            run = mcpp::build::run_ninja_reporting(*scanArgv, childEnv, std::chrono::milliseconds{0},
                                                   report, verbose, prefixes,
                                                   mcpp::build::progress::PassKind::Scan);
        // A failed scan ends the build with its own output.
        if (run.exitCode == 0 && !run.timedOut)
            run = mcpp::build::run_ninja_reporting(argv, childEnv, std::chrono::milliseconds{0},
                                                   report, verbose, prefixes);
        out = std::move(run.output);
        status = run.exitCode;
        reported = run.reported;
        // A stale graph is not this build's outcome: the full path plans
        // again and builds, and reports the packages then.
        if (status == 0 || reported || !is_stale_ninja_failure(out))
            report.finish(status == 0);
    } else {
        // Nobody reads this ninja's progress: it reports no action start
        // (build progress design 2026-09-29, §6.4).
        childEnv.emplace_back(std::string(mcpp::build::progress::kStartsEnv), "");
        mcpp::platform::process::RunResult r;
        if (scanArgv) r = mcpp::platform::process::capture_exec(*scanArgv, childEnv);
        if (r.exit_code == 0) r = mcpp::platform::process::capture_exec(argv, childEnv);
        out = std::move(r.output);
        status = r.exit_code;
    }
    if (status != 0) {
        if (!reported && is_stale_ninja_failure(out))
            return std::nullopt;
        // A failed step was reported when it failed (and every line, under
        // --verbose); what remains is the advice below.
        if (!reported) mcpp::ui::error("build failed");
        if (!reported && !(reporting && verbose))
            mcpp::ui::block(verbose ? out : mcpp::build::filter_ninja_output(out, prefixes));
        // Read from the RAW output, not from `diagnostics`: the filter drops
        // command lines, and a future filter change must not be able to
        // silently remove the advice along with them.
        if (auto advice = mcpp::build::link_failure_advice(out); !advice.empty())
            mcpp::ui::block(advice);
        // mcpp#662, the fast-path form: no `BuildPlan` here to name the C
        // library from (the whole point of this path is skipping `prepare`),
        // so both name arguments are empty — the note still fires (it reads
        // the isolation token in `out` itself) but names no package.
        if (auto advice = mcpp::build::graph_c_library_isolation_advice(out);
            !advice.empty())
            mcpp::ui::block(advice);
        // #696, the fast-path form of the same unnamed shape.
        if (auto advice = mcpp::build::graph_link_library_advice(out); !advice.empty())
            mcpp::ui::block(advice);
        // #690: the consumer-include note, from the list the plan wrote beside
        // build.ninja (`write_consumer_include_sidecar`).
        if (auto advice = mcpp::build::consumer_include_scope_advice(
                out, mcpp::build::read_consumer_include_sidecar(ninjaPath.parent_path()));
            !advice.empty())
            mcpp::ui::block(advice);
        // THE SAME ADVICE THE PLAN PATH GIVES, FROM THE LIST THE PLAN WROTE
        // DOWN. This path has no `BuildPlan` by construction, so the C
        // library's `[c-abi-absent]` table reaches it through a file beside
        // build.ninja rather than through a resolution it exists to skip.
        // Advice attached to one path only appears or not depending on
        // whether build.ninja happened to be up to date.
        {
            auto [cAbiName, absent] = mcpp::build::read_c_abi_absent_sidecar(
                ninjaPath.parent_path());
            if (auto advice = mcpp::build::c_abi_absent_facility_advice(
                    out, cAbiName, absent); !advice.empty())
                mcpp::ui::block(advice);
        }
        return 1;
    }
    // Verbose ninja output is narration, so it goes to the narration stream,
    // whether or not `--quiet` is also given (verbose output survives it).
    if (verbose && !reporting && !out.empty())
        mcpp::ui::block(out);
    // What the edges that ran had to say on success: the same reader the full
    // path calls (mcpp.build.advice), because this path skips `prepare` and a
    // report attached to one path only appears or not depending on whether
    // build.ninja was up to date.
    mcpp::build::advice::report_and_clear(outputDir);

    if (elapsedOut) {
        *elapsedOut = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0);
    }
    return 0;
}

// Which profile does this invocation mean? The fast paths exist precisely to
// avoid prepare_build, where the profile is normally settled — so they settle
// it here from the same pure rule (resolve_profile_name), which needs nothing
// but the manifest. nullopt = manifest unreadable ⇒ no fast path.
//
// Without this, an entry matched on target triple alone could have been built
// for a different profile, and the fast path would run ninja against that
// profile's build.ninja: `mcpp build --release` then a bare `mcpp build`
// reported success in 0.00s and left -O2 artifacts where -O0 -g was asked for.
struct FastPathIdentity {
    std::string profile;
    std::string cacheMode;
    // mcpp#365: author-written resource scripts, for the freshness sweep. They
    // ride along here because this is the one place on the fast path that
    // already parses the manifest — re-reading it to answer a second question
    // would be a second derivation of the same fact.
    std::vector<std::filesystem::path> resourceScripts;
    // Same argument one field down: the freshness sweep has to know which
    // extensions are module interfaces in THIS project, and this is already
    // the only manifest read on the fast path.
    mcpp::ExtensionTable extTable;
    // `[build] target` — the project's DEFAULT cross target, and the reason
    // try_fast_run cannot assume the artifact runs here. See its use.
    std::string defaultTarget;
    // `[hooks]` with at least one command (#496). Third field down riding on
    // the same single manifest read, and the only one that can VETO the fast
    // path rather than describe it — see try_fast_build.
    bool hooksActive = false;
    // What `--features` asked for, normalised so that spelling and order
    // cannot make two identical requests compare unequal.
    std::string features;
    // See toolchain_request_identity.
    std::string toolchainRequest;
};

std::optional<FastPathIdentity>
fast_path_identity(const std::filesystem::path& projectRoot,
                   std::string_view profileOverride = "",
                   std::string_view featuresRequested = "") {
    // The effective manifest: `target` is inheritable from `[workspace.build]`
    // (#690, W4).
    auto effective = mcpp::project::load_effective_manifest(projectRoot);
    if (!effective) return std::nullopt;
    const auto* m = &effective->manifest;
    return FastPathIdentity{
        mcpp::build::resolve_profile_name(*m, profileOverride),
        std::string(mcpp::build::cache_mode_name(
            mcpp::build::resolve_cache_mode(*m, ""))),
        m->resources.files,
        mcpp::extension_table_for(m->buildConfig.moduleExtensions,
                                  m->buildConfig.deviceExtensions),
        m->buildConfig.target,
        m->hooks.active(),
        normalize_features(featuresRequested),
        toolchain_request_identity(),
    };
}

// Try to fast-path: if build.ninja is newer than all inputs, just run ninja.
// Returns exit code on fast-path, or nullopt if full rebuild needed.
// WHAT THIS PROJECT CAN DO, AS OPPOSED TO WHAT THE ENGINE SUPPORTS.
//
// The engine knows no runner names, so it cannot print a static list of them —
// and that is the useful property, not a limitation. What a reader wants is
// what THIS graph supplies, which is knowable only after resolution.
export int list_runners(const std::string& package_filter,
                        const std::string& cache_mode, bool no_cache,
                        const std::string& target_triple,
                        // Which runners exist DEPENDS on the features: a board
                        // package supplies a different set for an emulator and
                        // for a probe. Reporting them without the axis that
                        // selects them would answer a question nobody asked.
                        const std::string& features = {},
                        const std::string& profile = {},
                        const std::string& accel = {}) {
    mcpp::build::BuildOverrides ov;
    ov.package_filter = package_filter;
    ov.cache_mode     = no_cache ? std::string("off") : cache_mode;
    ov.target_triple  = target_triple;
    ov.features       = features;
    ov.profile        = profile;
    ov.accel          = accel;
    // Reporting what `mcpp run` would do means resolving what `mcpp run`
    // resolves, tool tiers included — otherwise this command would list a
    // runner whose program it had declined to install.
    ov.will_run       = true;
    auto ctx = prepare_build(/*print_fp=*/false, /*includeDevDeps=*/false,
                             /*extraTargets=*/{}, ov);
    if (!ctx) { mcpp::ui::error(ctx.error()); return 2; }

    const auto& bc = ctx->manifest.buildConfig;
    const auto ft  = mcpp::toolchain::triple::parse(ctx->tc.targetTriple);
    const std::string key = ft ? ft->str() : ctx->tc.targetTriple;
    std::println("Target {}", key);

    if (bc.runner.empty() && bc.namedRunners.empty()) {
        std::println("  (none — this project reaches its artifact by executing it)");
        return 0;
    }
    if (!bc.runner.empty())
        std::println("  {:<12} {}", "(default)", bc.runner.front());
    for (auto const& [name, nr] : bc.namedRunners) {
        std::println("  {:<12} {}{}", name,
                     nr.argv.empty() ? std::string("(no argv)") : nr.argv.front(),
                     nr.longLived ? "   [long-lived]" : "");
    }
    if (bc.runExclusive)
        std::println("  note: this target's runs cannot overlap; `mcpp test` "
                     "serialises them");
    return 0;
}

// C1: a deleted root compile_commands.json used to stay deleted forever on
// the fast path, because nothing on it ever reached a writer (the fast path
// is defined as "skip preparation" — see the hooksActive check below, and
// design .agents/docs/2026-09-26-compile-database-and-issue-699-design.md
// §3.2 item 4). This restores it from the configuration's own database,
// already on disk at `outputDir` — no plan is built, so P3 (the fast path
// replays a build) holds: the root file is a copy, never a fresh plan.
// Errors are reported as warnings and never fail the fast build itself: a
// permission problem here is exactly what a normal build would already warn
// about (`write_compile_commands`), not a reason to fall back to the full
// path.
void restore_root_compile_commands(const std::filesystem::path& projectRoot,
                                   const std::filesystem::path& outputDir) {
    auto configPath = outputDir / "compile_commands.json";
    std::error_code ec;
    if (!std::filesystem::exists(configPath, ec) || ec) return;
    auto rootPath = projectRoot / "compile_commands.json";
    auto targetRoot = outputDir.parent_path().parent_path();
    auto result = mcpp::build::publish_root_compile_commands(
        configPath, rootPath, targetRoot, mcpp::home::root());
    if (!result) {
        mcpp::ui::warning(std::format(
            "compile_commands.json was not updated: {}", result.error().message));
    } else if (result->foreignEntries > 0) {
        mcpp::ui::warning(mcpp::build::foreign_entries_warning(result->foreignEntries));
    }
}

// The root compile database of a command that planned configuration groups of
// a workspace (workspace design 2026-09-29 §15): the union of the groups'
// databases (one entry per file and output), published once after every
// group, so the file does not depend on which group's build finished last.
export void publish_workspace_compile_commands(
        const std::filesystem::path& wsRoot,
        const std::vector<std::filesystem::path>& outputDirs) {
    std::vector<std::filesystem::path> databases;
    for (auto const& dir : outputDirs) {
        std::error_code ec;
        auto db = dir / "compile_commands.json";
        if (std::filesystem::exists(db, ec) && !ec) databases.push_back(std::move(db));
    }
    if (databases.empty()) return;
    auto result = mcpp::build::publish_root_compile_commands(
        databases, wsRoot / "compile_commands.json",
        outputDirs.front().parent_path().parent_path(), mcpp::home::root());
    if (!result) {
        mcpp::ui::warning(std::format(
            "compile_commands.json was not updated: {}", result.error().message));
    } else if (result->foreignEntries > 0) {
        mcpp::ui::warning(mcpp::build::foreign_entries_warning(result->foreignEntries));
    }
}

// Every xlings payload the entry's build read is still installed (#716). A
// cache written before the field was recorded declines once.
bool xlings_payloads_present(const BuildCacheEntry& e) {
    if (!e.xlingsPayloadsRecorded) return false;
    std::error_code ec;
    return std::ranges::all_of(e.xlingsPayloads, [&](const std::string& p) {
        return std::filesystem::is_directory(std::filesystem::path(p), ec);
    });
}

// A toolchain named by path (mcpp#755) is unchanged since the entry's build:
// each of its programs has the size and modification time prepare recorded
// beside the build. A managed toolchain never changes in place, and a build
// without the record used none.
bool local_toolchain_unchanged(const std::filesystem::path& outputDir) {
    std::ifstream in(outputDir / "local-toolchain.stamp", std::ios::binary);
    if (!in) return true;
    std::string line;
    while (std::getline(in, line)) {
        auto t1 = line.find('\t');
        auto t2 = t1 == std::string::npos ? t1 : line.find('\t', t1 + 1);
        if (t2 == std::string::npos) return false;
        const std::filesystem::path p(line.substr(t2 + 1));
        std::error_code ec;
        const auto size = std::filesystem::file_size(p, ec);
        if (ec) return false;
        const auto time = std::filesystem::last_write_time(p, ec);
        if (ec) return false;
        // `std::format`, not `std::to_string`: the clock's rep and
        // `uintmax_t` both convert to two integer overloads of the latter, and
        // libc++ calls that ambiguous.
        if (std::format("{}", static_cast<std::uint64_t>(size)) != line.substr(0, t1)
            || std::format("{}", static_cast<std::int64_t>(time.time_since_epoch().count()))
                   != line.substr(t1 + 1, t2 - t1 - 1))
            return false;
    }
    return true;
}

// Why a fast path declined, under `-v` (#734 E5). Each refusal names its
// condition, so a platform on which the fast path never serves shows which
// precondition it fails instead of only the slower build.
std::optional<int> fast_path_declined(std::string_view path, std::string_view why) {
    mcpp::log::verbose("fast-path", std::format("{} declined: {}", path, why));
    return std::nullopt;
}

// WHAT A RECORDED BUILD MUST SATISFY BEFORE ANY FAST PATH REPLAYS IT.
//
// `try_fast_build`, `try_fast_workspace_build` and `try_fast_run` each carried
// a list of declines of their own, 23, 30 and 30 of them, and a property added
// to one had to be added to the others by hand. #757 is the case in which no
// path received the property at all: the engine that wrote a graph was recorded
// nowhere and compared nowhere. The gates the three share are this one
// function, and a path keeps only what is its own: the selection a workspace
// command matches, a program to run, a runner, the run tier, and the project's
// own sources. A field added to the record is then checked in one place.
//
// The order is the cost of the question: the record itself first, then the
// files it names, then the sweeps of the trees it names, and last the snapshot
// of the artifacts, which reads the build directory.
export struct ReplayAsk {
    // What the graph must have been planned for (`request_tag`): a project's
    // features, or a workspace group's members.
    std::string request;
    // The manifest whose edit re-plans, and how a decline names it: the
    // project's, or the workspace's.
    std::filesystem::path manifest;
    std::string_view      manifestName = "mcpp.toml";
    // How a decline names the trees the recorded roots stand for: a project
    // has path dependencies, a workspace has members besides.
    std::string_view      treesName = "a path dependency's";
    // The project's own tree, for the paths that sweep one: asked with the time
    // of build.ninja, true when a source of it is newer. Empty for a path whose
    // trees are all recorded roots, which is what a workspace's members are.
    std::function<bool(std::filesystem::file_time_type)> projectSourcesNewer;
};

// What a path needs of an admitted record to go on: where the graph is, the
// ninja that drives it, and the artifacts' state before ninja runs, which is
// what tells afterwards whether an artifact was relinked.
export struct ReplayableBuild {
    std::filesystem::path outputDir;
    std::filesystem::path ninjaPath;
    std::string           ninjaProgram;
    mcpp::build::runtime_validation::ArtifactSnapshot validated;
};

export std::expected<ReplayableBuild, std::string>
admit_recorded_build(const BuildCacheEntry& e, const ReplayAsk& ask) {
    // The engine that wrote the graph is the engine that replays it (#757).
    // First, because every later question is about a graph this engine did not
    // write when this one fails.
    if (auto why = engine_declined_because(e.engine, running_engine())) return std::unexpected(*why);

    // The runtime the recorded build ran under. An entry written before the
    // immutable snapshot cannot say which environment the program needs, and
    // running it with another is worse than not using the cache: it works once
    // and then silently stops finding its runtime data (mcpp#352).
    if (!e.runtimeBinding) return std::unexpected("the recorded build predates the runtime binding");
    if (e.runtimeEnvKey.empty())
        return std::unexpected("the recorded build predates the runtime environment key"); // regenerate build.ninja once

    // P1: verify fingerprint matches the outputDir basename.
    const std::filesystem::path outputDir(e.outputDir);
    if (!e.fingerprint.empty() && outputDir.filename().string() != e.fingerprint)
        return std::unexpected("the recorded build directory is not the one for this fingerprint");

    std::error_code ec;
    const auto ninjaPath = outputDir / "build.ninja";
    if (!std::filesystem::exists(ninjaPath, ec)) return std::unexpected("build.ninja does not exist");

    // The graph names the engine it runs, and that is asked of the graph, not
    // only of the record: another engine's `--configure-only` rewrites the
    // graph and leaves the record that names this one.
    if (const auto named = mcpp::build::read_engine_binding(ninjaPath);
        !named.empty() && named != mcpp_exe_path().generic_string())
        return std::unexpected(std::format("build.ninja runs another engine ({})", named));

    // #407. Freshness is measured against the SOURCES, which says nothing
    // about what kind of graph this is. `mcpp test` and
    // `mcpp build --configure-only` write their plan -- dev-deps, test targets,
    // `default` naming the test binaries and NOT the package's target -- into
    // this same file, because the fingerprint covers neither input. Replaying
    // that for a plain build linked the tests, never linked the target, and
    // printed `Finished`; and a broken file under tests/ (never scanned here)
    // failed a plain `mcpp build` outright.
    if (!mcpp::build::is_plain_build_graph(ninjaPath))
        return std::unexpected("build.ninja was written by another mode (test, pack or a named target)");
    // What the graph was planned for (workspace design 2026-09-29 §3): the
    // features no longer name the directory, so the graph says which it has.
    if (mcpp::build::read_request(ninjaPath) != ask.request)
        return std::unexpected("build.ninja was written for another request (features or workspace members)");

    const auto ninjaTime = std::filesystem::last_write_time(ninjaPath, ec);
    if (ec) return std::unexpected("the time of build.ninja cannot be read");

    const auto runtimeTime = std::filesystem::last_write_time(
        e.runtimeBinding->subosDir / ".xlings.json", ec);
    if (ec || runtimeTime > ninjaTime)
        return std::unexpected("the runtime's .xlings.json is newer than build.ninja");

    const auto manifestTime = std::filesystem::last_write_time(ask.manifest, ec);
    if (ec || manifestTime > ninjaTime)
        return std::unexpected(std::format("{} is newer than build.ninja", ask.manifestName));

    // mcpp#225: bounded + vcs/build-dir-excluded walk (see sources_newer_than)
    // instead of a hand-rolled recursive_directory_iterator over src/.
    if (ask.projectSourcesNewer && ask.projectSourcesNewer(ninjaTime))
        return std::unexpected("a project source, build.mcpp, a build-program input or a resource script is newer than build.ninja");

    // A cache written before this field existed cannot say whether the build
    // had `path` dependencies, and answering "assume none" is the wrong half of
    // that guess: it would keep replaying a stale graph for exactly the projects
    // the field was added for. Decline once; the write below records the list
    // and every later invocation is fast again. It has to be every path: `mcpp
    // run` reaches its binary through the same check, and a `run` that skipped
    // it would execute an artifact built from a source set that no longer
    // exists.
    if (!e.depSourceRootsRecorded)
        return std::unexpected("the recorded build predates the list of path-dependency roots and their extension tables");
    if (dep_sources_newer_than(e.depSourceRoots, ninjaTime))
        return std::unexpected(std::format("{} manifest or source is newer than build.ninja", ask.treesName));
    if (!xlings_payloads_present(e)) return std::unexpected("a recorded xlings payload is missing");
    if (!local_toolchain_unchanged(e.outputDir))
        return std::unexpected("a program of the toolchain named by path changed");

    auto validated = mcpp::build::runtime_validation::validated_artifact_snapshot(
        outputDir, *e.runtimeBinding);
    if (!validated) return std::unexpected("no validated artifact snapshot is recorded for this build");

    auto ninjaProgram = e.ninjaProgram;
    // Legacy caches stored a shell-quoted path; execvp needs the raw path.
    if (ninjaProgram.size() >= 2 && ninjaProgram.front() == '\''
                                 && ninjaProgram.back() == '\'')
        ninjaProgram = ninjaProgram.substr(1, ninjaProgram.size() - 2);
    return ReplayableBuild{outputDir, ninjaPath, std::move(ninjaProgram), std::move(*validated)};
}

export std::optional<int> try_fast_build(const std::filesystem::path& projectRoot,
                                  bool verbose, bool no_cache,
                                  std::string_view currentTarget = "") {
    if (no_cache) return fast_path_declined("build", "the build cache is off (--no-cache)");

    // `--locked` MUST NOT MEET THE FAST PATH, OR IT ASSERTS NOTHING.
    //
    // The check it names lives at the resolution write point, and the fast path
    // exists precisely to skip resolution. Measured before this line existed: a
    // deliberately corrupted mcpp.lock passed `mcpp build --locked` and printed
    // "Finished" — the flag was accepted, the build was correct, and the
    // assertion never ran. A criterion that is skipped is worse than one that
    // is absent, because the green is read as a verification.
    //
    // Declining the fast path is the whole fix: `--locked` is for release
    // builds, audits and CI, none of which are the case the fast path serves.
    if (mcpp::platform::env::get("MCPP_LOCKED").value_or("") == "1")
        return fast_path_declined("build", "MCPP_LOCKED=1 asks for a locked resolution");
    // `--managed-only` is checked against the sources prepare decides, which
    // a fast path does not run (mcpp#755).
    if (auto v = mcpp::platform::env::get("MCPP_MANAGED_ONLY"); v && *v != "0")
        return fast_path_declined("build", "MCPP_MANAGED_ONLY asks for the sources to be checked");

    auto want = fast_path_identity(projectRoot);
    if (!want) return fast_path_declined("build", "the manifest or the request could not be read");

    // #496. A project with build hooks always takes the full path. The fast
    // path is defined as "skip preparation", and `build_start` is specified to
    // run AFTER it — a hook program installed as an `[xlings] deps` entry does
    // not exist until preparation has run. Declining here rather than in
    // cmd_build keeps the decision next to the manifest that answers it; the
    // full path then runs the hooks around run_build_plan.
    if (want->hooksActive) return fast_path_declined("build", "the project declares [hooks], which run on every build");

    // P3: read multi-entry cache and find the entry matching this
    // (target, profile, cache mode) triple. Matching on the target alone served
    // the wrong profile's artifacts, and ignoring the cache mode replayed a
    // cache-reading graph for a request that asked not to read the cache.
    auto entries = read_build_cache(projectRoot);
    const BuildCacheEntry* match = nullptr;
    for (auto& e : entries) {
        if (e.targetTriple == currentTarget && e.profile == want->profile
            && e.cacheMode == want->cacheMode && e.features == want->features
            && e.toolchainRecorded && e.toolchainRequest == want->toolchainRequest
            && e.selection.empty()) {
            match = &e;
            break;
        }
    }
    if (!match) return fast_path_declined("build", "no recorded build matches this request");

    // The gates every fast path shares (see admit_recorded_build); what is left
    // here is the project's own tree.
    auto admitted = admit_recorded_build(*match, {
        .request = mcpp::build::request_tag({}, want->features),
        .manifest = projectRoot / "mcpp.toml",
        .projectSourcesNewer = [&](std::filesystem::file_time_type ninjaTime) {
            return sources_newer_than(projectRoot, ninjaTime, want->resourceScripts,
                                      want->extTable);
        },
    });
    if (!admitted) return fast_path_declined("build", admitted.error());
    const auto& outputDir = admitted->outputDir;
    const auto& ninjaPath = admitted->ninjaPath;
    const auto& validatedBefore = admitted->validated;
    auto runtimeEnvKey = match->runtimeEnvKey;
    auto runtimeEnvValue = match->runtimeEnvValue;

    // All inputs are older than build.ninja → fast-path: just run ninja.
    // C1: this configuration is confirmed current, so the root database is
    // restored here rather than left to whatever a NEXT full build happens to
    // do — a project that never edits a source again would otherwise never
    // see it back.
    restore_root_compile_commands(projectRoot, outputDir);
    std::chrono::milliseconds elapsed{};
    auto rc = run_ninja_fast(admitted->ninjaProgram, outputDir, ninjaPath, verbose,
                             runtimeEnvKey, runtimeEnvValue, &elapsed);
    if (!rc) return fast_path_declined("build", "ninja reported a stale graph");
    if (*rc != 0) return rc;
    if (!mcpp::build::runtime_validation::artifact_snapshot_unchanged(
            validatedBefore))
        return fast_path_declined("build", "ninja relinked an artifact, whose closure the full path validates"); // relinked: full path reconstructs + validates closure

    // The descriptor the plan recorded (revision 3, §7.3); empty for a record
    // written before it was carried.
    mcpp::build::progress::note_sources(outputDir);
    mcpp::build::progress::finished(want->profile,
                                    mcpp::build::progress::read_descriptor(outputDir));
    return 0;
}

// The fast path of a command on a workspace (workspace design 2026-09-29
// §15): one record per configuration group of the command's selection, each
// checked the way `try_fast_build` checks a project's record, and the groups'
// graphs replayed when every one is current. A build with nothing to do then
// costs one check per group instead of a plan. The freshness sweep covers the
// trees of the members and of the `path` dependencies each record names, not
// the workspace's whole tree.
export std::optional<int> try_fast_workspace_build(
        const std::filesystem::path& wsRoot,
        const std::vector<std::vector<std::string>>& groups,
        bool verbose, bool no_cache) {
    if (no_cache) return fast_path_declined("workspace", "the build cache is off (--no-cache)");
    if (mcpp::platform::env::get("MCPP_LOCKED").value_or("") == "1")
        return fast_path_declined("workspace", "MCPP_LOCKED=1 asks for a locked resolution");
    // `--managed-only` is checked against the sources prepare decides, which
    // a fast path does not run (mcpp#755).
    if (auto v = mcpp::platform::env::get("MCPP_MANAGED_ONLY"); v && *v != "0")
        return fast_path_declined("workspace", "MCPP_MANAGED_ONLY asks for the sources to be checked");
    auto join = [](const std::vector<std::string>& v) {
        std::string out;
        for (auto const& x : v) { if (!out.empty()) out += '\x1e'; out += x; }
        return out;
    };
    std::vector<std::string> request;
    for (auto const& g : groups) request.insert(request.end(), g.begin(), g.end());
    const auto selection = join(request);
    const auto entries = read_build_cache(wsRoot);

    struct Ready {
        std::filesystem::path outputDir;
        std::string ninjaProgram, runtimeEnvKey, runtimeEnvValue;
        mcpp::build::runtime_validation::ArtifactSnapshot validated;
    };
    std::vector<Ready> ready;
    std::string profile;
    std::error_code ec;
    const auto wsToml = std::filesystem::last_write_time(wsRoot / "mcpp.toml", ec);
    if (ec) return fast_path_declined("workspace", "the workspace's mcpp.toml cannot be read");
    for (auto const& g : groups) {
        auto want = fast_path_identity(wsRoot / g.front());
        if (!want) return fast_path_declined("workspace", "a member's manifest could not be read");
        for (auto const& mp : g) {
            auto w = mp == g.front() ? want : fast_path_identity(wsRoot / mp);
            if (!w) return fast_path_declined("workspace", "a member's manifest could not be read");
            if (w->hooksActive)
                return fast_path_declined("workspace", "a member declares [hooks], which run on every build");
        }
        const auto group = join(g);
        const BuildCacheEntry* match = nullptr;
        for (auto const& e : entries)
            if (e.targetTriple.empty() && e.profile == want->profile
                && e.cacheMode == want->cacheMode && e.features.empty()
                && e.toolchainRecorded && e.toolchainRequest == want->toolchainRequest
                && e.selection == selection && e.group == group) { match = &e; break; }
        if (!match) return fast_path_declined("workspace", "no recorded build matches this selection");
        // The gates every fast path shares (see admit_recorded_build). A
        // workspace has no project tree of its own to sweep: its members are
        // recorded roots, each classified by its own package.
        auto admitted = admit_recorded_build(*match, {
            .request = mcpp::build::request_tag(group, {}),
            .manifest = wsRoot / "mcpp.toml",
            .manifestName = "the workspace's mcpp.toml",
            .treesName = "a member's or a path dependency's",
        });
        if (!admitted) return fast_path_declined("workspace", admitted.error());
        ready.push_back({admitted->outputDir, admitted->ninjaProgram, match->runtimeEnvKey,
                         match->runtimeEnvValue, std::move(admitted->validated)});
        profile = want->profile;
    }

    if (ready.size() > 1) {
        std::vector<std::filesystem::path> dirs;
        for (auto const& r : ready) dirs.push_back(r.outputDir);
        publish_workspace_compile_commands(wsRoot, dirs);
    }
    // With more than one configuration, a package's line names its own.
    mcpp::build::progress::configurations(ready.size());
    for (auto& r : ready) {
        if (ready.size() == 1) restore_root_compile_commands(wsRoot, r.outputDir);
        std::chrono::milliseconds elapsed{};
        auto rc = run_ninja_fast(r.ninjaProgram, r.outputDir, r.outputDir / "build.ninja",
                                 verbose, r.runtimeEnvKey, r.runtimeEnvValue, &elapsed);
        if (!rc) return fast_path_declined("workspace", "ninja reported a stale graph");
        if (*rc != 0) return rc;
        if (!mcpp::build::runtime_validation::artifact_snapshot_unchanged(r.validated))
            return fast_path_declined("workspace", "ninja relinked an artifact, whose closure the full path validates");
        mcpp::build::progress::note_sources(r.outputDir);
    }
    // The groups share the profile, and so its descriptor.
    mcpp::build::progress::finished(
        profile, ready.empty() ? std::string{}
                               : mcpp::build::progress::read_descriptor(ready.front().outputDir));
    return 0;
}

// THE BLANK LINE AFTER THE `Running` LINE BELONGS TO THAT LINE. It separates
// mcpp's narration from the program's output on a terminal, so it is narration
// as well: written to the stream the `Running` line was written to (standard
// error) and, like it, not under `--quiet`. It used to be a bare `println` to
// standard output, so `mcpp run -q` wrote one empty line in front of the
// program's own output, and `mcpp run -q > file` began with it (measured with
// 2026.9.30.2, `od -c`). Both streams are flushed after it, so that nothing
// mcpp wrote is still buffered when the program starts writing to the same
// terminal.
void run_separator() {
    mcpp::ui::line("");
    mcpp::ui::flush();
}

// THE PROGRAM OWNS THE TERMINAL FROM THE `Running` LINE ON, and nothing of
// mcpp runs after it: on POSIX mcpp is replaced by the program
// (process::run_foreground). The live report is closed, which restores the
// terminal's mode, and the run's closing notices are printed now, before the
// `Running` line, because after the program there is no mcpp left to print
// them.
void yield_terminal() {
    mcpp::build::progress::close();
    mcpp::ui::print_closing_notices();
}

// mcpp#225 (E2): `mcpp run`'s fast path. Mirrors try_fast_build's
// fingerprint/freshness gate against the SAME cache entry `mcpp build`
// wrote (targetTriple == "" — a HOST build; see the precondition below), then
// on a hit runs ninja and execs the cached run-target directly — skipping
// prepare_build (toolchain resolution + full modgraph scan) entirely.
// Returns nullopt when there's no usable cache entry (build_run_target
// falls back to the full prepare_build path, which also refreshes the
// cache for next time), an exit code otherwise.
std::optional<int> try_fast_run(const std::filesystem::path& projectRoot,
                                const std::optional<std::string>& targetName,
                                std::span<const std::string> passthrough) {
    // Same reason as try_fast_build's: this path skips resolution, and
    // `--locked` is an assertion about resolution.
    if (mcpp::platform::env::get("MCPP_LOCKED").value_or("") == "1")
        return fast_path_declined("run", "MCPP_LOCKED=1 asks for a locked resolution");
    // `--managed-only` is checked against the sources prepare decides, which
    // a fast path does not run (mcpp#755).
    if (auto v = mcpp::platform::env::get("MCPP_MANAGED_ONLY"); v && *v != "0")
        return fast_path_declined("run", "MCPP_MANAGED_ONLY asks for the sources to be checked");
    auto want = fast_path_identity(projectRoot);
    if (!want) return fast_path_declined("run", "the manifest or the request could not be read");

    // THE precondition of this whole function: it exec's the cached
    // artifact itself, so it is only ever valid when that artifact is for THIS
    // machine.
    //
    // The header above used to justify matching `targetTriple == ""` with
    // "`mcpp run` never takes a --target flag". The caller does guard the
    // flag — but a project can name its target in the MANIFEST instead, and
    // that spelling never reaches the cache key, so a cross build's entry is
    // written as "" and read back as if it were a host build.
    //
    // Measured on the shipped 2026.8.19.2, on the first two commands a
    // bare-metal user runs after `mcpp new`:
    //
    //     $ mcpp build && mcpp run
    //          Running `target/riscv64-none-elf/…/bin/blinky`   ← no emulator
    //     exit=1
    //
    // `mcpp run` alone was correct; only build-then-run reached the cache. So
    // the fast path is off whenever a default target is declared, and the full
    // prepare — which is what resolves the runner — takes over.
    if (!want->defaultTarget.empty()) return fast_path_declined("run", "the manifest names a default target");

    auto entries = read_build_cache(projectRoot);
    const BuildCacheEntry* match = nullptr;
    for (auto& e : entries) {
        if (e.targetTriple.empty() && e.profile == want->profile
            && e.cacheMode == want->cacheMode && e.features == want->features
            && e.toolchainRecorded && e.toolchainRequest == want->toolchainRequest
            && e.selection.empty()) {
            match = &e;
            break;
        }
    }
    if (!match || match->runTargets.empty()) return fast_path_declined("run", "no recorded build has a program to run");
    // A runner declared for the host target (a wrapper such as valgrind, or
    // a triple that is native here but carries an emulator) is consulted on
    // the prepare path through choose_runner. This path has no manifest to
    // read the template from, and executing the artifact bare here while the
    // other door wraps it would make the second `mcpp run` behave differently
    // from the first. The entry records the fact; the fast path declines.
    if (match->runnerDeclared) return fast_path_declined("run", "a runner is declared");
    // The same reasoning one axis over: this entry was written by a verb that
    // installed less than a run needs, so taking it would execute with a
    // declared tool absent. prepare_build provisions the difference.
    if (match->runTierPending) return fast_path_declined("run", "the run tier is not yet decided");

    // Locate the requested run-target before doing any filesystem freshness
    // work — an unrecognized name falls back to prepare_build, which gives
    // a proper "no binary target 'x' found" error instead of a silent miss.
    const std::pair<std::string, std::string>* chosen = nullptr;
    for (auto& rt : match->runTargets) {
        if (targetName && rt.first != *targetName) continue;
        chosen = &rt;
        if (targetName) break;
    }
    if (!chosen) return fast_path_declined("run", "the requested program is not among the recorded ones");

    // The gates every fast path shares (see admit_recorded_build), for the
    // reason `mcpp run` is among the paths that need them: it execs the
    // artifact itself, so an entry that cannot say which engine wrote it, or
    // which runtime the program needs, must not be taken. What is left here is
    // the project's own tree, as for try_fast_build; a graph that is not the
    // plain build's does not build the run target at all (#407), so ninja
    // against it would report success and then exec a stale or absent binary.
    auto admitted = admit_recorded_build(*match, {
        .request = mcpp::build::request_tag({}, want->features),
        .manifest = projectRoot / "mcpp.toml",
        .projectSourcesNewer = [&](std::filesystem::file_time_type ninjaTime) {
            return sources_newer_than(projectRoot, ninjaTime, want->resourceScripts,
                                      want->extTable);
        },
    });
    if (!admitted) return fast_path_declined("run", admitted.error());
    const auto& outputDir = admitted->outputDir;
    const auto& ninjaPath = admitted->ninjaPath;
    const auto& validatedBefore = admitted->validated;

    // Fresh → run ninja (picks up any incremental object/link work) then
    // exec the cached exe path directly. C1, same reason as try_fast_build's.
    restore_root_compile_commands(projectRoot, outputDir);
    auto rc = run_ninja_fast(admitted->ninjaProgram, outputDir, ninjaPath, /*verbose=*/false,
                             match->runtimeEnvKey, match->runtimeEnvValue);
    if (!rc) return fast_path_declined("run", "ninja reported a stale graph");
    if (*rc != 0) return kRunBuildFailed;
    if (!mcpp::build::runtime_validation::artifact_snapshot_unchanged(
            validatedBefore))
        return fast_path_declined("run", "ninja relinked an artifact, whose closure the full path validates"); // never execute an artifact not validated for this binding

    auto exe = outputDir / chosen->second;
    auto pathCtx = mcpp::fetcher::make_path_ctx(/*cfg=*/nullptr, projectRoot);
    yield_terminal();
    mcpp::ui::status("Running",
        std::format("`{}`", mcpp::ui::shorten_path(exe, pathCtx)));
    run_separator();
    std::vector<std::string> argv;
    argv.push_back(exe.string());
    for (auto& a : passthrough) argv.push_back(a);

    std::vector<std::pair<std::string, std::string>> childEnv;
    if (!match->runEnvKey.empty() && !match->runEnvValue.empty())
        childEnv.emplace_back(match->runEnvKey, match->runEnvValue);
    // ...and exactly the environment declaration snapshot used by the build.
    // Installing/changing a provider invalidates the fast path via the SubOS
    // manifest mtime check; this invocation never mixes a new run contract
    // with objects built under the old one.
    for (auto& kv : mcpp::platform::runtime::resolve_runtime_environment(
             *match->runtimeBinding,
             [](std::string_view v) -> std::optional<std::string> {
                 if (const char* e = std::getenv(std::string(v).c_str()))
                     return std::string(e);
                 return std::nullopt;
             }))
        childEnv.push_back(std::move(kv));

    // Same contract as the prepare path below: a refused spawn is reported and
    // exits in the 125-127 band, never folded into the artifact's own status.
    // No runner can be declared for an entry this path accepts (see the
    // `runnerDeclared` gate above), so the artifact is the only thing that
    // could have been refused.
    int spawnErr = 0;
    const int exitRc = mcpp::platform::process::run_foreground(argv, childEnv, &spawnErr);
    if (spawnErr != 0) {
        using namespace mcpp::build::runner_lookup;
        const auto triple = mcpp::toolchain::triple::host_triple().str();
        if (classify(spawnErr) == SpawnClass::Unloadable)
            std::println(stderr, "error: {}", unrunnable_message(triple, exe, spawnErr));
        else
            std::println(stderr, "error: {}", spawn_failed_message(exe.string(), spawnErr));
        return launcher_status(spawnErr);
    }
    return exitRc;
}

// The runner-resolution + exec tail shared by every way `build_run_target`
// can arrive at an artifact to run: the ordinary link output, or (#622 A10)
// the distributable `mcpp pack --format <name>` reported. Everything from
// here on asks only `ctx` and `exe` — which runner applies is a property of
// the project and the resolved triple, not of how the artifact was produced,
// and that is the whole point of `--format` reusing this tail rather than
// inventing a second resolution.
int run_artifact_via_runner(mcpp::build::BuildContext& ctx,
                            const std::filesystem::path& exe,
                            std::span<const std::string> passthrough,
                            bool no_runner,
                            std::string_view runner_name,
                            // Non-empty when `exe` is the distributable a
                            // `--format` pack reported; `runner_from_format`
                            // says the named runner was chosen by that name
                            // rather than typed with `--runner`.
                            std::string_view format_name = {},
                            bool runner_from_format = false) {
    auto pathCtx = mcpp::fetcher::make_path_ctx(/*cfg=*/nullptr, ctx.projectRoot);
    std::vector<std::string> argv;
    // An artifact this machine cannot execute — a freestanding image by
    // construction, a hosted cross artifact by circumstance — needs something
    // to stand in front of it. The runner template says what; mcpp never
    // guesses one, because which emulator and which machine model are facts
    // about the board or the host (see mcpp.freestanding.runner and
    // mcpp.build.runner_lookup). One read point decides for both `run` and
    // `test`: choose_runner.
    //
    // Two producers, and the precedence is the ordinary one: what the author
    // of THIS project wrote beats what a dependency supplied. The dependency
    // is the normal case on bare metal — a board-support package knows the
    // emulator, its machine model and its firmware mode, and computes the
    // absolute path that a static manifest cannot. The explicit key exists for
    // the other case: swapping `-bios default` for `-bios none -semihosting`
    // while debugging, or naming `qemu-aarch64-static` for a cross target.
    const bool isRunSlot = runner_name.empty();
    const std::string slotName{runner_name};
    const auto choice = choose_device_action(ctx, runner_name, no_runner);
    if (choice.ignored)
        mcpp::ui::info("note", std::format(
            "--no-runner: ignoring the runner declared for {}", choice.tripleKey));
    if (choice.fromManifest)
        mcpp::ui::info("note", std::format(
            "[target.{}] overrides the {} a dependency supplied",
            choice.tripleKey,
            isRunSlot ? std::string("runner")
                      : std::format("runner '{}'", slotName)));
    // THE THREE NEW SLOTS HAVE NO FALLBACK, AND `run` STILL DOES.
    //
    // An artefact with no runner on a hosted target is executed directly, and
    // that is right: the host can run it. There is no such reading of "no
    // flasher" — nothing else writes an image to a device — so an empty
    // template is an error for those three on EVERY target, not only a
    // freestanding one. Saying "nothing is configured" beats doing something
    // that was never asked for.
    if (!isRunSlot && choice.tmpl.empty()) {
        // AND THE MESSAGE LISTS WHAT THIS PROJECT DOES HAVE. A name the
        // engine does not know is usually a typo or a missing feature, and
        // "no such runner" alone leaves the reader guessing which.
        std::string have;
        for (auto const& [n, _] : ctx.manifest.buildConfig.namedRunners)
            have += (have.empty() ? "" : ", ") + n;
        std::println(stderr,
            "error: this project has no runner named '{}' for '{}'.\n"
            "       Available: {}\n"
            "       A package supplies one with `mcpp::runner(\"{}\", …)`, or a\n"
            "       project declares it:\n"
            "\n"
            "           [target.{}.runners]\n"
            "           {} = [\"<tool>\", \"<args>\", \"{{}}\"]\n"
            "\n"
            "       The artefact path is appended, or substituted for `{{}}`.",
            slotName, choice.tripleKey,
            have.empty() ? "(none — no package in this graph supplies a named runner)"
                         : have,
            slotName, choice.tripleKey, slotName);
        return 2;
    }
    if (isRunSlot && choice.freestanding && choice.tmpl.empty()) {
        std::println(stderr, "error: {}",
            mcpp::freestanding::no_runner_message(choice.tripleKey));
        return 2;
    }
    // A DISTRIBUTABLE THAT IS A DIRECTORY, AND NOTHING TO RUN IT (#634 B3).
    //
    // An application bundle is a directory. Handed to the kernel, it came back
    // as `could not be started: Permission denied (error 13)` with status 126
    // (measured on macos-15), a sentence about permissions for a request that
    // lacked a runner. The status stays 126 -- found, and not executable -- and
    // the sentence names the runner that would reach it, before any spawn.
    std::error_code dirEc;
    if (!format_name.empty() && choice.tmpl.empty()
        && std::filesystem::is_directory(exe, dirEc)) {
        std::println(stderr,
            "error: --format {} produced a directory, '{}', and no runner reaches it.\n"
            "       A directory is not executed directly: a runner named '{}' runs\n"
            "       it. A package supplies one with `mcpp::runner(\"{}\", …)`, or the\n"
            "       project declares it:\n"
            "\n"
            "           [target.{}.runners]\n"
            "           {} = [\"<tool>\", \"{{}}\"]",
            format_name, mcpp::ui::shorten_path(exe, pathCtx),
            format_name, format_name, choice.tripleKey, format_name);
        return 126;
    }
    std::optional<std::filesystem::path> runtimeFilesList;
    if (!choice.tmpl.empty()) {
        // The program is located by mcpp, not by posix_spawnp: a declared
        // payload's bin/ and then its root, then PATH — see runner_lookup for the shim
        // measurement that makes the order matter. Not found anywhere is
        // decided here, before any spawn, and is an error rather than a
        // fallback to bare execution (#544, D1): running the artifact under a
        // different interpreter with different arguments is the failure the
        // runner key exists to prevent.
        auto tmpl = choice.tmpl;
        const char* pathEnv = std::getenv("PATH");
        auto found = mcpp::build::runner_lookup::locate(
            tmpl.front(), ctx.xlingsDepBinDirs, pathEnv ? pathEnv : "");
        if (!found.program) {
            std::println(stderr, "error: {}",
                mcpp::build::runner_lookup::not_found_message(
                    choice.tripleKey, tmpl.front(), found.searched));
            return 2;
        }
        tmpl.front() = found.program->string();
        argv = mcpp::freestanding::expand(tmpl, exe);
        for (auto& a : passthrough) argv.push_back(a);
        // What the runner has to carry with the artifact. A distributable
        // holds its own files, so its list is empty and still exists.
        auto listed = write_runtime_files_list(
            ctx, exe,
            format_name.empty() ? runtime_files_for(ctx, exe)
                                : std::vector<std::pair<std::string, std::filesystem::path>>{});
        if (!listed) {
            std::println(stderr, "error: {}", listed.error());
            return 1;
        }
        runtimeFilesList = *listed;
        // The status word is the NAME the package chose, capitalised. The
        // engine has no table of verbs to look one up in, which is the point:
        // `Serve`, `Submit` and `Flash` all read correctly and none is known
        // here. A runner the format's own name selected is still `mcpp run`
        // running something, and says so.
        std::string verb = (isRunSlot || runner_from_format) ? std::string("Running")
                                                             : slotName;
        if (!isRunSlot && !runner_from_format && !verb.empty())
            verb[0] = static_cast<char>(std::toupper(verb[0]));
        yield_terminal();
        mcpp::ui::status(verb, std::format("`{} … {}`", choice.tmpl.front(),
                                           mcpp::ui::shorten_path(exe, pathCtx)));
    } else {
        argv.push_back(exe.string());
        for (auto& a : passthrough) argv.push_back(a);
        yield_terminal();
        mcpp::ui::status("Running",
            std::format("`{}`", mcpp::ui::shorten_path(exe, pathCtx)));
    }
    run_separator();

    std::vector<std::pair<std::string, std::string>> childEnv;
    auto [runEnvKey, runEnvValue] = compute_run_env(ctx.plan);
    if (!runEnvKey.empty() && !runEnvValue.empty())
        childEnv.emplace_back(runEnvKey, runEnvValue);
    // ...plus whatever the subos declares for the programs it hosts (#352).
    for (auto& kv : compute_subos_env(ctx.plan)) childEnv.push_back(std::move(kv));
    if (runtimeFilesList)
        childEnv.emplace_back(std::string(kRuntimeFilesEnv), runtimeFilesList->string());

    // Direct exec (no /bin/sh): the loader env reaches ONLY the target child,
    // never mcpp or a host shell. Fixes the bundled-glibc-vs-host-libtinfo
    // crash on newer-glibc distros.
    //
    // A refused spawn is typed and reported here, and exits in the 125-127 band
    // — never inside the range the program itself owns. With a runner the
    // failure is the runner's own (verbatim errno, no advice); without one,
    // ENOEXEC is the kernel saying this host cannot load the artifact, and the
    // message carries the key that would change that. Anything else is reported
    // as itself — EACCES is a permission problem, not an absence.
    int spawnErr = 0;
    const int rc = mcpp::platform::process::run_foreground(argv, childEnv, &spawnErr);
    if (spawnErr != 0) {
        using namespace mcpp::build::runner_lookup;
        if (!choice.tmpl.empty())
            std::println(stderr, "error: {}", spawn_failed_message(argv.front(), spawnErr));
        else if (classify(spawnErr) == SpawnClass::Unloadable)
            std::println(stderr, "error: {}",
                         unrunnable_message(choice.tripleKey, exe, spawnErr));
        else
            std::println(stderr, "error: {}", spawn_failed_message(exe.string(), spawnErr));
        return launcher_status(spawnErr);
    }
    return rc;
}

// `mcpp run` driver: build, locate the binary target, exec it with the
// resolved runtime environment. `package_filter` (`-p`/`--package`) scopes
// a workspace invocation to one member — single-member only, no
// `--workspace` fan-out (running N binaries in one invocation isn't a
// coherent "run"). Threaded straight to prepare_build's BuildOverrides,
// which already does the member switch (basename OR member path — the same
// rule mcpp::project::resolve_member_dir documents for build/test).
export int build_run_target(const std::optional<std::string>& targetName,
                            std::span<const std::string> passthrough,
                            const std::string& package_filter = {},
                            const std::string& cache_mode = {},
                            bool no_cache = false,
                            const std::string& target_triple = {},
                            bool no_runner = false,
                            // The NAME of the way to reach the artefact.
                            // Empty is the default runner — `mcpp run`. Any
                            // other value came from `--runner <name>` and the
                            // engine has never seen it before.
                            std::string_view runner_name = {},
                            // THE TWO AXES `build` AND `test` HAVE ALWAYS
                            // TAKEN, AND `run` DID NOT.
                            //
                            // Both change WHAT IS BUILT, so a `run` that could
                            // not express them could only ever execute whatever
                            // a previous `build` happened to leave behind — and
                            // there is no spelling of `mcpp run` that runs a
                            // release artefact, or one built with a feature on.
                            //
                            // It is the shape the whole device surface is built
                            // around: a board package expresses "emulator" and
                            // "hardware" as features, so `mcpp run --features
                            // hardware` is the command a developer types when
                            // the board arrives. Without this it was the one
                            // scenario the design's own example could not run.
                            const std::string& features = {},
                            const std::string& profile = {},
                            // The device axis. `--no-accel` arrives as the
                            // "(none)" sentinel, as it does for `build`.
                            const std::string& accel = {},
                            // #622 A10: reused VERBATIM from `mcpp pack
                            // --format` — same value space, same refusal
                            // naming what the resolved graph provides. Empty
                            // is the ordinary run of the link output.
                            const std::string& format = {}) {
    // --format WITH --no-runner: refused before anything is built. A
    // distributable (an `.apk`, an `.msi`, this record's own `blob` fixture)
    // is not the link output, and "run it directly, ignoring the runner" has
    // no reading for a file this host was never going to execute on its own.
    if (!format.empty() && no_runner) {
        std::println(stderr,
            "error: --format and --no-runner cannot be combined: a "
            "distributable is not something this host executes directly.");
        return 2;
    }

    // mcpp#225 (E2): reuse the resolved build cache when it's still fresh,
    // skipping prepare_build's toolchain resolution + modgraph scan
    // entirely — mirrors cmd_build's try_fast_build fast path. The cached
    // entry was written for whichever package occupied the project root
    // last time; a `-p` filter always needs prepare_build's member switch,
    // so skip the fast path in that case (mirrors cmd_build's fast-path
    // bypass whenever ov.package_filter is set).
    // A --cache/--no-cache override also bypasses the fast path, for the same
    // reason --profile does: the cached build.ninja was generated under the
    // previous mode, so reusing it would silently ignore the flag.
    // `--no-runner` bypasses it too: the fast path executes the artifact bare,
    // and it only takes an entry that records no runner (see runnerDeclared),
    // so with the flag there is nothing for it to ignore — and it has no
    // manifest to print the note against.
    if (package_filter.empty() && cache_mode.empty() && !no_cache
        && target_triple.empty() && !no_runner
        // AND NEITHER NEW AXIS IS SET. The cached entry was written for
        // whichever feature set and profile the last build used; taking it
        // here would silently ignore the flag, which is the same reason
        // `--cache` and `--profile` bypass it in `cmd_build`.
        && features.empty() && profile.empty() && accel.empty()
        // THE FAST PATH IS `run`'s, AND ONLY `run`'s.
        //
        // It exec's the cached artefact directly — that IS its definition — so
        // for `flash`, `monitor` or `debug` it would run the program on the
        // BUILD HOST and report success, having done none of what was asked.
        // Measured while writing e2e 333: `mcpp flash` printed
        // "Running target/…/bin/p".
        //
        // The guard is the slot rather than a flag, because the property that
        // makes the fast path wrong here is what the slot means.
        && runner_name.empty()
        // #622 A10: `--format` runs a DISTRIBUTABLE, produced by the pack
        // pipeline below — the cached artefact this path would exec bare is
        // the link output, which is not that file.
        && format.empty()) {
        if (auto root = mcpp::project::find_manifest_root(std::filesystem::current_path())) {
            if (auto rc = try_fast_run(*root, targetName, passthrough)) {
                return *rc;
            }
        }
    }

    // #622 A10: `mcpp run --format <name>` IS `mcpp pack --format <name>` --
    // the two prepares, the build, the staging, the provider's action --
    // followed by the ordinary run, with the artifact THAT reported as the
    // operand. Nothing here re-derives what the pack pipeline already
    // decided: whether the format is one the graph provides (refused by
    // `build_and_pack` naming what is available, exactly as `mcpp pack
    // --format bogus` is), the build, the staged tree, which action claimed
    // the request.
    if (!format.empty()) {
        mcpp::pack::Options popts;
        popts.targetTriple = target_triple;
        popts.profile      = profile;
        popts.format       = mcpp::pack::Format::Dispatched;
        popts.formatName   = format;
        // The pack passes build with the features this run was asked for; the
        // second prepare below already did, so without this the artifact and
        // the run disagreed about the graph (#641).
        popts.features     = features;
        auto outcome = mcpp::pack::build_and_pack(
            std::move(popts), /*modeFromUser=*/false, targetName.value_or(std::string{}));
        if (outcome.rc != 0) return kRunBuildFailed;
        if (outcome.artifacts.empty()) {
            // Not reached today: `build_and_pack` returns rc=0 only after
            // confirming at least one reported artifact exists on disk. Kept
            // as a named refusal rather than an assert, so a future format
            // shape that reports zero artifacts fails LOUDLY here instead of
            // dereferencing past the end of an empty vector below.
            std::println(stderr,
                "error: --format {} reported success and named no artifact to run", format);
            return 1;
        }
        // THE RUNNER RESOLUTION NEEDS A BuildContext, AND build_and_pack's OWN
        // ONE DOES NOT ESCAPE IT — it is an internal detail of a function
        // whose contract is a CLI exit code plus the paths it packed. Preparing
        // again is not a second build: the ninja graph above is already up to
        // date, so this is the same "prepare, then drive a no-op graph scan"
        // shape the plain run path below always pays once.
        mcpp::build::BuildOverrides ov2;
        ov2.package_filter = package_filter;
        ov2.cache_mode     = cache_mode;
        ov2.target_triple  = target_triple;
        ov2.features       = features;
        ov2.profile        = profile;
        ov2.accel          = accel;
        ov2.will_run       = true;
        auto ctx2 = prepare_build(/*print_fp=*/false, /*includeDevDeps=*/false,
                                  /*extraTargets=*/{}, ov2);
        if (!ctx2) { mcpp::ui::error(std::format("{}", ctx2.error())); return kRunBuildFailed; }
        if (run_build_plan(*ctx2, /*verbose=*/false, no_cache, target_triple) != 0)
            return kRunBuildFailed;
        // ONE DISTRIBUTABLE, OR A SENTENCE. The pack pipeline reports the
        // terminal artifacts of the request (outputs no other introduced
        // action consumes); a format that ends in two files has no single
        // operand a runner can take.
        if (outcome.artifacts.size() != 1) {
            std::string names;
            for (auto const& a : outcome.artifacts) {
                if (!names.empty()) names += ", ";
                names += a.string();
            }
            std::println(stderr,
                "error: --format {} produced {} distributables ({}); mcpp run needs "
                "exactly one to hand to the runner", format, outcome.artifacts.size(), names);
            return 1;
        }
        // THE NAMED RUNNER THE FORMAT'S OWN NAME SELECTS (#634 B3).
        //
        // A distributable is reached the way its format is reached, and the
        // name a package gives that way is the format's own: `dist-apple`
        // supplies `mcpp::runner("app", …)` for `--format app`. Without this,
        // `mcpp run --format app` took the DEFAULT runner -- the one a plain
        // `mcpp run` hands the link output -- and a project had to repeat the
        // format as `--runner app`. A typed `--runner` still wins, and a
        // format no runner is named after keeps the default runner.
        std::string effectiveRunner{runner_name};
        bool runnerFromFormat = false;
        if (effectiveRunner.empty()
            && !choose_device_action(*ctx2, format).tmpl.empty()) {
            effectiveRunner = format;
            runnerFromFormat = true;
        }
        return run_artifact_via_runner(*ctx2, outcome.artifacts.front(),
                                       passthrough, no_runner, effectiveRunner,
                                       format, runnerFromFormat);
    }

    // Build first. Single prepare_build → drive build → reuse ctx to locate
    // the binary, so we don't re-resolve the toolchain or re-scan modgraph.
    mcpp::build::BuildOverrides ov;
    ov.package_filter = package_filter;
    ov.cache_mode     = cache_mode;
    ov.target_triple  = target_triple;
    ov.features       = features;
    ov.profile        = profile;
    ov.accel          = accel;
    // This verb executes what it builds, so the `when = "run"` tool tier is
    // part of what has to exist. `mcpp build` does not set it, which is the
    // whole of the difference the tier buys.
    ov.will_run       = true;
    auto ctx = prepare_build(/*print_fp=*/false, /*includeDevDeps=*/false,
                             /*extraTargets=*/{}, ov);
    if (!ctx) { mcpp::ui::error(std::format("{}", ctx.error())); return kRunBuildFailed; }
    // `target_triple` IS PASSED, AND OMITTING IT WROTE A CROSS BUILD INTO
    // THE HOST'S CACHE SLOT.
    //
    // The last argument becomes the cache entry's `[target=]` key. `cmd_build`
    // supplies it at both of its call sites; this one did not, so
    // `mcpp run --target X` built X correctly and then recorded the result as a
    // HOST build. `try_fast_run` matches on `targetTriple.empty()`, so the very
    // next bare `mcpp run` took that entry and exec'd the cross artifact
    // directly — no build of the host target, and no runner:
    //
    //     $ mcpp run --target riscv64-none-elf   # correct, under qemu
    //     $ mcpp run
    //          Running `target/riscv64-none-elf/…/bin/openkal-same-source`
    //     exit=1
    //
    // The comment on `try_fast_run` records the same defect reached through the
    // MANIFEST's default target, and guards that door alone. This is the other
    // door: the flag. Measured on 2026.8.24.3, from a clean `target/`.
    if (run_build_plan(*ctx, /*verbose=*/false, no_cache, target_triple) != 0)
        return kRunBuildFailed;
    // The program run is the selected member's (workspace design 2026-09-29
    // §15), with its closure's runtime.
    focus_on_member(*ctx);

    // Find binary target
    const mcpp::build::LinkUnit* chosen = nullptr;
    for (auto& lu : ctx->plan.linkUnits) {
        if (lu.kind != mcpp::build::LinkUnit::Binary) continue;
        if (!lu.artifactOf.empty()) continue;   // mcpp#711; see compute_run_targets
        if (targetName && lu.targetName != *targetName) continue;
        chosen = &lu;
        if (targetName) break;
    }
    if (!chosen) {
        // #622 A3/A10: an `app` whose form on THIS row is a shared library
        // never becomes a `LinkUnit::Binary`, so the loop above cannot find
        // it — that is correct (there is no executable to exec), but "no
        // binary target 'myapp' found" would blame the user for a name that
        // does exist. Read the manifest directly and, when that is exactly
        // why the search came up empty, name the actual reason and the way
        // out (`--format`, §2.10) instead.
        for (auto const& t : ctx->manifest.targets) {
            if (t.kind != mcpp::manifest::Target::Application) continue;
            if (targetName && t.name != *targetName) continue;
            auto triple = mcpp::toolchain::triple::parse(ctx->tc.targetTriple);
            if (mcpp::toolchain::triple::application_form(
                    triple ? *triple : mcpp::toolchain::triple::Triple{})
                != mcpp::toolchain::triple::ApplicationForm::SharedObject)
                continue;
            std::string formats;
            for (auto const& f : ctx->plan.providedPackFormats)
                formats += (formats.empty() ? "" : ", ") + f;
            std::println(stderr,
                "error: '{}' is an application, and on {} an application is "
                "a shared library that a package installs. Run it through a "
                "distributable: mcpp run --format <name>, where <name> is "
                "one of: {}",
                t.name, ctx->tc.targetTriple,
                formats.empty() ? std::string("none declared") : formats);
            return 2;
        }
        std::println(stderr, "error: no binary target {}",
            targetName ? std::format("'{}' found", *targetName) : "in this package");
        return 2;
    }

    auto exe = ctx->outputDir / chosen->output;
    return run_artifact_via_runner(*ctx, exe, passthrough, no_runner, runner_name);
}

export enum class TestMessageFormat { Human, Json };

export struct TestOptions {
    std::string        filter;   // substring match on the path-based test name; empty = all
    TestMessageFormat  format = TestMessageFormat::Human;
    bool               list = false;   // enumerate only, no build/run
    // `--no-runner`: run the test binaries directly, ignoring a declared
    // runner — the operator on this host stating that the triple is native
    // here, a fact the manifest has no axis for (#544, D3).
    bool               noRunner = false;
    // `--no-run`: build the tests for `--target` and stop. THE CLAIM IT MAKES
    // IS NARROWER THAN A PASS, AND IT IS STATED RATHER THAN INFERRED.
    //
    // Without it, a target this host cannot execute leaves every test `NotRun`
    // and the command exits 2, which is correct: mcpp did not establish
    // whether the tests pass, and a zero there is the false reading #544
    // records. But `2` is also what a broken runner returns, so a caller that
    // wanted only the build --- a compatibility sweep measuring a target no
    // runner exists for --- cannot tell "the tests built" from "the tests
    // built and the runner is missing" and must settle for `mcpp build`, which
    // builds the package and, for a package whose only sources are under
    // `tests/`, compiles NOTHING of it at all.
    //
    // `--no-run` makes the narrower claim available as its own answer: every
    // selected test compiled and linked for the target, and none was executed.
    bool               noRun = false;
    // Per-test RUN deadline. The default is deliberately non-zero: `mcpp test`
    // is something CI runs unattended, and an unbounded default makes a single
    // hung test able to consume the whole job with nothing to show for it.
    // `--timeout 0` still means "no limit", it just has to be asked for.
    int                timeoutSecs = 300;
    // Per-ninja-invocation deadline (Phase A, the bulk pass, and each per-test
    // drive are timed separately). Covers the half `--timeout` never could:
    // a compile or link that never returns. POSIX only — see BuildOptions.
    //
    // Unlike timeoutSecs this defaults to OFF, and the asymmetry is measured,
    // not stylistic. A single test binary running longer than five minutes is
    // unusual; a cold dependency build taking longer than fifteen is ordinary —
    // one mcpp-index member (OpenCV from source) measures 1019s on Linux and
    // 1289s on Windows. A default ceiling would turn those slow-but-correct
    // builds red and blame mcpp for it. "How long may a build take" is a
    // property of the project, so the project says it; mcpp only has to make
    // saying it possible, which is what was missing.
    int                buildTimeoutSecs = 0;
};

// What one member's `run_tests` actually did. `--workspace` fans out over
// members and needs this to report per-member progress and a workspace total;
// the exit code alone cannot say how many tests ran or where the time went.
export struct TestRunSummary {
    int       passed    = 0;
    int       failed    = 0;
    // Tests that were built and not executed, with the one reason that applies
    // to all of them (#544): this host cannot load the artifacts, or the
    // declared runner could not be found or started. Not a failure — the
    // test did not run — and not a pass either: the exit code is 2.
    int         notRun  = 0;
    std::string notRunReason;
    // Built and deliberately not executed (`--no-run`). Counted apart from
    // `notRun` so the workspace total cannot add a stated build-only result to
    // a run that mcpp could not perform.
    int         built   = 0;
    // The wall time this member's tests waited for their build. Phase A + bulk
    // pass + per-test drives for a member planned alone; for a member planned
    // with others, the build of its whole group (`buildGroup`), plus its own
    // per-test drives.
    long long buildMs   = 0;
    long long runMs     = 0;   // the test binaries' own execution
    long long elapsedMs = 0;   // wall clock for the whole member
    bool      packageError = false;   // Phase A failed: no test ever ran
    // The configuration group whose one build this member's tests waited for,
    // when `mcpp test` planned several members together; -1 for a member
    // planned and built alone. Members of one group report the same
    // `buildMs`, so a consumer that sums it over members deduplicates by this
    // number.
    int       buildGroup = -1;
};

// One selected member of a `mcpp test` over several members, as the command
// layer found it: the path `[workspace] members` spells, and what discovery
// read from the member's own directory (two members may each have a
// `tests/main.cpp`).
export struct WorkspaceTestMember {
    std::string                         path;
    std::vector<mcpp::manifest::Target> targets;
    // Discovery failed: the member fails alone, with this message, and the
    // others are planned without it.
    std::string                         error;
    // The line a member with no tests reports, naming where discovery looked.
    std::string                         noTests;
};

// What the command layer reports around each member's run. `begin` is asked
// before a member's tests start, and a member it answers false for is not run
// (`--workspace-timeout`); `end` receives the member's exit status and summary.
export struct WorkspaceTestHooks {
    std::function<bool(std::size_t, const std::string&)>                  begin;
    std::function<void(std::size_t, const std::string&, int, const TestRunSummary&)> end;
};

// Minimal JSON string escaping for the --message-format json records. Same
// shape as json_escape in cmd_xpkg.cppm — kept local (15 lines) rather than
// shared across the cli/build module boundary.
static std::string test_json_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20)
                    out += std::format("\\u{:04x}", static_cast<unsigned char>(c));
                else out += c;
        }
    }
    return out;
}

// A test's result, as the records and the summary read it.
struct TestResult {
    std::string name;
    // `NotRun` (#544): built, and not executed — the host cannot load the
    // artifact, or the declared runner could not be found or started.
    // Reporting that as `RunFail (exit 127)` states that the test ran and
    // returned 127, which is false and indistinguishable from a missing
    // program; reporting it as a pass would be read as one.
    // `Built` (`--no-run`): compiled and linked, and deliberately not
    // executed. Distinct from `NotRun`, which means mcpp tried and could
    // not --- the difference is whether anything was left unanswered.
    enum class St { Pass, CompileFail, RunFail, NotRun, Built } status;
    int         exitCode = 0;
    std::string compileOutput;
    std::string runOutput;
    // The wall time of the step that decided the status: the run for a test
    // that ran, the build for a `compile_fail`. The meaning `duration_ms` has
    // always had; the test binary's own build time is `buildMs`.
    long long   durationMs = 0;
    bool        timedOut = false;  // killed by --timeout
    std::string reason;            // NotRun only: why, in one sentence
    // The build time of this test's own binary in this invocation: the sum of
    // its link edge and its main unit's compile edge in `.ninja_log`, 0 when
    // neither was rebuilt (`build_ms`, 2026.10.1.1+).
    long long   buildMs = 0;
};

// Streaming NDJSON: one record per test, emitted as it finishes — a
// consumer (e.g. the d2x provider) sees progress live, and a crash
// mid-run still leaves the completed records on stdout.
static void emit_test_json(const std::string& memberName, const TestResult& r) {
    const char* st = r.status == TestResult::St::Pass ? "pass"
                   : r.status == TestResult::St::CompileFail ? "compile_fail"
                   : r.status == TestResult::St::NotRun ? "not_run"
                   : r.status == TestResult::St::Built ? "built"
                                                        : "run_fail";
    std::string signal = (r.exitCode > 128 && r.exitCode < 128 + 65)
        ? std::to_string(r.exitCode - 128) : "null";
    std::println("{{\"member\":\"{}\",\"test\":\"{}\",\"status\":\"{}\","
                 "\"exit_code\":{},\"signal\":{},"
                 "\"duration_ms\":{},\"timed_out\":{},"
                 "\"compile_output\":\"{}\",\"run_output\":\"{}\","
                 "\"reason\":\"{}\",\"build_ms\":{}}}",
                 test_json_escape(memberName),
                 test_json_escape(r.name), st, r.exitCode, signal, r.durationMs,
                 r.timedOut ? "true" : "false",
                 test_json_escape(r.compileOutput), test_json_escape(r.runOutput),
                 test_json_escape(r.reason), r.buildMs);
    std::fflush(stdout);
}

// An edge ninja recorded in `.ninja_log`: what it made, and how long it took.
struct NinjaEdge {
    std::string output;
    long long   ms = 0;
};

// The edges ninja appended to `log` after byte `from`: the ones the drives
// since that point ran. The log accumulates across invocations, so an edge
// that was not rebuilt has an old entry that says how long it took once; the
// offset taken before the first drive is what tells this run's edges from
// those. A log that ninja rewrote (recompaction) is shorter than the offset
// and reads as no edges, which is an absent measurement and not a zero.
//
// Format (ninja log v5): `start_ms TAB end_ms TAB mtime TAB output TAB hash`.
static std::vector<NinjaEdge> ninja_edges_since(const std::filesystem::path& log,
                                                std::uintmax_t from) {
    std::vector<NinjaEdge> edges;
    std::error_code ec;
    const auto size = std::filesystem::file_size(log, ec);
    if (ec || size <= from) return edges;
    std::ifstream is(log, std::ios::binary);
    if (!is) return edges;
    is.seekg(static_cast<std::streamoff>(from));
    std::string line;
    while (std::getline(is, line)) {
        std::array<std::string_view, 5> f{};
        std::size_t n = 0, b = 0;
        std::string_view v = line;
        while (n < f.size()) {
            const auto t = v.find('\t', b);
            f[n++] = v.substr(b, t == std::string_view::npos ? std::string_view::npos : t - b);
            if (t == std::string_view::npos) break;
            b = t + 1;
        }
        if (n < 4) continue;
        long long start = 0, end = 0;
        auto [p1, e1] = std::from_chars(f[0].data(), f[0].data() + f[0].size(), start);
        auto [p2, e2] = std::from_chars(f[1].data(), f[1].data() + f[1].size(), end);
        if (e1 != std::errc{} || e2 != std::errc{} || end < start) continue;
        edges.push_back({std::string(f[3]), end - start});
    }
    return edges;
}

// A planned test build, and what the tests run against it need: the plan, the
// backend that drives it, how long planning and the build took, and how the
// test binaries are executed. One per configuration group of a `mcpp test`
// over several members; `run_tests` holds one for its one member.
struct TestBuild {
    std::optional<BuildContext>              ctx;
    std::unique_ptr<Backend>                 backend;
    long long                                prepareMs = 0;
    // Phase A, the bulk pass, and any attribution drive.
    long long                                buildMs = 0;
    bool                                     bulkBuiltEverything = false;
    std::filesystem::path                    ninjaLog;
    std::uintmax_t                           logFrom = 0;

    // How the test binaries are executed, resolved once for the build
    // (#544): every test of one build shares a target.
    RunnerChoice                             runnerChoice;
    std::vector<std::string>                 runnerTmpl;
    // Non-empty ⇒ no test is spawned; every one is reported NotRun with it.
    std::string                              invocationNotRunReason;
    // Non-zero: there is nothing to execute the tests with, and this is the
    // exit status every member of the build returns.
    int                                      runnerFatal = 0;
    // Set by the first worker whose spawn the kernel refused; every worker
    // checks it before spawning. Workers already past the check may be
    // refused the same way — harmless, a refused spawn has no side effects —
    // and each such result is NotRun, not RunFail. The reason is printed once.
    std::atomic<bool>                        hostCannotRun{false};
    std::string                              hostCannotRunReason;
};

// The "Compiling <package>" lines the tests' own lines follow.
static void test_announce(const BuildContext& ctx) {
    std::map<std::string, std::size_t> cachedUnits;
    for (auto& dep : ctx.cachedDeps) cachedUnits[dep.name] = dep.units;
    auto announce = [&](const std::string& name,
                        const mcpp::manifest::DependencySpec& spec,
                        std::string_view suffix) {
        std::string ver = spec.isPath() ? "(path)" : std::string("v") + spec.version;
        auto it = cachedUnits.find(name);
        if (it == cachedUnits.end()) {
            mcpp::ui::status("Compiling",
                std::format("{} {}{}", name, ver, suffix));
        } else {
            mcpp::ui::status("Cached",
                std::format("{} {} ({} unit{}){}", name, ver, it->second,
                            it->second == 1 ? "" : "s", suffix));
        }
    };
    std::set<std::string> announced;
    auto announce_package = [&](const mcpp::manifest::Manifest& m, std::string_view where) {
        announced.insert(m.package.name);
        mcpp::ui::status("Compiling",
            std::format("{} v{} ({})", m.package.name, m.package.version, where));
    };
    auto announce_dependencies = [&](const mcpp::manifest::Manifest& m) {
        for (auto& [name, spec] : m.dependencies) {
            if (announced.contains(name)) continue;
            announced.insert(name);
            announce(name, spec, "");
        }
        for (auto& [name, spec] : m.devDependencies) {
            if (announced.contains(name)) continue;
            announced.insert(name);
            announce(name, spec, " (dev)");
        }
    };
    // A plan of several members has a virtual root, which declares only its
    // members: the packages are the members, named as their directories.
    if (ctx.manifest.package.virtualRoot && !ctx.workspaceMembers.empty()) {
        for (auto const& wm : ctx.workspaceMembers) announce_package(wm.manifest, wm.memberPath);
        for (auto const& wm : ctx.workspaceMembers) announce_dependencies(wm.manifest);
        return;
    }
    announce_package(ctx.manifest, ".");
    announce_dependencies(ctx.manifest);
}

// Phase A goal set: every shared prerequisite — all package/dep compile
// units EXCEPT the tests' own main TUs, plus any non-test link outputs.
// In test mode the lib link unit is skipped entirely (plan.cppm), so the
// package's module objects are the only place shared breakage can show
// up; building them here is what keeps a broken src/ module a PACKAGE
// error instead of N identical per-test compile failures.
static std::vector<std::string> test_package_goals(const BuildContext& ctx) {
    std::set<std::filesystem::path> testMains;
    for (auto& lu : ctx.plan.linkUnits)
        if (lu.kind == mcpp::build::LinkUnit::TestBinary && lu.entryMain)
            testMains.insert(*lu.entryMain);
    std::vector<std::string> goals;
    for (auto& cu : ctx.plan.compileUnits)
        if (!testMains.contains(cu.source))
            goals.push_back(cu.object.generic_string());
    for (auto& lu : ctx.plan.linkUnits)
        if (lu.kind != mcpp::build::LinkUnit::TestBinary)
            goals.push_back(lu.output.generic_string());
    return goals;
}

// The part of Phase A that one member's tests need: the objects of the
// member's closure, which are the ones its test binaries link other than the
// tests' own main TUs. A member whose package does not build is found by
// building these alone, so that a group's other members still run.
static std::vector<std::string> member_package_goals(const BuildContext& ctx,
                                                    std::string_view owner) {
    std::set<std::filesystem::path> testMains;
    for (auto& lu : ctx.plan.linkUnits)
        if (lu.kind == mcpp::build::LinkUnit::TestBinary && lu.entryMain)
            testMains.insert(*lu.entryMain);
    std::set<std::filesystem::path> mainObjects;
    for (auto& cu : ctx.plan.compileUnits)
        if (testMains.contains(cu.source)) mainObjects.insert(cu.object);
    std::set<std::filesystem::path> seen;
    std::vector<std::string> goals;
    for (auto& lu : ctx.plan.linkUnits) {
        if (lu.kind != mcpp::build::LinkUnit::TestBinary || lu.memberOf != owner) continue;
        for (auto& o : lu.objects)
            if (!mainObjects.contains(o) && seen.insert(o).second)
                goals.push_back(o.generic_string());
    }
    return goals;
}

// Phase A, run against `tb` (see the note at its two callers): everything
// every test shares, built once. Nullopt when it built; else the failure,
// which the caller reports. Its wall time is added to the build's.
static std::optional<BuildError> test_phase_a(TestBuild& tb, const TestOptions& testOpts,
                                              bool json) {
    auto* ctx = &*tb.ctx;
    auto& backend = tb.backend;
    auto pkgTargets = test_package_goals(*ctx);
    if (pkgTargets.empty()) return std::nullopt;
    mcpp::build::BuildOptions aOpts;
    aOpts.ninjaTargets = pkgTargets;
    aOpts.buildTimeoutSecs = static_cast<unsigned>(testOpts.buildTimeoutSecs);
    // Phase A is the package's own build, and is reported as `mcpp build`
    // reports one (build progress design 2026-09-29); the tests' own
    // builds and runs below keep their per-test lines.
    std::optional<mcpp::build::progress::Build> phaseReport;
    if (!json && !mcpp::ui::is_quiet()) {
        phaseReport.emplace(ctx->outputDir);
        aOpts.progress = &*phaseReport;
    }
    auto tPhaseA = std::chrono::steady_clock::now();
    auto a = backend->build(ctx->plan, aOpts);
    tb.buildMs += std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - tPhaseA).count();
    if (!a) return std::move(a.error());

    // M3.2: populate BMI cache for deps that did NOT hit cache — deps
    // are package-level artifacts, so this belongs right after Phase A.
    mcpp::build::populate_dependency_cache(*ctx);

    // No "Finished test" line here: Phase A only built the shared
    // prerequisites. Printing a success banner right before per-test
    // failures read as a contradiction; the final summary carries timing.
    return std::nullopt;
}

// 6. Phase B. First a single keep-going bulk build over every selected
//    test goal — ninja parallelizes across tests and a failing test does
//    not stop the rest (-k 0). The result is deliberately ignored: the
//    per-test loop below re-drives each goal so a failure is attributed to
//    exactly one test.
//
//    ...but ONLY when this bulk build failed. A re-drive was assumed to be
//    a near no-op, and it is not: a drive re-emits build.ninja, rewrites
//    compile_commands.json, spawns ninja and re-validates the runtime
//    closure. Measured on the 83-test suite AFTER the rule E fix, that is
//    still ~39ms x 83 = 3.2s of a 5.3s hot run — spent re-asking a question
//    the bulk build just answered for every test at once.
//
//    `-k 0` means the bulk exit code is 0 IFF every selected goal built, so
//    it carries exactly the information the loop was re-deriving. When it
//    is non-zero the loop runs as before and each failure still names its
//    own test.
// `keep` says which test binaries are goals.
template <class Keep>
static void test_bulk(TestBuild& tb, const TestOptions& testOpts, Keep&& keep) {
    auto* ctx = &*tb.ctx;
    auto& backend = tb.backend;
    mcpp::build::BuildOptions bulk;
    bulk.report = mcpp::build::BuildOptions::Report::Caller;   // a test's own lines
    bulk.keepGoing = true;
    bulk.buildTimeoutSecs = static_cast<unsigned>(testOpts.buildTimeoutSecs);
    for (auto& lu : ctx->plan.linkUnits)
        if (keep(lu))
            bulk.ninjaTargets.push_back(lu.output.generic_string());
    if (!bulk.ninjaTargets.empty()) {
        auto tBulk = std::chrono::steady_clock::now();
        tb.bulkBuiltEverything = backend->build(ctx->plan, bulk).has_value();
        tb.buildMs += std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - tBulk).count();
    }
}

// How the test binaries are executed — the SAME runner `mcpp run` uses,
// resolved ONCE per build (#544). One read point, two callers; and
// one lookup, because every test of a build shares a target, so
// "the runner's program is not there" is a fact about the build and
// is reported once rather than once per test.
//
// Nothing else about the test model changes, and that is a measured
// result rather than a simplification: semihosting propagates the
// firmware's `main` return value to the emulator's exit code
// (`return 7` → qemu exits 7, verified), so "exit code is the verdict"
// holds under a runner exactly as it does on the host.
static void test_resolve_runner(TestBuild& tb, const TestOptions& testOpts, bool json) {
    auto* ctx = &*tb.ctx;
    tb.runnerChoice = choose_runner(*ctx, testOpts.noRunner);
    auto& runnerChoice = tb.runnerChoice;
    if (runnerChoice.ignored && !json)
        mcpp::ui::info("note", std::format(
            "--no-runner: ignoring the runner declared for {}", runnerChoice.tripleKey));
    if (runnerChoice.fromManifest && !json)
        mcpp::ui::info("note", std::format(
            "[target.{}].runner overrides the runner a dependency supplied",
            runnerChoice.tripleKey));
    if (runnerChoice.freestanding && runnerChoice.tmpl.empty()) {
        std::println(stderr, "error: {}",
            mcpp::freestanding::no_runner_message(runnerChoice.tripleKey));
        tb.runnerFatal = 2;
        return;
    }
    tb.runnerTmpl = runnerChoice.tmpl;
    auto& runnerTmpl = tb.runnerTmpl;
    if (!runnerTmpl.empty()) {
        const char* pathEnv = std::getenv("PATH");
        auto found = mcpp::build::runner_lookup::locate(
            runnerTmpl.front(), ctx->xlingsDepBinDirs, pathEnv ? pathEnv : "");
        if (found.program) runnerTmpl.front() = found.program->string();
        else tb.invocationNotRunReason = mcpp::build::runner_lookup::not_found_message(
            runnerChoice.tripleKey, runnerTmpl.front(), found.searched);
    }
}

// One member's tests, against a build that exists: the per-test builds that
// the bulk pass did not answer, then the runs, then the member's summary.
// `owner` is the member's package name in the plan, which picks its test
// binaries out of a plan that holds several members' (empty: every test
// binary of the plan). `carriedMs` is the time the member has already spent
// on planning and building, which `elapsed_ms` counts; `summary.buildMs`
// arrives holding the build's wall time.
static int test_run_member(TestBuild& tb, const TestOptions& testOpts,
                           std::span<const std::string> passthrough, bool json,
                           const std::string& memberName, const std::string& owner,
                           long long carriedMs, TestRunSummary& summary) {
    auto* ctx = &*tb.ctx;
    auto& backend = tb.backend;
    const auto tLoop = std::chrono::steady_clock::now();
    if (tb.runnerFatal) return tb.runnerFatal;
    const auto& runnerChoice = tb.runnerChoice;
    auto& runnerTmpl = tb.runnerTmpl;
    auto& invocationNotRunReason = tb.invocationNotRunReason;
    auto& hostCannotRun = tb.hostCannotRun;
    auto& hostCannotRunReason = tb.hostCannotRunReason;
    const bool bulkBuiltEverything = tb.bulkBuiltEverything;

    // Filter guard. The filter selects at the build/run stage ONLY — the plan
    // always contains every test, so build.ninja and compile_commands.json
    // stay complete (clangd depends on the latter; a filtered run must not
    // clobber it down to one entry).
    auto filter_match = [&](const mcpp::build::LinkUnit& lu) {
        return lu.kind == mcpp::build::LinkUnit::TestBinary
            && (owner.empty() || lu.memberOf == owner)
            && (testOpts.filter.empty()
                || lu.targetName.find(testOpts.filter) != std::string::npos);
    };
    std::vector<TestResult> results;
    auto emit_json = [&](const TestResult& r) {
        if (!json) return;
        emit_test_json(memberName, r);
    };

    // The runtime of THIS member: in a plan of several members, the plan's own
    // directories are the union over every member, and a member's tests are
    // told about its closure's alone.
    auto runtimeEnvKey = mcpp::platform::env::runtime_library_path_key();
    std::string runtimeEnvValue;
    bool hasRuntimeDirs = false;
    with_member(*ctx, owner, [&] {
        runtimeEnvValue = mcpp::platform::env::prepend_path_list(
            runtimeEnvKey, ctx->plan.runtimeLibraryDirs);
        hasRuntimeDirs = !ctx->plan.runtimeLibraryDirs.empty();
    });
    // Read once for the whole run rather than per test: it is one file, and
    // every test in a run belongs to the same subos.
    const auto subosEnv = compute_subos_env(ctx->plan);

    // macOS deliberately has no runtime-library-path key (env.cppm): injecting
    // DYLD_LIBRARY_PATH would reach every executable ninja launches and can
    // make system frameworks load a private libc++. The consequence is that a
    // test needing `[runtime] library_dirs` passes on Linux/Windows and fails
    // here with a dyld error that names neither the cause nor the platform —
    // so say it out loud rather than leaving the difference silent.
    if constexpr (mcpp::platform::is_macos) {
        if (runtimeEnvKey.empty() && hasRuntimeDirs) {
            mcpp::diag::warning("test/runtime-path",
                "macOS does not inject a runtime library path for test binaries "
                "(DYLD_LIBRARY_PATH is deliberately not set); dependencies must be "
                "reachable through the binary's rpath. A dyld 'image not found' "
                "failure below is this difference, not a broken test.");
        }
    }

    // How many test binaries run at once.
    //
    // The tests themselves were never the slow part — MEASURED on the 83-test
    // suite, the whole run phase is 1.8s against a 190s total — so this is the
    // tail, not the fix. It is still worth having: after the build-side work
    // (rule E, the per-test re-drive) the run phase is HALF of what is left.
    //
    // ONE test runs in the foreground, unbuffered. That is the debugging case:
    // a single long test streaming its progress is worth more than the ~0ms
    // concurrency would save on it, and capturing would hold that output back
    // until the test ended — including when it hangs, which is exactly when a
    // reader needs it.
    const int runJobs = [&] {
        // The machine's `[build] default_jobs` applies HERE TOO, and that is a
        // decision rather than an inheritance. A test runner at ten concurrent
        // processes has the same memory shape as a compile at ten, so someone
        // who set a machine-wide number almost certainly meant it for both;
        // `docs/04-mcpp-toml.md` says so, because one key with two
        // behaviours has to be stated. The fallback below is unchanged:
        // absent, this path uses the whole machine rather than the backend's
        // default, since there is no backend to defer to.
        int j = mcpp::build::schedule::resolve_jobs(ctx->manifest, {},
                                                    ctx->globalDefaultJobs);
        if (j <= 0) j = static_cast<int>(std::thread::hardware_concurrency());
        return j > 0 ? j : 1;
    }();

    struct Runnable {
        std::string name;
        std::vector<std::string> argv;
        std::vector<std::pair<std::string, std::string>> env;
        long long buildMs = 0;   // this test's own edges in .ninja_log
    };
    std::vector<Runnable> runnable;

    // Executes `list`, appending to `results` and emitting the per-test line.
    //
    // Output is CAPTURED whenever more than one test runs, and printed as one
    // contiguous block when that test finishes. Streaming N tests straight to
    // the terminal interleaves them line by line, which does not just look
    // untidy — it makes a failing assertion unattributable, and the whole
    // reason the per-test loop exists is attribution.

    auto run_tests_now = [&](std::vector<Runnable>& list) {
        if (list.empty()) return;
        const bool capture = json || list.size() > 1;
        const auto deadline = std::chrono::milliseconds(
            static_cast<long long>(testOpts.timeoutSecs) * 1000);
        // A PHYSICAL BOARD IS A MUTEX, AND NOTHING ELSE THIS POOL HAS EVER
        // SCHEDULED WAS ONE.
        //
        // An emulator takes N concurrent instances; a probe attached to one
        // board does not. Two `probe-rs` processes reaching for the same device
        // do not fail cleanly — they interleave, and the verdict they produce is
        // about neither test. Nothing in the argv says which case this is, so
        // the BOARD says it, once, with `mcpp:run-exclusive=1`, and the
        // project never has to remember `-j1`.
        //
        // Clamped rather than made an error: an exclusive target with one test
        // is an ordinary run, and refusing it would turn a correct
        // configuration into a failure.
        const bool exclusiveDevice = ctx->manifest.buildConfig.runExclusive
                                  && !runnerChoice.tmpl.empty();
        const int runJobsHere = exclusiveDevice ? 1 : runJobs;
        if (exclusiveDevice && runJobs > 1 && list.size() > 1) {
            mcpp::ui::info("note", std::format(
                "the target declares an exclusive device, so {} tests run one at "
                "a time", list.size()));
        }
        const int workers = capture
            ? std::min<int>(runJobsHere, static_cast<int>(list.size())) : 1;

        auto tRunPhase = std::chrono::steady_clock::now();
        std::atomic<std::size_t> next{0};
        std::mutex reportMutex;

        auto worker = [&] {
            for (;;) {
                std::size_t i = next.fetch_add(1);
                if (i >= list.size()) return;
                auto& r = list[i];

                // Stamped HERE, immediately before the exec — not when the
                // test was queued.
                //
                // Discovery, building and attribution all happen in a first
                // pass that completes before any test runs, and the workers
                // then take tests off a queue. A start time captured at queue
                // time therefore includes the whole preparation phase plus
                // however long this test waited for a worker, and `ok (2.30s)`
                // for a test that ran in 30ms is not a slow test, it is a
                // mislabelled one. The phase's own wall time is measured
                // separately by `tRunPhase` below.
                // Nothing to spawn when the invocation already knows the
                // answer: the runner is missing, or an earlier spawn was
                // refused by the kernel. Recorded as NotRun with that reason.
                if (!invocationNotRunReason.empty() || hostCannotRun.load()) {
                    std::scoped_lock lock(reportMutex);
                    if (!json) mcpp::ui::plain(std::format("{} ... not run", r.name));
                    results.push_back({r.name, TestResult::St::NotRun, 0, {}, {}, 0, false,
                                       invocationNotRunReason.empty() ? hostCannotRunReason
                                                                      : invocationNotRunReason});
                    std::fflush(stdout);
                    emit_json(results.back());
                    continue;
                }

                const auto tStart = std::chrono::steady_clock::now();
                bool timedOut = false;
                int exitCode = 0;
                int spawnErr = 0;
                std::string runOutput;
                if (capture) {
                    auto rr = mcpp::platform::process::capture_exec_deadline(
                        r.argv, r.env, deadline, &timedOut, {}, &spawnErr);
                    exitCode  = rr.exit_code;
                    runOutput = std::move(rr.output);
                } else {
                    mcpp::ui::status("Running", std::format("bin/{}", r.name));
                    exitCode = mcpp::platform::process::run_exec_deadline(
                        r.argv, r.env, deadline, &timedOut, &spawnErr);
                }
                auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - tStart).count();

                std::scoped_lock lock(reportMutex);
                if (spawnErr != 0) {
                    // Refused before it ran (#544). With a runner the failure
                    // is the runner's own; without one, ENOEXEC is the kernel
                    // saying this host cannot load the artifact. Either way
                    // it is a fact about the invocation, so it is printed once
                    // and every later test is NotRun without a spawn.
                    using namespace mcpp::build::runner_lookup;
                    std::string reason;
                    if (!runnerTmpl.empty())
                        reason = spawn_failed_message(r.argv.front(), spawnErr);
                    else if (classify(spawnErr) == SpawnClass::Unloadable)
                        reason = std::format(
                            "this host cannot execute {} artifacts: {} (error {}); "
                            "declare [target.{}].runner, or pass --no-runner on a "
                            "host that can",
                            runnerChoice.tripleKey, errno_text(spawnErr), spawnErr,
                            runnerChoice.tripleKey);
                    else
                        reason = spawn_failed_message(r.argv.front(), spawnErr);
                    if (!hostCannotRun.exchange(true)) {
                        hostCannotRunReason = reason;
                        if (!json) mcpp::ui::warning(reason);
                    }
                    if (!json) mcpp::ui::plain(std::format("{} ... not run", r.name));
                    results.push_back({r.name, TestResult::St::NotRun, 0, {}, {}, ms, false,
                                       reason, r.buildMs});
                    std::fflush(stdout);
                    emit_json(results.back());
                    continue;
                }
                if (timedOut) {
                    if (!json) mcpp::ui::plain(std::format(
                        "{} ... FAIL (timeout after {}s)", r.name, testOpts.timeoutSecs));
                    results.push_back({r.name, TestResult::St::RunFail, exitCode, {},
                                       runOutput, ms, true, {}, r.buildMs});
                } else if (exitCode == 0) {
                    if (!json) mcpp::ui::plain(std::format(
                        "{} ... ok ({:.2f}s)", r.name, static_cast<double>(ms) / 1000.0));
                    results.push_back({r.name, TestResult::St::Pass, 0, {},
                                       runOutput, ms, false, {}, r.buildMs});
                } else {
                    if (!json) mcpp::ui::plain(std::format(
                        "{} ... FAIL (exit {}, {:.2f}s)", r.name, exitCode,
                        static_cast<double>(ms) / 1000.0));
                    results.push_back({r.name, TestResult::St::RunFail, exitCode, {},
                                       runOutput, ms, false, {}, r.buildMs});
                }
                // The captured output belongs directly under its own line, or
                // it is attributable to nothing.
                if (!json && capture && !runOutput.empty()) {
                    std::fputs(runOutput.c_str(), stdout);
                    if (runOutput.back() != '\n') std::fputc('\n', stdout);
                }
                std::fflush(stdout);
                emit_json(results.back());
            }
        };

        if (workers <= 1) {
            worker();
        } else {
            std::vector<std::thread> pool;
            pool.reserve(static_cast<std::size_t>(workers));
            for (int w = 0; w < workers; ++w) pool.emplace_back(worker);
            for (auto& t : pool) t.join();
        }
        // WALL time of the phase, not the sum of the per-test durations: with
        // N running at once that sum exceeds the elapsed time and the summary
        // would report a run phase longer than the whole command.
        summary.runMs += std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - tRunPhase).count();
    };

    // The build part of a test's duration is that test binary's own edges, its
    // main TU and its link, among the ones ninja logged for this build. Read
    // once when the bulk pass built every test; after each drive otherwise,
    // since a drive logs its own.
    std::map<std::string, long long> edgeMs;
    auto read_edges = [&] {
        edgeMs.clear();
        for (auto& e : ninja_edges_since(tb.ninjaLog, tb.logFrom)) edgeMs[e.output] += e.ms;
    };
    read_edges();
    std::map<std::filesystem::path, std::filesystem::path> objectOf;
    for (auto& cu : ctx->plan.compileUnits) objectOf[cu.source] = cu.object;

    for (auto& lu : ctx->plan.linkUnits) {
        if (!filter_match(lu)) continue;

        auto tTest = std::chrono::steady_clock::now();
        auto test_ms = [&tTest] {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - tTest).count();
        };

        mcpp::ui::status("Compiling", std::format("{} (test)", lu.targetName));

        std::expected<mcpp::build::BuildResult, mcpp::build::BuildError> b{};
        if (!bulkBuiltEverything) {
            mcpp::build::BuildOptions bOpts;
            bOpts.report = mcpp::build::BuildOptions::Report::Caller;   // a test's own lines
            bOpts.ninjaTargets = {lu.output.generic_string()};
            bOpts.buildTimeoutSecs = static_cast<unsigned>(testOpts.buildTimeoutSecs);
            auto tBuild = std::chrono::steady_clock::now();
            b = backend->build(ctx->plan, bOpts);
            summary.buildMs += std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - tBuild).count();
        }
        if (!b) {
            if (!json) {
                // The test's own diagnostics, right under its FAIL line — a
                // reader fixes one test with one contiguous block of output.
                mcpp::ui::plain(std::format("{} ... FAIL ({}, {:.2f}s)",
                                            lu.targetName,
                                            b.error().timedOut
                                                ? std::format("build timeout after {}s",
                                                              testOpts.buildTimeoutSecs)
                                                : std::string{"compile"},
                                            static_cast<double>(test_ms()) / 1000.0));
                std::fflush(stdout);
                if (!b.error().diagnosticOutput.empty()) {
                    std::fputs(b.error().diagnosticOutput.c_str(), stderr);
                    if (b.error().diagnosticOutput.back() != '\n')
                        std::fputc('\n', stderr);
                    std::fflush(stderr);
                }
            }
            results.push_back({lu.targetName, TestResult::St::CompileFail, 0,
                               b.error().diagnosticOutput, {}, test_ms()});
            emit_json(results.back());
            continue;
        }

        auto exe = ctx->outputDir / lu.output;
        if (!bulkBuiltEverything) read_edges();
        long long buildMsOfTest = edgeMs[lu.output.generic_string()];
        if (lu.entryMain)
            if (auto o = objectOf.find(*lu.entryMain); o != objectOf.end())
                buildMsOfTest += edgeMs[o->second.generic_string()];

        // Through the runner resolved once above, or bare. The runner's
        // program was located already; only the artifact changes per test.
        std::vector<std::string> argv;
        if (runnerTmpl.empty()) argv.push_back(exe.string());
        else                    argv = mcpp::freestanding::expand(runnerTmpl, exe);
        for (auto& a : passthrough) argv.push_back(a);

        std::vector<std::pair<std::string, std::string>> childEnv;
        if (!runtimeEnvKey.empty() && !runtimeEnvValue.empty())
            childEnv.emplace_back(runtimeEnvKey, runtimeEnvValue);
        // ...and the subos's declared environment, same as `mcpp run` (#352).
        // A GL test that cannot find a driver fails the same way a GL program
        // does, so it must be told the same things.
        for (auto& kv : subosEnv) childEnv.push_back(kv);
        // ...and, through a runner, the files the test carries with it, as
        // `mcpp run` hands them over (see `runtime_files_for`). Written here,
        // in the single-threaded pass, one list per test program.
        if (!runnerTmpl.empty()) {
            std::vector<std::pair<std::string, std::filesystem::path>> carried;
            with_member(*ctx, owner, [&] { carried = runtime_files_for(*ctx, exe, owner); });
            if (auto listed = write_runtime_files_list(*ctx, exe, carried))
                childEnv.emplace_back(std::string(kRuntimeFilesEnv), listed->string());
            else if (invocationNotRunReason.empty())
                invocationNotRunReason = listed.error();
        }

        // Prepend the sandbox's subos/default/bin to the CHILD PATH so test
        // binaries that shell out to bootstrapped tools (patchelf, ninja) find
        // them — applied to the child only, not via a leaky shell prefix.
        if constexpr (!mcpp::platform::is_windows) {
            if (auto xpkgs = mcpp::xlings::paths::xpkgs_from_compiler(ctx->tc.binaryPath)) {
                // xpkgs is <registry>/data/xpkgs → registry = xpkgs/../..
                auto registryDir = xpkgs->parent_path().parent_path();
                auto sandboxBin  = registryDir / "subos" / "default" / "bin";
                if (std::filesystem::exists(sandboxBin)) {
                    std::array<std::filesystem::path, 1> extra{sandboxBin};
                    auto pathVal = mcpp::platform::env::prepend_path_list("PATH", extra);
                    if (!pathVal.empty()) childEnv.emplace_back("PATH", pathVal);
                }
            }
        }

        runnable.push_back({lu.targetName, std::move(argv), std::move(childEnv),
                            buildMsOfTest});
    }
    // Pass 2: run them. Concurrently unless there is exactly one — see
    // `runJobs` for why the single-test case is deliberately different.
    //
    // UNDER `--no-run` THE LIST IS THE ANSWER. Everything that reaches
    // `runnable` compiled and linked; a test that did not is already a
    // `CompileFail` in `results` and keeps that status, so this path reports
    // what was built without also reporting anything about what it does.
    if (testOpts.noRun) {
        for (auto& r : runnable)
            results.push_back({r.name, TestResult::St::Built, 0, {}, {}, 0,
                               false, {}, r.buildMs});
    } else {
        run_tests_now(runnable);
    }
    summary.elapsedMs = carriedMs + std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - tLoop).count();

    // 7. Summary.
    int passed = 0;
    int failed = 0;
    int notRun = 0;
    std::string notRunReason;
    std::vector<std::string> failures;
    int built = 0;
    for (auto& r : results) {
        if (r.status == TestResult::St::Pass) ++passed;
        else if (r.status == TestResult::St::Built) ++built;
        else if (r.status == TestResult::St::NotRun) {
            ++notRun;
            if (notRunReason.empty()) notRunReason = r.reason;
        }
        else { ++failed; failures.push_back(r.name); }
    }

    summary.passed = passed;
    summary.failed = failed;
    summary.notRun = notRun;
    summary.notRunReason = notRunReason;
    summary.built  = built;

    // "build X + run Y" rather than one merged number: on a member whose tests
    // are cheap but whose link is not, those two are three orders of magnitude
    // apart, and only the split says which one to go look at.
    auto timing = std::format("{:.2f}s (build {:.2f}s + run {:.2f}s)",
                              static_cast<double>(summary.elapsedMs) / 1000.0,
                              static_cast<double>(summary.buildMs)   / 1000.0,
                              static_cast<double>(summary.runMs)     / 1000.0);

    // 1 keeps meaning "a test ran and failed". 2 is "could not establish the
    // answer": the code the freestanding no-runner path already returns for
    // the same situation, and never 0 — a green exit with N tests not run is
    // the reading this repository has recorded as its most frequent false
    // pass (#544, D2).
    const int rc = failed ? 1 : (notRun ? 2 : 0);

    if (json) {
        // `build_group` names the configuration group whose one build this
        // member's tests waited for, when it was planned with others; its
        // `build_ms` is then the group's (docs/50 §8).
        const auto group = summary.buildGroup >= 0
            ? std::format(",\"build_group\":{}", summary.buildGroup) : std::string{};
        std::println("{{\"summary\":{{\"member\":\"{}\",\"passed\":{},\"failed\":{},"
                     "\"not_run\":{},\"not_run_reason\":\"{}\","
                     "\"built\":{},"
                     "\"elapsed_ms\":{},\"build_ms\":{},\"run_ms\":{}{}}}}}",
                     test_json_escape(memberName), passed, failed,
                     notRun, test_json_escape(notRunReason), built,
                     summary.elapsedMs, summary.buildMs, summary.runMs, group);
        std::fflush(stdout);
        return rc;
    }

    // The count is in the summary line at the same weight as failures, with
    // its reason: a quiet skip is read as a pass. First line of the reason
    // only — the full text was printed when it was established.
    auto counts = std::format("{} passed; {} failed", passed, failed);
    if (built) counts += std::format("; {} built, not run", built);
    if (notRun) {
        auto firstLine = notRunReason.substr(0, notRunReason.find('\n'));
        counts += std::format("; {} not run ({})", notRun, firstLine);
    }

    std::println("");
    if (rc == 0) {
        mcpp::ui::result("test result",
            std::format("ok. {}; finished in {}", counts, timing));
        return 0;
    }
    mcpp::ui::error(std::format(
        "test result: {}. {}; finished in {}",
        failed ? "FAILED" : "NOT RUN", counts, timing));
    if (failed) {
        std::println("");
        std::println("failures:");
        for (auto& n : failures) std::println("    {}", n);
        // (Each compile failure's diagnostics already printed inline under its
        // FAIL line in Phase B — the summary stays a compact name list.)
    }
    return rc;
}

// `mcpp test` driver: discover tests/**/*.cpp, synthesize targets, build
// with dev-deps, run each test binary, summarize.
export int run_tests(std::span<const std::string> passthrough,
                     BuildOverrides overrides = {},
                     TestOptions testOpts = {},
                     TestRunSummary* summaryOut = nullptr) {
    const bool json = (testOpts.format == TestMessageFormat::Json);
    // The member this call is scoped to (empty outside a workspace). Threaded
    // into every JSON record so a `--workspace` stream can be attributed: a
    // bare test name is ambiguous the moment two members both have a `smoke`.
    const std::string memberName = overrides.package_filter;
    TestRunSummary summary;
    struct SummaryWriter {
        TestRunSummary* out; const TestRunSummary* src;
        ~SummaryWriter() { if (out) *out = *src; }
    } summaryWriter{summaryOut, &summary};
    // Wall clock for the WHOLE member, started before Phase A. The old `t0`
    // sat after Phase A and the bulk pass, so `finished in` reported only the
    // per-test loop: measured on one member, 6.53s printed against 93.5s
    // actual — a 14x understatement, and worst exactly on the build-heavy
    // members where the number matters.
    auto tMember = std::chrono::steady_clock::now();
    auto member_ms = [&tMember] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - tMember).count();
    };
    // JSON mode: stdout carries NDJSON only. All ui::status/info lines print
    // to stdout, so silence them wholesale; errors already go to stderr.
    if (json) mcpp::ui::set_quiet(true);
    // The report covers the planning and the package's own build (Phase A);
    // it is closed before the tests' own lines.
    mcpp::build::progress::open(mcpp::log::is_verbose());

    auto root = mcpp::project::find_manifest_root(std::filesystem::current_path());
    if (!root) {
        mcpp::ui::error("no mcpp.toml found in current directory or any parent");
        return 2;
    }

    auto discovered = mcpp::build::discover_test_targets(
        *root, overrides.package_filter);
    if (!discovered) {
        mcpp::ui::error(discovered.error());
        return 2;
    }
    auto testRoot = discovered->packageRoot;
    auto testTargets = std::move(discovered->targets);
    if (testTargets.empty()) {
        // Names where it looked when the manifest chose the place, so that a
        // glob that matches nothing is not read as a project without tests.
        if (discovered->discoverDeclared) {
            std::string globs;
            for (auto const& g : discovered->discover)
                globs += std::format("{}\"{}\"", globs.empty() ? "" : ", ", g);
            std::println("no tests found ([test] discover = [{}])", globs);
        } else {
            std::println("no tests found in tests/");
        }
        return 0;
    }
    // --list: enumerate (filtered) tests and stop — no toolchain resolution,
    // no build. Names/paths come straight from discovery, so this also works
    // on tests that do not currently compile.
    if (testOpts.list) {
        std::size_t total = 0;
        for (auto& t : testTargets) {
            if (!testOpts.filter.empty()
                && t.name.find(testOpts.filter) == std::string::npos) continue;
            ++total;
            auto abs = std::filesystem::absolute(testRoot / t.main)
                           .lexically_normal().generic_string();
            if (json)
                std::println("{{\"member\":\"{}\",\"test\":\"{}\",\"main\":\"{}\"}}",
                             test_json_escape(memberName),
                             test_json_escape(t.name), test_json_escape(abs));
            else
                std::println("{}", t.name);
        }
        if (json) {
            std::println("{{\"summary\":{{\"total\":{}}}}}", total);
            std::fflush(stdout);
        }
        return 0;
    }
    // 3. prepare_build with dev-deps enabled + synthetic targets.
    // A test binary is executed, so the run tier applies here exactly as it
    // does to `mcpp run` — `[xlings.workspace]` has no separate `test` tier
    // because there is no separate need.
    overrides.will_run = true;
    auto prepared = prepare_build(/*print_fp=*/false,
                                  /*includeDevDeps=*/true,
                                  std::move(testTargets),
                                  std::move(overrides));
    if (!prepared) { mcpp::ui::error(prepared.error()); return 2; }
    TestBuild tb;
    tb.ctx.emplace(std::move(*prepared));
    tb.backend = mcpp::build::make_ninja_backend();
    tb.ninjaLog = tb.ctx->outputDir / ".ninja_log";
    {
        std::error_code ec;
        tb.logFrom = std::filesystem::file_size(tb.ninjaLog, ec);
        if (ec) tb.logFrom = 0;
    }
    auto* ctx = &*tb.ctx;

    // Filter guard. The filter selects at the build/run stage ONLY — the plan
    // above always contains every test, so build.ninja and
    // compile_commands.json stay complete (clangd depends on the latter; a
    // filtered run must not clobber it down to one entry).
    auto filter_match = [&](const mcpp::build::LinkUnit& lu) {
        return lu.kind == mcpp::build::LinkUnit::TestBinary
            && (testOpts.filter.empty()
                || lu.targetName.find(testOpts.filter) != std::string::npos);
    };
    if (!testOpts.filter.empty()) {
        bool any = false;
        for (auto& lu : ctx->plan.linkUnits)
            if (filter_match(lu)) { any = true; break; }
        if (!any) {
            if (json)
                std::println("{{\"error\":\"no-tests-matched\",\"filter\":\"{}\"}}",
                             test_json_escape(testOpts.filter));
            mcpp::ui::error(std::format("no tests match '{}'", testOpts.filter));
            return 2;
        }
    }

    // 4. "Compiling test_X (test)" lines for the test binaries.
    test_announce(*ctx);
    // List test binaries.
    // (Per-test "Compiling" lines print in Phase B, interleaved with each
    // test's own result — announcing them all up front separated the three
    // pieces of one test's story across the whole output.)

    // 5. Two-phase build. Phase A: package-level artifacts (everything that
    //    is not a test binary — libs, deps). A failure here is the PACKAGE's
    //    fault, not any single test's: report it as a build error, never as
    //    N red tests. Phase B (below): each test is built as its own ninja
    //    goal, so a compile failure is attributed to exactly that test and
    //    the rest still build and run.
    mcpp::build::progress::programs_done();
    if (auto a = test_phase_a(tb, testOpts, json)) {
        summary.packageError = true;
        summary.buildMs = tb.buildMs;
        summary.elapsedMs = member_ms();
        std::fflush(stdout);
        if (json)
            std::println("{{\"error\":\"package\",\"compile_output\":\"{}\"}}",
                         test_json_escape(a->diagnosticOutput));
        // Surface the compiler/linker stderr (parity with run_build_plan) —
        // otherwise `mcpp test` failures show only "build failed" with no
        // diagnostic, which is undebuggable (notably on CI). A failed step
        // was reported when it failed.
        if (!a->reported) mcpp::ui::error(a->message);
        mcpp::ui::block(a->diagnosticOutput);
        return 1;
    }
    // The tests' own lines follow, as they always have.
    mcpp::build::progress::close();

    test_bulk(tb, testOpts, filter_match);
    test_resolve_runner(tb, testOpts, json);
    summary.buildMs = tb.buildMs;
    return test_run_member(tb, testOpts, passthrough, json, memberName, /*owner=*/"",
                           member_ms(), summary);
}

// `mcpp test` over several members: the members are planned once per
// configuration group, each group's Phase A and test goals are built once, and
// then each member's tests run in member order, continuing past a failing
// member (member-selection design 2026-09-30, S4 and D1).
//
// `groups` holds the selected members by configuration, as `mcpp build` groups
// them, and `members` the same members in `[workspace] members` order with
// what discovery found in each. A member with no tests, or whose discovery
// failed, is not planned, as it never was: it reports its own result when its
// turn comes. A group that fails to plan is planned again member by member,
// so a member that fails to plan fails alone, and the members that plan are
// planned together again without it.
export void run_workspace_tests(std::span<const std::string> passthrough,
                                const BuildOverrides& base,
                                const TestOptions& testOpts,
                                const std::filesystem::path& wsRoot,
                                const std::vector<std::vector<std::string>>& groups,
                                std::vector<WorkspaceTestMember> members,
                                const WorkspaceTestHooks& hooks) {
    const bool json = (testOpts.format == TestMessageFormat::Json);
    if (json) mcpp::ui::set_quiet(true);
    // The report covers the planning and the packages' own builds (Phase A);
    // it is closed before the tests' own lines.
    mcpp::build::progress::open(mcpp::log::is_verbose());

    // What each member has to say when its turn comes, decided before any
    // build: a member whose tests cannot be planned says why, and one with
    // nothing to run says so.
    struct Slot {
        int         session = -1;     // the build that holds the member's tests
        std::string owner;            // the member's package name in that plan
        std::string error;            // it failed before its tests could run
        std::string note;             // it has no tests
        bool        noMatch = false;  // no test matches the filter
        bool        packageFailed = false;
        std::string packageOutput;    // the diagnostics of its package's build
        // Whether `packageOutput` was already printed, as the group's Phase A
        // failure; a member's own failure that differs from it is printed at
        // the member's turn, so a second broken member is not reported bare.
        bool        packageOutputShown = false;
    };
    std::vector<Slot> slots(members.size());
    std::map<std::string, std::size_t> indexOf;
    std::vector<bool> planned(members.size(), false);
    for (std::size_t i = 0; i < members.size(); ++i) {
        indexOf[members[i].path] = i;
        if (!members[i].error.empty()) { slots[i].error = members[i].error; continue; }
        if (members[i].targets.empty()) { slots[i].note = members[i].noTests; continue; }
        if (!testOpts.filter.empty()
            && std::ranges::none_of(members[i].targets, [&](auto const& t) {
                   return t.name.find(testOpts.filter) != std::string::npos; })) {
            slots[i].noMatch = true;
            continue;
        }
        planned[i] = true;
    }

    std::vector<std::unique_ptr<TestBuild>> sessions;
    std::vector<std::vector<std::size_t>>   sessionMembers;

    // One plan of the given members, with each member's tests.
    auto plan_session = [&](const std::vector<std::size_t>& who)
            -> std::expected<std::unique_ptr<TestBuild>, std::string> {
        BuildOverrides mo = base;
        mo.package_filter.clear();
        mo.project_root = wsRoot;
        mo.will_run = true;
        mo.workspace_members.clear();
        mo.workspace_request.clear();
        mo.member_targets.clear();
        for (auto i : who) {
            mo.workspace_members.push_back(members[i].path);
            mo.member_targets[members[i].path] = members[i].targets;
        }
        for (auto const& m : members) mo.workspace_request.push_back(m.path);
        const auto t0 = std::chrono::steady_clock::now();
        auto prepared = prepare_build(/*print_fp=*/false, /*includeDevDeps=*/true,
                                      /*extraTargets=*/{}, std::move(mo));
        if (!prepared) return std::unexpected(prepared.error());
        auto tb = std::make_unique<TestBuild>();
        tb->prepareMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        tb->ctx.emplace(std::move(*prepared));
        tb->backend = mcpp::build::make_ninja_backend();
        tb->ninjaLog = tb->ctx->outputDir / ".ninja_log";
        std::error_code ec;
        tb->logFrom = std::filesystem::file_size(tb->ninjaLog, ec);
        if (ec) tb->logFrom = 0;
        return tb;
    };
    auto attach = [&](std::unique_ptr<TestBuild> tb, const std::vector<std::size_t>& who) {
        mcpp::build::progress::programs_done();
        const int id = static_cast<int>(sessions.size());
        for (auto i : who) {
            slots[i].session = id;
            for (auto const& wm : tb->ctx->workspaceMembers)
                if (wm.memberPath == members[i].path) slots[i].owner = wm.name;
            // An empty owner selects every test binary of the plan, which is
            // another member's as well: a member the plan does not name runs
            // nothing.
            if (slots[i].owner.empty()) {
                slots[i].error = std::format(
                    "member '{}' is not among the members its plan holds", members[i].path);
                slots[i].session = -1;
            }
        }
        sessionMembers.push_back(who);
        sessions.push_back(std::move(tb));
    };

    for (auto const& g : groups) {
        std::vector<std::size_t> who;
        for (auto const& mp : g)
            if (auto it = indexOf.find(mp); it != indexOf.end() && planned[it->second])
                who.push_back(it->second);
        if (who.empty()) continue;
        auto tb = plan_session(who);
        if (tb) { attach(std::move(*tb), who); continue; }
        if (who.size() == 1) { slots[who.front()].error = tb.error(); continue; }
        // The group did not plan, and the message does not say which member is
        // to blame. Each member is planned alone to find out: the ones that
        // plan are planned together again without the others, and the ones
        // that do not are reported with their own reason.
        std::vector<std::size_t> survivors;
        std::vector<std::unique_ptr<TestBuild>> alone;
        for (auto i : who) {
            auto one = plan_session({i});
            if (one) { survivors.push_back(i); alone.push_back(std::move(*one)); }
            else slots[i].error = one.error();
        }
        if (survivors.size() > 1) {
            if (auto together = plan_session(survivors)) {
                attach(std::move(*together), survivors);
                continue;
            }
        }
        for (std::size_t k = 0; k < survivors.size(); ++k)
            attach(std::move(alone[k]), {survivors[k]});
    }
    // The groups' compile databases are published once, as the union, below:
    // a group's own build must not publish the root's as if it were the only
    // one (`mcpp build` does the same).
    if (sessions.size() > 1)
        for (auto& s : sessions) s->ctx->plan.publishRootCompileDb = false;

    // Build every group: Phase A once, then one keep-going pass over the test
    // goals of the members whose package built.
    for (std::size_t s = 0; s < sessions.size(); ++s) {
        auto& tb = *sessions[s];
        const auto& who = sessionMembers[s];
        test_announce(*tb.ctx);
        if (auto a = test_phase_a(tb, testOpts, json)) {
            // A failure of the package level is the package's fault, never N
            // red tests. A group's Phase A stops at its first failure, and the
            // failure says nothing of which member it belongs to: each
            // member's own part of it is built alone, so that a member whose
            // package builds still runs, and the one whose package does not is
            // reported as failed, alone.
            if (!json) {
                if (!a->reported) mcpp::ui::error(a->message);
                mcpp::ui::block(a->diagnosticOutput);
            }
            if (who.size() > 1) {
                for (auto i : who) {
                    if (slots[i].session < 0) continue;
                    const auto goals = member_package_goals(*tb.ctx, slots[i].owner);
                    if (goals.empty()) continue;
                    mcpp::build::BuildOptions own;
                    own.report = mcpp::build::BuildOptions::Report::Caller;   // a test's own lines
                    own.ninjaTargets = goals;
                    own.buildTimeoutSecs = static_cast<unsigned>(testOpts.buildTimeoutSecs);
                    const auto t0 = std::chrono::steady_clock::now();
                    auto r = tb.backend->build(tb.ctx->plan, own);
                    tb.buildMs += std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count();
                    if (!r) {
                        slots[i].packageFailed = true;
                        slots[i].packageOutput = r.error().diagnosticOutput;
                        slots[i].packageOutputShown =
                            r.error().diagnosticOutput == a->diagnosticOutput;
                    }
                }
            }
            // Nothing pointed at a member (a failure outside every member's
            // own objects), or the group is one member: they fail together.
            if (std::ranges::none_of(who, [&](auto i) { return slots[i].packageFailed; }))
                for (auto i : who) {
                    slots[i].packageFailed = true;
                    slots[i].packageOutput = a->diagnosticOutput;
                    slots[i].packageOutputShown = true;
                }
        }
        // The test goals of the members that can run.
        std::set<std::string> runnable;
        for (auto i : who)
            if (!slots[i].packageFailed && slots[i].session >= 0)
                runnable.insert(slots[i].owner);
        test_bulk(tb, testOpts, [&](const mcpp::build::LinkUnit& lu) {
            return lu.kind == mcpp::build::LinkUnit::TestBinary
                && runnable.contains(lu.memberOf)
                && (testOpts.filter.empty()
                    || lu.targetName.find(testOpts.filter) != std::string::npos);
        });
        if (!runnable.empty()) test_resolve_runner(tb, testOpts, json);
    }
    // The tests' own lines follow, as they always have.
    mcpp::build::progress::close();

    if (sessions.size() > 1) {
        std::vector<std::filesystem::path> dirs;
        for (auto& s : sessions) dirs.push_back(s->ctx->outputDir);
        publish_workspace_compile_commands(wsRoot, dirs);
    }

    // One record or line per group, before its first member's tests: the
    // members it built and the wall time of the build (member-selection design
    // D6). The time is the group's and not any member's, so it is stated once;
    // each member's summary names the group it waited for.
    for (std::size_t s = 0; s < sessions.size(); ++s) {
        auto& tb = *sessions[s];
        const auto& who = sessionMembers[s];
        if (json) {
            std::string list;
            for (auto i : who) {
                if (!list.empty()) list += ',';
                list += std::format("\"{}\"", test_json_escape(members[i].path));
            }
            std::println("{{\"group_build\":{{\"group\":{},\"members\":[{}],\"build_ms\":{}}}}}",
                         s, list, tb.buildMs);
            std::fflush(stdout);
            continue;
        }
        std::string names;
        for (auto i : who) names += (names.empty() ? "" : ", ") + members[i].path;
        // The edges that took the build its time, for the question the per
        // member split used to answer: which member's link, and not its tests,
        // is slow.
        auto edges = ninja_edges_since(tb.ninjaLog, tb.logFrom);
        std::ranges::sort(edges, [](auto const& a, auto const& b) { return a.ms > b.ms; });
        std::string slowest;
        for (std::size_t k = 0; k < edges.size() && k < 3; ++k) {
            if (edges[k].ms < 1000) break;
            slowest += std::format("{}{} {:.1f}s", slowest.empty() ? "" : ", ",
                                   edges[k].output, static_cast<double>(edges[k].ms) / 1000.0);
        }
        mcpp::ui::status("Workspace", std::format(
            "{}built {} {} in {:.2f}s{}",
            sessions.size() > 1 ? std::format("group {}/{} ", s + 1, sessions.size())
                                : std::string{},
            who.size() == 1 ? "member" : "members", names,
            static_cast<double>(tb.buildMs) / 1000.0,
            slowest.empty() ? std::string{} : std::format("; slowest: {}", slowest)));
    }

    for (std::size_t i = 0; i < members.size(); ++i) {
        auto& m = members[i];
        auto& slot = slots[i];
        if (hooks.begin && !hooks.begin(i, m.path)) continue;
        TestRunSummary sum;
        int rc = 0;
        if (!slot.error.empty()) {
            mcpp::ui::error(slot.error);
            rc = 2;
        } else if (slot.noMatch) {
            if (json)
                std::println("{{\"error\":\"no-tests-matched\",\"filter\":\"{}\"}}",
                             test_json_escape(testOpts.filter));
            mcpp::ui::error(std::format("no tests match '{}'", testOpts.filter));
            rc = 2;
        } else if (!slot.note.empty()) {
            if (!json) std::println("{}", slot.note);
        } else if (slot.session >= 0) {
            auto& tb = *sessions[static_cast<std::size_t>(slot.session)];
            sum.buildGroup = slot.session;
            sum.buildMs = tb.buildMs;
            if (slot.packageFailed) {
                sum.packageError = true;
                sum.elapsedMs = tb.prepareMs + tb.buildMs;
                if (json)
                    std::println("{{\"error\":\"package\",\"member\":\"{}\",\"compile_output\":\"{}\"}}",
                                 test_json_escape(m.path), test_json_escape(slot.packageOutput));
                mcpp::ui::error(std::format(
                    "member '{}': its package did not build, so its tests did not run", m.path));
                if (!json && !slot.packageOutputShown) mcpp::ui::block(slot.packageOutput);
                rc = 1;
            } else {
                rc = test_run_member(tb, testOpts, passthrough, json, m.path, slot.owner,
                                     tb.prepareMs + tb.buildMs, sum);
            }
        }
        if (hooks.end) hooks.end(i, m.path, rc, sum);
    }
}

// `mcpp clean` driver.
export int clean_project(bool wipe_bmi) {
    auto root = mcpp::project::find_manifest_root(std::filesystem::current_path());
    if (!root) { std::println(stderr, "error: not in an mcpp package"); return 2; }
    std::error_code ec;
    std::filesystem::remove_all(*root / "target", ec);
    if (ec) {
        std::println(stderr, "error: cannot remove target/: {}", ec.message());
        return 1;
    }
    mcpp::ui::line(std::format("Cleaned: {}", (*root / "target").string()));

    if (wipe_bmi) {
        auto cache = mcpp::toolchain::default_cache_root();
        std::filesystem::remove_all(cache, ec);
        mcpp::ui::line(std::format("Cleaned build cache: {}", cache.string()));
        mcpp::ui::line("  (`mcpp cache clean --legacy` also removes the unused "
                       "pre-v1 cache, if any)");
    }
    return 0;
}

// `mcpp clean --stale` driver (#565).
//
// Every build lands in target/<triple>/<fingerprint>/, and a changed
// fingerprint opens a fresh directory while the old one is never touched
// again. "Current" is what target/.build_cache records: one entry per
// (target, profile) built recently, which is the set the fast paths and
// `mcpp run` still resolve to. Every other directory under a recorded
// target/<triple>/ is a leftover from a configuration that no longer exists
// and can go without forcing a rebuild of anything that does.
//
// Three deliberate limits, each erring toward deleting a directory that a
// rebuild can recreate rather than one that cannot:
//   * Entries are matched by (triple directory, fingerprint), not by the
//     absolute path the record stores, so a moved checkout is not read as
//     "everything is stale".
//   * Only triple directories named by the record are visited. Anything else
//     under target/ (`dist/` from `mcpp pack`, whatever a later release adds)
//     is not a fingerprint directory and is left alone — as is a triple whose
//     entry has been evicted from the record; that one stays until it is
//     built again. Eviction *within* a live triple is not covered by that,
//     though: the record is an 8-entry LRU (`kBuildCacheMaxEntries`), so a
//     project built across more (target, profile) pairs than that loses its
//     oldest entries while their triple stays current through a sibling. Such
//     a directory reads as unrecorded, and the age guard below is what decides
//     it — which is the intended answer, one rebuild of a configuration that
//     has not been built inside the window.
//   * The record keys on (target, profile) while the fingerprint also folds in
//     features; a `--no-cache` build writes no entry at all; and `mcpp test`
//     builds through run_tests, which never writes one. So an unrecorded
//     directory is not proof of staleness, and recomputing fingerprints here
//     is not an option (prepare_build resolves dependencies and may reach the
//     network; a clean command must not). The guard that costs nothing and
//     matches how `cache prune` already thinks: an unrecorded directory
//     written within `--older-than` (default one day) is kept — that is the
//     build somebody just ran. Older and unrecorded goes; the cost of being
//     wrong there is one rebuild of a configuration nobody has touched since.
//
// With no record at all there is nothing to compare against, and the command
// refuses rather than guess.
//
// A WORKSPACE builds at its root (workspace design 2026-09-29 §15), so the
// command acts on the workspace's `target/` from anywhere inside it, and it
// also removes the build directories each member held under its own
// `target/` before that: none of them is read by a workspace build. A
// member's `target/.build-mcpp/`, where its build program's outputs are, is
// kept.
export int clean_stale(bool dryRun, std::int64_t keepWithinSecs) {
    namespace fs = std::filesystem;
    auto root = mcpp::project::find_manifest_root(fs::current_path());
    if (!root) { std::println(stderr, "error: not in an mcpp package"); return 2; }
    std::vector<fs::path> memberDirs;
    {
        auto m = mcpp::manifest::load(*root / "mcpp.toml", {.insideWorkspace = true});
        if (m && !m->workspace.present) {
            if (auto ws = mcpp::project::find_workspace_root(*root); !ws.empty()) {
                root = ws;
                m = mcpp::manifest::load(*root / "mcpp.toml");
            }
        }
        if (m && m->workspace.present)
            for (auto const& mp : m->workspace.members) memberDirs.push_back(*root / mp);
    }
    const fs::path target = *root / "target";

    // Triple directory -> the fingerprints recorded under it. One container
    // answers both questions the walk asks: whether to descend into a triple at
    // all, and whether a directory inside it is current.
    std::map<std::string, std::set<std::string>> current;
    for (const auto& e : read_build_cache(*root)) {
        const fs::path out(e.outputDir);
        const std::string triple = out.parent_path().filename().string();
        if (triple.empty()) continue;
        // BOTH NAMES WHEN THEY DISAGREE. try_fast_build refuses an entry whose
        // `fingerprint` is not its outputDir's basename (its P1 check). Here the
        // safe reading of the same disagreement is to protect whichever
        // directory either name points at: only one of them can exist, and the
        // other costs nothing to name.
        for (const std::string& fp : {e.fingerprint, out.filename().string()})
            if (!fp.empty()) current[triple].insert(fp);
    }
    if (current.empty()) {
        std::println(stderr, "error: {} has no build record, so nothing is known to be current; "
                             "run `mcpp build` once, then retry",
                     (*root / kBuildCacheFile).string());
        return 2;
    }

    std::uintmax_t bytes = 0;
    std::size_t removed = 0, failed = 0;
    std::error_code ec;
    for (fs::directory_iterator tripleIt(target, ec), end; tripleIt != end; tripleIt.increment(ec)) {
        std::error_code tec;
        const std::string triple = tripleIt->path().filename().string();
        const auto recorded = current.find(triple);
        if (!tripleIt->is_directory(tec) || tec || recorded == current.end()) continue;
        std::error_code iec;
        for (fs::directory_iterator fpIt(tripleIt->path(), iec), fend; fpIt != fend; fpIt.increment(iec)) {
            std::error_code fec;
            if (!fpIt->is_directory(fec) || fec) continue;
            const fs::path dir = fpIt->path();
            const std::string fp = dir.filename().string();
            if (recorded->second.contains(fp)) continue;
            const std::string shown = std::format("target/{}/{}", triple, fp);
            // build.ninja is rewritten by every build; the directory's own
            // mtime only moves when an entry is added or removed.
            const auto stamp = dir / "build.ninja";
            const auto written = fs::last_write_time(fs::exists(stamp, fec) ? stamp : dir, fec);
            if (!fec) {
                const auto age = std::chrono::duration_cast<std::chrono::seconds>(
                    fs::file_time_type::clock::now() - written).count();
                if (age < keepWithinSecs) {
                    const std::string ago = age < 3600 ? std::format("{}m", std::max<std::int64_t>(age / 60, 1))
                                          : age < 86400 ? std::format("{}h", age / 3600)
                                                        : std::format("{}d", age / 86400);
                    std::println("kept    {}  (not in the record, but written {} ago; see --older-than)", shown, ago);
                    continue;
                }
            }
            const auto size = mcpp::bmi_cache::dir_size(dir);
            if (dryRun) {
                std::println("would remove {}  ({})", shown, mcpp::bmi_cache::human_bytes(size));
            } else {
                std::error_code rec;
                fs::remove_all(dir, rec);
                if (rec) {
                    std::println(stderr, "error: cannot remove {}: {}", shown, rec.message());
                    ++failed;
                    continue;
                }
                std::println("removed {}  ({})", shown, mcpp::bmi_cache::human_bytes(size));
            }
            bytes += size;
            ++removed;
        }
        if (iec) {
            std::println(stderr, "error: cannot read {}: {}", tripleIt->path().string(), iec.message());
            ++failed;
        }
    }
    if (ec) {
        std::println(stderr, "error: cannot read {}: {}", target.string(), ec.message());
        return 1;
    }

    // The members' own build directories from before the workspace was the
    // unit of build: `<member>/target/<triple>/<16 hex digits>/`.
    auto isFingerprint = [](const std::string& name) {
        return name.size() == 16 && std::ranges::all_of(name, [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        });
    };
    for (auto const& memberDir : memberDirs) {
        std::error_code mec;
        const auto memberTarget = memberDir / "target";
        if (!fs::is_directory(memberTarget, mec)) continue;
        for (fs::directory_iterator tripleIt(memberTarget, mec), end; tripleIt != end;
             tripleIt.increment(mec)) {
            std::error_code tec;
            if (!tripleIt->is_directory(tec) || tec) continue;
            if (tripleIt->path().filename().string().starts_with(".")) continue;
            for (fs::directory_iterator fpIt(tripleIt->path(), tec), fend; fpIt != fend;
                 fpIt.increment(tec)) {
                std::error_code fec;
                if (!fpIt->is_directory(fec) || fec) continue;
                if (!isFingerprint(fpIt->path().filename().string())) continue;
                const auto dir = fpIt->path();
                const std::string shown = dir.lexically_relative(*root).generic_string();
                const auto size = mcpp::bmi_cache::dir_size(dir);
                if (dryRun) {
                    std::println("would remove {}  ({}; the workspace builds in {})", shown,
                                 mcpp::bmi_cache::human_bytes(size), target.string());
                } else {
                    std::error_code rec;
                    fs::remove_all(dir, rec);
                    if (rec) {
                        std::println(stderr, "error: cannot remove {}: {}", shown, rec.message());
                        ++failed;
                        continue;
                    }
                    std::println("removed {}  ({}; the workspace builds in {})", shown,
                                 mcpp::bmi_cache::human_bytes(size), target.string());
                }
                bytes += size;
                ++removed;
            }
        }
    }

    if (removed == 0 && failed == 0) {
        std::println("Nothing stale under {}: every fingerprint directory is recorded as current",
                     target.string());
    } else {
        std::println("{} {} director{} ({})", dryRun ? "Would remove" : "Removed", removed,
                     removed == 1 ? "y" : "ies", mcpp::bmi_cache::human_bytes(bytes));
    }
    return failed ? 1 : 0;
}

} // namespace mcpp::build
