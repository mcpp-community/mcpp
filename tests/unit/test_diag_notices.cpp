import std;
import mcpplibs.mtest;
import mcpplibs.os.stdio;
import mcpp.diag;
import mcpp.notice;
import mcpp.ui;

using namespace mcpplibs::mtest;

// CROSS-LAYER: mcpp.diag is the one sink of user-visible notices; mcpp.notice holds the policies
// and the record of what a project was told. Closing notices are advisories about the run as a
// whole, said once after the command's own output as `tip:` lines on stderr; a command that writes
// an envelope takes them and reports them as notes.

namespace dg = mcpp::diag;
namespace io = mcpplibs::os::stdio;

namespace {

std::filesystem::path scratch_target(std::string_view name) {
    auto dir = std::filesystem::temp_directory_path()
             / std::format("mcpp-diag-{}-{:x}", name, std::random_device{}()) / "target";
    std::filesystem::create_directories(dir);
    return dir;
}

std::string stderr_of(auto&& body) {
    auto capture = io::capture::begin(io::stream::error).value();
    body();
    return capture.text();
}

}  // namespace

UTest closing_notices_are_deduplicated_and_taken_once{"DiagNotices/ClosingAreDeduplicatedAndTakenOnce", [] {
    dg::reset();
    dg::closing("index/requires-newer-mcpp", "the index needs a newer mcpp");
    dg::closing("index/requires-newer-mcpp", "the index needs a newer mcpp");
    dg::closing("platform/newer", "a second notice");
    auto taken = dg::take_closing();
    expect(fatal(eq(taken.size(), 2u)));
    // The machine code is the domain's: the published code of this notice is unchanged.
    expect(eq(taken[0].code(), std::string("MCPP_INDEX_REQUIRES_NEWER_MCPP")));
    expect(eq(taken[1].what, std::string("a second notice")));
    expect(dg::take_closing().empty());
    dg::reset();
}};

UTest closing_notices_print_as_tip_lines_once{"DiagNotices/ClosingPrintAsTipLinesOnce", [] {
    dg::reset();
    mcpp::ui::disable_color();
    dg::closing("index/requires-newer-mcpp", "upgrade to see newer packages");
    const auto err = stderr_of([] {
        dg::print_closing();
        dg::print_closing();
    });
    expect(eq(err, std::string("tip: upgrade to see newer packages\n")));
    dg::reset();
}};

UTest failure_notices_are_said_only_after_a_failure{"DiagNotices/FailureOnlyAfterAFailure", [] {
    dg::reset();
    mcpp::ui::disable_color();
    dg::on_failure("platform/newer", "a newer platform may fix this");
    expect(stderr_of([] { dg::print_closing(false); }).empty());
    dg::on_failure("platform/newer", "a newer platform may fix this");
    expect(eq(stderr_of([] { dg::print_closing(true); }),
              std::string("tip: a newer platform may fix this\n")));
    dg::reset();
}};

UTest quiet_prints_no_closing_notice_but_consumes_it{"DiagNotices/QuietConsumesClosing", [] {
    dg::reset();
    dg::closing("index/requires-newer-mcpp", "not shown under --quiet");
    mcpp::ui::set_quiet(true);
    const auto err = stderr_of([] { dg::print_closing(); });
    mcpp::ui::set_quiet(false);
    expect(err.empty()) << err;
    expect(dg::take_closing().empty());
    dg::reset();
}};

UTest a_once_notice_is_said_once_per_project{"DiagNotices/OnceIsSaidOncePerProject", [] {
    const auto target = scratch_target("once");
    mcpp::ui::disable_color();
    auto run = [&] {
        dg::reset();
        dg::set_project_target(target, true);
        return stderr_of([] {
            dg::notice(dg::Severity::Note, "build-program/includes-header", "ninja", "ninja is taken from the host");
        });
    };
    expect(run().contains("ninja is taken from the host"));
    expect(run().empty());  // a second process of the same project
    expect(fatal(dg::records().size() == 1u));
    expect(!dg::records()[0].shown);  // kept for the envelope, marked as not shown

    // `mcpp clean` removes target/ and with it the record.
    std::filesystem::remove(target / mcpp::notice::kRecordFile);
    expect(run().contains("ninja is taken from the host"));
    dg::reset();
    std::filesystem::remove_all(target.parent_path());
}};

UTest a_note_hidden_by_quiet_is_not_marked_as_told{"DiagNotices/QuietNoteIsNotMarked", [] {
    const auto target = scratch_target("quiet");
    mcpp::ui::disable_color();
    dg::reset();
    dg::set_project_target(target, true);
    mcpp::ui::set_quiet(true);
    (void)stderr_of([] { dg::notice(dg::Severity::Note, "build-program/includes-header", "cmake", "cmake from the host"); });
    mcpp::ui::set_quiet(false);
    dg::reset();
    dg::set_project_target(target, true);
    expect(stderr_of([] {
        dg::notice(dg::Severity::Note, "build-program/includes-header", "cmake", "cmake from the host");
    }).contains("cmake from the host"));
    dg::reset();
    std::filesystem::remove_all(target.parent_path());
}};

UTest a_domain_without_a_policy_is_said_every_run{"DiagNotices/AlwaysIsSaidEveryRun", [] {
    const auto target = scratch_target("always");
    mcpp::ui::disable_color();
    for (int i = 0; i < 2; ++i) {
        dg::reset();
        dg::set_project_target(target, true);
        expect(stderr_of([] {
            dg::notice(dg::Severity::Warning, "build/depfile", "a.cpp", "no depfile");
        }).contains("no depfile"));
    }
    dg::reset();
    std::filesystem::remove_all(target.parent_path());
}};

UTest a_notice_of_several_subjects_names_the_new_ones{"DiagNotices/SeveralSubjectsNameTheNewOnes", [] {
    const auto target = scratch_target("several");
    mcpp::ui::disable_color();
    auto run = [&](std::vector<std::string> files) {
        dg::reset();
        dg::set_project_target(target, true);
        return stderr_of([&] {
            dg::notice(dg::Severity::Warning, "build-program/reads-outside", "pkg", files,
                       [](std::string_view phrase) { return std::format("reads {}", phrase); });
        });
    };
    expect(run({"/x/a.hpp", "/x/b.hpp"}).contains("reads /x/a.hpp and /x/b.hpp"));
    expect(run({"/x/a.hpp", "/x/b.hpp"}).empty()) << "told of both";
    const auto third = run({"/x/a.hpp", "/x/c.hpp"});
    expect(third.contains("reads /x/c.hpp")) << third;
    expect(!third.contains("a.hpp")) << "only the subject not told of";
    dg::reset();
    std::filesystem::remove_all(target.parent_path());
}};

UTest a_plan_of_its_own_reads_the_record_and_writes_none{"DiagNotices/PlanOfItsOwnWritesNoRecord", [] {
    // The build database, doctor and publish's plan write nothing of the project: they consult
    // what it was told, say the rest, and leave target/ as it was.
    const auto target = scratch_target("readonly");
    mcpp::ui::disable_color();
    auto run = [&](bool writes) {
        dg::reset();
        dg::set_project_target(target, writes);
        return stderr_of([] {
            dg::notice(dg::Severity::Note, "build-program/includes-header", "pkg", "pkg includes a header");
            dg::remember_project_fact("toolchain:native", "gcc@1");
        });
    };
    expect(run(false).contains("pkg includes a header"));
    expect(!std::filesystem::exists(target / mcpp::notice::kRecordFile)) << "a plan of its own wrote the record";
    expect(run(true).contains("pkg includes a header")) << "a plan of its own recorded its notice as told";
    expect(run(false).empty()) << "a plan of its own did not read what the project was told";
    expect(dg::project_fact("toolchain:native") == std::optional<std::string>("gcc@1"));
    dg::reset();
    std::filesystem::remove_all(target.parent_path());
}};
