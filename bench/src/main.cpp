// bench — build-engine benchmark harness.
//
//   bench [--engines a,b] [--variants headers,modules,modules-impl]
//         [--scenarios cold,noop,...] [--profile release|debug]
//         [--compiler default|gcc|clang] [--preset NAME] [--units N] [--fanin N] [--weight N]
//         [--jobs N] [--runs N] [--work DIR] [--out FILE] [--list]
//
// Writes a protocol-versioned JSON report to --out (default bench-report.json)
// and a human summary to stdout. The two are separate on purpose: the JSON is
// what merges across machines, the summary is what a person reads.
import std;
import bench.protocol;
import bench.spec;
import bench.platform;
import bench.toolchain;
import bench.registry;
import bench.runner;
import bench.engines.engine;
import bench.fixture.generate;
import bench.analysis.ninjalog;
import bench.analysis.graph;
import bench.analysis.report;

namespace {

struct Options {
    std::vector<std::string> engines;
    std::vector<bench::Variant>  variants;
    std::vector<bench::Scenario> scenarios;
    std::string profile{"release"};
    std::string compiler{"default"};
    bench::fixture::Shape shape{};
    int  jobs{0};
    int  runs{0};
    std::filesystem::path work{"bench-work"};
    std::filesystem::path out{"bench-report.json"};
    std::filesystem::path analyze;   // profile an existing ninja build dir instead
    std::filesystem::path project;   // measure an existing tree instead of a fixture
    std::filesystem::path buildfiles;// foreign build descriptions for that tree
    std::filesystem::path hub, leaf, body;   // what the scenarios perturb there
    // The engine every ratio is expressed against. cmake is the default and not
    // an arbitrary one: it is the reference implementation of C++ module support
    // (P1689 scanning + dyndep are its design), it is present on every machine
    // this suite runs on, and it is what a reader already has a feel for. An
    // absolute second count means nothing without knowing the runner; "1.8x
    // cmake" survives being read on a different machine.
    //
    // Defaulting it rather than leaving it empty is deliberate: a run that
    // forgot the flag produced a table of bare seconds, which is the one form
    // of this data that cannot be compared to anything.
    std::string baseline{"cmake"};
    // One configure or build may take this long before the child is killed.
    // NOT unlimited by default: a hung engine used to consume the whole 120
    // minute CI budget and report nothing, because a cell only prints once it
    // is over. 30 minutes is well clear of a cold cmake build of mcpp on a
    // 4-core runner (~16 min measured) and still leaves room in the job.
    double timeout_s{1800.0};
    // Engines whose `failed` must not fail the RUN. Empty by default: a failure
    // is "the engine ran and did not produce the artifact", which is a finding,
    // and a suite that reports findings with exit 0 is the one that let 48 of 72
    // cells fail unnoticed. A genuine known gap goes here WITH its reason in
    // bench/matrix.json, so it stays visible instead of becoming invisible.
    std::vector<std::string> allow_failed;
    bool list{false};
};

// Does `engine` (a label like "mcpp@2026.8.13.1") match one of the names the
// caller marked as allowed-to-fail? Substring, like --baseline, so a versioned
// mcpp label is reachable by the bare name.
bool listed(const std::vector<std::string>& names, std::string_view engine) {
    return std::ranges::any_of(names, [&](const std::string& n) {
        return !n.empty() && engine.find(n) != std::string_view::npos;
    });
}

// Splits a comma-separated list, IGNORING commas inside `[...]`.
//
// An engine spec may carry bracketed options (`mcpp[schedule=on]=/path`), and a
// second option would be separated by a comma — which this function would
// otherwise cut in half, handing `make_engine` the fragments `mcpp[a=1` and
// `b=2]=/path` and reporting "unknown engine" for a perfectly valid spec. The
// list separator and the option separator are the same character, so the list
// splitter is the one that has to know about the brackets.
//
// Harmless for every other caller: `--variants`, `--scenarios` and
// `--allow-failed` contain no brackets, so the depth counter never leaves zero.
std::vector<std::string> split(std::string_view s, char sep = ',') {
    std::vector<std::string> parts;
    std::size_t start = 0, depth = 0;
    for (std::size_t i = 0; i <= s.size(); ++i) {
        if (i < s.size()) {
            if (s[i] == '[') { ++depth; continue; }
            if (s[i] == ']') { if (depth) --depth; continue; }
            if (s[i] != sep || depth) continue;
        }
        if (i > start) parts.emplace_back(s.substr(start, i - start));
        start = i + 1;
    }
    return parts;
}

void usage() {
    std::println("bench — build-engine benchmark harness");
    std::println("");
    std::println("  --engines LIST     mcpp,cmake,xmake,bazel                  (default: all)");
    std::println("  --variants LIST    headers,modules,modules-impl            (default: all)");
    std::println("  --scenarios LIST   cold,noop,touch-hub,touch-leaf,edit-body,edit-comment");
    std::println("  --profile NAME     release | debug                         (default: release)");
    std::println("  --compiler NAME    default | gcc | clang | /path/to/g++    (default: default)");
    std::println("                     payload:gcc / payload:clang — the driver out of MCPP'S OWN");
    std::println("                     registry, i.e. the one mcpp itself builds with. Use this to");
    std::println("                     compare engines rather than compilers; a host g++ that cannot");
    std::println("                     build modules makes every other engine look broken.");
    std::println("  --preset NAME      smoke | standard | large  — a NAMED size, so two runs on");
    std::println("                     two machines compare. standard is the default shape.");
    std::println("                       smoke     4 units  / fan-in 2 / weight 1   (~2s, CI)");
    std::println("                       standard 20 units  / fan-in 3 / weight 4   (~18s cold, mcpp)");
    std::println("                       large    60 units  / fan-in 3 / weight 6");
    std::println("  --units N          fixture translation units               (default: 20)");
    std::println("  --fanin N          dependencies per unit                   (default: 3)");
    std::println("  --weight N         distinct template blocks per unit       (default: 4)");
    std::println("  --jobs N           parallelism handed to each engine       (default: engine's)");
    std::println("  --runs N           repetitions per cell                    (default: per scenario)");
    std::println("  --work DIR         scratch directory                       (default: bench-work)");
    std::println("  --out FILE         JSON report path                        (default: bench-report.json)");
    std::println("  --baseline NAME    normalise the summary against this engine   (default: cmake)");
    std::println("                     Substring match on the label; \"\" disables the column.");
    std::println("  --timeout SEC      kill one configure/build after this long    (default: 1800, 0 = never)");
    std::println("  --allow-failed L   engines whose failure must not fail the run (default: none)");
    std::println("");
    std::println("EXIT STATUS: 0 only when something was measured and nothing failed. A `failed`");
    std::println("cell means the engine ran and produced no artifact — that is a finding, not a");
    std::println("gap, so it is reported with a non-zero status unless --allow-failed names it.");
    std::println("`unavailable` and `skipped` are gaps and never fail the run.");
    std::println("  --list             print engines and their availability, then exit");
    std::println("  --analyze DIR      profile an existing ninja build dir (work, makespan,");
    std::println("                     critical path, concurrency) instead of measuring");
    std::println("");
    std::println("Measuring a REAL project instead of a generated fixture:");
    std::println("  --project DIR      build this tree as-is (e.g. mcpp itself)");
    std::println("  --buildfiles DIR   where cmake/xmake read their description from, when the");
    std::println("                     project does not carry one (bench/projects/<name>/)");
    std::println("  --hub FILE         file with many dependents      (touch-hub)");
    std::println("  --leaf FILE        file with no dependents        (touch-leaf)");
    std::println("  --body FILE        file whose body gets edited    (edit-body)");
    std::println("");
    std::println("Comparing two mcpp builds is an engine spec, not a flag:");
    std::println("  --engines mcpp=/usr/bin/mcpp,mcpp=./target/.../bin/mcpp");
    std::println("  each labels itself from the version it reports, so rows stay distinct");
}

std::expected<Options, std::string> parse(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        auto value = [&](std::string_view name) -> std::expected<std::string, std::string> {
            if (i + 1 >= argc) return std::unexpected(std::format("{} needs a value", name));
            return std::string(argv[++i]);
        };
        auto take_int = [&](std::string_view name, int& dst) -> std::optional<std::string> {
            auto v = value(name);
            if (!v) return v.error();
            dst = std::atoi(v->c_str());
            return std::nullopt;
        };

        if (a == "--engines")        { auto v = value(a); if (!v) return std::unexpected(v.error()); o.engines = split(*v); }
        else if (a == "--profile")   { auto v = value(a); if (!v) return std::unexpected(v.error()); o.profile = *v; }
        else if (a == "--compiler")  { auto v = value(a); if (!v) return std::unexpected(v.error()); o.compiler = *v; }
        else if (a == "--work")      { auto v = value(a); if (!v) return std::unexpected(v.error()); o.work = *v; }
        else if (a == "--out")       { auto v = value(a); if (!v) return std::unexpected(v.error()); o.out = *v; }
        // Presets come FIRST so an explicit --units/--weight after one still
        // wins. A benchmark whose size is a free-form pair of numbers cannot be
        // compared between two people who ran it; a named size can.
        else if (a == "--preset") {
            if (i + 1 >= argc) return std::unexpected(std::string("--preset needs a value"));
            const std::string_view v = argv[++i];
            if      (v == "smoke")    o.shape = {4, 2, 1};
            else if (v == "standard") o.shape = {20, 3, 4};
            else if (v == "large")    o.shape = {60, 3, 6};
            else return std::unexpected(std::format(
                "unknown preset '{}' (smoke | standard | large)", v));
        }
        else if (a == "--units")     { if (auto e = take_int(a, o.shape.units))  return std::unexpected(*e); }
        else if (a == "--fanin")     { if (auto e = take_int(a, o.shape.fanin))  return std::unexpected(*e); }
        else if (a == "--weight")    { if (auto e = take_int(a, o.shape.weight)) return std::unexpected(*e); }
        else if (a == "--jobs")      { if (auto e = take_int(a, o.jobs))         return std::unexpected(*e); }
        else if (a == "--runs")      { if (auto e = take_int(a, o.runs))         return std::unexpected(*e); }
        else if (a == "--list")      { o.list = true; }
        else if (a == "--analyze")   { auto v = value(a); if (!v) return std::unexpected(v.error()); o.analyze = *v; }
        else if (a == "--project")   { auto v = value(a); if (!v) return std::unexpected(v.error()); o.project = *v; }
        else if (a == "--buildfiles"){ auto v = value(a); if (!v) return std::unexpected(v.error()); o.buildfiles = *v; }
        else if (a == "--hub")       { auto v = value(a); if (!v) return std::unexpected(v.error()); o.hub  = *v; }
        else if (a == "--leaf")      { auto v = value(a); if (!v) return std::unexpected(v.error()); o.leaf = *v; }
        else if (a == "--body")      { auto v = value(a); if (!v) return std::unexpected(v.error()); o.body = *v; }
        else if (a == "--baseline")  { auto v = value(a); if (!v) return std::unexpected(v.error()); o.baseline = *v; }
        else if (a == "--allow-failed") { auto v = value(a); if (!v) return std::unexpected(v.error()); o.allow_failed = split(*v); }
        else if (a == "--timeout") {
            auto v = value(a); if (!v) return std::unexpected(v.error());
            o.timeout_s = std::atof(v->c_str());
            if (o.timeout_s < 0.0) return std::unexpected(std::string("--timeout must not be negative"));
        }
        else if (a == "-h" || a == "--help") { return std::unexpected("help"); }
        else if (a == "--variants") {
            auto v = value(a); if (!v) return std::unexpected(v.error());
            for (const auto& name : split(*v)) {
                const auto parsed = bench::variant_from(name);
                if (!parsed) return std::unexpected(std::format("unknown variant '{}'", name));
                o.variants.push_back(*parsed);
            }
        } else if (a == "--scenarios") {
            auto v = value(a); if (!v) return std::unexpected(v.error());
            for (const auto& name : split(*v)) {
                const auto parsed = bench::scenario_from(name);
                if (!parsed) return std::unexpected(std::format("unknown scenario '{}'", name));
                o.scenarios.push_back(*parsed);
            }
        } else {
            return std::unexpected(std::format("unknown argument '{}'", a));
        }
    }
    if (o.variants.empty()) {
        // A real project has exactly one form — its own. Offering it the
        // headers/modules axis would generate over the tree being measured.
        o.variants = o.project.empty()
            ? std::vector{bench::Variant::Headers, bench::Variant::Modules,
                          bench::Variant::ModulesImpl}
            : std::vector{bench::Variant::Native};
    }
    if (o.scenarios.empty())
        // ALL of them. A scenario that is defined, documented and advertised in
        // --help but left out of this list runs only when someone names it
        // explicitly, which in practice is never — `touch-leaf` sat unmeasured
        // in every default run and every CI matrix cell for exactly that reason.
        o.scenarios = {bench::Scenario::Cold,      bench::Scenario::Noop,
                       bench::Scenario::TouchHub,  bench::Scenario::TouchLeaf,
                       bench::Scenario::EditBody,  bench::Scenario::EditComment};
    return o;
}

}  // namespace

int main(int argc, char** argv) {
    auto opts = parse(argc, argv);
    if (!opts) {
        if (opts.error() == "help") { usage(); return 0; }
        std::println(std::cerr, "bench: {}", opts.error());
        usage();
        return 2;
    }

    // Analysis mode short-circuits everything else: it reads a build that has
    // already happened rather than causing one.
    if (!opts->analyze.empty()) {
        auto log = bench::analysis::parse_ninja_log(opts->analyze / ".ninja_log");
        if (!log) {
            std::println(std::cerr, "bench: {}", log.error());
            return 1;
        }
        if (log->edges.empty()) {
            std::println(std::cerr, "bench: .ninja_log has no edges — was anything built?");
            return 1;
        }
        const auto graph = bench::analysis::build_graph(opts->analyze, *log);
        const auto a     = bench::analysis::analyze(*log, graph);
        const auto cores = bench::platform::host_facts().logical_cores;
        bench::analysis::print_report(a, *log, graph, opts->analyze.string(),
                                      static_cast<std::size_t>(cores));
        return 0;
    }

    // `payload:gcc` / `payload:clang` become a concrete path BEFORE anything
    // else looks at the value, so the engines, the generated manifest, the cell
    // key and the report all describe the same compiler. Resolving it later, or
    // per engine, is how they came apart the first time.
    if (opts->compiler.starts_with("payload:")) {
        const auto want = opts->compiler.substr(std::string_view("payload:").size());
        const auto r    = bench::toolchain::payload_cxx(want);
        if (r.driver.empty()) {
            // Hard error, not a fallback. Falling back to the host compiler is
            // precisely what produced a matrix in which 48 of 72 cells failed
            // while the job reported success.
            std::println(std::cerr, "bench: --compiler {} could not be resolved: {}",
                         opts->compiler, r.why);
            return 2;
        }
        std::println("payload: {} → {}", opts->compiler, r.driver.string());
        opts->compiler = r.driver.string();
    }

    const auto specs = opts->engines.empty() ? bench::default_engine_specs() : opts->engines;
    std::vector<std::unique_ptr<bench::engines::Engine>> engines;
    for (const auto& spec : specs) {
        auto e = bench::make_engine(spec);
        if (!e) {
            std::println(std::cerr, "bench: unknown engine '{}'", spec);
            return 2;
        }
        engines.push_back(std::move(e));
    }

    // Two binaries that report the SAME version produce the same label, and two
    // rows with one name is a table nobody can read — the old-vs-new comparison
    // silently stops being one the moment a branch forgets to bump its version.
    // Say so rather than printing it twice.
    {
        std::vector<std::string_view> seen;
        for (std::size_t i = 0; i < engines.size(); ++i) {
            const auto n = engines[i]->name();
            if (std::ranges::find(seen, n) != seen.end())
                std::println(std::cerr,
                             "bench: WARNING — engine #{} also calls itself '{}'. Two "
                             "binaries reporting one version cannot be told apart in "
                             "the report; bump one, or pass distinct labels.",
                             i + 1, n);
            seen.push_back(n);
        }
    }

    if (opts->list) {
        std::println("{:<10} {:<12} {}", "engine", "available", "note");
        for (const auto& e : engines) {
            const auto a = e->probe();
            std::println("{:<10} {:<12} {}", e->name(), a.present ? "yes" : "no", a.note);
        }
        return 0;
    }

    // A path pins the compiler; a label keeps the result readable. `--compiler
    // /long/path/to/g++` would otherwise put that path in every cell key.
    const std::string compiler_label = [&] {
        const auto& c = opts->compiler;
        if (c.empty() || c == "default") return std::string("default");
        if (c.find('/') == std::string::npos && c.find('\\') == std::string::npos) return c;
        const auto stem = std::filesystem::path(c).filename().string();
        if (stem.starts_with("g++") || stem.starts_with("gcc"))   return std::string("gcc");
        if (stem.starts_with("clang"))                            return std::string("clang");
        return stem;
    }();

    // A foreign build description for a REAL project cannot derive the tree from
    // its own location — it lives in bench/projects/<name>/ and the tree is
    // elsewhere — so the harness has to tell it. Exported for the whole run
    // because it is constant for the whole run.
    //
    // Without this the xlings arm could never run at all: its CMakeLists starts
    // with a FATAL_ERROR demanding XLINGS_ROOT, nothing set it, and every cell
    // was recorded as `configure exited 1`. Twelve cells per job, three jobs,
    // all green.
    std::optional<bench::platform::ScopedEnv> project_root_env;
    if (!opts->project.empty()) {
        std::error_code ec;
        auto abs = std::filesystem::absolute(opts->project, ec);
        project_root_env.emplace("BENCH_PROJECT_ROOT",
                                 (ec ? opts->project : abs).string());
    }

    const auto facts = bench::platform::host_facts();
    bench::Report report;
    report.host = bench::HostInfo{facts.os, facts.arch, facts.cpu_model,
                                  facts.logical_cores, facts.physical_cores,
                                  facts.heterogeneous, facts.ram_bytes, opts->compiler};
    report.started_at = bench::platform::iso_now();

    // Live progress. It goes to STDERR so that stdout stays the report and can
    // still be redirected on its own, and it is flushed on every line because
    // the whole point is to be readable WHILE the run is happening — a buffered
    // progress line arrives with the summary, which is exactly too late.
    const auto t0 = std::chrono::steady_clock::now();
    std::string current_cell;
    const auto elapsed = [&] {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    };

    bench::RunOptions ro;
    ro.work_root       = opts->work;
    ro.shape           = opts->shape;
    ro.jobs            = opts->jobs;
    ro.runs_override   = opts->runs;
    ro.compiler        = opts->compiler;
    ro.buildfiles      = opts->buildfiles;
    ro.project         = opts->project;
    // --hub/--leaf/--body are PROJECT-RELATIVE, and have to be resolved here.
    //
    // They used to be taken as given, which made them relative to the harness's
    // working directory instead. That is the same directory only when you are
    // benchmarking the tree you happen to be standing in — true for mcpp
    // measuring itself, false for every other project — and the failure is
    // silent: `exists()` says no, the cell reports `skipped`, and the run still
    // exits 0. An absolute path is left alone, so `--hub /tmp/x.cppm` still works.
    const auto in_project = [&](const std::filesystem::path& p) {
        if (p.empty() || p.is_absolute()) return p;
        return opts->project.empty() ? p : opts->project / p;
    };
    ro.project_targets = bench::fixture::Targets{in_project(opts->hub),
                                                in_project(opts->leaf),
                                                in_project(opts->body)};
    ro.timeout_s       = opts->timeout_s;
    ro.on_progress     = [&](std::string_view what) {
        bool first = true;
        for (const auto part : std::views::split(what, '\n')) {
            const std::string_view line(part.begin(), part.end());
            if (line.empty() && !first) continue;
            if (first) std::print(std::cerr, "[{:>7.1f}s] {}  {}\n", elapsed(), current_cell, line);
            else       std::print(std::cerr, "             | {}\n", line);
            first = false;
        }
        std::cerr.flush();
    };
    const bench::Runner runner(ro);

    const auto fixture_name = opts->project.empty()
        ? std::format("synth-{}x{}", opts->shape.units, opts->shape.fanin)
        : opts->project.filename().string();

    std::println("host   : {} {} · {} · {} logical / {} physical{}",
                 facts.os, facts.arch, facts.cpu_model, facts.logical_cores,
                 facts.physical_cores, facts.heterogeneous ? " (heterogeneous)" : "");
    if (opts->project.empty())
        std::println("fixture: {} units, fanin {}, weight {}",
                     opts->shape.units, opts->shape.fanin, opts->shape.weight);
    else
        std::println("project: {} (measured in place)", opts->project.string());
    std::println("");

    for (const auto& engine : engines) {
        for (const auto variant : opts->variants) {
            // Materialise once per (engine, variant): the scenarios of a pair
            // share a tree on purpose, since generation time belongs to none of
            // them. Cells that will not run skip the cost entirely.
            const bool will_run = engine->probe().present && engine->supports(variant, opts->compiler);
            std::optional<bench::Runner::Instance> inst;
            if (will_run) inst = runner.materialise(engine->name(), variant);

            for (const auto scenario : opts->scenarios) {
                bench::CellResult cell;
                if (will_run) {
                    current_cell = bench::CellKey{
                        std::string(engine->name()), compiler_label, opts->profile,
                        std::string(to_string(scenario)), fixture_name,
                        std::string(to_string(variant))}.str();
                    cell = runner.measure(*engine, *inst, variant, scenario, opts->profile,
                                          opts->compiler, compiler_label, fixture_name);
                } else {
                    cell.key = bench::CellKey{std::string(engine->name()), compiler_label,
                                              opts->profile, std::string(to_string(scenario)),
                                              fixture_name, std::string(to_string(variant))};
                    const auto a = engine->probe();
                    cell.status = bench::Status::Unavailable;
                    cell.note   = a.present ? engine->unsupported_reason(variant, opts->compiler) : a.note;
                }

                if (cell.status == bench::Status::Ok) {
                    std::println("{:<38} {:>8.2f}s  (min {:.2f} / max {:.2f}, n={})",
                                 cell.key.str(), cell.median_s(), cell.min_s(),
                                 cell.max_s(), cell.samples.size());
                } else {
                    std::println("{:<38} {:>9}  {}", cell.key.str(),
                                 bench::to_string(cell.status), cell.note);
                }
                report.cells.push_back(std::move(cell));
            }
        }
    }

    // Normalised summary. A column of raw seconds answers "how long"; a column of
    // ratios answers "compared to what", and the second question is the one a
    // build-engine comparison is actually asking.
    if (!opts->baseline.empty()) {
        std::println("");
        std::println("=== relative to {} (>1.00 = slower than the baseline) ===", opts->baseline);

        // Group by (variant, scenario): a ratio only means something against the
        // SAME source form and the SAME perturbation.
        std::vector<std::pair<std::string, std::string>> groups;
        for (const auto& c : report.cells) {
            auto key = std::pair{c.key.variant, c.key.scenario};
            if (std::ranges::find(groups, key) == groups.end()) groups.push_back(key);
        }

        for (const auto& [variant, scenario] : groups) {
            const bench::CellResult* base = nullptr;
            for (const auto& c : report.cells)
                if (c.key.variant == variant && c.key.scenario == scenario
                    && c.key.engine.find(opts->baseline) != std::string::npos
                    && c.status == bench::Status::Ok)
                    base = &c;

            std::println("");
            std::println("-- {} / {} --", variant, scenario);
            if (!base) {
                // Saying "no baseline" beats printing ratios against nothing, and
                // beats silently omitting the group.
                std::println("   (no successful '{}' cell here; ratios omitted)", opts->baseline);
            }
            for (const auto& c : report.cells) {
                if (c.key.variant != variant || c.key.scenario != scenario) continue;
                if (c.status != bench::Status::Ok) {
                    std::println("   {:<22} {:>9}  {}", c.key.engine,
                                 bench::to_string(c.status), c.note);
                    continue;
                }
                if (base && base->median_s() > 0.0)
                    std::println("   {:<22} {:>8.2f}s  {:>6.2f}x{}", c.key.engine,
                                 c.median_s(), c.median_s() / base->median_s(),
                                 (&c == base) ? "   <- baseline" : "");
                else
                    std::println("   {:<22} {:>8.2f}s", c.key.engine, c.median_s());
            }
        }
    }

    std::ofstream out(opts->out, std::ios::binary | std::ios::trunc);
    out << bench::to_json(report);
    std::println("");
    std::println("report : {}", opts->out.string());

    // --- exit status -------------------------------------------------------
    //
    // A benchmark that cannot assert on TIMINGS (shared runners, changing CPU
    // models) can still assert that it MEASURED SOMETHING. Not doing so cost
    // this suite weeks: a matrix job in which 48 of 72 cells failed, 18 were
    // unavailable and the only 6 that ran were one engine on one variant,
    // reported success — as did an xlings job whose every single cell was
    // skipped because --hub named a file that no longer existed.
    //
    // `failed` is the finding: the engine ran and produced no artifact.
    // `unavailable` and `skipped` are gaps, are documented in the note, and
    // never fail the run.
    // --- internal consistency: a `cold` build must out-work its own `noop` ---
    //
    // NOT a performance threshold. The suite deliberately has none, because a
    // shared runner's variance would turn into red crosses people mute. This is
    // an INVARIANT: `cold` removes the build directory and rebuilds everything,
    // `noop` does nothing, so a `cold` in the same league as its own engine's
    // `noop` did not rebuild — the engine's clean() missed where that engine
    // actually keeps its artifacts.
    //
    // It is worth a check because the failure is invisible: the cell is `ok`,
    // it carries samples, and it reports a spectacular number. xmake on the
    // pinned mcpp workload produced `cold 0.60s` next to `touch-hub 82.79s`,
    // and nothing in the report said the first of those was not a build.
    //
    // 2x is the same floor README §4a R1 uses for "this is measuring process
    // startup". For every other scenario that is a caveat a reader applies; for
    // `cold` it is a defect.
    std::size_t suspect = 0;
    for (const auto& c : report.cells) {
        if (c.status != bench::Status::Ok || c.key.scenario != "cold") continue;
        const bench::CellResult* noop = nullptr;
        for (const auto& n : report.cells)
            if (n.status == bench::Status::Ok && n.key.scenario == "noop"
                && n.key.engine == c.key.engine && n.key.variant == c.key.variant)
                noop = &n;
        if (!noop || noop->median_s() <= 0.0) continue;
        if (c.median_s() < noop->median_s() * 2.0) {
            ++suspect;
            std::println(std::cerr,
                         "bench: {} reports cold={:.2f}s against its own noop={:.2f}s — a cold "
                         "build cannot be that cheap, so clean() did not remove this engine's "
                         "artifacts and the cell measured an up-to-date tree.",
                         c.key.str(), c.median_s(), noop->median_s());
        }
    }

    std::size_t ok = 0, failed = 0, waived = 0;
    for (const auto& c : report.cells) {
        if (c.status == bench::Status::Ok) { ++ok; continue; }
        if (c.status != bench::Status::Failed) continue;
        if (listed(opts->allow_failed, c.key.engine)) ++waived; else ++failed;
    }
    std::println("cells  : {} ok, {} failed{}, {} not applicable", ok, failed,
                 waived ? std::format(" ({} waived by --allow-failed)", waived) : "",
                 report.cells.size() - ok - failed - waived);

    if (failed) {
        std::println(std::cerr,
                     "bench: {} cell(s) FAILED — the engine ran and produced no artifact. "
                     "Each one's reason and log tail are above.", failed);
        return 1;
    }
    if (suspect) {
        std::println(std::cerr,
                     "bench: {} `cold` cell(s) did not actually rebuild (see above). Those "
                     "numbers are not measurements of a cold build.", suspect);
        return 1;
    }
    if (ok == 0) {
        std::println(std::cerr,
                     "bench: nothing was measured. Every cell was unavailable or skipped, "
                     "so this run contains no data; see each cell's note for why.");
        return 1;
    }
    return 0;
}
