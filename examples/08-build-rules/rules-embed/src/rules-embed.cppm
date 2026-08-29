// A rule that turns a data file into an object the link consumes.
//
// The module name is what THIS FILE declares; the package is called
// `rules-embed`, which is not a legal C++ module name and no longer has to be.
export module example.rules.embed;

import std;
import mcpp;

export namespace example::rules::embed {

struct options {
    // Where generated sources and objects go. Defaults to the build output
    // directory, which is the only place a rule may write.
    std::string out_dir = std::string(mcpp::out_dir());
};

// One planned edge, handed back so a caller that needs to adjust it can.
// A rule without this pair has a cliff: past its last knob the only way out is
// to hand-write the action, and that copy then drifts.
struct edge {
    std::string              id;
    std::string              description;
    std::vector<std::string> command;
    std::vector<std::string> inputs;
    std::vector<std::string> outputs;
};

// `<name>` is the symbol prefix; `<file>` is read at BUILD time, not now.
inline std::vector<edge> plan(std::span<const std::string> files,
                              options opt = {})
{
    const std::string root = mcpp::manifest_dir();
    if (root.empty()) {
        std::println(std::cerr,
            "example.rules.embed: no mcpp build context — this runs from build.mcpp");
        return {};
    }
    std::vector<edge> out;
    for (auto const& f : files) {
        const std::string stem = std::filesystem::path(f).stem().string();
        const std::string gen  = opt.out_dir + "/embed_" + stem + ".cpp";
        out.push_back(edge{
            .id          = "embed:" + stem,
            // What the engine cannot say: WHICH KNOBS produced this command.
            // The argv itself is already in build.ninja and `ninja -t commands`
            // recovers it, so repeating it here would only drift.
            .description = "embed " + f + " as " + stem + "_data",
            .command     = { "sh", (root + "/tools/embed.sh"), (root + "/" + f),
                             gen, stem },
            .inputs      = { root + "/" + f },
            .outputs     = { gen },
        });
    }
    return out;
}

// Hand the planned edges to mcpp. Split from `plan` so a caller can edit the
// list in between; `generate` below is exactly the composition of the two.
inline bool submit(std::span<const edge> edges) {
    if (edges.empty()) return false;
    for (auto const& e : edges) {
        mcpp::action a;
        a.id          = e.id.c_str();
        // `source`, not `object`: this rule generates C++ that the compile set
        // takes. A rule that emitted a .o directly would use role = "object";
        // the choice is about what the output IS, not about how it is made.
        a.role        = "source";
        a.description = e.description.c_str();
        for (auto const& c : e.command) a.arg(c.c_str());
        for (auto const& i : e.inputs)  a.input(i.c_str());
        for (auto const& o : e.outputs) a.output(o.c_str());
        a.submit();
    }
    return true;
}

inline bool generate(std::span<const std::string> files, options opt = {}) {
    auto edges = plan(files, std::move(opt));
    return submit(edges);
}

} // namespace example::rules::embed
