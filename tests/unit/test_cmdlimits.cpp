
import std;
import mcpplibs.mtest;
import mcpp.build.cmdlimits;

using namespace mcpplibs::mtest;

// The command-length budget, as data. Seven builds have died because a command
// outgrew a ceiling nobody tracked, and each was found by crashing at the end
// of a build with an error that named neither the edge nor the cause. These
// tests hold the two properties that make the table worth having: the numbers
// are the REAL ones, and the diagnostic says enough to act on.
//
// See .agents/docs/2026-08-06-command-length-architecture.md.

namespace cl = mcpp::build::cmdlimits;

namespace {

UTest every_channel_is_in_the_table{"CmdLimits/EveryChannelIsInTheTable", [] {
    // A channel that is not in the table has no budget, which is exactly the
    // state that produced this defect family seven times. `limit_of` must find
    // a real row for each enumerator, not fall through to the placeholder.
    for (auto c : {cl::Channel::NinjaArgv, cl::Channel::PosixShell,
                   cl::Channel::CmdWrapper, cl::Channel::RspContent,
                   cl::Channel::RspLine}) {
        expect(eq(cl::limit_of(c).channel, c));
        expect(!(cl::limit_of(c).imposedBy.empty()));
    }
}};

UTest numbers_are_the_real_ones{"CmdLimits/NumbersAreTheRealOnes", [] {
    // Each of these was learned the expensive way; a "tidier" number would be
    // wrong. MAX_ARG_STRLEN is 32 pages — NOT the 2 MiB ARG_MAX that #344
    // spent a release believing in.
    expect(eq(cl::limit_of(cl::Channel::PosixShell).bytes, 128u * 1024u));
    expect(eq(cl::limit_of(cl::Channel::NinjaArgv).bytes, 32u * 1024u));
    expect(eq(cl::limit_of(cl::Channel::CmdWrapper).bytes, 8191u));
    expect(eq(cl::limit_of(cl::Channel::RspLine).bytes, 128u * 1024u));
}};

UTest every_symptom_is_recorded{"CmdLimits/EverySymptomIsRecorded", [] {
    // The costly part of this family has never been the fix — it has been
    // RECOGNISING it. `Argument list too long`, `LNK1170` and a bare 127 have
    // nothing in common, so each row has to carry what the user actually sees.
    for (auto c : {cl::Channel::NinjaArgv, cl::Channel::PosixShell,
                   cl::Channel::CmdWrapper, cl::Channel::RspLine}) {
        auto const& l = cl::limit_of(c);
        expect(!l.symptom.empty()) << "channel has no symptom recorded";
        expect(!l.remedy.empty()) << "channel has no remedy recorded";
    }
}};

UTest shell_is_tighter_than_direct_spawn_on_windows{"CmdLimits/ShellIsTighterThanDirectSpawnOnWindows", [] {
    // cmd.exe is a quarter of CreateProcess, which is why #261's redirection
    // (it forced a `cmd /c` wrapper) failed where the direct spawn would have
    // been fine.
    expect(lt(cl::inline_budget(/*win=*/true, /*shell=*/true), cl::inline_budget(/*win=*/true, /*shell=*/false)));
}};

UTest under_budget_passes{"CmdLimits/UnderBudgetPasses", [] {
    expect(!(cl::check_inline(std::string(1000, 'x'), false, true)));
    expect(!(cl::check_inline(std::string(1000, 'x'), true, false)));
}};

UTest over_budget_is_reported_with_both_numbers{"CmdLimits/OverBudgetIsReportedWithBothNumbers", [] {
    // POSIX: ninja wraps in `sh -c`, so the whole command is one argv entry.
    auto over = cl::check_inline(std::string(200u * 1024u, 'x'), false, true);
    expect(fatal(bool(over)));
    expect(eq(over->channel, cl::Channel::PosixShell));
    expect(eq(over->actual, 200u * 1024u));
    expect(eq(over->allowed, 128u * 1024u));
}};

UTest windows_picks_the_channel_it_actually_travels{"CmdLimits/WindowsPicksTheChannelItActuallyTravels", [] {
    // 50 781 bytes is #274's real measurement (FFmpeg's 2281 units): fine for
    // CreateProcess, fatal through cmd.exe.
    const std::string cmd(50781, 'x');
    auto direct = cl::check_inline(cmd, /*win=*/true, /*shell=*/false);
    auto shell  = cl::check_inline(cmd, /*win=*/true, /*shell=*/true);
    expect(fatal(bool(direct)));
    expect(eq(direct->channel, cl::Channel::NinjaArgv));
    expect(fatal(bool(shell)));
    expect(eq(shell->channel, cl::Channel::CmdWrapper));
}};

UTest diagnostic_names_the_edge_the_numbers_and_the_way_out{"CmdLimits/DiagnosticNamesTheEdgeTheNumbersAndTheWayOut", [] {
    // Every underlying error omits the edge; that omission is what made these
    // expensive. The message must supply it, plus enough to act without
    // reading the source.
    auto over = cl::check_inline(std::string(200u * 1024u, 'x'), false, true);
    expect(fatal(bool(over)));
    const std::string msg = cl::explain("build edge 'bin/app' (rule cxx_link)", *over);

    expect(ne(msg.find("bin/app"), std::string::npos));
    expect(ne(msg.find("cxx_link"), std::string::npos));
    expect(ne(msg.find("204800"), std::string::npos));   // actual
    expect(ne(msg.find("131072"), std::string::npos));   // allowed
    expect(ne(msg.find("MAX_ARG_STRLEN"), std::string::npos));
    expect(ne(msg.find("response file"), std::string::npos));
    expect(ne(msg.find("2026-08-06-command-length-architecture"), std::string::npos));
}};

}  // namespace
