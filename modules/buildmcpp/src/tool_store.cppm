// mcpp.build.tool_store — where a dependency's HOST tools live, and how an
// entry gets filled.
//
// #355: a package can build a binary its consumers need at build time (protoc,
// grpc_cpp_plugin, flatc, moc, a transpiler). Until now a consumer had no way
// to reach it: `mcpp::dep_dir()` gives the dependency's SOURCE tree, and a
// dependency's `kind = "bin"` targets are never built at all (plan.cppm only
// walks the ROOT manifest's targets; the one exception, `kind = "shared"`, is
// built for the --target triple and so is useless as a host tool anyway).
//
// WHY IT CANNOT BE A NODE IN THE MAIN GRAPH
//
// Ordering. `build.mcpp` runs inside prepare (prepare.cppm's dep loop and root
// call site); the BuildPlan does not exist yet and build.ninja is written later
// still, in execute.cppm. Anything the main graph produces is therefore
// unavailable to the program that needs it. Add cross-compilation and it is not
// even the right binary: the main graph builds for --target, and a code
// generator has to run HERE.
//
// So the tool is produced by a NESTED build — the dependency package built as
// its own root, for the host, into a global store. That is Cargo's
// [build-dependencies], Bazel's exec configuration, vcpkg's `"host": true` and
// Conan's `tool_requires`, which is the whole industry's answer to this.
//
// WHAT MAKES IT CHEAP
//
// A tool is an EXECUTABLE, so it has zero ABI contact with the main build.
// The sub-build may therefore use the tool package's own [toolchain], its own
// profile, and its own resolution of its own dependencies — none of it has to
// agree with the consumer. (Contrast a `kind = "lib"` dependency, where every
// one of those must match.) That is what keeps this from needing a second
// coherent resolution universe.
//
// THE STORE IS THE INTERFACE
//
// An entry is `<key>/bin/<tool><exe>` plus an entry.json. How it gets filled is
// a provider detail: today `build-from-source` (a nested build) and `override`
// (the user pointed at an existing binary). A future `prebuilt-asset` provider
// — the descriptor ships a per-host binary, which is what protobuf upstream
// actually publishes — needs no change on the consumer side.
//
// See .agents/docs/2026-08-05-issue355-dependency-host-tools-design.md.

export module mcpp.build.tool_store;

import std;
import mcpp.libs.json;
import mcpp.manifest;
import mcpp.toolchain.fingerprint;

export namespace mcpp::build::tool_store {

// Bump ONLY when previously written entries become unusable (the layout or the
// key inputs changed shape). Deliberately not the mcpp release number: a tool
// binary's validity has nothing to do with mcpp's version.
inline constexpr int kEpoch = 1;

// How deep a tool request may nest before mcpp calls it a cycle. A tool
// package's own build.mcpp may legitimately need another tool (gRPC's needs
// protoc), so the limit cannot be 1 — but an unbounded chain is a bug, and
// hanging is a worse diagnostic than a named cycle.
inline constexpr int kMaxDepth = 4;

// One resolved tool: the package it came from, the target name, and the
// absolute path to the executable.
struct Tool {
    std::string           packageName;   // canonical FQN, e.g. "compat.protobuf"
    std::string           targetName;    // the [targets.X] name, e.g. "protoc"
    std::filesystem::path path;          // absolute path to the executable
    bool                  fromOverride = false;
};

// Everything that decides whether two requests are the same tool. Kept as a
// struct so the key inputs are recorded verbatim in entry.json and compared
// field by field on a hit — hash equality alone is never trusted (the
// discipline mcpp.bmi_cache established).
struct Key {
    int         epoch = kEpoch;
    std::string indexName;        // "compat" / "mcpplibs" / ...
    std::string packageName;      // canonical FQN
    std::string version;
    std::string targetName;
    std::string hostTriple;
    std::string compilerIdentity; // the HOST toolchain that will build it
    std::string profile;
    std::vector<std::string> features;   // resolved closure, sorted
    // The tool package's TRANSITIVE dependency closure, as sorted
    // `<name>@<version>` entries. Without it, bumping something the tool
    // depends on leaves a stale binary in the store — a silently wrong
    // artifact, the failure mode this project has paid for more than once.
    //
    // Transitive rather than direct-only: for an index package a frozen
    // version cannot change its own dependencies, so direct edges would do —
    // but a PATH dependency can, and then a change two levels down leaves the
    // tool's direct list untouched.
    std::vector<std::string> upstreamKeys;
};

std::string key_hex(const Key& k);
nlohmann::json to_json(const Key& k);

// THE IDENTITY OF A SOURCE TREE THAT HAS NO VERSION OF ITS OWN.
//
// An index package's `name@version` names immutable content, so the version
// alone identifies the bytes a tool was built from. A `path` package changes
// under an unchanged version, and a `git` package at a branch moves; the store
// keyed on the version alone then keeps a binary built from sources that no
// longer exist. Measured (2026-09-08, examples/12): an emitter change in a path
// tool package reached the consumer only after the package's version was
// bumped, and until then every build reported success over the previous
// compiler's output.
//
// A `git` package is keyed by its resolved commit, which is immutable content.
// A `path` package is keyed by this stamp: every regular file's relative path,
// size and modification time, in sorted order, hashed. It is what ninja itself
// trusts to decide a rebuild, costs one `stat` per file rather than a read,
// and moves in both directions -- an edit and its reversal each produce a new
// key, which is what the criterion demands. Build products, the version
// control directory and the engine's own scratch are excluded, since they
// change without the sources changing.
std::string tree_stamp(const std::filesystem::path& root);

// <cacheRoot>/tool/<index>/<pkg>@<ver>/<keyHex>/
std::filesystem::path entry_dir(const std::filesystem::path& cacheRoot, const Key& k);

// <cacheRoot>/tool/.build/<16 hex>/ -- where one consumer builds one entry.
//
// A SIBLING OF THE ENTRIES, NOT A CHILD OF ONE. The sub-build's own layout
// (`target/<triple>/<fingerprint>/obj/...`) is appended to this path, and the
// entry directory already spends its length on the index, the package, its
// version with the source stamp and the key: 133 characters on a Windows
// runner before the sub-build added its own (mcpp#641, item 3). The scratch
// needs none of those names, only an identity per (entry, consumer) pair, so
// that two projects building one tool at once do not share a ninja tree and a
// re-run reuses its own.
std::filesystem::path scratch_dir(const std::filesystem::path& cacheRoot,
                                  const std::filesystem::path& entryDir,
                                  const std::filesystem::path& consumerRoot);
std::filesystem::path bin_path(const std::filesystem::path& entryDir,
                               std::string_view toolName,
                               std::string_view exeSuffix);

// A complete, verified entry? (bin present AND entry.json records the same key
// inputs — never just the hash.)
bool entry_valid(const std::filesystem::path& entryDir, const Key& k,
                 std::string_view toolName, std::string_view exeSuffix);

void write_entry(const std::filesystem::path& entryDir, const Key& k);

// ── Overrides (the escape hatch every comparable system provides) ──────────
//
// vcpkg has VCPKG_HOST_TRIPLET, CMake projects have LLVM_NATIVE_TOOL_DIR and
// QT_HOST_PATH, Cargo has `target = "target"`. Without one, a user whose tool
// cannot be built from source — or who simply already has the right binary —
// has no way forward at all.
//
// Resolution order (first hit wins):
//   1. MCPP_TOOL_<SANITIZED_PKG>_<SANITIZED_TOOL>   (env; CI / distro packaging)
//   2. [tools.overrides] "<pkg>:<tool>" = "<path>"  (manifest)
//
// An override deliberately does NOT enter the store key: it is an escape
// hatch, not a reproducible input, and pretending otherwise would let a local
// path silently decide a cached artifact's identity.
std::optional<std::filesystem::path>
find_override(const mcpp::manifest::Manifest& rootManifest,
              std::string_view packageName, std::string_view shortName,
              std::string_view toolName);

// MCPP_FEATURE_-style sanitizer, shared with the env contract so a name is
// spelled the same on both sides.
std::string sanitize_env(std::string s);

// The env var name a build.mcpp reads for this tool.
std::string env_var_name(std::string_view packageName, std::string_view toolName);

} // namespace mcpp::build::tool_store

namespace mcpp::build::tool_store {

namespace fs = std::filesystem;

std::string sanitize_env(std::string s) {
    for (auto& c : s)
        c = std::isalnum(static_cast<unsigned char>(c))
          ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : '_';
    return s;
}

std::string env_var_name(std::string_view packageName, std::string_view toolName) {
    return "MCPP_DEP_" + sanitize_env(std::string(packageName))
         + "_BIN_" + sanitize_env(std::string(toolName));
}

nlohmann::json to_json(const Key& k) {
    nlohmann::json j;
    j["epoch"]             = k.epoch;
    j["index"]             = k.indexName;
    j["package"]           = k.packageName;
    j["version"]           = k.version;
    j["target"]            = k.targetName;
    j["host_triple"]       = k.hostTriple;
    j["compiler_identity"] = k.compilerIdentity;
    j["profile"]           = k.profile;
    j["features"]          = k.features;
    j["upstream_keys"]     = k.upstreamKeys;
    return j;
}

std::string tree_stamp(const fs::path& root) {
    std::vector<std::string> rows;
    std::error_code ec;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
    for (; it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        const auto& p = it->path();
        const auto name = p.filename().string();
        if (it->is_directory(ec)) {
            if (name == "target" || name == ".git" || name == ".mcpp") it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(ec)) continue;
        if (name == "compile_commands.json") continue;
        const auto rel = p.lexically_relative(root).generic_string();
        const auto sz  = fs::file_size(p, ec);
        const auto mt  = fs::last_write_time(p, ec).time_since_epoch().count();
        rows.push_back(std::format("{}|{}|{}", rel, sz, mt));
    }
    std::ranges::sort(rows);
    std::string joined;
    for (auto const& r : rows) { joined += r; joined += '\n'; }
    return mcpp::toolchain::hash_string(joined);
}

std::string key_hex(const Key& k) {
    return mcpp::toolchain::hash_string(to_json(k).dump());
}

fs::path entry_dir(const fs::path& cacheRoot, const Key& k) {
    return cacheRoot / "tool"
         / (k.indexName.empty() ? std::string("_") : k.indexName)
         / std::format("{}@{}", k.packageName, k.version)
         / key_hex(k);
}

fs::path scratch_dir(const fs::path& cacheRoot, const fs::path& entryDir,
                     const fs::path& consumerRoot) {
    // Identities, not narrow spellings: a home or project directory whose name
    // the Windows code page cannot spell must not throw here.
    const auto entry    = entryDir.lexically_normal().generic_u8string();
    const auto consumer = consumerRoot.lexically_normal().generic_u8string();
    std::string joined(reinterpret_cast<const char*>(entry.data()), entry.size());
    joined += '\n';
    joined.append(reinterpret_cast<const char*>(consumer.data()), consumer.size());
    return cacheRoot / "tool" / ".build" / mcpp::toolchain::hash_string(joined);
}

fs::path bin_path(const fs::path& entryDir, std::string_view toolName,
                  std::string_view exeSuffix) {
    return entryDir / "bin" / (std::string(toolName) + std::string(exeSuffix));
}

bool entry_valid(const fs::path& entryDir, const Key& k,
                 std::string_view toolName, std::string_view exeSuffix) {
    std::error_code ec;
    if (!fs::exists(bin_path(entryDir, toolName, exeSuffix), ec)) return false;
    std::ifstream is(entryDir / "entry.json");
    if (!is) return false;
    try {
        nlohmann::json recorded;
        is >> recorded;
        // Field-by-field, not hash-vs-hash: a directory named by a hash proves
        // only that someone once computed that hash. This is the same rule
        // bmi_cache follows and for the same reason.
        return recorded == to_json(k);
    } catch (...) {
        return false;
    }
}

void write_entry(const fs::path& entryDir, const Key& k) {
    std::error_code ec;
    fs::create_directories(entryDir, ec);
    std::ofstream os(entryDir / "entry.json", std::ios::trunc);
    if (os) os << to_json(k).dump(2) << '\n';
}

std::optional<fs::path>
find_override(const mcpp::manifest::Manifest& rootManifest,
              std::string_view packageName, std::string_view shortName,
              std::string_view toolName) {
    // 1. Environment — the CI / distro-packaging channel, and the one that
    //    works without editing a manifest you may not own.
    for (auto const& name : { std::string(packageName), std::string(shortName) }) {
        if (name.empty()) continue;
        auto var = "MCPP_TOOL_" + sanitize_env(name) + "_"
                 + sanitize_env(std::string(toolName));
        if (const char* v = std::getenv(var.c_str()); v && *v)
            return fs::path(v);
    }
    // 2. [tools.overrides] in the ROOT manifest. Accepts both the canonical
    //    and the namespace-stripped spelling, matching how `tools = [...]`
    //    itself may be written.
    for (auto const& name : { std::string(packageName), std::string(shortName) }) {
        if (name.empty()) continue;
        auto it = rootManifest.toolOverrides.find(
            std::format("{}:{}", name, toolName));
        if (it != rootManifest.toolOverrides.end() && !it->second.empty())
            return fs::path(it->second);
    }
    return std::nullopt;
}

} // namespace mcpp::build::tool_store
