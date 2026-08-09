#include <gtest/gtest.h>

import std;
import mcpp.config;
import mcpp.manifest;
import mcpp.platform.runtime_binding;
import mcpp.xlings.runtime_selection;

namespace runtime = mcpp::xlings::runtime;

namespace {

mcpp::manifest::Manifest named(std::string name) {
    mcpp::manifest::Manifest m;
    m.xlings.subosDeclared = true;
    m.xlings.subos = std::move(name);
    return m;
}

struct RuntimeHome {
    std::filesystem::path dir;
    mcpp::config::GlobalConfig cfg;

    RuntimeHome() {
        dir = std::filesystem::temp_directory_path()
            / std::format("mcpp_runtime_selection_{}", std::random_device{}());
        cfg.registryDir = dir / "xlings-home";
        std::filesystem::create_directories(cfg.registryDir);
    }

    ~RuntimeHome() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    void write(const std::filesystem::path& subos,
               std::string_view runtimeName = "glibc@2.39",
               std::string_view envValue = "${subosdir}/drivers") const {
        std::filesystem::create_directories(subos);
        std::ofstream(subos / ".xlings.json") << std::format(R"({{
          "subos_info": {{
            "schema_version": 1,
            "runtime": "{}",
            "envs": {{
              "mesa@25": [{{"var":"DRIVERS","op":"prepend","value":"{}"}}]
            }}
          }},
          "workspace": {{}}
        }})", runtimeName, envValue);
    }
};

TEST(RuntimeSelection, AbsenceMeansMcppDefault) {
    mcpp::manifest::Manifest root;
    auto selected = runtime::select_runtime(root, std::nullopt, "/repo");
    ASSERT_TRUE(selected.has_value()) << selected.error();
    EXPECT_EQ(selected->mode, runtime::RuntimeSelection::Mode::McppDefault);
    EXPECT_EQ(selected->source, runtime::RuntimeSelection::Source::DefaultPolicy);
    EXPECT_EQ(selected->subosName, "default");
    EXPECT_EQ(selected->ownerRoot, std::filesystem::path("/repo"));
}

TEST(RuntimeSelection, ExplicitDefaultRemainsNamed) {
    auto root = named("default");
    auto selected = runtime::select_runtime(root, std::nullopt, "/repo");
    ASSERT_TRUE(selected.has_value()) << selected.error();
    EXPECT_EQ(selected->mode, runtime::RuntimeSelection::Mode::NamedSubos);
    EXPECT_EQ(selected->source, runtime::RuntimeSelection::Source::Manifest);
    EXPECT_EQ(selected->subosName, "default");
}

TEST(RuntimeSelection, WorkspaceRootOverridesMember) {
    auto member = named("member-dev");
    auto workspace = named("workspace-el8");
    auto selected = runtime::select_runtime(
        member, std::cref(workspace), "/repo/member", "/repo");
    ASSERT_TRUE(selected.has_value()) << selected.error();
    EXPECT_EQ(selected->subosName, "workspace-el8");
    EXPECT_EQ(selected->ownerRoot, std::filesystem::path("/repo"));
}

TEST(RuntimeSelection, MemberAppliesWhenItIsTheIndependentRoot) {
    auto member = named("member-dev");
    auto selected = runtime::select_runtime(member, std::nullopt, "/repo/member");
    ASSERT_TRUE(selected.has_value()) << selected.error();
    EXPECT_EQ(selected->subosName, "member-dev");
    EXPECT_EQ(selected->ownerRoot, std::filesystem::path("/repo/member"));
}

TEST(RuntimeSelection, EmptyAndPathLikeNamesAreRejected) {
    for (auto bad : {"", ".", "..", "a/b", "a\\b", "a:b"}) {
        auto root = named(bad);
        auto selected = runtime::select_runtime(root, std::nullopt, "/repo");
        EXPECT_FALSE(selected.has_value()) << bad;
    }
}

TEST(RuntimeSelection, DependencyDeclarationCannotAffectConsumerSelection) {
    mcpp::manifest::Manifest consumer;
    auto dependency = named("dependency-only");
    (void)dependency;
    auto selected = runtime::select_runtime(consumer, std::nullopt, "/consumer");
    ASSERT_TRUE(selected.has_value()) << selected.error();
    EXPECT_EQ(selected->mode, runtime::RuntimeSelection::Mode::McppDefault);
    EXPECT_EQ(selected->subosName, "default");
}

TEST(RuntimeBinding, DefaultAlwaysUsesConfiguredMcppHomeDefault) {
    RuntimeHome h;
    const auto expected = h.cfg.xlingsHome() / "subos" / "default";
    h.write(expected);

    mcpp::manifest::Manifest root;
    auto selection = runtime::select_runtime(root, std::nullopt, h.dir / "repo");
    ASSERT_TRUE(selection);
    auto binding = mcpp::platform::runtime::resolve_runtime_binding(
        *selection, {}, h.cfg);
    ASSERT_TRUE(binding.has_value()) << binding.error();
    EXPECT_EQ(binding->subosDir, expected);
    EXPECT_EQ(binding->runtimeId, "glibc@2.39");
    EXPECT_EQ(binding->libc, std::optional<std::string>("glibc@2.39"));
    EXPECT_EQ(binding->provenance, "mcpp_default");
    EXPECT_FALSE(binding->contractHash.empty());
}

TEST(RuntimeBinding, ExplicitDefaultUsesGlobalDefaultButKeepsNamedIdentity) {
    RuntimeHome h;
    const auto expected = h.cfg.xlingsHome() / "subos" / "default";
    h.write(expected);
    auto manifest = named("default");
    auto selection = runtime::select_runtime(manifest, std::nullopt, h.dir / "repo");
    ASSERT_TRUE(selection);
    auto binding = mcpp::platform::runtime::resolve_runtime_binding(
        *selection, {}, h.cfg);
    ASSERT_TRUE(binding.has_value()) << binding.error();
    EXPECT_EQ(binding->subosDir, expected);
    EXPECT_EQ(binding->provenance, "named_subos");
    EXPECT_EQ(binding->selection.mode, runtime::RuntimeSelection::Mode::NamedSubos);
}

TEST(RuntimeBinding, NamedSubosIsOwnedByTheRootAndMissingIsHardError) {
    RuntimeHome h;
    auto manifest = named("el8");
    auto selection = runtime::select_runtime(manifest, std::nullopt, h.dir / "repo");
    ASSERT_TRUE(selection);

    auto missing = mcpp::platform::runtime::resolve_runtime_binding(
        *selection, {}, h.cfg);
    ASSERT_FALSE(missing.has_value());
    EXPECT_NE(missing.error().find("el8"), std::string::npos);

    const auto expected = h.dir / "repo" / ".mcpp" / ".xlings" / "subos" / "el8";
    h.write(expected);
    auto binding = mcpp::platform::runtime::resolve_runtime_binding(
        *selection, {}, h.cfg);
    ASSERT_TRUE(binding.has_value()) << binding.error();
    EXPECT_EQ(binding->subosDir, expected);
}

TEST(RuntimeBinding, NamedEnvironmentsHaveDistinctContractsAndRoundTrip) {
    RuntimeHome h;
    auto el8Manifest = named("el8");
    auto devManifest = named("dev");
    auto el8Selection = runtime::select_runtime(
        el8Manifest, std::nullopt, h.dir / "repo");
    auto devSelection = runtime::select_runtime(
        devManifest, std::nullopt, h.dir / "repo");
    ASSERT_TRUE(el8Selection);
    ASSERT_TRUE(devSelection);
    h.write(h.dir / "repo" / ".mcpp" / ".xlings" / "subos" / "el8");
    h.write(h.dir / "repo" / ".mcpp" / ".xlings" / "subos" / "dev");

    auto el8 = mcpp::platform::runtime::resolve_runtime_binding(
        *el8Selection, {}, h.cfg);
    auto dev = mcpp::platform::runtime::resolve_runtime_binding(
        *devSelection, {}, h.cfg);
    ASSERT_TRUE(el8);
    ASSERT_TRUE(dev);
    EXPECT_NE(el8->contractHash, dev->contractHash);

    auto encoded = mcpp::platform::runtime::serialize_runtime_binding(*el8);
    auto decoded = mcpp::platform::runtime::deserialize_runtime_binding(encoded);
    ASSERT_TRUE(decoded.has_value()) << decoded.error();
    EXPECT_EQ(decoded->contractHash, el8->contractHash);
    EXPECT_EQ(decoded->subosDir, el8->subosDir);
    ASSERT_EQ(decoded->environment.size(), 1u);
    EXPECT_EQ(decoded->environment[0].var, "DRIVERS");
}

} // namespace
