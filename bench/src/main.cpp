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
    std::filesystem::path hub, leaf, body;   // what the scenarios perturb there
    std::string baseline;                    // engine to normalise the summary against
    bool list{false};
};

std::vector<std::string> split(std::string_view s, char sep = ',') {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= s.size()) {
        const auto pos = s.find(sep, start);
        const auto end = (pos == std::string_view::npos) ? s.size() : pos;
        if (end > start) parts.emplace_back(s.substr(start, end - start));
        if (pos == std::string_view::npos) break;
        start = pos + 1;
    }
    return parts;
}

void usage() {
    std::println("bench — build-engine benchmark harness");
    std::println("");
    std::println("  --engines LIST     mcpp,cmake,xmake,meson,bazel            (default: all)");
    std::println("  --variants LIST    headers,modules,modules-impl            (default: all)");
    std::println("  --scenarios LIST   cold,noop,touch-hub,touch-leaf,edit-body,edit-comment");
    std::println("  --profile NAME     release | debug                         (default: release)");
    std::println("  --compiler NAME    default | gcc | clang                   (default: default)");
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
    std::println("  --baseline NAME    add a normalised column to the summary, relative to this");
    std::println("                     engine (e.g. --baseline cmake). Substring match on the label.");
    std::println("  --list             print engines and their availability, then exit");
    std::println("  --analyze DIR      profile an existing ninja build dir (work, makespan,");
    std::println("                     critical path, concurrency) instead of measuring");
    std::println("");
    std::println("Measuring a REAL project instead of a generated fixture:");
    std::println("  --project DIR      build this tree as-is (e.g. mcpp itself)");
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
        else if (a == "--hub")       { auto v = value(a); if (!v) return std::unexpected(v.error()); o.hub  = *v; }
        else if (a == "--leaf")      { auto v = value(a); if (!v) return std::unexpected(v.error()); o.leaf = *v; }
        else if (a == "--body")      { auto v = value(a); if (!v) return std::unexpected(v.error()); o.body = *v; }
        else if (a == "--baseline")  { auto v = value(a); if (!v) return std::unexpected(v.error()); o.baseline = *v; }
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

    const auto facts = bench::platform::host_facts();
    bench::Report report;
    report.host = bench::HostInfo{facts.os, facts.arch, facts.cpu_model,
                                  facts.logical_cores, facts.physical_cores,
                                  facts.heterogeneous, facts.ram_bytes, opts->compiler};
    report.started_at = bench::platform::iso_now();

    bench::RunOptions ro;
    ro.work_root       = opts->work;
    ro.shape           = opts->shape;
    ro.jobs            = opts->jobs;
    ro.runs_override   = opts->runs;
    ro.compiler        = opts->compiler;
    ro.project         = opts->project;
    ro.project_targets = bench::fixture::Targets{opts->hub, opts->leaf, opts->body};
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
    return 0;
}
