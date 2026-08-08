// mcpp.build.test_targets — shared tests/**/*.cpp discovery for test and IDE.
export module mcpp.build.test_targets;

import std;
import mcpp.manifest;
import mcpp.modgraph.scanner;
import mcpp.project;

export namespace mcpp::build {

struct TestTargetDiscovery {
    std::filesystem::path root;
    std::vector<mcpp::manifest::Target> targets;
};

std::expected<TestTargetDiscovery, std::string>
discover_test_targets(const std::filesystem::path& manifestRoot,
                      std::string_view packageFilter = {}) {
    auto rootManifest = mcpp::manifest::load(manifestRoot / "mcpp.toml");
    auto testRoot = manifestRoot;
    std::optional<mcpp::manifest::Manifest> manifest;
    if (rootManifest) {
        auto member = mcpp::project::resolve_member_dir(
            *rootManifest, manifestRoot, packageFilter);
        if (!member) return std::unexpected(member.error());
        testRoot = member->empty() ? manifestRoot : *member;

        if (member->empty()) {
            manifest.emplace(std::move(*rootManifest));
        } else if (auto loaded = mcpp::manifest::load(testRoot / "mcpp.toml")) {
            manifest.emplace(std::move(*loaded));
        }
    }

    const auto testFiles = mcpp::modgraph::expand_glob(testRoot, "tests/**/*.cpp");
    if (testFiles.empty()) return TestTargetDiscovery{.root = testRoot};

    // `[build].flags` 对测试源同样生效，必须与 `mcpp test` 的编译参数一致。
    struct TestGlobFlags {
        mcpp::manifest::GlobFlags flags;
        std::set<std::filesystem::path> files;
    };
    std::vector<TestGlobFlags> globFlags;
    // inventory 可在损坏源码/manifest 上工作；只有成功解析时才附加
    // `[build].flags`，真正 build/configure 随后仍由 prepare_build 严格校验。
    if (manifest) {
        for (const auto& flags : manifest->buildConfig.globFlags) {
            auto files = mcpp::modgraph::expand_glob(testRoot, flags.glob);
            globFlags.push_back({flags, {files.begin(), files.end()}});
        }
    }

    TestTargetDiscovery result{.root = testRoot};
    std::set<std::string> seenNames;
    for (const auto& file : testFiles) {
        auto relative = std::filesystem::relative(file, testRoot / "tests");
        auto name = relative.replace_extension("").generic_string();
        if (!seenNames.insert(name).second) {
            return std::unexpected(std::format(
                "duplicate test name '{}' (two test files map to the same name)", name));
        }

        mcpp::manifest::Target target;
        target.name = std::move(name);
        target.kind = mcpp::manifest::Target::TestBinary;
        target.main = std::filesystem::relative(file, testRoot).string();
        for (const auto& matched : globFlags) {
            if (!matched.files.contains(file)) continue;
            target.defines.insert(target.defines.end(),
                                  matched.flags.defines.begin(), matched.flags.defines.end());
            target.cflags.insert(target.cflags.end(),
                                 matched.flags.cflags.begin(), matched.flags.cflags.end());
            target.cxxflags.insert(target.cxxflags.end(),
                                   matched.flags.cxxflags.begin(), matched.flags.cxxflags.end());
        }
        result.targets.push_back(std::move(target));
    }
    return result;
}

} // namespace mcpp::build
