import std;
import mcpplibs.mtest;
import mcpp.build.test_schedule;

using namespace mcpplibs::mtest;

// The order, the division and the shards of a `mcpp test` run read the run
// store (docs/08 "The order of a run"): every shard computes one assignment
// from one store and one list, so the shards together run each test once.

namespace fs = std::filesystem;
namespace ts = mcpp::build::test_schedule;

namespace {
struct Store {
    fs::path dir;
    Store() {
        dir = fs::temp_directory_path() / std::format("mcpp-schedule-{:08x}", std::random_device{}());
        fs::create_directories(dir);
    }
    ~Store() { std::error_code ec; fs::remove_all(dir, ec); }
    void run(std::string_view id, std::string_view started, std::string_view programs, std::string_view cases) {
        fs::create_directories(dir / "runs" / std::string(id));
        std::ofstream(dir / "runs" / std::string(id) / "programs.ndjson", std::ios::binary) << programs;
        std::ofstream(dir / "runs" / std::string(id) / "cases.ndjson", std::ios::binary) << cases;
        std::ofstream(dir / "index.ndjson", std::ios::binary | std::ios::app)
            << std::format(R"({{"id":"{}","started":"{}","schema":"mtest.run/1"}})", id, started) << "\n";
    }
};
}  // namespace

UTest the_newest_measurement_is_the_estimate{"TestSchedule/TheNewestMeasurementIsTheEstimate", [] {
    Store s;
    s.run("a", "2026-11-01T00:00:00Z", R"({"name":"p","status":"pass","ms":100})" "\n",
          R"({"program":"p","id":"c1","status":"pass","ms":40})" "\n");
    s.run("b", "2026-11-02T00:00:00Z", R"({"name":"p","status":"pass","ms":300})" "\n"
                                       R"({"name":"q","status":"built","ms":1})" "\n",
          R"({"program":"p","id":"c1","status":"pass","ms":90})" "\n");
    const auto h = ts::read_history(s.dir);
    expect(eq(h.runs, std::size_t{2}));
    expect(eq(ts::estimate(h, "p", {}), 300.0));
    // A test built and not run measured nothing: the mean of the others.
    expect(eq(ts::estimate(h, "q", {}), 300.0));
    const std::vector<std::string> ids{"c1", "unmeasured"};
    expect(eq(ts::estimate(h, "", ids), 90.0 + 90.0));
}};

UTest a_shard_reads_no_run_of_its_own_group{"TestSchedule/AShardReadsNoRunOfItsOwnGroup", [] {
    // The shards of one run, run one after another on one store, read it as
    // the first read it; the runs of another group (main's) are its history.
    Store s;
    s.run("main", "2026-11-01T00:00:00Z", R"({"name":"p","status":"pass","ms":100})" "\n", "");
    fs::create_directories(s.dir / "runs" / "shard1");
    std::ofstream(s.dir / "runs" / "shard1" / "programs.ndjson") << R"({"name":"p","status":"pass","ms":900})" "\n";
    std::ofstream(s.dir / "index.ndjson", std::ios::app)
        << R"({"id":"shard1","started":"2026-11-02T00:00:00Z","group":"7","shard":"1/2"})" "\n";
    expect(eq(ts::estimate(ts::read_history(s.dir), "p", {}), 900.0));
    expect(eq(ts::estimate(ts::read_history(s.dir, 30, std::string("7")), "p", {}), 100.0));
    expect(eq(ts::estimate(ts::read_history(s.dir, 30, std::string("8")), "p", {}), 900.0));
}};

UTest the_shards_cover_every_test_once{"TestSchedule/TheShardsCoverEveryTestOnce", [] {
    const std::vector<double> est{50, 400, 10, 400, 90, 5, 300};
    const std::vector<std::string> names{"a", "b", "c", "d", "e", "f", "g"};
    const auto bin = ts::assign(est, names, 3);
    std::vector<double> load(3, 0);
    for (std::size_t i = 0; i < est.size(); ++i) load[bin[i]] += est[i];
    // Longest first, each to the least loaded: 400+10 | 400+5 | 300+90+50.
    expect(eq(load[0], 410.0));
    expect(eq(load[1], 405.0));
    expect(eq(load[2], 440.0));
    // The same list gives the same assignment.
    expect(ts::assign(est, names, 3) == bin);
}};

UTest a_long_part_divides_into_equal_groups{"TestSchedule/ALongPartDividesIntoEqualGroups", [] {
    Store s;
    s.run("a", "2026-11-01T00:00:00Z", "",
          R"({"program":"p","id":"x","status":"pass","ms":800})" "\n"
          R"({"program":"p","id":"y","status":"pass","ms":500})" "\n"
          R"({"program":"p","id":"z","status":"pass","ms":300})" "\n");
    const auto h = ts::read_history(s.dir);
    const std::vector<std::string> ids{"x", "y", "z"};
    const auto groups = ts::divide(h, ids, 2);
    expect(eq(groups.size(), std::size_t{2}));
    expect(groups[0] == std::vector<std::string>{"x"});
    // Each group keeps the order the cases were listed in.
    expect(groups[1] == std::vector<std::string>{"y", "z"});
    expect(eq(ts::divide(h, ids, 9).size(), std::size_t{3}));
}};

UTest a_shard_is_i_of_n{"TestSchedule/AShardIsIOfN", [] {
    const auto shard = ts::parse_shard("2/4");
    expect(fatal(shard.has_value()));
    expect(eq(shard->first, 2));
    expect(eq(shard->second, 4));
    expect(!ts::parse_shard("0/4"));
    expect(!ts::parse_shard("5/4"));
    expect(!ts::parse_shard("2"));
    expect(!ts::parse_shard("a/b"));
}};

UTest the_failures_of_the_newest_run{"TestSchedule/TheFailuresOfTheNewestRun", [] {
    Store s;
    s.run("old", "2026-11-01T00:00:00Z", R"({"name":"gone","status":"fail","ms":1})" "\n", "");
    s.run("new", "2026-11-02T00:00:00Z",
          R"({"name":"e2e/cli/output","part_of":"e2e/cli","status":"fail","ms":1})" "\n"
          R"({"name":"unit/a","status":"pass","ms":1})" "\n",
          R"({"program":"e2e/cli/output","id":"842","status":"fail","ms":1})" "\n"
          R"({"program":"e2e/cli/output","id":"843","status":"pass","ms":1})" "\n");
    auto f = ts::last_failures(s.dir);
    expect(fatal(f.has_value()));
    expect(eq(f->run, std::string("new")));
    expect(f->programs == std::vector<std::string>{"e2e/cli"});
    expect(f->cases == std::vector<std::string>{"842"});
    expect(!f->whole);
    // A program that failed outside its cases runs whole.
    s.run("newer", "2026-11-03T00:00:00Z", R"({"name":"unit/b","status":"fail","ms":1})" "\n", "");
    f = ts::last_failures(s.dir);
    expect(fatal(f.has_value()));
    expect(f->programs == std::vector<std::string>{"unit/b"});
    expect(f->whole);
    expect(!ts::last_failures(s.dir / "none"));
}};
