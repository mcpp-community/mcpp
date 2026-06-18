// mcpp.pack.pipeline — pack orchestration: build (re-preparing for musl static
// when needed), pick the main binary, plan + run the bundler.
// Bodies moved verbatim from the CLI layer. Zero behavior change.

module;
#include <cstdio>
#include <cstdlib>

export module mcpp.pack.pipeline;

import std;
import mcpp.build.prepare;
import mcpp.build.backend;
import mcpp.build.ninja;
import mcpp.build.plan;
import mcpp.config;
import mcpp.fetcher.progress;
import mcpp.pack;
import mcpp.ui;

namespace mcpp::pack {

// Everything after CLI option parsing for `mcpp pack`.
export int build_and_pack(Options opts, bool modeFromUser) {
    // `--target *-linux-musl` without an explicit `--mode` implies
    // `--mode static` — packaging a musl-static ELF as bundle-project
    // would feed patchelf a static binary and crash. The docs treat
    // this pair as equivalent; surface it in the code path too.
    if (!modeFromUser && opts.targetTriple.find("-musl") != std::string::npos) {
        opts.mode     = mcpp::pack::Mode::Static;
        modeFromUser  = true;   // user-equivalent intent — block manifest override
    }

    // ─── Build first (pack implies a fresh build) ────────────────────
    mcpp::build::BuildOverrides ov;
    if (opts.mode == mcpp::pack::Mode::Static && opts.targetTriple.empty())
        ov.target_triple = "x86_64-linux-musl";
    else
        ov.target_triple = opts.targetTriple;

    auto ctx = mcpp::build::prepare_build(/*print_fp=*/false, /*includeDevDeps=*/false,
                             /*extraTargets=*/{}, ov);
    if (!ctx) {
        mcpp::ui::error(ctx.error());
        return 2;
    }

    // Manifest may override mode only when neither --mode nor an
    // equivalent flag (--target *-musl → static) was given.
    if (!modeFromUser && !ctx->manifest.packConfig.defaultMode.empty()) {
        if (auto m = mcpp::pack::parse_mode(ctx->manifest.packConfig.defaultMode))
            opts.mode = *m;
    }

    // Re-derive target triple: if mode is Static we force the musl
    // triple even when the manifest's [pack].default_mode bumped us
    // here after `prepare_build` ran with the host toolchain.
    if (opts.mode == mcpp::pack::Mode::Static && ctx->tc.targetTriple.find("-musl") == std::string::npos) {
        // Need to re-prepare the build with the musl target.
        mcpp::build::BuildOverrides ov2;
        ov2.target_triple = "x86_64-linux-musl";
        auto ctx2 = mcpp::build::prepare_build(false, false, {}, ov2);
        if (!ctx2) { mcpp::ui::error(ctx2.error()); return 2; }
        ctx = std::move(ctx2);
    }

    auto be = mcpp::build::make_ninja_backend();
    mcpp::build::BuildOptions bo;
    auto br = be->build(ctx->plan, bo);
    if (!br) {
        mcpp::ui::error(br.error().message);
        return 1;
    }

    // ─── Pick the main binary target ─────────────────────────────────
    std::filesystem::path mainBinary;
    for (auto& lu : ctx->plan.linkUnits) {
        if (lu.kind == mcpp::build::LinkUnit::Binary
            && lu.targetName == ctx->manifest.package.name)
        {
            mainBinary = ctx->outputDir / lu.output;
            break;
        }
    }
    if (mainBinary.empty()) {
        // Fall back to the first binary target if package.name doesn't match.
        for (auto& lu : ctx->plan.linkUnits) {
            if (lu.kind == mcpp::build::LinkUnit::Binary) {
                mainBinary = ctx->outputDir / lu.output;
                break;
            }
        }
    }
    if (mainBinary.empty()) {
        mcpp::ui::error("no binary target to pack");
        return 1;
    }

    auto cfg = mcpp::config::load_or_init(/*quiet=*/false,
        mcpp::fetcher::make_bootstrap_progress_callback());
    if (!cfg) { mcpp::ui::error(cfg.error().message); return 4; }

    // ─── Build the plan + run ────────────────────────────────────────
    auto plan = mcpp::pack::make_plan(ctx->manifest, *cfg, opts,
        mainBinary, ctx->projectRoot, ctx->tc.targetTriple);
    if (!plan) { mcpp::ui::error(plan.error().message); return 1; }

    mcpp::ui::info("Packing", std::format("{} v{} ({})",
        plan->packageName, plan->packageVersion,
        mcpp::pack::mode_cli_name(plan->opts.mode)));

    auto r = mcpp::pack::run(*plan, *cfg);
    if (!r) {
        mcpp::ui::error(r.error().message);
        return 1;
    }

    auto pathCtx = mcpp::fetcher::make_path_ctx(&*cfg, ctx->projectRoot);
    auto outPath = (opts.format == mcpp::pack::Format::Tar)
        ? plan->tarballPath : plan->stagingRoot;
    mcpp::ui::status("Packed", mcpp::ui::shorten_path(outPath, pathCtx));
    return 0;
}

} // namespace mcpp::pack
