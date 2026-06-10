// mcpp.publish.pipeline — the publish pipeline (tarball + sha256 + xpkg.lua +
// next-step instructions) and xpkg emission to file/stdout.
// Bodies moved verbatim from the CLI layer. Zero behavior change.

module;
#include <cstdio>
#include <cstdlib>

export module mcpp.publish.pipeline;

import std;
import mcpp.manifest;
import mcpp.modgraph.scanner;
import mcpp.platform;
import mcpp.project;
import mcpp.publish.xpkg_emit;
import mcpp.ui;

namespace mcpp::publish {

// `mcpp emit xpkg [-V version] [-o output]`.
export int emit_xpkg_to(std::string version, const std::filesystem::path& output) {
    auto root = mcpp::project::find_manifest_root(std::filesystem::current_path());
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

// `mcpp publish [--dry-run] [--allow-dirty]`.
export int publish_package(bool dry_run, bool allow_dirty) {
    auto root = mcpp::project::find_manifest_root(std::filesystem::current_path());
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

} // namespace mcpp::publish
