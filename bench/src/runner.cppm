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
    std::filesystem::path project;            // empty → generate a fixture
    fixture::Targets      project_targets{};  // which files the scenarios perturb
    // The requested compiler, so the generated mcpp manifest can pin the same
    // family the other engines are handed.
    std::string           compiler;
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
inline bool insert_into_first_body(const std::filesystem::path& file, int nonce,
                                   bool statement) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();

    // Insert inside the first function body: after the first '{' that follows a
    // ')'. Anchoring on the brace rather than a name keeps this working for all
    // three variants, whose function text differs.
    //
    // A file may legitimately have NO function body — the modules-impl variant's
    // interface unit only declares — so a comment falls back to end-of-file
    // rather than reporting the scenario as inapplicable. A statement has no
    // such fallback: there is nowhere to put it that would mean the same thing.
    const auto paren = text.find(") {");
    const auto brace = paren == std::string::npos ? std::string::npos
                                                  : text.find('\n', paren);
    if (brace == std::string::npos) {
        if (statement) return false;
        text += std::format("\n// bench: comment perturbation #{}\n", nonce);
    } else {
        // `volatile` so no optimiser can delete the edit and hand back the
        // previous object file — that would quietly turn a semantic edit back
        // into a no-op, i.e. straight back into the bug this split exists to fix.
        //
        // The name carries the nonce because perturbations ACCUMULATE across the
        // repetitions of one cell: a fixed name redeclares itself on run 2 and
        // the build fails, which is exactly what the first version did.
        text.insert(brace + 1,
                    statement
                        ? std::format("    volatile int bench_nonce_{0} = {0};"
                                      " (void)bench_nonce_{0};\n", nonce)
                        : std::format("    // bench: comment perturbation #{}\n", nonce));
    }

    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << text;
    return true;
}

}  // namespace detail

class Runner {
public:
    // Explains a non-zero result. `RunResult::started()` is false when the child
    // never ran at all (bad path, not executable, missing loader) — in that case
    // the log file exists but is EMPTY, and pointing a reader at it sends them
    // looking for a compiler error that was never emitted. Say which of the two
    // happened.
    static std::string failure_note(std::string_view what, const platform::RunResult& r,
                                    const std::filesystem::path& log) {
        if (!r.started())
            return std::format("{}: could not start the process (no log written) — "
                               "check the engine's program path", what);
        return std::format("{} exited {} (see {})", what, r.exit_code, log.string());
    }

    explicit Runner(RunOptions opt) : opt_(std::move(opt)) {}

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
    std::filesystem::path log_dir() const {
        std::error_code ec;
        auto d = opt_.work_root / "logs";
        std::filesystem::create_directories(d, ec);
        return d;
    }

    Instance materialise(std::string_view engine, Variant variant) const {
        // PROJECT MODE. The tree already exists and belongs to someone; nothing
        // here may create or delete it. In particular the remove_tree below must
        // never run against it — deleting the user's repository is the one
        // failure mode this whole function has to make impossible.
        if (!opt_.project.empty()) {
            Instance inst;
            inst.project_dir = opt_.project;
            inst.build_dir   = opt_.project / "build";   // used by cmake/meson/xmake
            inst.targets     = opt_.project_targets;
            return inst;
        }

        // The engine LABEL can carry a version ("mcpp@2026.8.12.1"); the
        // directory name must stay predictable and portable, so it is slugged.
        std::string slug(engine);
        for (char& c : slug)
            if (c == '@' || c == '/' || c == '\\' || c == ':' || c == ' ') c = '-';
        const auto dir = opt_.work_root / std::format("{}-{}", slug, to_string(variant));
        platform::remove_tree(dir);
        std::filesystem::create_directories(dir);
        Instance inst;
        inst.project_dir = dir;
        inst.build_dir   = dir / "build";
        inst.targets     = fixture::emit_sources(dir, variant, opt_.shape);
        fixture::emit_all(dir, variant, opt_.shape, opt_.compiler);
        return inst;
    }

    // `compiler` is what the engine is told to use (possibly an absolute path
    // to a hermetic payload); `compiler_label` is the short name that goes into
    // the result key. Keeping them apart matters: the path pins fairness, the
    // label is what makes a result table readable and mergeable across machines
    // where the same compiler lives at a different path.
    CellResult measure(engines::Engine& engine, const Instance& inst, Variant variant,
                       Scenario scenario, std::string_view profile,
                       std::string_view compiler, std::string_view compiler_label,
                       std::string_view fixture_name) const {
        CellResult cell;
        cell.key = CellKey{std::string(engine.name()), std::string(compiler_label),
                           std::string(profile), std::string(to_string(scenario)),
                           std::string(fixture_name), std::string(to_string(variant))};

        // Availability before anything else: "not installed" must never be
        // reported as a slow or broken engine.
        const auto avail = engine.probe();
        if (!avail.present) {
            cell.status = Status::Unavailable;
            cell.note   = avail.note;
            return cell;
        }
        if (!engine.supports(variant, compiler)) {
            cell.status = Status::Unavailable;
            cell.note   = engine.unsupported_reason(variant, compiler);
            return cell;
        }

        // A scenario that needs a file nobody named cannot be run. Reporting it
        // as `skipped` with the reason beats perturbing an arbitrary file, which
        // would produce a number that looks valid and measures something else.
        if (const auto missing = unmet_target(inst, scenario); !missing.empty()) {
            cell.status = Status::Skipped;
            cell.note   = missing;
            return cell;
        }

        Job job;
        job.project_dir = inst.project_dir;
        job.build_dir   = inst.build_dir;
        // The child log goes in the WORK directory, never inside the measured
        // tree. In --project mode that tree is the user's repository, and a
        // harness that drops files into it is one `git add -A` away from
        // committing its own scratch (which is exactly what happened once).
        job.log_path    = log_dir() / std::format("{}-{}.log", engine.name(),
                                                  to_string(scenario));
        job.variant     = variant;
        job.profile     = std::string(profile);
        job.compiler    = std::string(compiler);
        job.jobs        = opt_.jobs;

        if (const auto cfg = engine.configure(job); !cfg.ok()) {
            cell.status = Status::Failed;
            cell.note   = failure_note("configure", cfg, job.log_path);
            return cell;
        }

        // One untimed seed build. An incremental scenario is only incremental
        // against an up-to-date tree, and it warms the page cache so run 1 is
        // not systematically slower than the rest.
        if (const auto seed = engine.build(job); !seed.ok()) {
            cell.status = Status::Failed;
            cell.note   = failure_note("seed build", seed, job.log_path);
            return cell;
        }

        // edit-body rewrites a source file. In project mode that file belongs to
        // the user, so its exact bytes are captured first and restored no matter
        // how this function exits — including on a failed build.
        // Both editing scenarios rewrite a source file. In project mode that file
        // belongs to the user, so the guard must cover each of them — an
        // unguarded scenario silently leaves the perturbation behind.
        const SourceGuard guard(
            scenario == Scenario::EditBody    ? inst.targets.body :
            scenario == Scenario::EditComment ? inst.targets.hub  : std::filesystem::path{});

        const int runs = opt_.runs_override > 0 ? opt_.runs_override : default_runs(scenario);
        for (int i = 0; i < runs; ++i) {
            if (!perturb(engine, job, inst, scenario, i)) {
                cell.status = Status::Failed;
                cell.note   = std::format("could not apply scenario '{}'", to_string(scenario));
                return cell;
            }

            // COLD IS "from nothing to a binary", so it must include configure.
            // Not a detail: cmake and meson keep their configure output INSIDE
            // the build dir that clean() just removed, so building without
            // re-configuring simply fails — which is how this was found. Timing
            // configure separately would also be wrong: a user waiting for a
            // clean build waits for both, and engines that fold configure into
            // the build (mcpp, bazel) would otherwise get a discount for it.
            double extra = 0.0;
            if (scenario == Scenario::Cold) {
                const auto cfg = engine.configure(job);
                if (!cfg.ok()) {
                    cell.status = Status::Failed;
                    cell.note   = failure_note(std::format("re-configure on run {}", i + 1),
                                               cfg, job.log_path);
                    return cell;
                }
                extra = cfg.wall_s;
            }

            const auto r = engine.build(job);
            if (!r.ok()) {
                cell.status = Status::Failed;
                cell.note   = failure_note(std::format("build on run {}", i + 1),
                                           r, job.log_path);
                return cell;
            }
            cell.samples.push_back(Sample{extra + r.wall_s, r.exit_code});
        }
        cell.status = Status::Ok;
        cell.note   = avail.note;
        return cell;
    }

private:
    RunOptions opt_;

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
    std::string unmet_target(const Instance& inst, Scenario scenario) const {
        const std::filesystem::path* want = nullptr;
        std::string_view which;
        switch (scenario) {
            case Scenario::TouchHub:  want = &inst.targets.hub;  which = "--hub";  break;
            case Scenario::TouchLeaf: want = &inst.targets.leaf; which = "--leaf"; break;
            case Scenario::EditBody:  want = &inst.targets.body; which = "--body"; break;
            // Deliberately the HUB, not the body target: the question is whether
            // a non-semantic change to a widely-imported INTERFACE cascades. In
            // the modules-impl variant `body` is an implementation unit with no
            // BMI at all, so asking it there would measure nothing.
            case Scenario::EditComment: want = &inst.targets.hub; which = "--hub"; break;
            default: return {};
        }
        if (want->empty())
            return std::format("scenario '{}' needs a file to perturb; pass {} <path>",
                               to_string(scenario), which);
        std::error_code ec;
        if (!std::filesystem::exists(*want, ec))
            return std::format("{} points at a file that does not exist: {}",
                               which, want->string());
        return {};
    }

    bool perturb(engines::Engine& engine, const Job& job, const Instance& inst,
                 Scenario scenario, int nonce) const {
        switch (scenario) {
            case Scenario::Cold:
                engine.clean(job);
                return true;
            case Scenario::Noop:
                return true;
            case Scenario::TouchHub:
                return platform::touch(inst.targets.hub);
            case Scenario::TouchLeaf:
                return platform::touch(inst.targets.leaf);
            case Scenario::EditBody:
                return detail::insert_into_first_body(inst.targets.body, nonce, true);
            case Scenario::EditComment:
                return detail::insert_into_first_body(inst.targets.hub, nonce, false);
        }
        return false;
    }
};

}  // namespace bench
