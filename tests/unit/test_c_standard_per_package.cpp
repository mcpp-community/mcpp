// #695 -- every package's C units compile at that package's own C standard.
//
// `[build] c_standard` used to take effect only on the package being built: the
// file-level `$cflags` carried the root's value, so a dependency's C units
// compiled at the consumer's standard while its own declaration was parsed,
// hashed into its cache key and never applied. The file-level line now carries
// `kDefaultCStandard`, and `make_plan` appends a package's own standard to that
// package's C units when it differs. These assertions are on the plan's
// per-unit C flags, which the compile edge, the scan edge and both build
// databases read after the file-level flags.

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
             / std::format("mcpp_c_standard_{}", std::random_device{}());
        std::filesystem::create_directories(path);
    }
    ~Tmp() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

mcpp::toolchain::Toolchain clang_like() {
    mcpp::toolchain::Toolchain tc;
    tc.compiler     = mcpp::toolchain::CompilerId::Clang;
    tc.version      = "22.1.8";
    tc.binaryPath   = "/usr/bin/clang++";
    tc.targetTriple = "x86_64-linux-gnu";
    return tc;
}

mcpp::toolchain::Toolchain cl_like() {
    mcpp::toolchain::Toolchain tc;
    tc.compiler     = mcpp::toolchain::CompilerId::MSVC;
    tc.version      = "19.51";
    tc.binaryPath   = "cl.exe";
    tc.targetTriple = "x86_64-pc-windows-msvc";
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
    std::string rootStandard;              // the consumer's `c_standard`
    std::string depStandard = "gnu11";     // the dependency's `c_standard`
    std::string quietStandard;             // a dependency with no C units
    mcpp::toolchain::Toolchain tc = clang_like();
    // The root as an executable whose entry `main` is a C file. The entry is
    // synthesized by the link-unit pass, outside the scanned units.
    bool cEntry = false;
};

struct Result {
    // The C flags the plan gave each unit, by source file name.
    std::map<std::string, std::vector<std::string>> cflags;
    std::map<std::string, std::vector<std::string>> cxxflags;
    std::vector<std::string> notApplied;
};

Result plan_for(const Fixture& f) {
    Tmp t;
    const auto appRoot   = t.path / "app";
    const auto depRoot   = t.path / "cdep";
    const auto plainRoot = t.path / "plain";
    const auto quietRoot = t.path / "quiet";

    mcpp::manifest::Manifest app;
    app.package.name    = "app";
    app.package.version = "0.1.0";
    app.buildConfig.cStandard = f.rootStandard;
    mcpp::manifest::Target bin;
    bin.name = "app";
    bin.kind = mcpp::manifest::Target::Library;
    if (f.cEntry) {
        bin.kind = mcpp::manifest::Target::Binary;
        bin.main = "src/entry.c";
        std::filesystem::create_directories(appRoot / "src");
        std::ofstream(appRoot / "src" / "entry.c") << "int main(void) { return 0; }\n";
    }
    app.targets.push_back(bin);

    mcpp::manifest::Manifest dep;
    dep.package.name    = "cdep";
    dep.package.version = "0.1.0";
    dep.buildConfig.cStandard = f.depStandard;

    // A dependency that declares nothing: it compiles at the default, whatever
    // its consumer declares.
    mcpp::manifest::Manifest plain;
    plain.package.name    = "plain";
    plain.package.version = "0.1.0";

    // A dependency that declares a standard and has no C units.
    mcpp::manifest::Manifest quiet;
    quiet.package.name    = "quiet";
    quiet.package.version = "0.1.0";
    quiet.buildConfig.cStandard = f.quietStandard;

    std::vector<mcpp::modgraph::PackageRoot> packages;
    packages.push_back({appRoot, app});
    packages.push_back({depRoot, dep});
    packages.push_back({plainRoot, plain});
    packages.push_back({quietRoot, quiet});

    using K = mcpp::SourceKind;
    mcpp::modgraph::Graph graph;
    graph.units.push_back(unit(appRoot,   "src/app.c",    "app",   K::C));
    graph.units.push_back(unit(appRoot,   "src/main.cpp", "app",   K::Cxx));
    graph.units.push_back(unit(depRoot,   "src/cdep.c",   "cdep",  K::C));
    graph.units.push_back(unit(depRoot,   "src/more.c",   "cdep",  K::C));
    graph.units.push_back(unit(plainRoot, "src/plain.c",  "plain", K::C));
    graph.units.push_back(unit(quietRoot, "src/quiet.cpp","quiet", K::Cxx));

    std::vector<std::size_t> topo;
    for (std::size_t i = 0; i < graph.units.size(); ++i) topo.push_back(i);

    auto plan = make_plan(app, f.tc, {}, graph, topo, packages,
                          appRoot, appRoot / "target" / "t", {}, {}, {});
    EXPECT_TRUE(plan.has_value()) << (plan ? "" : plan.error());
    Result r;
    if (!plan) return r;
    for (auto const& cu : plan->compileUnits) {
        r.cflags[cu.source.filename().string()]   = cu.packageCflags;
        r.cxxflags[cu.source.filename().string()] = cu.packageCxxflags;
    }
    r.notApplied = plan->cStandardsNotApplied;
    return r;
}

bool has(const std::vector<std::string>& v, std::string_view flag) {
    return std::ranges::find(v, flag) != v.end();
}

bool has_std(const std::vector<std::string>& v) {
    return std::ranges::any_of(v, [](const std::string& s) { return s.starts_with("-std="); });
}

} // namespace

// The report's case: a dependency that declares `gnu11` compiles its C units at
// `gnu11`, and not at the consumer's standard.
TEST(CStandardPerPackage, ADependencyCompilesAtItsOwnStandard) {
    auto r = plan_for(Fixture{});
    ASSERT_FALSE(r.cflags["cdep.c"].empty());
    EXPECT_EQ(r.cflags["cdep.c"].back(), "-std=gnu11");
    EXPECT_EQ(r.cflags["more.c"].back(), "-std=gnu11");
}

// The consumer's standard reaches the consumer's own C units and no one else's.
TEST(CStandardPerPackage, TheRootsStandardStaysInTheRoot) {
    Fixture f;
    f.rootStandard = "c99";
    auto r = plan_for(f);
    ASSERT_FALSE(r.cflags["app.c"].empty());
    EXPECT_EQ(r.cflags["app.c"].back(), "-std=c99");
    EXPECT_FALSE(has(r.cflags["plain.c"], "-std=c99"));
    EXPECT_FALSE(has_std(r.cflags["plain.c"]))
        << "a package that declares nothing compiles at the file-level default";
    EXPECT_EQ(r.cflags["cdep.c"].back(), "-std=gnu11");
}

// A declaration equal to the default adds nothing: the file-level line already
// carries it, so the command line is unchanged.
TEST(CStandardPerPackage, DeclaringTheDefaultAddsNothing) {
    Fixture f;
    f.depStandard = "c11";
    auto r = plan_for(f);
    EXPECT_FALSE(has_std(r.cflags["cdep.c"]));
}

// A C standard is a C setting: no C++ unit receives one.
TEST(CStandardPerPackage, NoCxxUnitReceivesACStandard) {
    Fixture f;
    f.rootStandard = "c99";
    auto r = plan_for(f);
    EXPECT_FALSE(has_std(r.cxxflags["main.cpp"]));
    EXPECT_FALSE(has_std(r.cflags["main.cpp"]));
}

// cl.exe compiles C in its default mode and takes no C `/std:` from mcpp yet
// (W3b). Nothing is added to a unit, and every declaring package that has C
// units is recorded once, so the backend can say so in one line.
TEST(CStandardPerPackage, ClRecordsWhatItDoesNotApply) {
    Fixture f;
    f.tc = cl_like();
    f.rootStandard  = "c11";
    f.quietStandard = "c17";
    auto r = plan_for(f);
    for (auto const& [file, v] : r.cflags)
        for (auto const& flag : v)
            EXPECT_FALSE(flag.starts_with("-std=") || flag.starts_with("/std:c"))
                << file << ": " << flag;
    EXPECT_EQ(r.notApplied, (std::vector<std::string>{"app (c11)", "cdep (gnu11)"}))
        << "a package with no C units (quiet) is not reported";
}

// With no cl.exe in the build, nothing is recorded as unapplied.
TEST(CStandardPerPackage, GnuDriversApplyEverythingTheyAreGiven) {
    Fixture f;
    f.rootStandard = "c99";
    auto r = plan_for(f);
    EXPECT_TRUE(r.notApplied.empty());
}

// A target's entry `main` written in C is a unit of the root package: it
// compiles at the root's standard, as a scanned C unit does.
TEST(CStandardPerPackage, ACEntryMainTakesItsPackagesStandard) {
    Fixture f;
    f.rootStandard = "c99";
    f.cEntry = true;
    auto r = plan_for(f);
    ASSERT_TRUE(r.cflags.contains("entry.c"))
        << "the plan has no unit for the entry main";
    ASSERT_FALSE(r.cflags["entry.c"].empty());
    EXPECT_EQ(r.cflags["entry.c"].back(), "-std=c99");
}
