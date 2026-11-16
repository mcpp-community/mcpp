
import std;
import mcpplibs.mtest;
import mcpplibs.os.stdio;
import mcpp.ui;
import mcpp.build.progress;
import mcpp.build.action_progress;

using namespace mcpplibs::mtest;

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

UTest reads_the_counts_and_the_end_time{"ProgressStatus/ReadsTheCountsAndTheEndTime", [] {
    auto s = parse_status("\x1b[0m@@mcpp 3 12 0.027@@ OBJ obj/a.o");
    expect(fatal(s.has_value()));
    expect(eq(s->finished, 3u));
    expect(eq(s->total, 12u));
    expect(eq(s->endMs, 27));
    expect(eq(s->text, "OBJ obj/a.o"));

    auto long_ = parse_status("\x1b[0m@@mcpp 1203 1203 2407.968@@ LINK bin/gui.exe");
    expect(fatal(long_.has_value()));
    expect(eq(long_->endMs, 2407968));

    // A nested ninja's status line, which the outer ninja relays with its
    // escape sequence stripped, is output, not a status line.
    // `%u`: the steps not yet started (revision 3, §5.8); absent in a line of
    // the three-field format.
    expect(!(s->unstarted.has_value()));
    auto u = parse_status("\x1b[0m@@mcpp 700 707 41.2 0@@ LINK bin/xlings");
    expect(fatal(u.has_value()));
    expect(eq(u->finished, 700u));
    expect(eq(u->total, 707u));
    expect(fatal(u->unstarted.has_value()));
    expect(eq(*u->unstarted, 0u));
    expect(eq(u->text, "LINK bin/xlings"));
    expect(!(parse_status("@@mcpp 3 12 0.027@@ OBJ obj/a.o").has_value()));
    expect(!(parse_status("[3/12] OBJ obj/a.o").has_value()));
    expect(!(parse_status("src/a.cpp:1:2: error: @@mcpp").has_value()));
    expect(!(parse_status("").has_value()));
}};

UTest groups_the_entries_of_one_step{"ProgressLog/GroupsTheEntriesOfOneStep", [] {
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
    expect(eq(version, 6));
    expect(fatal(eq(steps.size(), 3u)));
    expect(eq(steps[0].outputs, std::vector<std::string>{"obj/std.o"}));
    expect(eq(steps[1].outputs, std::vector<std::string>{"obj/std.compat.o"}));
    expect(eq(steps[2].outputs, (std::vector<std::string>{"obj/lib2.m.o", "pcm.cache/pm_lib2.pcm"})));
    expect(eq(steps[2].start, 8));
    expect(eq(steps[2].end, 21));
    expect(eq(consumed, text.rfind('\n') + 1));
    expect(fatal(eq(offsets.size(), 3u)));
    expect(eq(text.substr(offsets[2], 3), "8\t2"));
}};

UTest this_run_is_the_suffix_whose_ends_the_status_lines_stated{"ProgressLog/ThisRunIsTheSuffixWhoseEndsTheStatusLinesStated", [] {
    // A recompacted log: the old entries first, in no particular order, then
    // this run's, whose ends the status lines gave.
    std::vector<LogStep> steps{{0, 900, {"old/a.o"}}, {5, 40, {"old/b.o"}},
                               {0, 12, {"new/x.o"}}, {3, 40, {"new/y.o"}}};
    expect(eq(run_boundary(steps, {12, 40}), 2u));
    // A status line whose entry is not written yet does not move the boundary.
    expect(eq(run_boundary(steps, {12, 40, 55}), 2u));
    expect(eq(run_boundary(steps, {}), 4u));
}};

UTest reads_complete_lines_only{"ProgressStarts/ReadsCompleteLinesOnly", [] {
    std::size_t consumed = 0;
    auto s = parse_starts("gen/vcpkg.stamp\t1700000000000\n"
                          "./gen/b.stamp\t1700000000500\n"
                          "gen/c.st", &consumed);
    expect(fatal(eq(s.size(), 2u)));
    expect(eq(s[0].stamp, "gen/vcpkg.stamp"));
    expect(eq(s[1].stamp, "gen/b.stamp"));
    expect(eq(s[1].unixMs, 1700000000500));
    expect(eq(consumed, std::string_view("gen/vcpkg.stamp\t1700000000000\n"
                                         "./gen/b.stamp\t1700000000500\n").size()));
}};

// ─── The step record ─────────────────────────────────────────────────────

UTest every_statement_is_recorded_with_its_owner{"ProgressRecord/EveryStatementIsRecordedWithItsOwner", [] {
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
    expect(fatal(eq(steps.size(), 5u)));
    expect(eq(steps[0].outputs, (std::vector<std::string>{"obj/main.o", "pcm.cache/app.pcm"})));
    expect(eq(steps[0].rule, "cxx_obj"));
    expect(eq(steps[1].outputs, std::vector<std::string>{"bin/app.bin"}));
    expect(eq(steps[2].outputs, std::vector<std::string>{"obj/with space.o"}));
    expect(eq(steps[3].outputs, std::vector<std::string>{"C:/abs/gen.stamp"}));
    expect(eq(steps[4].owner, ""));
    // The first explicit input, unescaped; an implicit input is not one.
    expect(eq(steps[0].input, "src/main.cpp"));
    expect(eq(steps[2].input, "src/with space.cpp"));
    expect(eq(steps[3].input, "a.in"));

    auto r = a.record({{"app", true, "app v0.1.0 (.)", 0, 0}});
    expect(eq(r.steps, 5u));
    expect(fatal(eq(r.packages.size(), 2u)));
    expect(eq(r.packages[0].steps, 2u));
    expect(eq(r.packages[0].subject, "app v0.1.0 (.)"));
    // An owner the plan did not declare is shown by its name, as a dependency.
    expect(eq(r.packages[1].name, "mcpplibs.lib"));
    expect(!(r.packages[1].requested));
    expect(eq(r.packages[1].steps, 2u));
    expect(eq(r.owner.at("pcm.cache/app.pcm"), 0u));
    expect(eq(r.owner.at("obj/with space.o"), 1u));
    expect(!r.owner.contains("obj/mcpp_ios_init.o"));
    expect(eq(r.actions.at("C:/abs/gen.stamp"), "vcpkg install"));

    // The outputs of one step share its identity.
    expect(eq(r.step.at("obj/main.o"), r.step.at("pcm.cache/app.pcm")));
    expect(ne(r.step.at("obj/main.o"), r.step.at("bin/app.bin")));

    r.packages[0].detail = "v0.1.0 (.)";
    r.packages[0].source = "project";
    auto back = parse_record(format_record(r));
    expect(eq(back.steps, r.steps));
    expect(fatal(eq(back.packages.size(), 2u)));
    expect(eq(back.packages[0].subject, "app v0.1.0 (.)"));
    expect(eq(back.packages[0].detail, "v0.1.0 (.)"));
    expect(eq(back.packages[0].source, "project"));
    expect(bool(back.packages[0].requested));
    expect(eq(back.owner, r.owner));
    expect(eq(back.step, r.step));
    expect(eq(back.actions, r.actions));
}};

UTest a_version_one_record_is_read{"ProgressRecord/AVersionOneRecordIsRead", [] {
    // 2026.9.29.5 wrote version 1: six fields to a package line, the subject
    // carrying the origin. The fast path of a newer mcpp reads it as it is.
    auto r = parse_record("# mcpp steps v1\t3\nP\txlings\t1\t0\t2\txlings (.)\n"
                          "O\t0\t0\tobj/a.o\n");
    expect(eq(r.steps, 3u));
    expect(fatal(eq(r.packages.size(), 1u)));
    expect(eq(r.packages[0].subject, "xlings (.)"));
    expect(r.packages[0].detail.empty());
    expect(r.packages[0].source.empty());
    expect(r.descriptor.empty());
    expect(r.sources.empty());
    expect(eq(r.owner.at("obj/a.o"), 0u));
}};

UTest a_compile_is_named_by_its_source_and_the_header_carries_the_descriptor{"ProgressRecord/ACompileIsNamedByItsSourceAndTheHeaderCarriesTheDescriptor", [] {
    // An absolute path on this host, spelled as build.ninja spells it.
    const bool drive = !std::filesystem::path("/w").is_absolute();
    const std::string root = drive ? "C:/w" : "/w", ninjaRoot = drive ? "C$:/w" : "/w";
    Attribution a;
    a.owner("app");
    a.statement(std::format("build obj/app/src/main.o: cxx_obj {}/app/src/main.cpp | pcm.cache/std.pcm\n",
                            ninjaRoot));
    a.statement("build obj/app/gen.o: cxx_obj obj/gen.cpp\n");                  // in the build directory
    a.statement(std::format("build obj/app/x.o: cxx_obj {}/elsewhere/x.cpp\n", ninjaRoot));
    a.statement("build bin/app: cxx_link obj/app/src/main.o obj/util/src/u.o\n");
    a.owner("util");
    a.statement(std::format("build obj/util/src/u.o: cxx_obj {}/app/deps/util/src/u.cpp\n", ninjaRoot));
    a.statement("build obj/util/none.stamp: stamp || obj/util/src/u.o\n");       // no explicit input

    auto r = a.record({{"app", true, "app", 0, 0}}, {root + "/app", root + "/app/deps/util"});
    expect(eq(r.sources.at(r.step.at("obj/app/src/main.o")), "src/main.cpp"));
    // The innermost root that holds the file.
    expect(eq(r.sources.at(r.step.at("obj/util/src/u.o")), "src/u.cpp"));
    expect(!(r.sources.contains(r.step.at("obj/app/gen.o"))));
    expect(!(r.sources.contains(r.step.at("obj/app/x.o"))));
    expect(!(r.sources.contains(r.step.at("bin/app"))));
    expect(!(r.sources.contains(r.step.at("obj/util/none.stamp"))));

    r.descriptor = "unoptimized + debuginfo";
    auto back = parse_record(format_record(r));
    expect(eq(back.sources, r.sources));
    expect(eq(back.descriptor, "unoptimized + debuginfo"));

    // The fast path reads the header line alone.
    Tmp t;
    write_record(t.path, r);
    expect(eq(read_descriptor(t.path), "unoptimized + debuginfo"));
    expect(eq(read_descriptor(t.path / "absent"), ""));
}};

UTest paths_are_compared_normalised{"ProgressRecord/PathsAreComparedNormalised", [] {
    expect(eq(normalise("./obj\\a.o"), "obj/a.o"));
    expect(eq(normalise("obj/../obj/b.o"), "obj/b.o"));
}};

// ─── Measures of text ────────────────────────────────────────────────────

UTest display_width_counts_columns{"ProgressText/DisplayWidthCountsColumns", [] {
    expect(eq(mcpp::ui::display_width("abc"), 3u));
    expect(eq(mcpp::ui::display_width("\033[1m\033[92mabc\033[0m"), 3u));
    expect(eq(mcpp::ui::display_width("中文"), 4u));
    expect(eq(mcpp::ui::display_width("a · b → c …"), 11u));
}};

UTest fit_cuts_by_columns_and_closes_colour{"ProgressText/FitCutsByColumnsAndClosesColour", [] {
    expect(eq(mcpp::ui::fit("abcdef", 6), "abcdef"));
    expect(eq(mcpp::ui::fit("abcdef", 4), "abc…"));
    expect(eq(mcpp::ui::fit("中文路径", 5), "中文…"));
    auto coloured = mcpp::ui::fit("\033[32mabcdef\033[0m", 3);
    expect(eq(mcpp::ui::display_width(coloured), 3u));
    expect(coloured.ends_with("\033[0m"));
}};

UTest one_format_for_every_duration{"ProgressText/OneFormatForEveryDuration", [] {
    using ms = std::chrono::milliseconds;
    expect(eq(mcpp::ui::format_duration(ms(840)), "0.84s"));
    expect(eq(mcpp::ui::format_duration(ms(12'340)), "12.34s"));
    expect(eq(mcpp::ui::format_duration(ms(192'000)), "3m12s"));
    expect(eq(mcpp::ui::format_duration(ms(3'720'000)), "1h02m"));
    expect(eq(mcpp::ui::format_clock(ms(872'000)), "14:32"));
    expect(eq(mcpp::ui::format_clock(ms(3'730'000)), "1:02:10"));
}};

UTest the_state_starts_at_the_blocks_column{"ProgressText/TheStateStartsAtTheBlocksColumn", [] {
    mcpp::ui::disable_color();
    expect(eq(mcpp::ui::step_line("Compiling", "core (core)", 16, "done 1.20s"), "   Compiling core (core)     done 1.20s"));
    // A longer subject pushes its state two columns further.
    expect(eq(mcpp::ui::step_line("Compiling", "a-long-subject-here", 16, "failed"), "   Compiling a-long-subject-here  failed"));
}};

// ─── The region ──────────────────────────────────────────────────────────

UTest the_status_line_is_the_last_row_with_no_blank_row{"ProgressRegion/TheStatusLineIsTheLastRowWithNoBlankRow", [] {
    mcpp::ui::Frame f{{}, "    Building 612/1203 · 14:32"};
    auto rows = mcpp::ui::region_rows({"bar"}, f, 10);
    expect(fatal(eq(rows.size(), 2u)));
    expect(eq(rows[0], "bar"));
    expect(eq(rows[1], "    Building 612/1203 · 14:32"));
}};

UTest lines_beyond_the_room_are_summarised{"ProgressRegion/LinesBeyondTheRoomAreSummarised", [] {
    mcpp::ui::Frame f;
    for (int i = 0; i < 14; ++i) f.lines.push_back(std::format("line {}", i));
    f.status = "Building";
    auto rows = mcpp::ui::region_rows({}, f, 10);
    expect(fatal(eq(rows.size(), 11u)));   // 9 lines, the summary, the status
    expect(ne(rows[9].find("… 5 more"), std::string::npos));
}};

UTest a_frame_overwrites_the_old_rows_instead_of_erasing_them{"ProgressRegion/AFrameOverwritesTheOldRowsInsteadOfErasingThem", [] {
    // From the first row of a region of two rows: the line over the first,
    // the new status row over the second, the tail of each cleared, and only
    // then what is left below. No row is erased before it is written
    // (revision 3, §9.2).
    expect(eq(mcpp::ui::frame_bytes(2, "   Compiling a\n", {"s"}), "\r\033[1A   Compiling a\033[K\n\033[?7ls\033[K\033[?7h\033[J"));
    // A region drawn for the first time starts where the cursor is.
    expect(eq(mcpp::ui::frame_bytes(0, {}, {"s"}), "\033[?7ls\033[K\033[?7h"));
    // Erasing leaves the cursor at the region's first row.
    expect(eq(mcpp::ui::frame_bytes(1, {}, {}), "\r\033[J"));
    auto bytes = mcpp::ui::redraw_bytes(3, {"a", "status"});
    expect(bytes.starts_with("\r\033[2A\033[?7la")) << bytes;
    expect(eq(count(bytes, "\033[J"), 1u));
    expect(bytes.ends_with("\033[J"));
}};

UTest ambiguous_characters_count_two_where_the_terminal_draws_them_wide{"ProgressText/AmbiguousCharactersCountTwoWhereTheTerminalDrawsThemWide", [] {
    mcpp::ui::set_ambiguous_wide(true);
    expect(eq(mcpp::ui::display_width("·"), 2u));
    expect(eq(mcpp::ui::display_width("━─"), 4u));
    expect(eq(mcpp::ui::display_width("⣿⣀"), 2u));   // braille is narrow everywhere
    mcpp::ui::set_ambiguous_wide(false);
    expect(eq(mcpp::ui::display_width("·"), 1u));
}};

UTest the_phase_is_aligned_with_the_verbs{"ProgressText/ThePhaseIsAlignedWithTheVerbs", [] {
    mcpp::ui::disable_color();
    expect(eq(mcpp::ui::status_line("Building", "612/707 · 0:35"), "    Building 612/707 · 0:35"));
    expect(eq(mcpp::ui::status_line("Running", "· 0:04"), "     Running · 0:04"));
}};

// ─── When a package is named (revision 3, §5.3) ──────────────────────────

UTest an_action_start_loads_the_fast_path_record_before_ninja_writes_stdout{"ProgressModel/AnActionStartLoadsTheFastPathRecordBeforeNinjaWritesStdout", [] {
    namespace telemetry = mcpp::build::action_progress;
    Tmp tmp;
    Record rec;
    rec.packages = {{"progress-no-stdout", true, "progress-no-stdout", 0, 1, "v0.0.0 (.)", "project"}};
    rec.owner = {{"gen/check.stamp", 0}};
    rec.step = {{"gen/check.stamp", 0}};
    rec.actions = {{"gen/check.stamp", "check-action"}};
    rec.steps = 1;
    write_record(tmp.path, rec);
    Build build(tmp.path); // The cached path has a record on disk, not set_record().
    build.pass_begin();
    const auto env = build.environment();
    const auto root = std::ranges::find(env, telemetry::kRootEnv, &decltype(env)::value_type::first);
    expect(fatal(ne(root, env.end())));
    std::ofstream(telemetry::channel(root->second, "gen/check.stamp"))
        << R"({"v":1,"seq":1,"phase":"internal-check","done":1,"total":2})";
    const auto unixMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    append(tmp.path / kStartsFile, std::format("gen/check.stamp\t{}\n", unixMs));
    auto stderrCapture = mcpplibs::os::stdio::capture::begin(mcpplibs::os::stdio::stream::error).value();
    open(false);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    std::string row;
    do {
        row = status_row();
        if (row.contains("internal-check 1/2")) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < deadline);
    close();
    (void)stderrCapture.text();
    expect(build.has_record());
    expect(ne(row.find("check-action"), std::string::npos)) << row;
    expect(ne(row.find("internal-check 1/2"), std::string::npos)) << row;
    // No Build::status() or completed ninja-log entry was supplied.
}};

UTest a_new_snapshot_redraws_before_the_clock_heartbeat{"ProgressModel/ANewSnapshotRedrawsBeforeTheClockHeartbeat", [] {
    namespace telemetry = mcpp::build::action_progress;
    Tmp tmp;
    Record rec;
    rec.packages = {{"progress-redraw", true, "r", 0, 1, "", "project"}};
    rec.owner = {{"gen/redraw.stamp", 0}};
    rec.actions = {{"gen/redraw.stamp", "a"}};
    Build build(tmp.path);
    build.set_record(rec);
    build.pass_begin();
    const auto env = build.environment();
    const auto root = std::ranges::find(env, telemetry::kRootEnv, &decltype(env)::value_type::first);
    expect(fatal(ne(root, env.end())));
    const auto unixMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    append(tmp.path / kStartsFile, std::format("gen/redraw.stamp\t{}\n", unixMs));
    const bool wasLive = mcpp::ui::live_progress();
    mcpp::ui::set_live_progress(true);
    auto stderrCapture = mcpplibs::os::stdio::capture::begin(mcpplibs::os::stdio::stream::error).value();
    open(false);
    // Let the initial half-second display guard pass. This line then draws
    // the frame immediately before publication, resetting the clock redraw.
    std::this_thread::sleep_for(std::chrono::milliseconds(550));
    mcpp::ui::info("Probe", "before snapshot publication");
    std::ofstream(telemetry::channel(root->second, "gen/redraw.stamp"))
        << R"({"v":1,"seq":1,"phase":"snapshot-redraw","done":1,"total":2})";
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    close();
    const auto out = stderrCapture.text();
    mcpp::ui::set_live_progress(wasLive);
    expect(ne(out.find("snapshot-redraw 1/2"), std::string::npos)) << out;
    // No Ninja stdout, completion or other narration requested a redraw.
}};

UTest a_package_is_named_once_when_its_first_step_finishes{"ProgressModel/APackageIsNamedOnceWhenItsFirstStepFinishes", [] {
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
    auto stderrCapture = mcpplibs::os::stdio::capture::begin(mcpplibs::os::stdio::stream::error).value();
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
    auto out = stderrCapture.text();
    // The dependency first, as its first step finished first; each once; the
    // standard library module is the toolchain's and is not named.
    const auto dep = out.find("Compiling compat.dep v1.0.0");
    const auto app = out.find("Compiling app v0.1.0 (.)");
    expect(fatal(ne(dep, std::string::npos))) << out;
    expect(fatal(ne(app, std::string::npos))) << out;
    expect(lt(dep, app)) << out;
    expect(eq(count(out, "app v0.1.0"), 1u)) << out;
    expect(eq(out.find("std"), std::string::npos)) << out;
    // No outcome, no fold: the line states that the package compiles.
    expect(eq(out.find("done"), std::string::npos)) << out;
    expect(eq(out.find("dependenc"), std::string::npos)) << out;
}};

UTest a_package_the_cache_serves_is_named_cached_with_its_units{"ProgressModel/APackageTheCacheServesIsNamedCachedWithItsUnits", [] {
    mcpp::ui::disable_color();
    Tmp tmp;
    Record rec;
    rec.packages = {{"compat.ftxui", false, "compat.ftxui", 73, 2, "v6.1.9", "official"}};
    rec.owner = {{"obj/a.o", 0}, {"obj/b.o", 0}};
    rec.step  = {{"obj/a.o", 0}, {"obj/b.o", 1}};
    Build b(tmp.path);
    b.set_record(rec);
    auto stderrCapture = mcpplibs::os::stdio::capture::begin(mcpplibs::os::stdio::stream::error).value();
    b.pass_begin();
    append(tmp.path / ".ninja_log", "# ninja log v6\n1\t2\t0\tobj/a.o\taa\n1\t2\t0\tobj/b.o\tbb\n");
    b.status({2, 2, 2, 0, {}});
    b.pass_end();
    b.finish(true);
    auto out = stderrCapture.text();
    expect(ne(out.find("Cached compat.ftxui v6.1.9 (73 units)"), std::string::npos)) << out;
    expect(eq(count(out, "compat.ftxui"), 1u)) << out;
}};

// ─── What the count counts (build wall-time plan, W2) ────────────────────
//
// `Building f/t` states the work of the build. A clean build of xlings counted
// 1195 steps, 503 of them placements of the cache pass and 460 dependency
// scans, and read 967/1195 when its first compile began. A placement pass is
// read but not counted, a scan pass shows as `Scanning f/t`, and the main pass
// alone is `Building f/t`.
UTest only_the_main_pass_is_counted_as_building{"ProgressModel/OnlyTheMainPassIsCountedAsBuilding", [] {
    mcpp::ui::disable_color();
    Tmp tmp;
    Record rec;
    rec.packages = {{"app", true, "app", 0, 1, "v0.1.0 (.)", "project"}};
    rec.steps = 1;
    Build b(tmp.path);
    b.set_record(rec);
    auto stderrCapture = mcpplibs::os::stdio::capture::begin(mcpplibs::os::stdio::stream::error).value();
    b.pass_begin(mcpp::build::progress::PassKind::Placement);
    b.status({503, 503, 1, 0, {}});
    const auto placing = mcpp::build::progress::status_row();
    b.pass_end();
    b.pass_begin(mcpp::build::progress::PassKind::Scan);
    b.status({200, 460, 2, 100, {}});
    const auto scanning = mcpp::build::progress::status_row();
    b.pass_end();
    b.pass_begin(mcpp::build::progress::PassKind::Work);
    b.status({1, 232, 3, 200, {}});
    const auto building = mcpp::build::progress::status_row();
    b.pass_end();
    b.finish(true);
    stderrCapture.text();
    expect(eq(placing.find("503"), std::string::npos)) << placing;
    expect(ne(scanning.find("Scanning"), std::string::npos)) << scanning;
    expect(ne(scanning.find("200/460"), std::string::npos)) << scanning;
    expect(ne(building.find("Building"), std::string::npos)) << building;
    expect(ne(building.find(" 1/232"), std::string::npos)) << building;
}};

UTest a_failure_names_its_package_once{"ProgressModel/AFailureNamesItsPackageOnce", [] {
    mcpp::ui::disable_color();
    Tmp tmp;
    Record rec;
    rec.packages = {{"app", true, "app", 0, 1, "v0.1.0 (.)", "project"},
                    {"lib", true, "lib", 0, 1, "v0.1.0 (lib)", "project"}};
    rec.owner = {{"obj/a.o", 0}, {"obj/l.o", 1}};
    rec.step  = {{"obj/a.o", 0}, {"obj/l.o", 1}};
    Build b(tmp.path);
    b.set_record(rec);
    auto stderrCapture = mcpplibs::os::stdio::capture::begin(mcpplibs::os::stdio::stream::error).value();
    b.pass_begin();
    // A failed step is not in ninja's log: the package is named from the
    // FAILED line.
    const auto first = b.failed("obj/l.o ");
    const auto second = b.failed("obj/a.o ");
    b.pass_end();
    b.finish(false);
    auto out = stderrCapture.text();
    expect(fatal(first.has_value()));
    expect(eq(*first, "lib v0.1.0 (lib)"));
    expect(!second.has_value());
    expect(eq(count(out, "Compiling lib v0.1.0 (lib)"), 1u)) << out;
}};

UTest a_step_whose_entries_are_read_in_two_pieces_counts_once{"ProgressModel/AStepWhoseEntriesAreReadInTwoPiecesCountsOnce", [] {
    // ninja writes one log entry per output and flushes each: a read can land
    // between a module unit's object and its BMI.
    mcpp::ui::disable_color();
    Tmp tmp;
    Record rec;
    // The report is the process's, as it is the command's: a name no other
    // test of this binary announced.
    rec.packages = {{"mlib", true, "mlib", 0, 2, "v0.1.0 (mlib)", "project"}};
    rec.owner = {{"obj/lib.m.o", 0}, {"pcm.cache/lib.pcm", 0}, {"bin/lib.a", 0}};
    rec.step  = {{"obj/lib.m.o", 0}, {"pcm.cache/lib.pcm", 0}, {"bin/lib.a", 1}};
    Build b(tmp.path);
    b.set_record(rec);
    auto stderrCapture = mcpplibs::os::stdio::capture::begin(mcpplibs::os::stdio::stream::error).value();
    b.pass_begin();
    const auto log = tmp.path / ".ninja_log";
    append(log, "# ninja log v6\n1\t10\t0\tobj/lib.m.o\taa\n");
    b.status({1, 2, 10, {}, {}});
    append(log, "1\t10\t0\tpcm.cache/lib.pcm\taa\n");
    b.status({1, 2, 10, {}, {}});
    b.pass_end();
    b.finish(true);
    auto out = stderrCapture.text();
    expect(eq(count(out, "Compiling mlib"), 1u)) << out;
}};

UTest a_package_two_configurations_share_is_announced_once_per_command{"ProgressModel/APackageTwoConfigurationsShareIsAnnouncedOncePerCommand", [] {
    // A workspace builds one plan per configuration; a dependency they share
    // is the same line in each, and the command writes it once (L6).
    mcpp::ui::disable_color();
    auto stderrCapture = mcpplibs::os::stdio::capture::begin(mcpplibs::os::stdio::stream::error).value();
    for (int config = 0; config < 2; ++config) {
        Tmp tmp;
        Record rec;
        rec.packages = {{"shared", true, "shared", 0, 1, "v0.2.0 (shared)", "project"}};
        rec.owner = {{"obj/s.o", 0}};
        rec.step  = {{"obj/s.o", 0}};
        Build b(tmp.path);
        b.set_record(rec);
        b.pass_begin();
        append(tmp.path / ".ninja_log", "# ninja log v6\n1\t10\t0\tobj/s.o\taa\n");
        b.status({1, 1, 10, {}, {}});
        b.pass_end();
        b.finish(true);
    }
    auto out = stderrCapture.text();
    expect(eq(count(out, "Compiling shared v0.2.0 (shared)"), 1u)) << out;
}};

// `Finished` is written once per command. `mcpp run --format <name>` packs, and
// the pack states its build with `Finished`; the run then drives the same build
// again, and its second `Finished` repeated the line with the first's time
// counted twice (pack drive and selection design 2026-10-01, A3).
UTest finished_is_written_once_per_command{"ProgressModel/FinishedIsWrittenOncePerCommand", [] {
    mcpp::ui::disable_color();
    auto stderrCapture = mcpplibs::os::stdio::capture::begin(mcpplibs::os::stdio::stream::error).value();
    mcpp::build::progress::finished("release", "optimized");
    mcpp::build::progress::finished("release", "optimized");
    auto out = stderrCapture.text();
    expect(eq(count(out, "Finished"), 1u)) << out;
}};
