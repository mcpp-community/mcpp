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
import mcpp.config;
import mcpp.provider;
import mcpp.provider.select;
import mcpp.toolchain.payloads;
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

// The home's configuration, read as every command reads it and with nothing
// written (`Initialization::ReadOnly`): where its provider's stores and its
// index copies are is the configuration's answer, not a spelling of this file.
std::optional<mcpp::config::GlobalConfig> existing_config() {
    if (!existing_home()) return std::nullopt;
    auto cfg = mcpp::config::load_or_init(/*quiet=*/true, {}, {}, mcpp::config::Initialization::ReadOnly);
    if (!cfg) return std::nullopt;
    return std::move(*cfg);
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
    auto cfg = existing_config();
    if (!cfg) return out;
    // What `mcpp toolchain install` installed: the entries of the stores of the
    // home's provider that a request may name (the payload table,
    // mcpp.toolchain.payloads), as `<payload>@<version>`.
    for (auto const& store : mcpp::provider::open(mcpp::config::make_xlings_env(*cfg))->stores())
        for (auto const& entry : mcpp::provider::layout().entries(store))
            if (mcpp::toolchain::payloads::named(entry.name))
                keep(out, std::format("{}@{}", entry.name, entry.version), prefix);
    return out;
}

std::vector<std::string> index_packages(std::string_view prefix) {
    std::vector<std::string> out;
    auto cfg = existing_config();
    if (!cfg) return out;
    std::error_code ec;
    // The descriptors of the home's index copies (`<index>/pkgs/`), each named as
    // a dependency key spells it: the package's identity, read by the reader
    // `mcpp add` uses. A package of the default namespace is offered by its bare
    // name, and by its qualified one once the word names the namespace.
    std::vector<fs::path> descriptors;
    for (auto const& index : cfg->indexRepos) {
        if (index.dir.empty()) continue;
        for (auto it = fs::recursive_directory_iterator(index.dir / "pkgs", ec);
             !ec && it != fs::recursive_directory_iterator{}; it.increment(ec))
            if (it->path().extension() == ".lua") descriptors.push_back(it->path());
    }
    const std::string qualified = std::format("{}.", mcpp::manifest::kDefaultNamespace);
    for (auto const& file : descriptors) {
        std::ifstream in(file, std::ios::binary);
        const std::string text{std::istreambuf_iterator<char>(in), {}};
        const auto id = mcpp::manifest::canonical_xpkg_identity_from_lua(text);
        if (id.name.empty()) continue;
        const std::string ns = id.ns.empty() ? std::string(mcpp::manifest::kDefaultNamespace) : id.ns;
        const bool bare = ns == mcpp::manifest::kDefaultNamespace && !prefix.starts_with(qualified);
        keep(out, bare ? id.name : std::format("{}.{}", ns, id.name), prefix);
    }
    return out;
}

} // namespace mcpp::cli::complete
