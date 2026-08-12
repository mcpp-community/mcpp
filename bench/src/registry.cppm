// bench.registry — the one list of engines.
//
// Adding an engine is: write bench.engines.<name>, then add ONE line here.
// Nothing else in the suite — runner, protocol, scenarios, CI — changes.
export module bench.registry;

import std;
import bench.engines.engine;
import bench.engines.mcpp;
import bench.engines.cmake;
import bench.engines.xmake;
import bench.engines.meson;
import bench.engines.bazel;

export namespace bench {

// Order is the order results are reported in, so it is chosen for reading:
// the two mcpp variants adjacent (they are the before/after pair), then the
// other engines by how completely they support modules.
inline std::vector<std::unique_ptr<engines::Engine>> all_engines() {
    std::vector<std::unique_ptr<engines::Engine>> v;
    v.push_back(engines::make_mcpp());
    v.push_back(engines::make_mcpp_opt());
    v.push_back(engines::make_cmake());
    v.push_back(engines::make_xmake());
    v.push_back(engines::make_meson());
    v.push_back(engines::make_bazel());
    return v;
}

inline std::vector<std::string> engine_names() {
    std::vector<std::string> names;
    for (const auto& e : all_engines()) names.emplace_back(e->name());
    return names;
}

}  // namespace bench
