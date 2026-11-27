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

// The `[workspace.build]` keys are read from `mcpp::manifest::kWorkspaceBuildKeys`,
// the one statement of the inheritable subset that the parser also reads.
// Vectors of flags, vectors of directories and scalars are written back by
// three different rules, selected by the type of the row's field.
using BC = mcpp::manifest::BuildConfig;

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
    reader_key(const mcpp::manifest::DependencyTable& table, const std::vector<std::string>& segs,
               const t::Value& leaf) const {
        const auto section = table.section();
        t::Table root;
        root["package"] = t::Value{t::Table{
            {"name", t::Value{std::string("normalize")}},
            {"version", t::Value{std::string("0.0.0")}}}};
        std::set<std::string, std::less<>> ex{ "package", section };
        // Build the pruned subtree from the leaf upward.
        t::Value node = leaf;
        for (std::size_t i = segs.size(); i-- > 1;) {
            t::Table parent;
            parent[segs[i]] = node;
            node = t::Value{std::move(parent)};
        }
        t::Table sectionTable;
        sectionTable[segs[0]] = node;
        if (table.kind == mcpp::manifest::DependencyTableKind::Feature)
            root["feature-deps"] = t::Value{t::Table{{table.feature, t::Value{std::move(sectionTable)}}}};
        else
            root[section] = t::Value{std::move(sectionTable)};
        std::string prefix(section);
        for (std::size_t i = 0; i + 1 < segs.size(); ++i) {
            prefix += "." + segs[i];
            if (raw.has_explicit_table(prefix)) ex.insert(prefix);
        }
        auto text = t::serialize(t::Document{std::move(root), std::move(ex)});
        auto m = mcpp::manifest::parse_string(text, manifestPath);
        if (!m) return std::unexpected(m.error().format());
        const auto& map = family(*m, table);
        if (map.size() != 1)
            return std::unexpected(std::format(
                "internal error: the dependency at [{}] {} did not read back as "
                "exactly one entry (please report)", section, join(segs)));
        return map.begin()->first;
    }

    // The map of a manifest that a table's entries are read into.
    static const std::map<std::string, mcpp::manifest::DependencySpec>&
    family(const mcpp::manifest::Manifest& m, const mcpp::manifest::DependencyTable& table) {
        using K = mcpp::manifest::DependencyTableKind;
        static const std::map<std::string, mcpp::manifest::DependencySpec> none;
        if (table.kind == K::Feature) {
            auto it = m.featureDeps.find(table.feature);
            return it == m.featureDeps.end() ? none : it->second;
        }
        return table.kind == K::Build ? m.buildDependencies
             : table.kind == K::Dev   ? m.devDependencies
                                      : m.dependencies;
    }

    // Replace `workspace = true` by the source the workspace resolved.
    std::expected<void, std::string>
    resolve_workspace(t::Table& spec, const mcpp::manifest::DependencyTable& table,
                      const std::vector<std::string>& segs) {
        const auto section = table.section();
        const bool conditional = !table.selector.empty();
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
        auto key = reader_key(table, segs, t::Value{spec});
        if (!key) return std::unexpected(key.error());
        const auto& map = family(effective.manifest, table);
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
    apply_path_rule(t::Table& spec, const mcpp::manifest::DependencyTable& table,
                    const std::vector<std::string>& segs) {
        const auto section = table.section();
        const bool dev = table.kind == mcpp::manifest::DependencyTableKind::Dev;
        auto it = spec.find("path");
        if (it == spec.end() || !it->second.is_string()) return false;
        std::filesystem::path p(it->second.as_string());
        auto target = p.is_absolute() ? p : packageDir / p;
        // A workspace member is published under its own version wherever its
        // directory is, inside this package's included (mcpp#962: package
        // `hanim` and its member `hanim.core` at `core/`). Kept as a path, the
        // archive named the member by path while every other consumer named it
        // by version, and a graph that held both refused the two sources.
        const bool member = effective.member && effective.workspace
            && target.lexically_normal() != packageDir.lexically_normal()
            && mcpp::project::is_workspace_member(*effective.workspace,
                                                  effective.workspaceRoot, target);
        if (inside(packageDir, target) && !member) {
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
            // A SIBLING IS PUBLISHED UNDER ITS OWN VERSION (mcpp#947): the
            // member's manifest states it, and the edge takes it, as the
            // refusal below wrote it for the author to copy. Measured on a
            // workspace of 150 members: 311 such edges among the 67 a
            // release publishes. A member with no version is still refused.
            if (version != "<version>") {
                spec["version"] = t::Value{version};
                spec.erase(std::string("path"));
                changed = true;
                return false;
            }
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
    deps(t::Table& node, const mcpp::manifest::DependencyTable& table, std::vector<std::string> segs) {
        std::vector<std::string> remove;
        for (auto& [k, v] : node) {
            auto here = segs;
            here.push_back(k);
            if (!v.is_table()) continue;              // a version string
            auto& sub = v.as_table();
            if (!names_a_source(sub)) {
                if (auto r = deps(sub, table, here); !r) return r;
                continue;
            }
            if (auto r = resolve_workspace(sub, table, here); !r)
                return std::unexpected(r.error());
            auto drop = apply_path_rule(sub, table, here);
            if (!drop) return std::unexpected(drop.error());
            if (*drop) remove.push_back(k);
        }
        for (auto const& k : remove) node.erase(k);
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
            for (auto const& row : mcpp::manifest::kWorkspaceBuildKeys) {
                const std::string key(row.key);
                if (auto const* f = std::get_if<std::vector<std::string> BC::*>(&row.field)) {
                    if ((w.**f).empty()) continue;
                    auto* bt = build_table();
                    if (!bt) return std::unexpected(std::format(
                        "{}: `build` is not a table", manifestPath.string()));
                    (*bt)[key] = string_array(b.**f);
                    changed = true;
                } else if (auto const* f = std::get_if<
                               std::vector<std::filesystem::path> BC::*>(&row.field)) {
                    const auto n = (w.**f).size();
                    if (n == 0) continue;
                    std::vector<std::string> dirs;
                    const auto& all = b.**f;
                    for (std::size_t i = 0; i < all.size(); ++i) {
                        if (i >= n) { dirs.push_back(all[i].string()); continue; }
                        if (!inside(packageDir, all[i]))
                            return std::unexpected(std::format(
                                "{}: [workspace.build] {} entry '{}' resolves to '{}', "
                                "outside this package's directory, and the published "
                                "archive contains only that directory. Move the headers "
                                "into the package, or declare the directory in the "
                                "package's own [build] for its own build only.",
                                manifestPath.string(), key, (w.**f)[i].string(),
                                all[i].lexically_normal().string()));
                        dirs.push_back(all[i].lexically_normal()
                            .lexically_relative(packageDir.lexically_normal()).generic_string());
                    }
                    auto* bt = build_table();
                    if (!bt) return std::unexpected(std::format(
                        "{}: `build` is not a table", manifestPath.string()));
                    (*bt)[key] = string_array(dirs);
                    changed = true;
                } else if (auto const* f = std::get_if<std::string BC::*>(&row.field)) {
                    if ((w.**f).empty()) continue;
                    auto* bt = build_table();
                    if (!bt) return std::unexpected(std::format(
                        "{}: `build` is not a table", manifestPath.string()));
                    if (bt->contains(key)) continue;
                    (*bt)[key] = t::Value{b.**f};
                    changed = true;
                }
            }
        }
    }

    // W6: what the workspace layer gave the member is written into the
    // member's own tables -- `[workspace.toolchain]` as `[toolchain]`,
    // `[workspace.target.<sel>.build]` as `[target.<sel>.build]`, and so on --
    // merged as the build merged it (W3): a key the member wrote is the
    // member's, a list has the workspace's entries first. The archive is then
    // complete on its own (SPEC-004 §9.7), and the package built as the root of
    // a host-tool sub-build is configured as it was in its workspace. A
    // relative path was written against the workspace root; it is rewritten
    // against the package, and refused when it leaves the package.
    if (effective.member && effective.workspace && effective.workspace->layer) {
        auto wsDoc = t::parse_file(effective.workspaceRoot / "mcpp.toml");
        if (!wsDoc) return std::unexpected(std::format("{}: {}",
            (effective.workspaceRoot / "mcpp.toml").string(), wsDoc.error().message));
        const auto* wsTable = wsDoc->get_table("workspace");
        static constexpr std::string_view kPathKeys[] = {
            "include_dirs", "include_dirs_after", "private_include_dirs", "link_library_dirs", "path",
        };
        std::function<std::expected<void, std::string>(t::Table&, const t::Table&, const std::string&)>
            merge = [&](t::Table& into, const t::Table& from, const std::string& at)
                -> std::expected<void, std::string> {
            for (auto const& [key, value] : from) {
                const auto here = at.empty() ? key : at + "." + key;
                auto value2 = value;
                if (std::ranges::find(kPathKeys, key) != std::end(kPathKeys)) {
                    auto rebase = [&](const std::string& p) -> std::expected<std::string, std::string> {
                        std::filesystem::path path(p);
                        if (path.is_absolute()) return p;
                        const auto abs = (effective.workspaceRoot / path).lexically_normal();
                        if (!inside(packageDir, abs))
                            return std::unexpected(std::format(
                                "{}: [workspace.{}] entry '{}' resolves to '{}', outside this "
                                "package's directory, and the published archive contains only "
                                "that directory. Move it into the package, or declare it in the "
                                "package's own table for its own build only.",
                                manifestPath.string(), here, p, abs.string()));
                        return abs.lexically_relative(packageDir.lexically_normal()).generic_string();
                    };
                    if (value.is_string()) {
                        auto r = rebase(value.as_string());
                        if (!r) return std::unexpected(r.error());
                        value2 = t::Value{*r};
                    } else if (value.is_array()) {
                        t::Array a;
                        for (auto const& e : value.as_array()) {
                            if (!e.is_string()) { a.push_back(e); continue; }
                            auto r = rebase(e.as_string());
                            if (!r) return std::unexpected(r.error());
                            a.emplace_back(*r);
                        }
                        value2 = t::Value{std::move(a)};
                    }
                }
                auto it = into.find(key);
                if (it == into.end()) {
                    if (value2.is_table() && wsDoc->has_explicit_table("workspace." + here))
                        explicitTables.insert(here);
                    into.emplace(key, std::move(value2));
                    changed = true;
                    continue;
                }
                if (it->second.is_table() && value2.is_table()) {
                    if (auto r = merge(it->second.as_table(), value2.as_table(), here); !r) return r;
                } else if (it->second.is_array() && value2.is_array()) {
                    auto joined = value2.as_array();
                    for (auto const& e : it->second.as_array()) joined.push_back(e);
                    it->second = t::Value{std::move(joined)};
                    changed = true;
                }
                // A scalar the member wrote is the member's.
            }
            return {};
        };
        if (wsTable)
            for (auto const& name : effective.workspace->workspace.layerTables) {
                auto src = wsTable->find(name);
                if (src == wsTable->end() || !src->second.is_table()) continue;
                auto* dst = table_at(root, name, name, explicitTables);
                if (!dst) return std::unexpected(std::format(
                    "{}: `{}` is not a table", manifestPath.string(), name));
                auto from = src->second.as_table();
                // The member's own `default` speaks for every platform, as in
                // `merge_layer_root_position`.
                if (name == "toolchain" && dst->contains("default"))
                    std::erase_if(from, [](auto const& kv) { return kv.first != "bootstrap"; });
                if (auto r = merge(*dst, from, name); !r)
                    return std::unexpected(r.error());
            }
    }

    // Dependencies: every table that holds them, conditional ones included, in
    // the enumeration the manifest reader walks (mcpp#962: `[feature-deps.*]`
    // was not among the tables this walk named).
    Walk walk{ packageDir, effective, *doc, manifestPath };
    for (auto const& table : mcpp::manifest::dependency_tables(root))
        if (auto r = walk.deps(*table.table, table, {}); !r)
            return std::unexpected(r.error());
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
