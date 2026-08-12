// bench.spec — WHAT gets measured, expressed as data.
//
// Scenarios, jobs and the matrix live here so that adding a measurement never
// means editing the runner. The runner knows how to time a thing; this module
// knows which things are worth timing and how to perturb the tree first.
export module bench.spec;

import std;
import bench.protocol;

export namespace bench {

// The perturbation applied immediately before a timed build. Every one of these
// answers a different question, and the names are the vocabulary the whole
// suite (CLI, CI inputs, result files) speaks.
enum class Scenario {
    Cold,       // no build dir at all: full graph construction + every compile
    Noop,       // nothing changed: how cheap is "already up to date"
    TouchHub,   // mtime bump on a widely-imported unit, CONTENT UNCHANGED —
                // can the engine prove the interface did not change?
    EditBody,   // real SEMANTIC edit inside a function body — the everyday
                // developer loop. For an inline body in an interface unit this
                // legitimately changes the BMI and a cascade is CORRECT; that
                // is the point of comparing it against `modules-impl`, where
                // the same edit touches no interface at all.
    EditComment, // the file's bytes change but its interface does not (a comment
                // is inserted into a widely-imported unit). Distinct from
                // TouchHub: mtime engines see a real content change here, so
                // only an engine that compares the produced BMI can avoid the
                // cascade. Keeping this separate from EditBody is what stops a
                // "12x faster on edits" claim that is really about comments.
    TouchLeaf,  // mtime bump on a unit nobody imports: recompile 1 + link
};

constexpr std::string_view to_string(Scenario s) {
    switch (s) {
        case Scenario::Cold:      return "cold";
        case Scenario::Noop:      return "noop";
        case Scenario::TouchHub:  return "touch-hub";
        case Scenario::EditBody:  return "edit-body";
        case Scenario::EditComment: return "edit-comment";
        case Scenario::TouchLeaf: return "touch-leaf";
    }
    return "unknown";
}

constexpr std::optional<Scenario> scenario_from(std::string_view s) {
    if (s == "cold")       return Scenario::Cold;
    if (s == "noop")       return Scenario::Noop;
    if (s == "touch-hub")  return Scenario::TouchHub;
    if (s == "edit-body")  return Scenario::EditBody;
    if (s == "edit-comment") return Scenario::EditComment;
    if (s == "touch-leaf") return Scenario::TouchLeaf;
    return std::nullopt;
}

// Everything an engine needs to act, and nothing about how it is timed.
struct Job {
    std::filesystem::path project_dir;   // the fixture instance (holds the sources)
    // Where THIS engine's build description lives. Equal to project_dir for a
    // generated fixture, where the harness emits one file per engine into the
    // tree it just created.
    //
    // For a REAL project they separate. mcpp is built by mcpp, so a CMakeLists
    // and an xmake.lua at its root are files every contributor has to learn to
    // ignore; they live in bench/projects/<name>/ instead and reach back into
    // the tree. The alternative — copying them in for the duration of a run —
    // writes into the user's repository, which this harness refuses to do.
    std::filesystem::path buildfile_dir;
    std::filesystem::path build_dir;     // where this engine may write
    std::filesystem::path log_path;      // child stdout+stderr goes here
    Variant               variant{Variant::Modules};
    std::string           profile{"release"};   // release | debug
    std::string           compiler{"default"};  // gcc | clang | default
    int                   jobs{0};              // 0 = let the engine decide
};

// NOTE: which file each scenario perturbs is NOT declared here. Only the fixture
// knows its own shape — "hub" means something different in a 10-unit synthetic
// project than in mcpp's 137-module graph — so `fixture::Targets` owns it and
// this module stays free of any assumption about the project being measured.

// Cold builds are expensive and their variance is low; incremental scenarios are
// cheap and noisier, so they get more repetitions. Encoded here rather than in
// the runner so the policy is visible next to the scenario it applies to.
constexpr int default_runs(Scenario s) { return s == Scenario::Cold ? 3 : 5; }

}  // namespace bench
