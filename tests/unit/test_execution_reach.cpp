// Whether an artifact runs here is one predicate for every row (SPEC-016 §6;
// mcpp#957). A branch for Apple's rows stood before it and refused them off
// macOS whether or not a runner was declared; these cases hold the predicate
// to the rows that branch covered.
import std;
import mcpplibs.mtest;
import mcpp.build.execute;
import mcpp.build.prepare;
import mcpp.toolchain.triple;

using namespace mcpplibs::mtest;

UTest a_declared_runner_reaches_a_row_of_another_system{"ExecutionReach/ADeclaredRunnerReachesARowOfAnotherSystem", [] {
    const mcpp::build::BuildContext ctx;
    for (std::string_view row : {"aarch64-macos", "x86_64-macos", "aarch64-ios", "aarch64-ios-sim"}) {
        // A runner is declared for the row: it is what runs the artifact, on any host.
        expect(!mcpp::build::unreachable_execution(ctx, row, false, true).has_value())
            << row << ": a declared runner was not consulted";
        // `--no-runner` states that the row is native here, and is believed.
        expect(!mcpp::build::unreachable_execution(ctx, row, true, false).has_value()) << row;
    }
}};

UTest without_a_runner_a_row_of_another_system_is_built_not_run{"ExecutionReach/WithoutARunnerARowOfAnotherSystemIsBuiltNotRun", [] {
    const mcpp::build::BuildContext ctx;
    const auto host = mcpp::toolchain::triple::host_triple().os;
    for (std::string_view row : {"aarch64-macos", "aarch64-ios", "x86_64-linux-gnu", "x86_64-windows-msvc"}) {
        const auto t = mcpp::toolchain::triple::parse(row);
        expect(fatal(t.has_value())) << row;
        const auto why = mcpp::build::unreachable_execution(ctx, row, false, false);
        if (t->system_is_host()) {
            expect(!why.has_value()) << row << " is of this system: " << why.value_or("");
            continue;
        }
        expect(fatal(why.has_value())) << row << " is of another system and was not refused";
        // The refusal names the two systems from the row's data and the keys that add a way.
        expect(why->contains(std::format("{} artifacts run on {}, not on this {} host", row, t->execution_system(), host))) << *why;
        expect(why->contains("runner =") && why->contains("run_under =")) << *why;
    }
}};
