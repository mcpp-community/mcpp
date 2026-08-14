// The bench result protocol: what a measurement IS, independent of who produced
// it or what reads it.
//
// This module is the one piece of the suite that is expensive to change, so it
// is deliberately small and carries no logic beyond serialisation. Engines,
// scenarios and analysis all write toward these types; none of them may add a
// field without bumping kProtocolVersion.
//
// Three invariants are encoded here rather than left to convention, because each
// one was violated by the shell harness this suite replaces:
//
//   1. A FAILURE MUST NOT BE ABLE TO LOOK LIKE A MEASUREMENT. `status` and the
//      timings are separate fields, and a non-ok status carries no median. The
//      old harness formatted a failed cell as "0.000 s" and three of them went
//      into a results file looking like the fastest builds ever recorded.
//   2. A SKIP MUST CARRY ITS REASON. "bazel is not installed here" and "bazel
//      ran and failed" are opposite conclusions; `Unavailable` vs `Failed` plus
//      a mandatory note keeps them apart.
//   3. RESULTS TRAVEL WITH THEIR HOST. A wall-clock number without the machine
//      that produced it is not comparable to anything — least of all on a
//      heterogeneous CPU, where "32 cores" is not 32 of the same thing.
export module bench.protocol;

import std;

export namespace bench {

// Bump on ANY field addition/removal/semantic change. Readers compare against
// their own expectation and degrade explicitly rather than mis-parsing.
inline constexpr int kProtocolVersion = 1;

// ---------------------------------------------------------------------------

enum class Status { Ok, Failed, Skipped, Unavailable };

constexpr std::string_view to_string(Status s) {
    switch (s) {
        case Status::Ok:          return "ok";
        case Status::Failed:      return "failed";
        case Status::Skipped:     return "skipped";
        case Status::Unavailable: return "unavailable";
    }
    return "unknown";
}

// The source form the fixture is expressed in. This is the axis the whole suite
// exists to measure, so it is a first-class enum rather than a string tag.
enum class Variant {
    Headers,      // classic headers + separate .cpp implementation
    Modules,      // module interface units carrying their implementations
    ModulesImpl,  // module interface units + separate implementation units
    // An existing project measured as-is. The variant axis does not apply: the
    // project is whatever it already is, and generating over it would destroy
    // the very thing being measured.
    Native,
};

constexpr std::string_view to_string(Variant v) {
    switch (v) {
        case Variant::Headers:     return "headers";
        case Variant::Modules:     return "modules";
        case Variant::ModulesImpl: return "modules-impl";
        case Variant::Native:      return "native";
    }
    return "unknown";
}

constexpr std::optional<Variant> variant_from(std::string_view s) {
    if (s == "headers")      return Variant::Headers;
    if (s == "modules")      return Variant::Modules;
    if (s == "modules-impl") return Variant::ModulesImpl;
    if (s == "native")       return Variant::Native;
    return std::nullopt;
}

// ---------------------------------------------------------------------------

struct HostInfo {
    std::string   os;
    std::string   arch;
    std::string   cpu_model;
    int           logical_cores{};
    int           physical_cores{};
    // A 13900K is 8 P-cores + 16 E-cores. Reading its 32 threads as 32 equal
    // cores makes every parallelism figure wrong, so the fact is recorded
    // rather than inferred by whoever reads the numbers later.
    bool          heterogeneous{};
    std::uint64_t ram_bytes{};
    std::string   toolchain;   // e.g. "gcc 16.1.0"
};

// The full coordinate of one measurement. Every field is part of the identity;
// two cells differing in any of them are different measurements, never repeats.
struct CellKey {
    std::string engine;
    std::string compiler;
    std::string profile;
    std::string scenario;
    std::string fixture;
    std::string variant;

    [[nodiscard]] std::string str() const;
};

struct Sample {
    double wall_s{};
    int    exit_code{};
};

class CellResult {
public:
    CellKey             key;
    std::vector<Sample> samples;
    Status              status{Status::Skipped};
    std::string         note;      // required whenever status != Ok

    // Timings are DERIVED, never set alongside a non-ok status — that is what
    // makes invariant 1 structural instead of a review comment.
    [[nodiscard]] bool   has_timing() const;
    [[nodiscard]] double median_s() const;
    [[nodiscard]] double min_s() const;
    [[nodiscard]] double max_s() const;
};

struct Report {
    HostInfo                 host;
    std::string              started_at;   // ISO-8601, filled by the caller
    std::vector<CellResult>  cells;
    // Identifies WHICH RUN produced this. See RunId.
    std::string              run_id;
};

// ── The identity of a run: ONE fingerprint over the whole configuration ────
//
// Same shape as `mcpp build`: everything that decides WHAT is measured is
// hashed into one value, that value names a cache directory, a re-run with the
// same configuration hits it, and a different configuration lands somewhere
// else instead of overwriting. `--id` is not a separate concept — it is simply
// one more input to the hash, so passing it forks a fresh cache exactly the way
// changing the engine list does.
//
// Hashed: engines, variants, scenarios, run count, compiler, profile, fixture
// shape, project, buildfiles, and `--id`.
//
// ⚠️ NOT hashed: the mcpp binary, the installed cmake, anything that can change
// underneath while the command line stays the same. Folding those in would
// restart from zero on every rebuild — the normal case while developing, and
// exactly when resume is worth having. They are recorded per entry as observed
// facts, and adopting a record measured with a different one is REPORTED.
//
// ⚠️ WHY AN IDENTITY AT ALL: resuming means treating an old record as this
// run's result, so without one a resumable benchmark is a machine for silently
// splicing runs together. Not hypothetical — a killed run did not die on
// SIGTERM, finished its cell and wrote its report AFTER the `rm -rf` meant to
// clear the directory, leaving a 90-cell file beside a 72-cell file under names
// that gave no hint. The only field that told them apart was `started_at`,
// inside the JSON.
struct RunId {
    std::string fingerprint;      // 8 hex chars, like mcpp's build directories
    std::string config;           // the text it was taken over, for `--explain`

    [[nodiscard]] std::string str() const;
    [[nodiscard]] bool operator==(const RunId&) const = default;

    // FNV-1a. Short and stable; a cache key, not a security boundary.
    [[nodiscard]] static RunId of(std::string config_text);
};

// One measured unit: the smallest thing worth saving, and the granularity a
// resume works at.
//
//     os · toolchain · project · variant · scenario · engine · run index
//
// Appended to the journal the moment it is measured, so a kill costs at most
// the unit in flight rather than the whole invocation.
struct JournalEntry {
    std::string id;         // RunId::str()
    // The engine's own version banner AT THE TIME this was measured. Not part
    // of the id — a rebuilt binary must not invalidate a resume — but recorded
    // so that adopting a record measured with a different one is REPORTED
    // rather than silent.
    std::string engine_version;
    std::string project;
    std::string variant;
    std::string scenario;
    std::string engine;
    int         run{};      // 1-based
    double      wall_s{};
    int         exit_code{};
};

// Serialisation. Hand-rolled on purpose: the suite must build with nothing but
// `import std;` so it can be the FIRST thing that runs on a fresh machine.
// Defined in protocol.cpp — see the header comment for why.
std::string to_json(const Report& r);

}  // namespace bench
