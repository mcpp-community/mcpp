import std;
import mcpplibs.mtest;
import mcpp.build.action_progress;

using namespace mcpplibs::mtest;
namespace telemetry = mcpp::build::action_progress;

UTest accepts_unknown_totals_and_explicit_counts{"ActionProgress/AcceptsUnknownTotalsAndExplicitCounts", [] {
    const auto unknown = telemetry::parse(R"({"v":1,"seq":0,"phase":"configure","detail":"compiler checks"})");
    expect(fatal(bool(unknown)));
    expect(!unknown->total);
    expect(eq(telemetry::describe(*unknown), "configure · compiler checks"));
    const auto known = telemetry::parse(R"({"v":1,"seq":7,"phase":"compile","done":3,"total":9})");
    expect(fatal(bool(known)));
    expect(eq(telemetry::describe(*known), "compile 3/9"));
}};
UTest rejects_invalid_snapshots_without_inferring_success{"ActionProgress/RejectsInvalidSnapshotsWithoutInferringSuccess", [] {
    for (const auto text : {
        R"({"v":2,"seq":1,"phase":"compile"})",
        R"({"v":1,"seq":-1,"phase":"compile"})",
        R"({"v":1,"seq":1,"phase":""})",
        R"({"v":1,"seq":1,"phase":"compile","done":4,"total":3})",
        R"({"v":1,"seq":1,"phase":"compile","done":1.5})",
        R"({"v":1,"seq":1,"phase":"compile\nforged"})",
        R"({"v":1,"seq":1,"phase":"compile","detail":"\u001b[31m"})",
        R"({"v":1,"seq":1,"phase":"compile")"})
        expect(!telemetry::parse(text)) << text;
    expect(!(telemetry::parse(std::string(telemetry::kMaxBytes + 1, ' '))));
    // Finishing internal work is data, not a successful command or a stamp.
    expect(bool(telemetry::parse(R"({"v":1,"seq":2,"phase":"link","done":1,"total":1})")));
}};
UTest allocates_independent_run_directories_and_bounds_reads{"ActionProgress/AllocatesIndependentRunDirectoriesAndBoundsReads", [] {
    const auto base = std::filesystem::temp_directory_path();
    const auto first = telemetry::make_directory(base);
    const auto second = telemetry::make_directory(base);
    expect(fatal(!first.empty())); expect(fatal(!second.empty()));
    expect(ne(first, second));
    const auto a = telemetry::channel(first, "obj/a.o");
    expect(ne(a, telemetry::channel(first, "obj/b.o")));
    expect(ne(a, telemetry::channel(second, "obj/a.o")));
    std::ofstream(a) << std::string(telemetry::kMaxBytes + 1, ' ');
    expect(!telemetry::read(a));
    std::ofstream(a) << R"({"v":1,"seq":0,"phase":"work"})";
    expect(bool(telemetry::read(a)));
    std::filesystem::remove_all(first); std::filesystem::remove_all(second);
}};
