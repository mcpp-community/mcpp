#include <gtest/gtest.h>
#include <fstream>
#include <sstream>
#include <string>

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

// The official global `xim` index must NOT be injected/exposed into the project
// scope. `xim` (and its dynamic sub-indexes) are xlings GLOBAL-default indices —
// global IS the default scope; injecting xim as a project repo made every `xim:*`
// tool (cmake/glibc/gcc/make) resolve project-scoped and install into the project
// store instead of the shared registry, breaking ELF loader resolution for
// build-dep tools. `xim:*` resolves at global scope via the registry-local clone.
// See .agents/docs/2026-07-09-project-index-scope-global-infra-fix.md.
TEST(Config, ProjectIndexDirDoesNotInjectOfficialXimIndex) {
    auto project = make_tempdir("mcpp-config-project-xim-index");
    auto registry = make_tempdir("mcpp-config-registry");
    auto official = registry / "data" / "xim-pkgindex";
    std::filesystem::create_directories(official / "pkgs" / "p");
    {
        std::ofstream os(official / "pkgs" / "p" / "python.lua");
        os << "package = { name = \"python\" }\n";
    }

    auto localIndex = project / "compat";
    std::filesystem::create_directories(localIndex / "pkgs" / "c");

    mcpp::pm::IndexSpec local;
    local.name = "compat";
    local.path = localIndex;

    std::map<std::string, mcpp::pm::IndexSpec> indices;
    indices.emplace(local.name, local);

    mcpp::config::GlobalConfig cfg;
    cfg.registryDir = registry;
    ASSERT_TRUE(mcpp::config::ensure_project_index_dir(cfg, project, indices));

    // xim is NOT copied/exposed into the project data dir (it stays global).
    auto projectOfficial =
        project / ".mcpp" / ".xlings" / "data" / "xim-pkgindex";
    EXPECT_FALSE(std::filesystem::exists(projectOfficial / "pkgs" / "p" / "python.lua"));

    // The user-declared local `compat` index IS exposed project-locally.
    auto projectLocal = project / ".mcpp" / ".xlings" / "data" / "compat";
    EXPECT_TRUE(std::filesystem::exists(projectLocal / "pkgs"));

    // The seeded project .xlings.json index_repos must not contain `xim`.
    auto projJson = project / ".mcpp" / ".xlings.json";
    if (std::filesystem::exists(projJson)) {
        std::ifstream is(projJson);
        std::stringstream ss; ss << is.rdbuf();
        EXPECT_EQ(ss.str().find("\"xim\""), std::string::npos);
    }

    std::filesystem::remove_all(project);
    std::filesystem::remove_all(registry);
}

TEST(Config, ProjectLocalIndexStaleCacheIsRemoved) {
    auto project = make_tempdir("mcpp-config-local-index-cache");
    auto localIndex = project / "compat";
    std::filesystem::create_directories(localIndex / "pkgs" / "c");
    {
        std::ofstream os(localIndex / ".xlings-index-cache.json");
        os << R"({"entries":{"compat.lz4":{"path":"/tmp/deleted/pkgs/c/compat.lz4.lua"}}})";
    }

    mcpp::pm::IndexSpec local;
    local.name = "compat";
    local.path = localIndex;

    std::map<std::string, mcpp::pm::IndexSpec> indices;
    indices.emplace(local.name, local);

    mcpp::config::GlobalConfig cfg;
    ASSERT_TRUE(mcpp::config::ensure_project_index_dir(cfg, project, indices));
    EXPECT_FALSE(std::filesystem::exists(localIndex / ".xlings-index-cache.json"));

    std::filesystem::remove_all(project);
}
