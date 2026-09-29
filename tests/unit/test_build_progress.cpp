#include <gtest/gtest.h>

import std;
import mcpp.ui;
import mcpp.build.progress;

// The build's report (.agents/docs/2026-09-29-build-progress-display-design.md,
// revised by .agents/docs/2026-09-30-build-output-refinement-design.md): its
// readers, the step record, the measures of text the renderer uses, the bytes
// of one frame, and when a package is named.

using namespace mcpp::build::progress;

namespace {

struct Tmp {
    std::filesystem::path path;
    Tmp() {
        path = std::filesystem::temp_directory_path()
             / std::format("mcpp_progress_test_{}", std::random_device{}());
        std::filesystem::create_directories(path);
    }
    ~Tmp() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

void append(const std::filesystem::path& p, std::string_view text) {
    std::ofstream os(p, std::ios::binary | std::ios::app);
    os << text;
}

std::size_t count(std::string_view hay, std::string_view needle) {
    std::size_t n = 0;
    for (auto at = hay.find(needle); at != std::string_view::npos;
         at = hay.find(needle, at + needle.size()))
        ++n;
    return n;
}

} // namespace

// ─── The readers ─────────────────────────────────────────────────────────

TEST(ProgressStatus, ReadsTheCountsAndTheEndTime) {
    auto s = parse_status("\x1b[0m@@mcpp 3 12 0.027@@ OBJ obj/a.o");
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(s->finished, 3u);
    EXPECT_EQ(s->total, 12u);
    EXPECT_EQ(s->endMs, 27);
    EXPECT_EQ(s->text, "OBJ obj/a.o");

    auto long_ = parse_status("\x1b[0m@@mcpp 1203 1203 2407.968@@ LINK bin/gui.exe");
    ASSERT_TRUE(long_.has_value());
    EXPECT_EQ(long_->endMs, 2407968);

    // A nested ninja's status line, which the outer ninja relays with its
    // escape sequence stripped, is output, not a status line.
    // `%u`: the steps not yet started (revision 3, §5.8); absent in a line of
    // the three-field format.
    EXPECT_FALSE(s->unstarted.has_value());
    auto u = parse_status("\x1b[0m@@mcpp 700 707 41.2 0@@ LINK bin/xlings");
    ASSERT_TRUE(u.has_value());
    EXPECT_EQ(u->finished, 700u);
    EXPECT_EQ(u->total, 707u);
    ASSERT_TRUE(u->unstarted.has_value());
    EXPECT_EQ(*u->unstarted, 0u);
    EXPECT_EQ(u->text, "LINK bin/xlings");
    EXPECT_FALSE(parse_status("@@mcpp 3 12 0.027@@ OBJ obj/a.o").has_value());
    EXPECT_FALSE(parse_status("[3/12] OBJ obj/a.o").has_value());
    EXPECT_FALSE(parse_status("src/a.cpp:1:2: error: @@mcpp").has_value());
    EXPECT_FALSE(parse_status("").has_value());
}

TEST(ProgressLog, GroupsTheEntriesOfOneStep) {
    // Measured on ninja 1.12.1: a module unit's object and BMI share start,
    // end and hash; std.o and std.compat.o ended in the same millisecond with
    // different hashes, and are two steps.
    const std::string text =
        "# ninja log v6\n"
        "1\t3\t100\tobj/std.o\t3daa\n"
        "1\t3\t100\tobj/std.compat.o\t73c1\n"
        "8\t21\t101\tobj/lib2.m.o\t2745\n"
        "8\t21\t101\tpcm.cache/pm_lib2.pcm\t2745\n"
        "42\t385\t102\tobj/main.o\tadbb";   // still being written
    std::size_t consumed = 0;
    int version = 0;
    std::vector<std::size_t> offsets;
    auto steps = parse_log(text, &consumed, &version, &offsets);
    EXPECT_EQ(version, 6);
    ASSERT_EQ(steps.size(), 3u);
    EXPECT_EQ(steps[0].outputs, std::vector<std::string>{"obj/std.o"});
    EXPECT_EQ(steps[1].outputs, std::vector<std::string>{"obj/std.compat.o"});
    EXPECT_EQ(steps[2].outputs, (std::vector<std::string>{"obj/lib2.m.o", "pcm.cache/pm_lib2.pcm"}));
    EXPECT_EQ(steps[2].start, 8);
    EXPECT_EQ(steps[2].end, 21);
    EXPECT_EQ(consumed, text.rfind('\n') + 1);
    ASSERT_EQ(offsets.size(), 3u);
    EXPECT_EQ(text.substr(offsets[2], 3), "8\t2");
}

TEST(ProgressLog, ThisRunIsTheSuffixWhoseEndsTheStatusLinesStated) {
    // A recompacted log: the old entries first, in no particular order, then
    // this run's, whose ends the status lines gave.
    std::vector<LogStep> steps{{0, 900, {"old/a.o"}}, {5, 40, {"old/b.o"}},
                               {0, 12, {"new/x.o"}}, {3, 40, {"new/y.o"}}};
    EXPECT_EQ(run_boundary(steps, {12, 40}), 2u);
    // A status line whose entry is not written yet does not move the boundary.
    EXPECT_EQ(run_boundary(steps, {12, 40, 55}), 2u);
    EXPECT_EQ(run_boundary(steps, {}), 4u);
}

TEST(ProgressStarts, ReadsCompleteLinesOnly) {
    std::size_t consumed = 0;
    auto s = parse_starts("gen/vcpkg.stamp\t1700000000000\n"
                          "./gen/b.stamp\t1700000000500\n"
                          "gen/c.st", &consumed);
    ASSERT_EQ(s.size(), 2u);
    EXPECT_EQ(s[0].stamp, "gen/vcpkg.stamp");
    EXPECT_EQ(s[1].stamp, "gen/b.stamp");
    EXPECT_EQ(s[1].unixMs, 1700000000500);
    EXPECT_EQ(consumed, std::string_view("gen/vcpkg.stamp\t1700000000000\n"
                                         "./gen/b.stamp\t1700000000500\n").size());
}

// ─── The step record ─────────────────────────────────────────────────────

TEST(ProgressRecord, EveryStatementIsRecordedWithItsOwner) {
    Attribution a;
    a.owner("app");
    a.statement("build obj/main.o | pcm.cache/app.pcm : cxx_obj src/main.cpp\n  flags = -O2\n");
    a.statement("build bin/app.bin: objcopy_bin bin/app\ndefault bin/app.bin\n\n");
    a.owner("mcpplibs.lib");
    a.statement("build obj/with$ space.o : cxx_obj src/with$ space.cpp\n");
    a.statement("build C$:/abs/gen.stamp : mcpp_action_0 a.in\n");
    a.action("C:/abs/gen.stamp", "vcpkg install");
    a.owner("");
    a.statement("build _mcpp_staged_cache : phony obj/x.o\n");   // not a step
    a.statement("build obj/mcpp_ios_init.o : ios_init_object obj/mcpp_ios_init.c\n");
    a.statement("rule cxx_obj\n  command = c++ $in\n");

    const auto& steps = a.steps();
    ASSERT_EQ(steps.size(), 5u);
    EXPECT_EQ(steps[0].outputs, (std::vector<std::string>{"obj/main.o", "pcm.cache/app.pcm"}));
    EXPECT_EQ(steps[0].rule, "cxx_obj");
    EXPECT_EQ(steps[1].outputs, std::vector<std::string>{"bin/app.bin"});
    EXPECT_EQ(steps[2].outputs, std::vector<std::string>{"obj/with space.o"});
    EXPECT_EQ(steps[3].outputs, std::vector<std::string>{"C:/abs/gen.stamp"});
    EXPECT_EQ(steps[4].owner, "");

    auto r = a.record({{"app", true, "app v0.1.0 (.)", 0, 0}});
    EXPECT_EQ(r.steps, 5u);
    ASSERT_EQ(r.packages.size(), 2u);
    EXPECT_EQ(r.packages[0].steps, 2u);
    EXPECT_EQ(r.packages[0].subject, "app v0.1.0 (.)");
    // An owner the plan did not declare is shown by its name, as a dependency.
    EXPECT_EQ(r.packages[1].name, "mcpplibs.lib");
    EXPECT_FALSE(r.packages[1].requested);
    EXPECT_EQ(r.packages[1].steps, 2u);
    EXPECT_EQ(r.owner.at("pcm.cache/app.pcm"), 0u);
    EXPECT_EQ(r.owner.at("obj/with space.o"), 1u);
    EXPECT_FALSE(r.owner.contains("obj/mcpp_ios_init.o"));
    EXPECT_EQ(r.actions.at("C:/abs/gen.stamp"), "vcpkg install");

    // The outputs of one step share its identity.
    EXPECT_EQ(r.step.at("obj/main.o"), r.step.at("pcm.cache/app.pcm"));
    EXPECT_NE(r.step.at("obj/main.o"), r.step.at("bin/app.bin"));

    r.packages[0].detail = "v0.1.0 (.)";
    r.packages[0].source = "project";
    auto back = parse_record(format_record(r));
    EXPECT_EQ(back.steps, r.steps);
    ASSERT_EQ(back.packages.size(), 2u);
    EXPECT_EQ(back.packages[0].subject, "app v0.1.0 (.)");
    EXPECT_EQ(back.packages[0].detail, "v0.1.0 (.)");
    EXPECT_EQ(back.packages[0].source, "project");
    EXPECT_TRUE(back.packages[0].requested);
    EXPECT_EQ(back.owner, r.owner);
    EXPECT_EQ(back.step, r.step);
    EXPECT_EQ(back.actions, r.actions);
}

TEST(ProgressRecord, AVersionOneRecordIsRead) {
    // 2026.9.29.5 wrote version 1: six fields to a package line, the subject
    // carrying the origin. The fast path of a newer mcpp reads it as it is.
    auto r = parse_record("# mcpp steps v1\t3\nP\txlings\t1\t0\t2\txlings (.)\n"
                          "O\t0\t0\tobj/a.o\n");
    EXPECT_EQ(r.steps, 3u);
    ASSERT_EQ(r.packages.size(), 1u);
    EXPECT_EQ(r.packages[0].subject, "xlings (.)");
    EXPECT_TRUE(r.packages[0].detail.empty());
    EXPECT_TRUE(r.packages[0].source.empty());
    EXPECT_EQ(r.owner.at("obj/a.o"), 0u);
}

TEST(ProgressRecord, PathsAreComparedNormalised) {
    EXPECT_EQ(normalise("./obj\\a.o"), "obj/a.o");
    EXPECT_EQ(normalise("obj/../obj/b.o"), "obj/b.o");
}

// ─── Measures of text ────────────────────────────────────────────────────

TEST(ProgressText, DisplayWidthCountsColumns) {
    EXPECT_EQ(mcpp::ui::display_width("abc"), 3u);
    EXPECT_EQ(mcpp::ui::display_width("\033[1m\033[92mabc\033[0m"), 3u);
    EXPECT_EQ(mcpp::ui::display_width("中文"), 4u);
    EXPECT_EQ(mcpp::ui::display_width("a · b → c …"), 11u);
}

TEST(ProgressText, FitCutsByColumnsAndClosesColour) {
    EXPECT_EQ(mcpp::ui::fit("abcdef", 6), "abcdef");
    EXPECT_EQ(mcpp::ui::fit("abcdef", 4), "abc…");
    EXPECT_EQ(mcpp::ui::fit("中文路径", 5), "中文…");
    auto coloured = mcpp::ui::fit("\033[32mabcdef\033[0m", 3);
    EXPECT_EQ(mcpp::ui::display_width(coloured), 3u);
    EXPECT_TRUE(coloured.ends_with("\033[0m"));
}

TEST(ProgressText, OneFormatForEveryDuration) {
    using ms = std::chrono::milliseconds;
    EXPECT_EQ(mcpp::ui::format_duration(ms(840)), "0.84s");
    EXPECT_EQ(mcpp::ui::format_duration(ms(12'340)), "12.34s");
    EXPECT_EQ(mcpp::ui::format_duration(ms(192'000)), "3m12s");
    EXPECT_EQ(mcpp::ui::format_duration(ms(3'720'000)), "1h02m");
    EXPECT_EQ(mcpp::ui::format_clock(ms(872'000)), "14:32");
    EXPECT_EQ(mcpp::ui::format_clock(ms(3'730'000)), "1:02:10");
}

TEST(ProgressText, TheStateStartsAtTheBlocksColumn) {
    mcpp::ui::disable_color();
    EXPECT_EQ(mcpp::ui::step_line("Compiling", "core (core)", 16, "done 1.20s"),
              "   Compiling core (core)     done 1.20s");
    // A longer subject pushes its state two columns further.
    EXPECT_EQ(mcpp::ui::step_line("Compiling", "a-long-subject-here", 16, "failed"),
              "   Compiling a-long-subject-here  failed");
}

// ─── The region ──────────────────────────────────────────────────────────

TEST(ProgressRegion, TheStatusLineIsTheLastRowWithNoBlankRow) {
    mcpp::ui::Frame f{{}, "    Building 612/1203 · 14:32"};
    auto rows = mcpp::ui::region_rows({"bar"}, f, 10);
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[0], "bar");
    EXPECT_EQ(rows[1], "    Building 612/1203 · 14:32");
}

TEST(ProgressRegion, LinesBeyondTheRoomAreSummarised) {
    mcpp::ui::Frame f;
    for (int i = 0; i < 14; ++i) f.lines.push_back(std::format("line {}", i));
    f.status = "Building";
    auto rows = mcpp::ui::region_rows({}, f, 10);
    ASSERT_EQ(rows.size(), 11u);   // 9 lines, the summary, the status
    EXPECT_NE(rows[9].find("… 5 more"), std::string::npos);
}

TEST(ProgressRegion, AFrameOverwritesTheOldRowsInsteadOfErasingThem) {
    // From the first row of a region of two rows: the line over the first,
    // the new status row over the second, the tail of each cleared, and only
    // then what is left below. No row is erased before it is written
    // (revision 3, §9.2).
    EXPECT_EQ(mcpp::ui::frame_bytes(2, "   Compiling a\n", {"s"}),
              "\r\033[1A   Compiling a\033[K\n\033[?7ls\033[K\033[?7h\033[J");
    // A region drawn for the first time starts where the cursor is.
    EXPECT_EQ(mcpp::ui::frame_bytes(0, {}, {"s"}), "\033[?7ls\033[K\033[?7h");
    // Erasing leaves the cursor at the region's first row.
    EXPECT_EQ(mcpp::ui::frame_bytes(1, {}, {}), "\r\033[J");
    auto bytes = mcpp::ui::redraw_bytes(3, {"a", "status"});
    EXPECT_TRUE(bytes.starts_with("\r\033[2A\033[?7la")) << bytes;
    EXPECT_EQ(count(bytes, "\033[J"), 1u);
    EXPECT_TRUE(bytes.ends_with("\033[J"));
}

TEST(ProgressText, AmbiguousCharactersCountTwoWhereTheTerminalDrawsThemWide) {
    mcpp::ui::set_ambiguous_wide(true);
    EXPECT_EQ(mcpp::ui::display_width("·"), 2u);
    EXPECT_EQ(mcpp::ui::display_width("━─"), 4u);
    EXPECT_EQ(mcpp::ui::display_width("⣿⣀"), 2u);   // braille is narrow everywhere
    mcpp::ui::set_ambiguous_wide(false);
    EXPECT_EQ(mcpp::ui::display_width("·"), 1u);
}

TEST(ProgressText, ThePhaseIsAlignedWithTheVerbs) {
    mcpp::ui::disable_color();
    EXPECT_EQ(mcpp::ui::status_line("Building", "612/707 · 0:35"), "    Building 612/707 · 0:35");
    EXPECT_EQ(mcpp::ui::status_line("Running", "· 0:04"), "     Running · 0:04");
}

// ─── When a package is named (revision 3, §5.3) ──────────────────────────

TEST(ProgressModel, APackageIsNamedOnceWhenItsFirstStepFinishes) {
    mcpp::ui::disable_color();
    Tmp tmp;
    Record rec;
    rec.packages = {{"app", true, "app", 0, 2, "v0.1.0 (.)", "project"},
                    {"compat.dep", false, "compat.dep", 0, 1, "v1.0.0", "official"},
                    {"std", false, "std", 0, 2, {}, {}}};
    rec.owner = {{"obj/main.o", 0}, {"bin/app", 0}, {"obj/dep.o", 1},
                 {"pcm.cache/std.pcm", 2}};
    rec.step  = {{"obj/main.o", 0}, {"bin/app", 1}, {"obj/dep.o", 2}, {"pcm.cache/std.pcm", 3}};
    rec.steps = 4;
    Build b(tmp.path);
    b.set_record(rec);
    testing::internal::CaptureStdout();
    b.pass_begin();
    const auto log = tmp.path / ".ninja_log";
    append(log, "# ninja log v6\n1\t10\t0\tobj/dep.o\taa\n");
    b.status({1, 4, 10, {}, {}});
    append(log, "1\t12\t0\tpcm.cache/std.pcm\tdd\n10\t30\t0\tobj/main.o\tbb\n");
    b.status({3, 4, 30, {}, {}});
    append(log, "30\t50\t0\tbin/app\tcc\n");
    b.status({4, 4, 50, 0, {}});
    b.pass_end();
    b.finish(true);
    auto out = testing::internal::GetCapturedStdout();
    // The dependency first, as its first step finished first; each once; the
    // standard library module is the toolchain's and is not named.
    const auto dep = out.find("Compiling compat.dep v1.0.0");
    const auto app = out.find("Compiling app v0.1.0 (.)");
    ASSERT_NE(dep, std::string::npos) << out;
    ASSERT_NE(app, std::string::npos) << out;
    EXPECT_LT(dep, app) << out;
    EXPECT_EQ(count(out, "app v0.1.0"), 1u) << out;
    EXPECT_EQ(out.find("std"), std::string::npos) << out;
    // No outcome, no fold: the line states that the package compiles.
    EXPECT_EQ(out.find("done"), std::string::npos) << out;
    EXPECT_EQ(out.find("dependenc"), std::string::npos) << out;
}

TEST(ProgressModel, APackageTheCacheServesIsNamedCachedWithItsUnits) {
    mcpp::ui::disable_color();
    Tmp tmp;
    Record rec;
    rec.packages = {{"compat.ftxui", false, "compat.ftxui", 73, 2, "v6.1.9", "official"}};
    rec.owner = {{"obj/a.o", 0}, {"obj/b.o", 0}};
    rec.step  = {{"obj/a.o", 0}, {"obj/b.o", 1}};
    Build b(tmp.path);
    b.set_record(rec);
    testing::internal::CaptureStdout();
    b.pass_begin();
    append(tmp.path / ".ninja_log", "# ninja log v6\n1\t2\t0\tobj/a.o\taa\n1\t2\t0\tobj/b.o\tbb\n");
    b.status({2, 2, 2, 0, {}});
    b.pass_end();
    b.finish(true);
    auto out = testing::internal::GetCapturedStdout();
    EXPECT_NE(out.find("Cached compat.ftxui v6.1.9 (73 units)"), std::string::npos) << out;
    EXPECT_EQ(count(out, "compat.ftxui"), 1u) << out;
}

TEST(ProgressModel, AFailureNamesItsPackageOnce) {
    mcpp::ui::disable_color();
    Tmp tmp;
    Record rec;
    rec.packages = {{"app", true, "app", 0, 1, "v0.1.0 (.)", "project"},
                    {"lib", true, "lib", 0, 1, "v0.1.0 (lib)", "project"}};
    rec.owner = {{"obj/a.o", 0}, {"obj/l.o", 1}};
    rec.step  = {{"obj/a.o", 0}, {"obj/l.o", 1}};
    Build b(tmp.path);
    b.set_record(rec);
    testing::internal::CaptureStdout();
    b.pass_begin();
    // A failed step is not in ninja's log: the package is named from the
    // FAILED line.
    const auto first = b.failed("obj/l.o ");
    const auto second = b.failed("obj/a.o ");
    b.pass_end();
    b.finish(false);
    auto out = testing::internal::GetCapturedStdout();
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(*first, "lib v0.1.0 (lib)");
    EXPECT_FALSE(second.has_value());
    EXPECT_EQ(count(out, "Compiling lib v0.1.0 (lib)"), 1u) << out;
}

TEST(ProgressModel, AStepWhoseEntriesAreReadInTwoPiecesCountsOnce) {
    // ninja writes one log entry per output and flushes each: a read can land
    // between a module unit's object and its BMI.
    mcpp::ui::disable_color();
    Tmp tmp;
    Record rec;
    rec.packages = {{"lib", true, "lib", 0, 2, "v0.1.0 (lib)", "project"}};
    rec.owner = {{"obj/lib.m.o", 0}, {"pcm.cache/lib.pcm", 0}, {"bin/lib.a", 0}};
    rec.step  = {{"obj/lib.m.o", 0}, {"pcm.cache/lib.pcm", 0}, {"bin/lib.a", 1}};
    Build b(tmp.path);
    b.set_record(rec);
    testing::internal::CaptureStdout();
    b.pass_begin();
    const auto log = tmp.path / ".ninja_log";
    append(log, "# ninja log v6\n1\t10\t0\tobj/lib.m.o\taa\n");
    b.status({1, 2, 10, {}, {}});
    append(log, "1\t10\t0\tpcm.cache/lib.pcm\taa\n");
    b.status({1, 2, 10, {}, {}});
    b.pass_end();
    b.finish(true);
    auto out = testing::internal::GetCapturedStdout();
    EXPECT_EQ(count(out, "Compiling lib"), 1u) << out;
}
