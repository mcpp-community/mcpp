// mcpp#344 — a package's object addresses must be a function of that package
// alone.
//
// This is the machine-checkable form of the invariant the global build cache
// depends on. The cache key deliberately excludes the consuming project (that
// is what makes an entry shareable across projects), so if a dependency's
// object layout can shift when the consumer pulls in some UNRELATED package,
// two consumers write and read incompatible layouts under one key. #344 was
// exactly that: `compat.zlib`'s compress.o was `obj/compress.o` alone and
// `obj/compat_zlib/zlib-1.3.2/compress.o` alongside `compat.bzip2` (which ships
// its own compress.c), because basename disambiguation (#233) was driven by a
// census over every unit in the build.
//
// The test therefore builds the SAME dependency twice — once alone, once beside
// a package engineered to collide with it — and demands byte-identical
// addresses. It fails on the pre-#344 tree.

#include <gtest/gtest.h>

import std;
import mcpp.build.plan;
import mcpp.manifest;
import mcpp.modgraph.graph;
import mcpp.modgraph.scanner;
import mcpp.toolchain.model;

using namespace mcpp::build;

namespace {

struct Tmp {
    std::filesystem::path path;
    Tmp() {
        path = std::filesystem::temp_directory_path()
             / std::format("mcpp_obj_addr_{}", std::random_device{}());
        std::filesystem::create_directories(path);
    }
    ~Tmp() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

void touchFile(const std::filesystem::path& p) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream(p) << "/* test */\n";
}

mcpp::toolchain::Toolchain gccLike() {
    mcpp::toolchain::Toolchain tc;
    tc.compiler     = mcpp::toolchain::CompilerId::GCC;
    tc.version      = "16.1.0";
    tc.binaryPath   = "/usr/bin/g++";
    tc.targetTriple = "x86_64-linux-gnu";
    return tc;
}

mcpp::modgraph::PackageRoot makePackage(const std::filesystem::path& root,
                                        std::string_view name) {
    mcpp::modgraph::PackageRoot p;
    p.root = root;
    p.manifest.package.name     = std::string(name);
    p.manifest.package.version  = "1.0.0";
    p.manifest.package.standard = "c++23";
    return p;
}

// One C source per package, all sharing the basename `compress.c` — the shape
// that used to trigger the global census.
mcpp::modgraph::SourceUnit unitFor(const std::filesystem::path& pkgRoot,
                                   const std::filesystem::path& rel,
                                   std::string_view pkgName) {
    mcpp::modgraph::SourceUnit u;
    u.path        = pkgRoot / rel;
    u.relPath     = rel;
    u.packageName = std::string(pkgName);
    touchFile(u.path);
    return u;
}

struct Built {
    std::filesystem::path object;
    std::filesystem::path packageObjectRel;
};

// Plan a graph containing the root project plus `deps`, and return the
// addresses computed for the FIRST dependency's single unit.
std::optional<Built> planZlib(const Tmp& t, bool withBzip2, std::string* err) {
    auto projectRoot = t.path / "proj";
    auto storeRoot   = t.path / "store";

    mcpp::manifest::Manifest rootManifest;
    rootManifest.package.name     = "app";
    rootManifest.package.version  = "0.1.0";
    rootManifest.package.standard = "c++23";
    mcpp::manifest::Target lib;
    lib.name = "app";
    lib.kind = mcpp::manifest::Target::Library;
    rootManifest.targets.push_back(lib);

    std::vector<mcpp::modgraph::PackageRoot> packages;
    auto rootPkg = makePackage(projectRoot, "app");
    rootPkg.manifest = rootManifest;
    packages.push_back(rootPkg);
    packages.push_back(makePackage(storeRoot / "compat.zlib@1.3.2", "compat.zlib"));
    if (withBzip2)
        packages.push_back(
            makePackage(storeRoot / "compat.bzip2@1.0.8", "compat.bzip2"));

    mcpp::modgraph::Graph graph;
    graph.units.push_back(unitFor(projectRoot, "src/app.cpp", "app"));
    graph.units.push_back(
        unitFor(packages[1].root, "zlib-1.3.2/compress.c", "compat.zlib"));
    if (withBzip2)
        graph.units.push_back(
            unitFor(packages[2].root, "bzip2-1.0.8/compress.c", "compat.bzip2"));

    std::vector<std::size_t> topo;
    for (std::size_t i = 0; i < graph.units.size(); ++i) topo.push_back(i);

    auto plan = make_plan(rootManifest, gccLike(), {}, graph, topo, packages,
                          projectRoot, projectRoot / "target" / "t",
                          {}, {}, {storeRoot});
    if (!plan) { if (err) *err = plan.error(); return std::nullopt; }

    auto want = packages[1].root / "zlib-1.3.2" / "compress.c";
    for (auto& cu : plan->compileUnits) {
        if (cu.source != want) continue;
        return Built{cu.object, cu.packageObjectRel};
    }
    if (err) *err = "zlib compile unit not found in the plan";
    return std::nullopt;
}

} // namespace

// The load-bearing assertion. Adding an unrelated, deliberately colliding
// package to the graph must not move zlib's object by one byte — not its build
// path, and above all not its cache address.
TEST(ObjectAddress, DependencyAddressesAreImmuneToTheRestOfTheGraph) {
    Tmp t;
    std::string errAlone, errBeside;
    auto alone  = planZlib(t, /*withBzip2=*/false, &errAlone);
    auto beside = planZlib(t, /*withBzip2=*/true,  &errBeside);
    ASSERT_TRUE(alone)  << errAlone;
    ASSERT_TRUE(beside) << errBeside;

    EXPECT_EQ(alone->packageObjectRel, beside->packageObjectRel)
        << "the cache-entry address moved when an unrelated package joined the "
           "graph — that is mcpp#344";
    EXPECT_EQ(alone->object, beside->object);
}

// The address must also be package-internal: nothing about the consumer, and
// nothing that only exists on this machine. It is read back by another machine
// that computed the same key.
TEST(ObjectAddress, DependencyCacheAddressIsPackageInternal) {
    Tmp t;
    std::string err;
    auto built = planZlib(t, /*withBzip2=*/true, &err);
    ASSERT_TRUE(built) << err;

    auto rel = built->packageObjectRel.generic_string();
    EXPECT_FALSE(rel.empty());
    EXPECT_FALSE(built->packageObjectRel.is_absolute()) << rel;
    EXPECT_FALSE(rel.starts_with("..")) << rel;
    // Mirrors the source's path relative to its OWN package root, and carries
    // no package-partition component (that lives in the build path only).
    EXPECT_EQ(rel, "zlib-1.3.2/compress.o") << rel;

    // The build path does partition by package — that is what makes the
    // cross-package census unnecessary.
    auto obj = built->object.generic_string();
    EXPECT_EQ(obj, "obj/compat_zlib/zlib-1.3.2/compress.o") << obj;
}

// `path_is_under_any` decides both whether a package may be cached and where
// its objects are anchored, and it must answer LEXICALLY.
//
// A payload store whose entries are symlinks into another store is ordinary:
// tests/e2e/_inherit_toolchain.sh builds one so an isolated MCPP_HOME can reuse
// the developer's toolchains, and CI caches do the same to avoid re-downloading.
// std::filesystem::relative() runs weakly_canonical and resolves those links, at
// which point `<home>/registry/data/xpkgs/<pkg>` no longer looks like it is in
// the store — every such package silently drops out of the build cache. That
// regression was caught by e2e 40 and is pinned here where it is cheap.
TEST(ObjectAddress, PathContainmentIsLexicalSoSymlinkedStoresStillCount) {
    Tmp t;
    auto real  = t.path / "real-store";
    auto store = t.path / "home" / "registry" / "data" / "xpkgs";
    std::filesystem::create_directories(real / "compat.zlib@1.3.2" / "src");
    std::filesystem::create_directories(store);

    std::error_code ec;
    std::filesystem::create_directory_symlink(
        real / "compat.zlib@1.3.2", store / "compat.zlib@1.3.2", ec);
    if (ec) GTEST_SKIP() << "symlinks unavailable: " << ec.message();

    auto pkgRoot = store / "compat.zlib@1.3.2";
    EXPECT_TRUE(path_is_under_any(pkgRoot, {store}));
    EXPECT_TRUE(path_is_under_any(pkgRoot / "src" / "compress.c", {store}));

    // And it still says no to something genuinely outside — the gate has to
    // keep rejecting `target/.mangled/**`, which is the case it exists for.
    EXPECT_FALSE(path_is_under_any(t.path / "proj" / "target" / ".mangled" / "x",
                                   {store}));
    EXPECT_FALSE(path_is_under_any(pkgRoot, {}));
}

// The root project is never cached, so it keeps the flat layout every project
// has had since 0.0.1 — and a dependency shipping a same-named file can no
// longer force it to disambiguate.
TEST(ObjectAddress, RootObjectsStayFlatAndUncacheable) {
    Tmp t;
    auto projectRoot = t.path / "proj";
    auto storeRoot   = t.path / "store";

    mcpp::manifest::Manifest rootManifest;
    rootManifest.package.name     = "app";
    rootManifest.package.version  = "0.1.0";
    rootManifest.package.standard = "c++23";
    mcpp::manifest::Target lib;
    lib.name = "app";
    lib.kind = mcpp::manifest::Target::Library;
    rootManifest.targets.push_back(lib);

    std::vector<mcpp::modgraph::PackageRoot> packages;
    auto rootPkg = makePackage(projectRoot, "app");
    rootPkg.manifest = rootManifest;
    packages.push_back(rootPkg);
    packages.push_back(makePackage(storeRoot / "compat.zlib@1.3.2", "compat.zlib"));

    mcpp::modgraph::Graph graph;
    // Root ships its own compress.c, colliding with the dependency's.
    graph.units.push_back(unitFor(projectRoot, "src/compress.c", "app"));
    graph.units.push_back(
        unitFor(packages[1].root, "zlib-1.3.2/compress.c", "compat.zlib"));

    std::vector<std::size_t> topo{0, 1};
    auto plan = make_plan(rootManifest, gccLike(), {}, graph, topo, packages,
                          projectRoot, projectRoot / "target" / "t",
                          {}, {}, {storeRoot});
    ASSERT_TRUE(plan) << plan.error();

    for (auto& cu : plan->compileUnits) {
        if (cu.source == projectRoot / "src" / "compress.c") {
            EXPECT_EQ(cu.object.generic_string(), "obj/compress.o");
            EXPECT_TRUE(cu.packageObjectRel.empty())
                << "the root project must never get a cache address";
        }
    }
}
