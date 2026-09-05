// bench.runner — implementation.
//
// `module bench.runner;` with no `export`: an implementation unit. The runner is
// where a measurement actually happens — seed build, perturbation, timed build,
// journal append — and none of that belongs in a BMI. Only `RunOptions`,
// `Runner` and `Instance` stay declared in the interface, because those are what
// main.cpp names.
module bench.runner;

import std;
import bench.protocol;
import bench.spec;
import bench.platform;
import bench.engines.engine;
import bench.fixture.generate;
import bench.fixture.buildfiles;

namespace bench {

namespace detail {

std::optional<std::string_view> insert_into_first_body(
    const std::filesystem::path& file, int nonce, bool statement) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return std::nullopt;
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();

    // Insert inside the first function body: immediately after the first '{'
    // that follows a ')'. Anchoring on the brace rather than a name keeps this
    // working for all three variants, whose function text differs.
    //
    // AFTER THE BRACE, not after the newline that follows it. Those are the
    // same position only when the body spans several lines. Given a one-line
    // body — `export int f() { return 1; }` — the newline is past the CLOSING
    // brace, so the statement landed at namespace scope and the build died with
    //
    //     error: expected unqualified-id
    //         volatile int bench_nonce_0 = 0; (void)bench_nonce_0;
    //
    // pointing at a file the harness had just written. Honest (the cell failed
    // loudly) but wrong: the perturbation is supposed to be applicable to any
    // function, and "your function is on one line" is not a real limitation.
    //
    // A file may legitimately have NO function body — the modules-impl variant's
    // interface unit only declares — so a comment falls back to end-of-file
    // rather than reporting the scenario as inapplicable. A statement has no
    // such fallback: there is nowhere to put it that would mean the same thing.
    const auto paren = text.find(") {");
    const auto brace = paren == std::string::npos ? std::string::npos : paren + 2;
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
        // The leading newline is what makes a one-line body work: the text
        // opens its own line immediately after `{`, whatever followed it.
        text.insert(brace + 1,
                    statement
                        ? std::format("\n    volatile int bench_nonce_{0} = {0};"
                                      " (void)bench_nonce_{0};", nonce)
                        : std::format("\n    // bench: comment perturbation #{}", nonce));
    }

    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << text;
    return form;
}

}  // namespace detail

std::string Runner::failure_note(std::string_view what, const platform::RunResult& r, const std::filesystem::path& log) {
        if (!r.started())
            // The OS's own reason when there is one. "check the engine's program
            // path" was the only advice this could give, and it is wrong as
            // often as it is right — a windows/clang cell reported it while
            // `xmake --version` had just succeeded in the same job, so the path
            // was demonstrably fine and something else (a cwd, a handle) was not.
            return std::format("{}: could not start the process (no log written){}",
                               what,
                               r.start_error.empty()
                                   ? std::string(" — check the engine's program path")
                                   : std::format(" — {}", r.start_error));
        if (r.timed_out)
            return std::format("{} TIMED OUT after {:.0f}s and was killed (see {})",
                               what, r.wall_s, log.string());
        return std::format("{} exited {} (see {})", what, r.exit_code, log.string());
    }

Runner::Runner(RunOptions opt) : opt_(std::move(opt)) {}

std::filesystem::path Runner::log_dir() const {
        std::error_code ec;
        auto d = opt_.work_root / "logs";
        std::filesystem::create_directories(d, ec);
        return d;
    }

Runner::Instance Runner::materialise(std::string_view engine, Variant variant) const {
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

CellResult Runner::measure(engines::Engine& engine, const Instance& inst, Variant variant, Scenario scenario, std::string_view profile, std::string_view compiler, std::string_view compiler_label, std::string_view fixture_name) const {
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
            // 20 lines is right for an ordinary failure and useless for the one
            // that most needs a log: a compiler CRASH. clang prints ~40 lines of
            // stack dump after the line that names the file and the pass, so a
            // 20-line tail shows frames #30..#36 and the bare
            // `clang frontend command failed with exit code 139` — everything
            // identifying WHAT it was compiling has already scrolled past. That
            // is exactly what happened to the xlings/clang cell, and it is why
            // that crash is still undiagnosed.
            const auto crashed = platform::log_mentions(
                job.log_path, {"PLEASE submit a bug report", "Stack dump"});

            // A TAIL IS THE WRONG SHAPE WHEN THE TOOL IS CHATTY. Every build
            // engine here prints a progress line per translation unit, so 20
            // lines of tail is 20 lines of `generating.module.deps ...` and the
            // error that actually stopped it — printed once, hundreds of lines
            // earlier — is gone. That is not hypothetical: `xmake/clang` failed
            // with `seed build exited 255` and the captured tail contained
            // nothing but progress, so the cell could not be diagnosed from CI
            // at all and cost a full matrix cycle to learn nothing.
            //
            // So the lines that LOOK like an error are pulled out first, from
            // anywhere in the file, and the tail follows as context. Cheap, and
            // it is the difference between "exited 255" and a cause.
            if (const auto why = platform::log_grep(
                    job.log_path,
                    // EVERY ENGINE SPELLS IT DIFFERENTLY, and the first
                    // version of this list only knew the compiler's spelling.
                    // cmake writes `CMake Error in CMakeLists.txt:` — no colon
                    // after "Error", capital E — so the very next failure it
                    // was meant to explain slipped straight through it. That is
                    // the sieve's own version of the bug it exists to catch.
                    {"error:", "error :", "ERROR:", " error ", "Error in",
                     "Error at", "Error:", "fatal", "not found", "No such file",
                     "cannot find", "undefined", "failed to", "step failed",
                     "requires that", "Assertion", "abort"},
                    /*max=*/12);
                !why.empty())
                report(std::format("--- error lines from {} ---\n{}",
                                   job.log_path.filename().string(), why));

            if (const auto tail = platform::tail_of(job.log_path, crashed ? 80 : 20);
                !tail.empty())
                report(std::format("--- last lines of {} ---\n{}",
                                   job.log_path.filename().string(), tail));
        };

        // Asked once the Job exists, because the answer depends on the PROJECT —
        // which is why it cannot live beside the `supports()` check above.
        if (auto why = engine.unbuildable_reason(job); !why.empty()) {
            cell.status = Status::Unavailable;
            cell.note   = std::move(why);
            report(cell.note);
            return cell;
        }

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
            // Already in the journal under this run key: adopt it and move on.
            //
            // The PERTURBATION IS STILL SKIPPED ALONG WITH THE MEASUREMENT, and
            // that is the subtle part. `edit-body` inserts text carrying the run
            // index, so replaying a recorded run must not re-apply its edit —
            // doing so would leave the tree carrying an edit that no timing in
            // this run accounts for, and the NEXT run's build would inherit it.
            if (opt_.already_done &&
                opt_.already_done(to_string(scenario), engine.name(),
                                  to_string(variant), i + 1)) {
                report(std::format("run {}/{} — already recorded, skipping", i + 1, runs));
                if (opt_.recorded_sample) {
                    const auto s = opt_.recorded_sample(to_string(scenario), engine.name(),
                                                        to_string(variant), i + 1);
                    cell.samples.push_back(Sample{s.first, s.second});
                }
                continue;
            }
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
            if (opt_.record)
                opt_.record(to_string(scenario), engine.name(), to_string(variant),
                            static_cast<int>(i) + 1, extra + r.wall_s, r.exit_code);
        }
        cell.status = Status::Ok;
        // The engine's version banner, plus HOW the source was perturbed when
        // that choice was not fixed by the scenario name alone.
        cell.note = perturbForm.empty()
            ? avail.note
            : std::format("{} · perturbation: {}", avail.note, perturbForm);
        return cell;
    }

void Runner::report(std::string_view what) const {
        if (opt_.on_progress) opt_.on_progress(what);
    }

std::string Runner::unmet_target(const Instance& inst, Scenario scenario) const {
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

std::optional<std::string_view> Runner::perturb(engines::Engine& engine, const Job& job, const Instance& inst, Scenario scenario, int nonce) const {
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

}  // namespace bench
