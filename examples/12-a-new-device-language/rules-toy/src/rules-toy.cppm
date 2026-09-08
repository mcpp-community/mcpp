// A rule for `.toy`, a language whose entire definition is "one integer per
// line, and the file's name is the entry point".
//
// The compiler is a shell script rather than a vendor toolkit, for the same
// reason `tests/e2e/607` uses `cat` as a device linker: the subject is the
// GRAPH -- how a language that the engine does not know reaches the link --
// and a real device compiler would only add a download to it.
export module example.rules.toy;

import std;
import mcpp;

export namespace example::rules::toy {

struct options {
    std::string out_dir = std::string(mcpp::out_dir());

    // Where this rule's own files are. `mcpp::dep_dir` answers under the name
    // the CONSUMER declared in `[dependencies]`, which is why the default is a
    // string this package cannot verify -- a consumer that declares the edge
    // under another key passes its own spelling here.
    std::string rule_dir = std::string(mcpp::dep_dir("rules-toy"));
};

// Compile every `.toy` the consumer listed. `mcpp::device_sources()` returns
// the device sources of the package being built, so the rule reads the source
// set from the graph rather than asking the project to repeat it.
inline bool compile(options opt = {}) {
    const std::string root = mcpp::manifest_dir();
    if (root.empty()) {
        std::println(std::cerr,
            "example.rules.toy: no mcpp build context -- this runs from build.mcpp");
        return false;
    }

    if (opt.rule_dir.empty()) {
        std::println(std::cerr,
            "example.rules.toy: cannot locate this rule package. `mcpp::dep_dir` "
            "answers under the name the consumer declared; this rule looked for "
            "`rules-toy`. Pass `options::rule_dir` if the edge is declared "
            "under another key.");
        return false;
    }
    const std::string script = opt.rule_dir + "/tools/toyc.sh";

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
        const std::string desc = "compile " + src;
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
        a.arg("sh"); a.arg(script.c_str()); a.arg(abs.c_str()); a.arg(gen.c_str());
        a.arg(stem.c_str());
        a.input(abs.c_str());
        // THE COMPILER IS AN INPUT TOO. Without this line, editing `toyc.sh`
        // leaves every edge clean and the artifact keeps the bytes the previous
        // compiler produced -- a green build over a stale result, which is the
        // failure that is hardest to notice. Measured while writing this
        // example: a fix to the summator did not reach the program.
        a.input(script.c_str());
        a.output(gen.c_str());
        a.submit();
        any = true;
    }

    if (!any)
        std::println(std::cerr, "example.rules.toy: no `.toy` source in this package");
    return any;
}

} // namespace example::rules::toy
