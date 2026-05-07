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
