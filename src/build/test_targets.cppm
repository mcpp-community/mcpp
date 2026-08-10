// mcpp.build.test_targets — discovers convention-based test binaries.

export module mcpp.build.test_targets;

import std;
import mcpp.manifest;
import mcpp.modgraph.scanner;
import mcpp.project;

export namespace mcpp::build {

struct TestTargetSet {
    std::filesystem::path packageRoot;
    std::vector<mcpp::manifest::Target> targets;
};

// 损坏 manifest 时仅提供 best-effort 文件清单；严格调用方仍须走 prepare/validate。
std::expected<TestTargetSet, std::string>
discover_test_targets(const std::filesystem::path& manifestRoot,
                      std::string_view packageFilter) {
    auto packageRoot = manifestRoot;
    std::optional<mcpp::manifest::Manifest> packageManifest;

    // manifest 解析失败时保留文件清单，支持 `test --list` 的 best-effort 语义。
    if (auto rootManifest = mcpp::manifest::load(manifestRoot / "mcpp.toml")) {
        auto member = mcpp::project::resolve_member_dir(
            *rootManifest, manifestRoot, packageFilter);
        if (!member) return std::unexpected(member.error());
        // workspace member root is the only root accepted by prepare_build.
        if (!member->empty()) packageRoot = *member;
        if (auto manifest = mcpp::manifest::load(packageRoot / "mcpp.toml"))
            packageManifest = std::move(*manifest);
    }

    std::vector<mcpp::manifest::GlobFlags> globFlags;
    if (packageManifest) globFlags = packageManifest->buildConfig.globFlags;

    const auto testFiles = mcpp::modgraph::expand_glob(packageRoot, "tests/**/*.cpp");
    std::vector<std::set<std::filesystem::path>> globHits;
    globHits.reserve(globFlags.size());
    for (auto const& gf : globFlags) {
        auto hits = mcpp::modgraph::expand_glob(packageRoot, gf.glob);
        globHits.emplace_back(hits.begin(), hits.end());
    }

    TestTargetSet result{packageRoot, {}};
    result.targets.reserve(testFiles.size());
    std::set<std::string> seenNames;
    for (auto const& file : testFiles) {
        auto lexical_relative = [&](const std::filesystem::path& base,
                                    std::string_view boundary)
            -> std::expected<std::filesystem::path, std::string> {
            auto relative = file.lexically_relative(base);
            auto first = relative.begin();
            if (relative.empty() || relative.is_absolute()
                || (first != relative.end() && *first == "..")) {
                return std::unexpected(std::format(
                    "test file '{}' escapes {} '{}'",
                    file.string(), boundary, base.string()));
            }
            return relative;
        };

        auto testRelative = lexical_relative(packageRoot / "tests", "tests root");
        if (!testRelative) return std::unexpected(testRelative.error());
        auto mainRelative = lexical_relative(packageRoot, "package root");
        if (!mainRelative) return std::unexpected(mainRelative.error());

        auto name = testRelative->replace_extension("").generic_string();
        if (!seenNames.insert(name).second) {
            return std::unexpected(std::format(
                "duplicate test name '{}' (two test files map to the same name)", name));
        }

        mcpp::manifest::Target target;
        target.name = name;
        target.kind = mcpp::manifest::Target::TestBinary;
        target.main = mainRelative->string();
        for (std::size_t i = 0; i < globFlags.size(); ++i) {
            if (!globHits[i].contains(file)) continue;
            for (auto const& define : globFlags[i].defines)
                target.defines.push_back(define);
            for (auto const& flag : globFlags[i].cflags)
                target.cflags.push_back(flag);
            for (auto const& flag : globFlags[i].cxxflags)
                target.cxxflags.push_back(flag);
        }
        result.targets.push_back(std::move(target));
    }
    return result;
}

} // namespace mcpp::build
