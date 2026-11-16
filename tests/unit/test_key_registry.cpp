
import std;
import mcpplibs.mtest;
import mcpp.manifest;
import mcpp.project;

using namespace mcpplibs::mtest;

// SPEC-004 §9.10: the key registry, the workspace layer and the merge rules
// (W1-W3, W7). The end-to-end halves are tests/e2e/893-895 and 900.

namespace mm = mcpp::manifest;

namespace {

mm::Manifest parse(std::string_view text) {
    auto m = mm::parse_string(text);
    if (!m) expect(false) << m.error().format();
    return m ? *m : mm::Manifest{};
}

std::string parse_error(std::string_view text) {
    auto m = mm::parse_string(text);
    return m ? std::string{} : m.error().message;
}

}  // namespace

// The inheritable subset of [build] is stated by two tables: the registry's
// `shared` column and `kWorkspaceBuildKeys`, which also carries the field each
// key fills. They must name the same keys; `linkage` is the one key
// [workspace.build] reads that [build] does not (the target row and the
// profile set it there).
UTest the_shared_build_keys_are_the_workspace_build_keys{"KeyRegistry/TheSharedBuildKeysAreTheWorkspaceBuildKeys", [] {
    std::set<std::string> shared, table;
    for (auto const& row : mm::kKeyRegistry)
        if (row.table == "build" && row.shared) shared.emplace(row.key);
    for (auto const& row : mm::kWorkspaceBuildKeys)
        if (row.key != "linkage") table.emplace(row.key);
    expect(eq(shared, table));
}};

// Every configuration key of [build] is part of the value members are grouped
// by: two members that differ in one are not one plan.
UTest every_configuration_key_enters_the_grouping_key{"KeyRegistry/EveryConfigurationKeyEntersTheGroupingKey", [] {
    const auto key = mcpp::project::root_position_key(mm::Manifest{});
    const std::map<std::string_view, std::string_view> spelled = {
        {"profile", "default_profile"}, {"default-profile", "default_profile"},
        {"platform-dependencies", "platform_dependencies"},
    };
    for (auto const& row : mm::kKeyRegistry) {
        if (row.table != "build" || row.scope != mm::KeyScope::Configuration) continue;
        auto it = spelled.find(row.key);
        const std::string name(it == spelled.end() ? row.key : it->second);
        expect(ne(key.find(name + "="), std::string::npos)) << row.key;
    }
}};

// The parser accepts every [build] key of the registry without a warning.
UTest the_parser_reads_the_registrys_build_keys{"KeyRegistry/TheParserReadsTheRegistrysBuildKeys", [] {
    auto m = parse(R"(
[package]
name = "x"
version = "0.1.0"
[build]
sources = ["src/*.cpp"]
cxxflags = ["-O2"]
dialect_cxxflags = ["-DX=1"]
cache = "local"
)");
    expect(m.schemaWarnings.empty());
    auto w = parse(R"(
[package]
name = "x"
version = "0.1.0"
[build]
cxxflagz = ["-O2"]
)");
    expect(fatal(eq(w.schemaWarnings.size(), 1u)));
    expect(ne(w.schemaWarnings.front().find("cxxflagz"), std::string::npos));
}};

// W2: [workspace.X] is read by the reader of X.
UTest mirrors_are_read_by_the_reader_of_the_table_they_mirror{"WorkspaceLayer/MirrorsAreReadByTheReaderOfTheTableTheyMirror", [] {
    auto ws = parse(R"(
[workspace]
members = ["a"]
[workspace.toolchain]
default = "llvm@23.1.3"
[workspace.profile.release]
opt = 3
[workspace.target.x86_64-linux-musl]
linkage = "static"
[workspace.target.'cfg(os = "linux")'.build]
dialect_cxxflags = ["-DW=1"]
cxxflags = ["-DSHARED"]
[workspace.xlings.workspace]
cmake = "4.0.2"
)");
    expect(fatal(bool(ws.layer)));
    auto const& layer = *ws.layer;
    expect(eq(layer.toolchain.for_platform("linux").value_or(""), "llvm@23.1.3"));
    expect(fatal(layer.profiles.contains("release")));
    expect(eq(layer.profiles.at("release").optLevel, "3"));
    expect(bool(layer.profiles.at("release").optDeclared));
    expect(!(layer.profiles.at("release").debugDeclared));
    expect(eq(layer.targetOverrides.at("x86_64-linux-musl").linkage, "static"));
    expect(fatal(!layer.conditionalConfigs.empty()));
    expect(eq(ws.workspace.layerTables, (std::vector<std::string>{"toolchain", "profile", "target", "xlings"})));
    // The root itself states none of them for its own package.
    expect(ws.toolchain.byPlatform.empty());
    expect(ws.targetOverrides.empty());
}};

UTest refuses_what_describes_one_package_and_what_it_does_not_know{"WorkspaceLayer/RefusesWhatDescribesOnePackageAndWhatItDoesNotKnow", [] {
    expect(ne(parse_error(R"(
[workspace]
members = ["a"]
[workspace.tolchain]
default = "gcc@16.1.0"
)").find("[workspace] has no key or table 'tolchain'"), std::string::npos));
    expect(ne(parse_error(R"(
[workspace]
members = ["a"]
[workspace.target.x86_64-linux-gnu.dependencies]
fmt = "11.0.0"
)").find("describes one package"), std::string::npos));
    expect(ne(parse_error(R"(
[workspace]
members = ["a"]
[workspace.target.x86_64-linux-gnu.build]
sources = ["x.cpp"]
)").find("has no key 'sources'"), std::string::npos));
    expect(ne(parse_error(R"(
[workspace]
members = ["a"]
[workspace.target.x86_64-linux-gnu.build]
allow_host_libs = true
)").find("allow_host_libs is not inheritable"), std::string::npos));
    // On a root without [package], one table cannot speak to members twice.
    expect(ne(parse_error(R"(
[workspace]
members = ["a"]
[toolchain]
default = "gcc@16.1.0"
[workspace.toolchain]
default = "llvm@23.1.3"
)").find("write it once, as [workspace.toolchain]"), std::string::npos));
    // With [package], the root-position table is the root package's own.
    expect(parse_error(R"(
[package]
name = "root"
version = "0.1.0"
[workspace]
members = ["a"]
[toolchain]
default = "gcc@16.1.0"
[workspace.toolchain]
default = "llvm@23.1.3"
)").empty());
}};

// W7: a package table on a root without [package] acts on nothing.
UTest package_tables_on_a_virtual_root_are_recorded{"WorkspaceLayer/PackageTablesOnAVirtualRootAreRecorded", [] {
    auto ws = parse(R"(
[workspace]
members = ["a"]
[build]
cxxflags = ["-O2"]
[target.'cfg(os = "linux")'.build]
dialect_cxxflags = ["-DX=1"]
)");
    expect(fatal(eq(ws.virtualRootPackageTables.size(), 2u)));
    expect(ne(ws.virtualRootPackageTables[0].find("[build]"), std::string::npos));
    expect(ne(ws.virtualRootPackageTables[1].find("[workspace.target."), std::string::npos));
    auto rooted = parse(R"(
[package]
name = "root"
version = "0.1.0"
[workspace]
members = ["a"]
[build]
cxxflags = ["-O2"]
)");
    expect(rooted.virtualRootPackageTables.empty());
}};

// W3: scalars the member declared are the member's, named tables merge key by
// key, lists put the workspace first, conditional rows come first.
UTest the_merge_rule_is_the_keys{"WorkspaceLayer/TheMergeRuleIsTheKeys", [] {
    auto ws = parse(R"(
[workspace]
members = ["a"]
[workspace.toolchain]
linux = "llvm@23.1.3"
macos = "llvm@23.1.3"
[workspace.profile.release]
opt = 3
lto = true
cxxflags = ["-DWS"]
[workspace.target.x86_64-linux-musl]
linkage = "static"
cxx_runtime = "self-contained"
[workspace.target.'cfg(os = "linux")'.build]
dialect_cxxflags = ["-DW=1"]
)");
    auto member = parse(R"(
[package]
name = "a"
version = "0.1.0"
[toolchain]
linux = "gcc@16.1.0"
[profile.release]
opt = 2
cxxflags = ["-DMEMBER"]
[target.x86_64-linux-musl]
linkage = "dynamic"
[target.'cfg(os = "linux")'.build]
dialect_cxxflags = ["-DM=1"]
)");
    mcpp::project::inherit_workspace_root_position(member, ws, "/ws");
    mcpp::project::inherit_workspace_layer_rows(member, ws, "/ws");
    expect(eq(member.toolchain.for_platform("linux").value_or(""), "gcc@16.1.0"));
    expect(eq(member.toolchain.for_platform("macos").value_or(""), "llvm@23.1.3"));
    auto const& r = member.profiles.at("release");
    expect(eq(r.optLevel, "2"));
    expect(bool(r.lto));
    expect(eq(r.cxxflags, (std::vector<std::string>{"-DWS", "-DMEMBER"})));
    auto const& t = member.targetOverrides.at("x86_64-linux-musl");
    expect(eq(t.linkage, "dynamic"));
    expect(eq(t.cxxRuntime, "self-contained"));
    expect(fatal(ge(member.conditionalConfigs.size(), 2u)));
    expect(eq(member.conditionalConfigs.front().dialectCxxflags, (std::vector<std::string>{"-DW=1"})));

    // A member's own `default` speaks for every platform.
    auto own = parse(R"(
[package]
name = "b"
version = "0.1.0"
[toolchain]
default = "gcc@16.1.0"
)");
    mcpp::project::inherit_workspace_root_position(own, ws, "/ws");
    expect(eq(own.toolchain.for_platform("macos").value_or(""), "gcc@16.1.0"));
}};

// W4: grouping counts a conditional configuration row as it evaluates.
UTest grouping_reads_evaluated_rows{"WorkspaceLayer/GroupingReadsEvaluatedRows", [] {
    auto a = parse(R"(
[package]
name = "a"
version = "0.1.0"
[target.'cfg(os = "windows")'.build]
dialect_cxxflags = ["-DWIN=1"]
)");
    auto b = parse(R"(
[package]
name = "b"
version = "0.1.0"
)");
    auto linux = [](std::string_view p) { return p.find("linux") != std::string_view::npos; };
    auto windows = [](std::string_view p) { return p.find("windows") != std::string_view::npos; };
    expect(eq(mcpp::project::root_position_key(a, linux), mcpp::project::root_position_key(b, linux)));
    expect(ne(mcpp::project::root_position_key(a, windows), mcpp::project::root_position_key(b, windows)));
}};
