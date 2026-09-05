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
import bench.journal;
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
    // Names this run, and therefore its journal. Empty → derived from
    // os-toolchain-project, which is how the reports are already named.
    std::string           run_id;
    // What is under test, in the caller's words (mcpp's commit). Recorded, never
    // fingerprinted: folding it in would restart every resume on every rebuild.
    std::string           under_test;
    // Where fingerprinted run caches live, like a build directory.
    std::filesystem::path cache_root{".mbench"};
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
    std::println("  --id NAME          one more field in the run's fingerprint — use it to keep");
    std::println("                       two otherwise-identical runs apart");
    std::println("  --cache-root DIR   where fingerprinted runs are cached      (default: .mbench)");
    std::println("  --under-test TEXT  what is being measured, in your words — for mcpp the");
    std::println("                     commit, because its version is a DATE and every commit");
    std::println("                     on a branch reports the same one. Recorded in the report");
    std::println("                     and compared on resume; never part of the fingerprint.");
    std::println("");
    std::println("  A run is fingerprinted over its WHOLE configuration (engines, variants,");
    std::println("  scenarios, runs, compiler, profile, project, shape, --id) and cached in");
    std::println("  <cache-root>/<fingerprint>/. Re-running the same configuration RESUMES from");
    std::println("  what is already recorded there, one measured unit at a time; changing any of");
    std::println("  it lands in a different directory instead of overwriting. Delete the");
    std::println("  directory to start that configuration over.");
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
        else if (a == "--cache-root") { auto v = value(a); if (!v) return std::unexpected(v.error()); o.cache_root = *v; }
        else if (a == "--id")        { auto v = value(a); if (!v) return std::unexpected(v.error()); o.run_id = *v; }
        else if (a == "--under-test") { auto v = value(a); if (!v) return std::unexpected(v.error()); o.under_test = *v; }
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
        // ABSOLUTE, for the same reason as --buildfiles below: `build_dir` is
        // derived from this (`<project>/build`) and handed to xmake as `-o`,
        // which resolves a relative path against ITS cwd — the buildfile dir —
        // while clean() resolves the same string against the harness's cwd.
        // The two then name different directories: xmake wrote into
        //     bench/projects/xlings/bench/projects/xlings/<tree>/build
        // and clean() removed a path nothing had written to, so every `cold`
        // measured an up-to-date tree (0.76s against cmake's 103s). The
        // cold-vs-noop and cold-vs-peers invariants are what caught it.
        else if (a == "--project")   {
            auto v = value(a); if (!v) return std::unexpected(v.error());
            std::error_code ec;
            auto abs = std::filesystem::absolute(*v, ec);
            o.project = ec ? std::filesystem::path(*v) : abs;
        }
        // ABSOLUTE, resolved against the cwd the harness was STARTED in.
        //
        // Every engine is spawned with its cwd set to this directory, and xmake
        // is then handed it again as `-P`. A relative path therefore resolves
        // twice: `--buildfiles bench/projects/xlings` became
        // `bench/projects/xlings/bench/projects/xlings` and the whole arm failed
        // with `error: project not found!` — while the identical command run by
        // hand from the repository root worked, because there the doubling had
        // nothing to double against. Same shape as the `--buildir` doubling this
        // suite already fixed once; the fix belongs here, once, rather than in
        // each adapter.
        else if (a == "--buildfiles"){
            auto v = value(a); if (!v) return std::unexpected(v.error());
            std::error_code ec;
            auto abs = std::filesystem::absolute(*v, ec);
            o.buildfiles = ec ? std::filesystem::path(*v) : abs;
        }
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
    report.under_test = opts->under_test;

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
    // Defined here rather than after the runner: the journal keys on it.
    const auto fixture_name = opts->project.empty()
        ? std::format("synth-{}x{}", opts->shape.units, opts->shape.fanin)
        : opts->project.filename().string();
    const std::string fixture_name_for_key = fixture_name;
    std::size_t units_done = 0;

    // ── The journal, and what makes its records usable ────────────────────
    //
    // Beside the report, so a run's data and its record of how far it got stay
    // together. Every measured unit is appended the moment it exists.
    // ── The run's identity, and where its cache lives ─────────────────────
    //
    // One fingerprint over the whole configuration, the same way `mcpp build`
    // works: same configuration → same directory → resume; different
    // configuration → a different directory, instead of overwriting.
    // `--id` is just one more field in the hash.
    bench::RunId id;
    {
        std::string cfg = std::format(
            "id={};profile={};runs={};compiler={};project={};buildfiles={};"
            "units={};fanin={};weight={}",
            opts->run_id, opts->profile, opts->runs, opts->compiler,
            opts->project.string(), opts->buildfiles.string(),
            opts->shape.units, opts->shape.fanin, opts->shape.weight);
        for (const auto& e : engines) { cfg += ";e="; cfg += e->name(); }
        for (const auto v : opts->variants)   { cfg += ";v="; cfg += to_string(v); }
        for (const auto sc : opts->scenarios) { cfg += ";s="; cfg += to_string(sc); }
        id = bench::RunId::of(std::move(cfg));
    }

    // `.mbench/<fingerprint>/` beside the working directory, like a build cache.
    const auto cache_dir = opts->cache_root / id.fingerprint;
    std::error_code cache_ec;
    std::filesystem::create_directories(cache_dir, cache_ec);
    {
        // The configuration in full, beside the journal: a fingerprint nobody
        // can decode is a fingerprint nobody trusts.
        std::ofstream cfg_out(cache_dir / "config.txt", std::ios::trunc);
        if (cfg_out) cfg_out << id.config << '\n';
    }

    const bench::Journal journal(cache_dir / "journal.jsonl");
    auto loaded = journal.load(id.str());

    // A RESUME MUST NOT SILENTLY SPAN TWO BUILDS OF WHAT IS UNDER TEST.
    //
    // The fingerprint deliberately excludes the binary, so that rebuilding does
    // not throw the cache away — and that is exactly what makes this possible:
    // resume, rebuild, resume again, and the report carries samples from two
    // different binaries under one heading. `engine_version` catches a version
    // change, but mcpp's version is a DATE, so every commit on a branch reports
    // the same one and that check sees nothing.
    //
    // Recorded here rather than fingerprinted, and REPORTED on mismatch: the
    // caller is entitled to resume across a rebuild (that is the normal case
    // while developing), but not to be unaware that they did.
    {
        const auto stamp = cache_dir / "under-test.txt";
        std::string previous;
        if (std::ifstream in(stamp); in) std::getline(in, previous);
        if (!previous.empty() && !opts->under_test.empty() && previous != opts->under_test)
            std::println(std::cerr,
                         "bench: this cache holds samples measured with '{}' but this run is "
                         "'{}'. The report will mix them. Use --id to fork a fresh cache, or "
                         "delete {}.",
                         previous, opts->under_test, cache_dir.string());
        if (!opts->under_test.empty() && previous != opts->under_test) {
            std::ofstream out(stamp, std::ios::trunc);
            if (out) out << opts->under_test << '\n';
        }
    }

    if (loaded.skipped_other_id)
        std::println(std::cerr,
                     "bench: {} journal entries carry a different fingerprint and are ignored "
                     "(this run {}, theirs {}).",
                     loaded.skipped_other_id, id.str(), loaded.other_id);
    if (loaded.skipped_unparsable)
        std::println(std::cerr,
                     "bench: {} journal line(s) unreadable and skipped — the last line of a "
                     "killed run is expected to be half-written.", loaded.skipped_unparsable);
    std::println("run id : {} ({})", id.fingerprint,
                 loaded.units.empty() ? "fresh"
                                      : std::format("resuming, {} unit(s) recorded",
                                                    loaded.units.size()));

    ro.already_done = [&](std::string_view sc, std::string_view en,
                          std::string_view va, int run) {
        return loaded.units.contains(
            bench::Journal::unit_id(fixture_name_for_key, va, sc, en, run));
    };
    ro.recorded_sample = [&](std::string_view sc, std::string_view en,
                             std::string_view va, int run) {
        const auto it = loaded.units.find(
            bench::Journal::unit_id(fixture_name_for_key, va, sc, en, run));
        if (it == loaded.units.end()) return std::pair<double, int>{0.0, 0};
        // The binary is NOT part of the fingerprint — a rebuild must not throw
        // the cache away — so adopting a record measured with a different one is
        // reported instead. This is the one way a resume can mix two binaries.
        for (const auto& e : engines) {
            if (e->name() != en) continue;
            const auto now = e->probe().note;
            if (!it->second.engine_version.empty() && it->second.engine_version != now)
                std::println(std::cerr,
                             "bench: adopting a {} record measured with '{}', but it now "
                             "reports '{}' — both are in this table.",
                             en, it->second.engine_version, now);
            break;
        }
        return std::pair<double, int>{it->second.wall_s, it->second.exit_code};
    };
    ro.record = [&](std::string_view sc, std::string_view en, std::string_view va,
                    int run, double wall_s, int exit_code) {
        std::string ver;
        for (const auto& e : engines) if (e->name() == en) { ver = e->probe().note; break; }
        journal.append(bench::JournalEntry{id.str(), ver, fixture_name_for_key,
                                           std::string(va), std::string(sc),
                                           std::string(en), run, wall_s, exit_code});
    };

    const bench::Runner runner(ro);


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

    // --- cross-engine consistency: a cold build cannot be 20x cheaper than every
    //     other engine building the same sources ---
    //
    // The check above is RELATIVE TO ONE ENGINE, so it is blind to the case that
    // actually shipped: an engine that builds NOTHING has a cheap cold AND a
    // cheap noop, and their ratio looks healthy. bazel on the pinned mcpp tree
    // reported cold=0.43s / noop=0.22s — a ratio of 1.95, missing the 2x trip
    // wire by one hundredth of a second — while compiling zero of 137 units.
    //
    // Peers are the honest yardstick here, and they are already in the report:
    // engines in the same cell compile the same sources on the same machine, so
    // a 20x gap is not a fast engine, it is a different workload. The factor is
    // deliberately far past any real result (mcpp's best measured win over cmake
    // is 3.1x) so this fires on phantoms and never on a good number.
    for (const auto& c : report.cells) {
        if (c.status != bench::Status::Ok || c.key.scenario != "cold") continue;
        std::vector<double> peers;
        for (const auto& p : report.cells)
            if (p.status == bench::Status::Ok && p.key.scenario == "cold"
                && p.key.fixture == c.key.fixture && p.key.variant == c.key.variant
                && p.key.engine != c.key.engine && p.median_s() > 0.0)
                peers.push_back(p.median_s());
        if (peers.empty() || c.median_s() <= 0.0) continue;
        std::ranges::sort(peers);
        const double peer_median = peers[peers.size() / 2];
        if (c.median_s() * 20.0 < peer_median) {
            ++suspect;
            std::println(std::cerr,
                         "bench: {} reports cold={:.2f}s while other engines building the same "
                         "sources take {:.2f}s — {:.0f}x is not a faster engine, it is a smaller "
                         "workload; check that this engine's description actually names the sources.",
                         c.key.str(), c.median_s(), peer_median, peer_median / c.median_s());
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

    // ── A WAIVER MAY HIDE A FAILURE. IT MUST NOT HIDE THE COMPARISON. ───────
    //
    // `--allow-failed` exists so ONE arm with a documented gap does not turn a
    // whole matrix red. It was never meant to cover every foreign engine at
    // once — but nothing stopped it, and the result is a job that exits 0 while
    // its log reads
    //
    //     => install ftxui v6.1.9 .. failed
    //     => install mcpplibs-tinyhttps 0.2.9 .. failed
    //     error: <cmdline> missing std dependency for module ...
    //     cells  : 10 ok, 0 failed (10 waived by --allow-failed)
    //
    // A cell that calls itself a three-engine comparison, measures mcpp against
    // mcpp, and reports green. That is the exact shape this suite exists to
    // remove, so it may not be the suite's own.
    //
    // Named per ENGINE rather than counted: "10 waived" is a number, "cmake and
    // xmake produced nothing here" is the fact a reader needs.
    {
        std::map<std::string, std::pair<std::size_t, std::size_t>> per_engine;  // ok, waived
        for (const auto& c : report.cells) {
            auto& e = per_engine[c.key.engine];
            if (c.status == bench::Status::Ok) ++e.first;
            else if (c.status == bench::Status::Failed &&
                     listed(opts->allow_failed, c.key.engine)) ++e.second;
        }
        std::vector<std::string> silenced;
        for (const auto& [engine, counts] : per_engine)
            if (counts.first == 0 && counts.second > 0) silenced.push_back(engine);
        if (!silenced.empty()) {
            std::string list;
            for (const auto& e : silenced) { if (!list.empty()) list += ", "; list += e; }
            std::println(std::cerr,
                         "bench: WAIVED AWAY ENTIRELY: {} — every cell of {} failed and was "
                         "waived, so this run contains no measurement of {} at all. The run "
                         "is green by policy, not because those engines worked; whatever this "
                         "report is compared against, it is not them.",
                         list, silenced.size() == 1 ? "it" : "them",
                         silenced.size() == 1 ? "it" : "them");
        }
    }

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
