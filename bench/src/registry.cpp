// bench.registry — implementation.
//
// `module bench.registry;` with no `export`: an implementation unit, so nothing below
// reaches an importer's BMI. This is the one module that knows every engine,
// so keeping its bodies out of the BMI stops a change to any single engine
// from rippling through the importer graph.
module bench.registry;

import std;
import bench.engines.engine;
import bench.engines.mcpp;
import bench.engines.cmake;
import bench.engines.xmake;
import bench.engines.bazel;

namespace bench {

std::string anchor_program(std::string program) {
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

std::optional<std::pair<std::string, std::string>> engine_option(
    std::string_view engine, std::string_view key, std::string_view value) {
    if (engine == "mcpp" && key == "schedule")
        return std::pair{std::string("MCPP_BMI_SCHEDULE"), std::string(value)};
    return std::nullopt;
}

std::unique_ptr<engines::Engine> make_engine(std::string_view spec) {
    std::string name(spec);
    std::string program;
    std::map<std::string, std::string> env;

    // The BRACKETS are parsed first, then `=program`. Order matters: the option
    // list contains `=` itself (`mcpp[schedule=on]=/path`), so splitting on the
    // first `=` yields the name `mcpp[schedule`, and the whole spec is rejected
    // as an unknown engine.
    std::string opts;
    if (const auto lb = spec.find('['); lb != std::string_view::npos) {
        const auto rb = spec.find(']', lb);
        if (rb == std::string_view::npos) return nullptr;   // unterminated: reject
        name = std::string(spec.substr(0, lb));
        opts = std::string(spec.substr(lb + 1, rb - lb - 1));
        auto rest = spec.substr(rb + 1);
        if (!rest.empty()) {
            if (rest.front() != '=') return nullptr;        // trailing junk: reject
            program = anchor_program(std::string(rest.substr(1)));
        }
    } else if (const auto eq = spec.find('='); eq != std::string_view::npos) {
        name    = std::string(spec.substr(0, eq));
        program = anchor_program(std::string(spec.substr(eq + 1)));
    }

    for (std::size_t at = 0; at <= opts.size();) {
        const auto end  = std::min(opts.find(',', at), opts.size());
        const auto item = std::string_view(opts).substr(at, end - at);
        at = end + 1;
        if (item.empty()) continue;
        const auto sep = item.find('=');
        if (sep == std::string_view::npos) return nullptr;
        auto mapped = engine_option(name, item.substr(0, sep), item.substr(sep + 1));
        if (!mapped) return nullptr;                  // unknown: reject loudly
        env.emplace(std::move(mapped->first), std::move(mapped->second));
    }

    if (name == "mcpp")
        return engines::make_mcpp(program.empty() ? "mcpp" : program, {}, std::move(env));
    if (!env.empty()) return nullptr;   // no other engine takes options yet
    if (name == "cmake") return engines::make_cmake();
    if (name == "xmake") return engines::make_xmake();
    if (name == "bazel") return engines::make_bazel();
    return nullptr;
}

std::vector<std::string> default_engine_specs() {
    return {"mcpp", "cmake", "xmake", "bazel"};
}

}  // namespace bench
