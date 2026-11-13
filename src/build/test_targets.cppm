// mcpp.build.test_targets — discovers convention-based test binaries.

export module mcpp.build.test_targets;

import std;
import mcpp.manifest;
import mcpp.modgraph.glob;
import mcpp.modgraph.scanner;
import mcpp.project;
import mcpp.build.plan;   // source_defines_main
import mcpp.diag;

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
        // The effective manifest: a member that omits `version` still loads
        // instead of falling back to the default discovery (#690, W4).
        if (auto manifest = mcpp::project::load_effective_manifest(packageRoot))
            packageManifest = std::move(manifest->manifest);
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

    // `[test] programs = "directory"`: the files of one directory below a
    // glob's fixed directory are one program, named by the directory; its
    // `main` is the file that defines one, else the first, and the others
    // are its translation units. Two files of one directory that define
    // `main` are two beginnings of one program and are refused. A file
    // directly in the fixed directory stays a program of its own.
    std::map<std::filesystem::path, std::vector<std::filesystem::path>> sourcesOf;
    // The name of a program that is more than one file, by its main file.
    std::map<std::filesystem::path, std::string> programNameOf;
    const std::string programs = packageManifest ? packageManifest->testPrograms : std::string{};
    // `[test] programs = "package"` (mcpp#927, 2026-11-12 §18 P1): every file
    // the package discovers is one program, named by the fixed directory of
    // the first glob that found one (`tests`), linked once. A test is still
    // its own process, as every case of a program is.
    if (programs == "package" && !testFiles.empty()) {
        std::vector<std::filesystem::path> mains;
        for (auto const& [file, base] : testFiles)
            if (source_defines_main(file)) mains.push_back(file);
        if (mains.size() > 1)
            return std::unexpected(std::format(
                "[test] programs = \"package\": '{}' and '{}' both define `main`, and the files the "
                "package discovers are one program with one beginning",
                mains[0].lexically_relative(packageRoot).generic_string(),
                mains[1].lexically_relative(packageRoot).generic_string()));
        const auto base = testFiles.front().second;
        const auto main = mains.empty() ? testFiles.front().first : mains.front();
        for (auto const& [file, b] : testFiles)
            if (file != main) sourcesOf[main].push_back(file);
        auto name = base.lexically_relative(packageRoot).generic_string();
        if (name.empty() || name == ".") name = packageRoot.filename().generic_string();
        programNameOf[main] = name;
        testFiles = {{main, base}};
    }
    if (programs == "directory") {
        std::map<std::pair<std::filesystem::path, std::filesystem::path>, std::vector<std::filesystem::path>> groups;
        for (auto const& [file, base] : testFiles) {
            const auto dir = file.parent_path();
            if (dir.lexically_normal() == base.lexically_normal()) continue;
            groups[{dir, base}].push_back(file);
        }
        std::set<std::filesystem::path> grouped;
        std::vector<std::pair<std::filesystem::path, std::filesystem::path>> merged;
        for (auto& [key, files] : groups) {
            std::vector<std::filesystem::path> mains;
            std::ranges::copy_if(files, std::back_inserter(mains), source_defines_main);
            if (mains.size() > 1)
                return std::unexpected(std::format(
                    "[test] programs = \"directory\": '{}' and '{}' both define `main`, and the files of "
                    "'{}' are one program with one beginning",
                    mains[0].lexically_relative(packageRoot).generic_string(),
                    mains[1].lexically_relative(packageRoot).generic_string(),
                    key.first.lexically_relative(packageRoot).generic_string()));
            const auto main = mains.empty() ? files.front() : mains.front();
            for (auto const& f : files) {
                grouped.insert(f);
                if (f != main) sourcesOf[main].push_back(f);
            }
            // The directory names the program: the main file's name is replaced
            // by the directory's when the name is formed below.
            merged.emplace_back(main, key.second);
        }
        // The files the setting leaves as programs of their own, said once:
        // the reader of a suite that did not get one program per directory
        // learns which files and what does (§18 P1). A setting that changes
        // no program is a warning, which --strict refuses.
        std::map<std::filesystem::path, std::size_t> direct;
        for (auto const& [file, base] : testFiles)
            if (!grouped.contains(file)) ++direct[base];
        for (auto const& [base, n] : direct) {
            const auto where = base.lexically_relative(packageRoot).generic_string();
            if (groups.empty())
                mcpp::diag::degraded("test/programs", std::format(
                    "[test] programs = \"directory\" changes no program: the {} test file{} {} directly in {}/",
                    n, n == 1 ? "" : "s", n == 1 ? "is" : "are", where),
                    "each stays a program of its own, as without the setting",
                    "\"package\" makes the suite one program; a directory below it makes one of its files");
            else
                mcpp::diag::note("test/programs", std::format(
                    "[test] programs = \"directory\": {} file{} directly in {}/ stay{} program{} of {} own; "
                    "\"package\" makes the suite one program",
                    n, n == 1 ? "" : "s", where, n == 1 ? "s a" : "", n == 1 ? "" : "s",
                    n == 1 ? "its" : "their"));
        }
        std::erase_if(testFiles, [&](auto const& fb) { return grouped.contains(fb.first); });
        testFiles.insert(testFiles.end(), merged.begin(), merged.end());
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

        const auto merged = sourcesOf.find(file);
        const bool directoryProgram = programs == "directory"
            && file.parent_path().lexically_normal() != discoverBase.lexically_normal();
        const auto named = programNameOf.find(file);
        auto name = named != programNameOf.end() ? named->second
                  : directoryProgram ? testRelative->parent_path().generic_string()
                                     : testRelative->replace_extension("").generic_string();
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
        if (merged != sourcesOf.end())
            for (auto const& f : merged->second)
                target.testSources.push_back(f.lexically_relative(packageRoot).string());
        if (packageManifest) {
            target.windowsCodePage = packageManifest->testWindowsCodePage;
            target.windowsLongPaths = packageManifest->testWindowsLongPaths;
            // `[test] main = { from }` and `[test] start` (design 2026-10-26
            // §24.4 item 6): the test file stays `main`, the translation unit
            // that is this test's own, and the beginning comes from the entry.
            target.mainFrom = packageManifest->testMainFrom;
            if (!packageManifest->testMainFrom.empty())
                target.mainDeclaredBy = "[test] main";
            target.start = packageManifest->testStart;
            if (!target.start.empty())
                target.startDeclaredBy = std::format("[test] start = \"{}\"", target.start);
        }
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
    // A TEST'S NAME IS A PATH OF ITS PROGRAM (`bin/<name>`): one name that is
    // another's directory would make one program a directory of the other's.
    // The names are sorted: the first name below `<name>/` is the one to ask.
    for (auto it = seenNames.begin(); it != seenNames.end(); ++it) {
        auto next = seenNames.lower_bound(it->first + "/");
        if (next == seenNames.end() || !next->first.starts_with(it->first + "/")) continue;
        return std::unexpected(std::format(
            "test '{}' ({}) is a directory of test '{}' ({}): a test's name is the path of its "
            "program, and one cannot be both; move one of them",
            it->first, it->second.generic_string(), next->first, next->second.generic_string()));
    }
    return result;
}

} // namespace mcpp::build
