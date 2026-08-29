// Analysis as graph nodes, one per file.
//
// `role = "check"` runs BESIDE compilation. Serialising a whole build behind a
// linter costs more than it saves, and a failing check fails the build either
// way; `blocking = true` is for the case where a failure means the compile was
// wasted anyway.
//
// A check's output is a stamp and the command must create it. That is the
// ergonomic gap this package exists to close: clang-tidy does not write a
// stamp, so without a rule every project writes the same wrapper script.
export module example.rules.tidy;

import std;
import mcpp;
import example.rules.embed;   // a rule importing a rule

export namespace example::rules::tidy {

struct options {
    // The checker to run. Defaults to the bundled script so the example runs
    // anywhere; point it at clang-tidy for a real project.
    std::string program;
    // clang-tidy's `-p` wants this. It is a closed engine variable, expanded
    // by mcpp, so the rule never has to know the build directory's layout.
    bool pass_compile_db = false;
    // Gate compilation on the result rather than running alongside it.
    bool blocking = false;
    std::string out_dir = std::string(mcpp::out_dir());
};

struct edge {
    std::string              id;
    std::string              description;
    std::vector<std::string> command;
    std::vector<std::string> inputs;
    std::vector<std::string> outputs;
    bool                     blocking = false;
};

inline std::vector<edge> plan(std::span<const std::string> files,
                              options opt = {})
{
    const std::string root = mcpp::manifest_dir();
    if (root.empty()) {
        std::println(std::cerr,
            "example.rules.tidy: no mcpp build context — this runs from build.mcpp");
        return {};
    }
    if (files.empty()) {
        std::println(std::cerr, "example.rules.tidy: no files to check");
        return {};
    }
    const std::string program = opt.program.empty()
        ? root + "/tools/check.sh"
        : opt.program;

    std::vector<edge> out;
    for (auto const& f : files) {
        std::string stem = std::filesystem::path(f).stem().string();
        const std::string stamp = opt.out_dir + "/tidy/" + stem + ".stamp";
        std::vector<std::string> cmd{ "sh", program, stamp, root + "/" + f };
        if (opt.pass_compile_db) {
            cmd.push_back("-p");
            cmd.push_back("${mcpp.compile_db}");
        }
        out.push_back(edge{
            .id          = "tidy:" + stem,
            .description = std::string("check ") + f
                         + (opt.blocking ? " (blocking)" : " (parallel)"),
            .command     = std::move(cmd),
            .inputs      = { root + "/" + f },
            .outputs     = { stamp },
            .blocking    = opt.blocking,
        });
    }
    return out;
}

inline bool submit(std::span<const edge> edges) {
    if (edges.empty()) return false;
    for (auto const& e : edges) {
        mcpp::action a;
        a.id          = e.id.c_str();
        a.role        = "check";
        a.description = e.description.c_str();
        a.blocking    = e.blocking;
        for (auto const& c : e.command) a.arg(c.c_str());
        for (auto const& i : e.inputs)  a.input(i.c_str());
        for (auto const& o : e.outputs) a.output(o.c_str());
        a.submit();
    }
    return true;
}

inline bool check(std::span<const std::string> files, options opt = {}) {
    auto edges = plan(files, std::move(opt));
    return submit(edges);
}

} // namespace example::rules::tidy
