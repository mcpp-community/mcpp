#include <gtest/gtest.h>

import std;
import mcpp.pm.dep_spec;
import mcpp.pm.dependency_selector;
import mcpp.pm.index_route;
import mcpp.pm.index_spec;

namespace {

struct LocalIndex {
    std::filesystem::path root;

    explicit LocalIndex(std::string_view name) {
        root = std::filesystem::temp_directory_path()
             / std::format("mcpp-index-route-{}", name);
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root / "pkgs" / "a");
        std::ofstream(root / "pkgs" / "a" / "acme.util.lua") << R"(
package = {
    spec = "1",
    namespace = "acme",
    name = "util",
    type = "package",
}
)";
    }
    ~LocalIndex() { std::error_code ec; std::filesystem::remove_all(root, ec); }
};

mcpp::pm::IndexMap local_map(const LocalIndex& idx) {
    mcpp::pm::IndexMap indices;
    indices["acme"] = mcpp::pm::IndexSpec{ .name = "acme", .path = idx.root };
    return indices;
}

} // namespace

TEST(PmIndexRoute, FindForNsResolvesNestedNamespaceToItsRootIndex) {
    mcpp::pm::IndexMap indices;
    indices["acme"] = mcpp::pm::IndexSpec{ .name = "acme", .url = "https://x/y" };
    mcpp::pm::IndexRoute route{ &indices, "/nowhere", nullptr };

    EXPECT_NE(route.find_for_ns("acme"), nullptr);
    EXPECT_NE(route.find_for_ns("acme.nested"), nullptr);
    EXPECT_EQ(route.find_for_ns("other"), nullptr);
}

TEST(PmIndexRoute, LazyGitIndexCannotRefuteAMiss) {
    mcpp::pm::IndexMap indices;
    indices["acme"] = mcpp::pm::IndexSpec{ .name = "acme", .url = "https://x/y" };
    mcpp::pm::IndexRoute route{ &indices, "/nowhere", nullptr };

    EXPECT_TRUE(route.lazy_git("acme"));
    EXPECT_FALSE(route.authoritative_for("acme"));
}

TEST(PmIndexRoute, LocalPathIndexIsAuthoritative) {
    LocalIndex idx("authoritative");
    auto indices = local_map(idx);
    mcpp::pm::IndexRoute route{ &indices, "/nowhere", nullptr };

    EXPECT_FALSE(route.lazy_git("acme"));
    EXPECT_TRUE(route.authoritative_for("acme"));
}

// The regression behind #305/#307: a dotted selector is a NAMESPACE PATH, so it
// only resolves through the candidates the manifest parser derives. Probing the
// literal short name can never match — `package.name` is a single atomic
// segment, so nothing in any index is named "acme.util".
TEST(PmIndexRoute, DottedSelectorResolvesThroughItsCandidates) {
    LocalIndex idx("dotted");
    auto indices = local_map(idx);
    mcpp::pm::IndexRoute route{ &indices, "/nowhere", nullptr };

    auto selector = mcpp::pm::resolve_dependency_selector(
        "acme.util", mcpp::pm::DependencySelectorMode::OmittedMcpplibsPriority);
    auto found = mcpp::pm::lookup_descriptor(route, selector.candidates);

    ASSERT_TRUE(found.hit.has_value());
    EXPECT_EQ(found.hit->coord.namespace_, "acme");
    EXPECT_EQ(found.hit->coord.shortName, "util");
}

TEST(PmIndexRoute, LiteralDottedShortNameNeverMatches) {
    LocalIndex idx("literal");
    auto indices = local_map(idx);
    mcpp::pm::IndexRoute route{ &indices, "/nowhere", nullptr };

    std::vector<mcpp::pm::DependencyCoordinate> literal{
        { .namespace_ = "mcpplibs", .shortName = "acme.util" },
    };
    EXPECT_FALSE(mcpp::pm::lookup_descriptor(route, literal).hit.has_value());
}

TEST(PmIndexRoute, MissInAReadableIndexIsConclusive) {
    LocalIndex idx("conclusive");
    auto indices = local_map(idx);
    mcpp::pm::IndexRoute route{ &indices, "/nowhere", nullptr };

    std::vector<mcpp::pm::DependencyCoordinate> missing{
        { .namespace_ = "acme", .shortName = "nope" },
    };
    auto found = mcpp::pm::lookup_descriptor(route, missing);
    EXPECT_FALSE(found.hit.has_value());
    EXPECT_TRUE(found.conclusive);
}

TEST(PmIndexRoute, MissBehindAnUnreadableIndexIsNotConclusive) {
    mcpp::pm::IndexMap indices;
    indices["acme"] = mcpp::pm::IndexSpec{ .name = "acme", .url = "https://x/y" };
    mcpp::pm::IndexRoute route{ &indices, "/nowhere", nullptr };

    std::vector<mcpp::pm::DependencyCoordinate> missing{
        { .namespace_ = "acme", .shortName = "nope" },
    };
    auto found = mcpp::pm::lookup_descriptor(route, missing);
    EXPECT_FALSE(found.hit.has_value());
    EXPECT_FALSE(found.conclusive);
}
