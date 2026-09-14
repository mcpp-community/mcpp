// #641 item 2 -- a package that provides the C++ layer compiles its own
// implementation units at the standard it states.
//
// A module graph has one standard, and `make_plan` makes one exception: the
// C++ units of a C++-layer provider that neither provide nor import a module
// receive the provider's stated level after the graph's. Every other unit, the
// provider's module units included, keeps the graph's level. The flag rides the
// unit's own C++ flag vector, which every emitter reads, so these assertions on
// the plan are assertions on the compile edge, the scan edge and both build
// databases.

#include <gtest/gtest.h>

import std;
import mcpp.build.plan;
import mcpp.manifest;
import mcpp.modgraph.graph;
import mcpp.modgraph.scanner;
import mcpp.source_kind;
import mcpp.toolchain.model;

using namespace mcpp::build;

namespace {

struct Tmp {
    std::filesystem::path path;
    Tmp() {
        path = std::filesystem::temp_directory_path()
             / std::format("mcpp_cxx_layer_std_{}", std::random_device{}());
        std::filesystem::create_directories(path);
    }
    ~Tmp() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

mcpp::toolchain::Toolchain clangLike() {
    mcpp::toolchain::Toolchain tc;
    tc.compiler     = mcpp::toolchain::CompilerId::Clang;
    tc.version      = "22.1.8";
    tc.binaryPath   = "/usr/bin/clang++";
    tc.targetTriple = "x86_64-linux-gnu";
    return tc;
}

mcpp::modgraph::SourceUnit unit(const std::filesystem::path& root,
                                const std::filesystem::path& rel,
                                std::string_view pkg, mcpp::SourceKind kind) {
    mcpp::modgraph::SourceUnit u;
    u.path        = root / rel;
    u.relPath     = rel;
    u.packageName = std::string(pkg);
    u.kind        = kind;
    std::filesystem::create_directories(u.path.parent_path());
    std::ofstream(u.path) << "/* test */\n";
    return u;
}

struct Fixture {
    std::string graphStandard = "c++20";
    // The provider's `[package] standard`; empty means it wrote none.
    std::string providerStandard = "c++23";
    std::vector<std::string> providerProvides = {"hosted-standard-library",
                                                 "mcpp:c++-abi=libc++"};
};

// The C++ flags the plan gave each unit, by source file name.
std::map<std::string, std::vector<std::string>> plan_flags(const Fixture& f) {
    Tmp t;
    const auto projectRoot = t.path / "app";
    const auto providerRoot = t.path / "libcxx";
    const auto plainRoot = t.path / "plain";

    mcpp::manifest::Manifest root;
    root.package.name     = "app";
    root.package.version  = "0.1.0";
    root.package.standard = f.graphStandard;
    root.package.standardDeclared = true;
    mcpp::manifest::Target bin;
    bin.name = "app";
    bin.kind = mcpp::manifest::Target::Library;
    root.targets.push_back(bin);

    std::vector<mcpp::modgraph::PackageRoot> packages;
    packages.push_back({projectRoot, root});

    mcpp::manifest::Manifest provider;
    provider.package.namespace_ = "llvm";
    provider.package.name       = "libcxx";
    provider.package.version    = "22.1.8.2";
    if (!f.providerStandard.empty()) {
        provider.package.standard = f.providerStandard;
        provider.package.standardDeclared = true;
    }
    provider.provides = f.providerProvides;
    packages.push_back({providerRoot, provider});

    // An ordinary package that also states a higher level: the one-standard
    // rule still governs it.
    mcpp::manifest::Manifest plain;
    plain.package.name     = "plain";
    plain.package.version  = "0.1.0";
    plain.package.standard = "c++23";
    plain.package.standardDeclared = true;
    packages.push_back({plainRoot, plain});

    using K = mcpp::SourceKind;
    mcpp::modgraph::Graph graph;
    graph.units.push_back(unit(projectRoot, "src/app.cpp", "app", K::Cxx));
    graph.units.push_back(unit(providerRoot, "src/new.cpp", "llvm.libcxx", K::Cxx));
    auto iface = unit(providerRoot, "src/iface.cppm", "llvm.libcxx", K::ModuleInterface);
    iface.provides = mcpp::modgraph::ModuleId{"llvm.libcxx.iface"};
    iface.declaration = mcpp::modgraph::ModuleDeclaration::Interface;
    graph.units.push_back(iface);
    auto importer = unit(providerRoot, "src/importer.cpp", "llvm.libcxx", K::Cxx);
    importer.requires_.push_back(mcpp::modgraph::ModuleId{"std"});
    graph.units.push_back(importer);
    auto implUnit = unit(providerRoot, "src/iface_impl.cpp", "llvm.libcxx", K::Cxx);
    implUnit.requires_.push_back(mcpp::modgraph::ModuleId{"llvm.libcxx.iface"});
    implUnit.declaration = mcpp::modgraph::ModuleDeclaration::Implementation;
    graph.units.push_back(implUnit);
    graph.units.push_back(unit(providerRoot, "src/c_part.c", "llvm.libcxx", K::C));
    graph.units.push_back(unit(plainRoot, "src/plain.cpp", "plain", K::Cxx));

    std::vector<std::size_t> topo;
    for (std::size_t i = 0; i < graph.units.size(); ++i) topo.push_back(i);

    auto plan = make_plan(root, clangLike(), {}, graph, topo, packages,
                          projectRoot, projectRoot / "target" / "t",
                          {}, {}, {});
    EXPECT_TRUE(plan.has_value()) << (plan ? "" : plan.error());
    std::map<std::string, std::vector<std::string>> out;
    if (!plan) return out;
    for (auto const& cu : plan->compileUnits)
        out[cu.source.filename().string()] = cu.packageCxxflags;
    return out;
}

bool has(const std::vector<std::string>& v, std::string_view flag) {
    return std::ranges::find(v, flag) != v.end();
}

} // namespace

TEST(CxxLayerStandard, ProviderImplementationUnitGetsTheStatedLevel) {
    auto flags = plan_flags(Fixture{});
    ASSERT_TRUE(flags.contains("new.cpp"));
    ASSERT_FALSE(flags["new.cpp"].empty());
    EXPECT_EQ(flags["new.cpp"].back(), "-std=c++23");
}

TEST(CxxLayerStandard, UnitsThatTouchAModuleKeepTheGraphLevel) {
    auto flags = plan_flags(Fixture{});
    EXPECT_FALSE(has(flags["iface.cppm"], "-std=c++23"));      // provides a module
    EXPECT_FALSE(has(flags["importer.cpp"], "-std=c++23"));    // imports std
    EXPECT_FALSE(has(flags["iface_impl.cpp"], "-std=c++23"));  // `module M;`
    EXPECT_FALSE(has(flags["c_part.c"], "-std=c++23"));        // not C++
}

TEST(CxxLayerStandard, OtherPackagesKeepTheOneStandardRule) {
    auto flags = plan_flags(Fixture{});
    EXPECT_FALSE(has(flags["plain.cpp"], "-std=c++23"));
    EXPECT_FALSE(has(flags["app.cpp"], "-std=c++23"));
}

TEST(CxxLayerStandard, AProviderThatStatesNothingIsUnchanged) {
    Fixture f;
    f.providerStandard.clear();
    auto flags = plan_flags(f);
    for (auto const& [file, v] : flags)
        for (auto const& flag : v)
            EXPECT_FALSE(flag.starts_with("-std=")) << file << ": " << flag;
}

TEST(CxxLayerStandard, OnlyTheCurrentLayerSpellingIsEnough) {
    Fixture f;
    f.providerProvides = {"mcpp:c++-abi=libc++"};
    auto flags = plan_flags(f);
    ASSERT_FALSE(flags["new.cpp"].empty());
    EXPECT_EQ(flags["new.cpp"].back(), "-std=c++23");
}

TEST(CxxLayerStandard, AStatedStandardWithoutTheLayerIsNotAnException) {
    Fixture f;
    f.providerProvides.clear();
    auto flags = plan_flags(f);
    EXPECT_FALSE(has(flags["new.cpp"], "-std=c++23"));
}

TEST(CxxLayerStandard, TheLevelIsExactlyTheStatedOneUnderAHigherGraph) {
    Fixture f;
    f.graphStandard = "c++26";
    auto flags = plan_flags(f);
    ASSERT_FALSE(flags["new.cpp"].empty());
    EXPECT_EQ(flags["new.cpp"].back(), "-std=c++23");
}

TEST(CxxLayerStandard, TheSameLevelAsTheGraphAddsNothing) {
    Fixture f;
    f.graphStandard = "c++23";
    auto flags = plan_flags(f);
    EXPECT_FALSE(has(flags["new.cpp"], "-std=c++23"));
}

TEST(CxxLayerStandard, ThePredicateNamesEitherSpellingOfTheLayer) {
    mcpp::manifest::Manifest m;
    EXPECT_FALSE(mcpp::manifest::provides_cxx_layer(m));
    m.provides = {"hosted-standard-library"};
    EXPECT_TRUE(mcpp::manifest::provides_cxx_layer(m));
    m.provides = {"mcpp:c++-abi=libc++"};
    EXPECT_TRUE(mcpp::manifest::provides_cxx_layer(m));
    m.provides = {"mcpp:c-abi=musl"};
    EXPECT_FALSE(mcpp::manifest::provides_cxx_layer(m));
}
