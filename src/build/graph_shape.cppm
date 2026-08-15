// mcpp.build.graph_shape — what kind of graph a build.ninja holds, written
// into the file and read back out of it.
//
// `target/<triple>/<fp>/build.ninja` is SHARED MUTABLE STATE. Three modes
// write it — `mcpp build`, `mcpp test`, `mcpp build --configure-only` — and
// they land in the same directory because the fingerprint covers neither
// dev-dependencies nor test targets. But `emit_ninja_string`'s `default` line
// lists every link unit in the plan, and in test mode that is ONLY the test
// binaries: the package's own target is not in the graph at all.
//
// The fast path used to compare build.ninja's mtime against the SOURCES and
// nothing else, so `build → test → build` replayed the test graph, linked the
// tests, never linked the target, and printed `Finished`. Breaking a test file
// then failed a plain `mcpp build` with `src/` untouched.
//
// WHY THE FILE AND NOT THE CACHE. The first repair for this (mcpp#387) was on
// the WRITE side: the mode that rewrites the graph drops the fast-path entry
// afterwards. That works, and it has to be repeated by every future mode that
// rewrites build.ninja — the same decision derived in a new place each time,
// which is how the `mcpp test` half stayed broken after the `--configure-only`
// half was fixed. A READ-side invariant only has to hold once: the fast path
// checks the graph it is about to replay.
//
// Putting the shape in build.ninja rather than in `.build_cache` is the same
// argument one level down. Two files can disagree; a file that describes
// itself cannot. The single producer of build.ninja is also the single writer
// of this line.

export module mcpp.build.graph_shape;

import std;

export namespace mcpp::build {

enum class GraphShape {
    // What a plain `mcpp build` generates: the package's own targets.
    Normal,
    // Dev-dependencies and synthetic test targets are in the plan, so `default`
    // names the test binaries. Produced by `mcpp test` and by
    // `mcpp build --configure-only`.
    WithTests,
};

std::string_view to_string(GraphShape shape) {
    return shape == GraphShape::WithTests ? "test" : "normal";
}

// The marker line, without its newline. A ninja comment, so it costs nothing
// and older ninja versions do not care.
// `scheduleTag` names the SHAPE OF THE MODULE EDGES (see
// mcpp.build.schedule.policy): "none", "two-phase", "detach-codegen". It rides
// the same line for the same reason the shape does — build.ninja is shared
// mutable state and the fast path replays it, so a graph built under one
// schedule must not be replayed under another. Flipping the switch has to
// invalidate the graph, and the only way that cannot be forgotten is if the
// graph says which schedule produced it.
std::string header_line(GraphShape shape, std::string_view scheduleTag) {
    return std::format("# mcpp:graph={};schedule={}", to_string(shape), scheduleTag);
}

// Read the shape back. `nullopt` means "this file does not say" — a build.ninja
// written before this line existed, an unreadable file, or something that is
// not a mcpp graph at all. Callers must treat that as a MISS, never as
// `Normal`: the whole point is that an unlabelled graph is exactly the case
// that used to be replayed blind.
std::optional<GraphShape> read_shape(const std::filesystem::path& ninjaPath) {
    std::ifstream input(ninjaPath);
    if (!input) return std::nullopt;
    // The marker is written first, but read a few lines anyway so a future
    // banner above it does not silently turn every build into a full prepare.
    std::string line;
    for (int i = 0; i < 8 && std::getline(input, line); ++i) {
        constexpr std::string_view prefix = "# mcpp:graph=";
        if (!line.starts_with(prefix)) continue;
        auto value = std::string_view(line).substr(prefix.size());
        while (!value.empty() && (value.back() == '\r' || value.back() == ' '))
            value.remove_suffix(1);
        // `graph=<shape>[;schedule=<tag>]`. Split before comparing, so adding
        // the schedule field does not turn every existing graph into "unknown".
        if (const auto semi = value.find(';'); semi != std::string_view::npos)
            value = value.substr(0, semi);
        if (value == "normal") return GraphShape::Normal;
        if (value == "test")   return GraphShape::WithTests;
        // A shape this binary does not know is not `Normal`. An older mcpp
        // meeting a newer graph must fall back, not guess.
        return std::nullopt;
    }
    return std::nullopt;
}

// The schedule tag this graph was written with. Empty means the file predates
// the field — which is NOT the same as "none": an unlabelled graph is exactly
// the case that must not be replayed blind, so callers compare and miss.
std::string read_schedule(const std::filesystem::path& ninjaPath) {
    std::ifstream input(ninjaPath);
    if (!input) return {};
    std::string line;
    for (int i = 0; i < 8 && std::getline(input, line); ++i) {
        constexpr std::string_view prefix = "# mcpp:graph=";
        if (!line.starts_with(prefix)) continue;
        auto value = std::string_view(line).substr(prefix.size());
        while (!value.empty() && (value.back() == '\r' || value.back() == ' '))
            value.remove_suffix(1);
        const auto semi = value.find(';');
        if (semi == std::string_view::npos) return {};
        auto rest = value.substr(semi + 1);
        constexpr std::string_view schedPrefix = "schedule=";
        if (!rest.starts_with(schedPrefix)) return {};
        return std::string(rest.substr(schedPrefix.size()));
    }
    return {};
}

// The one question every fast path asks.
//
// It deliberately does NOT compare the schedule tag. The fast paths run BEFORE
// a plan exists, so they have no toolchain to derive the expected schedule
// from — and passing one in would mean deriving the same decision a second
// time, in a place that cannot see the compiler.
//
// Instead the schedule SWITCH is part of the toolchain fingerprint, so flipping
// it lands in a different build directory: a graph written under one schedule
// is structurally unreachable from a build configured with another. The tag on
// the line is then for humans and for `mcpp explain`, not for invalidation.
bool is_plain_build_graph(const std::filesystem::path& ninjaPath) {
    return read_shape(ninjaPath) == GraphShape::Normal;
}

} // namespace mcpp::build
