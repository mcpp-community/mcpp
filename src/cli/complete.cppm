// mcpp.cli.complete — the candidates mcpp's command line completes from.
//
// Completion is mcpplibs.cmdline's (design 2026-11-27 §4.11): a shell's adapter
// passes the words before the cursor to `mcpp __complete`, and the library
// answers from the command tree by the rules its parse reads it by. What the
// tree cannot know is mcpp's, and is here: the profiles and the members the
// manifest in this directory states, the dependencies it lists, the targets
// the engine has rows for, the toolchains installed in the home, the packages
// of the local index copies.
//
// Each reads what is already on disk and nothing more: a completion runs at
// every press of a key, so it initialises no home, fetches no index and plans
// no build. A question it cannot answer gives no candidates.

export module mcpp.cli.complete;

import std;
import mcpp.manifest;
import mcpp.project;
import mcpp.home;
import mcpp.toolchain.target_rows;

export namespace mcpp::cli::complete {

std::vector<std::string> profiles(std::string_view prefix);
std::vector<std::string> members(std::string_view prefix);
std::vector<std::string> dependencies(std::string_view prefix);
std::vector<std::string> targets(std::string_view prefix);
std::vector<std::string> toolchains(std::string_view prefix);
std::vector<std::string> index_packages(std::string_view prefix);

} // namespace mcpp::cli::complete

namespace mcpp::cli::complete {

namespace {

namespace fs = std::filesystem;

std::optional<mcpp::manifest::Manifest> here() {
    std::error_code ec;
    auto root = mcpp::project::find_manifest_root(fs::current_path(ec));
    if (!root) return std::nullopt;
    auto m = mcpp::manifest::load(*root / "mcpp.toml");
    if (!m) return std::nullopt;
    return std::move(*m);
}

void keep(std::vector<std::string>& out, std::string value, std::string_view prefix) {
    if (value.starts_with(prefix) && std::ranges::find(out, value) == out.end()) out.push_back(std::move(value));
}

// The home as it is, when there is one: a completion does not make one.
std::optional<fs::path> existing_home() {
    try {
        auto home = mcpp::home::root();
        std::error_code ec;
        if (fs::is_directory(home, ec)) return home;
    } catch (...) {
    }
    return std::nullopt;
}

} // namespace

std::vector<std::string> profiles(std::string_view prefix) {
    std::vector<std::string> out;
    for (auto p : {"dev", "release", "dist"}) keep(out, p, prefix);
    if (auto m = here())
        for (auto const& [name, _] : m->profiles) keep(out, name, prefix);
    return out;
}

std::vector<std::string> members(std::string_view prefix) {
    std::vector<std::string> out;
    if (auto m = here())
        for (auto const& member : m->workspace.members) keep(out, member, prefix);
    return out;
}

std::vector<std::string> dependencies(std::string_view prefix) {
    std::vector<std::string> out;
    if (auto m = here())
        for (auto const* family : {&m->dependencies, &m->devDependencies, &m->buildDependencies})
            for (auto const& [key, _] : *family) keep(out, key, prefix);
    return out;
}

std::vector<std::string> targets(std::string_view prefix) {
    std::vector<std::string> out;
    for (auto const* row : mcpp::toolchain::target::rows()) keep(out, std::string(row->triple), prefix);
    return out;
}

std::vector<std::string> toolchains(std::string_view prefix) {
    std::vector<std::string> out;
    auto home = existing_home();
    if (!home) return out;
    std::error_code ec;
    // `<home>/registry/data/xpkgs/xim-x-<family>/<version>`: what
    // `mcpp toolchain install` installed.
    for (auto family : {"gcc", "llvm", "msvc", "emsdk"})
        for (auto const& v : fs::directory_iterator(*home / "registry" / "data" / "xpkgs" / std::format("xim-x-{}", family), ec))
            if (v.is_directory(ec)) keep(out, std::format("{}@{}", family, v.path().filename().string()), prefix);
    return out;
}

std::vector<std::string> index_packages(std::string_view prefix) {
    std::vector<std::string> out;
    auto home = existing_home();
    if (!home) return out;
    std::error_code ec;
    // `<home>/indices/<index>/pkgs/<letter>/<name>.lua`, named `<namespace>.<name>`
    // when the descriptor states a namespace other than the default one.
    std::vector<fs::path> descriptors;
    for (auto const& index : fs::directory_iterator(*home / "indices", ec))
        for (auto const& d : fs::recursive_directory_iterator(index.path() / "pkgs", ec))
            if (d.path().extension() == ".lua") descriptors.push_back(d.path());
    for (auto const& file : descriptors) {
        std::ifstream in(file);
        std::string text(4096, '\0');
        in.read(text.data(), static_cast<std::streamsize>(text.size()));
        text.resize(static_cast<std::size_t>(in.gcount()));
        std::string ns;
        if (auto at = text.find("namespace"); at != std::string::npos)
            if (auto q = text.find('"', at); q != std::string::npos)
                if (auto e = text.find('"', q + 1); e != std::string::npos) ns = text.substr(q + 1, e - q - 1);
        const auto name = file.stem().string();
        keep(out, ns.empty() || ns == "mcpplibs" ? name : std::format("{}.{}", ns, name), prefix);
    }
    return out;
}

} // namespace mcpp::cli::complete
