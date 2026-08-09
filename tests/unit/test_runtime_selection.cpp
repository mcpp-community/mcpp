#include <gtest/gtest.h>

import std;
import mcpp.config;
import mcpp.manifest;
import mcpp.platform;
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
            "host_glibc": "2.43",
            "envs": {{
              "mesa@25": [{{"var":"DRIVERS","op":"prepend","value":"{}"}}]
            }},
            "runtime_contract": {{
              "providers": [{{
                "capability": "render.demo",
                "provider": {{"namespace":"xim","name":"renderer",
                  "version":"4.0.0","source":"xim-pkgindex@rev"}}
              }}],
              "artifacts": [{{
                "role":"driver",
                "provider": {{"namespace":"xim","name":"renderer",
                  "version":"4.0.0","source":"xim-pkgindex@rev"}},
                "path":"${{subosdir}}/runtime/renderer.bin",
                "provenance":"subos_view","abi":"fixture-v1",
                "digest":"sha256:def","host_fingerprint":"host-2"
              }}]
            }}
          }},
          "workspace": {{}}
        }})", runtimeName, envValue);
    }

    void point_default_glibc_view_at(std::string_view version) const {
        auto payload = install_glibc_payload(version);
        auto view = cfg.xlingsHome() / "subos" / "default" / "lib";
        std::filesystem::create_directories(view);
        std::filesystem::create_symlink(
            payload / "libc.so.6", view / "libc.so.6");
        std::filesystem::create_symlink(
            payload / "ld-linux-x86-64.so.2",
            view / "ld-linux-x86-64.so.2");
    }

    std::filesystem::path install_glibc_payload(
        std::string_view version) const {
        auto payload = cfg.xlingsHome() / "data" / "xpkgs"
                     / "xim-x-glibc" / version / "lib";
        std::filesystem::create_directories(payload);
        std::ofstream(payload / "libc.so.6") << "fixture libc";
        std::ofstream(payload / "ld-linux-x86-64.so.2") << "fixture loader";
        return payload;
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
    if constexpr (mcpp::platform::is_linux)
        EXPECT_EQ(binding->libc, std::optional<std::string>("glibc@2.39"));
    else
        EXPECT_FALSE(binding->libc.has_value());
    EXPECT_EQ(binding->provenance, "mcpp_default");
    EXPECT_FALSE(binding->contractHash.empty());
    ASSERT_EQ(binding->runtimeProviders.size(), 1u);
    EXPECT_EQ(binding->runtimeProviders[0].provider.name, "renderer");
    ASSERT_EQ(binding->runtimeArtifacts.size(), 1u);
    EXPECT_EQ(binding->runtimeArtifacts[0].path,
              (expected / "runtime/renderer.bin").lexically_normal());
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

TEST(RuntimeBinding, PhysicalSubosViewReconcilesAStaleDeclaredGlibcIdentity) {
    if constexpr (!mcpp::platform::is_linux)
        GTEST_SKIP() << "glibc SubOS views only exist on Linux";

    RuntimeHome h;
    const auto subos = h.cfg.xlingsHome() / "subos" / "default";
    h.write(subos, "glibc@2.39");
    h.point_default_glibc_view_at("2.44");

    mcpp::manifest::Manifest root;
    auto selection = runtime::select_runtime(root, std::nullopt, h.dir / "repo");
    ASSERT_TRUE(selection);
    auto binding = mcpp::platform::runtime::resolve_runtime_binding(
        *selection, {}, h.cfg);
    ASSERT_TRUE(binding.has_value()) << binding.error();

    EXPECT_EQ(binding->runtimeId, "glibc@2.44");
    EXPECT_EQ(binding->libc, std::optional<std::string>("glibc@2.44"));
    ASSERT_EQ(binding->libraryDirs.size(), 1u);
    EXPECT_EQ(binding->libraryDirs.front(),
              h.cfg.xlingsHome() / "data" / "xpkgs"
                  / "xim-x-glibc" / "2.44" / "lib");
}

TEST(RuntimeBinding, BrokenSubosViewFallsBackOnlyToTheExactDeclaredPayload) {
    if constexpr (!mcpp::platform::is_linux)
        GTEST_SKIP() << "glibc SubOS views only exist on Linux";

    RuntimeHome h;
    const auto subos = h.cfg.xlingsHome() / "subos" / "default";
    h.write(subos, "glibc@2.44");
    auto payload = h.install_glibc_payload("2.44");
    auto view = subos / "lib";
    std::filesystem::create_directories(view);
    std::filesystem::create_symlink(
        payload.parent_path() / "lib64" / "libc.so.6",
        view / "libc.so.6");

    mcpp::manifest::Manifest root;
    auto selection = runtime::select_runtime(root, std::nullopt, h.dir / "repo");
    ASSERT_TRUE(selection);
    auto binding = mcpp::platform::runtime::resolve_runtime_binding(
        *selection, {}, h.cfg);
    ASSERT_TRUE(binding.has_value()) << binding.error();

    EXPECT_EQ(binding->runtimeId, "glibc@2.44");
    ASSERT_EQ(binding->libraryDirs.size(), 1u);
    EXPECT_EQ(binding->libraryDirs.front(), payload);
    EXPECT_EQ(binding->loader,
              std::optional<std::filesystem::path>(
                  payload / "ld-linux-x86-64.so.2"));
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
    if constexpr (mcpp::platform::is_linux) {
        ASSERT_TRUE(decoded->hostLibc.has_value());
        EXPECT_EQ(*decoded->hostLibc, "2.43");
    } else {
        EXPECT_FALSE(decoded->hostLibc.has_value());
    }
    EXPECT_EQ(decoded->subosDir, el8->subosDir);
    ASSERT_EQ(decoded->environment.size(), 1u);
    EXPECT_EQ(decoded->environment[0].var, "DRIVERS");
    ASSERT_EQ(decoded->runtimeProviders.size(), 1u);
    EXPECT_EQ(decoded->runtimeProviders[0].provider.namespace_, "xim");
    ASSERT_EQ(decoded->runtimeArtifacts.size(), 1u);
    EXPECT_EQ(decoded->runtimeArtifacts[0].hostFingerprint, "host-2");
}

} // namespace
