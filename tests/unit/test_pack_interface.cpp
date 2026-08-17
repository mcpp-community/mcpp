#include <gtest/gtest.h>

import std;
import mcpp.pack.interface;
import mcpp.modgraph.graph;

using namespace mcpp::pack;
using mcpp::modgraph::Graph;
using mcpp::modgraph::ModuleId;
using mcpp::modgraph::SourceUnit;

namespace {

// The shape of the library the design is written against:
//
//   mathkit.cppm   export module mathkit;  export import :api;   ← root
//   api.cppm       export module mathkit:api;                    ← interface partition
//   secret.cppm    module mathkit:secret;   ← implementation partition, PRIVATE
//   impl.cpp       module mathkit;  import :secret;
//   capi.c         (no module at all)
//
// mcpp's text scanner does not record `module M:part;` as a PROVIDER, so
// `secret.cppm` has no `provides` here — that is deliberately how the graph
// really looks, not a simplification.
Graph library_graph(bool interfaceReachesSecret = false) {
    Graph g;
    auto add = [&](std::string path, std::optional<std::string> provides,
                   std::vector<std::string> requires_) {
        SourceUnit u;
        u.path        = std::move(path);
        u.packageName = "mathkit";
        if (provides) u.provides = ModuleId{ *provides };
        for (auto& r : requires_) u.requires_.push_back(ModuleId{ std::move(r) });
        g.units.push_back(std::move(u));
    };

    add("src/mathkit.cppm", "mathkit",
        interfaceReachesSecret ? std::vector<std::string>{ "mathkit:api", "mathkit:secret" }
                               : std::vector<std::string>{ "mathkit:api" });
    add("src/api.cppm",   "mathkit:api", {});
    add("src/secret.cppm", std::nullopt,  { "mathkit" });
    add("src/impl.cpp",    std::nullopt,  { "mathkit", "mathkit:secret" });
    add("src/capi.c",      std::nullopt,  {});

    for (std::size_t i = 0; i < g.units.size(); ++i)
        if (g.units[i].provides)
            g.producerOf.emplace(g.units[i].provides->logicalName, i);
    return g;
}

std::vector<std::string> names(const std::vector<std::filesystem::path>& v) {
    std::vector<std::string> out;
    for (auto const& p : v) out.push_back(p.filename().string());
    std::ranges::sort(out);
    return out;
}

}  // namespace

// ─── what travels, and what does not ───────────────────────────────────────

TEST(InterfaceClosure, PublishesTheRootAndItsInterfacePartitionOnly) {
    auto c = interface_closure(library_graph(), "mathkit", "mathkit");
    ASSERT_TRUE(c.has_value()) << (c ? "" : c.error());
    EXPECT_EQ(names(c->published), (std::vector<std::string>{"api.cppm", "mathkit.cppm"}));
}

TEST(InterfaceClosure, WithholdsTheImplementationPartitionSource) {
    // The whole point for a closed-source library: `secret.cppm` produces a
    // BMI and an object, and its SOURCE must not be published.
    auto c = interface_closure(library_graph(), "mathkit", "mathkit");
    ASSERT_TRUE(c.has_value());
    EXPECT_EQ(names(c->withheld),
              (std::vector<std::string>{"capi.c", "impl.cpp", "secret.cppm"}));
}

TEST(InterfaceClosure, DropSetIsThePublishedObjectsNotEveryModuleObject) {
    // Measured: deleting every `.m.o` also deletes `secret.m.o`, which holds
    // real code, and every target then fails to link with an undefined
    // reference that names neither the archive nor the rule that removed it.
    auto c = interface_closure(library_graph(), "mathkit", "mathkit");
    ASSERT_TRUE(c.has_value());
    auto drop = published_object_names(*c);
    std::ranges::sort(drop);
    EXPECT_EQ(drop, (std::vector<std::string>{"api.m.o", "mathkit.m.o"}));
    EXPECT_EQ(std::ranges::find(drop, "secret.m.o"), drop.end());
}

// ─── the loud half of the asymmetry ────────────────────────────────────────

TEST(InterfaceClosure, AnInterfaceThatReachesAnUnprovidedPartitionIsAnError) {
    // If the root interface imports an implementation partition, the consumer
    // needs that source to build the BMI at all. The scanner cannot see the
    // partition's provider, so the closure would silently under-ship and the
    // consumer would fail. Stop at pack time instead, where the author is.
    auto c = interface_closure(library_graph(/*interfaceReachesSecret=*/true),
                               "mathkit", "mathkit");
    ASSERT_TRUE(c.has_value());
    ASSERT_EQ(c->unresolvedImports.size(), 1u);
    EXPECT_EQ(c->unresolvedImports[0], "mathkit:secret");
}

TEST(InterfaceClosure, ADependencysModuleIsNeitherPublishedNorUnresolved) {
    auto g = library_graph();
    g.units[0].requires_.push_back(ModuleId{"compat.zlib"});   // someone else's
    auto c = interface_closure(g, "mathkit", "mathkit");
    ASSERT_TRUE(c.has_value());
    EXPECT_TRUE(c->unresolvedImports.empty());
    EXPECT_EQ(c->published.size(), 2u);
}

TEST(InterfaceClosure, ForeignPackageUnitsAreNotFollowed) {
    auto g = library_graph();
    SourceUnit other;
    other.path        = "vendor/zlib.cppm";
    other.packageName = "compat.zlib";
    other.provides    = ModuleId{"compat.zlib"};
    g.producerOf.emplace("compat.zlib", g.units.size());
    g.units.push_back(std::move(other));
    g.units[0].requires_.push_back(ModuleId{"compat.zlib"});

    auto c = interface_closure(g, "mathkit", "mathkit");
    ASSERT_TRUE(c.has_value());
    EXPECT_EQ(names(c->published), (std::vector<std::string>{"api.cppm", "mathkit.cppm"}));
    // Nor does a foreign unit show up as something we withheld.
    auto withheld = names(c->withheld);
    EXPECT_EQ(std::ranges::find(withheld, "zlib.cppm"), withheld.end());
}

TEST(InterfaceClosure, RefusesAnUnknownRoot) {
    auto c = interface_closure(library_graph(), "mathkit", "nosuch");
    ASSERT_FALSE(c.has_value());
    EXPECT_NE(c.error().find("nosuch"), std::string::npos);
}

TEST(InterfaceClosure, RefusesARootOwnedByAnotherPackage) {
    auto g = library_graph();
    g.units[0].packageName = "somebody.else";
    auto c = interface_closure(g, "mathkit", "mathkit");
    ASSERT_FALSE(c.has_value());
    EXPECT_NE(c.error().find("somebody.else"), std::string::npos);
}
