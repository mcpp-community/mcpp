// The repository's own files agree with one another (formerly tests/e2e/45 and 616): install.sh
// maps each machine to an asset the release publishes; the examples the documentation links and
// the examples on disk are the same set. They read files and build nothing.
import std;
import mcpplibs.mtest;

using namespace mcpplibs::mtest;
namespace fs = std::filesystem;

namespace {

fs::path repository() { return fs::path(__FILE__).parent_path().parent_path().parent_path(); }

std::string read(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

bool any_line(const std::string& text, const std::regex& re) {
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);)
        if (std::regex_search(line, re)) return true;
    return false;
}

UTest install_platform_mapping{"RepoConsistency/InstallScriptNamesThePublishedAssets", [] {
    // install.sh's platform ids are the assets release.yml publishes: macOS arm64 is
    // `macosx-arm64`, Linux aarch64 (also reported as arm64) is `linux-aarch64`, and no macOS
    // x86_64 asset is advertised, since none is published.
    const auto install = read(repository() / "install.sh");
    const auto release = read(repository() / ".github/workflows/release.yml");
    expect(fatal(!install.empty() && !release.empty())) << "install.sh or release.yml is missing";
    expect(any_line(install, std::regex(R"(Darwin-arm64\).*PLAT="macosx-arm64")"))) << "Darwin-arm64 must map to the released macosx-arm64 asset";
    expect(any_line(install, std::regex(R"(Linux-aarch64 \| Linux-arm64\).*PLAT="linux-aarch64")"))) << "Linux-aarch64 must map to the released linux-aarch64 asset";
    expect(!any_line(install, std::regex(R"(Darwin-x86_64\).*PLAT=)"))) << "install.sh advertises Darwin-x86_64, but release CI publishes no macOS x86_64 asset";
    expect(install.contains("Currently supported: linux-x86_64, linux-aarch64, macosx-arm64")) << "the supported platform message must match the published install assets";
    expect(release.contains("mcpp-macosx-arm64.tar.gz")) << "the release workflow must publish the macosx-arm64 alias install.sh uses";
    expect(release.contains("mcpp-linux-aarch64.tar.gz")) << "the release workflow must publish the linux-aarch64 alias install.sh uses";
}};

UTest examples_and_their_index{"RepoConsistency/ExamplesAndTheirIndexAgree", [] {
    // Nothing in CI builds an example, so a rename breaks the curriculum silently. The check is
    // both ways: every example a document links exists, and every project under examples/ (a
    // directory with an mcpp.toml, at any depth) is linked, itself or through an ancestor, since
    // a document links the lesson rather than its build root. Membership is equality on a whole
    // path, so `04-work` is not reached through `04-workspace`.
    const auto root = repository();
    std::vector<fs::path> projects;
    for (auto it = fs::recursive_directory_iterator(root / "examples"); it != fs::recursive_directory_iterator{}; ++it) {
        if (it->is_directory() && it->path().filename() == "target") { it.disable_recursion_pending(); continue; }
        if (it->is_regular_file() && it->path().filename() == "mcpp.toml")
            projects.push_back(it->path().parent_path().lexically_relative(root / "examples"));
    }
    expect(fatal(!projects.empty())) << "no project under examples/";
    const std::regex link(R"(\]\(\.\.(/\.\.)?/examples/([A-Za-z0-9._/-]+)\))");
    for (std::string_view doc : {"docs/03-examples.md", "docs/zh/03-examples.md"}) {
        const auto text = read(root / doc);
        expect(fatal(!text.empty())) << "missing " << doc;
        std::set<std::string> listed;
        for (auto m = std::sregex_iterator(text.begin(), text.end(), link); m != std::sregex_iterator{}; ++m) {
            auto path = (*m)[2].str();
            while (path.ends_with('/')) path.pop_back();
            listed.insert(path);
        }
        expect(fatal(!listed.empty())) << doc << " lists no example link at all";
        for (const auto& path : listed)
            expect(fs::is_directory(root / "examples" / path)) << doc << " links examples/" << path << ", which does not exist";
        for (const auto& project : projects) {
            bool found = false;
            for (auto probe = project; !probe.empty() && probe != "."; probe = probe.parent_path())
                if (listed.contains(probe.generic_string())) { found = true; break; }
            expect(found) << "examples/" << project.generic_string() << " has an mcpp.toml and " << doc << " never links it";
        }
    }
}};

}  // namespace
