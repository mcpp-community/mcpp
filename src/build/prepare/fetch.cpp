// fetch.cpp -- what prepare_build reaches outside the tree: git remotes and
// cached clones, network steps retried, the `[xlings]` addresses a verb needs,
// installed and recorded, and the index cause a resolution failure carries.
// Declared in `:state`, or exported from prepare.cppm for the unit tests.

module;
#include <cstdio>
#include <cstdlib>

module mcpp.build.prepare;
import :state;

import mcpp.build.prepare_inputs;

import std;
import mcpp.build.refusal;
import mcpp.build.version_floor;
import mcpp.home;
import mcpp.platform.axis;
import mcpp.libs.json;
import mcpp.log;
import mcpp.manifest;
import mcpp.source_kind;
// For `resolve_version_match` / `list_installed_versions`: a bare compiler
// family named by the dependency graph resolves to a concrete version through
// exactly the path `mcpp toolchain default <family>` uses.
import mcpp.build.plan;
import mcpp.build.flags;          // compute_flags — the per-role contracts (#418)
import mcpp.build.graph_shape;  // #407: the graph says which mode wrote it
import mcpp.build.build_program;
import mcpp.build.backend;      // BuildOptions for the tool sub-build
import mcpp.build.ninja;        // make_ninja_backend — driving that sub-build
import mcpp.config;
import mcpp.provider;
import mcpp.provider.select;
import mcpp.platform;
import mcpp.fetcher;
import mcpp.fetcher.progress;
import mcpp.pm.resolver;
import mcpp.pm.index_spec;
import mcpp.pm.index_contract;
import mcpp.pm.index_route;
import mcpp.pm.index_refresh;
import mcpp.pm.mangle;
import mcpp.pm.dep_spec;
import mcpp.pm.dependency_selector;
import mcpp.pm.lock_io;
import mcpp.ui;
import mcpp.log;

namespace mcpp::build {

// Is this git remote reachable without a network round-trip?
//
// `--offline` means "never touch the network" (docs/04-mcpp-toml.md), and its
// standing promise is that anything already on disk still builds. A remote that
// names a local directory — or a file:// URL — is served by plain filesystem
// reads, so refusing it would break that promise without buying any isolation.
// The dependency-download gate further down draws the same line.
//
// Recognising a scheme (`https://`, `ssh://`, `git://`) or scp-like syntax
// (`git@host:path`) as remote first keeps a Windows drive letter (`C:\repo`,
// which contains a colon but no `@`) on the local side.
bool is_local_git_remote(std::string_view url) {
    if (url.starts_with("file://"))                     return true;
    if (url.contains("://"))                            return false;
    if (url.contains('@') && url.contains(':'))         return false;
    std::error_code ec;
    return std::filesystem::exists(std::filesystem::path(url), ec);
}

// The commit a cached clone is actually parked on, or "" if it cannot be read.
//
// Used to detect a clone that was interrupted between `git clone` and
// `git checkout` — the directory exists and looks like a repository, but sits
// on the wrong commit. Only meaningful when the expected revision is a sha,
// i.e. for branch deps after resolution.
//
// stderr is folded in so a git warning cannot leak to the user's terminal;
// the last line is taken so such a warning cannot corrupt the sha either.
std::string git_cache_head(const std::filesystem::path& gitRoot) {
    auto r = mcpp::platform::process::capture(std::format(
        "git -C {} rev-parse HEAD 2>&1",
        mcpp::platform::shell::quote(gitRoot.string())));
    if (r.exit_code != 0) return {};
    std::string out = r.output;
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r'
                            || out.back() == ' '  || out.back() == '\t'))
        out.pop_back();
    if (auto nl = out.find_last_of("\r\n"); nl != std::string::npos)
        out.erase(0, nl + 1);
    return out;
}

// The target-facing answers a `build.mcpp` may ask the engine for.
//
// ONE function because there are TWO call sites — the root project and each
// dependency — and four values derived independently in two places is the
// shape this codebase keeps paying for. A board package that got the right
// answer as a root project and a stale one as a dependency would fail only in
// the consuming build, which is the harder direction to debug.
// A NETWORK STEP OF A BUILD, RETRIED — AND IT HAD NO RETRY AT ALL.
//
// A dependency resolved by `git` is fetched on every machine that has not
// cached it, and a transport that hiccups once failed the whole build:
//
//     error: git clone of 'https://github.com/…' failed:
//     Cloning into '/home/runner/.mcpp/git/63269d80b47f71e6'...
//
// — no message from git, which is what a connection that dies mid-transfer
// looks like. Measured twice on 2026-08-23: once in continuous integration and
// once locally as `TLS connect error: … unexpected eof while reading`.
//
// THREE ATTEMPTS, AND THE LAST FAILURE IS REPORTED UNCHANGED. A wrong URL
// and a missing branch fail exactly as a transient fault does, so this cannot
// tell them apart and does not try: a permanent failure costs three seconds and
// produces the message it always did. Hiding a real error behind a retry is the
// worse trade, which is why the count is small and the report is untouched.
//
// BOTH NETWORK STEPS, not one. The first version retried only the clone —
// and a probe with a nonexistent repository failed in ONE second, because the
// step that runs first is `git ls-remote` and it was still bare. A retry on
// half of a path is a retry that reports success at having been added.
//
// `between` runs after a failed attempt: the clone needs the partial directory
// removed, or git's next attempt fails with "already exists and is not an empty
// directory" — a second, different error that says nothing about the first.
mcpp::platform::process::RunResult run_with_network_retry(
        std::string_view command,
        const std::function<void()>& between,
        std::string_view progressLabel) {
    mcpp::platform::process::RunResult r{};
    mcpp::platform::env::note_network_access();   // the envelope's `effects` (#648 A4)
    // A clone runs for as long as the repository takes to arrive, and its
    // output was captured whole, so a large one showed nothing until it
    // finished. With a label, git's download phase is drawn with the renderer
    // every other acquisition uses (W11), each redraw read as it happens; the
    // output is still kept whole for the failure message. A clone that writes
    // nothing for fifteen minutes is stopped as stalled.
    auto run_once = [&]() -> mcpp::platform::process::RunResult {
        if (progressLabel.empty()) return mcpp::platform::process::capture(command);
        mcpp::platform::process::RunResult out;
        std::optional<mcpp::ui::ProgressBar> bar;
        bool timedOut = false;
        out.exit_code = mcpp::platform::process::run_streaming_bounded(command,
            [&](std::string_view line) {
                out.output.append(line).push_back('\n');
                auto g = mcpp::fetcher::parse_git_progress(line);
                if (!g || g->phase != "Receiving objects") return;
                if (!bar) bar.emplace("Fetching", progressLabel);
                bar->update(g->percent);
            },
            std::chrono::milliseconds{0}, std::chrono::minutes{15}, &timedOut,
            /*split_on_cr=*/true);
        if (bar) {
            if (out.exit_code == 0 && !timedOut) bar->finish();
            else                                 bar->finish_failed(progressLabel);
        }
        if (timedOut && out.exit_code == 0) out.exit_code = 124;
        return out;
    };
    for (int attempt = 1; attempt <= 3; ++attempt) {
        r = run_once();
        if (r.exit_code == 0) return r;
        if (between) between();
        if (attempt < 3)
            std::this_thread::sleep_for(std::chrono::seconds(attempt));
    }
    return r;
}

std::vector<std::string>
applicable_xlings_addresses(const mcpp::manifest::Manifest& man,
                            const std::vector<std::string>& activeFeatures,
                            ToolPurpose purpose, bool isRoot) {
    using W = mcpp::manifest::ToolWhen;
    std::vector<std::string> out;
    auto wanted = [&](const std::string& address) {
        switch (man.xlings.when_of(address)) {
            case W::Always: return true;
            case W::Build:  return true;
            case W::Run:    return purpose == ToolPurpose::Run;
            case W::Dev:    return isRoot;
        }
        return true;
    };
    auto add = [&](const std::string& address) {
        if (!wanted(address)) return;
        if (std::ranges::find(out, address) == out.end()) out.push_back(address);
    };
    for (auto const& a : man.xlings.deps) add(a);
    // `[feature-xlings.<f>]` contributes only while `<f>` is active. A consumer
    // that never asks for `hardware` never downloads a probe driver — which is
    // the same mechanism `[feature-deps]` gives a package, applied to tools.
    for (auto const& f : activeFeatures)
        if (auto it = man.xlings.featureDeps.find(f);
            it != man.xlings.featureDeps.end())
            for (auto const& a : it->second) add(a);
    return out;
}

// Install a set of `[xlings.workspace]` addresses, and record that the list was
// done.
//
// EXTRACTED SO THE DEPENDENCY GRAPH CAN USE THE SAME PATH. This was the
// root project's provisioning, inline and reachable only from there. A
// board-support package that declares the emulator its machine needs is
// precisely the thing that should say so once, and a consumer that has to
// repeat the declaration to get it installed is the duplication such a package
// exists to remove — so the graph pass calls this with what the dependencies
// declared, under the same stamp discipline and the same auto-install gate.
//
// `label` names the caller in every message, because "which of the two passes
// is this" is the first thing a reader of the failure needs.
std::expected<void, std::string>
provision_xlings_addresses(const mcpp::config::GlobalConfig& cfg,
                           const std::vector<std::string>& declaredDeps,
                           const std::filesystem::path& legacyStampRoot,
                           std::string_view label) {
    if (declaredDeps.empty()) return {};
            // THE STAMP RECORDS A GLOBAL EFFECT, SO IT LIVES WHERE THE
            // EFFECT DOES. It used to sit in `<project>/.mcpp/`, while the
            // installation goes to the registry a few lines below — the
            // scope difference is deliberate and explained there. Two
            // consequences followed from the mismatch: wiping or replacing
            // `MCPP_HOME` left a project still claiming the packages were
            // installed, and `mcpp clean` (which removes `target/` and
            // never `.mcpp/`) could not clear it. Keyed by the LIST, not by
            // the project, because the installation is shared: two projects
            // declaring the same packages should pay for it once.
            //
            // A STAMP IS NOT A PRESENCE CHECK (#716). It records that the
            // list was installed once; a payload removed since -- `xlings
            // remove`, a pruned cache, a deleted directory -- left the stamp
            // claiming it, the build skipped provisioning and succeeded with
            // `xpkg_dir` answering "". So the stamp counts only while every
            // address still resolves to a payload, answered by the same lookup
            // `xpkg_dir` uses. That costs one record read or directory scan per
            // address, and it was blocked until the lookup could answer an
            // unpinned or two-segment address the way xlings resolved it.
            const auto stampDir = mcpp::home::root() / "provisioned";
            // `std::uint64_t`, not `std::size_t`: the offset basis below is
            // a 64-bit constant and a 32-bit host would truncate it, giving
            // that host a different key space for no reason anyone could
            // see. A collision is not a correctness problem either way —
            // the file stores the LIST and the comparison below is against
            // its content, so two lists sharing a key re-provision rather
            // than silently adopt each other's record.
            auto stamp_key = [&] {
                std::uint64_t h = 1469598103934665603ull;   // FNV-1a
                for (auto const& d : declaredDeps)
                    for (unsigned char ch : d + "\n")
                        { h ^= ch; h *= 1099511628211ull; }
                return std::format("xlings-deps-{:016x}", h);
            };
            const auto stamp = stampDir / stamp_key();
            // Idempotence by CONTENT, not by existence: editing the list
            // has to re-provision, and an unchanged list must not pay for
            // an xlings round-trip on every build.
            auto join_deps = [&](std::string_view sep) {
                std::string out;
                for (auto const& d : declaredDeps) {
                    if (!out.empty()) out += sep;
                    out += d;
                }
                return out;
            };
            std::string want;
            for (auto const& d : declaredDeps) { want += d; want += '\n'; }
            std::string have;
            if (std::ifstream in{stamp}; in)
                have.assign(std::istreambuf_iterator<char>(in), {});
            // The stamp the previous location left behind. Read, never
            // deleted: an older mcpp sharing the checkout still uses it,
            // and a stale extra file is cheaper than a downgrade that
            // re-provisions on every build.
            //
            // IT DOES NOT MEAN "PROVISIONED SUCCESSFULLY". The release
            // that wrote it did not read the result — that is the defect
            // above — so it means only "this list was attempted". Treating
            // it as proof would carry the bug across the very upgrade that
            // fixes it: a project whose dependency never installed would
            // adopt the stamp and stay silently broken.
            //
            // So it is consulted in ONE place, below, where the alternative
            // is worse: the auto-install gate. Online, nothing is adopted
            // and every project re-provisions once, which is a cheap round
            // trip that re-validates the claim.
            const auto legacyStamp = legacyStampRoot / ".mcpp" / ".xlings-deps.stamp";
            auto legacy_stamp_matches = [&] {
                std::string legacy;
                if (std::ifstream in{legacyStamp}; in)
                    legacy.assign(std::istreambuf_iterator<char>(in), {});
                return legacy == want;
            };
            const auto xlEnv = mcpp::config::make_xlings_env(cfg);
            std::vector<std::string> missing;
            if (have == want)
                for (auto const& d : declaredDeps)
                    if (!mcpp::provider::payload(*mcpp::provider::open(xlEnv), mcpp::provider::parse_ref(d)))
                        missing.push_back(d);
            if (!missing.empty()) {
                std::string list;
                for (auto const& m : missing) list += (list.empty() ? "" : ", ") + m;
                mcpp::log::verbose("xlings", std::format(
                    "{}: recorded as provisioned in {}, but no payload is "
                    "installed for: {}", label, stamp.string(), list));
                if (mcpp::platform::env::offline_mode()
                    || mcpp::platform::env::no_auto_install()) {
                    std::string_view release =
                        mcpp::platform::env::offline_mode()
                        ? "drop --offline / unset MCPP_OFFLINE"
                        : "unset MCPP_NO_AUTO_INSTALL";
                    refusal::record(refusal::Code::OfflineDownloadRequired);
                    return std::unexpected(std::format(
                        "{} are recorded as provisioned, but these payloads "
                        "are not installed: {}\n"
                        "       record: {}\n"
                        "       install them yourself with:\n"
                        "         xlings install {}\n"
                        "       or {} to let mcpp do it.",
                        label, list, stamp.string(), join_deps(" "), release));
                }
            }
            bool needProvision = (have != want) || !missing.empty();
            if (needProvision && missing.empty()) {
                // THE AUTO-INSTALL GATE, WHICH THIS PATH DID NOT HAVE.
                //
                // `[toolchain]` is the precedent this whole mechanism cites
                // ("the same 'declare it and mcpp provisions it on first
                // use' contract"), and that path refuses on either knob and
                // names the one that fired — see the auto-install branch
                // above. This one honoured neither, so a CI exporting
                // MCPP_NO_AUTO_INSTALL specifically to prevent an unasked
                // download got one anyway, from a path that had never heard
                // of the variable.
                //
                // Placed inside `have != want`, so it gates the ATTEMPT and
                // not the block: a project whose packages are already
                // provisioned still builds offline, which is the behaviour
                // that would otherwise regress.
                if (mcpp::platform::env::offline_mode()
                    || mcpp::platform::env::no_auto_install()) {
                    // THE ONE PLACE THE LEGACY STAMP IS TRUSTED, and the
                    // reason is that relocating a record must not refuse a
                    // build that worked yesterday. Every project that had
                    // already provisioned carries the old stamp and no new
                    // one, so on the first build after upgrading it reads
                    // as un-provisioned — and here, with the network shut
                    // off, there is no way to find out otherwise. Refusing
                    // would be a regression caused entirely by moving a
                    // file, which is the least defensible kind.
                    //
                    // Proceeding is the pre-upgrade behaviour exactly: if
                    // the packages really are missing, the build fails
                    // downstream on a missing header, as it did before.
                    // The registry stamp is NOT written — nothing here
                    // verified anything.
                    if (!legacy_stamp_matches()) {
                        std::string_view release =
                            mcpp::platform::env::offline_mode()
                            ? "drop --offline / unset MCPP_OFFLINE"
                            : "unset MCPP_NO_AUTO_INSTALL";
                        refusal::record(refusal::Code::OfflineDownloadRequired);
                        return std::unexpected(std::format(
                            "{} are declared but not provisioned, "
                            "and auto-install is off.\n"
                            "       declared: {}\n"
                            "       install them yourself with:\n"
                            "         xlings install {}\n"
                            "       or {} to let mcpp do it.",
                            label, join_deps(", "), join_deps(" "), release));
                    }
                    mcpp::log::verbose("xlings",
                        std::format("{}: auto-install is off and this project "
                        "carries a pre-2026.9.1.1 provisioning stamp for the "
                        "same list; proceeding without re-checking", label));
                    // Deliberately NOT writing the registry stamp: nothing
                    // here verified anything, and a record of a check that
                    // did not happen is the defect this release removes.
                    needProvision = false;
                }
            }
            if (needProvision) {
                mcpp::ui::status("Provisioning",
                    std::format("{} ({})", label, join_deps(", ")));
                // An address whose index a `[index.repos.<name>]` table
                // redirects is installed from that source, and says so: an
                // installation from a branch checkout must not read as one
                // from the published index (#634, C4).
                std::set<std::string> redirected;
                for (auto const& d : declaredDeps) {
                    const auto colon = d.find(':');
                    if (colon == std::string::npos) continue;
                    const auto index = d.substr(0, colon);
                    for (auto const& r : cfg.indexRepos) {
                        if (!r.fromConfig || r.name != index) continue;
                        if (r.name == "mcpplibs" && r.url == mcpp::config::kMcpplibsIndexUrl)
                            continue;
                        if (redirected.insert(index).second)
                            mcpp::ui::status("Index", std::format(
                                "{} -> {} ([index.repos.{}] in config.toml)",
                                r.name, r.url, r.name));
                    }
                }
                // GLOBAL scope, and the scope is the whole point.
                //
                // The obvious alternative -- `install_packages` against
                // `make_project_xlings_env` -- installs at PROJECT scope,
                // and that measurably does not work: on a fresh MCPP_HOME
                // the headers land in
                // `<proj>/.mcpp/.xlings/subos/_/usr/include` while
                // `--sysroot` names `<MCPP_HOME>/registry/subos/default`,
                // so `#include <gbm.h>` still failed with the dependency
                // installed and declared. Two SubOS views, and the payload
                // in the one the compiler does not read.
                //
                // `make_xlings_env` is the GLOBAL env, so this lands in the
                // registry whose SubOS *is* mcpp's sysroot -- the same
                // place `[toolchain]` has always installed into. A project
                // dependency and a toolchain dependency now agree on where
                // they live, which is the only arrangement in which one
                // `--sysroot` can see both.
                //
                // `install_packages` rather than `resolve_xpkg_path`: the
                // latter requires `<name>@<version>` and rejects a bare
                // `mesa`, while a manifest is entitled to name a package
                // without pinning it. install_packages resolves the version
                // itself and reports an ambiguous name with its candidates,
                // which is the error the author can act on.
                // Built with the JSON library rather than by formatting
                // the strings in. `deps` is manifest input, so a name
                // containing a quote or a backslash would otherwise emit
                // malformed JSON and the failure would surface as an
                // unrelated xlings parse error naming neither the manifest
                // nor the key.
                nlohmann::json args;
                args["targets"] = declaredDeps;
                args["yes"]     = true;

                mcpp::fetcher::InstallProgressHandler progress;
                auto r = mcpp::provider::open(xlEnv)->call("install_packages", args.dump(), &progress);
                // `if (!r)` IS NOT THE FAILURE TEST, AND TESTING ONLY
                // IT MADE THIS PATH REPORT SUCCESS FOR EVERY FAILURE XLINGS
                // CAN REPORT.
                //
                // `xlings::call` returns `expected<CallResult, string>` and
                // is in the VALUE state whenever the child ran at all — the
                // error channel means "the call did not happen". A
                // capability's own status arrives inside `CallResult`,
                // parsed off the NDJSON `{"kind":"result","exitCode":N}`
                // line, because the xlings process itself exits 0 by design
                // once it has spoken the protocol.
                //
                // Measured before this fix: a manifest declaring a package
                // that cannot exist printed `Provisioning [xlings] deps
                // (…)`, xlings answered `E_NOT_FOUND` with `exitCode: 1`,
                // and mcpp stamped it as done and reported a successful
                // build. #531 was written because "the declaration looked
                // accepted and did nothing" is the worst shape a config key
                // can have; unread, its own fix reproduced that shape and
                // the stamp made it permanent.
                //
                // The correct idiom is not new — the dependency install
                // path in this same file reads `r->exitCode` — it was
                // simply not applied here.
                const bool called   = r.has_value();
                const int  childRc  = called ? r->exitCode : -1;
                if (!called || childRc != 0) {
                    // Prefer xlings' own message: for an unresolvable name
                    // it names the repos it searched and whether the index
                    // is current, which is the part the author can act on.
                    std::string why = !called ? r.error()
                        : (r->error ? r->error->message
                                    : std::format("xlings exited {}", childRc));
                    if (auto captured = progress.captured_error();
                        !captured.empty() && called && !r->error)
                        why = captured;
                    // The hint is where "run `xlings update` if the package
                    // was just published" lives, and for the commonest
                    // failure — a name that is not in the synced index —
                    // it is the whole of the actionable content.
                    if (called && r->error && !r->error->hint.empty())
                        why += "\n       " + r->error->hint;
                    // xlings' own diagnostic, with its hint, IS the report
                    // (design 2026-10-26 D17). This used to end with "you can
                    // install them manually with: xlings install …" -- the
                    // same install, which meets the same refusal, so the one
                    // line of advice was the one that could not help. An
                    // ambiguous bare name ("mesa" matching two repos) lands
                    // here, and xlings' own message names the candidates.
                    return std::unexpected(std::format(
                        "provisioning {} failed: {}\n"
                        "       the whole failure is in ~/.mcpp/log/mcpp.log; `mcpp self doctor` "
                        "reports the state of mcpp's package home",
                        label, why));
                }
                // What xlings resolved each address to, where it says so
                // (protocol 1.1). Recorded per address so every later lookup
                // -- the root's, a member's, a dependency's build program --
                // gets xlings' answer rather than a re-derivation of it.
                for (auto const& e : r->dataEvents)
                    if (e.dataKind == "install_targets")
                        mcpp::provider::open(xlEnv)->remember(e);
                // Written only on success, for the same reason the check
                // above exists: a stamp is a record that the effect
                // happened, and recording an effect that did not is worse
                // than not recording it — the next build skips the attempt.
                std::error_code sec;
                std::filesystem::create_directories(stamp.parent_path(), sec);
                if (std::ofstream out{stamp}; out) out << want;
            }
    return {};
}

std::string with_index_cause(std::string msg) {
    if (auto hint = mcpp::pm::unusable_index_hint(); !hint.empty())
        msg += "\n" + hint;
    return msg;
}

} // namespace mcpp::build
