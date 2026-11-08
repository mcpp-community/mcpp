// mcpp.cli.cmd_patch — the implementation. An implementation unit, so that the
// modules it reads are not reachable from the CLI's interface: imported by the
// interface, GCC 16.1 ended with an internal compiler error at `import mcpp.cli;`
// in main.cpp (measured 2026-11-09, as for mcpp.build.flag_origins).
module mcpp.cli.cmd_patch;

import std;
import mcpplibs.cmdline;
import mcpp.build.prepare;
import mcpp.manifest;
import mcpp.project;
import mcpp.pm.patch;
import mcpp.ui;

namespace mcpp::cli {

namespace {

namespace fs = std::filesystem;

struct Edge {
    std::string                     key;
    mcpp::manifest::DependencySpec  spec;
};

// The root's edge to `name`: its key, or its short name.
std::optional<Edge> edge_of(const mcpp::manifest::Manifest& m, std::string_view name) {
    for (auto const& [key, spec] : m.dependencies)
        if (key == name || spec.shortName == name) return Edge{ key, spec };
    return std::nullopt;
}

fs::path edit_dir(const fs::path& project, std::string_view key) {
    return project / "target" / "patch" / std::string(key);
}

fs::path base_dir(const fs::path& project, std::string_view key) {
    return project / "target" / "patch" / (std::string(key) + ".base");
}

int fail(std::string_view text) {
    mcpp::ui::error(text);
    return 2;
}

}  // namespace

int cmd_patch_edit(const mcpplibs::cmdline::ParsedArgs& parsed) {
    const std::string name = parsed.positional(0);
    if (name.empty()) return fail("mcpp patch edit needs a dependency: mcpp patch edit <dependency>");
    auto ctx = mcpp::build::prepare_build(false, false, {}, {});
    if (!ctx) return fail(ctx.error());
    auto edge = edge_of(ctx->manifest, name);
    if (!edge) return fail(std::format("'{}' is not a dependency of {}", name, ctx->manifest.package.name));
    if (edge->spec.isPath())
        return fail(std::format("'{}' is a path dependency: its tree is the project's to edit", name));
    // The package by its name, or a patched variant's (`<name>+p<digest>`).
    auto root = std::ranges::find_if(ctx->packageRoots, [&](auto const& e) {
        for (auto const& n : { edge->spec.shortName, edge->spec.namespace_ + "." + edge->spec.shortName })
            if (e.first == n || e.first.starts_with(n + "+p")) return true;
        return false;
    });
    if (root == ctx->packageRoots.end()) return fail(std::format("the build did not read '{}'", name));
    const auto work = edit_dir(ctx->projectRoot, edge->key), base = base_dir(ctx->projectRoot, edge->key);
    std::error_code ec;
    if (fs::exists(work, ec))
        return fail(std::format("'{}' is being edited at {}: `mcpp patch save {}` writes its patch", name,
                                work.string(), name));
    fs::create_directories(work.parent_path(), ec);
    const auto real = fs::weakly_canonical(root->second, ec);
    for (auto const& to : { base, work }) {
        fs::copy(real, to, fs::copy_options::recursive, ec);
        if (ec) return fail(std::format("cannot copy {} to {}: {}", real.string(), to.string(), ec.message()));
    }
    mcpp::ui::info("Editing", std::format("{} at {}", name, work.string()));
    mcpp::ui::line(std::format("        the tree is the package's root, its {} patch(es) applied; "
                               "`mcpp patch save {}` writes the change", edge->spec.patches.size(), name));
    return 0;
}

int cmd_patch_save(const mcpplibs::cmdline::ParsedArgs& parsed) {
    const std::string name = parsed.positional(0);
    if (name.empty()) return fail("mcpp patch save needs a dependency: mcpp patch save <dependency>");
    auto project = mcpp::project::find_manifest_root(fs::current_path());
    if (!project) return fail("no mcpp.toml in this directory or its parents");
    const auto manifestPath = *project / "mcpp.toml";
    auto m = mcpp::manifest::load(manifestPath);
    if (!m) return fail(m.error().format());
    auto edge = edge_of(*m, name);
    if (!edge) return fail(std::format("'{}' is not a dependency of {}", name, m->package.name));
    const auto work = edit_dir(*project, edge->key), base = base_dir(*project, edge->key);
    std::error_code ec;
    if (!fs::is_directory(work, ec) || !fs::is_directory(base, ec))
        return fail(std::format("'{}' is not being edited: `mcpp patch edit {}` begins", name, name));
    const auto diff = mcpp::pm::patch::diff_trees(base, work);
    if (diff.empty()) {
        mcpp::ui::info("Unchanged", std::format("{}: the tree is as it was; no patch is written", name));
        return 0;
    }
    // The next patch of the edge, named after the package and its place in the set.
    const auto stem = edge->spec.shortName.empty() ? edge->key : edge->spec.shortName;
    fs::path rel;
    for (std::size_t n = edge->spec.patches.size() + 1;; ++n) {
        rel = fs::path("patches") / std::format("{}-{:04}.patch", stem, n);
        if (!fs::exists(*project / rel, ec)) break;
    }
    fs::create_directories((*project / rel).parent_path(), ec);
    {
        std::ofstream out(*project / rel, std::ios::binary);
        out << diff;
        if (!out) return fail(std::format("cannot write {}", (*project / rel).string()));
    }
    std::vector<std::string> patches;
    for (auto const& p : edge->spec.patches)
        patches.push_back(fs::path(p).lexically_relative(*project).generic_string());
    patches.push_back(rel.generic_string());
    bool recorded = false;
    if (!edge->spec.isGit() && !edge->spec.version.empty()) {
        std::ifstream in(manifestPath, std::ios::binary);
        const std::string text{std::istreambuf_iterator<char>(in), {}};
        auto edited = mcpp::manifest::upsert_dependency_text(text, {
            .namespace_ = edge->spec.namespace_.empty() ? std::string("mcpplibs") : edge->spec.namespace_,
            .shortName = edge->spec.shortName,
            .version = edge->spec.version,
            .features = edge->spec.features,
            .patches = patches,
        });
        if (edited) {
            std::ofstream out(manifestPath, std::ios::binary | std::ios::trunc);
            out << *edited;
            recorded = bool(out);
        }
    }
    fs::remove_all(work, ec);
    fs::remove_all(base, ec);
    mcpp::ui::info("Saved", std::format("{} ({} bytes)", rel.generic_string(), diff.size()));
    if (recorded) {
        mcpp::ui::line(std::format("        mcpp.toml: {} patches = [{} file(s)]", edge->key, patches.size()));
    } else {
        std::string list;
        for (auto const& p : patches) list += std::format("{}\"{}\"", list.empty() ? "" : ", ", p);
        mcpp::ui::line(std::format("        add it to the edge in mcpp.toml: patches = [{}]", list));
    }
    return 0;
}

}  // namespace mcpp::cli
