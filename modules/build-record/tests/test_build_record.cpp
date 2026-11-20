import std;
import mcpplibs.mtest;
import mcpp.build.record;

using namespace mcpplibs::mtest;

// SUBSYSTEM-LEVEL: the line the compiler executor appends, the record a build writes from it
// (SPEC-017 B4 `builds.ndjson`), and the questions asked of the record. That a build writes it,
// and `mcpp test` keeps it in its run, is stated by the e2e suite (tests/e2e/test/streams.cpp).

namespace rec = mcpp::build::record;

UTest a_usage_line_reads_back{"BuildRecord/AUsageLineReadsBack", [] {
    const rec::Usage u{"obj/a.o", 100, 350, 240.5, 1u << 20};
    const auto text = rec::usage_line(u) + "not json\n" + rec::usage_line({"obj/b.o", 1, 2}) + "{\"output\":";
    const auto back = rec::parse_usage(text);
    expect(fatal(eq(back.size(), std::size_t{2})));
    expect(eq(back[0].output, std::string("obj/a.o")));
    expect(eq(back[0].endedUnixMs - back[0].startedUnixMs, 250LL));
    expect(back[0].cpuMs == std::optional<double>(240.5));
    expect(back[0].peakBytes == std::optional<std::uint64_t>(1u << 20));
    expect(!back[1].cpuMs && !back[1].peakBytes);
}};

UTest an_edge_takes_its_measurement_by_its_output{"BuildRecord/AnEdgeTakesItsMeasurementByItsOutput", [] {
    rec::Record r{"b1", {{"obj/a.o", "app", "", 0, 300}, {"bin/app", "app", "", 300, 50}}, {{"dep", 12}}};
    rec::attach(r, {{"obj/a.o", 0, 1, 10.0}, {"obj/a.o", 0, 1, 280.0, 4096}});
    expect(r.edges[0].cpuMs == std::optional<double>(280.0)) << "the newest line of an output";
    expect(r.edges[0].peakBytes == std::optional<std::uint64_t>(4096));
    expect(!r.edges[1].cpuMs) << "a link the executor does not run is not measured";
    const auto t = rec::totals(r);
    expect(eq(t.ms, 350.0));
    expect(eq(t.cpuMs, 280.0));
    expect(eq(t.measured, std::size_t{1}));
    expect(eq(t.cachedUnits, std::size_t{12}));
}};

UTest a_record_reads_back_from_its_lines{"BuildRecord/ARecordReadsBackFromItsLines", [] {
    rec::Record a{"b1", {{"obj/a.o", "app", "t", 5, 300, 290.0, 1024}, {"obj/c.o", "", "", 0, 900}}, {{"dep", 3}}};
    rec::Record b{"b2", {{"obj/x.o", "lib", "", 1, 2}}, {}};
    const auto back = rec::from_ndjson(rec::to_ndjson(a) + rec::to_ndjson(b));
    expect(fatal(eq(back.size(), std::size_t{2})));
    expect(eq(back[0].id, std::string("b1")));
    expect(eq(back[0].edges.size(), std::size_t{2}));
    expect(eq(back[0].edges[0].program, std::string("t")));
    expect(back[0].edges[0].peakBytes == std::optional<std::uint64_t>(1024));
    expect(eq(back[0].cached.size(), std::size_t{1}));
    expect(eq(back[0].cached[0].units, std::size_t{3}));
    expect(eq(back[1].edges[0].package, std::string("lib")));
    const auto top = rec::slowest(back[0], 1);
    expect(fatal(eq(top.size(), std::size_t{1})));
    expect(eq(top[0].output, std::string("obj/c.o")));
}};

UTest identifiers_differ{"BuildRecord/IdentifiersDiffer", [] {
    std::set<std::string> ids;
    for (int i = 0; i < 64; ++i) ids.insert(rec::new_id());
    expect(ge(ids.size(), std::size_t{60}));
}};
