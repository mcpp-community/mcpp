// mcpp.build.flag_origins — every word a unit is compiled with and its link
// line, each with the layer that contributed it (mcpp#911 G2; SPEC-016 §5.2).
//
// The command lines are the build's own (`unit_invocations`, `compute_flags`),
// so what is explained is what runs. A unit's line has three parts the engine
// writes in order: the package's include directories, the configuration's
// flags, and the package's own flags; the first and the last are the
// package's by construction. The configuration's flags are one string the
// engine composes from the row, the platform, the toolchain and its own
// conventions, and a word of it is attributed to the first of these that
// gives it: the row's machine description (after the replacements stated for
// it), the platform's `[platform.build]` and `[platform.link]`, a path inside
// the toolchain's payload, and otherwise the engine. A word two layers give
// is the first one's.

export module mcpp.build.flag_origins;

import std;
import nlohmann.json;
import mcpp.build.plan;

export namespace mcpp::build {

struct FlagWord {
    std::string word;
    std::string layer;    // compiler | row | replace | platform | toolchain | package | engine
    std::string detail;   // the row, the statement, the package, the toolchain
};

struct UnitFlagOrigins {
    std::string           file;
    std::string           package;
    std::vector<FlagWord> words;
};

struct FlagOrigins {
    std::string                  identity;
    std::vector<UnitFlagOrigins> units;
    std::vector<FlagWord>        link;   // the configuration's link flags
};

// The units whose source path ends with `unit` (all of the root package's when
// empty), and the link flags.
FlagOrigins flag_origins(const BuildPlan& plan, std::string_view unit);

nlohmann::json flag_origins_json(const FlagOrigins& o);

} // namespace mcpp::build

