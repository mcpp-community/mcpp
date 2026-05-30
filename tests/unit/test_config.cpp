#include <gtest/gtest.h>

import std;
import mcpp.config;
import mcpp.pm.index_spec;

namespace {

std::filesystem::path make_tempdir(std::string_view name) {
    auto base = std::filesystem::temp_directory_path()
              / std::format("{}-{}", name, std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(base);
    return base;
}

}  // namespace

TEST(Config, ProjectXlingsDataRootsIncludeLegacyAndNestedLayouts) {
    auto project = std::filesystem::path{"/tmp/mcpp-project"};
    auto roots = mcpp::config::project_xlings_data_roots(project);

    ASSERT_EQ(roots.size(), 2u);
    EXPECT_EQ(roots[0], project / ".mcpp" / "data");
    EXPECT_EQ(roots[1], project / ".mcpp" / ".xlings" / "data");
}

TEST(Config, ProjectIndexDataInitializedChecksNestedXlingsData) {
    auto project = make_tempdir("mcpp-config-index-state");
    auto nestedIndexDir = project / ".mcpp" / ".xlings" / "data" / "xim-index-repos";
    std::filesystem::create_directories(nestedIndexDir);
    {
        std::ofstream os(nestedIndexDir / "xim-indexrepos.json");
        os << "{}";
    }

    EXPECT_TRUE(mcpp::config::project_index_data_initialized(project));

    std::filesystem::remove_all(project);
}

TEST(Config, ResolveProjectIndexPathUsesProjectRootForRelativeLocalIndex) {
    auto project = std::filesystem::path{"/tmp/mcpp-project"};
    mcpp::pm::IndexSpec spec;
    spec.name = "xlings";
    spec.path = "mcpp";

    EXPECT_EQ(mcpp::config::resolve_project_index_path(project, spec),
              (project / "mcpp").lexically_normal());
}

TEST(Config, ProjectIndexJsonEscapesLocalIndexPath) {
    auto project = make_tempdir("mcpp-config-json-escape");
    auto index = project / "local" / "index";
    std::filesystem::create_directories(index / "pkgs");

    mcpp::pm::IndexSpec local;
    local.name = "local\"dev";
    local.path = index;

    mcpp::pm::IndexSpec remote;
    remote.name = "remote";
    remote.url = R"(https://example.com/a\b"c)";

    std::map<std::string, mcpp::pm::IndexSpec> indices;
    indices.emplace(local.name, local);
    indices.emplace(remote.name, remote);

    mcpp::config::GlobalConfig cfg;
    ASSERT_TRUE(mcpp::config::ensure_project_index_dir(cfg, project, indices));

    std::ifstream is(project / ".mcpp" / ".xlings.json");
    ASSERT_TRUE(is);
    std::stringstream ss;
    ss << is.rdbuf();
    auto content = ss.str();

    EXPECT_NE(content.find(R"("name": "local\"dev")"), std::string::npos)
        << content;
    EXPECT_NE(content.find(index.generic_string()), std::string::npos)
        << content;
    EXPECT_NE(content.find(R"(https://example.com/a\\b\"c)"), std::string::npos)
        << content;

    is.close();
    std::filesystem::remove_all(project);
}
