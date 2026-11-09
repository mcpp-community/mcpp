// mcpp.manifest.platform_providers — what a platform's statement provides
// (SPEC-016 §3, §5).
//
// A statement's providers are its `[[platform.provider]]` tables, `[platform]`
// as the provider of its `targets`, and `[platform.hosted]` as the hosted
// provider of the systems it names. The engine registers these providers, the
// reader checks them against the rows, and `mcpp why platform` reports the
// identities they serve; all three read this one list.

export module mcpp.manifest.platform_providers;

import std;
import mcpp.manifest.platform;
import mcpp.toolchain.triple;
import mcpp.toolchain.target_schema;
import mcpp.toolchain.builtin_targets;

export namespace mcpp::manifest::platform {

struct StatedProvider {
    std::string              name;
    std::vector<std::string> serves, hosts, pairs;
    int                      priority = 100;
    std::string              toolchain, sysroot, linkage;
    bool                     shorthand = false;   // `[platform]` or `[platform.hosted]`
};

// `name` is the platform's name (`platform_name`), the name of every provider
// that states none.
std::vector<StatedProvider> stated_providers(const Statement& s, std::string_view name);

// One identity a provider serves, with the provider's priority.
struct ServedIdentity {
    std::string identity, provider;
    int         priority = 0;
};

// The identities the statement's providers serve among the built-in rows and the
// rows it defines, by identity, then priority (highest first), then provider.
std::vector<ServedIdentity> served_identities(const Statement& s, std::string_view name);

// The statement checked against the rows: a row defined twice, or a definition
// of a built-in row, is an error; a provider that serves no row, and two
// providers that serve one identity at one priority, are warnings.
struct StatementFindings {
    std::vector<std::string> errors, warnings;
};
StatementFindings check_statement(const Statement& s, std::string_view name);

} // namespace mcpp::manifest::platform

namespace mcpp::manifest::platform {

namespace {

namespace target = mcpp::toolchain::target;

bool builtin_row(std::string_view triple) {
    return std::ranges::any_of(mcpp::toolchain::builtin::kRows,
                               [&](auto const& r) { return r.triple == triple; });
}

// The rows a pattern matches: the statement's own, then the built-in ones.
std::vector<std::string> rows_matching(const Statement& s, std::string_view pattern) {
    std::vector<std::string> out;
    for (auto const& d : s.defines)
        if (target::serves_triple(pattern, d.triple) && !std::ranges::contains(out, d.triple))
            out.push_back(d.triple);
    for (auto const& r : mcpp::toolchain::builtin::kRows)
        if (target::serves_triple(pattern, r.triple) && !std::ranges::contains(out, r.triple))
            out.emplace_back(r.triple);
    return out;
}

} // namespace

std::vector<StatedProvider> stated_providers(const Statement& s, std::string_view name) {
    std::vector<StatedProvider> out;
    for (auto const& p : s.providers)
        out.push_back({ p.name.empty() ? std::string(name) : p.name, p.serves, p.hosts, p.pairs, p.priority,
                        p.toolchain.empty() ? s.toolchain.value : p.toolchain, p.sysroot, p.linkage, false });
    if (s.targets.set)
        out.push_back({ std::string(name), s.targets.value, {"*"}, {}, s.prioritySet ? s.priority : 100,
                        s.toolchain.value, {}, {}, true });
    // A HOSTED ENTRY NAMES A SYSTEM; THE C LIBRARY NAMES THE ENV: it serves every
    // identity of that architecture and system.
    if (s.hosted.set) {
        std::vector<std::string> serves;
        for (auto const& w : s.hosted.targets.value)
            if (auto t = mcpp::toolchain::triple::parse(w)) {
                serves.push_back(std::format("{}-{}-*", t->arch, t->os));
                serves.push_back(std::format("{}-{}", t->arch, t->os));
            }
        if (!s.hosted.targets.set) serves = {"*-*-*"};
        out.push_back({ std::string(name), std::move(serves), {"*"}, {}, s.prioritySet ? s.priority - 50 : 50,
                        s.hosted.toolchain.set ? s.hosted.toolchain.value : s.toolchain.value, {}, {}, true });
    }
    return out;
}

std::vector<ServedIdentity> served_identities(const Statement& s, std::string_view name) {
    std::vector<ServedIdentity> out;
    for (auto const& p : stated_providers(s, name)) {
        std::set<std::string> seen;
        for (auto const& pattern : p.serves)
            for (auto& id : rows_matching(s, pattern))
                if (seen.insert(id).second) out.push_back({ std::move(id), p.name, p.priority });
    }
    std::ranges::sort(out, [](auto const& a, auto const& b) {
        return std::tie(a.identity, b.priority, a.provider) < std::tie(b.identity, a.priority, b.provider);
    });
    return out;
}

StatementFindings check_statement(const Statement& s, std::string_view name) {
    StatementFindings f;
    std::set<std::string> own;
    for (auto const& d : s.defines) {
        if (builtin_row(d.triple))
            f.errors.push_back(std::format("[[platform.define]] '{}' is a built-in row; a platform serves it "
                                           "with [[platform.provider]] serves = [\"{}\"]", d.triple, d.triple));
        else if (!own.insert(d.triple).second)
            f.errors.push_back(std::format("[[platform.define]] '{}' is defined twice", d.triple));
    }
    for (auto const& p : stated_providers(s, name)) {
        if (p.shorthand) continue;
        if (std::ranges::none_of(p.serves, [&](auto const& pattern) { return !rows_matching(s, pattern).empty(); }))
            f.warnings.push_back(std::format("[[platform.provider]] '{}' serves no row ({})", p.name,
                                             p.serves.empty() ? std::string("serves is empty") : p.serves.front()));
    }
    const auto served = served_identities(s, name);
    for (std::size_t i = 1; i < served.size(); ++i) {
        auto const& a = served[i - 1];
        auto const& b = served[i];
        if (a.identity == b.identity && a.priority == b.priority && a.provider != b.provider)
            f.warnings.push_back(std::format("'{}' is served by '{}' and '{}' at priority {}: the name orders them",
                                             b.identity, a.provider, b.provider, b.priority));
    }
    return f;
}

} // namespace mcpp::manifest::platform
