// mcpp.manifest.dependency_tables — the tables of a manifest that hold
// dependencies, enumerated in one place.
//
// A manifest states dependencies in four kinds of table, each of them also
// under a condition:
//
//   [dependencies]                         [target.<selector>.dependencies]
//   [build-dependencies]                   [target.<selector>.build-dependencies]
//   [dev-dependencies]                     [target.<selector>.dev-dependencies]
//   [feature-deps.<feature>]               [target.<selector>.feature-deps.<feature>]
//
// Three parts of mcpp walk them: the reader that loads a manifest, the editor
// behind `mcpp add` and `mcpp remove`, and the normalisation `mcpp publish`
// applies to the manifest an archive carries. Each named the tables itself,
// and the lists differed: the normalisation knew three of the four kinds, so a
// `path` dependency under `[feature-deps.<feature>]` was published as written
// and no consumer could reach it (mcpp#962); the editor knew two. The list is
// here, and the three walk it.

export module mcpp.manifest.dependency_tables;

import std;
import mcpp.libs.toml;

export namespace mcpp::manifest {

enum class DependencyTableKind { Normal, Build, Dev, Feature };

// The four kinds, in the order a manifest is read.
inline constexpr DependencyTableKind kDependencyTableKinds[] = {
    DependencyTableKind::Normal, DependencyTableKind::Dev, DependencyTableKind::Build,
    DependencyTableKind::Feature,
};

// The key a kind is written under: `dependencies`, `build-dependencies`,
// `dev-dependencies`, `feature-deps`.
constexpr std::string_view dependency_table_name(DependencyTableKind kind) {
    switch (kind) {
        case DependencyTableKind::Normal:  return "dependencies";
        case DependencyTableKind::Build:   return "build-dependencies";
        case DependencyTableKind::Dev:     return "dev-dependencies";
        case DependencyTableKind::Feature: return "feature-deps";
    }
    return "dependencies";
}

// One table of dependencies, where a document has it.
template <class TableT>
struct DependencyTableOf {
    DependencyTableKind kind = DependencyTableKind::Normal;
    std::string         feature;    // the feature of a `feature-deps` table; empty otherwise
    std::string         selector;   // the `[target.<selector>]` it is under, as written; empty: none
    TableT*             table = nullptr;

    // The path below its scope, as a header spells it: `dependencies`,
    // `feature-deps.<feature>`.
    std::string section() const {
        std::string out(dependency_table_name(kind));
        if (kind == DependencyTableKind::Feature) out += "." + feature;
        return out;
    }
};
using DependencyTable      = DependencyTableOf<mcpp::libs::toml::Table>;
using ConstDependencyTable = DependencyTableOf<const mcpp::libs::toml::Table>;

// The dependency tables written directly in `scope`: the document's root, or
// the table of one `[target.<selector>]`.
std::vector<DependencyTable> dependency_tables_in(mcpp::libs::toml::Table& scope,
                                                  std::string_view selector = {});
std::vector<ConstDependencyTable> dependency_tables_in(const mcpp::libs::toml::Table& scope,
                                                       std::string_view selector = {});

// Every dependency table of a document: the root's, then those of each
// `[target.<selector>]` in the order the document holds them.
std::vector<DependencyTable> dependency_tables(mcpp::libs::toml::Table& root);
std::vector<ConstDependencyTable> dependency_tables(const mcpp::libs::toml::Table& root);

} // namespace mcpp::manifest

namespace mcpp::manifest {

namespace {

template <class TableT>
std::vector<DependencyTableOf<TableT>> tables_in(TableT& scope, std::string_view selector) {
    std::vector<DependencyTableOf<TableT>> out;
    for (auto kind : kDependencyTableKinds) {
        auto it = scope.find(dependency_table_name(kind));
        if (it == scope.end() || !it->second.is_table()) continue;
        auto& table = it->second.as_table();
        if (kind != DependencyTableKind::Feature) {
            out.push_back({kind, {}, std::string(selector), &table});
            continue;
        }
        for (auto& [feature, deps] : table)
            if (deps.is_table())
                out.push_back({kind, feature, std::string(selector), &deps.as_table()});
    }
    return out;
}

template <class TableT>
std::vector<DependencyTableOf<TableT>> all_tables(TableT& root) {
    auto out = tables_in(root, {});
    auto target = root.find("target");
    if (target == root.end() || !target->second.is_table()) return out;
    for (auto& [selector, row] : target->second.as_table()) {
        if (!row.is_table()) continue;
        for (auto& t : tables_in(row.as_table(), selector)) out.push_back(std::move(t));
    }
    return out;
}

} // namespace

std::vector<DependencyTable> dependency_tables_in(mcpp::libs::toml::Table& scope,
                                                  std::string_view selector) {
    return tables_in(scope, selector);
}
std::vector<ConstDependencyTable> dependency_tables_in(const mcpp::libs::toml::Table& scope,
                                                       std::string_view selector) {
    return tables_in(scope, selector);
}
std::vector<DependencyTable> dependency_tables(mcpp::libs::toml::Table& root) {
    return all_tables(root);
}
std::vector<ConstDependencyTable> dependency_tables(const mcpp::libs::toml::Table& root) {
    return all_tables(root);
}

} // namespace mcpp::manifest
