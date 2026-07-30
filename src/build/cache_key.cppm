// mcpp.build.cache_key — per-package identity for the global build cache.
//
// The global dep cache used to be keyed by the WHOLE-PROJECT fingerprint
// (src/toolchain/fingerprint.cppm), whose flags field folds in every package in
// the graph *including the root* — its name, its version, its [build] flags.
// Consequences, all measured:
//
//   * bumping only the root's `version` changed the key ⇒ every dependency
//     entry and the std BMI were invalidated by `mcpp version bump`;
//   * two projects with identical dependencies and toolchain shared nothing,
//     because their package names differ;
//   * on one developer machine that produced 26 GB across 1198 fingerprint
//     directories: `compat.zlib@1.3.2` stored 162 times, 15 distinct std module
//     identities stored 1014 times.
//
// A dependency's artifacts do not depend on the consumer's identity. Verified:
// the root's `[build] cflags/cxxflags` are NOT passed to dependency translation
// units — a dependency compiles with its own buildConfig plus the shared
// toolchain and profile flags. So the key is built per package, from exactly
// the axes that reach that package's compiler command lines:
//
//   A toolchain   compiler id/version/driver identity, target triple, stdlib
//   B language    C++ standard + flag, dialect flags, C standard, macOS target
//   C profile     opt level / debug / lto / strip  ← was missing entirely
//   D identity    index name, package FQN, version
//   E own config  features, cflags/cxxflags/asmflags/ldflags, per-glob flags,
//                 defines, generated files, include dirs, source list
//   F upstream    the KEY of each direct dependency, recursively (Merkle)
//   G epoch       kCacheEpoch — cache-format compatibility, decoupled from the
//                 mcpp release number
//
// Why F is recursive rather than "the upstream's public includes + defines":
// GCC embeds a CRC of an imported module's BMI into the importer's BMI. Rebuild
// a dependency with a changed interface (or a changed ABI via a `-D`) and the
// importer's stale BMI hard-fails with `module 'B' CRC mismatch` /
// `Bad import dependency`. The importer's BMI is bound to the exact BMI it read,
// so what its key needs is the upstream's identity itself, not an enumeration
// of interface surface that can silently miss an item (re-exported transitive
// modules are not even visible in the upstream's own manifest). The failure
// modes are asymmetric, which settles the direction: too narrow a key on the
// BMI axis is a loud compiler error, while too narrow a key on the object axis
// is a silently wrong `.o` — objects carry no such self-check.
//
// The recursion's real cost is that an upstream's PRIVATE changes cascade
// downstream. For the population this cache serves that is ~zero: an index
// package's descriptor is frozen per version, so its key can only move via a
// version bump (which must invalidate consumers anyway — they link its objects)
// or via a whole-graph axis. Path and git packages, the only ones that can
// change private flags without a version bump, never enter the cache at all.

export module mcpp.build.cache_key;

import std;
import mcpp.libs.json;
import mcpp.manifest;
import mcpp.modgraph.scanner;
import mcpp.toolchain.detect;
import mcpp.toolchain.fingerprint;

export namespace mcpp::build::cache_key {

// Bump ONLY when a change makes previously written entries unusable (the
// serialized input shape, the artifact layout, or the staging contract).
// Deliberately NOT the mcpp release number: folding the whole version in
// orphaned every entry on every release, including plain C object files whose
// validity has nothing to do with mcpp's version.
inline constexpr int kCacheEpoch = 1;

// Axes A/B/C — identical for every package in one build, computed once.
struct BuildAxes {
    // A
    std::string compilerId;
    std::string compilerVersion;
    std::string driverIdentity;
    std::string targetTriple;
    std::string stdlibId;
    std::string stdlibVersion;
    // B
    std::string cppStandard;
    std::string cppStandardFlag;
    std::vector<std::string> dialectFlags;
    std::string cStandard;
    std::string macosDeploymentTarget;
    // C
    std::string optLevel;
    bool        debug = false;
    bool        lto   = false;
    bool        strip = false;
};

// Axes D/E/F for one package.
struct PackageAxes {
    // D
    std::string indexName;
    std::string packageName;
    std::string version;
    // E
    // Filled by the caller: the union of features requested of this package
    // over every incoming dependency edge, sorted. Their EFFECTS are already
    // folded into the vectors below (a feature contributes -DMCPP_FEATURE_*,
    // featureDefines, featureFlags and featureSources before the key is
    // computed), so this is redundant for correctness — it is here so
    // entry.json says which features an entry was built with, which is the
    // first thing anyone diagnosing a wrong hit will want.
    std::vector<std::string> features;
    std::vector<std::string> cflags;
    std::vector<std::string> cxxflags;
    std::vector<std::string> ldflags;
    std::vector<std::string> defines;
    std::vector<std::string> globFlags;     // pre-serialized, ordered
    std::vector<std::string> generatedFiles;// "path=content", ordered
    std::vector<std::string> includeDirs;   // store-relative, ordered
    std::vector<std::string> sourceGlobs;   // [build] sources, ordered
    std::vector<std::string> sources;       // package-root-relative, sorted
    // F — keys of direct dependencies, sorted
    std::vector<std::string> upstreamKeys;
};

// The canonical serialization. Also what lands in entry.json, so a cache hit
// can be validated field by field instead of trusting that equal hashes mean
// equal inputs.
nlohmann::json to_json(const BuildAxes& b, const PackageAxes& p);

// 16 hex chars, from the canonical serialization.
std::string key_hex(const BuildAxes& b, const PackageAxes& p);

// Axes A/B/C from a resolved toolchain + the root manifest (which is where the
// whole-graph language and profile settings live).
BuildAxes build_axes(const mcpp::toolchain::Toolchain& tc,
                     const mcpp::manifest::Manifest&   rootManifest,
                     std::string_view                  cppStandardFlag,
                     const std::vector<std::string>&   dialectFlags,
                     std::string_view                  macosDeploymentTarget);

// Axes E from one PackageRoot. `storeRoot` is stripped off absolute include
// dirs so the key survives a different MCPP_HOME (the payload paths are
// <mcppHome>/registry/data/xpkgs/...; leaving them absolute would make every
// entry a miss on another machine, or after a home relocation).
void fill_package_config(PackageAxes&                         out,
                         const mcpp::modgraph::PackageRoot&   pkg,
                         const std::filesystem::path&         storeRoot);

} // namespace mcpp::build::cache_key

namespace mcpp::build::cache_key {

namespace {

// Length-prefixed field joining. A plain separator would let ("a", "bc") and
// ("ab", "c") collide, which for a cache key means serving one package's
// objects for another's.
void put(std::string& s, std::string_view label, std::string_view value) {
    s += label;
    s += '=';
    s += std::to_string(value.size());
    s += ':';
    s += value;
    s += '\x1f';
}

void put_list(std::string& s, std::string_view label,
              const std::vector<std::string>& values) {
    s += label;
    s += '[';
    s += std::to_string(values.size());
    s += ']';
    for (auto& v : values) {
        s += '\x1e';
        s += std::to_string(v.size());
        s += ':';
        s += v;
    }
    s += '\x1f';
}

} // namespace

nlohmann::json to_json(const BuildAxes& b, const PackageAxes& p) {
    nlohmann::json j;
    j["epoch"] = kCacheEpoch;
    j["toolchain"] = {
        {"compiler", b.compilerId},
        {"compiler_version", b.compilerVersion},
        {"driver_identity", b.driverIdentity},
        {"target_triple", b.targetTriple},
        {"stdlib", b.stdlibId},
        {"stdlib_version", b.stdlibVersion},
    };
    j["language"] = {
        {"cpp_standard", b.cppStandard},
        {"cpp_standard_flag", b.cppStandardFlag},
        {"dialect_flags", b.dialectFlags},
        {"c_standard", b.cStandard},
        {"macos_deployment_target", b.macosDeploymentTarget},
    };
    j["profile"] = {
        {"opt_level", b.optLevel},
        {"debug", b.debug},
        {"lto", b.lto},
        {"strip", b.strip},
    };
    j["package"] = {
        {"index", p.indexName},
        {"name", p.packageName},
        {"version", p.version},
    };
    j["config"] = {
        {"features", p.features},
        {"cflags", p.cflags},
        {"cxxflags", p.cxxflags},
        {"ldflags", p.ldflags},
        {"defines", p.defines},
        {"glob_flags", p.globFlags},
        {"generated_files", p.generatedFiles},
        {"include_dirs", p.includeDirs},
        {"source_globs", p.sourceGlobs},
        {"sources", p.sources},
    };
    j["upstream"] = p.upstreamKeys;
    return j;
}

std::string key_hex(const BuildAxes& b, const PackageAxes& p) {
    std::string s = "mcpp-cache-key-v1\x1f";
    put(s, "epoch", std::to_string(kCacheEpoch));
    // A
    put(s, "cc",       b.compilerId);
    put(s, "ccver",    b.compilerVersion);
    put(s, "driver",   b.driverIdentity);
    put(s, "triple",   b.targetTriple);
    put(s, "stdlib",   b.stdlibId);
    put(s, "stdlibv",  b.stdlibVersion);
    // B
    put(s, "std",      b.cppStandard);
    put(s, "stdflag",  b.cppStandardFlag);
    put_list(s, "dialect", b.dialectFlags);
    put(s, "cstd",     b.cStandard);
    put(s, "macos",    b.macosDeploymentTarget);
    // C
    put(s, "opt",      b.optLevel);
    put(s, "debug",    b.debug ? "1" : "0");
    put(s, "lto",      b.lto   ? "1" : "0");
    put(s, "strip",    b.strip ? "1" : "0");
    // D
    put(s, "index",    p.indexName);
    put(s, "pkg",      p.packageName);
    put(s, "ver",      p.version);
    // E
    put_list(s, "features",  p.features);
    put_list(s, "cflags",    p.cflags);
    put_list(s, "cxxflags",  p.cxxflags);
    put_list(s, "ldflags",   p.ldflags);
    put_list(s, "defines",   p.defines);
    put_list(s, "globflags", p.globFlags);
    put_list(s, "genfiles",  p.generatedFiles);
    put_list(s, "includes",  p.includeDirs);
    put_list(s, "srcglobs",  p.sourceGlobs);
    put_list(s, "sources",   p.sources);
    // F
    put_list(s, "upstream",  p.upstreamKeys);
    return mcpp::toolchain::hash_string(s);
}

BuildAxes build_axes(const mcpp::toolchain::Toolchain& tc,
                     const mcpp::manifest::Manifest&   rootManifest,
                     std::string_view                  cppStandardFlag,
                     const std::vector<std::string>&   dialectFlags,
                     std::string_view                  macosDeploymentTarget)
{
    BuildAxes b;
    b.compilerId      = std::string(tc.compiler_name());
    b.compilerVersion = tc.version;
    // Same rule the whole-project fingerprint uses: prefer the declared driver
    // identity, else hash the driver binary. Two payloads of "gcc 16.1.0" that
    // are not the same build must not share a cache entry.
    b.driverIdentity  = !tc.driverIdent.empty()
        ? mcpp::toolchain::hash_string(tc.driverIdent)
        : (tc.binaryPath.empty() ? std::string{}
                                 : mcpp::toolchain::hash_file(tc.binaryPath));
    b.targetTriple    = tc.targetTriple;
    b.stdlibId        = tc.stdlibId;
    b.stdlibVersion   = tc.stdlibVersion;

    b.cppStandard     = rootManifest.package.standard;
    b.cppStandardFlag = std::string(cppStandardFlag);
    b.dialectFlags    = dialectFlags;
    b.cStandard       = rootManifest.buildConfig.cStandard;
    b.macosDeploymentTarget = std::string(macosDeploymentTarget);

    b.optLevel        = rootManifest.buildConfig.optLevel;
    b.debug           = rootManifest.buildConfig.debug;
    b.lto             = rootManifest.buildConfig.lto;
    b.strip           = rootManifest.buildConfig.strip;
    return b;
}

void fill_package_config(PackageAxes&                        out,
                         const mcpp::modgraph::PackageRoot&  pkg,
                         const std::filesystem::path&        storeRoot)
{
    const auto& bc = pkg.manifest.buildConfig;

    out.cflags      = bc.cflags;
    out.cxxflags    = bc.cxxflags;
    out.ldflags     = bc.ldflags;
    out.defines     = bc.defines;
    out.sourceGlobs = bc.sources;

    if (!bc.cStandard.empty()) {
        // A package may pin its own C standard; it reaches its own C units.
        out.cflags.push_back("__c_standard=" + bc.cStandard);
    }

    for (auto const& gf : bc.globFlags) {
        std::string one = "glob:" + gf.glob;
        for (auto const& f : gf.cflags)   one += "\x1egc:"  + f;
        for (auto const& f : gf.cxxflags) one += "\x1egxx:" + f;
        for (auto const& f : gf.asmflags) one += "\x1egas:" + f;
        for (auto const& f : gf.defines)  one += "\x1egd:"  + f;
        out.globFlags.push_back(std::move(one));
    }

    for (auto const& [path, content] : bc.generatedFiles) {
        out.generatedFiles.push_back(
            path.generic_string() + "=" + content);
    }
    std::ranges::sort(out.generatedFiles);

    // Include dirs are absolute payload paths under the xpkgs store; make them
    // store-relative so the key does not encode this machine's MCPP_HOME.
    auto storeStr = storeRoot.generic_string();
    auto relativize = [&](const std::filesystem::path& p) {
        auto s = p.generic_string();
        if (!storeStr.empty() && s.starts_with(storeStr))
            return "<store>" + s.substr(storeStr.size());
        auto pkgStr = pkg.root.generic_string();
        if (!pkgStr.empty() && s.starts_with(pkgStr))
            return "<pkg>" + s.substr(pkgStr.size());
        return s;
    };
    auto add_dirs = [&](const std::vector<std::filesystem::path>& dirs,
                        std::string_view tag) {
        for (auto& d : dirs)
            out.includeDirs.push_back(std::string(tag) + ":" + relativize(d));
    };
    if (pkg.usageResolved) {
        add_dirs(pkg.privateBuild.includeDirs,      "priv");
        add_dirs(pkg.publicUsage.includeDirs,       "pub");
        add_dirs(pkg.privateBuild.includeDirsAfter, "priv_after");
        add_dirs(pkg.publicUsage.includeDirsAfter,  "pub_after");
    }
    add_dirs(bc.includeDirs,      "decl");
    add_dirs(bc.includeDirsAfter, "decl_after");
}

} // namespace mcpp::build::cache_key
