// The rule for `.toy`.
//
// It contains no compiler. `toyc` is an ordinary mcpp package next door, and
// this module's whole job is to turn "the consumer listed some `.toy` files"
// into edges in the build graph -- ordered, fingerprinted and incremental like
// every other edge.
export module example.rules.toy;

import std;
import mcpp;

export namespace example::rules::toy {

struct options {
    std::string out_dir = std::string(mcpp::out_dir());

    // The compiler, as the consumer's build program sees it. `dep_bin` answers
    // under the name in the manifest that DECLARED the tool -- this package's
    // `[feature-deps]` entry -- and the answer travels to the consumer because
    // that entry says `reexport = true`.
    std::string compiler = std::string(mcpp::dep_bin("toyc", "toyc"));
};

// Compile every `.toy` the consumer listed.
inline bool compile(options opt = {}) {
    const std::string root = mcpp::manifest_dir();
    if (root.empty()) {
        std::println(std::cerr,
            "example.rules.toy: no mcpp build context -- this runs from build.mcpp");
        return false;
    }

    if (opt.compiler.empty()) {
        std::println(std::cerr,
            "example.rules.toy: the `toyc` compiler was not built. Activate this "
            "package's `rules-toy` feature, which is what asks the graph for it: "
            "rules-toy = {{ path = \"...\", features = [\"rules-toy\"] }}");
        return false;
    }

    // `mcpp::device_sources()` is one string, one path per line, and it is the
    // package's WHOLE device set rather than this rule's share of it. A rule
    // selects by the extension it claims and leaves the rest: a project with a
    // second backend puts that backend's sources in the same list, and handing
    // them to `toyc` would fail on a file `toyc` never claimed.
    std::vector<std::string> mine;
    std::string_view all(mcpp::device_sources());
    for (std::size_t i = 0; i <= all.size();) {
        const auto sep = all.find('\n', i);
        const auto one = all.substr(i, sep == std::string_view::npos ? all.size() - i : sep - i);
        i = sep == std::string_view::npos ? all.size() + 1 : sep + 1;
        if (one.ends_with(".toy")) mine.emplace_back(one);
    }

    bool any = false;
    for (auto const& src : mine) {
        const std::string stem = std::filesystem::path(src).stem().string();
        const std::string gen  = opt.out_dir + "/toy_" + stem + ".cpp";

        // The strings outlive the action. `a.id = ("toy:" + stem).c_str()`
        // would hand it a pointer into a temporary that is gone by `submit`.
        const std::string id   = "toy:" + stem;
        const std::string desc = "toyc " + src;
        // ABSOLUTE. `mcpp::device_sources()` answers package-root-relative, and
        // an action's command does not run from the package root -- it runs
        // from the build directory. Measured: the relative path reached `toyc`
        // unchanged and the read failed there.
        const std::string abs  = root + "/" + src;

        mcpp::action a;
        a.id          = id.c_str();
        // `source`: what this produces is C++ that mcpp then compiles. A rule
        // whose compiler emitted an object directly would use `object`.
        a.role        = "source";
        a.description = desc.c_str();
        a.arg(opt.compiler.c_str()).arg(abs.c_str()).arg("-o").arg(gen.c_str());
        a.input(abs.c_str());
        // THE COMPILER IS AN INPUT TOO. Without this line, a change to `toyc`
        // leaves every edge clean and the artifact keeps the bytes the previous
        // compiler produced -- a green build over a stale result, which is the
        // failure that is hardest to notice.
        a.input(opt.compiler.c_str());
        a.output(gen.c_str());
        a.submit();
        any = true;
    }

    if (!any)
        std::println(std::cerr, "example.rules.toy: no `.toy` source in this package");
    return any;
}

} // namespace example::rules::toy
