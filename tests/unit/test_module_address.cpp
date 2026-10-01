// A unit's BMI and the module map it reads are a function of the unit and its
// imports, not of the rest of the graph (pack drive and selection design
// 2026-10-01, B1; mcpp#751).
//
// A workspace's build directory serves every selection of one configuration:
// `mcpp build --workspace` and `mcpp build -p app` compile `core` into the same
// directory. 2026.9.30.2 placed a BMI below its provider's directory only when
// two packages of the GRAPH provided its name, so `core`'s BMI path and every
// importer's flags depended on whether the graph also held `tool`, which
// provides a module of the same name and shares no program with `core`. Each
// switch between the two selections recompiled `core`.
//
// The test plans `core` twice, once without `tool` and once beside it, and
// demands identical BMI paths, flags and maps. Under 2026.9.30.2's rule the two
// plans differ in both. It is the BMI counterpart of test_object_address.cpp
// (mcpp#344).

#include <gtest/gtest.h>

import std;
import mcpp.build.plan;
import mcpp.build.ninja;
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
             / std::format("mcpp_module_addr_{}", std::random_device{}());
        std::filesystem::create_directories(path);
    }
    ~Tmp() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

mcpp::toolchain::Toolchain toolchain(mcpp::toolchain::CompilerId id) {
    mcpp::toolchain::Toolchain tc;
    tc.compiler     = id;
    tc.version      = id == mcpp::toolchain::CompilerId::GCC ? "16.1.0" : "22.1.8";
    tc.binaryPath   = id == mcpp::toolchain::CompilerId::GCC ? "/usr/bin/g++" : "/usr/bin/clang++";
    tc.targetTriple = "x86_64-linux-gnu";
    return tc;
}

mcpp::modgraph::PackageRoot package(const std::filesystem::path& root, std::string_view name) {
    mcpp::modgraph::PackageRoot p;
    p.root = root;
    p.manifest.package.name     = std::string(name);
    p.manifest.package.version  = "0.1.0";
    p.manifest.package.standard = "c++23";
    return p;
}

// `provides` is empty for a unit that provides no module. A plain string and
// not an `std::optional<std::string>`, which does not construct under clang
// with the MSVC STL (see `CompileUnit::providesModule`).
mcpp::modgraph::SourceUnit unit(const std::filesystem::path& root, std::string_view rel,
                                std::string_view pkg, std::string_view provides,
                                std::vector<std::string> requires_) {
    mcpp::modgraph::SourceUnit u;
    u.path        = root / rel;
    u.relPath     = std::filesystem::path(rel);
    u.packageName = std::string(pkg);
    u.kind        = provides.empty() ? mcpp::SourceKind::Cxx : mcpp::SourceKind::ModuleInterface;
    if (!provides.empty()) {
        u.provides = mcpp::modgraph::ModuleId{std::string(provides)};
        u.providesInterface = true;
        u.declaration = mcpp::modgraph::ModuleDeclaration::Interface;
    }
    for (auto& r : requires_) u.requires_.push_back(mcpp::modgraph::ModuleId{r});
    std::filesystem::create_directories(u.path.parent_path());
    std::ofstream(u.path) << "// test\n";
    return u;
}

struct CoreView {
    std::string bmiFile;
    std::vector<std::string> flags;
    std::string mapContent;
    std::vector<std::string> arguments;
};

struct Planned {
    std::map<std::string, CoreView> core;   // by source file name
    bool needsPic = false;
};

// A workspace plan (virtual root `workspace`) of `app -> core`, and with
// `withTool` also `tool`, which provides its own module `m` as `core` does.
std::expected<BuildPlan, std::string> plan_workspace(const Tmp& t, mcpp::toolchain::CompilerId id,
                                                     bool withTool);

std::optional<Planned> plan_core(const Tmp& t, mcpp::toolchain::CompilerId id, bool withTool,
                                 std::string* err) {
    auto plan = plan_workspace(t, id, withTool);
    if (!plan) { if (err) *err = plan.error(); return std::nullopt; }
    Planned out;
    out.needsPic = plan->needsPic;
    for (auto const& cu : plan->compileUnits) {
        if (cu.packageName != "core") continue;
        CoreView v;
        v.bmiFile = cu.bmiFile;
        v.flags = cu.packageCxxflags;
        if (auto it = plan->moduleScopes.find(cu.moduleScope); it != plan->moduleScopes.end()) {
            v.mapContent = it->second.content;
            v.arguments  = it->second.arguments;
        }
        out.core[cu.source.filename().string()] = std::move(v);
    }
    return out;
}

std::expected<BuildPlan, std::string> plan_workspace(const Tmp& t, mcpp::toolchain::CompilerId id,
                                                     bool withTool) {
    const auto ws = t.path / "ws";
    mcpp::manifest::Manifest root;
    root.package.name        = "workspace";
    root.package.version     = "0.0.0";
    root.package.standard    = "c++23";
    root.package.virtualRoot = true;

    std::vector<mcpp::modgraph::PackageRoot> packages;
    auto rootPkg = package(ws, "workspace");
    rootPkg.manifest = root;
    packages.push_back(rootPkg);
    packages.push_back(package(ws / "core", "core"));
    packages.push_back(package(ws / "app", "app"));
    if (withTool) packages.push_back(package(ws / "tool", "tool"));

    mcpp::modgraph::Graph graph;
    graph.units.push_back(unit(ws / "core", "src/m.cppm", "core", "m", {"std"}));
    graph.units.push_back(unit(ws / "core", "src/core.cppm", "core", "corelib", {"m"}));
    graph.units.push_back(unit(ws / "app", "src/main.cpp", "app", "", {"corelib"}));
    if (withTool) {
        graph.units.push_back(unit(ws / "tool", "src/m.cppm", "tool", "m", {}));
        graph.units.push_back(unit(ws / "tool", "src/main.cpp", "tool", "", {"m"}));
    }
    for (std::size_t i = 0; i < graph.units.size(); ++i)
        if (auto const& p = graph.units[i].provides)
            graph.providersOf[p->logicalName].push_back(i);
    for (auto const& [name, units] : graph.providersOf)
        if (units.size() == 1) graph.producerOf[name] = units.front();
    graph.closures["core"] = {"core"};
    graph.closures["app"]  = {"app", "core"};
    if (withTool) graph.closures["tool"] = {"tool"};

    std::vector<std::size_t> topo;
    for (std::size_t i = 0; i < graph.units.size(); ++i) topo.push_back(i);

    return make_plan(root, toolchain(id), {}, graph, topo, packages, ws,
                     ws / "target" / "t", {}, {}, {});
}

void expect_same_core(mcpp::toolchain::CompilerId id) {
    Tmp t;
    std::string err;
    auto alone  = plan_core(t, id, false, &err);
    ASSERT_TRUE(alone) << err;
    auto beside = plan_core(t, id, true, &err);
    ASSERT_TRUE(beside) << err;
    ASSERT_EQ(alone->core.size(), 2u);
    ASSERT_EQ(beside->core.size(), 2u);
    for (auto const& [file, a] : alone->core) {
        auto const& b = beside->core.at(file);
        EXPECT_EQ(a.bmiFile, b.bmiFile) << file;
        EXPECT_EQ(a.flags, b.flags) << file;
        EXPECT_EQ(a.mapContent, b.mapContent) << file;
        EXPECT_EQ(a.arguments, b.arguments) << file;
    }
    EXPECT_EQ(alone->needsPic, beside->needsPic);
}

} // namespace

// The load-bearing assertion: a provider of the same module name elsewhere in
// the graph does not move `core`'s BMIs or change a word of its commands.
TEST(ModuleAddress, MemberCommandsAreImmuneToAnotherProviderOfTheName) {
    expect_same_core(mcpp::toolchain::CompilerId::GCC);
    expect_same_core(mcpp::toolchain::CompilerId::Clang);
}

// Every BMI of a package other than the root lies below that package's
// directory, and a unit is told where through one map that lists its own
// module and what it reaches through its imports, and nothing else.
TEST(ModuleAddress, MemberBmisAreQualifiedAndTheMapIsTheUnitsReach) {
    Tmp t;
    std::string err;
    auto gcc = plan_core(t, mcpp::toolchain::CompilerId::GCC, true, &err);
    ASSERT_TRUE(gcc) << err;
    auto const& iface = gcc->core.at("core.cppm");
    EXPECT_EQ(iface.bmiFile, "core/corelib.gcm");
    EXPECT_NE(iface.mapContent.find("corelib gcm.cache/core/corelib.gcm\n"), std::string::npos);
    EXPECT_NE(iface.mapContent.find("m gcm.cache/core/m.gcm\n"), std::string::npos);
    EXPECT_NE(iface.mapContent.find("std gcm.cache/std.gcm\n"), std::string::npos);
    EXPECT_EQ(iface.mapContent.find("tool"), std::string::npos)
        << "the map names a module the unit does not reach:\n" << iface.mapContent;
    ASSERT_FALSE(iface.flags.empty());
    EXPECT_TRUE(iface.flags.back().find("-fmodule-mapper=modmap/core-") != std::string::npos)
        << iface.flags.back();

    auto clang = plan_core(t, mcpp::toolchain::CompilerId::Clang, true, &err);
    ASSERT_TRUE(clang) << err;
    auto const& ci = clang->core.at("core.cppm");
    EXPECT_EQ(ci.bmiFile, "core/corelib.pcm");
    // clang reads the imported module's BMI from the argument file; its own
    // module is written through -fmodule-output and is not listed there.
    ASSERT_EQ(ci.arguments.size(), 1u);
    EXPECT_TRUE(ci.arguments.front().starts_with("-fmodule-file=m="));
    EXPECT_TRUE(ci.arguments.front().ends_with("pcm.cache/core/m.pcm")) << ci.arguments.front();
    ASSERT_FALSE(ci.flags.empty());
    EXPECT_TRUE(ci.flags.back().find(".modmap") != std::string::npos) << ci.flags.back();
    EXPECT_EQ(ci.flags.back().front(), '@');
}

// The root package's modules stay at their names, and a unit that reaches only
// those and `std` is told nothing: a project without dependency modules is laid
// out, and every command spelled, as before.
TEST(ModuleAddress, RootModulesStayFlatAndNeedNoMap) {
    Tmp t;
    const auto proj = t.path / "proj";
    mcpp::manifest::Manifest root;
    root.package.name     = "app";
    root.package.version  = "0.1.0";
    root.package.standard = "c++23";
    std::vector<mcpp::modgraph::PackageRoot> packages;
    auto rootPkg = package(proj, "app");
    rootPkg.manifest = root;
    packages.push_back(rootPkg);

    mcpp::modgraph::Graph graph;
    graph.units.push_back(unit(proj, "src/m.cppm", "app", "m", {"std"}));
    graph.units.push_back(unit(proj, "src/main.cpp", "app", "", {"m", "std"}));
    graph.providersOf["m"] = {0};
    graph.producerOf["m"] = 0;
    graph.closures["app"] = {"app"};
    std::vector<std::size_t> topo{0, 1};

    for (auto id : {mcpp::toolchain::CompilerId::GCC, mcpp::toolchain::CompilerId::Clang}) {
        auto plan = make_plan(root, toolchain(id), {}, graph, topo, packages, proj,
                              proj / "target" / "t", {}, {}, {});
        ASSERT_TRUE(plan) << plan.error();
        EXPECT_TRUE(plan->moduleScopes.empty());
        for (auto const& cu : plan->compileUnits) {
            EXPECT_TRUE(cu.moduleScope.empty()) << cu.source;
            if (!cu.providesModule.empty())
                EXPECT_EQ(cu.bmiFile.find('/'), std::string::npos) << cu.bmiFile;
        }
    }
}

// Position independence is a property of the target (B2): every unit of a
// hosted ELF target is compiled position-independent whether or not the graph
// links a shared library, and a freestanding target's units are not.
TEST(ModuleAddress, PositionIndependenceFollowsTheTargetNotTheGraph) {
    Tmp t;
    std::string err;
    auto planned = plan_core(t, mcpp::toolchain::CompilerId::GCC, false, &err);
    ASSERT_TRUE(planned) << err;
    EXPECT_TRUE(planned->needsPic);

    const auto proj = t.path / "bare";
    mcpp::manifest::Manifest root;
    root.package.name     = "fw";
    root.package.version  = "0.1.0";
    root.package.standard = "c++23";
    std::vector<mcpp::modgraph::PackageRoot> packages;
    auto rootPkg = package(proj, "fw");
    rootPkg.manifest = root;
    packages.push_back(rootPkg);
    mcpp::modgraph::Graph graph;
    graph.units.push_back(unit(proj, "src/main.cpp", "fw", "", {}));
    auto tc = toolchain(mcpp::toolchain::CompilerId::GCC);
    tc.targetTriple = "arm-none-eabi";
    auto plan = make_plan(root, tc, {}, graph, {0}, packages, proj, proj / "target" / "t",
                          {}, {}, {});
    ASSERT_TRUE(plan) << plan.error();
    EXPECT_FALSE(plan->needsPic);
}

// Every argument file a unit names is the one its own map states, with the
// arguments the backend writes into it. A module and a unit importing it in
// one package can list the same BMIs while only the importer loads one; when
// the two shared a key, the importer named a file nothing wrote.
TEST(ModuleAddress, EveryNamedArgumentFileIsOneThePlanStates) {
    Tmp t;
    auto plan = plan_workspace(t, mcpp::toolchain::CompilerId::Clang, true);
    ASSERT_TRUE(plan) << plan.error();
    std::size_t named = 0;
    for (auto const& cu : plan->compileUnits) {
        for (auto const& f : cu.packageCxxflags) {
            if (!f.starts_with("@")) continue;
            ++named;
            auto it = plan->moduleScopes.find(cu.moduleScope);
            ASSERT_NE(it, plan->moduleScopes.end()) << cu.source;
            EXPECT_FALSE(it->second.arguments.empty()) << cu.source;
            EXPECT_FALSE(it->second.argsFile.empty()) << cu.source;
            EXPECT_TRUE(f.ends_with(it->second.argsFile.filename().string())) << f;
        }
    }
    // core.cppm, app's main.cpp and tool's main.cpp import a module of
    // another package or of a member.
    EXPECT_EQ(named, 3u);
}

// The argument file of a module map, as each compiler reads it. MSVC takes an
// option's value only from the same line of a command file (`D8004: '/reference'
// requires an argument` on windows-2025 when they were on two lines), and reads
// a non-ASCII path only with a byte order mark; clang reads GNU rules, where a
// word with a space is quoted.
TEST(ModuleAddress, ArgumentFilesAreWrittenAsEachCompilerReadsThem) {
    Tmp t;
    auto read = [](const std::filesystem::path& p) {
        std::ifstream in(p, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), {});
    };

    BuildPlan msvc;
    msvc.outputDir = t.path / "msvc";
    msvc.toolchain.compiler = mcpp::toolchain::CompilerId::MSVC;
    ModuleScope ms;
    ms.mapFile = "modmap/core-1.map";
    ms.content = "m ifc.cache/core/m.ifc\n";
    ms.argsFile = "modmap/core-1.modmap";
    ms.arguments = {"/reference", "m=C:/build/ifc.cache/core/m.ifc",
                    "/reference", "n=C:/build/ifc.cache/core/n.ifc"};
    msvc.moduleScopes.emplace("core-1", ms);
    write_module_maps(msvc);
    EXPECT_EQ(read(msvc.outputDir / "modmap/core-1.modmap"),
              "\xEF\xBB\xBF/reference m=C:/build/ifc.cache/core/m.ifc\n"
              "/reference n=C:/build/ifc.cache/core/n.ifc\n");
    EXPECT_EQ(read(msvc.outputDir / "modmap/core-1.map"), ms.content);

    BuildPlan clang;
    clang.outputDir = t.path / "clang";
    clang.toolchain.compiler = mcpp::toolchain::CompilerId::Clang;
    ModuleScope cs;
    cs.mapFile = "modmap/core-2.map";
    cs.content = "m pcm.cache/core/m.pcm\n";
    cs.argsFile = "modmap/core-2.modmap";
    cs.arguments = {"-fmodule-file=m=/build dir/pcm.cache/core/m.pcm",
                    "-fmodule-file=n=/build/pcm.cache/core/n.pcm"};
    clang.moduleScopes.emplace("core-2", cs);
    write_module_maps(clang);
    EXPECT_EQ(read(clang.outputDir / "modmap/core-2.modmap"),
              "\"-fmodule-file=m=/build dir/pcm.cache/core/m.pcm\"\n"
              "-fmodule-file=n=/build/pcm.cache/core/n.pcm\n");
}
