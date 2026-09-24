// mcpp.publish.normalize — the manifest a published archive carries.
//
// A workspace member's `mcpp.toml` is valid only inside its workspace: it may
// omit `version` and `license` because `[workspace.package]` supplies them, its
// compile flags may come from `[workspace.build]`, and it may reach a sibling
// through `path = "../util"`. The archive `mcpp publish` produces contains the
// member directory alone, and a consumer reads that file as written. Before
// #690 the archive therefore carried a manifest that no consumer could build:
// the inherited configuration was absent and the sibling edge pointed outside
// the archive (design record 2026-09-25, section 3.6).
//
// THE PUBLISHED MANIFEST IS NORMALISED, NOT REWRITTEN. The raw TOML tree of the
// member is the starting point, and only three kinds of edit are made to it:
//
//   1. Inherited values are written back where the member did not declare
//      them. The values are read from the EFFECTIVE manifest that
//      `load_effective_manifest` produced, so the inheritance rule is applied
//      by the one function that applies it for a build and is not restated
//      here.
//   2. A dependency written `workspace = true` receives the source the
//      workspace resolved for it, read from the same effective manifest.
//   3. A `path` edge that leaves the package directory is replaced by its
//      `version`, or refused when it has none. A `path` edge that stays inside
//      the directory is part of the archive and is kept.
//
// Anything else in the file, including keys this version does not know, passes
// through unchanged. A manifest that needs none of the edits is published
// byte for byte as it is on disk.

export module mcpp.publish.normalize;

import std;
import mcpp.libs.toml;
import mcpp.manifest;
import mcpp.project;

export namespace mcpp::publish {

struct NormalizedManifest {
    std::string              text;      // the manifest the archive carries
    std::string              original;  // the file as it is on disk
    bool                     changed = false;
    // `text` read back by the manifest parser. Its dependency maps are the
    // ones a consumer of the archive resolves, so the descriptor's `deps` are
    // emitted from them.
    mcpp::manifest::Manifest manifest;
};

std::expected<NormalizedManifest, std::string>
normalize_for_publish(const std::filesystem::path&           packageDir,
                      const mcpp::project::EffectiveManifest& effective);

} // namespace mcpp::publish

namespace mcpp::publish {

namespace {

namespace t = mcpp::libs::toml;

// `[workspace.build]` keys, each paired with the field of `BuildConfig` that
// carries it. Vectors of flags, vectors of directories and scalars are written
// back by three different rules, so the table records which rule applies.
struct StringVectorKey {
    std::string_view key;
    std::vector<std::string> mcpp::manifest::BuildConfig::* field;
};
struct PathVectorKey {
    std::string_view key;
    std::vector<std::filesystem::path> mcpp::manifest::BuildConfig::* field;
};
struct ScalarKey {
    std::string_view key;
    std::string mcpp::manifest::BuildConfig::* field;
};

using BC = mcpp::manifest::BuildConfig;

const StringVectorKey kStringVectors[] = {
    {"cflags",           &BC::cflags},
    {"cxxflags",         &BC::cxxflags},
    {"ldflags",          &BC::ldflags},
    {"defines",          &BC::defines},
    {"dialect_cxxflags", &BC::dialectCxxflags},
};
const PathVectorKey kPathVectors[] = {
    {"include_dirs",         &BC::includeDirs},
    {"include_dirs_after",   &BC::includeDirsAfter},
    {"private_include_dirs", &BC::privateIncludeDirs},
};
const ScalarKey kScalars[] = {
    {"c_standard",              &BC::cStandard},
    {"linkage",                 &BC::linkage},
    {"target",                  &BC::target},
    {"cxx_runtime",             &BC::cxxRuntime},
    {"dependency_linkage",      &BC::dependencyLinkage},
    {"macos_deployment_target", &BC::macosDeploymentTarget},
    {"ios_deployment_target",   &BC::iosDeploymentTarget},
};

t::Value string_array(const std::vector<std::string>& v) {
    t::Array a;
    for (auto const& s : v) a.emplace_back(s);
    return t::Value{std::move(a)};
}

// The table at `key` inside `parent`, created (and recorded as a header) when
// absent. Returns nullptr when the key holds something other than a table.
t::Table* table_at(t::Table& parent, const std::string& key,
                   const std::string& dottedPath,
                   std::set<std::string, std::less<>>& explicitTables) {
    auto it = parent.find(key);
    if (it == parent.end()) {
        parent[key] = t::Value{t::Table{}};
        explicitTables.insert(dottedPath);
        return &parent[key].as_table();
    }
    if (!it->second.is_table()) return nullptr;
    return &it->second.as_table();
}

// Same discriminator the manifest reader uses: a dependency table that names a
// source is a specification; any other table is a namespace or a selector.
bool names_a_source(const t::Table& sub) {
    for (auto const& [k, v] : sub)
        if (k == "path" || k == "version" || k == "git" || k == "workspace")
            return true;
    return false;
}

std::string join(const std::vector<std::string>& segs) {
    std::string out;
    for (auto const& s : segs) out += (out.empty() ? "" : ".") + s;
    return out;
}

// Is `target` inside `dir`? Both are compared lexically: the archive is built
// from the tracked tree, whose paths are what a consumer receives.
bool inside(const std::filesystem::path& dir, const std::filesystem::path& target) {
    auto rel = target.lexically_normal().lexically_relative(dir.lexically_normal());
    if (rel.empty()) return false;
    auto first = *rel.begin();
    return first != "..";
}

struct Walk {
    const std::filesystem::path&           packageDir;
    const mcpp::project::EffectiveManifest& effective;
    const t::Document&                      raw;
    const std::filesystem::path             manifestPath;
    bool                                    changed = false;

    // The map key the manifest reader assigns to the dependency at `segs`
    // inside `section`. Obtained by letting the reader parse a document that
    // declares only this dependency, so the key rules (namespace tables,
    // dotted selectors, quoted legacy keys) are not restated here.
    std::expected<std::string, std::string>
    reader_key(std::string_view section, const std::vector<std::string>& segs,
               const t::Value& leaf) const {
        t::Table root;
        root["package"] = t::Value{t::Table{
            {"name", t::Value{std::string("normalize")}},
            {"version", t::Value{std::string("0.0.0")}}}};
        std::set<std::string, std::less<>> ex{ "package", std::string(section) };
        // Build the pruned subtree from the leaf upward.
        t::Value node = leaf;
        for (std::size_t i = segs.size(); i-- > 1;) {
            t::Table parent;
            parent[segs[i]] = node;
            node = t::Value{std::move(parent)};
        }
        t::Table sectionTable;
        sectionTable[segs[0]] = node;
        root[std::string(section)] = t::Value{std::move(sectionTable)};
        std::string prefix(section);
        for (std::size_t i = 0; i + 1 < segs.size(); ++i) {
            prefix += "." + segs[i];
            if (raw.has_explicit_table(prefix)) ex.insert(prefix);
        }
        auto text = t::serialize(t::Document{std::move(root), std::move(ex)});
        auto m = mcpp::manifest::parse_string(text, manifestPath);
        if (!m) return std::unexpected(m.error().format());
        const auto& map = section == "dependencies"     ? m->dependencies
                        : section == "build-dependencies" ? m->buildDependencies
                                                          : m->devDependencies;
        if (map.size() != 1)
            return std::unexpected(std::format(
                "internal error: the dependency at [{}] {} did not read back as "
                "exactly one entry (please report)", section, join(segs)));
        return map.begin()->first;
    }

    const std::map<std::string, mcpp::manifest::DependencySpec>&
    effective_map(std::string_view section) const {
        const auto& m = effective.manifest;
        return section == "dependencies"       ? m.dependencies
             : section == "build-dependencies" ? m.buildDependencies
                                               : m.devDependencies;
    }

    // Replace `workspace = true` by the source the workspace resolved.
    std::expected<void, std::string>
    resolve_workspace(t::Table& spec, std::string_view section,
                      const std::vector<std::string>& segs, bool conditional) {
        auto it = spec.find("workspace");
        if (it == spec.end() || !it->second.is_bool() || !it->second.as_bool())
            return {};
        if (conditional)
            return std::unexpected(std::format(
                "{}: [{}] '{}' inherits from [workspace.dependencies] inside a "
                "conditional section, and a published manifest cannot carry "
                "that inheritance. State its source (version, path or git) in "
                "the member's manifest.",
                manifestPath.string(), section, join(segs)));
        if (!effective.member) return {};
        auto key = reader_key(section, segs, t::Value{spec});
        if (!key) return std::unexpected(key.error());
        const auto& map = effective_map(section);
        auto found = map.find(*key);
        if (found == map.end() || found->second.inheritWorkspace)
            return std::unexpected(std::format(
                "{}: [{}] '{}' is declared `workspace = true`, and "
                "[workspace.dependencies] of '{}' does not name it.",
                manifestPath.string(), section, join(segs),
                (effective.workspaceRoot / "mcpp.toml").string()));
        const auto& s = found->second;
        for (auto k : {"workspace", "version", "path", "git", "rev", "tag", "branch"})
            spec.erase(std::string(k));
        if (!s.version.empty()) spec["version"] = t::Value{s.version};
        if (!s.path.empty())    spec["path"]    = t::Value{s.path};
        if (!s.git.empty()) {
            spec["git"] = t::Value{s.git};
            if (!s.gitRev.empty())
                spec[s.gitRefKind.empty() ? std::string("rev") : s.gitRefKind] =
                    t::Value{s.gitRev};
        }
        changed = true;
        return {};
    }

    // Apply the path rule. Returns true when the entry is to be removed.
    std::expected<bool, std::string>
    apply_path_rule(t::Table& spec, std::string_view section,
                    const std::vector<std::string>& segs, bool dev) {
        auto it = spec.find("path");
        if (it == spec.end() || !it->second.is_string()) return false;
        std::filesystem::path p(it->second.as_string());
        auto target = p.is_absolute() ? p : packageDir / p;
        if (inside(packageDir, target)) {
            if (p.is_absolute()) {
                // Only reachable through `workspace = true`: the workspace
                // anchors the path, and an absolute path cannot be published.
                spec["path"] = t::Value{target.lexically_normal()
                    .lexically_relative(packageDir.lexically_normal()).generic_string()};
                changed = true;
            }
            return false;
        }
        if (spec.contains("version")) {
            spec.erase(std::string("path"));
            changed = true;
            return false;
        }
        if (dev) {
            // A consumer never resolves a development dependency, and one
            // that exists only as a path has no published form.
            changed = true;
            return true;
        }
        const auto name = join(segs);
        if (effective.member && effective.workspace
            && mcpp::project::is_workspace_member(*effective.workspace,
                                                  effective.workspaceRoot, target)) {
            std::string version = "<version>";
            if (auto sib = mcpp::project::load_effective_manifest(target);
                sib && !sib->manifest.package.version.empty())
                version = sib->manifest.package.version;
            return std::unexpected(std::format(
                "{}: [{}] '{}' reaches the workspace member '{}' by path, and "
                "the published archive contains only this package's directory. "
                "State the version the member is published under beside the "
                "path:\n\n    {} = {{ path = \"{}\", version = \"{}\" }}\n",
                manifestPath.string(), section, name,
                target.lexically_normal().string(), name, it->second.as_string(),
                version));
        }
        return std::unexpected(std::format(
            "{}: [{}] '{}' is a path dependency outside this package ('{}'), "
            "which a consumer of the published archive cannot reach. Depend on "
            "a published version instead, or state `version` beside `path`.",
            manifestPath.string(), section, name, it->second.as_string()));
    }

    // Walk one dependency table. `segs` is the key path below the section.
    std::expected<void, std::string>
    deps(t::Table& table, std::string_view section, std::vector<std::string> segs,
         bool conditional, bool dev) {
        std::vector<std::string> remove;
        for (auto& [k, v] : table) {
            auto here = segs;
            here.push_back(k);
            if (!v.is_table()) continue;              // a version string
            auto& sub = v.as_table();
            if (!names_a_source(sub)) {
                if (auto r = deps(sub, section, here, conditional, dev); !r) return r;
                continue;
            }
            if (auto r = resolve_workspace(sub, section, here, conditional); !r)
                return std::unexpected(r.error());
            auto drop = apply_path_rule(sub, section, here, dev);
            if (!drop) return std::unexpected(drop.error());
            if (*drop) remove.push_back(k);
        }
        for (auto const& k : remove) table.erase(k);
        return {};
    }
};

} // namespace

std::expected<NormalizedManifest, std::string>
normalize_for_publish(const std::filesystem::path&           packageDir,
                      const mcpp::project::EffectiveManifest& effective) {
    const auto manifestPath = packageDir / "mcpp.toml";
    NormalizedManifest out;
    {
        std::ifstream is(manifestPath, std::ios::binary);
        if (!is) return std::unexpected(std::format("cannot open '{}'", manifestPath.string()));
        std::stringstream ss;
        ss << is.rdbuf();
        out.original = ss.str();
    }
    auto doc = t::parse(out.original);
    if (!doc) return std::unexpected(std::format("{}:{}:{}: {}", manifestPath.string(),
        doc.error().where.line, doc.error().where.column, doc.error().message));
    auto& root = doc->root();
    auto& explicitTables = doc->explicit_tables();
    bool changed = false;

    if (effective.member && effective.workspace) {
        const auto& ws  = *effective.workspace;
        const auto& inh = ws.workspace.inherited;
        const auto& eff = effective.manifest;

        // [package]: the fields the member left to the workspace.
        auto* pkg = table_at(root, "package", "package", explicitTables);
        if (!pkg) return std::unexpected(std::format(
            "{}: `package` is not a table", manifestPath.string()));
        auto put_string = [&](std::string_view key, const std::string& value) {
            if (value.empty() || pkg->contains(key)) return;
            (*pkg)[std::string(key)] = t::Value{value};
            changed = true;
        };
        put_string("version",     eff.package.version);
        put_string("license",     eff.package.license);
        put_string("description", eff.package.description);
        put_string("repo",        eff.package.repo);
        if (!eff.package.authors.empty() && !pkg->contains("authors")) {
            (*pkg)["authors"] = string_array(eff.package.authors);
            changed = true;
        }
        // `standard` may also be declared under the deprecated `[language]`,
        // so declaredness is read from the parser rather than from the tree.
        auto rawManifest = mcpp::manifest::parse_string(out.original, manifestPath,
                                                        {.insideWorkspace = true});
        if (!rawManifest) return std::unexpected(rawManifest.error().format());
        if (!rawManifest->package.standardDeclared && inh.standardDeclared) {
            (*pkg)["standard"] = t::Value{eff.package.standard};
            changed = true;
        }

        // [build]: the keys of [workspace.build], with the member's own
        // entries after the workspace's, as `inherit_workspace_build` ordered
        // them.
        if (inh.buildPresent) {
            const auto& w = inh.build;
            const auto& b = eff.buildConfig;
            t::Table* build = nullptr;
            auto build_table = [&]() -> t::Table* {
                if (!build) build = table_at(root, "build", "build", explicitTables);
                return build;
            };
            for (auto const& k : kStringVectors) {
                if ((w.*k.field).empty()) continue;
                auto* bt = build_table();
                if (!bt) return std::unexpected(std::format(
                    "{}: `build` is not a table", manifestPath.string()));
                (*bt)[std::string(k.key)] = string_array(b.*k.field);
                changed = true;
            }
            for (auto const& k : kPathVectors) {
                const auto n = (w.*k.field).size();
                if (n == 0) continue;
                std::vector<std::string> dirs;
                const auto& all = b.*k.field;
                for (std::size_t i = 0; i < all.size(); ++i) {
                    if (i >= n) { dirs.push_back(all[i].string()); continue; }
                    if (!inside(packageDir, all[i]))
                        return std::unexpected(std::format(
                            "{}: [workspace.build] {} entry '{}' resolves to '{}', "
                            "outside this package's directory, and the published "
                            "archive contains only that directory. Move the headers "
                            "into the package, or declare the directory in the "
                            "package's own [build] for its own build only.",
                            manifestPath.string(), k.key, (w.*k.field)[i].string(),
                            all[i].lexically_normal().string()));
                    dirs.push_back(all[i].lexically_normal()
                        .lexically_relative(packageDir.lexically_normal()).generic_string());
                }
                auto* bt = build_table();
                if (!bt) return std::unexpected(std::format(
                    "{}: `build` is not a table", manifestPath.string()));
                (*bt)[std::string(k.key)] = string_array(dirs);
                changed = true;
            }
            for (auto const& k : kScalars) {
                if ((w.*k.field).empty()) continue;
                auto* bt = build_table();
                if (!bt) return std::unexpected(std::format(
                    "{}: `build` is not a table", manifestPath.string()));
                if (bt->contains(k.key)) continue;
                (*bt)[std::string(k.key)] = t::Value{b.*k.field};
                changed = true;
            }
        }
    }

    // Dependencies: the unconditional sections and every conditional one.
    Walk walk{ packageDir, effective, *doc, manifestPath };
    for (auto section : {"dependencies", "build-dependencies", "dev-dependencies"}) {
        auto it = root.find(section);
        if (it == root.end() || !it->second.is_table()) continue;
        if (auto r = walk.deps(it->second.as_table(), section, {}, false,
                               std::string_view(section) == "dev-dependencies"); !r)
            return std::unexpected(r.error());
    }
    if (auto it = root.find("target"); it != root.end() && it->second.is_table()) {
        for (auto& [selector, row] : it->second.as_table()) {
            if (!row.is_table()) continue;
            for (auto section : {"dependencies", "build-dependencies", "dev-dependencies"}) {
                auto st = row.as_table().find(section);
                if (st == row.as_table().end() || !st->second.is_table()) continue;
                if (auto r = walk.deps(st->second.as_table(), section, {}, true,
                                       std::string_view(section) == "dev-dependencies"); !r)
                    return std::unexpected(r.error());
            }
        }
    }
    changed = changed || walk.changed;

    out.changed = changed;
    out.text = changed
        ? "# Normalised by `mcpp publish` from mcpp.toml.orig: the values this\n"
          "# package inherited from its workspace are written out, and every\n"
          "# dependency names a source a consumer can reach.\n"
          + t::serialize(*doc)
        : out.original;
    auto reread = mcpp::manifest::parse_string(out.text, manifestPath);
    if (!reread) return std::unexpected(std::format(
        "internal error: the normalised manifest of '{}' does not parse "
        "(please report): {}", manifestPath.string(), reread.error().format()));
    out.manifest = std::move(*reread);
    return out;
}

} // namespace mcpp::publish
