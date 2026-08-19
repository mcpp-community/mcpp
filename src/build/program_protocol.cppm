// mcpp.build.program_protocol — the CONTRACT between mcpp and a `build.mcpp`.
//
// WHY THIS IS ITS OWN MODULE
//
// `mcpp.build.directives` answers "what is a directive" — one row per wire
// name, driving parse / serialize / apply / declared-output / private-fold.
// That is a big table and it grows every time a directive is added.
//
// The three things in this file are a different kind of fact: they are the
// terms both sides agree on BEFORE any directive is exchanged.
//
//   * which wire version is spoken,
//   * when previously written cache entries stop meaning what they said,
//   * how long the program may run.
//
// They have separate consumers — `mcpp.build.hostprogram` stamps the protocol
// version into the bundled `mcpp` module and needs nothing else from the
// directive table; `mcpp.build.build_program` needs the run bound. Splitting
// them out keeps those two from importing the table, and keeps this file
// small enough that changing a protocol term is visibly a protocol change.
//
// Imports `std` and NOTHING else, deliberately: a protocol term that needed
// the manifest, the toolchain or the filesystem to be stated would not be a
// protocol term.
//
// See .agents/docs/2026-08-05-build-mcpp-extensibility-architecture.md §4 and
// .agents/docs/2026-08-11-source-kind-table-and-build-program-timeout.md §4.

export module mcpp.build.program_protocol;

import std;

export namespace mcpp::build::program_protocol {

// ── Protocol version ───────────────────────────────────────────────────────
//
// The wire version this engine speaks. The bundled `mcpp` module announces the
// version it was built against (`mcpp:protocol=<N>`) before main runs, so a
// program and the engine that compiled it always agree — the announcement only
// ever disagrees when a *cached* helper binary outlives an engine change, which
// is precisely the case worth catching.
//
// Bump when the meaning of an existing directive changes, or when a new
// directive is added that a program may rely on. An engine seeing a HIGHER
// number than this must refuse: it cannot know what it is being asked to do,
// and "warn and ignore" would turn that into a silently different build.
// v2 (#359): adds `rerun-if-changed-glob`.
inline constexpr int kProtocolVersion = 4;

// ── Cache-format epoch ─────────────────────────────────────────────────────
//
// Bump ONLY when previously written build.mcpp.cache entries become unusable —
// the record shape changed, or a directive's *interpretation* changed so that
// replaying a cached value would no longer mean what it meant when written.
// Deliberately NOT the mcpp release number: folding the whole version in would
// re-run every build program on every release for nothing. Same discipline as
// mcpp.build.cache_key::kCacheEpoch.
// Epoch 2 (#359): entries gained `glob` records. An engine that does not know
// them would replay a strict subset of the declared inputs and call a stale
// build fresh, which is exactly the silent-wrong-answer this guard exists for.
inline constexpr int kCacheEpoch = 2;

// ── Run bound ──────────────────────────────────────────────────────────────
//
// How long a build program may RUN before mcpp kills it. The compile is
// deliberately left unbounded — the same asymmetry `mcpp test` settled on
// (run 300s / build 0): a long compile is usually legitimate (a first-run std
// module build is minutes) and killing it produces a baffling failure, while a
// long-running build PROGRAM is usually stuck, and without a bound the whole
// build hangs with no diagnostic at all.
inline constexpr int kDefaultRunTimeoutSecs = 600;

// The environment override, in seconds. `nullopt` means "not set / unusable"
// — a malformed or negative value is ignored rather than fatal, which is the
// behaviour this variable has always had.
//
// Split out from run_timeout() so the PRECEDENCE below is a pure function and
// can be tested without touching the process environment.
std::optional<int> env_timeout_override();

// The effective bound. Precedence, highest first:
//
//   1. `envSecs`       — MCPP_BUILD_PROGRAM_TIMEOUT, this invocation only
//   2. `manifestSecs`  — `[build] build_program_timeout` in the manifest of
//                        the package that OWNS this build.mcpp (its author is
//                        the one who knows how long the generator takes; a
//                        consumer who needs to override reaches for the env
//                        var, which is global for exactly that reason)
//   3. kDefaultRunTimeoutSecs
//
// Zero at any level means "no bound" and is returned as a zero duration, which
// capture_exec_deadline treats as unbounded. `nullopt` and `0` are therefore
// NOT the same thing at either level, which is why both are optionals all the
// way down rather than an int with a sentinel.
//
// Mirrors the precedence `macos_deployment_target` documents (env > manifest >
// built-in default); a second shape for the same idea would be one more thing
// to remember.
std::chrono::milliseconds run_timeout(std::optional<int> envSecs,
                                      std::optional<int> manifestSecs);

// Convenience for callers that have a manifest but no reason to read the
// environment themselves.
std::chrono::milliseconds run_timeout_for(std::optional<int> manifestSecs);

} // namespace mcpp::build::program_protocol

namespace mcpp::build::program_protocol {

std::optional<int> env_timeout_override() {
    const char* v = std::getenv("MCPP_BUILD_PROGRAM_TIMEOUT");
    if (!v) return std::nullopt;
    std::string_view sv(v);
    int parsed = 0;
    auto r = std::from_chars(sv.data(), sv.data() + sv.size(), parsed);
    if (r.ec != std::errc{} || r.ptr != sv.data() + sv.size()) return std::nullopt;
    if (parsed < 0) return std::nullopt;
    return parsed;
}

std::chrono::milliseconds run_timeout(std::optional<int> envSecs,
                                      std::optional<int> manifestSecs) {
    int secs = kDefaultRunTimeoutSecs;
    if (manifestSecs && *manifestSecs >= 0) secs = *manifestSecs;
    if (envSecs && *envSecs >= 0)           secs = *envSecs;
    return std::chrono::milliseconds(static_cast<long long>(secs) * 1000);
}

std::chrono::milliseconds run_timeout_for(std::optional<int> manifestSecs) {
    return run_timeout(env_timeout_override(), manifestSecs);
}

} // namespace mcpp::build::program_protocol
