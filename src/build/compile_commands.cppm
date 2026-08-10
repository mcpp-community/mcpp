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

// Split one flag string into CDB `arguments` tokens.
//
// Exported because it IS the contract: what a consumer receives in
// `arguments` is decided here, and that contract needs pinning by test
// rather than by inspection of a whole generated document.
std::vector<std::string> split_flags(std::string_view s);

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

}  // namespace

// Split one flag string into CDB `arguments` tokens.
//
// Three things happen here, and the ORDER between them is the whole point.
//
// 1. Ninja escapes are undone (`$ ` -> space, `$:` -> `:`, `$$` -> `$`).
//    `flags.cppm::escape_path` adds them so a path survives embedding in a
//    ninja rule string. A CDB consumer execs `arguments` literally -- no
//    ninja -- so `C$:\Users\...` would be a path that does not exist.
//
// 2. Shell quoting is removed. `flags.cppm::shell_quote_arg` wraps a token
//    whose characters would split or alter a word in `sh -c` / cmd.exe --
//    and its trigger set contains the BACKSLASH, so on Windows every
//    path-bearing flag is quoted. That quoting is correct for ninja and
//    wrong for a consumer that never invokes a shell: the quotes arrive as
//    part of the filename.
//
// 3. Tokens split on spaces -- but not on a space that came from `$ `, and
//    not on one inside quotes.
//
// Getting (3) wrong is what shipped: quoting was ignored entirely, so a
// quoted path containing a space was cut in half at that space. Measured on
// a real build under `/tmp/.../my project`:
//
//   '-fmodule-file=std=/tmp/.../my project/.../std.pcm   <- closing quote gone
//   '-fprebuilt-module-path=/tmp/.../my project/.../pcm.cache'
//
// One argument became two, one of them opening a quote that never closes.
// The comment that used to live here stated the right principle -- consumers
// exec literally, so escapes must be undone -- and implemented half of it.
//
// A quote in the MIDDLE of a token is data: `-DGREETING="hi"` keeps its
// inner quotes, or the define changes meaning. Only a quote that OPENS a
// region is quoting.
std::vector<std::string> split_flags(std::string_view s) {
    std::vector<std::string> out;
    std::string token;
    bool started = false;          // token has begun (so "" is a real empty arg)
    char quote = 0;                // active quote char, 0 when outside

    auto flush = [&] {
        if (started) { out.push_back(std::move(token)); token.clear(); }
        started = false;
    };

    for (std::size_t i = 0; i < s.size(); ++i) {
        char c = s[i];

        // Ninja escapes first, and INSIDE quotes too -- a quoted path's space
        // arrives as `$ ` because flags.cppm escapes before it quotes.
        if (c == '$' && i + 1 < s.size()) {
            char nc = s[i + 1];
            if (nc == ' ' || nc == ':' || nc == '$') {
                token.push_back(nc);
                started = true;
                ++i;
                continue;
            }
        }

        if (quote) {
            if (c == quote) { quote = 0; continue; }   // closes the region
            token.push_back(c);
            continue;
        }

        // Opens a region only at a token boundary; elsewhere it is data.
        if ((c == '"' || c == '\'') && !started) { quote = c; started = true; continue; }

        if (c == ' ') { flush(); continue; }

        token.push_back(c);
        started = true;
    }
    flush();
    return out;
}

namespace {

// NATIVE separators for every path this emitter SPELLS ITSELF. Each ingestion
// point (manifest globs, include_dirs, build.mcpp directives) is normalized at
// the source, but this is the last line for the fields the CDB schema defines
// — a path that slips through with a mixed `root\a/b` spelling (MSVC keeps
// input `/` verbatim) breaks CLion, and "all ingestion points are covered" is
// not a claim that can be proven once and stay true.
//
// It is NOT a whole-argv guarantee: the flag strings (split_flags(f.cxx), the
// package cflags/cxxflags) pass through untouched, because normalizing an
// arbitrary flag payload is unsafe — `-DPATH="/etc/x"` holds real slashes.
// Those channels are normalized where they are ingested instead.
// make_preferred() is a no-op on POSIX.
std::string native_string(const std::filesystem::path& p) {
    auto n = p;
    n.make_preferred();
    return n.string();
}

std::vector<std::string> local_include_args(const CompileUnit& cu) {
    std::vector<std::string> args;
    args.reserve(cu.localIncludeDirs.size());
    for (auto const& inc : cu.localIncludeDirs) {
        args.push_back("-I" + native_string(inc));
    }
    // #249: after-dirs keep their -idirafter spelling in the compile DB so
    // tooling (clangd) reproduces the compiler's search order.
    for (auto const& inc : cu.localIncludeDirsAfter) {
        args.push_back("-idirafter" + native_string(inc));
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

        auto output_path = native_string(plan.outputDir / cu.object);

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
        args.push_back(native_string(cu.source));
        args.push_back("-o");
        args.push_back(output_path);

        nlohmann::json entry;
        entry["directory"] = native_string(plan.projectRoot);
        entry["file"] = native_string(cu.source);
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

    // Dedup key = the file's PATH, spelled the way a fresh plan spells it
    // (native separators). A prior CDB written before the mixed-separator
    // fix (#390) carries `root\generated/modules\x.cppm` entries that are
    // the SAME file as the fresh `root\generated\modules\x.cppm` — a literal
    // string comparison would keep both and the user's upgrade would not
    // visibly fix anything. Normalizing makes the merge self-healing: the
    // stale mixed entry is skipped on the first `mcpp build` after upgrade.
    // fileExists still probes the raw spelling — Windows accepts both.
    auto norm_key = [](std::string_view f) {
        auto p = std::filesystem::path(std::string(f)).lexically_normal();
        p.make_preferred();
        return p.string();
    };

    // Files the current plan already covers — those entries are authoritative.
    std::set<std::string> freshFiles;
    for (auto const& e : freshJ) {
        if (e.contains("file") && e["file"].is_string())
            freshFiles.insert(norm_key(e["file"].get<std::string>()));
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
            if (freshFiles.contains(norm_key(f))) continue;       // fresh wins
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
