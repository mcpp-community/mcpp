// mcpp.cli.cmd_surface — mcpp's surface, and the part of it one invocation used
// (SPEC-017 P5.6 `exercise`, design 2026-11-20 §5.16).
//
// The surface is written out, never written down: the commands and flags come from the definition
// the command line is parsed with, the keys from the manifest's key registry, the reasons from the
// refusal codes. A ledger that compares the surface with what the tests used therefore cannot
// drift from the product the way a hand-kept list does.
//
// The options the parser adds itself (`--help` on every command, `--version` at the top) are
// mcpplibs.cmdline's, not mcpp's, and are not items.

export module mcpp.cli.cmd_surface;

import std;
import mcpplibs.cmdline;
import mcpp.manifest.key_registry;
import mcpp.build.refusal;
import mcpp.notice;           // the notices whose policy is registered
import mcpp.surface;
import mcpp.ui;

export namespace mcpp::cli {

// Each item of the surface, commands first, then flags, keys and reasons, each sorted by name.
std::vector<mcpp::surface::Item> surface_items(const mcpplibs::cmdline::App& app);

// `mcpp emit surface [-o FILE]`.
int cmd_emit_surface(const mcpplibs::cmdline::App& app, const mcpplibs::cmdline::ParsedArgs& parsed);

// The command and the flags `parsed` names, recorded as used (MCPP_SURFACE_RECORD).
void record_invocation(const mcpplibs::cmdline::App& app, const mcpplibs::cmdline::ParsedArgs& parsed);

// The root's flags `argv` names (`--name`, `--name=value`), recorded as used. They are read
// before the parser sees the command line -- `--no-color`, `--frozen` and `--system` are taken
// out of it, `--protocol-version` answers before it -- so the parsed record never holds them.
void record_global_flags(const mcpplibs::cmdline::App& app, int argc, char** argv);

}  // namespace mcpp::cli

namespace mcpp::cli {
namespace {

namespace cl = mcpplibs::cmdline;
namespace sf = mcpp::surface;
using Node = cl::completions::Command;

bool parser_added(std::string_view path, std::string_view option) {
    return option == "help" || (path.empty() && option == "version");
}

std::string child_path(std::string_view path, std::string_view name) {
    return path.empty() ? std::string(name) : std::format("{} {}", path, name);
}

void walk(const Node& node, const std::string& path, std::vector<sf::Item>& out) {
    if (!path.empty()) out.push_back({std::string(sf::kCommand), path});
    for (auto const& o : node.options)
        if (!parser_added(path, o.long_name))
            out.push_back({std::string(sf::kFlag), sf::flag_name(path, o.long_name)});
    for (auto const& sub : node.subcommands) walk(sub, child_path(path, sub.name), out);
}

const Node* find_sub(const Node& node, std::string_view name) {
    for (auto const& sub : node.subcommands)
        if (sub.name == name) return &sub;
    return nullptr;
}

bool declares(const Node& node, std::string_view option) {
    return std::ranges::any_of(node.options, [&](auto const& o) { return o.long_name == option; });
}

using Chain = std::vector<std::pair<const Node*, std::string>>;

// The command, at `depth` or above it, whose definition declares `option`.
const std::string* owner(const Chain& chain, std::size_t depth, std::string_view option) {
    for (std::size_t at = depth + 1; at > 0; --at)
        if (declares(*chain[at - 1].first, option)) return &chain[at - 1].second;
    return nullptr;
}

}  // namespace

std::vector<sf::Item> surface_items(const cl::App& app) {
    std::vector<sf::Item> out;
    walk(cl::snapshot(app), "", out);
    for (auto const& row : mcpp::manifest::kKeyRegistry)
        out.push_back({std::string(sf::kKey), row.key.empty()
            ? std::string(row.table) : std::format("{}.{}", row.table, row.key)});
    for (auto c : mcpp::build::refusal::codes())
        out.push_back({std::string(sf::kReason), std::string(mcpp::build::refusal::name(c))});
    for (auto const& k : mcpp::notice::kKinds)
        out.push_back({std::string(sf::kNotice), std::string(k.domain)});
    constexpr std::array kOrder{sf::kCommand, sf::kFlag, sf::kKey, sf::kReason, sf::kNotice};
    auto rank = [&](const sf::Item& i) { return std::ranges::find(kOrder, i.kind) - kOrder.begin(); };
    std::ranges::sort(out, [&](auto const& a, auto const& b) {
        return std::pair(rank(a), std::string_view(a.name)) < std::pair(rank(b), std::string_view(b.name));
    });
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

int cmd_emit_surface(const cl::App& app, const cl::ParsedArgs& parsed) {
    std::string text;
    const auto items = surface_items(app);
    for (auto const& i : items) text += sf::line(i);
    const auto o = parsed.value("output");
    if (!o) {
        std::print("{}", text);
        return 0;
    }
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(*o).parent_path(), ec);
    std::ofstream f(*o, std::ios::binary);
    if (!f) {
        mcpp::ui::error(std::format("cannot write {}", *o));
        return 2;
    }
    f << text;
    mcpp::ui::info("surface", std::format("wrote {} ({} items)", *o, items.size()));
    return 0;
}

void record_invocation(const cl::App& app, const cl::ParsedArgs& parsed) {
    if (std::getenv(std::string(sf::kRecordEnv).c_str()) == nullptr) return;
    // The parser keeps a global option at the level it was dispatched from, so a flag is the item
    // of the nearest command, from its level up, whose definition declares it.
    const Node root = cl::snapshot(app);
    Chain chain{{&root, ""}};
    std::vector<const cl::ParsedArgs*> levels{&parsed};
    for (auto* p = &parsed; p->subcommand_matches; p = p->subcommand_matches.get()) {
        const Node* sub = find_sub(*chain.back().first, p->subcommand_name_);
        if (sub == nullptr) break;
        chain.emplace_back(sub, child_path(chain.back().second, sub->name));
        levels.push_back(p->subcommand_matches.get());
        sf::exercised(sf::kCommand, chain.back().second);
    }
    for (std::size_t d = 0; d < levels.size(); ++d) {
        for (auto const& [name, value] : levels[d]->opts) {
            const auto* path = value.is_set() ? owner(chain, d, name) : nullptr;
            if (path != nullptr && !parser_added(*path, name))
                sf::exercised(sf::kFlag, sf::flag_name(*path, name));
        }
    }
}

void record_global_flags(const cl::App& app, int argc, char** argv) {
    if (std::getenv(std::string(sf::kRecordEnv).c_str()) == nullptr) return;
    const Node root = cl::snapshot(app);
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "--") break;
        if (!a.starts_with("--")) continue;
        const auto eq = a.find('=');
        const auto name = a.substr(2, eq == std::string_view::npos ? std::string_view::npos : eq - 2);
        if (declares(root, name) && !parser_added("", name)) sf::exercised(sf::kFlag, sf::flag_name("", name));
    }
}

}  // namespace mcpp::cli
