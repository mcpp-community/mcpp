// mcpp.build.configure - configure-only prerequisites and execution.

module;
#include <cstdio>

export module mcpp.build.configure;

import std;
import mcpp.build.backend;
import mcpp.build.execute;
import mcpp.build.ninja;
import mcpp.build.plan;
import mcpp.build.prepare;
import mcpp.build.stage;
import mcpp.diag;
import mcpp.toolchain.model;
import mcpp.toolchain.registry;
import mcpp.ui;

export namespace mcpp::build {

std::expected<std::size_t, std::string>
stage_configure_prerequisites(const BuildPlan& plan) {
    const auto options = mcpp::build::stage::StageOptions{
        .verify = mcpp::build::stage::Verify::Size};
    std::size_t staged = 0;

    auto stage_one = [&](const std::filesystem::path& source,
                         const std::filesystem::path& destination,
                         std::string_view label)
        -> std::expected<void, std::string> {
        auto result = mcpp::build::stage::stage_file(source, destination, options);
        if (!result) {
            return std::unexpected(std::format(
                "cannot stage configure prerequisite '{}' ({}): {}",
                label, source.string(), result.error().message));
        }
        if (result->copied) ++staged;
        return {};
    };

    // clangd needs the BMI files for import resolution, but never the std
    // module's matching object files during configure-only.
    if (!plan.stdBmiPath.empty()) {
        auto result = stage_one(
            plan.stdBmiPath,
            mcpp::toolchain::staged_std_bmi_path(plan.toolchain, plan.outputDir),
            "std");
        if (!result) return std::unexpected(result.error());
    }
    if (!plan.stdCompatBmiPath.empty()) {
        auto result = stage_one(
            plan.stdCompatBmiPath,
            mcpp::toolchain::staged_std_compat_bmi_path(
                plan.toolchain, plan.outputDir),
            "std.compat");
        if (!result) return std::unexpected(result.error());
    }

    const auto traits = mcpp::toolchain::bmi_traits(plan.toolchain);
    for (const auto& unit : plan.compileUnits) {
        if (!unit.servedFromCache || !unit.providesModule
            || unit.cachedBmi.empty()) continue;

        std::string fileName;
        fileName.reserve(unit.providesModule->size() + traits.bmiExt.size());
        for (char ch : *unit.providesModule)
            fileName.push_back(ch == ':' ? '-' : ch);
        fileName += traits.bmiExt;

        auto result = stage_one(
            unit.cachedBmi, plan.outputDir / traits.bmiDir / fileName,
            *unit.providesModule);
        if (!result) return std::unexpected(result.error());
    }

    return staged;
}

int run_configure_plan(BuildContext& ctx, bool verbose) {
    auto staged = stage_configure_prerequisites(ctx.plan);
    if (!staged) {
        mcpp::ui::error(staged.error());
        return 1;
    }

    auto backend = mcpp::build::make_ninja_backend();
    BuildOptions options;
    options.verbose = verbose;
    options.dryRun = true;
    options.requireCompileDatabase = true;

    // The backend writes build.ninja before it honors dryRun, and a configure
    // plan's graph is NOT a normal build's graph: it carries the test targets
    // and dev-dependencies, so its `default` line names the test binaries and
    // omits the package's own target entirely.
    //
    // This used to be handled here, by dropping the fast-path cache entry that
    // still claimed the build dir held a normal graph. That repair was on the
    // WRITE side and therefore had to be repeated by every mode that rewrites
    // the graph — which is exactly why `mcpp test` stayed broken (#407) after
    // this half was fixed (#387). The plan now stamps its shape into
    // build.ninja and the fast paths check it, so a graph that is not a plain
    // build's graph cannot be replayed no matter who wrote it. Nothing to do
    // here.
    auto result = backend->build(ctx.plan, options);
    if (!result) {
        mcpp::ui::error(result.error().message);
        if (!result.error().diagnosticOutput.empty())
            std::fputs(result.error().diagnosticOutput.c_str(), stderr);
        return 1;
    }
    if (!mcpp::diag::flush(ctx.strict)) return 1;

    mcpp::ui::status("Configured", std::format(
        "{} ({} compile command{})", ctx.manifest.package.name,
        result->compileCommands,
        result->compileCommands == 1 ? "" : "s"));
    return 0;
}

} // namespace mcpp::build
