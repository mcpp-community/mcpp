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
    // The globs discovery used, and whether the manifest wrote them. Carried
    // so that "no tests found" can name where it looked.
    std::vector<std::string> discover;
    bool discoverDeclared = false;
};

// `[test] discover` when the manifest says nothing.
inline constexpr std::string_view kDefaultTestDiscover = "tests/**/*.cpp";

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

    TestTargetSet result{packageRoot, {}, {}, false};
    if (packageManifest && packageManifest->testDiscoverDeclared) {
        result.discover = packageManifest->testDiscover;
        result.discoverDeclared = true;
    } else {
        result.discover = { std::string(kDefaultTestDiscover) };
    }

    // `[test] discover` IN THE VOCABULARY OF `[build] sources` (#634 A5):
    // every positive glob is expanded, then every `!` glob, and an excluded
    // file is excluded whichever positive glob found it. A file keeps the
    // FIRST positive glob that found it, because that glob's fixed prefix is
    // what its name is relative to -- which is how the default,
    // `tests/**/*.cpp`, names `tests/unit/test_span.cpp` `unit/test_span`, the
    // name every earlier release gave it.
    std::vector<std::pair<std::filesystem::path, std::filesystem::path>> testFiles;
    {
        std::set<std::filesystem::path> excluded;
        for (auto const& g : result.discover) {
            if (g.starts_with('!'))
                for (auto& p : mcpp::modgraph::expand_glob(packageRoot, g.substr(1)))
                    excluded.insert(p);
        }
        std::set<std::filesystem::path> seenFiles;
        for (auto const& g : result.discover) {
            if (g.starts_with('!')) continue;
            const auto prefix = mcpp::modgraph::glob_literal_prefix(g);
            const auto base = prefix.empty() ? packageRoot : packageRoot / prefix;
            for (auto& p : mcpp::modgraph::expand_glob(packageRoot, g)) {
                if (excluded.contains(p) || !seenFiles.insert(p).second) continue;
                testFiles.emplace_back(p, base);
            }
        }
    }
    std::vector<std::set<std::filesystem::path>> globHits;
    globHits.reserve(globFlags.size());
    for (auto const& gf : globFlags) {
        auto hits = mcpp::modgraph::expand_glob(packageRoot, gf.glob);
        globHits.emplace_back(hits.begin(), hits.end());
    }

    result.targets.reserve(testFiles.size());
    std::map<std::string, std::filesystem::path> seenNames;
    for (auto const& [file, discoverBase] : testFiles) {
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

        auto testRelative = lexical_relative(discoverBase, "its discover glob's directory");
        if (!testRelative) return std::unexpected(testRelative.error());
        auto mainRelative = lexical_relative(packageRoot, "package root");
        if (!mainRelative) return std::unexpected(mainRelative.error());

        auto name = testRelative->replace_extension("").generic_string();
        if (auto [it, fresh] = seenNames.emplace(name, *mainRelative); !fresh) {
            return std::unexpected(std::format(
                "duplicate test name '{}': '{}' and '{}' map to the same name "
                "(a test's name is its path relative to the fixed directory of "
                "the [test] discover glob that found it)",
                name, it->second.generic_string(), mainRelative->generic_string()));
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
