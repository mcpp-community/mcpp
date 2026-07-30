// mcpp.bmi_cache — cross-project persistent cache of dependency build outputs.
//
// Layout:
//   <cache root>/pkg/<index>/<pkg>@<version>/<key16>/
//   (the cache root is $MCPP_HOME/build-cache/v1 — see mcpp.home::cache_root)
//     entry.json                       sentinel + self-description + file list
//     bmi/<module>.{gcm,pcm}
//     obj/<relative>.o
//
// <key16> comes from mcpp.build.cache_key: a per-package Merkle key over the
// toolchain, the language/dialect settings, the resolved profile, the package's
// own identity and build config, and — recursively — the keys of its direct
// dependencies. See that module's header for why each axis is there.
//
// Two properties this layout has and the previous one did not:
//
//  1. The directory name is derived only from things that actually reach the
//     package's compiler command lines. The old key was the whole-project
//     fingerprint, so a consumer's own name and version were part of every
//     dependency's cache path: `mcpp version bump` invalidated the entire cache
//     and no two projects ever shared an entry.
//
//  2. An entry describes itself. `is_cached` compares the recorded inputs
//     field by field against the inputs computed for this build, so a hit is
//     evidence rather than the mere existence of a directory. The std module
//     path has validated hits this way since it was written
//     (mcpp.toolchain.stdmod's metadata_matches); the dependency path carried
//     only a file list, which is why nothing could be audited when a wrong
//     entry was suspected.
//
// populate_from holds an advisory exclusive lock on the entry directory so two
// concurrent builds racing to fill the same entry cannot interleave writes, and
// writes entry.json last so a crash mid-populate leaves a miss, not a
// half-populated hit.

module;

export module mcpp.bmi_cache;

import std;
import mcpp.libs.json;
import mcpp.platform;

export namespace mcpp::bmi_cache {

// Schema of entry.json. Bumped only if the file's own shape changes;
// cache-content compatibility is carried by cache_key::kCacheEpoch, which
// travels inside `inputs`.
inline constexpr int kEntrySchema = 1;

struct CacheKey {
    // The resolved cache root (mcpp::home::cache_root()). Passed in rather than
    // recomputed here: the layout root must have exactly ONE definition, and a
    // second copy of "<home>/build-cache/v1" in this file is precisely the kind
    // of cross-file invariant a comment cannot enforce. Tests supply a temp
    // directory the same way.
    std::filesystem::path cacheRoot;
    std::string indexName;       // "mcpplibs" / "compat" / ...
    std::string packageName;     // "compat.zlib"
    std::string version;         // "1.3.2"
    std::string keyHex;          // cache_key::key_hex(...)
    // The full key inputs, recorded in entry.json and compared field by field
    // on a hit. Never trust equal hashes alone.
    nlohmann::json inputs;
    std::string bmiDirName   = "gcm.cache"; // consumer-side directory name
    std::string manifestTag  = "gcm";       // "gcm" | "pcm"

    std::filesystem::path dir() const {
        return cacheRoot / "pkg" / indexName
             / std::format("{}@{}", packageName, version) / keyHex;
    }

    std::filesystem::path entryFile() const { return dir() / "entry.json"; }
    std::filesystem::path bmiDir()    const { return dir() / "bmi"; }
    std::filesystem::path objDir()    const { return dir() / "obj"; }
};

// File names (basenames for BMIs, output-relative paths for objects) belonging
// to one package's cache entry.
struct DepArtifacts {
    std::vector<std::string> bmiFiles;
    std::vector<std::string> objFiles;
};

// True when entry.json exists, its schema matches, its recorded inputs equal
// `key.inputs` field for field, and every listed file is present on disk.
bool is_cached(const CacheKey& key);

// The artifact list of a validated entry. Does NOT copy anything: the ninja
// backend stages cached files through its own `stage_file` edges, so that a
// staged file is the output of an edge ninja has a command-line record for.
// Copying them behind ninja's back — which this module used to do — left every
// staged output with no entry in .ninja_log, and ninja treats that as dirty
// ("command line not found in log"), so every cached dependency was recompiled
// anyway while the CLI reported it as cached.
std::expected<DepArtifacts, std::string> resolve_cached(const CacheKey& key);

// Refresh the entry's `accessed` stamp. Rewrites entry.json only — never the
// artifacts, whose mtimes must stay put (ninja's restat handling compares them).
// This is what makes `mcpp cache gc` a real LRU: pruning used to read the
// directory's mtime, which only ever recorded when the entry was WRITTEN, so a
// dependency that hit on every build looked stale.
void touch_accessed(const CacheKey& key);

// Copy fresh build outputs from projectTarget/{bmiDirName,obj} into the entry,
// then write entry.json last as the sentinel.
std::expected<void, std::string>
populate_from(const CacheKey& key,
              const std::filesystem::path& projectTargetDir,
              const DepArtifacts& artifacts);

// Absolute paths of an entry's artifacts, for the stage edges.
std::filesystem::path cached_bmi_path(const CacheKey& key, std::string_view basename);
std::filesystem::path cached_obj_path(const CacheKey& key, std::string_view rel);

} // namespace mcpp::bmi_cache

namespace mcpp::bmi_cache {

namespace {

bool copy_one(const std::filesystem::path& from,
              const std::filesystem::path& to,
              std::error_code& ec)
{
    std::filesystem::create_directories(to.parent_path(), ec);
    std::filesystem::copy_file(from, to,
        std::filesystem::copy_options::overwrite_existing, ec);
    return !ec;
}

std::optional<nlohmann::json> read_entry(const std::filesystem::path& p) {
    std::ifstream is(p);
    if (!is) return std::nullopt;
    nlohmann::json j;
    try { is >> j; } catch (...) { return std::nullopt; }
    return j;
}

DepArtifacts artifacts_from(const nlohmann::json& j) {
    DepArtifacts a;
    if (auto it = j.find("bmi"); it != j.end() && it->is_array())
        for (auto& v : *it) if (v.is_string()) a.bmiFiles.push_back(v.get<std::string>());
    if (auto it = j.find("obj"); it != j.end() && it->is_array())
        for (auto& v : *it) if (v.is_string()) a.objFiles.push_back(v.get<std::string>());
    return a;
}

// Field-by-field, not `==` on the whole object: an entry written by an older
// mcpp may legitimately carry extra keys, but every key the CURRENT build cares
// about has to be present and equal. A missing key is a mismatch, never a pass.
bool inputs_match(const nlohmann::json& recorded, const nlohmann::json& expected) {
    if (!recorded.is_object() || !expected.is_object()) return false;
    for (auto it = expected.begin(); it != expected.end(); ++it) {
        auto found = recorded.find(it.key());
        if (found == recorded.end()) return false;
        if (*found != it.value()) return false;
    }
    return true;
}

std::string now_iso8601() {
    // No <chrono> zoned formatting: libstdc++'s `import std` support for
    // std::format on chrono types is partial (see fingerprint.cppm's
    // hand-rolled hex for the same reason). Seconds since epoch is monotonic
    // enough for an LRU stamp and needs no formatting support at all.
    auto now = std::chrono::system_clock::now();
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch()).count();
    return std::to_string(secs);
}

std::expected<void, std::string>
write_entry(const std::filesystem::path& path, const nlohmann::json& j) {
    auto tmp = path;
    tmp += ".tmp";
    {
        std::ofstream os(tmp, std::ios::binary);
        if (!os) return std::unexpected(std::format(
            "cannot write cache entry '{}'", tmp.string()));
        os << j.dump(2) << "\n";
        if (!os) return std::unexpected(std::format(
            "failed while writing cache entry '{}'", tmp.string()));
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) return std::unexpected(std::format(
        "cache entry rename: {}", ec.message()));
    return {};
}

} // namespace

std::filesystem::path cached_bmi_path(const CacheKey& key, std::string_view basename) {
    return key.bmiDir() / std::string(basename);
}

std::filesystem::path cached_obj_path(const CacheKey& key, std::string_view rel) {
    return key.objDir() / std::filesystem::path(std::string(rel));
}

bool is_cached(const CacheKey& key) {
    auto j = read_entry(key.entryFile());
    if (!j) return false;
    if (j->value("schema", 0) != kEntrySchema) return false;
    if (j->value("key", std::string{}) != key.keyHex) return false;
    auto it = j->find("inputs");
    if (it == j->end() || !inputs_match(*it, key.inputs)) return false;

    auto arts = artifacts_from(*j);
    std::error_code ec;
    for (auto& g : arts.bmiFiles)
        if (!std::filesystem::exists(cached_bmi_path(key, g), ec)) return false;
    for (auto& o : arts.objFiles)
        if (!std::filesystem::exists(cached_obj_path(key, o), ec)) return false;
    return true;
}

std::expected<DepArtifacts, std::string> resolve_cached(const CacheKey& key) {
    auto j = read_entry(key.entryFile());
    if (!j) return std::unexpected(std::format(
        "cannot read cache entry '{}'", key.entryFile().string()));
    return artifacts_from(*j);
}

void touch_accessed(const CacheKey& key) {
    auto j = read_entry(key.entryFile());
    if (!j) return;
    (*j)["accessed"] = now_iso8601();
    (void)write_entry(key.entryFile(), *j);
}

std::expected<void, std::string>
populate_from(const CacheKey& key,
              const std::filesystem::path& projectTargetDir,
              const DepArtifacts& arts)
{
    auto cacheDir = key.dir();
    std::error_code ec;
    std::filesystem::create_directories(cacheDir, ec);
    auto lock = mcpp::platform::fs::FileLock::try_acquire(cacheDir);
    if (!lock) {
        // Another writer holds the lock; it will finish the entry.
        return {};
    }

    auto cacheBmi = key.bmiDir();
    auto cacheObj = key.objDir();
    std::filesystem::create_directories(cacheBmi, ec);
    std::filesystem::create_directories(cacheObj, ec);

    auto projectBmi = projectTargetDir / key.bmiDirName;
    auto projectObj = projectTargetDir / "obj";

    for (auto& g : arts.bmiFiles) {
        auto from = projectBmi / g;
        if (!std::filesystem::exists(from)) {
            return std::unexpected(std::format(
                "expected build output missing: {}", from.string()));
        }
        if (!copy_one(from, cacheBmi / g, ec)) {
            return std::unexpected(std::format(
                "populate bmi '{}': {}", g, ec.message()));
        }
    }
    for (auto& o : arts.objFiles) {
        auto from = projectObj / o;
        if (!std::filesystem::exists(from)) {
            return std::unexpected(std::format(
                "expected build output missing: {}", from.string()));
        }
        if (!copy_one(from, cached_obj_path(key, o), ec)) {
            return std::unexpected(std::format(
                "populate obj '{}': {}", o, ec.message()));
        }
    }

    // entry.json LAST — it is the sentinel. Preserve `created` when refilling an
    // existing entry so gc's age reporting stays meaningful.
    nlohmann::json j;
    if (auto prev = read_entry(key.entryFile()); prev && prev->contains("created"))
        j["created"] = (*prev)["created"];
    else
        j["created"] = now_iso8601();
    j["schema"]   = kEntrySchema;
    j["key"]      = key.keyHex;
    j["package"]  = std::format("{}/{}@{}", key.indexName, key.packageName, key.version);
    j["bmi_dir"]  = key.bmiDirName;
    j["tag"]      = key.manifestTag;
    j["inputs"]   = key.inputs;
    j["bmi"]      = arts.bmiFiles;
    j["obj"]      = arts.objFiles;
    j["accessed"] = now_iso8601();
    return write_entry(key.entryFile(), j);
}

} // namespace mcpp::bmi_cache
