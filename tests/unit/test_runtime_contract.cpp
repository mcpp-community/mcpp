#include <gtest/gtest.h>

import std;
import mcpp.build.plan;
import mcpp.manifest.toml;
import mcpp.manifest.types;
import mcpp.manifest.xpkg;
import mcpp.modgraph.scanner;
import mcpp.platform;
import mcpp.platform.axis;
import mcpp.platform.runtime_binding;
import mcpp.xlings.subos_info;

namespace build = mcpp::build;
namespace mf = mcpp::manifest;

namespace {

std::string source_without_comments(std::string_view source) {
    enum class State { Code, Line, Block, String, Character } state = State::Code;
    std::string out;
    out.reserve(source.size());
    for (std::size_t i = 0; i < source.size(); ++i) {
        const char c = source[i];
        const char n = i + 1 < source.size() ? source[i + 1] : '\0';
        if (state == State::Code) {
            if (c == '/' && n == '/') { state = State::Line; ++i; out += "  "; }
            else if (c == '/' && n == '*') { state = State::Block; ++i; out += "  "; }
            else {
                out.push_back(c);
                if (c == '"') state = State::String;
                else if (c == '\'') state = State::Character;
            }
        } else if (state == State::Line) {
            if (c == '\n') { state = State::Code; out.push_back(c); }
            else out.push_back(' ');
        } else if (state == State::Block) {
            if (c == '*' && n == '/') { state = State::Code; ++i; out += "  "; }
            else out.push_back(c == '\n' ? '\n' : ' ');
        } else {
            out.push_back(c);
            if (c == '\\' && i + 1 < source.size()) out.push_back(source[++i]);
            else if ((state == State::String && c == '"')
                  || (state == State::Character && c == '\'')) state = State::Code;
        }
    }
    return out;
}

mcpp::modgraph::PackageRoot package(
    const std::filesystem::path& root,
    std::string ns,
    std::string name,
    std::string version,
    std::string source) {
    mcpp::modgraph::PackageRoot out;
    out.root = root;
    out.manifest.package.namespace_ = std::move(ns);
    out.manifest.package.name = std::move(name);
    out.manifest.package.version = std::move(version);
    out.manifest.package.sourceProvenance = std::move(source);
    return out;
}

TEST(RuntimeContract, TomlReadsStructuredValuesAndKeepsLegacyFields) {
    auto parsed = mf::parse_string(R"(
[package]
name = "consumer"
version = "1.2.3"

[runtime]
library_dirs = ["legacy/run"]
dlopen_libs = ["liblegacy.so.1"]
capabilities = ["legacy.display"]
provides = ["legacy.provider"]
libraries = ["widget"]
link_library_dirs = ["link"]
transitive_needed_dirs = ["needed"]
runtime_search_dirs = ["run"]
frameworks = ["WindowKit"]
deploy_files = ["bin/widget.dll"]
requirements = [
  { kind = "capability", value = "display.present", phase = "run", required = true },
  { kind = "soname", value = "libwidget.so.1", phase = "link", required = false },
]
artifacts = [
  { role = "library", path = "run/libwidget.so.1", provenance = "payload", abi = "elf-x86_64", digest = "sha256:abc", host_fingerprint = "host-1" },
]
)");
    ASSERT_TRUE(parsed) << parsed.error().format();
    ASSERT_EQ(parsed->runtimeConfig.requirements.size(), 2u);
    EXPECT_EQ(parsed->runtimeConfig.requirements[0].kind, "capability");
    EXPECT_EQ(parsed->runtimeConfig.requirements[0].value, "display.present");
    EXPECT_TRUE(parsed->runtimeConfig.requirements[0].required);
    EXPECT_FALSE(parsed->runtimeConfig.requirements[1].required);
    ASSERT_EQ(parsed->runtimeConfig.artifacts.size(), 1u);
    EXPECT_EQ(parsed->runtimeConfig.artifacts[0].role, "library");
    EXPECT_EQ(parsed->runtimeConfig.artifacts[0].hostFingerprint, "host-1");
    EXPECT_EQ(parsed->runtimeConfig.linkIntent.libraries,
              std::vector<std::string>{"widget"});
    EXPECT_EQ(parsed->runtimeConfig.linkIntent.runtimeSearchDirs,
              std::vector<std::filesystem::path>{"run"});
    EXPECT_EQ(parsed->runtimeConfig.libraryDirs[0], "legacy/run");
    EXPECT_EQ(parsed->runtimeConfig.capabilities[0], "legacy.display");
    EXPECT_EQ(parsed->runtimeConfig.provides[0], "legacy.provider");
}

TEST(RuntimeContract, XpkgReadsTheSameStructuredContract) {
    constexpr auto lua = R"(
package = {
  spec = "1",
  namespace = "acme",
  name = "backend",
  xpm = { linux = { ["2.0.0"] = { url = "u", sha256 = "h" } } },
  mcpp = {
    sources = { "src/backend.cpp" },
    runtime = {
      requirements = {
        { kind = "capability", value = "display.present", phase = "run", required = true },
      },
      artifacts = {
        { role = "library", path = "lib/libbackend.so", provenance = "subos_view", abi = "elf-x86_64" },
      },
      libraries = { "backend" },
      link_library_dirs = { "lib" },
      transitive_needed_dirs = { "closure" },
      runtime_search_dirs = { "runtime" },
      frameworks = { "WindowKit" },
      deploy_files = { "bin/backend.dll" },
    },
  },
}
)";
    auto parsed = mf::synthesize_from_xpkg_lua(
        lua, "backend", "2.0.0", mcpp::platform::HostPlatform::current());
    ASSERT_TRUE(parsed) << parsed.error().format();
    ASSERT_EQ(parsed->runtimeConfig.requirements.size(), 1u);
    EXPECT_EQ(parsed->runtimeConfig.requirements[0].value, "display.present");
    ASSERT_EQ(parsed->runtimeConfig.artifacts.size(), 1u);
    EXPECT_EQ(parsed->runtimeConfig.artifacts[0].path, "lib/libbackend.so");
    EXPECT_EQ(parsed->runtimeConfig.linkIntent.transitiveNeededDirs,
              std::vector<std::filesystem::path>{"closure"});
}

TEST(RuntimeContract, RequirementsNeverSelfProvideAndCanonicalIdsDoNotCollide) {
    auto base = std::filesystem::temp_directory_path() / "mcpp-runtime-contract";
    auto consumer = package(base / "consumer", "mcpplibs", "consumer", "1.0.0",
                            "path+consumer");
    consumer.manifest.runtimeConfig.requirements.push_back({
        .kind = "capability", .value = "display.present", .phase = "run",
        .required = true,
    });
    // Legacy `capabilities` remains a requirement, never an implicit provider.
    consumer.manifest.runtimeConfig.capabilities.push_back("legacy.display");

    auto alpha = package(base / "alpha", "alpha", "backend", "2.0.0",
                         "index+alpha@rev-a");
    alpha.manifest.runtimeConfig.provides.push_back("display.present");
    alpha.manifest.runtimeConfig.artifacts.push_back({
        .role = "library", .path = "lib/libbackend.so",
        .provenance = "payload", .abi = "elf-x86_64",
    });
    alpha.manifest.runtimeConfig.linkIntent.libraries = {
        "lib/libbackend.a", "backend"};

    auto beta = package(base / "beta", "beta", "backend", "3.0.0",
                        "index+beta@rev-b");
    beta.manifest.runtimeConfig.provides.push_back("display.present");
    beta.manifest.runtimeConfig.artifacts.push_back({
        .role = "library", .path = "lib/libbackend.so",
        .provenance = "host_link", .abi = "elf-x86_64",
    });

    auto contract = build::resolve_runtime_contract({consumer, alpha, beta});
    ASSERT_EQ(contract.requirements.size(), 2u);
    EXPECT_EQ(contract.requirements[0].requester.canonical(),
              "mcpplibs.consumer@1.0.0");
    EXPECT_EQ(contract.requirements[0].requester.sourceProvenance,
              "path+consumer");

    ASSERT_EQ(contract.providers.size(), 2u);
    EXPECT_EQ(contract.providers[0].provider.canonical(), "alpha.backend@2.0.0");
    EXPECT_EQ(contract.providers[1].provider.canonical(), "beta.backend@3.0.0");
    EXPECT_NE(contract.providers[0].provider, contract.providers[1].provider);
    EXPECT_TRUE(std::ranges::none_of(contract.providers, [](auto const& provider) {
        return provider.provider.canonical() == "mcpplibs.consumer@1.0.0";
    }));

    ASSERT_EQ(contract.artifacts.size(), 2u);
    EXPECT_EQ(contract.artifacts[0].provider.canonical(), "alpha.backend@2.0.0");
    EXPECT_EQ(contract.artifacts[1].provider.canonical(), "beta.backend@3.0.0");
    EXPECT_EQ(contract.artifacts[0].path,
              (base / "alpha/lib/libbackend.so").lexically_normal());
    ASSERT_EQ(contract.linkIntent.libraries.size(), 2u);
    EXPECT_EQ(contract.linkIntent.libraries[0],
              (base / "alpha/lib/libbackend.a").lexically_normal().string());
    EXPECT_EQ(contract.linkIntent.libraries[1], "backend");
}

TEST(RuntimeContract, SourceOwnsNoProviderSpecificSelectionOrProbeBranch) {
    auto repo = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    auto src = repo / "src";
    ASSERT_TRUE(std::filesystem::is_directory(src)) << src;
    // Intentionally assembled so the ownership gate does not flag its own
    // vocabulary if this test is ever moved under src/.
    const std::vector<std::string> providerWords = {
        "open" "gl", "vul" "kan", "me" "sa", "nvi" "dia",
        "w" "sl", "i" "cd",
    };
    const std::vector<std::string> launchWords = {
        "capture_" "exec", "run_" "exec", "spawn", "system(",
    };
    for (auto it = std::filesystem::recursive_directory_iterator(src);
         it != std::filesystem::recursive_directory_iterator{}; ++it) {
        if (!it->is_regular_file()) continue;
        auto ext = it->path().extension().string();
        if (ext != ".cpp" && ext != ".cppm") continue;
        std::ifstream input(it->path());
        std::string raw((std::istreambuf_iterator<char>(input)), {});
        auto code = source_without_comments(raw);
        std::ranges::transform(code, code.begin(),
            [](unsigned char c) { return std::tolower(c); });

        std::size_t line = 0;
        for (auto text : code | std::views::split('\n')) {
            ++line;
            std::string lineText(text.begin(), text.end());
            const bool branch = lineText.find("if (") != std::string::npos
                || lineText.find("if(") != std::string::npos
                || lineText.find("switch (") != std::string::npos
                || lineText.find("switch(") != std::string::npos
                || lineText.find("case ") != std::string::npos;
            if (!branch) continue;
            for (auto const& word : providerWords) {
                EXPECT_EQ(lineText.find(word), std::string::npos)
                    << it->path() << ':' << line
                    << " contains provider-specific selection logic";
            }
        }
        for (auto const& provider : providerWords) {
            for (auto pos = code.find(provider); pos != std::string::npos;
                 pos = code.find(provider, pos + 1)) {
                const auto begin = pos > 240 ? pos - 240 : 0;
                const auto window = code.substr(begin, 480);
                for (auto const& launch : launchWords) {
                    EXPECT_EQ(window.find(launch), std::string::npos)
                        << it->path()
                        << " couples a provider-specific word to a launched probe";
                }
            }
        }
    }
}

TEST(RuntimeContract, XlingsSelectedFactsPrecedeDescriptorFallbacks) {
    build::BuildPlan plan;
    plan.runtimeProviders.push_back({
        .capability = "render.demo",
        .provider = {.namespace_ = "mcpplibs", .name = "fallback",
                     .version = "1.0.0", .sourceProvenance = "index+mcpplibs"},
    });
    mcpp::platform::runtime::RuntimeBinding binding;
    binding.runtimeProviders.push_back({
        .capability = "render.demo",
        .provider = {.namespace_ = "xim", .name = "selected",
                     .version = "4.0.0", .source = "xim-pkgindex@rev"},
    });
    binding.runtimeArtifacts.push_back({
        .role = "driver",
        .provider = {.namespace_ = "xim", .name = "selected",
                     .version = "4.0.0", .source = "xim-pkgindex@rev"},
        .path = "/runtime/selected.artifact",
        .provenance = "subos_view",
        .abi = "fixture-v1",
        .digest = "sha256:def",
        .hostFingerprint = "host-2",
    });

    build::merge_runtime_binding_contract(plan, binding);
    ASSERT_EQ(plan.runtimeProviders.size(), 2u);
    EXPECT_EQ(plan.runtimeProviders.front().provider.canonical(),
              "xim.selected@4.0.0");
    ASSERT_EQ(plan.runtimeArtifacts.size(), 1u);
    EXPECT_EQ(plan.runtimeArtifacts[0].provider.canonical(),
              "xim.selected@4.0.0");
    EXPECT_EQ(plan.runtimeArtifacts[0].hostFingerprint, "host-2");
}

}  // namespace
