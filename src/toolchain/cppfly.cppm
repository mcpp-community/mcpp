// mcpp.toolchain.cppfly — the `standard = "c++fly"` capability: per-compiler
// support & mapping for "latest -std= level + every enableable experimental
// gate (language + stdlib)". Pure data tables + query functions, the fourth
// sibling of CommandDialect / BmiTraits / ProviderCapabilities — and the
// first consumer of Toolchain::version for gating.
//
// Also owns the "latest level for this toolchain" table, which fixes
// standard = "c++latest" reaching the GNU driver as the invalid spelling
// -std=c++latest (design §11-Q5).
//
// Table facts pinned by on-machine probes (gcc 16.1.0, 2026-07-14):
// contracts are enabled by -std=c++26 alone (__cpp_contracts defined);
// reflection additionally needs -freflection (__cpp_impl_reflection).
// See .agents/docs/2026-07-14-std-features-experimental-gate-design.md.

export module mcpp.toolchain.cppfly;

import std;
import mcpp.toolchain.dialect;
import mcpp.toolchain.model;

export namespace mcpp::toolchain::cppfly {

struct FeatureState {
    std::string name;      // "reflection"
    std::string paper;     // "P2996" — summary/diagnostics
    std::string flags;     // enabled: extra flags ("" = enabled by std level alone)
    std::string reason;    // skipped: why
    bool enabled = false;
};

struct Resolution {
    std::string stdCanonical;               // "c++26" / "c++2c" / "c++23"
    int stdLevel = 26;
    std::vector<std::string> flags;         // union of enabled gate flags, deduped
    std::vector<FeatureState> features;     // enabled + skipped, declaration order
};

// Leading integer of Toolchain::version ("16.1.0" → 16; unparsable → 0).
int compiler_major(const Toolchain& tc);

// Latest -std= canonical the resolved toolchain accepts. Used for both
// c++fly and c++latest (the raw canonicals are not valid -std= spellings).
std::string latest_std_canonical(const Toolchain& tc, int* levelOut = nullptr);

// The full c++fly answer for a toolchain: latest level + every gate it
// supports (enabled) and every gate it lacks (skipped, with reason).
Resolution resolve(const Toolchain& tc);

// The dialect-complete std flag for any normalized standard, including
// c++latest/c++fly which resolve per-toolchain. Plain canonicals delegate
// to std_flag_for unchanged.
std::string std_flag(const Toolchain& tc, std::string_view canonical, int level);

// Graph-global dialect flags: manifest-declared ∪ (experimental ? fly gate
// flags : ∅), deduped, declaration order kept. The ONE merge both consumers
// (BuildPlan::dialectFlags and prepare's stdFlagAndDialect) go through so
// scan-time, prebuild-time and compile-time dialect provably agree.
std::vector<std::string> effective_dialect_flags(const Toolchain& tc,
    bool experimental, std::vector<std::string> manifestDialectFlags);

} // namespace mcpp::toolchain::cppfly

namespace mcpp::toolchain::cppfly {

namespace {

// ── Table 1: family × min major → latest supported -std= canonical ──────
// First matching row wins (rows per family ordered newest-first).
struct LatestStdRule {
    CompilerId family;
    int minMajor;
    std::string_view canonical;
    int level;
};
constexpr LatestStdRule kLatestStd[] = {
    { CompilerId::GCC,   14, "c++26", 26 },
    { CompilerId::GCC,    0, "c++23", 23 },
    { CompilerId::Clang, 17, "c++2c", 26 },
    { CompilerId::Clang,  0, "c++23", 23 },
    // MSVC has no /std:c++26 — level > 20 is spelled /std:c++latest by
    // std_flag_for; the canonical here only feeds diagnostics.
    { CompilerId::MSVC,   0, "c++26", 26 },
};

// ── Table 2: experimental language-feature gates (version-gated) ────────
// A family with no row = unsupported. flags == "" = supported at the fly
// std level with no extra gate flag (still reported for the summary).
struct GateRule {
    CompilerId family;
    int minMajor;
    std::string_view flags;
};
constexpr GateRule kReflectionRules[] = {
    { CompilerId::GCC, 16, "-freflection" },
};
constexpr GateRule kContractsRules[] = {
    { CompilerId::GCC, 16, "" },
};
struct Gate {
    std::string_view name;
    std::string_view paper;
    std::span<const GateRule> rules;
};
constexpr Gate kGates[] = {
    { "reflection", "P2996", kReflectionRules },
    { "contracts",  "P2900", kContractsRules  },
};

// ── Table 3: stdlib experimental gates (stdlib dimension) ───────────────
struct StdlibGateRule {
    std::string_view name;
    std::string_view stdlibId;
    std::string_view flags;
};
constexpr StdlibGateRule kStdlibGates[] = {
    // libstdc++ and MSVC STL ship their unstable bits ungated; libc++ hides
    // them behind -fexperimental-library.
    { "experimental-library", "libc++", "-fexperimental-library" },
};

void add_unique(std::vector<std::string>& v, std::string_view f) {
    if (f.empty()) return;
    if (std::find(v.begin(), v.end(), f) == v.end()) v.emplace_back(f);
}

} // namespace

int compiler_major(const Toolchain& tc) {
    int major = 0;
    bool any = false;
    for (char c : tc.version) {
        if (c < '0' || c > '9') break;
        major = major * 10 + (c - '0');
        any = true;
    }
    return any ? major : 0;
}

std::string latest_std_canonical(const Toolchain& tc, int* levelOut) {
    const int major = compiler_major(tc);
    for (auto& r : kLatestStd) {
        if (r.family != tc.compiler) continue;
        if (major >= r.minMajor) {
            if (levelOut) *levelOut = r.level;
            return std::string(r.canonical);
        }
    }
    if (levelOut) *levelOut = 26;   // unknown family: newest ratified level
    return "c++26";
}

Resolution resolve(const Toolchain& tc) {
    Resolution r;
    r.stdCanonical = latest_std_canonical(tc, &r.stdLevel);
    const int major = compiler_major(tc);
    for (auto& g : kGates) {
        FeatureState st;
        st.name = std::string(g.name);
        st.paper = std::string(g.paper);
        const GateRule* hit = nullptr;
        for (auto& rule : g.rules) {
            if (rule.family == tc.compiler) { hit = &rule; break; }
        }
        if (!hit) {
            st.reason = std::format("{}: unsupported", tc.compiler_name());
        } else if (major < hit->minMajor) {
            st.reason = std::format("{} {} < {}", tc.compiler_name(), major, hit->minMajor);
        } else {
            st.enabled = true;
            st.flags = std::string(hit->flags);
            add_unique(r.flags, hit->flags);
        }
        r.features.push_back(std::move(st));
    }
    for (auto& sg : kStdlibGates) {
        FeatureState st;
        st.name = std::string(sg.name);
        st.paper = "stdlib";
        if (tc.stdlibId == sg.stdlibId) {
            st.enabled = true;
            st.flags = std::string(sg.flags);
            add_unique(r.flags, sg.flags);
        } else {
            st.reason = std::format("stdlib is {}, gate applies to {}",
                tc.stdlibId.empty() ? "unknown" : tc.stdlibId, sg.stdlibId);
        }
        r.features.push_back(std::move(st));
    }
    return r;
}

std::string std_flag(const Toolchain& tc, std::string_view canonical, int level) {
    std::string resolvedCanonical(canonical);
    int resolvedLevel = level;
    // c++latest (999) / c++fly (1000): resolve to the family's real latest
    // level — the raw canonical is not a valid -std= spelling on GNU.
    if (level >= 999) resolvedCanonical = latest_std_canonical(tc, &resolvedLevel);
    return std_flag_for(dialect_for(tc), resolvedCanonical, resolvedLevel);
}

std::vector<std::string> effective_dialect_flags(const Toolchain& tc,
    bool experimental, std::vector<std::string> manifestDialectFlags)
{
    if (!experimental) return manifestDialectFlags;
    for (auto& f : resolve(tc).flags) add_unique(manifestDialectFlags, f);
    return manifestDialectFlags;
}

} // namespace mcpp::toolchain::cppfly
