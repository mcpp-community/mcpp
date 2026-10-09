// mcpp.manifest.key_registry - the class of every manifest key, stated once.
//
// SPEC-004 §2 gives each key two independent properties: its SCOPE, which is
// the key's own (whom its value acts on), and its SOURCE, which is the
// table's (the command line, the package's own table, the workspace layer,
// the engine's default). A condition (`[target.<sel>]`) is a predicate on a
// value and changes neither.
//
//   P  package     acts on this package's units (`cxxflags`, `sources`)
//   U  usage       flows from a dependency to its consumers (`include_dirs`)
//   C  configuration  one value per build, for the whole graph (`target`,
//                  `dialect_cxxflags`, `[toolchain]`); read from the root
//   M  metadata    enters no command (`license`)
//
// A row also says how a member and its workspace layer are merged (W3) and
// whether the key may be shared through `[workspace.X]` (W2) or written under
// a condition. The parser's known-key checks for `[build]` and
// `[target.<sel>.build]`, the keys `[workspace.target.<sel>.build]` accepts,
// and the package tables W7 reports on a workspace root without `[package]`
// are read from here; tests/unit/test_key_registry.cpp checks the rest
// (`kWorkspaceBuildKeys`, the grouping key) against it.

export module mcpp.manifest.key_registry;

import std;

export namespace mcpp::manifest {

enum class KeyScope { Package, Usage, Configuration, Metadata };

// How a member's value and its workspace layer's combine (W3).
enum class KeyMerge {
    Scalar,   // the member's, when it declared the key
    List,     // appended, the workspace's entries first
    Defines,  // a set by macro name; the member's same name replaces
    Table,    // a named table: merged key by key
};

struct KeyClass {
    std::string_view table;   // "build", "target.<sel>.build", "toolchain", ...
    std::string_view key;     // "" for the table as a whole
    KeyScope         scope;
    KeyMerge         merge;
    bool             shared;      // may be written as [workspace.<table>] (W2)
    bool             conditional; // may be written under [target.<sel>]
};

inline constexpr KeyClass kKeyRegistry[] = {
    // [build]
    {"build", "accel",                   KeyScope::Configuration, KeyMerge::Scalar,  false, false},
    {"build", "allow_host_libs",         KeyScope::Package,       KeyMerge::Scalar,  false, false},
    {"build", "bmi_schedule",            KeyScope::Configuration, KeyMerge::Scalar,  false, false},
    {"build", "build_program_timeout",   KeyScope::Package,       KeyMerge::Scalar,  false, false},
    {"build", "c_standard",              KeyScope::Package,       KeyMerge::Scalar,  true,  false},
    {"build", "cache",                   KeyScope::Configuration, KeyMerge::Scalar,  false, false},
    {"build", "cflags",                  KeyScope::Package,       KeyMerge::List,    true,  true },
    {"build", "cxxflags",                KeyScope::Package,       KeyMerge::List,    true,  true },
    {"build", "cxx_runtime",             KeyScope::Configuration, KeyMerge::Scalar,  true,  false},
    {"build", "default-profile",         KeyScope::Configuration, KeyMerge::Scalar,  false, false},
    {"build", "defines",                 KeyScope::Package,       KeyMerge::Defines, true,  true },
    {"build", "dependency_linkage",      KeyScope::Configuration, KeyMerge::Scalar,  true,  false},
    {"build", "dialect_cxxflags",        KeyScope::Configuration, KeyMerge::List,    true,  true },
    {"build", "flags",                   KeyScope::Package,       KeyMerge::List,    false, true },
    {"build", "include_dirs",            KeyScope::Usage,         KeyMerge::List,    true,  true },
    {"build", "include_dirs_after",      KeyScope::Usage,         KeyMerge::List,    true,  true },
    {"build", "ios_deployment_target",   KeyScope::Configuration, KeyMerge::Scalar,  true,  false},
    {"build", "jobs",                    KeyScope::Configuration, KeyMerge::Scalar,  false, false},
    {"build", "ldflags",                 KeyScope::Usage,         KeyMerge::List,    true,  true },
    {"build", "macos_deployment_target", KeyScope::Configuration, KeyMerge::Scalar,  true,  false},
    {"build", "module_extensions",       KeyScope::Package,       KeyMerge::List,    false, false},
    {"build", "platform-dependencies",   KeyScope::Configuration, KeyMerge::Scalar,  false, false},
    {"build", "private_include_dirs",    KeyScope::Package,       KeyMerge::List,    true,  true },
    {"build", "profile",                 KeyScope::Configuration, KeyMerge::Scalar,  false, false},
    {"build", "sources",                 KeyScope::Package,       KeyMerge::List,    false, true },
    {"build", "static_stdlib",           KeyScope::Configuration, KeyMerge::Scalar,  false, false},
    {"build", "std-compat-module",       KeyScope::Package, KeyMerge::Scalar,  false, false},
    {"build", "std-module",              KeyScope::Package, KeyMerge::Scalar,  false, false},
    {"build", "std-module-flags",        KeyScope::Package, KeyMerge::List,    false, true },
    {"build", "target",                  KeyScope::Configuration, KeyMerge::Scalar,  true,  false},
    // `[workspace.build]` accepts `linkage`; `[build]` reads it from the
    // target row and the profile.
    // [target.<sel>] scalar rows
    {"target.<sel>", "cxx_runtime",      KeyScope::Configuration, KeyMerge::Scalar,  true,  true },
    {"target.<sel>", "linkage",          KeyScope::Configuration, KeyMerge::Scalar,  true,  true },
    {"target.<sel>", "min_api_level",    KeyScope::Configuration, KeyMerge::Scalar,  true,  true },
    {"target.<sel>", "runner",           KeyScope::Configuration, KeyMerge::Scalar,  true,  true },
    {"target.<sel>", "sysroot",          KeyScope::Configuration, KeyMerge::Scalar,  true,  true },
    {"target.<sel>", "toolchain",        KeyScope::Configuration, KeyMerge::Scalar,  true,  true },
    // [target.<sel>] subtables
    {"target.<sel>.abi",     "",         KeyScope::Configuration, KeyMerge::Table,   true,  true },
    {"target.<sel>.runtime", "",         KeyScope::Usage,         KeyMerge::Table,   true,  true },
    {"target.<sel>.xlings",  "",         KeyScope::Configuration, KeyMerge::Table,   true,  true },
    {"target.<sel>.targets", "",         KeyScope::Package,       KeyMerge::Table,   false, true },
    {"target.<sel>.dependencies", "",    KeyScope::Package,       KeyMerge::Table,   false, true },
    // whole tables
    {"toolchain",            "",         KeyScope::Configuration, KeyMerge::Table,   true,  false},
    {"indices",              "",         KeyScope::Configuration, KeyMerge::Table,   true,  false},
    {"profile.<name>",       "",         KeyScope::Configuration, KeyMerge::Table,   true,  false},
    {"xlings",               "",         KeyScope::Configuration, KeyMerge::Table,   true,  false},
    {"package",              "",         KeyScope::Metadata,      KeyMerge::Table,   true,  false},
    {"targets",              "",         KeyScope::Package,       KeyMerge::Table,   false, false},
    {"dependencies",         "",         KeyScope::Package,       KeyMerge::Table,   false, false},
    {"dev-dependencies",     "",         KeyScope::Package,       KeyMerge::Table,   false, false},
    {"build-dependencies",   "",         KeyScope::Package,       KeyMerge::Table,   false, false},
    {"features",             "",         KeyScope::Package,       KeyMerge::Table,   false, false},
    {"resources",            "",         KeyScope::Package,       KeyMerge::Table,   false, false},
    {"test",                 "",         KeyScope::Package,       KeyMerge::Table,   false, false},
    {"hooks",                "",         KeyScope::Package,       KeyMerge::Table,   false, false},
    {"runtime",              "",         KeyScope::Package,       KeyMerge::Table,   false, false},
};

// The keys of `table`, in registry order.
inline std::vector<std::string_view> registry_keys(std::string_view table) {
    std::vector<std::string_view> out;
    for (auto const& row : kKeyRegistry)
        if (row.table == table && !row.key.empty()) out.push_back(row.key);
    return out;
}

// The keys of `[build]` that may be written under `[target.<sel>.build]`.
inline std::vector<std::string_view> conditional_build_keys() {
    std::vector<std::string_view> out;
    for (auto const& row : kKeyRegistry)
        if (row.table == "build" && row.conditional) out.push_back(row.key);
    return out;
}

// The keys `[workspace.target.<sel>.build]` accepts: shared and conditional.
inline std::vector<std::string_view> shared_conditional_build_keys() {
    std::vector<std::string_view> out;
    for (auto const& row : kKeyRegistry)
        if (row.table == "build" && row.conditional && row.shared) out.push_back(row.key);
    return out;
}

inline const KeyClass* key_class(std::string_view table, std::string_view key) {
    for (auto const& row : kKeyRegistry)
        if (row.table == table && row.key == key) return &row;
    return nullptr;
}

}  // namespace mcpp::manifest
