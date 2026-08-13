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
inline std::optional<std::string_view> insert_into_first_body(
    const std::filesystem::path& file, int nonce, bool statement) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return std::nullopt;
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
    std::string_view form = "in-body";
    if (brace == std::string::npos) {
        if (statement) return std::nullopt;
        form = "end-of-file";
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
    return form;
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
        if (r.timed_out)
            return std::format("{} TIMED OUT after {:.0f}s and was killed (see {})",
                               what, r.wall_s, log.string());
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
        // mcpp reads its own manifest from the tree and ignores this; cmake and
        // xmake are told to look here for their description.
        job.buildfile_dir = opt_.buildfiles.empty() ? inst.project_dir : opt_.buildfiles;
        job.build_dir   = inst.build_dir;
        // The child log goes in the WORK directory, never inside the measured
        // tree. In --project mode that tree is the user's repository, and a
        // harness that drops files into it is one `git add -A` away from
        // committing its own scratch (which is exactly what happened once).
        job.log_path    = log_dir() / std::format("{}-{}.log", engine.name(),
                                                  to_string(scenario));
        // Cleared ONCE per cell; every child then appends. The alternative —
        // truncating per child — is what made a 0.60s "cold" build unexplainable:
        // the timed build's one line of output had erased the configure that
        // preceded it.
        { std::ofstream clear(job.log_path, std::ios::binary | std::ios::trunc); }
        job.variant     = variant;
        job.profile     = std::string(profile);
        job.compiler    = std::string(compiler);
        job.jobs        = opt_.jobs;
        job.timeout_s   = opt_.timeout_s;

        // Turns a failure into something a reader can act on WITHOUT the log
        // file, which on a CI runner is deleted with the machine. Every module
        // cell in the matrix failed behind a bare "see .../cmake-cold.log" and
        // the job stayed green; neither half of that was noticed for weeks.
        const auto fail = [&](std::string_view what, const platform::RunResult& r) {
            cell.status = Status::Failed;
            cell.note   = failure_note(what, r, job.log_path);
            report(cell.note);
            if (const auto tail = platform::tail_of(job.log_path); !tail.empty())
                report(std::format("--- last lines of {} ---\n{}",
                                   job.log_path.filename().string(), tail));
        };

        report("configure");
        if (const auto cfg = engine.configure(job); !cfg.ok()) {
            fail("configure", cfg);
            return cell;
        }

        // One untimed seed build. An incremental scenario is only incremental
        // against an up-to-date tree, and it warms the page cache so run 1 is
        // not systematically slower than the rest.
        report("seed build");
        if (const auto seed = engine.build(job); !seed.ok()) {
            fail("seed build", seed);
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

        std::string_view perturbForm;
        const int runs = opt_.runs_override > 0 ? opt_.runs_override : default_runs(scenario);
        for (int i = 0; i < runs; ++i) {
            report(std::format("run {}/{}", i + 1, runs));
            const auto form = perturb(engine, job, inst, scenario, i);
            if (!form) {
                cell.status = Status::Failed;
                cell.note   = std::format("could not apply scenario '{}'", to_string(scenario));
                report(cell.note);
                return cell;
            }
            if (!form->empty()) perturbForm = *form;

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
                    fail(std::format("re-configure on run {}", i + 1), cfg);
                    return cell;
                }
                extra = cfg.wall_s;
            }

            const auto r = engine.build(job);
            if (!r.ok()) {
                fail(std::format("build on run {}", i + 1), r);
                return cell;
            }
            cell.samples.push_back(Sample{extra + r.wall_s, r.exit_code});
        }
        cell.status = Status::Ok;
        // The engine's version banner, plus HOW the source was perturbed when
        // that choice was not fixed by the scenario name alone.
        cell.note = perturbForm.empty()
            ? avail.note
            : std::format("{} · perturbation: {}", avail.note, perturbForm);
        return cell;
    }

private:
    RunOptions opt_;

    void report(std::string_view what) const {
        if (opt_.on_progress) opt_.on_progress(what);
    }

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
    std::optional<std::string_view> perturb(engines::Engine& engine, const Job& job,
                                            const Instance& inst,
                                            Scenario scenario, int nonce) const {
        const std::optional<std::string_view> applied{std::string_view{}};
        const auto done = [&](bool ok) { return ok ? applied : std::nullopt; };
        switch (scenario) {
            case Scenario::Cold:        engine.clean(job); return applied;
            case Scenario::Noop:        return applied;
            case Scenario::TouchHub:    return done(platform::touch(inst.targets.hub));
            case Scenario::TouchLeaf:   return done(platform::touch(inst.targets.leaf));
            case Scenario::EditBody:
                return detail::insert_into_first_body(inst.targets.body, nonce, true);
            case Scenario::EditComment:
                return detail::insert_into_first_body(inst.targets.hub, nonce, false);
        }
        return std::nullopt;
    }
};

}  // namespace bench
