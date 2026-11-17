// mcpp.publish.pipeline — the publish pipeline (tarball + sha256 + xpkg.lua +
// next-step instructions) and xpkg emission to file/stdout.
// Both read the effective manifest of the project and emit from the
// normalised one (#690, mcpp.publish.normalize).

export module mcpp.publish.pipeline;

import std;
import mcpplibs.mwrap.libc;
import mcpp.manifest;
import mcpp.modgraph.scanner;
import mcpp.run;
import mcpp.project;
import mcpp.publish.normalize;
import mcpp.publish.xpkg_emit;
import mcpp.ui;

namespace libc = mcpplibs::mwrap::libc::macros;

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
        std::println(libc::stderr(), "error: no mcpp.toml found");
        return 2;
    }
    // The effective manifest (#690): a workspace member reads as it builds.
    auto eff = mcpp::project::load_effective_manifest(*root);
    if (!eff) { std::println(libc::stderr(), "error: {}", eff.error()); return 2; }
    auto normalized = normalize_for_publish(*root, *eff);
    if (!normalized) { std::println(libc::stderr(), "error: {}", normalized.error()); return 2; }
    auto m = std::optional<mcpp::manifest::Manifest>(descriptor_manifest(*eff, *normalized));
    auto scan = mcpp::modgraph::scan_package(*root, *m);
    if (!scan.errors.empty()) {
        for (auto& e : scan.errors) std::println(libc::stderr(), "error: {}", e.format());
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
        if (!os) { std::println(libc::stderr(), "error: cannot write '{}'", output.string()); return 1; }
        os << lua;
        mcpp::ui::line(std::format("Wrote {}", output.string()));
    }
    return 0;
}

// What `mcpp publish` is asked (mcpp#947).
export struct PublishRequest {
    bool dryRun = false;
    bool allowDirty = false;
    // The tag's template (`{version}`, `{name}`, `{namespace}`); empty: `v{version}`.
    std::string tag;
    // The members of a workspace to publish, as `[workspace] members` spells
    // them, and its root. Empty: the package the command is run in.
    std::filesystem::path    workspaceRoot;
    std::vector<std::string> members;
};

namespace {

struct Published {
    std::string identity, version, tag, repo, sha;
    std::filesystem::path archive, descriptor;
    std::string descriptorFile;   // its name in an index
    std::string lua;
};

// One package into `distDir`: its archive, named by its identity, and its
// descriptor.
std::expected<Published, int> publish_one(const std::filesystem::path& root,
                                          const std::filesystem::path& distDir,
                                          const PublishRequest& req) {
    // The effective manifest (#690), and the normalised form the archive
    // carries: a workspace member's own file is valid only inside its
    // workspace, and the archive contains the member alone.
    auto eff = mcpp::project::load_effective_manifest(root);
    if (!eff) {
        mcpp::ui::error(std::format("manifest parse: {}", eff.error()));
        return std::unexpected(2);
    }
    auto normalized = normalize_for_publish(root, *eff);
    if (!normalized) {
        mcpp::ui::error(normalized.error());
        return std::unexpected(2);
    }
    auto m = std::optional<mcpp::manifest::Manifest>(descriptor_manifest(*eff, *normalized));
    auto scan = mcpp::modgraph::scan_package(root, *m);
    if (!scan.errors.empty()) {
        for (auto& e : scan.errors) mcpp::ui::error(e.format());
        return std::unexpected(2);
    }

    auto& pkg = m->package;
    Published out;
    out.identity = mcpp::publish::archive_identity(pkg.namespace_, pkg.name);
    out.version = pkg.version;
    out.repo = pkg.repo;
    out.descriptorFile = mcpp::publish::descriptor_file_name(pkg.namespace_, pkg.name);
    const auto prefix = pkg.namespace_.empty() ? std::string{} : pkg.namespace_ + ".";
    auto tag = mcpp::publish::release_tag(req.tag, pkg.namespace_,
        pkg.name.starts_with(prefix) ? pkg.name.substr(prefix.size()) : pkg.name, pkg.version);
    if (!tag) { mcpp::ui::error(tag.error()); return std::unexpected(2); }
    out.tag = *tag;
    mcpp::ui::status("Packaging", std::format("{} v{}", out.identity, pkg.version));

    std::error_code ec;
    std::filesystem::create_directories(distDir, ec);
    out.archive = distDir / std::format("{}-{}.tar.gz", out.identity, pkg.version);
    out.descriptor = distDir / out.descriptorFile;

    // 1. Pack source via `git archive` (respects .gitignore). A normalised
    //    manifest replaces `mcpp.toml` and the file as written is kept beside
    //    it as `mcpp.toml.orig`; an unchanged one leaves the archive exactly as
    //    `git archive HEAD` produces it.
    std::vector<mcpp::pm::ArchiveOverlay> overlays;
    if (normalized->changed) {
        overlays.push_back({ "mcpp.toml",      normalized->text });
        overlays.push_back({ "mcpp.toml.orig", normalized->original });
        auto manifestCopy = distDir / std::format("{}-{}.mcpp.toml", out.identity, pkg.version);
        std::ofstream os(manifestCopy, std::ios::binary);
        os << normalized->text;
        if (!os) {
            mcpp::ui::error(std::format("cannot write '{}'", manifestCopy.string()));
            return std::unexpected(1);
        }
        mcpp::ui::status("Manifest",
            std::format("{} (normalised; the archive carries it as mcpp.toml)",
                        manifestCopy.string()));
    }
    if (auto err = mcpp::publish::make_release_tarball(
            root, out.identity, pkg.version, out.archive, overlays);
        !err.empty())
    {
        mcpp::ui::error(std::format("tarball: {}", err));
        return std::unexpected(1);
    }
    auto tarballSize = std::filesystem::file_size(out.archive, ec);

    // 2. Compute SHA-256.
    out.sha = mcpp::publish::sha256_of_file(out.archive);
    if (out.sha.empty()) {
        mcpp::ui::error("sha256: failed to hash tarball (is `sha256sum` installed?)");
        return std::unexpected(1);
    }

    // 3. The release's URL by convention, from manifest.repo and the tag.
    auto url = mcpp::publish::release_tarball_url(pkg.repo, out.identity, pkg.version, out.tag);
    if (url.empty()) {
        mcpp::ui::error(std::format(
            "cannot derive tarball URL: [package].repo='{}' is empty or not "
            "a https URL. Set [package].repo in mcpp.toml.", pkg.repo));
        return std::unexpected(1);
    }

    // 4. Build release info + emit xpkg.lua.
    auto release = mcpp::publish::make_release_info(pkg.version, url, out.sha);
    out.lua = mcpp::publish::emit_xpkg(*m, scan.graph, release);
    {
        std::ofstream os(out.descriptor);
        os << out.lua;
        if (!os) {
            mcpp::ui::error(std::format("cannot write '{}'", out.descriptor.string()));
            return std::unexpected(1);
        }
    }
    mcpp::ui::status("Tarball", std::format("{} ({} bytes)", out.archive.string(), tarballSize));
    mcpp::ui::status("SHA-256", out.sha);
    mcpp::ui::status("Xpkg",    out.descriptor.string());
    return out;
}

} // namespace

// `mcpp publish [--dry-run] [--allow-dirty] [--tag TEMPLATE] [--workspace | -p MEMBER...]`.
export int publish_package(const PublishRequest& req) {
    auto here = mcpp::project::find_manifest_root(std::filesystem::current_path());
    if (!here) { mcpp::ui::error("no mcpp.toml in current dir or parents"); return 2; }
    const auto top = req.members.empty() ? *here : req.workspaceRoot;

    // Sanity: working tree clean (best-effort via git status).
    if (!req.allowDirty && std::filesystem::exists(top / ".git")) {
        auto gitStatus = mcpp::run::execute({ "git", "-C", top.string(), "status", "--porcelain" });
        std::string out = gitStatus.output;
        if (!out.empty()) {
            mcpp::ui::error("working tree has uncommitted changes; pass --allow-dirty to skip this check");
            std::println(libc::stderr(), "{}", out);
            return 1;
        }
    }

    // Every archive of one run is in one directory, the workspace's where
    // members are published: that directory is what a release attaches.
    const auto distDir = top / "target" / "dist";
    std::vector<Published> done;
    if (req.members.empty()) {
        auto one = publish_one(*here, distDir, req);
        if (!one) return one.error();
        done.push_back(std::move(*one));
    }
    for (auto const& member : req.members) {
        auto one = publish_one((req.workspaceRoot / member).lexically_normal(), distDir, req);
        if (!one) return one.error();
        // Two packages under one tag with one archive name would be one asset.
        for (auto const& before : done)
            if (before.tag == one->tag && before.archive == one->archive) {
                mcpp::ui::error(std::format(
                    "two members are published as '{}' under the tag '{}': their identity and "
                    "version are equal", one->archive.filename().string(), one->tag));
                return 2;
            }
        done.push_back(std::move(*one));
    }

    // SHA256SUMS beside the archives, one per tag (`sha256sum -c` reads it).
    std::map<std::string, std::string> sums;
    for (auto const& p : done)
        sums[p.tag] += std::format("{}  {}\n", p.sha, p.archive.filename().string());
    if (!req.members.empty())
        for (auto const& [tag, text] : sums) {
            const auto file = distDir / (sums.size() == 1 ? std::string("SHA256SUMS") : std::format("SHA256SUMS-{}", tag));
            std::ofstream(file, std::ios::binary | std::ios::trunc) << text;
            mcpp::ui::status("Checksums", file.string());
        }

    if (req.dryRun)
        for (auto const& p : done) {
            std::println("");
            if (req.members.empty()) std::println("--- xpkg.lua content ---");
            else                     std::println("--- {} ---", p.descriptorFile);
            std::print("{}", p.lua);
            std::println("--- end ---");
        }

    // Step-by-step PR instructions: guidance about what to do next, which is
    // narration. The documents a `--dry-run` prints above are the result.
    mcpp::ui::line("");
    mcpp::ui::line("Next steps to publish to mcpp-index:");
    for (auto const& [tag, text] : sums) {
        mcpp::ui::line("");
        mcpp::ui::line(std::format("  1. Tag this commit and push:"));
        mcpp::ui::line(std::format("       git tag -a {0} -m \"{0}\"", tag));
        mcpp::ui::line("       git push --tags");
        mcpp::ui::line("");
        mcpp::ui::line("  2. Upload to your repo's GitHub Release:");
        std::string repo;
        for (auto const& p : done) if (p.tag == tag) {
            if (repo.empty()) { repo = p.repo; mcpp::ui::line(std::format("       URL: {}/releases/new?tag={}", repo, tag)); }
            mcpp::ui::line(std::format("       Attach: {}", p.archive.string()));
        }
    }
    mcpp::ui::line("");
    mcpp::ui::line("  3. Open a PR to mcpp-index:");
    mcpp::ui::line("       Fork:  https://github.com/mcpplibs/mcpp-index");
    for (auto const& p : done) {
        mcpp::ui::line(std::format("       Add:   pkgs/{}/{}", p.descriptorFile.empty() ? '?' : p.descriptorFile[0], p.descriptorFile));
        mcpp::ui::line(std::format("       (file content is in {})", p.descriptor.string()));
    }
    mcpp::ui::line("");
    return 0;
}

} // namespace mcpp::publish
