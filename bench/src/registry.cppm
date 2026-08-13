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

// A spec may carry ENGINE OPTIONS in brackets: `mcpp[schedule=on]=/path/to/mcpp`.
//
// This exists for opt-in behaviour. mcpp's split build schedule is a key in the
// MEASURED PROJECT's manifest, and the measured projects are pinned workloads —
// one of them belongs to someone else — so the suite had no way to reach the
// largest cold-build change in the release it was benchmarking, and reported
// "no improvement" for something worth 2.29x.
//
// Bracket options become environment variables for that engine's child only, so
// both arms sit in one report against one baseline on one machine. Unbracketed
// specs are untouched, and an unknown option is an error rather than a silently
// ignored word — a benchmark that quietly measures the default when you asked
// for the option is the exact failure this is meant to remove.
inline std::optional<std::pair<std::string, std::string>> engine_option(
    std::string_view engine, std::string_view key, std::string_view value) {
    if (engine == "mcpp" && key == "schedule")
        return std::pair{std::string("MCPP_BMI_SCHEDULE"), std::string(value)};
    return std::nullopt;
}

inline std::unique_ptr<engines::Engine> make_engine(std::string_view spec) {
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

// The default set, used when --engines is omitted. Order is the reporting order,
// chosen for reading: mcpp first (the subject), then the others.
inline std::vector<std::string> default_engine_specs() {
    return {"mcpp", "cmake", "xmake", "bazel"};
}

}  // namespace bench
