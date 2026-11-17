// mcpp.build.schedule.policy — what every build has whatever its shape: how
// many compilers run at once, and the compiler cap of a generation.
//
// The shape of a module build (two-phase, detached code generation) and the
// switch that asks for it are mcpp.buildopt.schedule's, with the measurements
// behind the table: they are among the things mcpp does beyond what the
// compiler and ninja do, and those are kept in one package (modules/buildopt,
// SPEC-019). `decide` here applies the one rule that is not an optimization --
// every compiler of a build is a participant of its generation -- to the shape
// that table gives.

export module mcpp.build.schedule.policy;

import std;
import mcpp.toolchain.model;
import mcpp.manifest;
import mcpp.invocation;
import mcpp.build.capacity;
export import mcpp.buildopt.schedule;

export namespace mcpp::build::schedule {

// The shape of a module build and the switch that asks for it are
// mcpp.buildopt.schedule's: one of the things mcpp does beyond what the
// compiler and ninja do, kept with the others. What stays here is what every
// build has whatever its shape: the job count, and the compiler cap of a
// generation.
using mcpp::buildopt::schedule::Strategy;
using mcpp::buildopt::schedule::to_string;
using mcpp::buildopt::schedule::Decision;
using mcpp::buildopt::schedule::requested_switch;
using mcpp::buildopt::schedule::decide_shape;

// How many compilers this machine should run at once.
//
// Precedence: MCPP_JOBS (where `--jobs` lands) > `[build] jobs` >
// `globalDefault` (the per-machine config's `[build] default_jobs`) > 0,
// meaning "say nothing" and leave the backend's own default. The default is
// unchanged on purpose: altering everyone's concurrency is a behaviour change,
// and `globalDefault` is 0 for everyone who has not written the key.
//
// THE GLOBAL VALUE ARRIVES AS A PARAMETER, not as a config import. This
// function depends on the manifest and on the host, and nothing else; reaching
// into `$MCPP_HOME/config.toml` from here would give the schedule a second
// source of truth to keep consistent. The caller reads the config it has
// already loaded.
//
// It sits BELOW the manifest and ABOVE the backend because of what each of the
// three describes. `MCPP_JOBS` is this invocation. `[build] jobs` is this
// project, and a project that states a number has a reason the machine cannot
// know. `default_jobs` is the machine, and it is the only one of the three that
// can hold a machine fact: `--jobs` must be repeated on every invocation, and
// `[build] jobs` is per-package while `[workspace.build]` rightly refuses it,
// so a seven-member workspace would otherwise carry the number seven times and
// commit a property of one developer's laptop to the repository.
//
// `auto` is resolved HERE, against the machine doing the build, never frozen
// into a manifest. Measured on this repository: the cold self-build takes 81.0s
// at -j8 and 79.9s at -j32 — 4x the workers for 1.4%, because the build is
// latency-bound — while a single module compile peaks at 0.5–1.0 GB, so the
// extra jobs are pure memory pressure. On a high-core, modest-RAM machine the
// backend default swaps.
//
// `onInvalid` is called with the offending text instead of warning directly, so
// this stays free of any UI dependency and remains testable.
int resolve_jobs(const manifest::Manifest& m,
                 const std::function<void(std::string_view)>& onInvalid = {},
                 int globalDefault = 0);

// `requested` is the user's switch: "auto" (default), "on", "off". `hostJobs` is
// the already-resolved parallelism (`--jobs` or `[build] jobs`), or 0 meaning
// "the user said nothing, leave the backend's own default".
//
// `autoJobs` is what this machine would choose if asked — `recommended_jobs`
// against the live host. Computed by the CALLER so `decide` stays pure, and
// needed because 0 is not a usable answer for every strategy:
//
// UNDER DetachCodegen, `hostJobs == 0` MEANS NO BOUND AT ALL. A detached
// compiler stops holding a ninja slot the moment it publishes its BMI, so
// ninja's -j is no longer a limit on how many compilers are running — the
// semaphore is. A cap of 0 disables the semaphore (`acquire_token` returns
// immediately), and the graph then goes out with `sched_cap = 0`: ninja keeps
// starting compiles as fast as BMIs appear, with nothing counting them. That is
// hazard 2 in detach_codegen, and it was live in the DEFAULT configuration —
// nobody passing `--jobs` got any bound, on a workload whose single compile
// peaks near a gigabyte.
Decision decide(const toolchain::Toolchain& tc, std::string_view requested, int hostJobs,
                int autoJobs = 0);

// ---------------------------------------------------------------------------

Decision decide(const toolchain::Toolchain& tc, std::string_view requested, int hostJobs,
                int autoJobs) {
    Decision d = decide_shape(tc, requested, hostJobs, autoJobs);
    // EVERY COMPILER OF A BUILD IS A PARTICIPANT OF ITS GENERATION (2026.11.7.1).
    // Under every strategy a compile edge runs through the capped executor, so a
    // later build does not enter the build directory while a compiler of an
    // earlier one -- whose mcpp was killed -- still writes there (design
    // 2026-11-07 §4.1.2). The cap is the strategy's, else the job count, else
    // what this machine would choose.
    if (d.compilerCap <= 0) d.compilerCap = hostJobs > 0 ? hostJobs : autoJobs;
    return d;
}

int resolve_jobs(const manifest::Manifest& m,
                 const std::function<void(std::string_view)>& onInvalid,
                 int globalDefault) {
    auto from_text = [&](std::string_view v) -> std::optional<int> {
        if (v.empty()) return std::nullopt;
        if (v == "auto") {
            const auto cap = mcpp::build::capacity::host_capacity();
            return mcpp::build::capacity::recommended_jobs(cap);
        }
        int n = 0;
        const auto* first = v.data();
        const auto* last  = v.data() + v.size();
        if (auto [p, ec] = std::from_chars(first, last, n);
            ec == std::errc{} && p == last && n > 0)
            return n;
        // A malformed value must not silently become "use the default" — that
        // is how a typo turns into a build that is mysteriously slower.
        if (onInvalid) onInvalid(v);
        return std::nullopt;
    };
    if (auto e = mcpp::invocation::get("MCPP_JOBS"))
        if (auto n = from_text(*e)) return *n;
    if (auto n = from_text(m.buildConfig.jobs)) return *n;
    // Not through `from_text`: this value has already been parsed as an
    // integer by the config loader, so "auto" and a malformed spelling are not
    // reachable here and a second diagnostic for them would describe a state
    // that cannot occur. A non-positive value reads as absent, which is what
    // the generated template's `0` means.
    if (globalDefault > 0) return globalDefault;
    return 0;
}

}  // namespace mcpp::build::schedule
