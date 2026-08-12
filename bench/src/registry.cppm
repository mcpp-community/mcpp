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

// Makes a program spec independent of the current directory.
//
// Every measured command runs with its cwd set to the project under test, so a
// relative `--engines mcpp=./mcpp-old` resolves against the FIXTURE rather than
// the shell the user typed it in. The spawn then fails with "could not start",
// which is reported per cell as `exited -1` — a whole matrix of failures whose
// cause is one missing `./`. Resolving here, once, at the only place a spec
// becomes an engine, removes the class of bug rather than documenting it.
//
// Bare names (`mcpp`, `cmake`) are left alone: those are PATH lookups, which the
// child performs itself and which cwd does not affect.
inline std::string anchor_program(std::string program) {
    if (program.empty()) return program;
    if (program.find('/') == std::string::npos &&
        program.find('\\') == std::string::npos)
        return program;                       // bare name → PATH, cwd-independent
    std::error_code ec;
    auto abs = std::filesystem::absolute(program, ec);
    if (ec) return program;                   // leave it; probe() will report it
    // weakly_canonical also collapses `..`, which absolute() keeps.
    auto canon = std::filesystem::weakly_canonical(abs, ec);
    return ec ? abs.string() : canon.string();
}

inline std::unique_ptr<engines::Engine> make_engine(std::string_view spec) {
    std::string name(spec);
    std::string program;
    if (const auto eq = spec.find('='); eq != std::string_view::npos) {
        name    = std::string(spec.substr(0, eq));
        program = anchor_program(std::string(spec.substr(eq + 1)));
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
