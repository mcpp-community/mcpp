// mcpp.build.flag_origins — the attribution (see the interface). An
// implementation unit, so that the modules it reads are not reachable from
// the interface's importers: with them imported by the interface, GCC 16.1
// ended with an internal compiler error (a segmentation fault) at
// `import mcpp.cli;` in main.cpp, which imports the interface through
// mcpp.doctor (measured 2026-11-09).
module mcpp.build.flag_origins;

import std;
import nlohmann.json;
import mcpp.source_kind;
import mcpp.build.plan;
import mcpp.build.flags;
import mcpp.build.compile_commands;
import mcpp.manifest.flag_words;
import mcpp.toolchain.model;
import mcpp.toolchain.triple;
import mcpp.freestanding.target;
import mcpp.platform;

namespace mcpp::build {

namespace {


// The options whose value is the next word: one word of the explanation.
constexpr std::string_view kTakesValue[] = {
    "-isystem", "-I", "-iquote", "-idirafter", "-include", "-imacros", "-x", "-Xclang",
    "-Xlinker", "-B", "-L", "-T", "-m", "-MF", "-MT", "-target", "-isysroot", "-arch",
    "-e", "-z", "-o",
};

std::vector<std::string> joined(const std::vector<std::string>& tokens) {
    std::vector<std::string> out;
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        if (i + 1 < tokens.size() && std::ranges::find(kTakesValue, tokens[i]) != std::end(kTakesValue)) {
            out.push_back(tokens[i] + " " + tokens[i + 1]);
            ++i;
        } else {
            out.push_back(tokens[i]);
        }
    }
    return out;
}

struct Layers {
    // The row's machine description, as the line has it.
    std::set<std::string>                         row;
    std::string                                   rowName;
    std::set<std::string>                         platform;
    std::string                                   platformName;
    std::vector<std::string>                      toolchainRoots;
    std::string                                   toolchainName;
};

Layers layers_of(const BuildPlan& plan) {
    Layers l;
    auto const& tc = plan.toolchain;
    l.rowName = tc.targetTriple;
    if (auto spec = mcpp::freestanding::resolve(tc.targetTriple)) {
        auto words = mcpp::freestanding::row_words(*spec);
        for (auto& w : mcpp::freestanding::row_link_words(*spec)) words.push_back(std::move(w));
        l.row.insert(words.begin(), words.end());
    }
    if (plan.platform.active) {
        l.platformName = plan.platform.display();
        auto const& f = plan.platform.flags;
        for (auto const* list : { &f.cflags, &f.cxxflags, &f.asmflags, &f.ldflags })
            for (auto const& w : joined(mcpp::manifest::flag_words(*list))) l.platform.insert(w);
        for (auto const& w : joined(mcpp::manifest::flag_words(plan.platform.fields.link.flags.value)))
            l.platform.insert(w);
    }
    if (!tc.binaryPath.empty())
        l.toolchainRoots.push_back(tc.binaryPath.parent_path().parent_path().generic_string());
    if (!tc.base.root.empty()) l.toolchainRoots.push_back(tc.base.root.generic_string());
    l.toolchainName = tc.label();
    return l;
}

FlagWord attribute(const Layers& l, const std::string& w) {
    if (l.row.contains(w)) return { w, "row", l.rowName };
    if (l.platform.contains(w)) return { w, "platform", l.platformName };
    for (auto const& root : l.toolchainRoots) {
        auto generic = w;
        std::ranges::replace(generic, '\\', '/');
        if (!root.empty() && generic.find(root) != std::string::npos)
            return { w, "toolchain", l.toolchainName };
    }
    return { w, "engine", {} };
}

} // namespace

FlagOrigins flag_origins(const BuildPlan& plan, std::string_view unit) {
    FlagOrigins o;
    o.identity = plan.toolchain.targetTriple;
    const auto flags = compute_flags(plan);
    const auto layers = layers_of(plan);
    const auto& root = plan.manifest.package.name;
    for (auto const& inv : unit_invocations(plan, flags)) {
        auto const& cu = *inv.unit;
        const auto file = cu.source.generic_string();
        if (unit.empty() ? cu.packageName != root : !file.ends_with(unit)) continue;
        UnitFlagOrigins u{ file, cu.packageName, {} };
        const bool isC = cu.kind == mcpp::SourceKind::C || cu.kind == mcpp::SourceKind::GasAsm;
        const auto own = joined(isC ? mcpp::manifest::flag_words(cu.packageCflags)
                                    : mcpp::manifest::flag_words(cu.packageCxxflags));
        const std::set<std::string> ownSet(own.begin(), own.end());
        // The package's include directories, as the line spells them.
        std::set<std::string> includes;
        for (auto inc : cu.localIncludeDirs) includes.insert("-I" + inc.make_preferred().string());
        for (auto inc : cu.localIncludeDirsAfter) includes.insert("-idirafter" + inc.make_preferred().string());
        const auto words = joined(inv.arguments);
        for (std::size_t i = 0; i < words.size(); ++i) {
            const auto& w = words[i];
            if (i == 0) { u.words.push_back({ w, "compiler", layers.toolchainName }); continue; }
            if (w == "-c" || w == inv.file || w.starts_with("-o ")) continue;
            // The defines the engine gives every unit of a target travel with
            // the package's own flags.
            if (w.starts_with("-D__MCPP_")) { u.words.push_back({ w, "engine", "the target's define" }); continue; }
            if (ownSet.contains(w) || includes.contains(w)) {
                u.words.push_back({ w, "package", cu.packageName });
                continue;
            }
            u.words.push_back(attribute(layers, w));
        }
        o.units.push_back(std::move(u));
    }
    const auto ldOwn = joined(mcpp::manifest::flag_words(plan.manifest.buildConfig.ldflags));
    const std::set<std::string> ldOwnSet(ldOwn.begin(), ldOwn.end());
    for (auto const& w : joined(split_flags(flags.ld))) {
        if (ldOwnSet.contains(w)) { o.link.push_back({ w, "package", root }); continue; }
        o.link.push_back(attribute(layers, w));
    }
    return o;
}

nlohmann::json flag_origins_json(const FlagOrigins& o) {
    auto words = [](const std::vector<FlagWord>& ws) {
        auto a = nlohmann::json::array();
        for (auto const& w : ws)
            a.push_back({ { "word", w.word }, { "layer", w.layer }, { "detail", w.detail } });
        return a;
    };
    auto units = nlohmann::json::array();
    for (auto const& u : o.units)
        units.push_back({ { "file", u.file }, { "package", u.package }, { "words", words(u.words) } });
    return { { "identity", o.identity }, { "units", units }, { "link", words(o.link) } };
}

} // namespace mcpp::build
