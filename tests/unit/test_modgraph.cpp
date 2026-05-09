#include <gtest/gtest.h>

import std;
import mcpp.modgraph.graph;
import mcpp.modgraph.scanner;
import mcpp.modgraph.validate;
import mcpp.manifest;

using namespace mcpp::modgraph;

namespace {

std::filesystem::path make_tempdir(std::string_view prefix) {
    auto tmp = std::filesystem::temp_directory_path();
    auto dir = tmp / std::format("{}-{}", prefix, std::random_device{}() );
    std::filesystem::create_directories(dir);
    return dir;
}

void write(const std::filesystem::path& p, std::string_view content) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream os(p);
    os << content;
}

} // namespace

TEST(Scanner, ProvidesAndRequires) {
    auto dir = make_tempdir("mcpp-scanner");
    write(dir / "src" / "foo.cppm", R"(export module foo;
import std;
import bar;
export int answer();
)");

    auto u = scan_file(dir / "src" / "foo.cppm", "pkg");
    ASSERT_TRUE(u.has_value()) << u.error().format();
    ASSERT_TRUE(u->provides.has_value());
    EXPECT_EQ(u->provides->logicalName, "foo");
    ASSERT_EQ(u->requires_.size(), 2u);
    EXPECT_EQ(u->requires_[0].logicalName, "std");
    EXPECT_EQ(u->requires_[1].logicalName, "bar");

    std::filesystem::remove_all(dir);
}

TEST(Scanner, PartitionImportFromPrimaryInterface) {
    // Primary module interface: `export module foo;` → logicalName = "foo".
    // `import :tls;` resolves to "foo:tls".
    auto dir = make_tempdir("mcpp-scanner");
    write(dir / "src" / "foo.cppm", R"(export module foo;
import :tls;
)");
    auto u = scan_file(dir / "src" / "foo.cppm", "pkg");
    ASSERT_TRUE(u.has_value()) << u.error().format();
    ASSERT_EQ(u->requires_.size(), 1u);
    EXPECT_EQ(u->requires_[0].logicalName, "foo:tls");
    std::filesystem::remove_all(dir);
}

TEST(Scanner, PartitionImportFromAnotherPartition) {
    // Partition interface: `export module foo:http;` → logicalName = "foo:http".
    // `import :tls;` must resolve to "foo:tls" (the sibling partition),
    // NOT "foo:http:tls" (which is what a naive prepend produces).
    auto dir = make_tempdir("mcpp-scanner");
    write(dir / "src" / "http.cppm", R"(export module foo:http;
import :tls;
import :socket;
)");
    auto u = scan_file(dir / "src" / "http.cppm", "pkg");
    ASSERT_TRUE(u.has_value()) << u.error().format();
    ASSERT_TRUE(u->provides.has_value());
    EXPECT_EQ(u->provides->logicalName, "foo:http");
    ASSERT_EQ(u->requires_.size(), 2u);
    EXPECT_EQ(u->requires_[0].logicalName, "foo:tls");
    EXPECT_EQ(u->requires_[1].logicalName, "foo:socket");
    std::filesystem::remove_all(dir);
}

TEST(Scanner, PartitionImportWithDottedModuleName) {
    // Dotted module names (xpkg-style, e.g. `mcpplibs.tinyhttps:http`)
    // — only the colon-prefixed partition suffix is what we strip.
    auto dir = make_tempdir("mcpp-scanner");
    write(dir / "src" / "http.cppm", R"(export module mcpplibs.tinyhttps:http;
import :tls;
)");
    auto u = scan_file(dir / "src" / "http.cppm", "pkg");
    ASSERT_TRUE(u.has_value()) << u.error().format();
    ASSERT_EQ(u->requires_.size(), 1u);
    EXPECT_EQ(u->requires_[0].logicalName, "mcpplibs.tinyhttps:tls");
    std::filesystem::remove_all(dir);
}

TEST(Scanner, RejectsConditionalImport) {
    auto dir = make_tempdir("mcpp-scanner");
    write(dir / "main.cpp", R"(import std;
#ifdef WANT_X
import x;
#endif
int main(){})");
    auto r = scan_file(dir / "main.cpp", "pkg");
    EXPECT_FALSE(r.has_value());
    EXPECT_NE(r.error().message.find("conditional"), std::string::npos);
    std::filesystem::remove_all(dir);
}

TEST(Scanner, RejectsHeaderUnit) {
    auto dir = make_tempdir("mcpp-scanner");
    write(dir / "main.cpp", R"(import std;
import "x.h";
int main(){})");
    auto r = scan_file(dir / "main.cpp", "pkg");
    EXPECT_FALSE(r.has_value());
    EXPECT_NE(r.error().message.find("header units"), std::string::npos);
    std::filesystem::remove_all(dir);
}

TEST(Validate, NamingRequiresPackagePrefix) {
    Graph g;
    SourceUnit u;
    u.path = "/x/foo.cppm";
    u.packageName = "myorg.foo";        // unit's own package (post-multi-pkg refactor)
    u.provides = ModuleId{"core"};
    g.units.push_back(u);
    g.producerOf["core"] = 0;

    mcpp::manifest::Manifest m;
    m.package.name = "myorg.foo";

    auto rep = validate(g, m);
    EXPECT_FALSE(rep.ok());
    bool found = false;
    for (auto& e : rep.errors) {
        if (e.message.find("prefixed by package name") != std::string::npos) { found = true; break; }
    }
    EXPECT_TRUE(found) << "expected naming-violation error";
}

TEST(Validate, ForbiddenTopName) {
    Graph g;
    SourceUnit u;
    u.path = "/x/foo.cppm";
    u.packageName = "myorg.foo";
    u.provides = ModuleId{"util"};
    g.units.push_back(u);
    g.producerOf["util"] = 0;

    mcpp::manifest::Manifest m;
    m.package.name = "myorg.foo";

    auto rep = validate(g, m);
    EXPECT_FALSE(rep.ok());
}

TEST(Validate, LibRootHappyPath) {
    // Project: lib target "tinyhttps", convention puts the lib root at
    // src/tinyhttps.cppm exporting `mcpplibs.tinyhttps`. Two partition
    // siblings sit alongside.
    Graph g;
    SourceUnit root;
    root.path = "src/tinyhttps.cppm";
    root.packageName = "mcpplibs.tinyhttps";
    root.provides = ModuleId{"mcpplibs.tinyhttps"};
    g.units.push_back(root);
    g.producerOf["mcpplibs.tinyhttps"] = 0;
    SourceUnit p1;
    p1.path = "src/tls.cppm";
    p1.packageName = "mcpplibs.tinyhttps";
    p1.provides = ModuleId{"mcpplibs.tinyhttps:tls"};
    g.units.push_back(p1);
    g.producerOf["mcpplibs.tinyhttps:tls"] = 1;

    mcpp::manifest::Manifest m;
    m.package.name = "mcpplibs.tinyhttps";
    mcpp::manifest::Target t;
    t.name = "tinyhttps";
    t.kind = mcpp::manifest::Target::Library;
    m.targets.push_back(t);

    auto rep = validate(g, m);     // empty projectRoot → on-disk check skipped
    EXPECT_TRUE(rep.ok()) << "errors:" << [&]{
        std::string s; for (auto& e : rep.errors) s += "\n  " + e.message; return s;
    }();
}

TEST(Validate, LibRootExportsPartitionIsError) {
    // Lib root file at the conventional path exports `:foo` (a partition
    // suffix) — must be rejected: lib root must be the primary module.
    Graph g;
    SourceUnit u;
    u.path = "src/tinyhttps.cppm";
    u.packageName = "mcpplibs.tinyhttps";
    u.provides = ModuleId{"mcpplibs.tinyhttps:something"};
    g.units.push_back(u);
    g.producerOf["mcpplibs.tinyhttps:something"] = 0;

    mcpp::manifest::Manifest m;
    m.package.name = "mcpplibs.tinyhttps";
    mcpp::manifest::Target t;
    t.name = "tinyhttps";
    t.kind = mcpp::manifest::Target::Library;
    m.targets.push_back(t);

    auto rep = validate(g, m);
    EXPECT_FALSE(rep.ok());
    bool found = false;
    for (auto& e : rep.errors) {
        if (e.message.find("partition") != std::string::npos
            && e.message.find("primary module") != std::string::npos) { found = true; break; }
    }
    EXPECT_TRUE(found) << "expected lib-root partition error";
}

TEST(Validate, LibRootWrongModuleNameIsError) {
    // Lib root file exports a module that doesn't match [package].name.
    Graph g;
    SourceUnit u;
    u.path = "src/tinyhttps.cppm";
    u.packageName = "mcpplibs.tinyhttps";
    u.provides = ModuleId{"some.other.module"};
    g.units.push_back(u);
    g.producerOf["some.other.module"] = 0;

    mcpp::manifest::Manifest m;
    m.package.name = "mcpplibs.tinyhttps";
    mcpp::manifest::Target t;
    t.name = "tinyhttps";
    t.kind = mcpp::manifest::Target::Library;
    m.targets.push_back(t);

    auto rep = validate(g, m);
    EXPECT_FALSE(rep.ok());
    bool found = false;
    for (auto& e : rep.errors) {
        if (e.message.find("expected 'mcpplibs.tinyhttps'") != std::string::npos) {
            found = true; break;
        }
    }
    EXPECT_TRUE(found) << "expected expected-module-name error";
}

TEST(Validate, LibRootNotEnforcedForBinaryProject) {
    // Pure-binary project: no lib target → no lib-root checks. Even if a
    // file at src/<tail>.cppm exists exporting an unrelated module, no
    // error should fire.
    Graph g;
    mcpp::manifest::Manifest m;
    m.package.name = "myapp";
    mcpp::manifest::Target t;
    t.name = "myapp";
    t.kind = mcpp::manifest::Target::Binary;
    t.main = "src/main.cpp";
    m.targets.push_back(t);

    auto rep = validate(g, m);
    EXPECT_TRUE(rep.ok());
}

TEST(Validate, LibRootMissingFileWithExplicitPathIsError) {
    Graph g;
    mcpp::manifest::Manifest m;
    m.package.name = "myorg.foo";
    m.lib.path = "src/does-not-exist.cppm";
    mcpp::manifest::Target t;
    t.name = "foo";
    t.kind = mcpp::manifest::Target::Library;
    m.targets.push_back(t);

    // Pass a non-empty projectRoot so the on-disk check is enabled.
    auto rep = validate(g, m, std::filesystem::current_path());
    EXPECT_FALSE(rep.ok());
    bool found = false;
    for (auto& e : rep.errors) {
        if (e.message.find("does not exist") != std::string::npos) { found = true; break; }
    }
    EXPECT_TRUE(found) << "expected explicit-path-missing error";
}

TEST(TopoSort, DetectsCycle) {
    Graph g;
    g.units.resize(2);
    g.units[0].provides = ModuleId{"a"};
    g.units[1].provides = ModuleId{"b"};
    g.units[0].requires_.push_back({"b"});
    g.units[1].requires_.push_back({"a"});
    g.producerOf["a"] = 0;
    g.producerOf["b"] = 1;
    g.edges.push_back({0, 1});  // a->b
    g.edges.push_back({1, 0});  // b->a

    auto r = topo_sort(g);
    EXPECT_FALSE(r.has_value());
}

TEST(IsPublicPackage, DotMarksPublic) {
    EXPECT_TRUE(is_public_package_name("myorg.foo"));
    EXPECT_FALSE(is_public_package_name("foo"));
}

TEST(IsForbiddenTopModule, KnownNames) {
    EXPECT_TRUE(is_forbidden_top_module("core"));
    EXPECT_TRUE(is_forbidden_top_module("util.x"));
    EXPECT_FALSE(is_forbidden_top_module("myorg.foo"));
}
