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

// Merge freshly-emitted CDB text (`fresh`, from the current build plan) with a
// prior CDB on disk (`existing`). A prior entry is preserved ONLY when its
// `file` is absent from `fresh` AND still exists on disk (per `fileExists`);
// everything else comes from `fresh`. Result is sorted by `file` for stable
// output. A malformed `existing` is ignored (falls back to `fresh`).
//
// Rationale: `mcpp build` regenerates the CDB from a plan that lacks test files
// / dev-deps, while `mcpp test` writes them in. Without merging, whichever ran
// last wins and clangd loses coverage for tests/ (no completion). Merging makes
// the CDB the union of every command's real plan — offline-safe, no extra
// dependency resolution. See .agents/docs/2026-06-25-cdb-test-coverage-design.md.
std::string merge_compile_commands(
    std::string_view fresh,
    std::string_view existing,
    const std::function<bool(const std::filesystem::path&)>& fileExists);

// Write compile_commands.json to the project root.
void write_compile_commands(const BuildPlan& plan, const CompileFlags& flags);

}  // namespace mcpp::build

namespace mcpp::build {

namespace {

bool is_c_source(const std::filesystem::path& src) {
    auto ext = src.extension();
    return ext == ".c" || ext == ".m";
}

// Split a flag string into individual tokens AND un-escape ninja-style
// path escapes (`$ ` → space, `$:` → `:`, `$$` → `$`).
//
// `flags.cppm::escape_path` ninja-escapes path arguments so they survive
// embedding in ninja rule strings. Those escaped strings are then captured
// into `f.cxx` / `f.cc` which is what we receive here. CDB consumers like
// clangd exec the `arguments` array literally — no ninja involved — so
// escaped chars must be undone or paths like `C:\Users\...` come through
// as `C$:\Users\...` and break clangd's path resolution on Windows. (The
// same issue would silently affect any POSIX path containing a space or
// `$` — those just happen to be rare.)
//
// Splitting and un-escaping in one pass is correct: a literal space inside
// a path appears as `$ ` in the input, which we must NOT treat as a token
// separator.
std::vector<std::string> split_flags(std::string_view s) {
    std::vector<std::string> out;
    std::string token;
    auto flush = [&] {
        if (!token.empty()) {
            out.push_back(std::move(token));
            token.clear();
        }
    };
    for (std::size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '$' && i + 1 < s.size()) {
            char nc = s[i + 1];
            if (nc == ' ' || nc == ':' || nc == '$') {
                token.push_back(nc);
                ++i;
                continue;
            }
        }
        if (c == ' ') {
            flush();
        } else {
            token.push_back(c);
        }
    }
    flush();
    return out;
}

std::vector<std::string> local_include_args(const CompileUnit& cu) {
    std::vector<std::string> args;
    args.reserve(cu.localIncludeDirs.size());
    for (auto const& inc : cu.localIncludeDirs) {
        args.push_back("-I" + inc.string());
    }
    return args;
}

std::vector<std::string> package_flag_args(const CompileUnit& cu, bool isCSource) {
    std::string joined;
    auto const& flags = isCSource ? cu.packageCflags : cu.packageCxxflags;
    for (auto const& flag : flags) {
        joined += ' ';
        joined += flag;
    }
    return split_flags(joined);
}

}  // namespace

std::string emit_compile_commands(const BuildPlan& plan, const CompileFlags& flags) {
    nlohmann::json entries = nlohmann::json::array();

    for (auto& cu : plan.compileUnits) {
        // NASM units carry a command line no CDB consumer (clangd, …) can
        // interpret — a bogus entry actively harms LSP diagnostics, so they
        // are excluded from the CDB entirely.
        if (cu.source.extension() == ".asm") continue;
        // Pick compiler + flags based on source type. GAS units (.S/.s) ride
        // the C driver with the asm-safe flag string, mirroring build.ninja.
        const auto ext = cu.source.extension();
        const bool isGasSource = ext == ".S" || ext == ".s";
        const bool isCSource = is_c_source(cu.source) || isGasSource;
        const auto& compiler = isCSource ? flags.ccBinary : flags.cxxBinary;
        const auto& flagStr = isGasSource ? flags.as
                            : isCSource   ? flags.cc
                                          : flags.cxx;

        auto output_path = (plan.outputDir / cu.object).string();

        // Build arguments array.
        nlohmann::json args = nlohmann::json::array();
        args.push_back(compiler.string());
        for (auto& f : local_include_args(cu))
            args.push_back(std::move(f));
        for (auto& f : split_flags(flagStr))
            args.push_back(std::move(f));
        for (auto& f : package_flag_args(cu, isCSource))
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

std::string merge_compile_commands(
    std::string_view fresh,
    std::string_view existing,
    const std::function<bool(const std::filesystem::path&)>& fileExists) {
    auto freshJ = nlohmann::json::parse(fresh, nullptr, /*allow_exceptions=*/false);
    if (freshJ.is_discarded() || !freshJ.is_array())
        return std::string(fresh);

    // Files the current plan already covers — those entries are authoritative.
    std::set<std::string> freshFiles;
    for (auto const& e : freshJ) {
        if (e.contains("file") && e["file"].is_string())
            freshFiles.insert(e["file"].get<std::string>());
    }

    // Keep fresh order, then append still-valid prior entries the plan doesn't
    // cover (e.g. tests/ from a previous `mcpp test`). Drop entries for files
    // that no longer exist so the CDB never accrues dead references.
    nlohmann::json merged = freshJ;
    auto existingJ = nlohmann::json::parse(existing, nullptr, /*allow_exceptions=*/false);
    if (!existingJ.is_discarded() && existingJ.is_array()) {
        for (auto const& e : existingJ) {
            if (!e.contains("file") || !e["file"].is_string()) continue;
            auto f = e["file"].get<std::string>();
            if (freshFiles.contains(f)) continue;             // fresh wins
            if (!fileExists(std::filesystem::path(f))) continue;  // pruned
            merged.push_back(e);
        }
    }

    return merged.dump(2) + "\n";
}

void write_compile_commands(const BuildPlan& plan, const CompileFlags& flags) {
    auto content = emit_compile_commands(plan, flags);
    auto path = plan.projectRoot / "compile_commands.json";

    if (std::filesystem::exists(path)) {
        std::ifstream is(path);
        std::stringstream ss;
        ss << is.rdbuf();
        auto existing = ss.str();

        // Preserve still-valid prior entries this plan doesn't cover — chiefly
        // tests/ entries a previous `mcpp test` wrote — so a plain `mcpp build`
        // doesn't wipe clangd's coverage of test files. Offline-safe: no extra
        // dependency resolution, just a merge of real prior plans.
        content = merge_compile_commands(content, existing,
            [](const std::filesystem::path& p) { return std::filesystem::exists(p); });

        // Only write if content changed (avoid triggering clangd re-index).
        if (existing == content)
            return;
    }

    std::ofstream os(path);
    os << content;
}

}  // namespace mcpp::build
