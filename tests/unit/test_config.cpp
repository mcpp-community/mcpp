#include <fstream>
#include <sstream>
#include <string>

import std;
import mcpplibs.mtest;
import mcpp.config;
import mcpp.fallback.config_migration;
import mcpp.pm.index_spec;

using namespace mcpplibs::mtest;

namespace {

std::filesystem::path make_tempdir(std::string_view name) {
    auto base = std::filesystem::temp_directory_path()
              / std::format("{}-{}", name, std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(base);
    return base;
}

}  // namespace

UTest project_xlings_data_roots_include_legacy_and_nested_layouts{"Config/ProjectXlingsDataRootsIncludeLegacyAndNestedLayouts", [] {
    auto project = std::filesystem::path{"/tmp/mcpp-project"};
    auto roots = mcpp::config::project_xlings_data_roots(project);

    expect(fatal(eq(roots.size(), 2u)));
    expect(eq(roots[0], project / ".mcpp" / "data"));
    expect(eq(roots[1], project / ".mcpp" / ".xlings" / "data"));
}};

UTest project_index_data_initialized_checks_nested_xlings_data{"Config/ProjectIndexDataInitializedChecksNestedXlingsData", [] {
    auto project = make_tempdir("mcpp-config-index-state");
    auto nestedIndexDir = project / ".mcpp" / ".xlings" / "data" / "xim-index-repos";
    std::filesystem::create_directories(nestedIndexDir);
    {
        std::ofstream os(nestedIndexDir / "xim-indexrepos.json");
        os << "{}";
    }

    expect(bool(mcpp::config::project_index_data_initialized(project)));

    std::filesystem::remove_all(project);
}};

UTest resolve_project_index_path_uses_project_root_for_relative_local_index{"Config/ResolveProjectIndexPathUsesProjectRootForRelativeLocalIndex", [] {
    auto project = std::filesystem::path{"/tmp/mcpp-project"};
    mcpp::pm::IndexSpec spec;
    spec.name = "xlings";
    spec.path = "mcpp";

    expect(eq(mcpp::config::resolve_project_index_path(project, spec), (project / "mcpp").lexically_normal()));
}};

UTest project_index_json_escapes_local_index_path{"Config/ProjectIndexJsonEscapesLocalIndexPath", [] {
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
    expect(fatal(bool(mcpp::config::ensure_project_index_dir(cfg, project, indices))));

    std::ifstream is(project / ".mcpp" / ".xlings.json");
    expect(fatal(bool(is)));
    std::stringstream ss;
    ss << is.rdbuf();
    auto content = ss.str();

    expect(ne(content.find(R"("name": "local\"dev")"), std::string::npos))
        << content;
    expect(ne(content.find(index.generic_string()), std::string::npos))
        << content;
    expect(ne(content.find(R"(https://example.com/a\\b\"c)"), std::string::npos))
        << content;

    is.close();
    std::filesystem::remove_all(project);
}};

// The official global `xim` index must NOT be injected/exposed into the project
// scope. `xim` (and its dynamic sub-indexes) are xlings GLOBAL-default indices —
// global IS the default scope; injecting xim as a project repo made every `xim:*`
// tool (cmake/glibc/gcc/make) resolve project-scoped and install into the project
// store instead of the shared registry, breaking ELF loader resolution for
// build-dep tools. `xim:*` resolves at global scope via the registry-local clone.
// See .agents/docs/2026-07-09-project-index-scope-global-infra-fix.md.
UTest project_index_dir_does_not_inject_official_xim_index{"Config/ProjectIndexDirDoesNotInjectOfficialXimIndex", [] {
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
    expect(fatal(bool(mcpp::config::ensure_project_index_dir(cfg, project, indices))));

    // xim is NOT copied/exposed into the project data dir (it stays global).
    auto projectOfficial =
        project / ".mcpp" / ".xlings" / "data" / "xim-pkgindex";
    expect(!std::filesystem::exists(projectOfficial / "pkgs" / "p" / "python.lua"));

    // The user-declared local `compat` index IS exposed project-locally.
    auto projectLocal = project / ".mcpp" / ".xlings" / "data" / "compat";
    expect(std::filesystem::exists(projectLocal / "pkgs"));

    // The seeded project .xlings.json index_repos must not contain `xim`.
    auto projJson = project / ".mcpp" / ".xlings.json";
    if (std::filesystem::exists(projJson)) {
        std::ifstream is(projJson);
        std::stringstream ss; ss << is.rdbuf();
        expect(eq(ss.str().find("\"xim\""), std::string::npos));
    }

    std::filesystem::remove_all(project);
    std::filesystem::remove_all(registry);
}};

UTest project_local_index_stale_cache_is_removed{"Config/ProjectLocalIndexStaleCacheIsRemoved", [] {
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
    expect(fatal(bool(mcpp::config::ensure_project_index_dir(cfg, project, indices))));
    expect(!std::filesystem::exists(localIndex / ".xlings-index-cache.json"));

    std::filesystem::remove_all(project);
}};

// ── #267-A/#269: index org migration + artifact adoption ────────────────────

namespace {

std::string read_all(const std::filesystem::path& p) {
    std::stringstream ss;
    ss << std::ifstream(p).rdbuf();
    return ss.str();
}

}  // namespace

UTest canonicalize_rewrites_org_and_name{"ConfigIndexMigration/CanonicalizeRewritesOrgAndName", [] {
    mcpp::config::GlobalConfig cfg;
    cfg.defaultIndex = "mcpp-index";
    cfg.indexRepos.push_back({ "mcpp-index",
        std::string(mcpp::config::kMcpplibsIndexUrlLegacy) });
    mcpp::config::canonicalize_legacy_index_names(cfg);
    expect(fatal(eq(cfg.indexRepos.size(), 1u)));
    expect(eq(cfg.defaultIndex, "mcpplibs"));
    expect(eq(cfg.indexRepos[0].name, "mcpplibs"));
    expect(eq(cfg.indexRepos[0].url, mcpp::config::kMcpplibsIndexUrl));
}};

UTest canonicalize_folds_legacy_and_default_entries{"ConfigIndexMigration/CanonicalizeFoldsLegacyAndDefaultEntries", [] {
    mcpp::config::GlobalConfig cfg;
    cfg.indexRepos.push_back({ "mcpplibs",
        std::string(mcpp::config::kMcpplibsIndexUrlLegacy) });
    cfg.indexRepos.push_back({ "mcpplibs",
        std::string(mcpp::config::kMcpplibsIndexUrl) });
    mcpp::config::canonicalize_legacy_index_names(cfg);
    expect(eq(cfg.indexRepos.size(), 1u));
}};

UTest canonicalize_leaves_foreign_repos_alone{"ConfigIndexMigration/CanonicalizeLeavesForeignReposAlone", [] {
    mcpp::config::GlobalConfig cfg;
    cfg.indexRepos.push_back({ "other", "https://example.com/other-index.git" });
    mcpp::config::canonicalize_legacy_index_names(cfg);
    expect(fatal(eq(cfg.indexRepos.size(), 1u)));
    expect(eq(cfg.indexRepos[0].name, "other"));
    expect(eq(cfg.indexRepos[0].url, "https://example.com/other-index.git"));
}};

UTest migrate_config_toml_rewrites_org_url_idempotently{"ConfigIndexMigration/MigrateConfigTomlRewritesOrgUrlIdempotently", [] {
    auto dir = make_tempdir("mcpp-migrate-toml");
    auto p = dir / "config.toml";
    {
        std::ofstream os(p);
        os << "[index]\ndefault = \"mcpplibs\"\n\n[index.repos.\"mcpplibs\"]\n"
              "url = \"https://github.com/mcpp-community/mcpp-index.git\"\n";
    }
    expect(bool(mcpp::fallback::migrate_config_toml_index_names(p)));
    auto text = read_all(p);
    expect(ne(text.find("github.com/mcpplibs/mcpp-index.git"), std::string::npos));
    expect(eq(text.find("mcpp-community/mcpp-index"), std::string::npos));
    expect(!mcpp::fallback::migrate_config_toml_index_names(p));  // idempotent
    expect(eq(read_all(p), text));
    std::filesystem::remove_all(dir);
}};

UTest canonicalize_fills_default_artifact_for_official_repo{"ConfigIndexMigration/CanonicalizeFillsDefaultArtifactForOfficialRepo", [] {
    mcpp::config::GlobalConfig cfg;
    cfg.indexRepos.push_back({ "mcpplibs",
        std::string(mcpp::config::kMcpplibsIndexUrlLegacy) });
    mcpp::config::canonicalize_legacy_index_names(cfg);
    expect(fatal(eq(cfg.indexRepos.size(), 1u)));
    expect(eq(cfg.indexRepos[0].artifact, mcpp::config::kMcpplibsIndexArtifact));
}};

UTest canonicalize_respects_explicit_git_source_and_custom_artifact{"ConfigIndexMigration/CanonicalizeRespectsExplicitGitSourceAndCustomArtifact", [] {
    mcpp::config::GlobalConfig cfg;
    mcpp::config::IndexRepo git{ "mcpplibs",
        std::string(mcpp::config::kMcpplibsIndexUrl) };
    git.source = "git";
    cfg.indexRepos.push_back(git);
    mcpp::config::IndexRepo custom{ "mirror",
        std::string(mcpp::config::kMcpplibsIndexUrl) };
    custom.artifact = "https://example.com/my-mirror";
    cfg.indexRepos.push_back(custom);
    cfg.indexRepos.push_back({ "other", "https://example.com/other-index.git" });
    mcpp::config::canonicalize_legacy_index_names(cfg);
    expect(fatal(eq(cfg.indexRepos.size(), 3u)));
    expect(cfg.indexRepos[0].artifact.empty());        // explicit git opt-out
    expect(eq(cfg.indexRepos[1].artifact, "https://example.com/my-mirror"));
    expect(cfg.indexRepos[2].artifact.empty());        // foreign repo untouched
}};

UTest xlings_json_heals_pretty_legacy_entry{"ConfigIndexMigration/XlingsJsonHealsPrettyLegacyEntry", [] {
    auto dir = make_tempdir("mcpp-migrate-xj");
    auto p = dir / ".xlings.json";
    {
        std::ofstream os(p);
        os << "{\n  \"index_repos\": [\n"
              "    { \"name\": \"mcpplibs\", \"url\": "
              "\"https://github.com/mcpp-community/mcpp-index.git\" }\n"
              "  ],\n  \"subos\": \"default\",\n  \"mirror\": \"auto\"\n}\n";
    }
    expect(bool(mcpp::fallback::migrate_xlings_json_index_names(p)));
    auto text = read_all(p);
    expect(ne(text.find(
        "\"url\": \"https://github.com/mcpplibs/mcpp-index.git\", "
        "\"artifact\": { \"GLOBAL\": \"https://github.com/xlings-res/mcpp-index\", "
        "\"CN\": \"https://gitcode.com/xlings-res/mcpp-index\" }"), std::string::npos)) << text;
    expect(ne(text.find("\"subos\": \"default\""), std::string::npos));
    expect(eq(text.find("mcpp-community/mcpp-index"), std::string::npos));
    expect(!mcpp::fallback::migrate_xlings_json_index_names(p));  // idempotent
    expect(eq(read_all(p), text));
    std::filesystem::remove_all(dir);
}};

UTest xlings_json_heals_compact_legacy_name_and_url{"ConfigIndexMigration/XlingsJsonHealsCompactLegacyNameAndUrl", [] {
    auto dir = make_tempdir("mcpp-migrate-xjc");
    auto p = dir / ".xlings.json";
    {
        std::ofstream os(p);
        os << "{\"index_repos\":[{\"name\":\"mcpp-index\","
              "\"url\":\"https://github.com/mcpp-community/mcpp-index.git\"}],"
              "\"mirror\":\"auto\"}";
    }
    expect(bool(mcpp::fallback::migrate_xlings_json_index_names(p)));
    auto text = read_all(p);
    expect(ne(text.find("\"name\":\"mcpplibs\""), std::string::npos));
    expect(ne(text.find(
        "\"url\":\"https://github.com/mcpplibs/mcpp-index.git\", "
        "\"artifact\": { \"GLOBAL\": \"https://github.com/xlings-res/mcpp-index\", "
        "\"CN\": \"https://gitcode.com/xlings-res/mcpp-index\" }"), std::string::npos)) << text;
    expect(!mcpp::fallback::migrate_xlings_json_index_names(p));
    std::filesystem::remove_all(dir);
}};

// #648 A6. A home seeded with the GitHub-only default artifact gains the CN half
// of the region object once; the migration is idempotent afterwards.
UTest xlings_json_upgrades_the_flat_default_artifact_to_the_region_object{"ConfigIndexMigration/XlingsJsonUpgradesTheFlatDefaultArtifactToTheRegionObject", [] {
    auto dir = make_tempdir("mcpp-migrate-xja");
    auto p = dir / ".xlings.json";
    {
        std::ofstream os(p);
        os << "{\n  \"index_repos\": [\n"
              "    { \"name\": \"mcpplibs\", \"url\": "
              "\"https://github.com/mcpplibs/mcpp-index.git\", "
              "\"artifact\": \"https://github.com/xlings-res/mcpp-index\" }\n"
              "  ]\n}\n";
    }
    expect(bool(mcpp::fallback::migrate_xlings_json_index_names(p)));
    auto text = read_all(p);
    expect(ne(text.find("\"artifact\": { \"GLOBAL\": \"https://github.com/xlings-res/mcpp-index\", "
        "\"CN\": \"https://gitcode.com/xlings-res/mcpp-index\" }"), std::string::npos)) << text;
    expect(!mcpp::fallback::migrate_xlings_json_index_names(p));
    expect(eq(read_all(p), text));
    std::filesystem::remove_all(dir);
}};

// An artifact base the user wrote is theirs: no CN half is added to it.
UTest xlings_json_leaves_a_user_artifact_base_alone{"ConfigIndexMigration/XlingsJsonLeavesAUserArtifactBaseAlone", [] {
    auto dir = make_tempdir("mcpp-migrate-xju");
    auto p = dir / ".xlings.json";
    std::string body =
        "{\n  \"index_repos\": [\n"
        "    { \"name\": \"mcpplibs\", \"url\": "
        "\"https://github.com/mcpplibs/mcpp-index.git\", "
        "\"artifact\": \"https://mirror.example/xlings-res/mcpp-index\" }\n"
        "  ]\n}\n";
    {
        std::ofstream os(p);
        os << body;
    }
    expect(!mcpp::fallback::migrate_xlings_json_index_names(p));
    expect(eq(read_all(p), body));
    std::filesystem::remove_all(dir);
}};

// A value written while another reader reads the file: the reader sees the previous copy or the
// new one, whole, and the writer leaves no file of its own beside it. Programs started at once on
// a fresh home each write config.toml; a rewrite in place (truncate, then write) let one read a
// part of another's ("invalid config.toml: expected '='").
UTest config_value_is_replaced_whole{"Config/ConfigValueIsReplacedWhole", [] {
    const auto home = make_tempdir("mcpp-config-replace");
    mcpp::config::GlobalConfig cfg;
    cfg.configFile = home / "config.toml";
    const std::string padding(64 * 1024, '#');
    {
        std::ofstream(cfg.configFile) << "[xlings]\nbinary = \"bundled\"\n" << padding << "\n[toolchain]\ndefault = \"gcc@16.1.0\"\n";
    }
    std::atomic<int> partial{0};
    // Windows refuses a rename over a file a reader holds open, and the writer retries
    // (replace_file); a reader there pauses between reads, as the readers of a home do, where
    // elsewhere it reads without a pause. A failed write stops and joins the reader (jthread).
    const bool pauses = host() == platform::windows;
    std::jthread reader([&](std::stop_token stop) {
        while (!stop.stop_requested()) {
            {
                std::ifstream in(cfg.configFile);
                const std::string text{std::istreambuf_iterator<char>(in), {}};
                if (!text.contains("[toolchain]\ndefault = \"")) ++partial;
            }
            if (pauses) std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
    });
    for (int i = 0; i < 200; ++i)
        expect(fatal(mcpp::config::write_config_value(cfg, "toolchain", "default", i % 2 ? "\"llvm@23.1.3\"" : "\"gcc@16.1.0\"").has_value()));
    reader.request_stop();
    reader.join();
    expect(eq(partial.load(), 0)) << "a reader saw config.toml without its [toolchain] default";
    std::size_t files = 0;
    for ([[maybe_unused]] const auto& e : std::filesystem::directory_iterator(home)) ++files;
    expect(eq(files, std::size_t{1})) << "a writer left a file beside config.toml";
    std::filesystem::remove_all(home);
}};
