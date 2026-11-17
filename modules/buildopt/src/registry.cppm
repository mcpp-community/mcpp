// mcpp.buildopt.registry — the register of what mcpp does beyond the standard
// build (SPEC-019).
//
// ONE TABLE. An entry states what it is, the seam of the pipeline it acts at,
// its level, whether it is on by default and what states otherwise. Nothing
// mcpp does to a build beyond the compiler's and ninja's own behaviour is
// outside this table; `mcpp explain` and the package's README list it.
//
// THE LEVEL DECIDES THE DEFAULT, NOT THE ENTRY:
//
//   Exact        the products are the bytes the standard build gives. On, and
//                it has no switch of its own: there is nothing to choose.
//   Conditional  the program is the same; something beside it may differ (the
//                lines an importer names). On, under the guards the entry
//                states, with a switch.
//   Shape        the graph or its processes take another shape; the products
//                are the same, the ways to fail are new. Off until it has run
//                on every host, with a switch.
//   Source       a source is built as other units than it is written as. With
//                a switch; on where the entry falls back to the source as
//                written whenever the units do not build.
//
// ONE STATEMENT TURNS ALL OF IT OFF: `MCPP_BUILDOPT=off` gives the standard
// build, which is how a product is compared with what any entry made of it.

export module mcpp.buildopt.registry;

import std;
import mcpp.invocation;

export namespace mcpp::buildopt::registry {

enum class Level { Exact, Conditional, Shape, Source };
enum class Seam  { Source, Graph, Edge };

constexpr std::string_view to_string(Level l) {
    switch (l) {
        case Level::Exact:       return "exact";
        case Level::Conditional: return "conditional";
        case Level::Shape:       return "shape";
        case Level::Source:      return "source";
    }
    return "exact";
}
constexpr std::string_view to_string(Seam s) {
    switch (s) {
        case Seam::Source: return "source";
        case Seam::Graph:  return "graph";
        case Seam::Edge:   return "edge";
    }
    return "edge";
}

struct Entry {
    std::string_view id;
    std::string_view what;      // one sentence
    Seam             seam;
    Level            level;
    bool             byDefault;
    std::string_view key;       // the manifest key that states otherwise; empty: none
    std::string_view env;       // the same for one invocation
    std::string_view values;    // the key's values, the default first
};

inline constexpr std::string_view kKeepBmi         = "keep-bmi";
inline constexpr std::string_view kKeepObject      = "keep-object";
inline constexpr std::string_view kGcmStamps       = "gcm-stamps";
inline constexpr std::string_view kGcmLocations    = "gcm-locations";
inline constexpr std::string_view kScanPass        = "scan-pass";
inline constexpr std::string_view kSplitSchedule   = "split-schedule";
inline constexpr std::string_view kPrivateFragment = "private-fragment";

inline constexpr std::array<Entry, 7> kEntries{{
    {kKeepBmi, "a BMI compiled to the bytes it had is kept, and its importers are not compiled",
     Seam::Edge, Level::Exact, true, {}, {}, {}},
    {kKeepObject, "an object compiled to the bytes it had is kept, and nothing is archived or linked for it",
     Seam::Edge, Level::Exact, true, {}, {}, {}},
    {kGcmStamps, "the two wall-clock stamps of a GCC BMI are not a difference",
     Seam::Edge, Level::Exact, true, {}, {}, {}},
    {kGcmLocations, "a GCC BMI whose line maps alone moved is kept; its importers name the earlier lines",
     Seam::Edge, Level::Conditional, true, "[profile.<name>] bmi_cutoff", "MCPP_BMI_CUTOFF",
     "ignore-locations | exact"},
    {kScanPass, "the scans of a graph run as a pass of their own before its compiles",
     Seam::Graph, Level::Exact, true, {}, {}, {}},
    {kSplitSchedule, "importers start when a BMI exists, not when its compiler exits (two-phase, detached code generation)",
     Seam::Graph, Level::Shape, false, "[build] bmi_schedule", "MCPP_BMI_SCHEDULE", "auto | on | off"},
    {kPrivateFragment, "a private module fragment is built as a unit of its own, so an edit of it reaches no importer",
     Seam::Source, Level::Source, true, "[build] private_fragment", "MCPP_PRIVATE_FRAGMENT",
     "unit | as-written"},
}};

constexpr const Entry* find(std::string_view id) {
    for (auto const& e : kEntries)
        if (e.id == id) return &e;
    return nullptr;
}

// The statement that turns every entry off for one invocation.
inline constexpr std::string_view kStandardEnv = "MCPP_BUILDOPT";

// Whether this invocation asks for the standard build (`MCPP_BUILDOPT=off`).
// The one impure function here: the statement is read in one place.
bool standard_only();

// The table as `mcpp explain` and the README print it: one row an entry.
std::string table();

} // namespace mcpp::buildopt::registry

namespace mcpp::buildopt::registry {

bool standard_only() {
    const auto stated = mcpp::invocation::get(kStandardEnv);
    return stated && *stated == "off";
}

std::string table() {
    std::string out = "| Entry | Seam | Level | Default | Stated otherwise by | What |\n|---|---|---|---|---|---|\n";
    for (auto const& e : kEntries) {
        std::string by;
        if (!e.key.empty()) by = std::format("`{}`, `{}` (`{}`)", e.key, e.env, e.values);
        else by = std::format("`{}=off` alone", kStandardEnv);
        out += std::format("| `{}` | {} | {} | {} | {} | {} |\n", e.id, to_string(e.seam), to_string(e.level),
                           e.byDefault ? "on" : "off", by, e.what);
    }
    return out;
}

} // namespace mcpp::buildopt::registry
