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
};

namespace detail {

// EditBody must present content the previous build has never seen, on EVERY
// repetition. An idempotent edit is a real edit on run 1 and a bare `touch` on
// runs 2..N — which measures a different, much cheaper scenario and silently
// drags the median toward it. The counter is what keeps every run honest.
inline bool edit_body(const std::filesystem::path& file, int nonce) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();

    // Insert inside the first function body: after the first '{' that follows a
    // ')'. Anchoring on the brace rather than a name keeps this working for all
    // three variants, whose function text differs.
    const auto paren = text.find(") {");
    if (paren == std::string::npos) return false;
    const auto brace = text.find('\n', paren);
    if (brace == std::string::npos) return false;

    const auto marker = std::format("\n    // bench: body perturbation #{}\n", nonce);
    text.insert(brace + 1, marker);

    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << text;
    return true;
}

}  // namespace detail

class Runner {
public:
    explicit Runner(RunOptions opt) : opt_(std::move(opt)) {}

    // Materialise one fixture instance. Kept separate from measure() so a single
    // tree is reused across scenarios of the same (engine, variant) pair — the
    // generation cost is real and belongs to neither measurement.
    struct Instance {
        std::filesystem::path project_dir;
        std::filesystem::path build_dir;
        fixture::Targets      targets;
    };

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
        fixture::emit_all(dir, variant, opt_.shape);
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
        if (!engine.supports(variant)) {
            cell.status = Status::Unavailable;
            cell.note   = engine.unsupported_reason(variant);
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
        job.log_path    = inst.project_dir / "bench-child.log";
        job.variant     = variant;
        job.profile     = std::string(profile);
        job.compiler    = std::string(compiler);
        job.jobs        = opt_.jobs;

        if (const auto cfg = engine.configure(job); !cfg.ok()) {
            cell.status = Status::Failed;
            cell.note   = std::format("configure exited {} (see {})", cfg.exit_code,
                                      job.log_path.string());
            return cell;
        }

        // One untimed seed build. An incremental scenario is only incremental
        // against an up-to-date tree, and it warms the page cache so run 1 is
        // not systematically slower than the rest.
        if (const auto seed = engine.build(job); !seed.ok()) {
            cell.status = Status::Failed;
            cell.note   = std::format("seed build exited {} (see {})", seed.exit_code,
                                      job.log_path.string());
            return cell;
        }

        // edit-body rewrites a source file. In project mode that file belongs to
        // the user, so its exact bytes are captured first and restored no matter
        // how this function exits — including on a failed build.
        const SourceGuard guard(scenario == Scenario::EditBody ? inst.targets.body
                                                               : std::filesystem::path{});

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
                    cell.note   = std::format("re-configure exited {} on run {} (see {})",
                                              cfg.exit_code, i + 1, job.log_path.string());
                    return cell;
                }
                extra = cfg.wall_s;
            }

            const auto r = engine.build(job);
            if (!r.ok()) {
                cell.status = Status::Failed;
                cell.note   = std::format("build exited {} on run {} (see {})",
                                          r.exit_code, i + 1, job.log_path.string());
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
                return detail::edit_body(inst.targets.body, nonce);
        }
        return false;
    }
};

}  // namespace bench
