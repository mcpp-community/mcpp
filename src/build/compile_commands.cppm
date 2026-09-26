// mcpp.build.compile_commands — generate compile_commands.json for IDE integration.
//
// Produces a Clang JSON Compilation Database (compile_commands.json)
// from the BuildPlan + CompileFlags using nlohmann::json for safe
// serialisation (no manual escaping).
//
// Uses the `arguments` array format (preferred over `command` string
// per clangd docs).
//
// TWO FILES, ONE RULE EACH (design 2026-09-26, .agents/docs/2026-09-26-
// compile-database-and-issue-699-design.md §3.2).
//
//   - The CONFIGURATION's database, `<outputDir>/compile_commands.json`, is
//     the merged record of every command that has planned in this exact
//     output directory (`build`, `test`, `run`, `--configure-only`): the
//     fresh plan's entries, plus the entries it already holds whose `file`,
//     resolved against `directory`, the fresh plan lacks and which still
//     exist. Every entry in it was written by mcpp in this one configuration,
//     so no ownership test is needed and none is made.
//   - The ROOT file, `<projectRoot>/compile_commands.json` (the path clangd's
//     own upward search finds, or `plan.compileDbPath` where a symlink there
//     is written through), is a COPY of the current configuration's database:
//     replaced whole, never merged, and left untouched when byte-identical so
//     clangd is not triggered for nothing. Switching toolchain or profile
//     switches the whole file; switching back restores that configuration's
//     entries, its test units included.
//
// `write_compile_commands` performs both steps for a freshly rendered plan
// (the full build path, ninja_backend.cppm). `publish_root_compile_commands`
// is exported on its own because the FAST path (execute.cppm) restores the
// root file from a configuration database already on disk, with no plan at
// all (C1: a deleted root file used to stay deleted forever, because nothing
// on the fast path ever reached a writer).
//
// See .agents/docs/2026-05-12-compile-commands-design.md and the 2026-09-26
// design record above.

export module mcpp.build.compile_commands;

import std;
import mcpp.source_kind;
import mcpp.build.plan;
import mcpp.build.flags;
import mcpp.home;
import mcpp.libs.json;
import mcpp.platform.fs;
import mcpp.platform;
import mcpp.manifest.flag_words;
import mcpp.toolchain.model;

export namespace mcpp::build {

// The words the compiler receives from one flag string the engine rendered
// for a ninja `command =` line: ninja's `$` escapes are undone, then the
// host's command-line reader splits the text (POSIX `sh`, or the MSVCRT
// rules on Windows; mcpp::manifest::host_command_words).
//
// It serves the rendered strings only, the global `$cflags`/`$cxxflags`/
// `$asmflags`. A unit's own flag lists are never rendered and re-read: the
// databases list their words directly (mcpp::manifest::flag_words).
//
// Exported because it IS the contract: what a consumer receives in
// `arguments` is decided here, and that contract needs pinning by test
// rather than by inspection of a whole generated document.
std::vector<std::string> split_flags(std::string_view s);

// The flag list an assembly unit's edge carries: the -D/-U/-I words of the
// unit's C flags (feature defines land there), then its per-glob asmflags.
// NASM shares the GNU -D/-U/-I spelling (and 2.14 and later insert a missing
// -I path separator itself), so one filter serves both assembler rules, and
// the compile database lists the same list for a GAS unit.
std::vector<std::string> unit_asm_flags(const CompileUnit& cu);

// ONE TRANSLATION UNIT AS THE COMPILER IS INVOKED FOR IT.
//
// The compile database and the build database (mcpp.build.build_database) both
// render this record, so the `arguments` one lists for a unit cannot differ from
// the other's. NASM units have no record: no consumer of either format can
// interpret their command line.
struct UnitInvocation {
    const CompileUnit*       unit = nullptr;   // into the plan it came from
    std::string              directory;
    std::string              file;
    std::vector<std::string> arguments;        // driver first
    std::string              output;
};

std::vector<UnitInvocation> unit_invocations(const BuildPlan& plan,
                                             const CompileFlags& flags);

// Generate compile_commands.json content as a string.
std::string emit_compile_commands(const BuildPlan& plan, const CompileFlags& flags);

// Merge freshly-emitted CDB text (`fresh`, from the current build plan) with a
// prior CDB of the SAME CONFIGURATION on disk (`existing`). A prior entry is
// preserved ONLY when its `file`, resolved against its `directory` when not
// already absolute, is absent from `fresh` (under the same resolution) AND
// still exists on disk (per `fileExists`, probed against the resolved path);
// everything else comes from `fresh`. Result is sorted by `file` for stable
// output. A malformed `existing` is ignored (falls back to `fresh`).
//
// Rationale: `mcpp build` regenerates the database from a plan that lacks
// test files / dev-deps, while `mcpp test` writes them in; both plan into the
// same output directory, so without merging, whichever ran last wins and
// clangd loses coverage for tests/ (no completion). Merging makes the
// configuration's database the union of every command's real plan —
// offline-safe, no extra dependency resolution. `directory` enters the
// identity because a unit's own `directory` need not be the process's
// working directory (the standard-library units always name the shared std
// cache; every project unit now names the output directory, §3.3). See
// .agents/docs/2026-06-25-cdb-test-coverage-design.md and the 2026-09-26
// design record.
std::string merge_compile_commands(
    std::string_view fresh,
    std::string_view existing,
    const std::function<bool(const std::filesystem::path&)>& fileExists);

struct CompileCommandsWriteResult {
    bool changed = false;
    std::size_t commandCount = 0;
    // Set only by publish_root_compile_commands (and by write_compile_commands,
    // which calls it): the number of entries the REPLACED root file held that
    // were not mcpp's own. Zero when the root was not replaced, held nothing
    // foreign, or is being written for the first time.
    std::size_t foreignEntries = 0;
};

struct CompileCommandsWriteError {
    std::string message;
};

using ReplaceFile = std::function<bool(const std::filesystem::path&,
                                       const std::filesystem::path&,
                                       std::error_code&)>;

// Writes the CONFIGURATION's database at `path` (normally
// `plan.outputDir / "compile_commands.json"`): merges `fresh` with whatever
// `path` already holds (a symlink there is followed, as at the root),
// leaves the file untouched when the result is byte-identical, and replaces
// it atomically otherwise.
std::expected<CompileCommandsWriteResult, CompileCommandsWriteError>
publish_compile_commands(
    const std::filesystem::path& path,
    std::string_view fresh,
    const std::function<bool(const std::filesystem::path&)>& fileExists,
    ReplaceFile replaceFile = mcpp::platform::fs::replace_file);

// Copies the configuration database's CURRENT CONTENT on disk at
// `configDbPath` over the root file at `rootPath` (following a symlink
// there, as today): replaced WHOLE, never merged, and left untouched when
// byte-identical. This is the one function both the full build (right after
// it writes the configuration database) and the fast path (which has no
// plan, and reads the configuration database instead of holding it in
// memory) call to publish the root file — design §3.2 items 2 to 4.
//
// `targetRoot` and `mcppHome` decide which of the root file's PRIOR entries
// (by `output`, resolved against `directory`) are mcpp's own: under
// `targetRoot` (this project's whole `target/` tree — a toolchain or
// profile switch replaces the root file too, and that is not a foreign
// write) or under `mcppHome` (the shared std cache the standard-library
// units of §3.5 write into). Every other prior entry is counted in the
// result's `foreignEntries`, so the caller can warn once.
std::expected<CompileCommandsWriteResult, CompileCommandsWriteError>
publish_root_compile_commands(
    const std::filesystem::path& configDbPath,
    const std::filesystem::path& rootPath,
    const std::filesystem::path& targetRoot,
    const std::filesystem::path& mcppHome,
    ReplaceFile replaceFile = mcpp::platform::fs::replace_file);

// Writes the configuration's database for `plan`, then publishes the root
// copy from it. `commandCount` is the configuration database's entry count;
// `foreignEntries` is the root publish's (see publish_root_compile_commands).
std::expected<CompileCommandsWriteResult, CompileCommandsWriteError>
write_compile_commands(const BuildPlan& plan, const CompileFlags& flags);

}  // namespace mcpp::build

namespace mcpp::build {

// The order is the one ninja and the host apply: ninja replaces `$ `, `$:` and
// `$$` while it builds the command line, and only then does the host read the
// line into words. flags.cppm escapes for ninja before it quotes for the host,
// so a quoted path's space arrives here as `$ ` inside the quotes, and it is a
// space again before the reader sees the quotes.
//
// The reader is the host's own. The version before #655 split on spaces and
// removed a quote only when it opened a token, which is neither host's rule:
// `\"` stayed escaped and a quote inside a word stayed in the word, so the
// database listed arguments the compiler never received.
std::vector<std::string> split_flags(std::string_view s) {
    std::string unescaped;
    unescaped.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '$' && i + 1 < s.size()
            && (s[i + 1] == ' ' || s[i + 1] == ':' || s[i + 1] == '$')) {
            unescaped.push_back(s[++i]);
            continue;
        }
        unescaped.push_back(s[i]);
    }
    return mcpp::manifest::host_command_words(unescaped,
                                              mcpp::platform::is_windows);
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

// The unit's own flag lists as words. The edge writes the same words, each
// quoted for the host (ninja_backend.cppm::join_flags), so the database lists
// what the compiler receives without reading a command line back.
std::vector<std::string> package_flag_args(const CompileUnit& cu, bool isCSource) {
    return mcpp::manifest::flag_words(isCSource ? cu.packageCflags : cu.packageCxxflags);
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

// `file` (or, for the ownership test below, `output`) resolved against
// `directory` when it is not already absolute: the identity the within-
// configuration merge and the root's ownership test both use (design §3.2
// items 1 and 3). Every path mcpp itself writes into either field is already
// absolute (a unit's source and its output are both absolute; see
// plan.cppm), so this only matters for an entry another tool wrote, or for a
// future producer that follows the JSON format's licence to write a relative
// one.
std::filesystem::path resolve_against_directory(const nlohmann::json& entry,
                                                std::string_view field) {
    const std::string key(field);
    if (!entry.is_object() || !entry.contains(key) || !entry.at(key).is_string())
        return {};
    std::filesystem::path p(entry.at(key).get<std::string>());
    if (p.is_absolute()) return p.lexically_normal();
    if (!entry.contains("directory") || !entry.at("directory").is_string()) return p;
    return (std::filesystem::path(entry.at("directory").get<std::string>()) / p).lexically_normal();
}

// A key for the merge's dedup set: the resolved path, in one spelling. Not
// just `.string()` — a prior CDB written before the mixed-separator fix
// (#390) carries `root\generated/modules\x.cppm` entries that are the SAME
// file as a fresh `root\generated\modules\x.cppm` one, and a literal string
// comparison would keep both. Normalizing makes the merge self-healing.
std::string dedup_key(const std::filesystem::path& resolved) {
    auto p = resolved.lexically_normal();
    p.make_preferred();
    return p.string();
}

// §3.2 item 3: an entry is mcpp's own when its `output`, resolved against its
// `directory`, lies under this project's `target/` (every configuration, not
// only the current one — switching toolchain or profile replaces the root
// file too, and that is not a foreign write) or under the mcpp home (the
// shared std cache the standard-library units of §3.5 write their objects
// into, outside `target/`). An entry with no `output` at all — the shape a
// hand-written or another tool's database uses — is never mcpp's: mcpp
// always writes one.
bool is_mcpps_entry(const nlohmann::json& entry,
                    const std::filesystem::path& targetRoot,
                    const std::filesystem::path& mcppHome) {
    auto output = resolve_against_directory(entry, "output");
    if (output.empty()) return false;
    auto under = [&](const std::filesystem::path& base) {
        if (base.empty()) return false;
        auto rel = output.lexically_relative(base.lexically_normal());
        if (rel.empty() || rel == std::filesystem::path(".")) return false;
        return *rel.begin() != std::filesystem::path("..");
    };
    return under(targetRoot) || under(mcppHome);
}

// Reads `path` through one symlink hop (as the root file's redirect always
// has) and returns the real file to publish to, plus its current content
// when it exists. A first build has nothing to resolve: symlink_status
// reports the missing path with type()==not_found on every standard library
// (the error code category differs — generic ENOENT vs system
// ERROR_FILE_NOT_FOUND — so that case is not an error).
struct ExistingDocument {
    std::filesystem::path      publishPath;
    std::optional<std::string> content;
};

std::expected<ExistingDocument, CompileCommandsWriteError>
read_existing_document(const std::filesystem::path& path) {
    std::filesystem::path publishPath = path;
    std::error_code statusEc;
    const auto linkStatus = std::filesystem::symlink_status(path, statusEc);
    if (statusEc && linkStatus.type() != std::filesystem::file_type::not_found) {
        return std::unexpected(write_error(std::format(
            "cannot inspect compile database '{}': {}", path.string(),
            statusEc.message())));
    }
    if (linkStatus.type() == std::filesystem::file_type::symlink) {
        auto target = std::filesystem::read_symlink(path, statusEc);
        if (statusEc) {
            return std::unexpected(write_error(std::format(
                "cannot resolve compile database link '{}': {}", path.string(),
                statusEc.message())));
        }
        publishPath = target.is_absolute() ? target : path.parent_path() / target;
    }

    std::optional<std::string> existing;
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
    return ExistingDocument{std::move(publishPath), std::move(existing)};
}

std::expected<void, CompileCommandsWriteError>
atomic_replace_document(const std::filesystem::path& publishPath,
                        const std::string& content, const ReplaceFile& replaceFile) {
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
    return {};
}

}  // namespace

std::vector<std::string> unit_asm_flags(const CompileUnit& cu) {
    std::vector<std::string> out;
    for (auto& w : mcpp::manifest::flag_words(cu.packageCflags)) {
        if (w.starts_with("-D") || w.starts_with("-U") || w.starts_with("-I"))
            out.push_back(mcpp::manifest::flag_element(w));
    }
    out.insert(out.end(), cu.packageAsmflags.begin(), cu.packageAsmflags.end());
    return out;
}

std::vector<UnitInvocation> unit_invocations(const BuildPlan& plan,
                                             const CompileFlags& flags) {
    std::vector<UnitInvocation> out;
    out.reserve(plan.compileUnits.size());

    // C4: the driver's own suffix→language table is private and version-
    // dependent (BmiTraits::moduleInterfaceLangFlag's note on why the build
    // states it unconditionally instead of trusting a driver to infer it);
    // this record states the same flag at the same position, so a reader
    // that replays `arguments` verbatim reads a module-extension source the
    // way the build did rather than guessing from its name.
    const auto traits = mcpp::toolchain::bmi_traits(plan.toolchain);

    for (auto& cu : plan.compileUnits) {
        // NASM units carry a command line no CDB consumer (clangd, …) can
        // interpret — a bogus entry actively harms LSP diagnostics, so they
        // are excluded from the CDB entirely.
        if (cu.kind == mcpp::SourceKind::NasmAsm) continue;
        // Pick compiler + flags based on source ROLE. GAS units (.S/.s) ride
        // the C driver with the asm-safe flag string, mirroring build.ninja.
        const bool isGasSource = cu.kind == mcpp::SourceKind::GasAsm;
        const bool isCSource = cu.kind == mcpp::SourceKind::C || isGasSource;
        const auto& compiler = isCSource ? flags.ccBinary : flags.cxxBinary;
        const auto& flagStr = isGasSource ? flags.as
                            : isCSource   ? flags.cc
                                          : flags.cxx;

        UnitInvocation inv;
        inv.unit      = &cu;
        inv.output    = native_string(plan.outputDir / cu.object);
        // C3: every project unit runs in the output directory — the directory
        // ninja actually invokes the compiler in — never the project root.
        // The standard-library units keep their own `directory` (the shared
        // std cache), which unit_invocations never touches: they are
        // synthesised separately (mcpp.build.build_database::render).
        inv.directory = native_string(plan.outputDir);
        inv.file      = native_string(cu.source);

        // Build arguments array.
        inv.arguments.push_back(compiler.string());
        for (auto& f : local_include_args(cu))
            inv.arguments.push_back(std::move(f));
        for (auto& f : split_flags(flagStr))
            inv.arguments.push_back(std::move(f));
        for (auto& f : isGasSource ? mcpp::manifest::flag_words(unit_asm_flags(cu))
                                   : package_flag_args(cu, isCSource))
            inv.arguments.push_back(std::move(f));
        // C4: immediately before `-c`, since the GNU dialects read the flag
        // positionally (BmiTraits::moduleInterfaceLangFlag). Only a unit that
        // actually provides a module needs it — an implementation unit (a
        // plain source, or `module M;` inside a module-extension file) is
        // not the "which language is this extension" ambiguity the flag
        // removes. MSVC's form (`/interface /TP`) waits for a Windows
        // measurement of clang-cl-mode clangd (design §3.4, "Open").
        if (!cu.providesModule.empty()
            && plan.toolchain.compiler != mcpp::toolchain::CompilerId::MSVC) {
            for (auto& w : split_flags(traits.moduleInterfaceLangFlag))
                inv.arguments.push_back(std::move(w));
        }
        inv.arguments.push_back("-c");
        inv.arguments.push_back(inv.file);
        inv.arguments.push_back("-o");
        inv.arguments.push_back(inv.output);

        out.push_back(std::move(inv));
    }
    return out;
}

std::string emit_compile_commands(const BuildPlan& plan, const CompileFlags& flags) {
    nlohmann::json entries = nlohmann::json::array();

    for (auto& inv : unit_invocations(plan, flags)) {
        nlohmann::json args = nlohmann::json::array();
        for (auto& a : inv.arguments) args.push_back(std::move(a));

        nlohmann::json entry;
        entry["directory"] = std::move(inv.directory);
        entry["file"] = std::move(inv.file);
        entry["arguments"] = std::move(args);
        entry["output"] = std::move(inv.output);

        entries.push_back(std::move(entry));
    }

    // D5a, S1-12-1: the standard-library units are translation units of this
    // build like any other, recovered once onto the plan (prepare.cppm,
    // BuildPlan::stdModuleUnits) from the SAME command
    // mcpp.build.build_database's S1 rendering uses, so the two documents
    // cannot list different arguments for the same unit (P1).
    for (auto const& unit : plan.stdModuleUnits) {
        nlohmann::json args = nlohmann::json::array();
        for (auto const& a : unit.arguments) args.push_back(a);

        nlohmann::json entry;
        entry["directory"] = native_string(unit.workDirectory);
        entry["file"] = native_string(unit.source);
        entry["arguments"] = std::move(args);
        entry["output"] = native_string(unit.object);

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
    // The key resolves `file` against `directory` (resolve_against_directory):
    // every path mcpp writes is already absolute, so this is a no-op for a
    // fresh entry, but it is what makes an EXISTING entry from before this
    // resolution rule (or, in principle, one with a relative `file`) compare
    // correctly against it.
    std::set<std::string> freshFiles;
    for (auto const& e : freshJ) {
        auto resolved = resolve_against_directory(e, "file");
        if (!resolved.empty()) freshFiles.insert(dedup_key(resolved));
    }

    // Keep fresh order, then append still-valid prior entries the plan doesn't
    // cover (e.g. tests/ from a previous `mcpp test`). Drop entries for files
    // that no longer exist so the database never accrues dead references.
    nlohmann::json merged = freshJ;
    auto existingJ = nlohmann::json::parse(existing, nullptr, /*allow_exceptions=*/false);
    if (!existingJ.is_discarded() && existingJ.is_array()) {
        for (auto const& e : existingJ) {
            auto resolved = resolve_against_directory(e, "file");
            if (resolved.empty()) continue;
            if (freshFiles.contains(dedup_key(resolved))) continue;  // fresh wins
            if (!fileExists(resolved)) continue;                     // pruned
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

    auto doc = read_existing_document(path);
    if (!doc) return std::unexpected(doc.error());

    // 完全相同的有效输入不重写文件，避免 clangd 因 mtime 变化重复索引。
    if (doc->content && *doc->content == fresh)
        return CompileCommandsWriteResult{false, freshJson.size(), 0};

    std::string content(fresh);
    if (doc->content) {
        // 保留仍存在但当前 plan 未覆盖的条目，主要是之前 test 生成的 TU。
        content = merge_compile_commands(content, *doc->content, fileExists);
    }

    auto finalJson = nlohmann::json::parse(content, nullptr, /*allow_exceptions=*/false);
    if (finalJson.is_discarded() || !finalJson.is_array()) {
        return std::unexpected(write_error(std::format(
            "final compile database for '{}' is not a JSON array", path.string())));
    }
    sort_entries_by_file(finalJson);
    content = finalJson.dump(2) + "\n";

    if (doc->content && *doc->content == content)
        return CompileCommandsWriteResult{false, finalJson.size(), 0};

    if (auto r = atomic_replace_document(doc->publishPath, content, replaceFile); !r)
        return std::unexpected(r.error());

    return CompileCommandsWriteResult{true, finalJson.size(), 0};
}

std::expected<CompileCommandsWriteResult, CompileCommandsWriteError>
publish_root_compile_commands(
    const std::filesystem::path& configDbPath,
    const std::filesystem::path& rootPath,
    const std::filesystem::path& targetRoot,
    const std::filesystem::path& mcppHome,
    ReplaceFile replaceFile) {
    std::string content;
    {
        std::ifstream input(configDbPath, std::ios::binary);
        if (!input) {
            return std::unexpected(write_error(std::format(
                "cannot read configuration compile database '{}'", configDbPath.string())));
        }
        std::stringstream ss;
        ss << input.rdbuf();
        if (input.bad()) {
            return std::unexpected(write_error(std::format(
                "cannot read configuration compile database '{}'", configDbPath.string())));
        }
        content = ss.str();
    }

    auto configJson = nlohmann::json::parse(content, nullptr, /*allow_exceptions=*/false);
    if (configJson.is_discarded() || !configJson.is_array()) {
        return std::unexpected(write_error(std::format(
            "configuration compile database '{}' is not a JSON array",
            configDbPath.string())));
    }

    auto doc = read_existing_document(rootPath);
    if (!doc) return std::unexpected(doc.error());

    // §3.2 item 2: identical → untouched, so clangd is not triggered for
    // nothing (and the fast path costs one read, not one write, on a hit).
    if (doc->content && *doc->content == content)
        return CompileCommandsWriteResult{false, configJson.size(), 0};

    // §3.2 item 3: count the replaced file's foreign entries BEFORE replacing
    // it, so the caller can warn with a number. A prior file this parser
    // cannot read as a JSON array at all (another tool's own format, or one
    // corrupted) is replaced the same way, silently — there is no entry
    // count to report for a document that was never a list of entries.
    std::size_t foreign = 0;
    if (doc->content) {
        auto priorJson = nlohmann::json::parse(*doc->content, nullptr, false);
        if (!priorJson.is_discarded() && priorJson.is_array()) {
            for (auto const& e : priorJson)
                if (!is_mcpps_entry(e, targetRoot, mcppHome)) ++foreign;
        }
    }

    if (auto r = atomic_replace_document(doc->publishPath, content, replaceFile); !r)
        return std::unexpected(r.error());

    return CompileCommandsWriteResult{true, configJson.size(), foreign};
}

std::expected<CompileCommandsWriteResult, CompileCommandsWriteError>
write_compile_commands(const BuildPlan& plan, const CompileFlags& flags) {
    // A JSON document holds UTF-8 text only, and the serialiser throws on any
    // other byte (`type_error.316`). The entry points refuse or skip a path
    // with no UTF-8 spelling (#693), so a string reaching this point in another
    // encoding entered some other way. It fails this document, which the
    // caller reports as a warning or, when the database is required, as an
    // error, and never as an internal exception.
    std::string fresh;
    try {
        fresh = emit_compile_commands(plan, flags);
    } catch (const nlohmann::json::exception& e) {
        return std::unexpected(write_error(std::format(
            "it cannot be written as JSON, which holds UTF-8 text only ({})",
            e.what())));
    }

    auto fileExists = [](const std::filesystem::path& candidate) {
        return std::filesystem::exists(candidate);
    };

    // §3.2 item 1: the configuration's own database, merged within itself.
    auto configPath = plan.outputDir / "compile_commands.json";
    auto configResult = publish_compile_commands(configPath, fresh, fileExists);
    if (!configResult) return configResult;

    // §3.2 items 2-3: the root is a copy of it, replaced whole. `emit`
    // (`mcpp emit build-database`) never reaches this function (SPEC-005
    // R2.1: it writes nothing into the project), so in practice
    // `plan.compileDbPath` is always the project root's file here; the
    // fallback exists because the field itself is more general (plan.cppm).
    auto rootPath = plan.compileDbPath.empty()
        ? plan.projectRoot / "compile_commands.json"
        : plan.compileDbPath;
    auto targetRoot = plan.outputDir.parent_path().parent_path();
    auto rootResult = publish_root_compile_commands(
        configPath, rootPath, targetRoot, mcpp::home::root());
    if (!rootResult) return rootResult;

    return CompileCommandsWriteResult{
        rootResult->changed, configResult->commandCount, rootResult->foreignEntries};
}

}  // namespace mcpp::build
