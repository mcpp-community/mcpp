// bench.runner — turns a cell coordinate into a measurement.
//
// The runner knows how to time and how to perturb; it knows nothing about any
// particular engine or source form. Everything engine-specific arrives through
// the Engine interface, everything project-specific through the fixture.
export module bench.runner;

import std;
import bench.protocol;
import bench.spec;
import bench.platform;
import bench.engines.engine;
import bench.fixture.generate;
import bench.fixture.buildfiles;

export namespace bench {

struct RunOptions {
    std::filesystem::path work_root{"bench-work"};
    fixture::Shape        shape{};
    int                   jobs{0};
    int                   runs_override{0};   // 0 → per-scenario default

    // Project mode: measure an EXISTING tree instead of a generated fixture.
    // This is how mcpp benchmarks itself, and how the suite is pointed at any
    // real codebase — a synthetic graph cannot reproduce the shape of one.
    // One directory holding the FOREIGN build descriptions for --project mode
    // (CMakeLists.txt, xmake.lua, ...). Empty → each engine reads its
    // description from the project tree, which is what a generated fixture does.
    std::filesystem::path buildfiles;
    std::filesystem::path project;            // empty → generate a fixture
    fixture::Targets      project_targets{};  // which files the scenarios perturb
    // The requested compiler, so the generated mcpp manifest can pin the same
    // family the other engines are handed.
    std::string           compiler;

    // How long ONE configure or build may run. 0 = forever, which is the right
    // default for a library and the wrong one for CI — main gives it a value.
    double                timeout_s{0.0};

    // Live progress, and the ONLY thing that makes a long run legible while it
    // is happening. A cell prints when it finishes, so a matrix cell that hangs
    // in its third engine looks identical to one that hangs in its first: two
    // CI jobs sat 25 minutes inside a child with a completely silent log.
    //
    // A callback rather than a print, because the runner must not own an output
    // policy — the tests drive it with no sink at all.
    std::function<void(std::string_view)> on_progress;

    // ── Resume, at the granularity of ONE measured unit ────────────────────
    //
    // The unit is (project, variant, scenario, engine, run). `already_done`
    // answers "is this one already in the journal for THIS run key"; `record`
    // appends it the moment it is measured.
    //
    // Hooks rather than a Journal member so the runner keeps knowing nothing
    // about files — the same reason `on_progress` is a callback.
    //
    // THE SEED BUILD IS NOT A UNIT and is redone on resume. An incremental
    // scenario only means anything against an up-to-date tree, and that state
    // is what the seed establishes; it lives on disk, not in the journal. So
    // resuming a partially-measured cell costs one seed build. Stated because
    // otherwise resume looks free and someone will be surprised by the clock.
    std::function<bool(std::string_view scenario, std::string_view engine,
                       std::string_view variant, int run)>              already_done;
    std::function<void(std::string_view scenario, std::string_view engine,
                       std::string_view variant, int run,
                       double wall_s, int exit_code)>                    record;
    // The recorded (wall_s, exit) for a unit `already_done` said yes to.
    std::function<std::pair<double, int>(std::string_view scenario, std::string_view engine,
                                         std::string_view variant, int run)> recorded_sample;
};

namespace detail {

// EditBody must present content the previous build has never seen, on EVERY
// repetition. An idempotent edit is a real edit on run 1 and a bare `touch` on
// runs 2..N — which measures a different, much cheaper scenario and silently
// drags the median toward it. The counter is what keeps every run honest.
// Inserts text into the first function body of `file`.
//
// `statement` decides WHAT this measures, and the two are not interchangeable:
//
//   true  — a real statement. The function's object code changes, and for an
//           inline body in an interface unit the BMI changes too, so a cascade
//           is the CORRECT answer, not a defect.
//   false — a comment. The bytes change but nothing observable does, so an
//           engine that compares the produced BMI can stop the cascade while an
//           mtime-only engine cannot.
//
// They used to be one function that inserted a comment and was called
// "edit_body". Every "engine X is N times faster on edits" number it produced
// was really a statement about comments.
// Returns WHICH FORM the perturbation took, or nullopt if it could not be
// applied. The form is not a detail — it changes what the cell measures:
//
//   "in-body"     the text lands inside the first function body, so every
//                 subsequent line in the file moves. GCC records source
//                 locations for inline bodies in the BMI, so the BMI genuinely
//                 changes and a cascade is CORRECT.
//   "end-of-file" the unit had no function body to insert into, so a comment is
//                 appended instead. No existing line moves, the BMI is
//                 unchanged, and an engine that compares BMIs skips the whole
//                 cascade.
//
// Those are different questions, and the suite was answering both under one
// scenario name. Measured on the same engine, same compiler, same day:
// `edit-comment` on mcpp's hub (66 lines, no bodies → end-of-file) came out at
// 0.38s, and on xlings' hub (566 lines, 56 bodies → in-body) at 95.02s. Read
// side by side without knowing the form, that reads as "the optimisation works
// on one project and not the other", which is not what happened at all.
//
// So the form goes into the cell's note. Same rule as `status` carrying its
// reason: a number whose meaning depends on an invisible choice is not a
// measurement.
std::optional<std::string_view> insert_into_first_body(
    const std::filesystem::path& file, int nonce, bool statement);

}  // namespace detail

class Runner {
public:
    // Explains a non-zero result. `RunResult::started()` is false when the child
    // never ran at all (bad path, not executable, missing loader) — in that case
    // the log file exists but is EMPTY, and pointing a reader at it sends them
    // looking for a compiler error that was never emitted. Say which of the two
    // happened.
    static std::string failure_note(std::string_view what, const platform::RunResult& r, const std::filesystem::path& log);

    explicit Runner(RunOptions opt);

    // Materialise one fixture instance. Kept separate from measure() so a single
    // tree is reused across scenarios of the same (engine, variant) pair — the
    // generation cost is real and belongs to neither measurement.
    struct Instance {
        std::filesystem::path project_dir;
        std::filesystem::path build_dir;
        fixture::Targets      targets;
    };

    // Where child stdout/stderr is collected. Always under the work root, so it
    // is disposable and never lands in the project being measured.
    std::filesystem::path log_dir() const;

    Instance materialise(std::string_view engine, Variant variant) const;

    // `compiler` is what the engine is told to use (possibly an absolute path
    // to a hermetic payload); `compiler_label` is the short name that goes into
    // the result key. Keeping them apart matters: the path pins fairness, the
    // label is what makes a result table readable and mergeable across machines
    // where the same compiler lives at a different path.
    CellResult measure(engines::Engine& engine, const Instance& inst, Variant variant, Scenario scenario, std::string_view profile, std::string_view compiler, std::string_view compiler_label, std::string_view fixture_name) const;

private:
    RunOptions opt_;

    void report(std::string_view what) const;

    // Restores a file's exact bytes on destruction. Not a convenience: without
    // it a benchmark run leaves edit markers in the measured repository, and a
    // failed cell leaves them silently.
    class SourceGuard {
    public:
        explicit SourceGuard(std::filesystem::path file) : file_(std::move(file)) {
            if (file_.empty()) return;
            std::ifstream in(file_, std::ios::binary);
            if (!in) { file_.clear(); return; }
            saved_.assign((std::istreambuf_iterator<char>(in)),
                          std::istreambuf_iterator<char>());
        }
        ~SourceGuard() {
            if (file_.empty()) return;
            std::ofstream out(file_, std::ios::binary | std::ios::trunc);
            out << saved_;
        }
        SourceGuard(const SourceGuard&)            = delete;
        SourceGuard& operator=(const SourceGuard&) = delete;
    private:
        std::filesystem::path file_;
        std::string           saved_;
    };

    // Which target a scenario needs, and whether it is present and real.
    std::string unmet_target(const Instance& inst, Scenario scenario) const;

    // nullopt = could not apply. A non-empty string_view describes the FORM,
    // which the caller records in the cell note — see insert_into_first_body.
    // nullopt        — could not apply; the cell fails and says so.
    // an EMPTY view   — applied, and the scenario's name already says everything
    //                   about what was done (`cold` cleans, `touch-*` bumps an
    //                   mtime; there is only one way to do either).
    // a NON-EMPTY view — applied in one of several forms, and the form changes
    //                   what the cell measures, so it is recorded in the note.
    //                   Only the editing scenarios have this; see
    //                   insert_into_first_body.
    std::optional<std::string_view> perturb(engines::Engine& engine, const Job& job, const Instance& inst, Scenario scenario, int nonce) const;
};

}  // namespace bench
