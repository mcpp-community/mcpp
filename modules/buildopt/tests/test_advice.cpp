// What a build says of a module that is one file: where the rebuild started,
// how many units followed, and when nothing is said.

import std;
import mcpplibs.mtest;
import mcpp.buildopt.advice;

using namespace mcpplibs::mtest;
namespace advice = mcpp::buildopt::advice;

namespace {
// `core` imported by five modules, each imported by `main`.
std::vector<advice::Unit> graph(bool withImplementation = false) {
    std::vector<advice::Unit> u{{"core.cppm", "core", {}, false}};
    std::vector<std::string> mids;
    for (int i = 0; i < 5; ++i) {
        const auto m = std::format("m{}", i);
        mids.push_back(m);
        u.push_back({m + ".cppm", m, {"core"}, false});
    }
    u.push_back({"main.cpp", "", mids, false});
    if (withImplementation) u.push_back({"core.cpp", "", {"core"}, true});
    return u;
}
} // namespace

UTest where_the_rebuild_started{"Advice/TheModuleTheRebuildStartedAtIsNamedWithItsImporters", [] {
    const auto found = advice::single_file_cascades(graph(), {"core.pcm", "m0.pcm", "m1.pcm"});
    expect(fatal(found.size() == 1u));
    expect(eq(found[0].module, std::string("core")));
    expect(eq(found[0].source, std::string("core.cppm")));
    expect(eq(found[0].importers, std::size_t{6}));   // five modules and main
    const auto line = advice::line(found[0]);
    expect(line.find("core.cppm is the whole of module core, which 6 units import") != std::string::npos) << line;
    expect(line.find("module :private;") != std::string::npos);
}};

UTest nothing_is_said{"Advice/NothingIsSaidOfAModuleWithAnImplementationUnitOrFewImporters", [] {
    // An implementation unit: the module is not one file. It is no importer either.
    expect(advice::single_file_cascades(graph(true), {"core.gcm"}).empty());
    // Few importers.
    expect(advice::single_file_cascades(graph(), {"m0.pcm"}).empty());
    // Nothing replaced.
    expect(advice::single_file_cascades(graph(), {}).empty());
    // A partition is not where a one-file module's advice applies.
    std::vector<advice::Unit> parts = graph();
    parts.push_back({"core-p.cppm", "core:p", {}, false});
    expect(advice::single_file_cascades(parts, {"core.pcm"}).empty());
}};

UTest said_once{"Advice/AnAdviceIsSaidOnceABuildDirectory", [] {
    const auto d = std::filesystem::temp_directory_path() / std::format("mcpp-advice-{}", std::random_device{}());
    std::filesystem::create_directories(d);
    expect(advice::first_time(d, "core"));
    expect(!advice::first_time(d, "core"));
    expect(advice::first_time(d, "other"));
    std::filesystem::remove_all(d);
}};
