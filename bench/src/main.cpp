// bench — build-engine benchmark harness.
//
//   bench [--engines a,b] [--variants headers,modules,modules-impl]
//         [--scenarios cold,noop,...] [--profile release|debug]
//         [--compiler default|gcc|clang] [--units N] [--fanin N] [--weight N]
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
    std::println("  --engines LIST     mcpp,mcpp-opt,cmake,xmake,meson,bazel   (default: all)");
    std::println("  --variants LIST    headers,modules,modules-impl            (default: all)");
    std::println("  --scenarios LIST   cold,noop,touch-hub,edit-body,touch-leaf");
    std::println("  --profile NAME     release | debug                         (default: release)");
    std::println("  --compiler NAME    default | gcc | clang                   (default: default)");
    std::println("  --units N          fixture translation units               (default: 40)");
    std::println("  --fanin N          dependencies per unit                   (default: 3)");
    std::println("  --weight N         template instantiations per unit        (default: 6)");
    std::println("  --jobs N           parallelism handed to each engine       (default: engine's)");
    std::println("  --runs N           repetitions per cell                    (default: per scenario)");
    std::println("  --work DIR         scratch directory                       (default: bench-work)");
    std::println("  --out FILE         JSON report path                        (default: bench-report.json)");
    std::println("  --list             print engines and their availability, then exit");
    std::println("  --analyze DIR      profile an existing ninja build dir (work, makespan,");
    std::println("                     critical path, concurrency) instead of measuring");
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
        else if (a == "--units")     { if (auto e = take_int(a, o.shape.units))  return std::unexpected(*e); }
        else if (a == "--fanin")     { if (auto e = take_int(a, o.shape.fanin))  return std::unexpected(*e); }
        else if (a == "--weight")    { if (auto e = take_int(a, o.shape.weight)) return std::unexpected(*e); }
        else if (a == "--jobs")      { if (auto e = take_int(a, o.jobs))         return std::unexpected(*e); }
        else if (a == "--runs")      { if (auto e = take_int(a, o.runs))         return std::unexpected(*e); }
        else if (a == "--list")      { o.list = true; }
        else if (a == "--analyze")   { auto v = value(a); if (!v) return std::unexpected(v.error()); o.analyze = *v; }
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
    if (o.variants.empty())
        o.variants = {bench::Variant::Headers, bench::Variant::Modules,
                      bench::Variant::ModulesImpl};
    if (o.scenarios.empty())
        o.scenarios = {bench::Scenario::Cold, bench::Scenario::Noop,
                       bench::Scenario::TouchHub, bench::Scenario::EditBody};
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

    auto engines = bench::all_engines();
    if (!opts->engines.empty()) {
        std::erase_if(engines, [&](const auto& e) {
            return std::ranges::find(opts->engines, std::string(e->name())) == opts->engines.end();
        });
        if (engines.empty()) {
            std::println(std::cerr, "bench: no engine matched --engines");
            return 2;
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

    const auto facts = bench::platform::host_facts();
    bench::Report report;
    report.host = bench::HostInfo{facts.os, facts.arch, facts.cpu_model,
                                  facts.logical_cores, facts.physical_cores,
                                  facts.heterogeneous, facts.ram_bytes, opts->compiler};
    report.started_at = bench::platform::iso_now();

    bench::RunOptions ro;
    ro.work_root     = opts->work;
    ro.shape         = opts->shape;
    ro.jobs          = opts->jobs;
    ro.runs_override = opts->runs;
    const bench::Runner runner(ro);

    const auto fixture_name = std::format("synth-{}x{}", opts->shape.units, opts->shape.fanin);

    std::println("host   : {} {} · {} · {} logical / {} physical{}",
                 facts.os, facts.arch, facts.cpu_model, facts.logical_cores,
                 facts.physical_cores, facts.heterogeneous ? " (heterogeneous)" : "");
    std::println("fixture: {} units, fanin {}, weight {}",
                 opts->shape.units, opts->shape.fanin, opts->shape.weight);
    std::println("");

    for (const auto& engine : engines) {
        for (const auto variant : opts->variants) {
            // Materialise once per (engine, variant): the scenarios of a pair
            // share a tree on purpose, since generation time belongs to none of
            // them. Cells that will not run skip the cost entirely.
            const bool will_run = engine->probe().present && engine->supports(variant);
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
                    cell.note   = a.present ? engine->unsupported_reason(variant) : a.note;
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

    std::ofstream out(opts->out, std::ios::binary | std::ios::trunc);
    out << bench::to_json(report);
    std::println("");
    std::println("report : {}", opts->out.string());
    return 0;
}
