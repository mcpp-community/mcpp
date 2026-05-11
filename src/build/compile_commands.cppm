// mcpp.build.compile_commands — generate compile_commands.json for IDE integration.
//
// Produces a Clang JSON Compilation Database (compile_commands.json)
// from the BuildPlan + CompileFlags using nlohmann::json for safe
// serialisation (no manual escaping).
//
// Uses the `arguments` array format (preferred over `command` string
// per clangd docs).
//
// Output location: <projectRoot>/compile_commands.json so clangd finds
// it via its default upward directory walk — zero configuration needed.
//
// See .agents/docs/2026-05-12-compile-commands-design.md.

export module mcpp.build.compile_commands;

import std;
import mcpp.build.plan;
import mcpp.build.flags;
import mcpp.libs.json;

export namespace mcpp::build {

// Generate compile_commands.json content as a string.
std::string emit_compile_commands(const BuildPlan& plan, const CompileFlags& flags);

// Write compile_commands.json to the project root.
void write_compile_commands(const BuildPlan& plan, const CompileFlags& flags);

}  // namespace mcpp::build

namespace mcpp::build {

namespace {

bool is_c_source(const std::filesystem::path& src) {
    return src.extension() == ".c";
}

// Split a flag string into individual tokens.
std::vector<std::string> split_flags(std::string_view s) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && s[i] == ' ')
            ++i;
        if (i >= s.size())
            break;
        std::size_t start = i;
        while (i < s.size() && s[i] != ' ')
            ++i;
        out.emplace_back(s.substr(start, i - start));
    }
    return out;
}

}  // namespace

std::string emit_compile_commands(const BuildPlan& plan, const CompileFlags& flags) {
    nlohmann::json entries = nlohmann::json::array();

    for (auto& cu : plan.compileUnits) {
        // Pick compiler + flags based on source type.
        const auto& compiler = is_c_source(cu.source) ? flags.ccBinary : flags.cxxBinary;
        const auto& flagStr = is_c_source(cu.source) ? flags.cc : flags.cxx;

        auto output_path = (plan.outputDir / cu.object).string();

        // Build arguments array.
        nlohmann::json args = nlohmann::json::array();
        args.push_back(compiler.string());
        for (auto& f : split_flags(flagStr))
            args.push_back(std::move(f));
        args.push_back("-c");
        args.push_back(cu.source.string());
        args.push_back("-o");
        args.push_back(output_path);

        nlohmann::json entry;
        entry["directory"] = plan.projectRoot.string();
        entry["file"] = cu.source.string();
        entry["arguments"] = std::move(args);
        entry["output"] = output_path;

        entries.push_back(std::move(entry));
    }

    return entries.dump(2) + "\n";
}

void write_compile_commands(const BuildPlan& plan, const CompileFlags& flags) {
    auto content = emit_compile_commands(plan, flags);
    auto path = plan.projectRoot / "compile_commands.json";

    // Only write if content changed (avoid triggering clangd re-index).
    if (std::filesystem::exists(path)) {
        std::ifstream is(path);
        std::stringstream ss;
        ss << is.rdbuf();
        if (ss.str() == content)
            return;
    }

    std::ofstream os(path);
    os << content;
}

}  // namespace mcpp::build
