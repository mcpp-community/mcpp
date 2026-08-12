// bench.registry — turning `--engines` text into engine objects.
//
// Adding an engine is: write bench.engines.<name>, then add ONE line to
// `make_engine`. Nothing else in the suite — runner, protocol, scenarios, CI —
// changes.
//
// A spec is either a bare name (`cmake`) or `name=program` (`mcpp=/path/to/mcpp`).
// The second form is what makes "is the new release faster?" a normal query:
//
//     --engines mcpp=/usr/bin/mcpp,mcpp=./target/x86_64-linux-gnu/*/bin/mcpp
//
// registers two mcpp engines that label themselves from the version each binary
// reports, so the two rows never collapse into one.
export module bench.registry;

import std;
import bench.engines.engine;
import bench.engines.mcpp;
import bench.engines.cmake;
import bench.engines.xmake;
import bench.engines.meson;
import bench.engines.bazel;

export namespace bench {

inline std::unique_ptr<engines::Engine> make_engine(std::string_view spec) {
    std::string name(spec);
    std::string program;
    if (const auto eq = spec.find('='); eq != std::string_view::npos) {
        name    = std::string(spec.substr(0, eq));
        program = std::string(spec.substr(eq + 1));
    }

    if (name == "mcpp")  return engines::make_mcpp(program.empty() ? "mcpp" : program);
    if (name == "cmake") return engines::make_cmake();
    if (name == "xmake") return engines::make_xmake();
    if (name == "meson") return engines::make_meson();
    if (name == "bazel") return engines::make_bazel();
    return nullptr;
}

// The default set, used when --engines is omitted. Order is the reporting order,
// chosen for reading: mcpp first (the subject), then the others.
inline std::vector<std::string> default_engine_specs() {
    return {"mcpp", "cmake", "xmake", "meson", "bazel"};
}

}  // namespace bench
