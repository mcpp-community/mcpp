// mcpp.publish.pipeline — the publish pipeline (tarball + sha256 + xpkg.lua +
// next-step instructions) and xpkg emission to file/stdout.
// Both read the effective manifest of the project and emit from the
// normalised one (#690, mcpp.publish.normalize).

module;
#include <cstdio>
#include <cstdlib>

export module mcpp.publish.pipeline;

import std;
import mcpp.manifest;
import mcpp.modgraph.scanner;
import mcpp.platform;
import mcpp.project;
import mcpp.publish.normalize;
import mcpp.publish.xpkg_emit;
import mcpp.ui;

namespace mcpp::publish {

namespace {

// The manifest a descriptor is generated from: the effective manifest (what
// the package is), with the dependency maps of the normalised manifest (what a
// consumer of the published archive resolves). A sibling reached by
// `path` plus `version` is a version dependency there, so the descriptor lists
// it instead of dropping it with every other path edge.
mcpp::manifest::Manifest descriptor_manifest(const mcpp::project::EffectiveManifest& eff,
                                             const NormalizedManifest& normalized) {
    auto m = eff.manifest;
    if (normalized.changed) {
        m.dependencies      = normalized.manifest.dependencies;
        m.buildDependencies = normalized.manifest.buildDependencies;
        m.devDependencies   = normalized.manifest.devDependencies;
    }
    return m;
}

} // namespace

// `mcpp emit xpkg [-V version] [-o output] [--namespace NS]`.
export int emit_xpkg_to(std::string version, const std::filesystem::path& output,
                        std::string namespaceOverride = {}) {
    auto root = mcpp::project::find_manifest_root(std::filesystem::current_path());
    if (!root) {
        std::println(stderr, "error: no mcpp.toml found");
        return 2;
    }
    // The effective manifest (#690): a workspace member reads as it builds.
    auto eff = mcpp::project::load_effective_manifest(*root);
    if (!eff) { std::println(stderr, "error: {}", eff.error()); return 2; }
    auto normalized = normalize_for_publish(*root, *eff);
    if (!normalized) { std::println(stderr, "error: {}", normalized.error()); return 2; }
    auto m = std::optional<mcpp::manifest::Manifest>(descriptor_manifest(*eff, *normalized));
    auto scan = mcpp::modgraph::scan_package(*root, *m);
    if (!scan.errors.empty()) {
        for (auto& e : scan.errors) std::println(stderr, "error: {}", e.format());
        return 2;
    }

    if (version.empty()) version = m->package.version;

    // #278: `--namespace` lets a one-off archival run supply the namespace
    // without editing mcpp.toml. Without a namespace from either source the
    // emitted descriptor is namespace-less, and hand-adding one afterwards is
    // precisely what produces an uninstallable split-form descriptor — so say
    // so, loudly, at the moment of generation.
    if (!namespaceOverride.empty()) m->package.namespace_ = namespaceOverride;
    if (m->package.namespace_.empty()) {
        mcpp::ui::warning(std::format(
            "emitted descriptor declares no `namespace`. If you file this into a "
            "namespaced index, do NOT hand-add `namespace = \"<org>\"` — that "
            "makes `name` a split form the index can never resolve (mcpp#278). "
            "Declare `[package] namespace` in mcpp.toml or pass "
            "`--namespace <org>`, so both fields are emitted together."));
    }

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

    // The effective manifest (#690), and the normalised form the archive
    // carries: a workspace member's own file is valid only inside its
    // workspace, and the archive contains the member alone.
    auto eff = mcpp::project::load_effective_manifest(*root);
    if (!eff) {
        mcpp::ui::error(std::format("manifest parse: {}", eff.error()));
        return 2;
    }
    auto normalized = normalize_for_publish(*root, *eff);
    if (!normalized) {
        mcpp::ui::error(normalized.error());
        return 2;
    }
    auto m = std::optional<mcpp::manifest::Manifest>(descriptor_manifest(*eff, *normalized));
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

    // 1. Pack source via `git archive` (respects .gitignore). A normalised
    //    manifest replaces `mcpp.toml` and the file as written is kept beside
    //    it as `mcpp.toml.orig`; an unchanged one leaves the archive exactly as
    //    `git archive HEAD` produces it.
    std::vector<mcpp::pm::ArchiveOverlay> overlays;
    if (normalized->changed) {
        overlays.push_back({ "mcpp.toml",      normalized->text });
        overlays.push_back({ "mcpp.toml.orig", normalized->original });
        auto manifestCopy = distDir / std::format("{}-{}.mcpp.toml", pkg.name, pkg.version);
        std::ofstream os(manifestCopy, std::ios::binary);
        os << normalized->text;
        if (!os) {
            mcpp::ui::error(std::format("cannot write '{}'", manifestCopy.string()));
            return 1;
        }
        mcpp::ui::status("Manifest",
            std::format("{} (normalised; the archive carries it as mcpp.toml)",
                        manifestCopy.string()));
    }
    if (auto err = mcpp::publish::make_release_tarball(
            *root, pkg.name, pkg.version, tarball, overlays);
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
    std::println("       Fork:  https://github.com/mcpplibs/mcpp-index");
    std::println("       Add:   pkgs/{}/{}.lua", first, pkg.name);
    std::println("       (file content is in {})", xpkgPath.string());
    std::println("");
    // TODO(post-v0.0.3): if `gh` CLI is on PATH and authenticated, offer
    //   `mcpp publish --auto` to:
    //     - gh release create v<v> <tarball>
    //     - fork mcpp-index, add pkg lua, gh pr create
    //   See docs/11-publishing-a-library.md.
    std::println("Tip: future versions of mcpp may automate steps 2-3 via the gh CLI.");
    return 0;
}

} // namespace mcpp::publish
