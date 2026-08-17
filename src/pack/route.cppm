// mcpp.pack.route — which target `mcpp pack` packs, and therefore which of the
// two pipelines runs.
//
// The routing question has exactly one input: `[targets.<n>].kind`. A program
// becomes an application bundle, a library becomes a library package. That is
// why there is no `--lib` flag and no `--artifact static|shared` — every one
// of those would be a second place to answer a question the manifest already
// answers, and the two answers could then disagree.
//
// Reading the manifest here (rather than inside each pipeline) keeps the
// decision ahead of the build: `mcpp pack nosuch` should say so in
// milliseconds, not after compiling the project.

export module mcpp.pack.route;

import std;
import mcpp.manifest;
import mcpp.project;

export namespace mcpp::pack {

struct PackRoute {
    std::string targetName;
    bool        library = false;   // kind = lib | shared
};

// Resolve `requested` (possibly empty) against the current project.
//
// Empty picks the only packable target. A project with both a program and a
// library has no single obvious answer, so it is asked rather than guessed:
// packing the wrong one produces a plausible-looking archive of the wrong
// shape, which is worse than an error.
std::expected<PackRoute, std::string> route_pack_target(std::string_view requested);

} // namespace mcpp::pack

namespace mcpp::pack {

std::expected<PackRoute, std::string> route_pack_target(std::string_view requested) {
    auto root = mcpp::project::find_manifest_root(std::filesystem::current_path());
    if (!root) return std::unexpected("no mcpp.toml in current dir or parents");
    auto m = mcpp::manifest::load(*root / "mcpp.toml");
    if (!m) return std::unexpected(m.error().format());

    auto is_library = [](const mcpp::manifest::Target& t) {
        return t.kind == mcpp::manifest::Target::Library
            || t.kind == mcpp::manifest::Target::SharedLibrary;
    };
    auto kind_name = [](const mcpp::manifest::Target& t) -> std::string_view {
        switch (t.kind) {
            case mcpp::manifest::Target::Binary:        return "bin";
            case mcpp::manifest::Target::Library:       return "lib";
            case mcpp::manifest::Target::SharedLibrary: return "shared";
            case mcpp::manifest::Target::TestBinary:    return "test";
        }
        return "?";
    };

    if (!requested.empty()) {
        for (auto const& t : m->targets) {
            if (t.name != requested) continue;
            if (t.kind == mcpp::manifest::Target::TestBinary)
                return std::unexpected(std::format(
                    "target '{}' is a test binary; there is nothing to distribute",
                    requested));
            return PackRoute{ t.name, is_library(t) };
        }
        std::string list;
        for (auto const& t : m->targets) {
            if (!list.empty()) list += ", ";
            list += std::format("{} ({})", t.name, kind_name(t));
        }
        return std::unexpected(std::format(
            "no target named '{}'{}{}", requested,
            list.empty() ? "" : "; this package declares: ", list));
    }

    // A WORKSPACE ROOT has no targets of its own — a virtual one has no
    // `[package]` at all. `mcpp pack` there has always meant "pack the member",
    // and the application pipeline is what resolves which member that is. So
    // hand it straight through rather than reading the root's (empty) target
    // list and concluding there is nothing to pack.
    //
    // Found by running the old binary and the new one against
    // examples/04-workspace: the routing added here turned a working command
    // into "this package declares no program and no library to pack".
    if (m->targets.empty() && m->workspace.present) return PackRoute{ {}, false };

    // Nothing requested. A program is still the default — `mcpp pack` has
    // always meant "bundle this application" and a project that has one is
    // asking for that.
    const mcpp::manifest::Target* onlyLib = nullptr;
    std::size_t libCount = 0;
    for (auto const& t : m->targets) {
        if (t.kind == mcpp::manifest::Target::Binary) return PackRoute{ t.name, false };
        if (is_library(t)) { onlyLib = &t; ++libCount; }
    }
    if (libCount == 1) return PackRoute{ onlyLib->name, true };
    if (libCount == 0)
        return std::unexpected("this package declares no program and no library to pack");

    std::string list;
    for (auto const& t : m->targets) {
        if (!is_library(t)) continue;
        if (!list.empty()) list += ", ";
        list += std::format("{} ({})", t.name, kind_name(t));
    }
    return std::unexpected(std::format(
        "this package declares more than one library, so `mcpp pack` cannot pick "
        "one for you.\n  Name it: {}", list));
}

} // namespace mcpp::pack
