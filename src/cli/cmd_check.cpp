// mcpp.cli.cmd_check — the implementation, an implementation unit for the
// reason mcpp.cli.cmd_patch states.
module mcpp.cli.cmd_check;

import std;
import mcpplibs.cmdline;
import mcpp.manifest;
import mcpp.toolchain.target_schema;
import mcpp.toolchain.target_rows;
import mcpp.ui;

namespace mcpp::cli {

namespace {

namespace fs = std::filesystem;
namespace mp = mcpp::manifest::platform;
namespace target = mcpp::toolchain::target;

std::vector<std::string> segments(std::string_view triple) {
    std::vector<std::string> out;
    for (auto part : std::views::split(triple, '-')) out.emplace_back(part.begin(), part.end());
    return out;
}

bool word(std::string_view w) {
    return !w.empty() && std::ranges::all_of(w, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.';
    });
}

}  // namespace

int cmd_check_platform(const mcpplibs::cmdline::ParsedArgs& parsed) {
    const fs::path dir = parsed.positional(0).empty() ? fs::current_path() : fs::path(parsed.positional(0));
    auto m = mcpp::manifest::load(dir / "mcpp.toml");
    if (!m) {
        mcpp::ui::error(m.error().format());
        return 1;
    }
    if (!m->platformDeclared) {
        mcpp::ui::error(std::format("{} states no [platform]: it is not a platform package",
                                    (dir / "mcpp.toml").string()));
        return 1;
    }
    for (auto const& w : m->schemaWarnings) mcpp::ui::warning(w);
    auto const& s = m->platform;
    std::vector<std::string> errors, warnings;
    std::set<std::string> archs, own;
    for (auto const* r : target::rows()) archs.insert(std::string(r->arch));

    for (auto const& d : s.defines) {
        const auto seg = segments(d.triple);
        if (seg.size() < 2 || seg.size() > 3 || !std::ranges::all_of(seg, word)) {
            errors.push_back(std::format("[[platform.define]] '{}' is not <arch>-<os>[-<env>] of words", d.triple));
            continue;
        }
        if (!archs.contains(seg[0]))
            warnings.push_back(std::format("'{}': no built-in row has the architecture '{}'", d.triple, seg[0]));
        if (target::find_row(d.triple) && !own.contains(d.triple))
            errors.push_back(std::format("'{}' is a built-in row; a platform serves it with "
                                         "[[platform.provider]] serves = [\"{}\"]", d.triple, d.triple));
        if (!own.insert(d.triple).second)
            errors.push_back(std::format("'{}' is defined twice", d.triple));
        if (d.clangTriple.empty())
            warnings.push_back(std::format("'{}' states no clang-triple: the compiler is given the row's own spelling",
                                           d.triple));
    }

    // Each identity this platform serves, with the provider and priority.
    struct Served { std::string identity, provider; int priority; };
    std::vector<Served> served;
    auto rows_matching = [&](std::string_view pattern) {
        std::vector<std::string> out;
        for (auto const& t : own) if (target::serves_triple(pattern, t)) out.push_back(t);
        for (auto const* r : target::rows())
            if (target::serves_triple(pattern, r->triple) && !own.contains(std::string(r->triple)))
                out.emplace_back(r->triple);
        return out;
    };
    const std::string platformName = m->package.name;
    if (s.targets.set)
        for (auto const& t : s.targets.value)
            for (auto const& id : rows_matching(t))
                served.push_back({ id, platformName, s.prioritySet ? s.priority : 100 });
    for (auto const& p : s.providers) {
        const auto name = p.name.empty() ? platformName : p.name;
        std::size_t n = 0;
        for (auto const& pattern : p.serves)
            for (auto const& id : rows_matching(pattern)) {
                served.push_back({ id, name, p.priority });
                ++n;
            }
        if (n == 0)
            warnings.push_back(std::format("the provider '{}' serves no row ({})", name,
                                           p.serves.empty() ? std::string("serves is empty") : p.serves.front()));
    }
    std::ranges::sort(served, [](auto const& a, auto const& b) {
        return std::tie(a.identity, b.priority, a.provider) < std::tie(b.identity, a.priority, b.provider);
    });
    for (std::size_t i = 1; i < served.size(); ++i)
        if (served[i].identity == served[i - 1].identity && served[i].priority == served[i - 1].priority
            && served[i].provider != served[i - 1].provider)
            warnings.push_back(std::format("'{}' is served by '{}' and '{}' at priority {}: the name orders them",
                                           served[i].identity, served[i - 1].provider, served[i].provider,
                                           served[i].priority));
    if (served.empty()) errors.push_back("the platform serves no identity: state targets or [[platform.provider]] serves");

    mcpp::ui::info("Platform", std::format("{} {}", m->package.name, m->package.version));
    for (auto const& e : served)
        mcpp::ui::line(std::format("    serves {:<28} {:<20} priority {}", e.identity, e.provider, e.priority));
    for (auto const& w : warnings) mcpp::ui::warning(w);
    for (auto const& e : errors) mcpp::ui::error(e);
    if (!errors.empty()) return 1;
    mcpp::ui::info("Checked", std::format("{} row(s) defined, {} identit{} served{}", own.size(), served.size(),
                                          served.size() == 1 ? "y" : "ies",
                                          warnings.empty() ? "" : std::format(", {} warning(s)", warnings.size())));
    return 0;
}

}  // namespace mcpp::cli
