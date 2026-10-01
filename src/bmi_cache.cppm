// mcpp.bmi_cache — cross-project persistent cache of dependency build outputs.
//
// Layout:
//   <cache root>/pkg/<index>/<pkg>@<version>/<key16>/
//   (the cache root is $MCPP_HOME/build-cache/v1 — see mcpp.home::cache_root)
//     entry.json                       sentinel + self-description + file list
//     bmi/<module>.{gcm,pcm}
//     obj/<package-internal path>.o
//
// The obj address is PACKAGE-INTERNAL: it mirrors the source's path relative to
// its own package root and contains nothing about the consuming project
// (mcpp#344). It used to be the consumer's build-dir path with `obj/` stripped,
// which made the layout depend on which OTHER packages the consumer happened to
// pull in — #233's basename disambiguation is triggered by a census over the
// whole build dir — while the key deliberately excludes the consumer. Two
// consumers then wrote and read incompatible layouts under one key and the
// second one died in ninja's graph phase.
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
//
// A SECOND KIND OF ENTRY IS PUBLISHED WHOLE (#748, B1). What a build program
// imports, the bundled `mcpp` module and the host modules of rule packages, is
// compiled in a directory of its own and then moved into place, so the entry
// never exists half-written and two threads (or two mcpp processes) compiling
// the same key cannot interleave their files. `stage_entry` creates that
// directory beside the entry, on the same file system, and `publish_staged`
// writes entry.json into it, last, and renames it to the entry's address. The
// entry has the layout above, so `probe_cached`, `touch_accessed`,
// `mcpp cache gc`, `list`, `info` and `verify` treat it as they treat any other.
// Its address is either below the package address, in the global cache, or
// exactly `CacheKey::directDir`, in a workspace's own store.

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
    // When set, the entry lives exactly here rather than below the package
    // address: a workspace's own store of what belongs to that workspace (a host
    // module from a path dependency, whose sources can change without its name
    // and version changing). Everything below it is laid out as in the global
    // cache. `cacheRoot` then names where staging directories go, and is the
    // workspace store's own root.
    std::filesystem::path directDir;

    std::filesystem::path dir() const {
        if (!directDir.empty()) return directDir;
        return cacheRoot / "pkg" / indexName
             / std::format("{}@{}", packageName, version) / keyHex;
    }

    std::filesystem::path entryFile() const { return dir() / "entry.json"; }
    std::filesystem::path bmiDir()    const { return dir() / "bmi"; }
    std::filesystem::path objDir()    const { return dir() / "obj"; }
};

// One cached object file. The two addresses are deliberately separate
// (mcpp#344):
//
//   cacheRel — where it lives INSIDE the entry (`<entry>/obj/<cacheRel>`).
//              Must be a pure function of the package, because the key
//              deliberately excludes the consumer. plan.cppm derives it.
//   buildRel — where THIS build produces it (`<buildDir>/<buildRel>`).
//              Consumer-side and therefore not recordable: two consumers of
//              one entry may legitimately place the same object at different
//              build-dir paths.
//
// Collapsing the two — recording the consumer's path as the entry's address —
// is exactly what made the second consumer of an entry fail with ninja's
// "missing and no known rule to make it". Only `cacheRel` ever reaches
// entry.json; `buildRel` is populate-time input and is empty on read-back.
struct ObjArtifact {
    std::string           cacheRel;
    std::filesystem::path buildRel;
};

// The artifacts belonging to one package's cache entry: BMI basenames plus the
// objects above.
// Where a BMI of an entry lies in the build's BMI directory when not at its
// name: below its package's directory, which is every package but the root's
// (pack drive and selection design 2026-10-01, B1). Read when the entry is
// populated, as `ObjArtifact::buildRel` is, and never written to entry.json,
// whose BMIs are named by module.
//
// A vector of this pair and not a `std::map<std::string, std::string,
// std::less<>>`: with that member in this struct, clang 22.1.8 crashed
// (SIGSEGV in ASTReader::readTypeRecord) compiling every importer of
// mcpp.build.prepare that instantiates a ranges algorithm of its own, on Linux
// and macOS alike, while GCC 16 compiled it.
struct BmiPlacement {
    std::string name;       // as `DepArtifacts::bmiFiles` names it
    std::string buildRel;   // relative to the build's BMI directory
};

struct DepArtifacts {
    std::vector<std::string>  bmiFiles;
    std::vector<BmiPlacement> bmiPlacements;
    std::vector<ObjArtifact>  objFiles;
};

// Why an entry could not serve this build.
struct CacheProbe {
    bool ok = false;
    // Non-empty ONLY when the entry itself validated (schema, key, inputs) but
    // does not carry the artifacts THIS build asked for. After mcpp#344 that
    // shape should be unreachable, which is precisely why it must be reported
    // rather than silently folded into "miss": a systematic recurrence would
    // otherwise present as "the cache simply never hits", with no signal at
    // all — the same failure mode as the fake `Cached` that went unnoticed for
    // three months.
    std::vector<std::string> layoutMismatch;
};

// Validate an entry AGAINST WHAT THIS BUILD WILL ACTUALLY READ.
//
// A hit requires all of: entry.json exists, its schema matches, its recorded
// key matches, its recorded inputs equal `key.inputs` field for field, and
// every artifact in `requested` is both listed by the entry and present on
// disk. Checking only the entry's OWN file list — which is what this used to
// do — validates a different question than the one the caller goes on to ask,
// and the two answers diverged the moment the object layout stopped being a
// function of the package alone.
//
// Anything short of a full match is a MISS. This function must never be the
// reason a build fails: an unusable entry costs a recompile, and the staging
// edges that would read it are never emitted.
CacheProbe probe_cached(const CacheKey& key, const DepArtifacts& requested);

// probe_cached(...).ok
bool is_cached(const CacheKey& key, const DepArtifacts& requested);

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

// A fresh directory a producer writes an entry's `bmi/` and `obj/` into, on the
// same file system as the entry so that `publish_staged` can rename it into
// place. It is outside every directory `mcpp cache` walks, so a producer that is
// interrupted leaves nothing for `gc` to report as an incomplete entry.
std::expected<std::filesystem::path, std::string>
stage_entry(const CacheKey& key);

// Writes entry.json into `staged`, last, and renames `staged` to the entry's
// address. `artifacts` lists what `staged` holds (`objFiles[].cacheRel` names
// the file below `obj/`; `buildRel` is not used). Returns true when this call
// put the entry in place and false when an entry that satisfies `artifacts` was
// already there, another thread's or another process's, in which case `staged`
// is removed and the entry in place is the one to use. An entry that is there
// and does not satisfy it (a crash that predates the rename, an older layout) is
// replaced.
std::expected<bool, std::string>
publish_staged(const CacheKey& key,
               const std::filesystem::path& staged,
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

// Read-back fills `cacheRel` only: `buildRel` is consumer-side and is not — and
// must not be — recorded in the entry.
DepArtifacts artifacts_from(const nlohmann::json& j) {
    DepArtifacts a;
    if (auto it = j.find("bmi"); it != j.end() && it->is_array())
        for (auto& v : *it) if (v.is_string()) a.bmiFiles.push_back(v.get<std::string>());
    if (auto it = j.find("obj"); it != j.end() && it->is_array())
        for (auto& v : *it)
            if (v.is_string()) a.objFiles.push_back({v.get<std::string>(), {}});
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

CacheProbe probe_cached(const CacheKey& key, const DepArtifacts& requested) {
    CacheProbe probe;
    auto j = read_entry(key.entryFile());
    if (!j) return probe;
    if (j->value("schema", 0) != kEntrySchema) return probe;
    if (j->value("key", std::string{}) != key.keyHex) return probe;
    auto it = j->find("inputs");
    if (it == j->end() || !inputs_match(*it, key.inputs)) return probe;

    // The entry itself is valid. From here on, every remaining check is about
    // whether it holds what THIS build is going to read.
    auto recorded = artifacts_from(*j);
    std::set<std::string> haveBmi(recorded.bmiFiles.begin(), recorded.bmiFiles.end());
    std::set<std::string> haveObj;
    for (auto& o : recorded.objFiles) haveObj.insert(o.cacheRel);

    std::error_code ec;
    for (auto& g : requested.bmiFiles) {
        if (!haveBmi.contains(g)
            || !std::filesystem::exists(cached_bmi_path(key, g), ec))
            probe.layoutMismatch.push_back(g);
    }
    for (auto& o : requested.objFiles) {
        if (!haveObj.contains(o.cacheRel)
            || !std::filesystem::exists(cached_obj_path(key, o.cacheRel), ec))
            probe.layoutMismatch.push_back(o.cacheRel);
    }
    probe.ok = probe.layoutMismatch.empty();
    return probe;
}

bool is_cached(const CacheKey& key, const DepArtifacts& requested) {
    return probe_cached(key, requested).ok;
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

    for (auto& g : arts.bmiFiles) {
        std::string rel = g;
        for (auto const& p : arts.bmiPlacements)
            if (p.name == g) { rel = p.buildRel; break; }
        auto from = projectBmi / rel;
        if (!std::filesystem::exists(from)) {
            return std::unexpected(std::format(
                "expected build output missing: {}", from.string()));
        }
        if (!copy_one(from, cacheBmi / g, ec)) {
            return std::unexpected(std::format(
                "populate bmi '{}': {}", g, ec.message()));
        }
    }
    // Read from `buildRel`, write at `cacheRel`. These are NOT the same path in
    // general (mcpp#344): the build-dir layout partitions objects by package,
    // the entry's layout is package-internal, and a source that sits outside its
    // package root is re-anchored for the entry. Deriving one from the other
    // here is what this split exists to prevent.
    for (auto& o : arts.objFiles) {
        auto from = projectTargetDir / o.buildRel;
        if (o.buildRel.empty() || !std::filesystem::exists(from)) {
            return std::unexpected(std::format(
                "expected build output missing: {}", from.string()));
        }
        if (!copy_one(from, cached_obj_path(key, o.cacheRel), ec)) {
            return std::unexpected(std::format(
                "populate obj '{}': {}", o.cacheRel, ec.message()));
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
    // Only the entry-internal addresses. Recording the consumer's build path
    // here is mcpp#344 in one line.
    {
        auto objs = nlohmann::json::array();
        for (auto& o : arts.objFiles) objs.push_back(o.cacheRel);
        j["obj"] = std::move(objs);
    }
    j["accessed"] = now_iso8601();
    return write_entry(key.entryFile(), j);
}

std::expected<std::filesystem::path, std::string>
stage_entry(const CacheKey& key) {
    // Unique across threads, processes and calls without asking the OS for a
    // process id: a counter for the threads of this process, and the clock and a
    // random number for the others.
    static std::atomic<unsigned long long> counter{0};
    static const unsigned long long salt = [] {
        std::random_device rd;
        return (static_cast<unsigned long long>(rd()) << 32) ^ rd();
    }();
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    // Short: the compiler is handed paths below this directory, and a path of a
    // few hundred characters is one some Windows tools cannot open.
    const auto unique = std::format("{:.8}.{:08x}.{}", key.keyHex,
                                    static_cast<unsigned>(salt ^ static_cast<unsigned long long>(ticks)),
                                    counter++);
    const auto base = key.directDir.empty()
        ? key.cacheRoot / "tmp"
        : key.directDir.parent_path() / ".tmp";
    const auto staged = base / unique;
    std::error_code ec;
    // A producer that was interrupted (Ctrl-C, a killed build) leaves its staging
    // directory behind, and nothing else removes it: no listing of the cache
    // reaches it. One that is a day old belongs to no producer still running.
    {
        const auto cutoff = std::filesystem::file_time_type::clock::now() - std::chrono::hours(24);
        std::error_code lec;
        for (auto const& old : std::filesystem::directory_iterator(base, lec)) {
            std::error_code tec;
            if (std::filesystem::last_write_time(old.path(), tec) < cutoff && !tec)
                std::filesystem::remove_all(old.path(), tec);
        }
    }
    std::filesystem::create_directories(staged / "bmi", ec);
    if (ec) return std::unexpected(std::format(
        "cannot create a staging directory under '{}': {}", base.string(), ec.message()));
    std::filesystem::create_directories(staged / "obj", ec);
    if (ec) return std::unexpected(std::format(
        "cannot create a staging directory under '{}': {}", base.string(), ec.message()));
    return staged;
}

std::expected<bool, std::string>
publish_staged(const CacheKey& key,
               const std::filesystem::path& staged,
               const DepArtifacts& arts)
{
    std::error_code ec;
    // Every artifact the entry is about to claim is in the directory now, before
    // anything names it.
    for (auto& g : arts.bmiFiles)
        if (!std::filesystem::exists(staged / "bmi" / g, ec)) {
            std::filesystem::remove_all(staged, ec);
            return std::unexpected(std::format("expected build output missing: bmi/{}", g));
        }
    for (auto& o : arts.objFiles)
        if (!std::filesystem::exists(staged / "obj" / std::filesystem::path(o.cacheRel), ec)) {
            std::filesystem::remove_all(staged, ec);
            return std::unexpected(std::format("expected build output missing: obj/{}", o.cacheRel));
        }

    nlohmann::json j;
    j["created"]  = now_iso8601();
    j["schema"]   = kEntrySchema;
    j["key"]      = key.keyHex;
    j["package"]  = std::format("{}/{}@{}", key.indexName, key.packageName, key.version);
    j["bmi_dir"]  = key.bmiDirName;
    j["tag"]      = key.manifestTag;
    j["inputs"]   = key.inputs;
    j["bmi"]      = arts.bmiFiles;
    {
        auto objs = nlohmann::json::array();
        for (auto& o : arts.objFiles) objs.push_back(o.cacheRel);
        j["obj"] = std::move(objs);
    }
    j["accessed"] = now_iso8601();
    // entry.json LAST: it is the sentinel, and it is written into the directory
    // before the directory has an address, so the entry is never seen without it.
    if (auto w = write_entry(staged / "entry.json", j); !w) {
        std::filesystem::remove_all(staged, ec);
        return std::unexpected(w.error());
    }

    const auto target = key.dir();
    std::filesystem::create_directories(target.parent_path(), ec);
    std::filesystem::rename(staged, target, ec);
    if (!ec) return true;

    // The target exists (a directory is not replaced by a rename). Another
    // producer finished first, and its entry is as good as this one when it
    // satisfies what was asked: use it.
    if (probe_cached(key, arts).ok) {
        std::filesystem::remove_all(staged, ec);
        return false;
    }
    // It does not, so it is a remnant, and the next step replaces it.
    std::filesystem::remove_all(target, ec);
    std::filesystem::rename(staged, target, ec);
    if (!ec) return true;
    // A third producer may have won in between; the entry is usable then.
    if (probe_cached(key, arts).ok) {
        std::error_code rm;
        std::filesystem::remove_all(staged, rm);
        return false;
    }
    std::error_code rm;
    std::filesystem::remove_all(staged, rm);
    return std::unexpected(std::format(
        "cannot publish cache entry '{}': {}", target.string(), ec.message()));
}

} // namespace mcpp::bmi_cache
