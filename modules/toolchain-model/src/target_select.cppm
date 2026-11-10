// mcpp.toolchain.target_select — from a request to an identity and its provider.
//
// THE ONE RULE OF SPEC-016 §4. Explicit choices win: the caller passes a named
// provider or a declared compiler family, and only candidates that agree with
// them remain. Otherwise the candidates of the request -- each row the request
// matches, with each provider that serves it -- are ordered by priority (larger
// first), then by row, then by provider name, and the first one available on
// this host is taken. Nothing here names a system, a C library or a provider:
// the rows and providers are data (mcpp.toolchain.target_rows), and so are the
// priorities, which a project or a machine may change.
//
// A request names a pattern. One that writes an env segment matches that row
// only; one that writes `arch-os` matches every row of that system except those
// reached only by name (`exact`), so `x86_64-windows` is the MSVC, MinGW and
// musl identities and `aarch64-macos` is both the native identity and its musl
// sibling. When no candidate is available on the host the first is taken
// anyway, unavailable, and what refuses it is the toolchain or target-side step
// that finds the missing piece, as before a provider was a concept.

export module mcpp.toolchain.target_select;

import std;
import mcpp.toolchain.target_schema;
import mcpp.toolchain.target_rows;

export namespace mcpp::toolchain::target {

struct Candidate {
    const Row*      row      = nullptr;
    const Provider* provider = nullptr;   // null: no provider serves the row
    int             priority = 0;         // after the overrides
    bool            available = false;
    std::string     why;                  // why it is not available here
};

struct Context {
    std::string host;                     // `<os>-<arch>` of the build machine
    // Answers an engine probe a provider names (`msvc`).
    std::function<bool(std::string_view probe)> probe;
    // A project's or a machine's priority for a provider on a row, if one is set.
    std::function<std::optional<int>(const Row&, std::string_view provider)> priority;
    // A declared compiler family: only providers that pair with it serve.
    std::string family;
    // A provider chosen by name (`builtin:<name>`): only it serves.
    std::string provider;
    // `--platform none`: no provider a platform package implements serves.
    bool platformPackages = true;
    // A platform package chosen for the request that no built-in provider
    // names: the providers of other platform packages do not serve it.
    std::function<bool(std::string_view platform)> platformChosen;
};

struct Selection {
    std::string request;                  // as written
    const Row*      row      = nullptr;   // the identity
    const Provider* provider = nullptr;
    int             priority = 0;
    bool            fallback = false;     // the first candidate is not available here
    std::vector<Candidate> candidates;    // in order
    std::string identity() const { return row ? std::string(row->triple) : std::string{}; }
};

// The rows a request reaches, in the table's order. `arch`, `os`, `env` are the
// request's canonical segments; `envWritten` whether the request wrote the env.
std::vector<const Row*> rows_of(std::string_view arch, std::string_view os, std::string_view env,
                                bool envWritten);

// The selection. Empty `candidates` when the request reaches no row.
Selection select(std::string_view request, std::string_view arch, std::string_view os,
                 std::string_view env, bool envWritten, const Context& ctx);

// `<os>-<arch>` of this machine, in the words `hosts` uses.
std::string host_pair(std::string_view os, std::string_view arch);

} // namespace mcpp::toolchain::target

namespace mcpp::toolchain::target {

std::vector<const Row*> rows_of(std::string_view arch, std::string_view os, std::string_view env,
                                bool envWritten) {
    std::vector<const Row*> out;
    for (auto const* r : rows()) {
        if (r->arch != arch || r->os != os) continue;
        if (envWritten ? r->env != env : (r->exact && r->env != env)) continue;
        out.push_back(r);
    }
    return out;
}

std::string host_pair(std::string_view os, std::string_view arch) {
    return std::format("{}-{}", os, arch);
}

Selection select(std::string_view request, std::string_view arch, std::string_view os,
                 std::string_view env, bool envWritten, const Context& ctx) {
    Selection s;
    s.request = std::string(request);
    const auto reached = rows_of(arch, os, env, envWritten);
    // A planned row is a candidate only when the request reaches nothing else:
    // it is then the answer, and the tier check refuses it by name.
    const bool onlyPlanned = std::ranges::all_of(reached, [](const Row* r) { return r->tier == "planned"; });
    const auto all = providers();
    for (std::size_t i = 0; i < reached.size(); ++i) {
        const Row& row = *reached[i];
        if (row.tier == "planned" && !onlyPlanned) continue;
        bool served = false;
        for (auto const* p : all) {
            if (!serves(*p, row.triple)) continue;
            if (!ctx.provider.empty() && p->name != ctx.provider) continue;
            if (!ctx.platformPackages && !p->platform.empty()) continue;
            if (ctx.platformChosen && !p->platform.empty() && !ctx.platformChosen(p->platform)) continue;
            served = true;
            Candidate c{ &row, p, p->priority, true, {} };
            if (ctx.priority)
                if (auto n = ctx.priority(row, p->name)) c.priority = *n;
            if (!ctx.family.empty() && !p->pairs.empty()
                && std::ranges::find(p->pairs, ctx.family) == p->pairs.end()) {
                c.available = false;
                std::string pairs;
                for (auto f : p->pairs) pairs += std::format("{}{}", pairs.empty() ? "" : ", ", f);
                c.why = std::format("{} pairs with {}, not {}", p->name, pairs, ctx.family);
            } else if (!available_on(*p, ctx.host, row.arch)) {
                c.available = false;
                c.why = std::format("{} is not available on {}", p->name, ctx.host);
            } else if (!p->probe.empty() && !(ctx.probe && ctx.probe(p->probe))) {
                c.available = false;
                c.why = std::format("{} is not installed here", p->name);
            } else if (row.tier == "planned") {
                c.available = false;
                c.why = std::format("{} is planned", row.triple);
            }
            s.candidates.push_back(std::move(c));
        }
        // A row no provider serves (a platform's row before its provider is
        // read, or a provider chosen by name that does not serve it) is still a
        // candidate when nothing was chosen by name: the platform serves it.
        if (!served && ctx.provider.empty())
            s.candidates.push_back(Candidate{ &row, nullptr, 0, true, {} });
    }
    std::ranges::stable_sort(s.candidates, [&](const Candidate& a, const Candidate& b) {
        if (a.priority != b.priority) return a.priority > b.priority;
        const auto ra = std::ranges::find(reached, a.row) - reached.begin();
        const auto rb = std::ranges::find(reached, b.row) - reached.begin();
        if (ra != rb) return ra < rb;
        const auto na = a.provider ? a.provider->name : std::string_view{};
        const auto nb = b.provider ? b.provider->name : std::string_view{};
        return na < nb;
    });
    if (s.candidates.empty()) return s;
    auto chosen = std::ranges::find_if(s.candidates, [](const Candidate& c) { return c.available; });
    if (chosen == s.candidates.end()) chosen = s.candidates.begin();
    s.row = chosen->row;
    s.provider = chosen->provider;
    s.priority = chosen->priority;
    s.fallback = chosen != s.candidates.begin();
    return s;
}

} // namespace mcpp::toolchain::target
