#include <gtest/gtest.h>

import std;
import mcpp.manifest;
import mcpp.modgraph.graph;
import mcpp.modgraph.library_interface;

// ONE DEFINITION OF WHAT A LIBRARY OFFERS (SPEC-008 v0.2, mcpp#797): the
// interface root and what it re-exports, the interface units of the active
// features, and -- with no root -- every exported module. The target build,
// the build programs' graph, `mcpp pack` and the `exports` assertion all read
// this function.

using namespace mcpp::modgraph;

namespace {

struct Tree {
    std::filesystem::path root;
    explicit Tree(std::string_view name)
        : root(std::filesystem::temp_directory_path()
               / std::format("mcpp-libiface-{}-{}", name, std::random_device{}())) {
        std::filesystem::create_directories(root / "src");
        std::filesystem::create_directories(root / "rules");
    }
    ~Tree() { std::error_code ec; std::filesystem::remove_all(root, ec); }
    std::filesystem::path touch(std::string_view rel) {
        auto p = root / rel;
        std::ofstream(p) << "// unit\n";
        return p.lexically_normal();
    }
};

SourceUnit unit(const std::filesystem::path& p, std::string module,
                std::vector<std::string> reexports = {}) {
    SourceUnit u;
    u.path = p;
    u.packageName = "acme.lib";
    u.provides = ModuleId{std::move(module)};
    for (auto& r : reexports) u.reexports.push_back(ModuleId{std::move(r)});
    return u;
}

}  // namespace

TEST(LibraryInterface, TheRootAndWhatItReExports) {
    Tree t("root");
    Graph g;
    g.units.push_back(unit(t.touch("src/lib.cppm"), "acme.lib", {"acme.lib.part"}));
    g.units.push_back(unit(t.touch("src/part.cppm"), "acme.lib.part"));
    g.units.push_back(unit(t.touch("src/detail.cppm"), "acme.lib.detail"));
    mcpp::manifest::Manifest m;
    m.package.name = "lib";
    auto iface = library_interface(g, m, t.root, "acme.lib", {});
    EXPECT_EQ(iface.rootModule, "acme.lib");
    EXPECT_EQ(iface.publicModules, (std::set<std::string>{"acme.lib", "acme.lib.part"}));
    EXPECT_EQ(iface.exportedModules,
              (std::set<std::string>{"acme.lib", "acme.lib.part", "acme.lib.detail"}));
}

TEST(LibraryInterface, NoRootStatesNoInterface) {
    Tree t("noroot");
    Graph g;
    g.units.push_back(unit(t.touch("src/a.cppm"), "acme.a"));
    g.units.push_back(unit(t.touch("src/b.cppm"), "acme.b"));
    mcpp::manifest::Manifest m;
    m.package.name = "lib";
    auto iface = library_interface(g, m, t.root, "acme.lib", {});
    EXPECT_TRUE(iface.rootModule.empty());
    EXPECT_EQ(iface.publicModules, iface.exportedModules);
}

TEST(LibraryInterface, ADeclaredRootIsTheOneUsed) {
    Tree t("declared");
    Graph g;
    g.units.push_back(unit(t.touch("src/lib.cppm"), "acme.conventional"));
    g.units.push_back(unit(t.touch("rules/facade.cppm"), "acme.facade"));
    mcpp::manifest::Manifest m;
    m.package.name = "lib";
    m.lib.path = "rules/facade.cppm";
    auto iface = library_interface(g, m, t.root, "acme.lib", {});
    EXPECT_TRUE(iface.rootDeclared);
    EXPECT_EQ(iface.rootModule, "acme.facade");
    EXPECT_EQ(iface.publicModules, (std::set<std::string>{"acme.facade"}));
}

TEST(LibraryInterface, AnActiveFeatureAddsItsInterfaceUnits) {
    Tree t("features");
    Graph g;
    g.units.push_back(unit(t.touch("src/lib.cppm"), "acme.lib"));
    g.units.push_back(unit(t.touch("rules/cuda.cppm"), "acme.rules.cuda"));
    g.units.push_back(unit(t.touch("rules/spirv.cppm"), "acme.rules.spirv"));
    mcpp::manifest::Manifest m;
    m.package.name = "lib";
    m.buildConfig.featureSources["rules-cuda"] = {"rules/cuda.cppm"};
    m.buildConfig.featureSources["rules-spirv"] = {"rules/spirv.cppm"};
    auto iface = library_interface(g, m, t.root, "acme.lib", {"rules-cuda"});
    EXPECT_EQ(iface.publicModules, (std::set<std::string>{"acme.lib", "acme.rules.cuda"}));
}

TEST(LibraryInterface, OnlyThePackagesOwnUnitsCount) {
    Tree t("own");
    Graph g;
    g.units.push_back(unit(t.touch("src/lib.cppm"), "acme.lib"));
    auto other = unit(t.touch("src/other.cppm"), "other.mod");
    other.packageName = "other.pkg";
    g.units.push_back(other);
    mcpp::manifest::Manifest m;
    m.package.name = "lib";
    auto iface = library_interface(g, m, t.root, "acme.lib", {});
    EXPECT_FALSE(iface.exportedModules.contains("other.mod"));
}
