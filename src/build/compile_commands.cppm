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
import mcpp.platform.fs;

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

struct CompileCommandsWriteResult {
    bool changed = false;
    std::size_t commandCount = 0;
};

struct CompileCommandsWriteError {
    std::string message;
};

using ReplaceFile = std::function<bool(const std::filesystem::path&,
                                       const std::filesystem::path&,
                                       std::error_code&)>;

std::expected<CompileCommandsWriteResult, CompileCommandsWriteError>
publish_compile_commands(
    const std::filesystem::path& path,
    std::string_view fresh,
    const std::function<bool(const std::filesystem::path&)>& fileExists,
    ReplaceFile replaceFile = mcpp::platform::fs::replace_file);

std::expected<CompileCommandsWriteResult, CompileCommandsWriteError>
write_compile_commands(const BuildPlan& plan, const CompileFlags& flags);

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
    // #249: after-dirs keep their -idirafter spelling in the compile DB so
    // tooling (clangd) reproduces the compiler's search order.
    for (auto const& inc : cu.localIncludeDirsAfter) {
        args.push_back("-idirafter" + inc.string());
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

void sort_entries_by_file(nlohmann::json& entries) {
    std::stable_sort(entries.begin(), entries.end(), [](auto const& lhs, auto const& rhs) {
        auto file = [](auto const& entry) {
            return entry.contains("file") && entry["file"].is_string()
                ? entry["file"].template get<std::string>()
                : std::string{};
        };
        return file(lhs) < file(rhs);
    });
}

CompileCommandsWriteError write_error(std::string message) {
    return CompileCommandsWriteError{std::move(message)};
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

    sort_entries_by_file(merged);

    return merged.dump(2) + "\n";
}

std::expected<CompileCommandsWriteResult, CompileCommandsWriteError>
publish_compile_commands(
    const std::filesystem::path& path,
    std::string_view fresh,
    const std::function<bool(const std::filesystem::path&)>& fileExists,
    ReplaceFile replaceFile) {
    auto freshJson = nlohmann::json::parse(fresh, nullptr, /*allow_exceptions=*/false);
    if (freshJson.is_discarded() || !freshJson.is_array()) {
        return std::unexpected(write_error(std::format(
            "fresh compile database for '{}' is not a JSON array", path.string())));
    }

    std::filesystem::path publishPath = path;
    std::error_code statusEc;
    const auto linkStatus = std::filesystem::symlink_status(path, statusEc);
    // A first build has no prior CDB to inspect. symlink_status reports the
    // missing path with type()==not_found on libstdc++/libc++/MSVC (the error
    // code category differs: generic ENOENT vs system ERROR_FILE_NOT_FOUND),
    // so that is the normal fresh-workspace case, not an error.
    if (statusEc && linkStatus.type() != std::filesystem::file_type::not_found) {
        return std::unexpected(write_error(std::format(
            "cannot inspect compile database '{}': {}", path.string(),
            statusEc.message())));
    }
    const bool isLink = linkStatus.type() == std::filesystem::file_type::symlink;
    if (isLink) {
        auto target = std::filesystem::read_symlink(path, statusEc);
        if (statusEc) {
            return std::unexpected(write_error(std::format(
                "cannot resolve compile database link '{}': {}", path.string(),
                statusEc.message())));
        }
        publishPath = target.is_absolute() ? target : path.parent_path() / target;
    }

    std::optional<std::string> existing;
    {
        std::ifstream input(publishPath, std::ios::binary);
        if (input) {
            std::stringstream ss;
            ss << input.rdbuf();
            if (input.bad()) {
                return std::unexpected(write_error(std::format(
                    "cannot read existing compile database '{}'", publishPath.string())));
            }
            existing = ss.str();
        } else {
            std::error_code existsEc;
            auto exists = std::filesystem::exists(publishPath, existsEc);
            if (existsEc) {
                return std::unexpected(write_error(std::format(
                    "cannot inspect existing compile database '{}': {}",
                    publishPath.string(), existsEc.message())));
            }
            if (exists) {
                return std::unexpected(write_error(std::format(
                    "cannot read existing compile database '{}'", publishPath.string())));
            }
        }
    }  // input closed before the atomic replace: Windows cannot replace an open file.

    // 完全相同的有效输入不重写文件，避免 clangd 因 mtime 变化重复索引。
    if (existing && *existing == fresh) {
        return CompileCommandsWriteResult{false, freshJson.size()};
    }

    std::string content(fresh);
    if (existing) {
        // 保留仍存在但当前 plan 未覆盖的条目，主要是之前 test 生成的 TU。
        content = merge_compile_commands(content, *existing, fileExists);
    }

    auto finalJson = nlohmann::json::parse(content, nullptr, /*allow_exceptions=*/false);
    if (finalJson.is_discarded() || !finalJson.is_array()) {
        return std::unexpected(write_error(std::format(
            "final compile database for '{}' is not a JSON array", path.string())));
    }
    sort_entries_by_file(finalJson);
    content = finalJson.dump(2) + "\n";

    if (existing && *existing == content) {
        return CompileCommandsWriteResult{false, finalJson.size()};
    }

    static std::atomic<std::uint64_t> sequence{0};
    const auto nonce = std::random_device{}();
    // 临时文件和链接目标同目录，避免 rename 跨文件系统；随机量降低跨进程碰撞概率。
    auto temp = publishPath.parent_path()
        / std::format(".{}.tmp.{}.{}.{}", publishPath.filename().string(),
                      std::chrono::steady_clock::now().time_since_epoch().count(),
                      static_cast<unsigned long long>(nonce),
                      sequence.fetch_add(1, std::memory_order_relaxed));
    auto cleanup_temp = [&] {
        std::error_code cleanupEc;
        std::filesystem::remove(temp, cleanupEc);
    };

    std::ofstream output(temp, std::ios::binary | std::ios::trunc);
    if (!output) {
        return std::unexpected(write_error(std::format(
            "cannot open temporary compile database '{}'", temp.string())));
    }
    output << content;
    output.flush();
    if (!output) {
        output.close();
        cleanup_temp();
        return std::unexpected(write_error(std::format(
            "cannot write temporary compile database '{}'", temp.string())));
    }
    output.close();
    if (!output) {
        cleanup_temp();
        return std::unexpected(write_error(std::format(
            "cannot close temporary compile database '{}'", temp.string())));
    }

    std::error_code ec;
    if (!replaceFile(temp, publishPath, ec)) {
        cleanup_temp();
        return std::unexpected(write_error(std::format(
            "cannot replace '{}': {}", publishPath.string(), ec.message())));
    }

    return CompileCommandsWriteResult{true, finalJson.size()};
}

std::expected<CompileCommandsWriteResult, CompileCommandsWriteError>
write_compile_commands(const BuildPlan& plan, const CompileFlags& flags) {
    auto path = plan.compileDbPath.empty()
        ? plan.projectRoot / "compile_commands.json"
        : plan.compileDbPath;
    return publish_compile_commands(
        path, emit_compile_commands(plan, flags),
        [](const std::filesystem::path& candidate) {
            return std::filesystem::exists(candidate);
        });
}

}  // namespace mcpp::build
