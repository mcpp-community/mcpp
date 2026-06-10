// mcpp.cli.cmd_publish — publish / pack / emit xpkg
//
// Extracted verbatim from cli.cppm (cli modularization, see
// .agents/docs/2026-06-10-cli-modularization.md). Zero behavior change:
// bodies are byte-identical moves; only the surrounding module/namespace
// changed (mcpp::cli::detail -> mcpp::cli).

module;
#include <cstdio>
#include <cstdlib>

export module mcpp.cli.cmd_publish;

import std;
import mcpplibs.cmdline;
import mcpp.cli.build;
import mcpp.cli.common;
import mcpp.cli.install_ui;
import mcpp.build.backend;
import mcpp.build.ninja;
import mcpp.build.plan;
import mcpp.config;
import mcpp.manifest;
import mcpp.modgraph.scanner;
import mcpp.pack;
import mcpp.platform;
import mcpp.publish.xpkg_emit;
import mcpp.ui;

namespace mcpp::cli {

// `mcpp emit xpkg ...` — only one subcommand defined, so the action sits
// directly on the `emit xpkg` nested subcommand and receives its
// ParsedArgs.
export int cmd_emit_xpkg(const mcpplibs::cmdline::ParsedArgs& parsed) {
    std::string version = parsed.option_or_empty("version").value();
    std::filesystem::path output =
        parsed.option_or_empty("output").value();

    auto root = find_manifest_root(std::filesystem::current_path());
    if (!root) {
        std::println(stderr, "error: no mcpp.toml found");
        return 2;
    }
    auto m = mcpp::manifest::load(*root / "mcpp.toml");
    if (!m) { std::println(stderr, "error: {}", m.error().format()); return 2; }
    auto scan = mcpp::modgraph::scan_package(*root, *m);
    if (!scan.errors.empty()) {
        for (auto& e : scan.errors) std::println(stderr, "error: {}", e.format());
        return 2;
    }

    if (version.empty()) version = m->package.version;
    auto release = mcpp::publish::placeholder_release(version);
    auto lua = mcpp::publish::emit_xpkg(*m, scan.graph, release);

    if (output.empty()) {
        std::print("{}", lua);
    } else {
        std::ofstream os(output);
        if (!os) { std::println(stderr, "error: cannot write '{}'", output.string()); return 1; }
        os << lua;
        std::println("Wrote {}", output.string());
    }
    return 0;
}

export int cmd_publish(const mcpplibs::cmdline::ParsedArgs& parsed) {
    bool dry_run    = parsed.is_flag_set("dry-run");
    bool allow_dirty= parsed.is_flag_set("allow-dirty");

    auto root = find_manifest_root(std::filesystem::current_path());
    if (!root) { mcpp::ui::error("no mcpp.toml in current dir or parents"); return 2; }

    // Sanity: working tree clean (best-effort via git status).
    if (!allow_dirty && std::filesystem::exists(*root / ".git")) {
        auto gitStatus = mcpp::platform::process::capture(
            std::format("git -C {} status --porcelain 2>&1",
                        mcpp::platform::shell::quote(root->string())));
        std::string out = gitStatus.output;
        if (!out.empty()) {
            mcpp::ui::error("working tree has uncommitted changes; pass --allow-dirty to skip this check");
            std::println(stderr, "{}", out);
            return 1;
        }
    }

    auto m = mcpp::manifest::load(*root / "mcpp.toml");
    if (!m) {
        mcpp::ui::error(std::format("manifest parse: {}", m.error().format()));
        return 2;
    }
    auto scan = mcpp::modgraph::scan_package(*root, *m);
    if (!scan.errors.empty()) {
        for (auto& e : scan.errors) mcpp::ui::error(e.format());
        return 2;
    }

    auto& pkg = m->package;
    mcpp::ui::status("Packaging", std::format("{} v{}", pkg.name, pkg.version));

    // Output dir: target/dist/
    auto distDir = *root / "target" / "dist";
    std::error_code ec;
    std::filesystem::create_directories(distDir, ec);

    auto tarball = distDir / std::format("{}-{}.tar.gz", pkg.name, pkg.version);
    auto xpkgPath = distDir / std::format("{}.lua", pkg.name);

    // 1. Pack source via `git archive` (respects .gitignore).
    if (auto err = mcpp::publish::make_release_tarball(
            *root, pkg.name, pkg.version, tarball);
        !err.empty())
    {
        mcpp::ui::error(std::format("tarball: {}", err));
        return 1;
    }
    auto tarballSize = std::filesystem::file_size(tarball, ec);

    // 2. Compute SHA-256.
    auto sha = mcpp::publish::sha256_of_file(tarball);
    if (sha.empty()) {
        mcpp::ui::error("sha256: failed to hash tarball (is `sha256sum` installed?)");
        return 1;
    }

    // 3. Compute the convention-based GitHub Release URL from manifest.repo.
    auto url = mcpp::publish::release_tarball_url(
        pkg.repo, pkg.name, pkg.version);
    if (url.empty()) {
        mcpp::ui::error(std::format(
            "cannot derive tarball URL: [package].repo='{}' is empty or not "
            "a https URL. Set [package].repo in mcpp.toml.", pkg.repo));
        return 1;
    }

    // 4. Build release info + emit xpkg.lua.
    auto release = mcpp::publish::make_release_info(pkg.version, url, sha);
    auto lua = mcpp::publish::emit_xpkg(*m, scan.graph, release);

    {
        std::ofstream os(xpkgPath);
        os << lua;
        if (!os) {
            mcpp::ui::error(std::format(
                "cannot write '{}'", xpkgPath.string()));
            return 1;
        }
    }

    mcpp::ui::status("Tarball",
        std::format("{} ({} bytes)", tarball.string(), tarballSize));
    mcpp::ui::status("SHA-256", sha);
    mcpp::ui::status("Xpkg",    xpkgPath.string());

    if (dry_run) {
        std::println("");
        std::println("--- xpkg.lua content ---");
        std::print("{}", lua);
        std::println("--- end ---");
    }

    // 5. Print step-by-step PR instructions.
    char first = pkg.name.empty() ? '?' : pkg.name[0];
    std::println("");
    std::println("Next steps to publish to mcpp-index:");
    std::println("");
    std::println("  1. Tag this commit and push:");
    std::println("       git tag -a v{0} -m \"v{0}\"", pkg.version);
    std::println("       git push --tags");
    std::println("");
    std::println("  2. Upload the tarball to your repo's GitHub Release:");
    std::println("       URL: {}/releases/new?tag=v{}", pkg.repo, pkg.version);
    std::println("       Attach: {}", tarball.string());
    std::println("");
    std::println("  3. Open a PR to mcpp-index:");
    std::println("       Fork:  https://github.com/mcpp-community/mcpp-index");
    std::println("       Add:   pkgs/{}/{}.lua", first, pkg.name);
    std::println("       (file content is in {})", xpkgPath.string());
    std::println("");
    // TODO(post-v0.0.3): if `gh` CLI is on PATH and authenticated, offer
    //   `mcpp publish --auto` to:
    //     - gh release create v<v> <tarball>
    //     - fork mcpp-index, add pkg lua, gh pr create
    //   See docs/34-release-readiness.md §3.
    std::println("Tip: future versions of mcpp may automate steps 2-3 via the gh CLI.");
    return 0;
}

// ─── M5: mcpp pack — bundle build output into a self-contained tarball ───
//
// Three modes (see docs/35-pack-design.md):
//   static          full musl static, no dynamic deps
//   bundle-project  default; only bundle non-system .so
//   bundle-all      bundle every dynamic dep including libc / libstdc++
export int cmd_pack(const mcpplibs::cmdline::ParsedArgs& parsed) {
    // ─── Resolve mode ────────────────────────────────────────────────
    mcpp::pack::Options opts;
    bool modeFromUser = false;
    if (auto v = parsed.value("mode")) {
        auto m = mcpp::pack::parse_mode(*v);
        if (!m) {
            mcpp::ui::error(std::format(
                "invalid --mode '{}'; expected static | bundle-project | bundle-all", *v));
            return 2;
        }
        opts.mode = *m;
        modeFromUser = true;
    }
    if (auto v = parsed.value("format")) {
        if (*v == "tar")      opts.format = mcpp::pack::Format::Tar;
        else if (*v == "dir") opts.format = mcpp::pack::Format::Dir;
        else {
            mcpp::ui::error(std::format(
                "invalid --format '{}'; expected tar | dir", *v));
            return 2;
        }
    }
    if (auto v = parsed.value("output")) opts.output = *v;
    if (auto v = parsed.value("target")) opts.targetTriple = *v;

    // `--target *-linux-musl` without an explicit `--mode` implies
    // `--mode static` — packaging a musl-static ELF as bundle-project
    // would feed patchelf a static binary and crash. The docs treat
    // this pair as equivalent; surface it in the code path too.
    if (!modeFromUser && opts.targetTriple.find("-musl") != std::string::npos) {
        opts.mode     = mcpp::pack::Mode::Static;
        modeFromUser  = true;   // user-equivalent intent — block manifest override
    }

    // ─── Build first (pack implies a fresh build) ────────────────────
    BuildOverrides ov;
    if (opts.mode == mcpp::pack::Mode::Static && opts.targetTriple.empty())
        ov.target_triple = "x86_64-linux-musl";
    else
        ov.target_triple = opts.targetTriple;

    auto ctx = prepare_build(/*print_fp=*/false, /*includeDevDeps=*/false,
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
        BuildOverrides ov2;
        ov2.target_triple = "x86_64-linux-musl";
        auto ctx2 = prepare_build(false, false, {}, ov2);
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
        make_bootstrap_progress_callback());
    if (!cfg) { mcpp::ui::error(cfg.error().message); return 4; }

    // ─── Build the plan + run ────────────────────────────────────────
    auto plan = mcpp::pack::make_plan(ctx->manifest, *cfg, opts,
        mainBinary, ctx->projectRoot, ctx->tc.targetTriple);
    if (!plan) { mcpp::ui::error(plan.error().message); return 1; }

    mcpp::ui::info("Packing", std::format("{} v{} ({})",
        plan->packageName, plan->packageVersion,
        mcpp::pack::mode_name(plan->opts.mode)));

    auto r = mcpp::pack::run(*plan, *cfg);
    if (!r) {
        mcpp::ui::error(r.error().message);
        return 1;
    }

    auto pathCtx = make_path_ctx(&*cfg, ctx->projectRoot);
    auto outPath = (opts.format == mcpp::pack::Format::Tar)
        ? plan->tarballPath : plan->stagingRoot;
    mcpp::ui::status("Packed", mcpp::ui::shorten_path(outPath, pathCtx));
    return 0;
}

} // namespace mcpp::cli
