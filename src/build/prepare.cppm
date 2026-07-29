// mcpp.build.prepare — BuildContext + prepare_build: the build-orchestration
// core (workspace -> toolchain -> dependency resolution -> features ->
// modgraph -> fingerprint -> plan -> lockfile).
// Bodies moved verbatim from the CLI layer. Zero behavior change.

module;
#include <cstdio>
#include <cstdlib>

export module mcpp.build.prepare;

import std;
import mcpp.diag;
import mcpp.platform.axis;
import mcpp.libs.json;
import mcpp.log;
import mcpp.manifest;
import mcpp.modgraph.graph;
import mcpp.modgraph.scanner;
import mcpp.modgraph.validate;
import mcpp.toolchain.clang;
import mcpp.toolchain.cppfly;
import mcpp.toolchain.detect;
import mcpp.toolchain.dialect;
import mcpp.toolchain.fingerprint;
import mcpp.toolchain.msvc;
import mcpp.toolchain.registry;
import mcpp.toolchain.stdmod;
import mcpp.toolchain.post_install;
import mcpp.toolchain.abi;
import mcpp.toolchain.triple;
import mcpp.build.plan;
import mcpp.build.build_program;
import mcpp.lockfile;
import mcpp.config;
import mcpp.xlings;
import mcpp.platform;
import mcpp.fetcher;
import mcpp.fetcher.progress;
import mcpp.pm.resolver;
import mcpp.pm.index_spec;
import mcpp.pm.index_route;
import mcpp.pm.mangle;
import mcpp.pm.compat;
import mcpp.pm.dep_spec;
import mcpp.version_req;
import mcpp.ui;
import mcpp.log;
import mcpp.fallback.install_integrity;
import mcpp.bmi_cache;
import mcpp.project;

namespace mcpp::build {

// mcpp#237: surface xpkg-descriptor mcpp-segment keys this mcpp did not
// recognise. The parser collects them into `xpkgUnknownKeys` and skips the
// value; without this a typo like `dependencies = {...}` (correct key: `deps`)
// dropped the dependency with no diagnostic. Called at the descriptor-adoption
// sites (a fetched dep with no mcpp.toml, synthesized from the index `mcpp={}`
// block) — the single place the descriptor becomes a build input. Warning (not
// hard error) keeps forward-compat: an older mcpp building a newer descriptor
// should not fail outright, only tell the user what it ignored.
inline void warn_unknown_xpkg_keys(const mcpp::manifest::Manifest& dm,
                                   std::string_view depLabel) {
    for (auto const& key : dm.xpkgUnknownKeys) {
        auto suggestion = mcpp::manifest::closest_known_xpkg_key(key);
        if (suggestion.empty())
            mcpp::ui::warning(std::format(
                "dependency '{}': unknown mcpp-segment key '{}' in its xpkg "
                "descriptor — ignored (schema mismatch or typo)", depLabel, key));
        else
            mcpp::ui::warning(std::format(
                "dependency '{}': unknown mcpp-segment key '{}' in its xpkg "
                "descriptor — ignored; did you mean '{}'?", depLabel, key, suggestion));
    }
}

// ── L1 platform-conditional config: cfg() predicate evaluation ──────────────
// Context = the RESOLVED target's coordinates. A `[target.'cfg(...)'.build]`
// predicate is evaluated against this (target triple for a cross build, host
// for a native build), so conditional flags follow what the binary will run on
// — not the build host. See the manifest design doc.
namespace cfgpred {

struct Ctx { std::string os, arch, family, env; };

// Derive the cfg context from the resolved --target triple, falling back to
// the host for a native build. Parsing goes through triple.cppm — the single
// triple parser — so the cfg vocabulary IS the canonical triple vocabulary
// (os: linux|macos|windows, arch: GNU spellings, env: gnu|musl|msvc), and
// alias spellings ("x86_64-w64-mingw32") evaluate identically to canonical.
inline Ctx context_for(std::string_view targetTriple) {
    namespace triple = mcpp::toolchain::triple;
    Ctx c;
    auto t = targetTriple.empty()
        ? std::optional<triple::Triple>(triple::host_triple())
        : triple::parse(targetTriple);
    if (t) {
        c.os     = t->os;
        c.arch   = t->arch;
        c.env    = t->env;
        c.family = t->family();
    } else {
        // Escape-hatch triple outside the language: only the leading arch
        // segment is derivable; other dimensions stay empty (never match).
        auto dash = targetTriple.find('-');
        c.arch = std::string(dash == std::string_view::npos ? targetTriple
                                                            : targetTriple.substr(0, dash));
    }
    return c;
}

// Recursive-descent evaluator over the inside of `cfg(...)`:
//   expr := all(list) | any(list) | not(expr) | key="value" | bareword
//   key  ∈ {os, arch, family, env}   bareword ∈ {windows, unix, linux, macos}
struct Parser {
    std::string_view s; std::size_t i = 0; const Ctx& c;
    void ws() { while (i < s.size() && std::isspace((unsigned char)s[i])) ++i; }
    bool eat(char ch) { ws(); if (i < s.size() && s[i] == ch) { ++i; return true; } return false; }
    std::string ident() {
        ws(); std::size_t b = i;
        while (i < s.size() && (std::isalnum((unsigned char)s[i]) || s[i] == '_')) ++i;
        return std::string(s.substr(b, i - b));
    }
    std::string str() {
        ws(); if (i >= s.size() || s[i] != '"') return {};
        ++i; std::size_t b = i; while (i < s.size() && s[i] != '"') ++i;
        auto v = std::string(s.substr(b, i - b)); if (i < s.size()) ++i; return v;
    }
    bool match_alias(const std::string& a) {
        if (a == "windows") return c.os == "windows";
        if (a == "linux")   return c.os == "linux";
        if (a == "macos")   return c.os == "macos";
        if (a == "unix")    return c.family == "unix";
        return false;  // unknown bareword → no match
    }
    bool match_kv(const std::string& k, const std::string& v) {
        if (k == "os")     return c.os == v;
        if (k == "arch")   return c.arch == v;
        if (k == "family") return c.family == v;
        if (k == "env")    return c.env == v;
        return false;
    }
    bool expr() {
        std::string id = ident();
        if (id == "all" || id == "any") {
            eat('(');
            bool acc = (id == "all");
            ws();
            if (!(i < s.size() && s[i] == ')')) {
                do { bool r = expr(); acc = (id == "all") ? (acc && r) : (acc || r); }
                while (eat(','));
            }
            eat(')');
            return acc;
        }
        if (id == "not") { eat('('); bool r = expr(); eat(')'); return !r; }
        ws();
        if (i < s.size() && s[i] == '=') { ++i; return match_kv(id, str()); }
        return match_alias(id);
    }
};

// Evaluate a `[target.<predicate>]` key. Returns the cfg() result, or — for a
// non-cfg key (a bare triple) — an exact match against the resolved triple.
inline bool matches(const std::string& predicate, const Ctx& c, std::string_view triple) {
    std::string_view k = predicate;
    if (k.starts_with("cfg(") && k.ends_with(")")) {
        Parser p{ k.substr(4, k.size() - 5), 0, c };
        return p.expr();
    }
    // Bare OS/family alias sugar: `[target.linux]` ≡ `[target.'cfg(linux)']`.
    // These aliases are never valid triples (no dash), so there is no ambiguity
    // with the exact-triple namespace. Evaluated as the cfg bareword.
    if (predicate == "windows" || predicate == "linux" ||
        predicate == "macos"   || predicate == "unix") {
        Parser p{ predicate, 0, c };
        return p.expr();
    }
    // Bare-triple match, spelling-independent: a `[target.x86_64-w64-mingw32]`
    // key matches a resolved `x86_64-windows-gnu` build (and vice versa) —
    // both normalize through triple::parse. Unparseable keys (the explicit-
    // section escape hatch) fall back to exact string comparison.
    if (triple.empty()) return false;
    if (auto p = mcpp::toolchain::triple::parse(predicate)) {
        if (auto rt = mcpp::toolchain::triple::parse(triple))
            return p->str() == rt->str();
    }
    return predicate == triple;
}

}  // namespace cfgpred

export std::filesystem::path target_dir(const mcpp::toolchain::Toolchain& tc,
                                 const mcpp::toolchain::Fingerprint& fp,
                                 const std::filesystem::path& root)
{
    // Canonical triple names the output directory (D1: `target/
    // x86_64-windows-gnu/`, not the GNU spelling the compiler reports via
    // -dumpmachine) — alias inputs land in the same directory. Triples
    // outside the language keep their raw spelling.
    auto triple = tc.targetTriple.empty() ? std::string{"unknown"} : tc.targetTriple;
    if (auto t = mcpp::toolchain::triple::parse(triple)) triple = t->str();
    return root / "target" / triple / fp.hex;
}


// Compose a stable canonical compile-flags string for fingerprinting.
std::string canonical_compile_flags(const mcpp::manifest::Manifest& m) {
    std::string s;
    s += "-std="; s += m.package.standard;
    s += " -fmodules";
    // macOS deployment target changes the effective compile triple
    // (arm64-apple-macosxNN) — a std.pcm built for one target cannot be
    // loaded by a TU compiled for another. Fold the resolved value
    // (env override > [build] macos_deployment_target manifest default)
    // into the fingerprint so switching targets rebuilds the BMI cache
    // instead of dying with a module config mismatch.
    //
    // The built-in default floor (rustc-style) lives in the single
    // resolver (platform::macos::deployment_target), so this rule, the
    // flags and the std-module prebuild always agree — the 0.0.50-era
    // attempt to inject a default here alone left the test build's
    // std.pcm unstaged (import std failed wholesale on macos CI).
    if constexpr (mcpp::platform::is_macos) {
        auto dtv = mcpp::platform::macos::deployment_target(
            m.buildConfig.macosDeploymentTarget);
        if (!dtv.empty()) {
            s += " macos_deployment_target=";
            s += dtv;
        }
    }
    if (!m.buildConfig.cStandard.empty()) {
        s += " c_standard=";
        s += m.buildConfig.cStandard;
    }
    for (auto const& flag : m.buildConfig.cflags) {
        s += " cflag:";
        s += flag;
    }
    for (auto const& flag : m.buildConfig.cxxflags) {
        s += " cxxflag:";
        s += flag;
    }
    // Explicit [build] dialect_cxxflags (auto-promoted ones are already in
    // cxxflags above) — they change every BMI in the graph.
    for (auto const& flag : m.buildConfig.dialectCxxflags) {
        s += " dialect:";
        s += flag;
    }
    for (auto const& flag : m.buildConfig.ldflags) {
        s += " ldflag:";
        s += flag;
    }
    // Per-glob flags (G4): full ordered serialization — glob + every list —
    // so editing any entry (or reordering) re-fingerprints the output dir.
    for (auto const& gf : m.buildConfig.globFlags) {
        s += " globflags:"; s += gf.glob;
        for (auto const& f : gf.cflags)   { s += " gc:";  s += f; }
        for (auto const& f : gf.cxxflags) { s += " gxx:"; s += f; }
        for (auto const& f : gf.asmflags) { s += " gas:"; s += f; }
        for (auto const& f : gf.defines)  { s += " gd:";  s += f; }
    }
    return s;
}

std::string canonical_package_build_metadata(
    const std::vector<mcpp::modgraph::PackageRoot>& packages)
{
    std::string s;
    for (auto const& pkg : packages) {
        s += "\npackage:";
        s += pkg.manifest.package.namespace_;
        s += "/";
        s += pkg.manifest.package.name;
        s += "@";
        s += pkg.manifest.package.version;
        if (!pkg.manifest.buildConfig.cStandard.empty()) {
            s += " c_standard=";
            s += pkg.manifest.buildConfig.cStandard;
        }
        for (auto const& flag : pkg.manifest.buildConfig.cflags) {
            s += " cflag:";
            s += flag;
        }
        for (auto const& flag : pkg.manifest.buildConfig.cxxflags) {
            s += " cxxflag:";
            s += flag;
        }
        for (auto const& flag : pkg.manifest.buildConfig.ldflags) {
            s += " ldflag:";
            s += flag;
        }
        // Per-glob flags — same full ordered serialization as the root-side
        // block above. Until #253 dependency globFlags were unfingerprinted
        // (held only by "descriptor frozen per version" + "feature toggles
        // always change cflags via -DMCPP_FEATURE_*"); feature-folded entries
        // make the vector build-variant, so fingerprint it directly.
        // featureOrigin is diagnostic-only and deliberately NOT serialized
        // (the active feature set is already in cflags above).
        for (auto const& gf : pkg.manifest.buildConfig.globFlags) {
            s += " globflags:"; s += gf.glob;
            for (auto const& f : gf.cflags)   { s += " gc:";  s += f; }
            for (auto const& f : gf.cxxflags) { s += " gxx:"; s += f; }
            for (auto const& f : gf.asmflags) { s += " gas:"; s += f; }
            for (auto const& f : gf.defines)  { s += " gd:";  s += f; }
        }
        if (pkg.usageResolved) {
            for (auto const& dir : pkg.privateBuild.includeDirs) {
                s += " private_include:";
                s += dir.generic_string();
            }
            for (auto const& dir : pkg.publicUsage.includeDirs) {
                s += " public_include:";
                s += dir.generic_string();
            }
            for (auto const& dir : pkg.privateBuild.includeDirsAfter) {
                s += " private_include_after:";
                s += dir.generic_string();
            }
            for (auto const& dir : pkg.publicUsage.includeDirsAfter) {
                s += " public_include_after:";
                s += dir.generic_string();
            }
        }
        for (auto const& [path, content] : pkg.manifest.buildConfig.generatedFiles) {
            s += " genfile:";
            s += path.generic_string();
            s += "=";
            s += content;
        }
    }
    return s;
}

std::expected<void, std::string>
materialize_generated_files(const std::filesystem::path& root,
                            const mcpp::manifest::Manifest& manifest)
{
    for (auto const& [relPath, content] : manifest.buildConfig.generatedFiles) {
        if (relPath.empty()) {
            return std::unexpected("generated_files contains an empty path");
        }
        if (relPath.is_absolute()) {
            return std::unexpected(std::format(
                "generated_files path '{}' must be relative", relPath.generic_string()));
        }
        auto const genericPath = relPath.generic_string();
        for (std::size_t begin = 0; begin <= genericPath.size();) {
            auto const end = genericPath.find('/', begin);
            auto const part = genericPath.substr(begin, end == std::string::npos
                                                           ? std::string::npos
                                                           : end - begin);
            if (part == "..") {
                return std::unexpected(std::format(
                    "generated_files path '{}' must not escape the package root",
                    relPath.generic_string()));
            }
            if (end == std::string::npos) {
                break;
            }
            begin = end + 1;
        }

        auto out = root / relPath.lexically_normal();
        std::error_code ec;
        std::filesystem::create_directories(out.parent_path(), ec);
        if (ec) {
            return std::unexpected(std::format(
                "cannot create directory for generated file '{}': {}",
                out.string(), ec.message()));
        }

        // Skip the write when the on-disk content is already identical: ninja
        // is mtime-driven, and an unconditional rewrite bumps the mtime every
        // build, recompiling every TU that #includes the materialized file
        // (via depfiles) — e.g. a frozen-snapshot config.h included by
        // thousands of TUs. Change detection is already owned by the
        // fingerprint (content is folded in above), so skipping only
        // preserves the mtime — mirroring the build.mcpp cache design,
        // which likewise avoids mtime churn on unchanged outputs.
        {
            std::ifstream is(out, std::ios::binary);
            if (is) {
                std::string existing((std::istreambuf_iterator<char>(is)),
                                     std::istreambuf_iterator<char>());
                if (is && existing == content) {
                    continue;
                }
            }
        }

        std::ofstream os(out, std::ios::binary);
        if (!os) {
            return std::unexpected(std::format(
                "cannot write generated file '{}'", out.string()));
        }
        os << content;
        if (!os) {
            return std::unexpected(std::format(
                "failed while writing generated file '{}'", out.string()));
        }
    }
    return {};
}

// L1 cfg merge for ONE package's manifest (root or ANY dependency — path,
// git, or version/registry): append the matching conditional
// cflags/cxxflags/ldflags and sources (G1b) to its buildConfig. Sources also
// update the legacy modules.sources mirror — the scanner walks that.
//
// #229: this is the SINGLE funnel for cfg-conditional sources/flags — every
// package's manifest passes through exactly one call to this function,
// always immediately BEFORE that manifest is captured into `packages[]` via
// makePackageRoot()/propagateLinkFlags() (which snapshot buildConfig into
// privateBuild/linkUsage and into the root's propagated ldflags — merging
// any later than that point is silently lost for flags, though not for
// sources, which the modgraph scan re-reads live). Three call sites, one per
// loading branch, together cover every package exactly once: the root
// (before its own makePackageRoot), the path/git-dep branch, and
// loadVersionDep() (shared by the main per-dependency loop, the
// multi-version mangling secondary, and the SemVer-merge re-fetch — all three
// of ITS callers get the merge for free from the one call inside it).
// (Conditional *dependencies* are a separate, root-only concern: they must be
// merged into the dependency map BEFORE resolution even starts, so a
// dependency's own conditional deps are out of scope — see the root cfg
// block that merges `cc.dependencies` etc.)
void merge_conditional_build_inputs(mcpp::manifest::Manifest& m,
                             const cfgpred::Ctx& ctx,
                             std::string_view targetTriple)
{
    for (auto const& cc : m.conditionalConfigs) {
        if (!cfgpred::matches(cc.predicate, ctx, targetTriple)) continue;
        // One append() for every field the axis may carry (#258). Matching
        // sections land AFTER the base entries, so a conditional rule beats
        // a broader unconditional one under GNU last-wins — which is what
        // makes an off-OS REMOVAL expressible (`-U` after the base `-D`).
        mcpp::manifest::append(m.buildConfig, cc.inputs);
        // `modules.sources` is the scanner's own view and is not part of
        // BuildInputs, so conditional sources are mirrored into it here.
        for (auto const& s : cc.inputs.sources)
            m.modules.sources.push_back(s);
    }
}

// Desugar `[build].defines` into `-D<x>` on both C and C++ flag channels.
//
// ORDER (both halves are load-bearing): this must run AFTER
// merge_conditional_build_inputs — `defines` is a BuildInputs member, so a
// matching `[target.'cfg(...)'.build] defines` has been appended by then and
// folds in the same pass, landing after the unconditional entries so GNU
// last-wins gives the conditional rule precedence — and BEFORE the manifest is
// snapshotted into packages[] / fingerprinted, because that snapshot (not the
// manifest) is what the P1689 scan, the compile edges and compute_fingerprint
// actually read.
//
// Idempotent: clearing the vector after folding makes repeated calls harmless.
// Both `cflags` and `cxxflags` get the macro; assembly units pick it up for
// free via the -D/-U/-I subset the ninja backend filters out of packageCflags.
void fold_build_defines_into_flags(mcpp::manifest::BuildConfig& bc) {
    for (auto const& d : bc.defines) {
        bc.cflags.push_back("-D" + d);
        bc.cxxflags.push_back("-D" + d);
    }
    bc.defines.clear();
}

// Feature-activation closure — THE single implementation (build.mcpp env
// contract, Stage 2a feature-deps, and the main feature pass all call this):
// seed = [features].default ∪ requested, expanded transitively over implies;
// the literal name "default" is never itself a feature.
//
// `seedDefault` is the funnel for consumer-side `default-features = false`
// (#242, Cargo parity): when false the dependency's own `[features].default`
// is NOT seeded, so only the explicitly `requested` features (and their
// transitive `implies`) activate. The root package always seeds its own
// default (seedDefault=true); a dependency passes its dep spec's
// `defaultFeatures` flag. `requested` is applied identically either way.
std::vector<std::string> feature_closure(const mcpp::manifest::Manifest& pm,
                                         const std::vector<std::string>& requested,
                                         bool seedDefault = true)
{
    std::vector<std::string> act, q;
    if (seedDefault)
        if (auto it = pm.featuresMap.find("default"); it != pm.featuresMap.end())
            q.insert(q.end(), it->second.begin(), it->second.end());
    q.insert(q.end(), requested.begin(), requested.end());
    std::set<std::string> seen;
    while (!q.empty()) {
        auto f = q.back(); q.pop_back();
        if (f == "default" || !seen.insert(f).second) continue;
        act.push_back(f);
        if (auto it = pm.featuresMap.find(f); it != pm.featuresMap.end())
            q.insert(q.end(), it->second.begin(), it->second.end());
    }
    return act;
}

// --features value → tokens (comma/space separated).
std::vector<std::string> parse_feature_request(std::string_view s) {
    std::vector<std::string> out;
    for (std::size_t p = 0; p < s.size();) {
        auto c = s.find_first_of(", ", p);
        auto tok = s.substr(p, c == std::string_view::npos ? std::string_view::npos : c - p);
        if (!tok.empty()) out.emplace_back(tok);
        if (c == std::string_view::npos) break;
        p = c + 1;
    }
    return out;
}

bool is_std_module(std::string_view name) {
    return name == "std" || name == "std.compat";
}

std::string trim_copy(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.erase(0, 1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.pop_back();
    return s;
}

bool source_file_imports_std(const std::filesystem::path& path) {
    std::ifstream is(path);
    if (!is) return false;

    std::string line;
    while (std::getline(is, line)) {
        line = trim_copy(std::move(line));
        std::size_t i = std::string::npos;
        if (line.starts_with("import ")) {
            i = 7;
        } else if (line.starts_with("export import ")) {
            i = 14;
        }
        if (i == std::string::npos) continue;
        while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i])))
            ++i;

        std::string name;
        while (i < line.size()
            && (std::isalnum(static_cast<unsigned char>(line[i]))
                || line[i] == '_' || line[i] == '.' || line[i] == ':')) {
            name.push_back(line[i]);
            ++i;
        }
        if (is_std_module(name)) return true;
    }
    return false;
}

bool graph_or_targets_import_std(const mcpp::modgraph::Graph& graph,
                                 const mcpp::manifest::Manifest& manifest,
                                 const std::filesystem::path& projectRoot) {
    for (auto& u : graph.units) {
        for (auto& req : u.requires_) {
            if (is_std_module(req.logicalName))
                return true;
        }
    }

    // Some target entry files can be added to the plan after the package scan.
    // Check them here so std BMI setup matches what make_plan will compile.
    for (auto& t : manifest.targets) {
        if (!t.main.empty() && source_file_imports_std(projectRoot / t.main))
            return true;
    }
    return false;
}

export struct BuildContext {
    // --strict: degradations reported through mcpp::diag become errors.
    // Carried on the context because the build's degradations are discovered
    // during backend emission, i.e. after prepare_build has returned — the
    // single place that settles the policy is run_build_plan (execute.cppm).
    bool                            strict = false;
    mcpp::manifest::Manifest        manifest;
    mcpp::toolchain::Toolchain      tc;
    mcpp::toolchain::Fingerprint    fp;
    std::filesystem::path           projectRoot;
    std::filesystem::path           outputDir;
    std::filesystem::path           stdBmi;
    std::filesystem::path           stdObject;
    mcpp::build::BuildPlan          plan;

    // M3.2 BMI cache: deps that did NOT hit cache and therefore need
    // populate_from(...) AFTER backend.build succeeds.
    struct CacheTask {
        mcpp::bmi_cache::CacheKey       key;
        mcpp::bmi_cache::DepArtifacts   artifacts;
    };
    std::vector<CacheTask>          depsToPopulate;

    // Names of deps that DID hit cache (for ui status output).
    std::vector<std::string>        cachedDepLabels;     // "mcpplibs.cmdline v0.0.1"
};

// Command-level overrides (--target / --static).
// Empty defaults preserve pre-existing behaviour exactly.
export struct BuildOverrides {
    std::string target_triple;       // empty = host triple, fall through to [toolchain]
    bool        force_static = false; // --static (or implied by musl target)
    std::string package_filter;      // -p <name>: only build this workspace member
    std::string profile;             // --profile <name> (default "release")
    std::string features;            // --features a,b,c (root package activation)
    bool        strict = false;      // --strict: schema warnings become errors
    std::string capabilities;        // --cap blas=openblas,lapack=mkl (provider pins)
};

// `prepare_build` builds the BuildContext for any verb that compiles.
//   includeDevDeps: when true, dev-dependencies are also fetched + scanned
//                   into the modgraph. mcpp test passes true; build/run pass false.
//   extraTargets:   additional Target entries (e.g. synthetic test targets)
//                   appended to the manifest before the modgraph runs.
//   overrides:      --target / --static.
export std::expected<BuildContext, std::string>
prepare_build(bool print_fingerprint,
              bool includeDevDeps = false,
              std::vector<mcpp::manifest::Target> extraTargets = {},
              BuildOverrides overrides = {}) {
    auto root = mcpp::project::find_manifest_root(std::filesystem::current_path());
    if (!root) {
        return std::unexpected("no mcpp.toml found in current directory or any parent");
    }

    auto m = mcpp::manifest::load(*root / "mcpp.toml");
    if (!m) return std::unexpected(m.error().format());

    // ─── Workspace handling ────────────────────────────────────────────
    // If the manifest has [workspace] and is a virtual workspace (no [package]),
    // or if -p filter is set, switch to the target member's manifest.
    std::optional<mcpp::manifest::Manifest> wsManifest;  // keep workspace manifest alive
    if (m->workspace.present) {
        std::string targetMember;

        if (!overrides.package_filter.empty()) {
            // -p <name>: find matching member by directory basename or path
            for (auto& mp : m->workspace.members) {
                auto basename = std::filesystem::path(mp).filename().string();
                if (basename == overrides.package_filter || mp == overrides.package_filter) {
                    targetMember = mp;
                    break;
                }
            }
            if (targetMember.empty()) {
                return std::unexpected(std::format(
                    "workspace member '{}' not found in [workspace].members",
                    overrides.package_filter));
            }
        } else if (m->package.name.empty()) {
            // Virtual workspace: find a member with a binary target, or use last member.
            for (auto& mp : m->workspace.members) {
                auto memberDir = *root / mp;
                auto mm = mcpp::manifest::load(memberDir / "mcpp.toml");
                if (!mm) continue;
                for (auto& t : mm->targets) {
                    if (t.kind == mcpp::manifest::Target::Binary) {
                        targetMember = mp;
                        break;
                    }
                }
                if (!targetMember.empty()) break;
            }
            if (targetMember.empty() && !m->workspace.members.empty()) {
                targetMember = m->workspace.members.back();
            }
        }
        // else: rooted workspace with [package] — build root normally.

        if (!targetMember.empty()) {
            auto memberDir = *root / targetMember;
            if (!std::filesystem::exists(memberDir / "mcpp.toml")) {
                return std::unexpected(std::format(
                    "workspace member '{}' has no mcpp.toml", targetMember));
            }
            wsManifest = std::move(*m);  // preserve workspace manifest
            m = mcpp::manifest::load(memberDir / "mcpp.toml");
            if (!m) return std::unexpected(std::format(
                "workspace member '{}': {}", targetMember, m.error().format()));

            // Merge workspace dependency versions/paths. `*root` is still the
            // WORKSPACE root here (the `root = memberDir` reassignment below
            // hasn't happened yet), so it anchors any relative `path` in
            // `[workspace.dependencies]` (#224).
            mcpp::project::merge_workspace_deps(*m, *wsManifest, *root);

            // Inherit workspace toolchain if member doesn't define one
            if (m->toolchain.byPlatform.empty()) {
                m->toolchain = wsManifest->toolchain;
            }
            // Inherit workspace target overrides
            for (auto& [triple, entry] : wsManifest->targetOverrides) {
                if (!m->targetOverrides.contains(triple)) {
                    m->targetOverrides[triple] = entry;
                }
            }
            // Inherit workspace indices if member doesn't define any. `*root`
            // is still the workspace root here, which is what a relative
            // `[indices].path` was written against (#224).
            mcpp::project::inherit_workspace_indices(*m, *wsManifest, *root);

            mcpp::ui::status("Workspace", std::format("building member '{}'", targetMember));
            root = memberDir;
        }
    } else {
        // Not at workspace root — check if we're inside a workspace
        auto wsRoot = mcpp::project::find_workspace_root(*root);
        if (!wsRoot.empty()) {
            auto wsm = mcpp::manifest::load(wsRoot / "mcpp.toml");
            if (wsm && wsm->workspace.present) {
                // #224: anchor relative `path`/`[indices].path` to the
                // workspace root, not this member's own directory.
                mcpp::project::merge_workspace_deps(*m, *wsm, wsRoot);
                if (m->toolchain.byPlatform.empty()) {
                    m->toolchain = wsm->toolchain;
                }
                for (auto& [triple, entry] : wsm->targetOverrides) {
                    if (!m->targetOverrides.contains(triple)) {
                        m->targetOverrides[triple] = entry;
                    }
                }
                // Inherit workspace indices if member doesn't define any
                mcpp::project::inherit_workspace_indices(*m, *wsm, wsRoot);
            }
        }
    }

    // Inject synthetic targets (e.g. test binaries from `mcpp test`).
    for (auto& t : extraTargets) m->targets.push_back(t);

    // Surface non-fatal manifest schema warnings (e.g. unsupported [targets.*]
    // keys). Under --strict they become errors — same policy as the
    // feature/platform schema checks below.
    for (auto const& w : m->schemaWarnings) {
        if (overrides.strict) return std::unexpected(w);
        mcpp::diag::warning("manifest/schema", w);
    }

    // ─── Toolchain resolution (docs/21) ────────────────────────────────
    // Priority chain:
    //   1. mcpp.toml [toolchain].<platform>      → resolve_xpkg_path → abs path
    //   2. $CXX env var
    //   3. PATH g++  (with warning)
    std::filesystem::path explicit_compiler;
    std::optional<mcpp::config::GlobalConfig> cfg_opt;
    bool bootstrap_checked = false;
    auto get_cfg = [&](bool requireBootstrap = true) -> std::expected<mcpp::config::GlobalConfig*, std::string> {
        if (!cfg_opt) {
            auto c = mcpp::config::load_or_init(/*quiet=*/false,
                mcpp::fetcher::make_bootstrap_progress_callback());
            if (!c) return std::unexpected(c.error().message);
            cfg_opt = std::move(*c);
        }
        // Commands that need bootstrap tools (build, run, toolchain install)
        // pass requireBootstrap=true to get an early, clear error.
        if (requireBootstrap && !bootstrap_checked) {
            bootstrap_checked = true;
            auto problem = mcpp::config::check_base_init(*cfg_opt);
            if (!problem.empty()) {
                return std::unexpected(std::format(
                    "{}\n  hint: run `mcpp self init --force` to reset and re-initialize",
                    problem));
            }
        }
        return &*cfg_opt;
    };

    constexpr std::string_view kCurrentPlatform = mcpp::platform::name;

    // M5.5: toolchain resolution priority:
    //   0. --target X / --static, looked up in [target.<triple>]
    //   1. project mcpp.toml [toolchain].<platform> or .default
    //   2. global ~/.mcpp/config.toml [toolchain].default
    //   3. hard error (no system fallback)
    // Resolve the build profile, overlaid by any [profile.<name>] from the
    // manifest → buildConfig. `effectiveProfile` outlives the block: the
    // build.mcpp env contract exposes it as MCPP_PROFILE.
    std::string effectiveProfile;
    {
        auto& pname = effectiveProfile;
        // Precedence: --profile / --release / --dev flag (overrides.profile) >
        // [build].default-profile (project default) > "dev" (global default).
        // The global default is "dev" (-O0 -g) to follow the dominant convention
        // (Cargo/Meson/CMake/Zig/Bazel/MSBuild all default to debug); release is
        // opt-in via --release / --profile release. A project that wants its
        // plain `mcpp build` optimized sets [build].default-profile = "release"
        // (mcpp's own mcpp.toml does this, so the released binary stays -O2).
        pname = !overrides.profile.empty()             ? overrides.profile
              : !m->buildConfig.defaultProfile.empty() ? m->buildConfig.defaultProfile
              :                                          "dev";
        mcpp::manifest::Profile pr;
        if (pname == "dev" || pname == "debug") { pr.optLevel = "0"; pr.debug = true; }
        else if (pname == "dist")               { pr.optLevel = "3"; pr.strip = true; }
        // (built-in dist intentionally leaves lto off: several packaged gcc
        //  payloads ship without the LTO plugin; enable via [profile.dist].)
        else                                    { pr.optLevel = "2"; } // release
        if (auto it = m->profiles.find(pname); it != m->profiles.end()) pr = it->second;
        m->buildConfig.optLevel = pr.optLevel;
        m->buildConfig.debug    = pr.debug;
        m->buildConfig.lto      = pr.lto;
        m->buildConfig.strip    = pr.strip;
        m->buildConfig.cflags.insert(m->buildConfig.cflags.end(),
                                     pr.cflags.begin(), pr.cflags.end());
        m->buildConfig.cxxflags.insert(m->buildConfig.cxxflags.end(),
                                       pr.cxxflags.begin(), pr.cxxflags.end());
        m->buildConfig.ldflags.insert(m->buildConfig.ldflags.end(),
                                      pr.ldflags.begin(), pr.ldflags.end());
    }

    // [package] platforms — fixed vocabulary owned by mcpp (it owns the
    // target/triple system). Unknown values: warning, or error under --strict.
    for (auto& pf : m->package.platforms) {
        if (pf != "linux" && pf != "macos" && pf != "windows") {
            auto msg = std::format(
                "[package] platforms contains unknown platform '{}' "
                "(expected: linux | macos | windows)", pf);
            if (overrides.strict) return std::unexpected(msg);
            mcpp::diag::warning("manifest/platforms", msg);
        }
    }

    auto tcSpec = m->toolchain.for_platform(kCurrentPlatform);
    if (!tcSpec.has_value()) {
        auto cfg = get_cfg();
        if (cfg && !(*cfg)->defaultToolchain.empty()) {
            tcSpec = (*cfg)->defaultToolchain;
        }
    }

    // ─── --target / --static overrides ──────────────────────────────────
    // Target-axis default resolution when no --target flag was passed:
    // [build] target (project default, ≙ cargo build.target) >
    // [toolchain] default_target (global config) > host.
    if (overrides.target_triple.empty() && !m->buildConfig.target.empty())
        overrides.target_triple = m->buildConfig.target;
    if (overrides.target_triple.empty()) {
        if (auto cfg = get_cfg(); cfg && !(*cfg)->defaultTarget.empty())
            overrides.target_triple = (*cfg)->defaultTarget;
    }
    // Normalize the triple (alias spellings → canonical), validate against
    // the known-target vocabulary, then apply the manifest [target.<triple>]
    // override and the vocabulary-table convention (pin + default linkage).
    if (!overrides.target_triple.empty()) {
        namespace triple = mcpp::toolchain::triple;
        auto parsed = triple::parse(overrides.target_triple);

        // [target.X] lookup is spelling-independent: a section keyed
        // `x86_64-w64-mingw32` matches `--target x86_64-windows-gnu` and
        // vice versa. Unparseable keys/inputs compare exactly (escape hatch).
        auto it = m->targetOverrides.find(overrides.target_triple);
        if (it == m->targetOverrides.end() && parsed) {
            for (auto o = m->targetOverrides.begin();
                 o != m->targetOverrides.end(); ++o) {
                if (auto k = triple::parse(o->first);
                    k && k->str() == parsed->str()) { it = o; break; }
            }
        }
        bool hasExplicitSection   = it != m->targetOverrides.end();
        bool hasToolchainOverride = hasExplicitSection
                                 && !it->second.toolchain.empty();
        const triple::TargetInfo* known =
            parsed ? triple::find_known_target(*parsed) : nullptr;

        // Validation: a typo must never silently fall through to the host
        // toolchain (the worst failure mode — you think you cross-compiled).
        // An explicit [target.X] section is the escape hatch for custom
        // triples outside the vocabulary.
        if (!known && !hasExplicitSection) {
            auto sug = triple::did_you_mean(overrides.target_triple);
            return std::unexpected(std::format(
                "unknown target '{}'{}\n"
                "       known targets: `mcpp toolchain list`; a custom triple needs an\n"
                "       explicit [target.{}] section in mcpp.toml",
                overrides.target_triple,
                sug ? std::format(" — did you mean '{}'?", *sug) : "",
                overrides.target_triple));
        }
        if (known && known->tier == "planned" && !hasToolchainOverride) {
            return std::unexpected(std::format(
                "target '{}' is registered but not yet supported (planned) — "
                "no toolchain is published for it yet.\n"
                "       An explicit [target.{}] toolchain override can opt in early.",
                parsed->str(), parsed->str()));
        }
        // Canonical from here on: cfg evaluation, spec attachment and the
        // target/ output directory all see one spelling.
        if (parsed) overrides.target_triple = parsed->str();

        if (hasExplicitSection) {
            if (!it->second.toolchain.empty()) tcSpec = it->second.toolchain;
            if (!it->second.linkage.empty())   m->buildConfig.linkage = it->second.linkage;
        }
        // Convention from the vocabulary table (triple.cppm): the target's
        // pinned toolchain (host-awareness — native musl-gcc vs triple-named
        // cross, winlibs mingw vs Linux-hosted cross — lives in the payload
        // mapping, not here) and its default linkage. GCC 16 pin rationale:
        // GCC 15 drops module template instantiations at link (remediation
        // doc A2; packages shipped 2026-07-08/09, GitHub+GitCode).
        if (known && !hasToolchainOverride && !known->pin.empty())
            tcSpec = std::string(known->pin);
        if (known && known->defaultStatic && m->buildConfig.linkage.empty())
            m->buildConfig.linkage = "static";
    }
    if (overrides.force_static) m->buildConfig.linkage = "static";

    // #254: everything compiled INTO this build is resolved for the TARGET —
    // an xpkg descriptor's per-OS sections (sources, flags, deps) and its xpm
    // asset/version table all describe code that will run on the target, not
    // on the machine building it. Previously a compile-time host constant,
    // which is invisible natively (host == target) and picks the wrong leg
    // under --target.
    //
    // Computed HERE, not earlier: `overrides.target_triple` is only complete
    // above — it is filled from `[build] target` and the config default, then
    // canonicalized. Reading it before that point would silently fall back to
    // the host for any project that sets its target in the manifest rather
    // than on the command line.
    const auto targetPlatform = mcpp::platform::TargetPlatform::for_os(
        cfgpred::context_for(overrides.target_triple).os);

    // ── L1: merge conditional [target.'cfg(...)'.build] sources/flags AND
    // root-only [target.'cfg(...)'.dependencies] ─────────────────────────────
    // Evaluated now (target resolved) against the resolved target — the
    // --target triple for a cross build, else the host.
    //
    // #229: merge_conditional_build_inputs MUST run here — before
    // `packages[0] = makePackageRoot(*root, *m)` snapshots `m->buildConfig`
    // into `packages[0].privateBuild`/`.manifest` — because that snapshot,
    // not `*m`, is what the modgraph scan and per-TU compile-flag assembly
    // actually read afterward. Every dependency (path/git/version alike) gets
    // the SAME treatment, at the mirror-image point in its own load path
    // (right before ITS `makePackageRoot`/`propagateLinkFlags`) — see the
    // dependency-manifest-acquisition block below. That makes this the root
    // package's half of the one funnel, not a special case: every package is
    // merged exactly once, immediately before it is captured into `packages[]`.
    if (!m->conditionalConfigs.empty()) {
        auto cc_ctx = cfgpred::context_for(overrides.target_triple);
        merge_conditional_build_inputs(*m, cc_ctx, overrides.target_triple);
        for (auto const& cc : m->conditionalConfigs) {
            if (!cfgpred::matches(cc.predicate, cc_ctx, overrides.target_triple))
                continue;
            // Conditional dependencies (Phase 1b): merge into the manifest maps
            // before dependency resolution so they resolve like any dep. insert()
            // keeps an existing unconditional entry (no silent override).
            // Root-only — a dependency's own conditional deps are out of scope.
            m->dependencies.insert(cc.dependencies.begin(), cc.dependencies.end());
            m->devDependencies.insert(cc.devDependencies.begin(), cc.devDependencies.end());
            m->buildDependencies.insert(cc.buildDependencies.begin(), cc.buildDependencies.end());
        }
    }
    // `[build].defines` must reach the scanner (P1689) and the compile edge,
    // and must participate in the fingerprint. Fold before dependency
    // resolution / fingerprinting.
    fold_build_defines_into_flags(m->buildConfig);

    // msvc@system: a *system* toolchain — located on the machine, never
    // resolved through xim packages. mcpp does not install MSVC.
    bool tcSpecIsMsvc = false;
    if (tcSpec.has_value()) {
        if (auto s = mcpp::toolchain::parse_toolchain_spec(*tcSpec);
            s && mcpp::toolchain::is_system_toolchain(*s))
            tcSpecIsMsvc = true;
    }
    if (tcSpecIsMsvc) {
        if (!mcpp::platform::is_windows) {
            return std::unexpected(std::format(
                "toolchain '{}' is only available on Windows hosts", *tcSpec));
        }
        auto inst = mcpp::toolchain::msvc::detect_installation();
        if (!inst) {
            return std::unexpected(mcpp::toolchain::msvc::install_guidance());
        }
        explicit_compiler = inst->clPath;
        mcpp::ui::info("Resolved", std::format(
            "msvc@system → msvc {} ({})",
            inst->display_version(), inst->clPath.string()));
    } else if (tcSpec.has_value() && *tcSpec != "system") {
        auto spec = mcpp::toolchain::parse_toolchain_spec(*tcSpec);
        if (!spec || spec->version.empty()) {
            return std::unexpected(std::format(
                "[toolchain].{} = '{}' is invalid; expected '<pkg>@<version>'",
                kCurrentPlatform, *tcSpec));
        }
        // A `--target <triple>` build carries the (already canonical) triple
        // into the spec's target axis: the payload mapping then resolves the
        // right package/frontend (e.g. aarch64-linux-musl-g++ for a cross
        // musl build, never the host g++). Escape-hatch triples outside the
        // language don't parse and leave the spec on the host target.
        if (!overrides.target_triple.empty()) {
            if (auto t = mcpp::toolchain::triple::parse(overrides.target_triple))
                spec->target = *t;
        }
        auto pkg = mcpp::toolchain::to_xim_package(*spec);

        auto cfg = get_cfg();
        if (!cfg) return std::unexpected(cfg.error());
        mcpp::fetcher::Fetcher fetcher(**cfg);

        mcpp::ui::info("Resolving", "toolchain");
        mcpp::fetcher::InstallProgressHandler progress;
        auto payload = fetcher.resolve_xpkg_path(pkg.target(), /*autoInstall=*/true, &progress);
        if (!payload) {
            return std::unexpected(std::format(
                "toolchain '{}': {}", *tcSpec, payload.error().message));
        }

        explicit_compiler = mcpp::toolchain::toolchain_frontend(payload->binDir, pkg);
        if (!std::filesystem::exists(explicit_compiler)) {
            return std::unexpected(std::format(
                "toolchain payload '{}' has no known C++ frontend in {}",
                pkg.target(), payload->binDir.string()));
        }
        // Same post-install fixup as `mcpp toolchain install` — this manifest
        // [toolchain] path previously ran none, so a freshly auto-installed
        // payload kept its stale install-time cfg / unpatched runtime libs.
        mcpp::toolchain::ensure_post_install_fixup(**cfg, payload->root, pkg);
        // Canonical rendering, whatever spelling the manifest/config used:
        // "Resolved gcc@16.1.0 → x86_64-linux-musl → <frontend>".
        mcpp::ui::info("Resolved",
            std::format("{} → {}", spec->display(),
                mcpp::ui::shorten_path(explicit_compiler,
                    mcpp::fetcher::make_path_ctx(&**get_cfg(), *root))));
    } else if (tcSpec.has_value() && *tcSpec == "system") {
        // Explicit user opt-in to system PATH compiler — kept as escape hatch.
    } else if (auto* opt = std::getenv("MCPP_NO_AUTO_INSTALL"); opt && *opt && *opt != '0') {
        // CI / offline / test opt-out: hard-error instead of silently
        // pulling ~800 MB of toolchain. Preserves the original M5.5
        // contract for environments that need it.
        namespace pins = mcpp::toolchain::triple::pins;
        if constexpr (mcpp::platform::is_macos || mcpp::platform::is_windows) {
            return std::unexpected(std::format(
                "no toolchain configured.\n"
                "       run one of:\n"
                "         mcpp toolchain install {}\n"
                "         mcpp toolchain default {}\n"
                "       or unset MCPP_NO_AUTO_INSTALL to let mcpp auto-install.",
                pins::kSuggestLlvm, pins::kFirstRunMacWin));
        } else {
            return std::unexpected(std::format(
                "no toolchain configured.\n"
                "       run one of:\n"
                "         mcpp toolchain install {}\n"
                "         mcpp toolchain default {}\n"
                "       or unset MCPP_NO_AUTO_INSTALL to let mcpp auto-install.",
                pins::kSuggestGccMusl, pins::kFirstRunLinuxOther));
        }
    } else {
        // First-run UX: no project-level [toolchain], no global default,
        // and the user just ran `mcpp build` (or similar). Auto-install
        // the platform's canonical default so the user gets a working
        // binary out of the box without any config. We pin it as the
        // global default so the next invocation is silent.
        // Users can switch any time via `mcpp toolchain default <spec>`.
        //
        // macOS: LLVM/Clang — Apple doesn't ship GCC; upstream LLVM with
        //        bundled libc++ is the self-contained choice.
        // Linux: glibc gcc — the platform-native ABI. A musl-static default
        //        cannot link the glibc world (X11/GL/system libs), so it
        //        breaks GUI/native packages out of the box. musl-static stays
        //        opt-in via `mcpp build --target x86_64-linux-musl` for users
        //        who explicitly want portable static binaries.
        // Linux default is arch-aware:
        //   x86_64 → glibc gcc (native ABI; the glibc toolchain is published
        //            for x86_64). musl-static stays opt-in via --target.
        //   other arches (aarch64, ...) → musl-static gcc: it's what's
        //            published for them, is self-contained, and yields portable
        //            static binaries (ideal for aarch64 / Termux, no bionic dep).
        //            glibc-world linking (X11/GL) needs an explicit glibc
        //            toolchain, addable later for native-ABI aarch64 builds.
        namespace pins = mcpp::toolchain::triple::pins;
        std::string defaultSpec;
        if constexpr (mcpp::platform::is_macos || mcpp::platform::is_windows) {
            defaultSpec = std::string(pins::kFirstRunMacWin);
        } else if (mcpp::platform::host_arch == std::string_view("x86_64")) {
            defaultSpec = std::string(pins::kFirstRunLinuxX86_64);
        } else {
            defaultSpec = std::string(pins::kFirstRunLinuxOther);
        }
        auto defaultParsed = mcpp::toolchain::parse_toolchain_spec(defaultSpec);
        // The legacy "-musl" spelling normalizes to (gcc, <host>-linux-musl),
        // so the resolver finds the `<host_arch>-linux-musl-g++` frontend
        // without any manual triple seeding.
        bool muslDefault = defaultParsed->target.is_musl();
        auto defaultPkg = mcpp::toolchain::to_xim_package(*defaultParsed);

        if constexpr (mcpp::platform::is_macos || mcpp::platform::is_windows) {
            mcpp::ui::info("First run",
                std::format("no toolchain configured — installing {} (LLVM/Clang) as default",
                            defaultSpec));
        } else {
            mcpp::ui::info("First run",
                std::format("no toolchain configured — installing {} ({}) as default",
                            defaultSpec, muslDefault ? "musl, static" : "glibc, native ABI"));
        }

        auto cfg = get_cfg();
        if (!cfg) return std::unexpected(cfg.error());
        mcpp::fetcher::Fetcher fetcher(**cfg);

        mcpp::fetcher::InstallProgressHandler progress;
        // The glibc default toolchain needs the sysroot payloads (C library +
        // kernel headers). The musl default is self-contained, so skip them.
        if (!mcpp::platform::is_macos && !mcpp::platform::is_windows && !muslDefault) {
            for (auto dep : {"xim:glibc", "xim:linux-headers"}) {
                (void)fetcher.resolve_xpkg_path(dep, /*autoInstall=*/true, &progress);
            }
        }
        auto payload = fetcher.resolve_xpkg_path(defaultPkg.target(),
                            /*autoInstall=*/true, &progress);
        if (!payload) {
            return std::unexpected(std::format(
                "auto-installing default toolchain {} failed: {}\n"
                "       you can install it manually with:\n"
                "         mcpp toolchain install {}",
                defaultSpec, payload.error().message, defaultSpec));
        }
        explicit_compiler = mcpp::toolchain::toolchain_frontend(payload->binDir, defaultPkg);
        if (!std::filesystem::exists(explicit_compiler)) {
            return std::unexpected(std::format(
                "default toolchain payload {} has no known C++ frontend in {}",
                defaultPkg.target(), payload->binDir.string()));
        }

        // The freshly-installed toolchain needs the SAME post-install fixup
        // (patchelf / specs / cfg wiring against the sandbox glibc) that
        // `mcpp toolchain install` performs — without it a fresh sandbox
        // gcc cannot find the C library (stdlib.h: No such file or
        // directory) and a fresh llvm keeps its stale install-time cfg.
        mcpp::toolchain::ensure_post_install_fixup(**cfg, payload->root, defaultPkg);

        // Persist the default so we don't ask again next time.
        if (auto wr = mcpp::config::write_default_toolchain(**cfg, defaultSpec); wr) {
            (*cfg)->defaultToolchain = defaultSpec;
            mcpp::ui::status("Default", std::format("set to {}", defaultSpec));
        } // best-effort: a failed config write only loses the persistence,
          // not the running build.
        tcSpec = defaultSpec;
    }

    auto tc = mcpp::toolchain::detect(explicit_compiler);
    if (!tc) return std::unexpected(tc.error().message);

    // Native MSVC builds need the synthesized INCLUDE/LIB env — absent when
    // detection found VC tools but no Windows SDK. Fail here with guidance
    // instead of cl.exe's later "cannot open include file: 'corecrt.h'".
    if (tc->compiler == mcpp::toolchain::CompilerId::MSVC
        && tc->envOverrides.empty()) {
        return std::unexpected(std::format(
            "msvc {} was detected at {}, but no Windows SDK was found —\n"
            "       cl.exe cannot compile without the UCRT/SDK headers.\n"
            "       Install the 'Windows 11 SDK' component via the Visual Studio\n"
            "       Installer (it is part of the Desktop development with C++\n"
            "       workload), then retry.",
            tc->version, tc->binaryPath.string()));
    }

    // For musl-gcc the toolchain is fully self-contained
    // (`<root>/x86_64-linux-musl/{include,lib}` is its own sysroot).
    // musl-gcc's `-dumpmachine` reports `x86_64-linux-musl`.
    bool isMuslTc = mcpp::toolchain::is_musl_target(*tc);

    // A musl toolchain only really makes sense with static linkage —
    // dynamic-musl binaries depend on a system /lib/ld-musl-x86_64.so.1
    // that most distros don't ship. Default linkage to "static" when
    // the resolved toolchain is musl, unless the user has already opted
    // out via [build].linkage / [target.<triple>].linkage.
    if (isMuslTc && m->buildConfig.linkage.empty()) {
        m->buildConfig.linkage = "static";
    }

    // Sysroot comes from the toolchain payload itself (GCC -print-sysroot,
    // Clang clang++.cfg). mcpp does not override it — the payload is
    // self-describing. See docs: 2026-05-21-linux-sysroot-missing-kernel-headers.md

    // ── L3: project-local `build.mcpp` imperative build program ─────────────
    // The ROOT program is compiled with the HOST toolchain and run AFTER
    // dependency resolution + feature activation (so it receives
    // MCPP_DEP_<NAME>_DIR like a dependency's does — design §3.1 item 4) and
    // BEFORE the modgraph scan (so its `generated=`/`source=` sources are
    // picked up) — see the call site further below, after the dep build.mcpp
    // loop. Its stdout directives augment buildConfig; a declared-input cache
    // re-runs it only when its source/inputs/env/contract change. It cannot
    // gate the top-level dependency graph (leaf-only rule). Under a cross
    // --target it runs with a host-resolved toolchain and sees MCPP_TARGET =
    // the cross triple (G3).
    // See .agents/docs/2026-06-30-l3-build-mcpp-implementation-design.md,
    // 2026-07-17-asm-sources-and-general-build-capabilities-design.md §2.4 and
    // 2026-07-19-large-source-pkg-platform-fixes-and-buildmcpp-generation-design.md.
    // Root [generated_files]: materialize before build.mcpp and the modgraph
    // scan so synthesized sources are globbed like any on-disk file — and
    // BEFORE dependency resolution, since generated_files may produce
    // build.mcpp itself. (The per-dependency call sits in the dep resolution
    // loop below; the root manifest needs its own.)
    if (!m->buildConfig.generatedFiles.empty()) {
        if (auto r = materialize_generated_files(*root, *m); !r) {
            return std::unexpected(r.error());
        }
    }

    // Canonical rendering of the resolved target (for the env contract).
    std::string resolvedTargetCanonical;
    if (!overrides.target_triple.empty()) {
        auto tt = mcpp::toolchain::triple::parse(overrides.target_triple);
        resolvedTargetCanonical = tt ? tt->str() : overrides.target_triple;
    }

    // Host toolchain for build.mcpp (G3): under a cross --target the resolved
    // `tc` is the cross toolchain, whose products cannot run here — resolve a
    // host-target toolchain from the same spec vocabulary (the spec WITHOUT
    // the --target axis), lazily and only when a build.mcpp actually exists
    // (root or dependency).
    std::optional<std::pair<std::filesystem::path, mcpp::toolchain::Toolchain>> hostTcCache;
    auto host_tc_for_build_program = [&]() -> std::expected<
            std::pair<std::filesystem::path, mcpp::toolchain::Toolchain>, std::string> {
        if (overrides.target_triple.empty())
            return std::pair{explicit_compiler, *tc};
        if (hostTcCache) return *hostTcCache;
        if (!tcSpec || *tcSpec == "system" || tcSpecIsMsvc) {
            return std::unexpected(std::string(
                "build.mcpp under a cross --target needs a resolvable host "
                "toolchain — set one via [toolchain] or `mcpp toolchain default`"));
        }
        auto spec = mcpp::toolchain::parse_toolchain_spec(*tcSpec);
        if (!spec || spec->version.empty()) {
            return std::unexpected(std::format(
                "toolchain spec '{}' is invalid for the build.mcpp host resolve", *tcSpec));
        }
        // Deliberately NO target injection: the spec resolves for the host.
        auto pkg = mcpp::toolchain::to_xim_package(*spec);
        auto cfgH = get_cfg();
        if (!cfgH) return std::unexpected(cfgH.error());
        mcpp::fetcher::Fetcher fetcher(**cfgH);
        mcpp::fetcher::InstallProgressHandler progress;
        auto payload = fetcher.resolve_xpkg_path(pkg.target(), /*autoInstall=*/true, &progress);
        if (!payload) {
            return std::unexpected(std::format(
                "host toolchain for build.mcpp ('{}'): {}", *tcSpec,
                payload.error().message));
        }
        auto frontend = mcpp::toolchain::toolchain_frontend(payload->binDir, pkg);
        if (!std::filesystem::exists(frontend)) {
            return std::unexpected(std::format(
                "host toolchain payload '{}' has no known C++ frontend in {}",
                pkg.target(), payload->binDir.string()));
        }
        mcpp::toolchain::ensure_post_install_fixup(**cfgH, payload->root, pkg);
        auto htc = mcpp::toolchain::detect(frontend);
        if (!htc) return std::unexpected(htc.error().message);
        mcpp::ui::info("Resolved", std::format(
            "host toolchain for build.mcpp: {}", htc->label()));
        hostTcCache = std::pair{frontend, *htc};
        return *hostTcCache;
    };

    // Resolve dependencies: walk the **transitive** graph from the main
    // manifest, BFS-style. Each unique `(namespace, shortName)` is fetched
    // once, its `[build].include_dirs` are propagated to the main
    // manifest, and its own `[dependencies]` are queued for processing
    // (its `[dev-dependencies]` are NOT — those are private to the dep's
    // own test runs).
    //
    // Conflict policy: C++ modules require globally-unique module names
    // and ODR-respecting symbols, so the same `(ns, name)` resolved to
    // two different exact versions is an error — mcpp prints both
    // requesting parents and asks the user to align them.

    // Auto-refresh the builtin package index only when a version dependency
    // is actually routed there. Local/remote project indices are handled by
    // the project-scoped setup below; refreshing the global index for those
    // packages is both unnecessary and can make offline/local-index builds
    // block on unrelated remote repositories.
    if (!m->dependencies.empty()) {
        auto usesBuiltinIndex = [&](const mcpp::manifest::DependencySpec& spec) {
            if (spec.isPath() || spec.isGit()) return false;

            auto ns = spec.namespace_.empty()
                ? std::string(mcpp::pm::kDefaultNamespace)
                : spec.namespace_;
            if (ns == mcpp::pm::kDefaultNamespace) {
                // R6: `[indices] default = {...}` (normalized to
                // kDefaultNamespace by toml.cppm) redirects the default
                // namespace away from the builtin registry — consult it
                // before assuming builtin, same as any named namespace.
                auto it = m->indices.find(std::string(mcpp::pm::kDefaultNamespace));
                return it == m->indices.end() || it->second.is_builtin();
            }

            auto it = m->indices.find(ns);
            if (it == m->indices.end()) return true;
            return it->second.is_builtin();
        };

        bool needsBuiltinIndexRefresh = false;
        for (auto& [_, spec] : m->dependencies) {
            if (usesBuiltinIndex(spec)) {
                needsBuiltinIndexRefresh = true;
                break;
            }
        }
        if (needsBuiltinIndexRefresh) {
            auto cfg2 = get_cfg();
            if (cfg2) {
                auto xlEnv = mcpp::config::make_xlings_env(**cfg2);
                if (!mcpp::xlings::is_index_fresh(xlEnv, (*cfg2)->searchTtlSeconds)) {
                    mcpp::ui::status("Updating", "package index (auto-refresh)");
                    mcpp::xlings::ensure_index_fresh(
                        xlEnv, (*cfg2)->searchTtlSeconds, /*quiet=*/true);
                }
            }
        }
    }

    // Set up project-level .mcpp/ directory for custom indices and/or the
    // [xlings] build environment (L-1). This creates .mcpp/.xlings.json with
    // custom non-builtin index entries (so xlings can clone them) plus the
    // [xlings] deps/workspace/subos/envs materialized verbatim.
    if (!m->indices.empty() || !m->xlings.empty()) {
        auto cfg2 = get_cfg();
        if (cfg2) {
            mcpp::xlings::ProjectEnv penv;
            penv.deps  = m->xlings.deps;
            penv.subos = m->xlings.subos;
            for (auto const& [k, v] : m->xlings.workspace) penv.workspace.emplace_back(k, v);
            for (auto const& [k, v] : m->xlings.envs)      penv.envs.emplace_back(k, v);
            mcpp::config::ensure_project_index_dir(**cfg2, *root, m->indices, penv);

            // On first build, the project index data root may be empty because
            // ensure_project_index_dir only writes .xlings.json but does not
            // trigger clone/link creation. Local path indices are read directly;
            // remote custom indices are synced quietly before dependency resolution.
            bool hasCustomIndices = false;
            for (auto& [idxName, spec] : m->indices) {
                if (!spec.is_builtin()) {
                    hasCustomIndices = true;
                    break;
                }
            }
            if (hasCustomIndices) {
                bool needsClone = !mcpp::config::project_index_data_initialized(*root);
                if (needsClone) {
                    bool needsRemoteUpdate = false;
                    for (auto& [idxName, spec] : m->indices) {
                        if (spec.is_builtin() || spec.is_local()) continue;
                        needsRemoteUpdate = true;
                        break;
                    }
                    if (needsRemoteUpdate) {
                        mcpp::ui::status("Fetching", "custom index repos (first use)");
                        auto projEnv = mcpp::config::make_project_xlings_env(**cfg2, *root);
                        int rc = mcpp::xlings::update_index(projEnv, /*quiet=*/true);
                        if (rc != 0) {
                            return std::unexpected(
                                "project custom index update failed; run `mcpp index update` for details");
                        }
                    }
                }
            }
        }
    }

    std::vector<mcpp::modgraph::PackageRoot> packages;
    packages.push_back({*root, *m});

    // dep_manifests is kept around purely so the build plan can move it
    // out at the end (PackageRoot stores a `Manifest` by value, so the
    // unique_ptr is not load-bearing for liveness — it's a leftover from
    // an earlier design and harmless).
    std::vector<std::unique_ptr<mcpp::manifest::Manifest>> dep_manifests;
    auto cache_index_name = [](std::string_view ns) {
        if (ns.empty()) return std::string(mcpp::pm::kDefaultNamespace);
        return std::string(ns);
    };
    struct DepCacheIdentity {
        std::string indexName;
        std::string packageName;
        std::string version;
    };
    std::vector<DepCacheIdentity> dep_cache_identities;
    struct GitLockIdentity {
        std::string source;
        std::string hash;
    };
    std::map<std::string, GitLockIdentity> root_git_lock_identities;

    struct ResolvedKey {
        std::string ns;
        std::string shortName;
        auto operator<=>(const ResolvedKey&) const = default;
    };
    struct ResolvedRecord {
        std::string version;            // empty for path/git deps
        std::string constraint;         // AND-combined original constraints (version src only)
        std::string requestedBy;        // human-readable for error messages
        std::string source;             // "version" | "path" | "git" — for type-clash check
        std::size_t depIndex = 0;       // index into dep_manifests/packages-1 (for in-place re-fetch)
        std::vector<std::string> linkFlagsAdded;  // entries appended to m->buildConfig.ldflags by this dep
    };
    std::map<ResolvedKey, ResolvedRecord> resolved;

    // Sentinel for "the consumer is the main package" (no dep_manifests entry).
    constexpr std::size_t kMainConsumer = static_cast<std::size_t>(-1);

    struct WorkItem {
        std::string                          name;                // dep map key as written
        mcpp::manifest::DependencySpec       spec;                // copy (we may mutate version)
        std::string                          requestedBy;         // who asked for it
        std::string                          originalConstraint;  // spec.version BEFORE pinning (for SemVer merge)
        std::size_t                          consumerDepIndex;    // dep_manifests slot of who pushed this child; kMainConsumer for main
        std::filesystem::path                resolveRoot;         // base dir for relative path deps (empty = use project root)
    };
    std::deque<WorkItem> worklist;

    // SemVer constraint resolver, shared across the worklist so transitive
    // deps with caret/range constraints (`^1.0`) also get pinned to a
    // concrete version before fetch.
    auto resolveSemver = [&](mcpp::manifest::DependencySpec& s,
                              const std::string& depName)
        -> std::expected<void, std::string>
    {
        if (s.isPath() || s.isGit()) return {};
        if (!mcpp::pm::is_version_constraint(s.version)) return {};
        auto cfg = get_cfg();
        if (!cfg) return std::unexpected(cfg.error());
        mcpp::fetcher::Fetcher fetcher(**cfg);
        // 0.0.10+: use structured namespace from DependencySpec.
        auto resolved = mcpp::pm::resolve_semver(
            s.namespace_, s.shortName.empty() ? depName : s.shortName,
            s.version, fetcher, targetPlatform);
        if (!resolved) return std::unexpected(resolved.error());
        mcpp::ui::info("Resolved",
            std::format("{} {} → v{}", depName, s.version, *resolved));
        s.version = std::move(*resolved);
        return {};
    };

    // Acquire a version-source dep at a specific pinned version. Used both
    // by the first-time walk and by the SemVer merger when a re-fetch at a
    // different version is needed. Returns the dep's effective root (where
    // mcpp.toml lives) and a fully loaded manifest.
    using LoadedDep = std::pair<std::filesystem::path, mcpp::manifest::Manifest>;
    // Index routing — WHICH index answers for a namespace and how its
    // descriptors are read — lives in mcpp.pm.index_route, shared with the
    // `mcpp add` existence gate so the two cannot disagree about which
    // packages are real (#305/#307). `cfg` is filled in per call: the route is
    // rebuilt on demand because `root` moves when a workspace member is
    // selected above.
    auto index_route = [&](mcpp::config::GlobalConfig* cfg = nullptr) {
        return mcpp::pm::IndexRoute{ &m->indices, *root, cfg };
    };
    auto findIndexForNs = [&](const std::string& ns)
        -> const mcpp::pm::IndexSpec*
    {
        return index_route().find_for_ns(ns);
    };

    // Identity-first candidate probe. A candidate is DISAMBIGUATED by the
    // DECLARED (namespace, name) of whatever descriptor the index holds — never
    // by whether a canonically-named file `<ns>.<short>.lua` happens to exist on
    // disk. It routes through the same identity-verified readers the load path
    // uses (`read_xpkg_lua*`, which gate every hit on the descriptor's declared
    // identity and already cover non-canonical filenames), so candidate selection
    // and loading can never disagree about what a candidate resolves to.
    //
    // SCOPE (#278, do not over-read the paragraph above): identity governs which
    // hits are ACCEPTED, not which files are REACHED. Discovery is still bounded
    // by the candidate-filename list from `compat::xpkg_lua_candidates` — there
    // is no index-wide scan of `pkgs/*/*.lua` anywhere in mcpp, so a descriptor
    // whose filename matches none of the candidates is simply not found. The
    // `IdentityIndex` that would lift that bound was deferred with §5 of the
    // 2026-06-26 design and is deliberately NOT being added: see
    // .agents/docs/2026-07-25-issue278-descriptor-name-form-canonicalization-design.md
    // §3.2/§4.2 for why bare-name discovery across arbitrary namespaces is a
    // reproducibility hazard rather than a convenience.
    //
    // Before this, selection probed the canonical filename only, so a descriptor
    // filed under a non-canonical name (e.g. `aimol.tensorvia-cpu` declared in the
    // mcpplibs index as bare `pkgs/t/tensorvia-cpu.lua`) was invisible to its own
    // peer-root candidate `(aimol, tensorvia-cpu)`, leaving the request pinned to
    // the wrong front candidate `(mcpplibs.aimol, …)`. See
    // .agents/docs/2026-06-26-identity-first-resolution-no-filename.md.
    auto readStrictLuaForCandidate =
        [&](const mcpp::pm::DependencyCoordinate& coord)
            -> std::optional<std::string>
    {
        auto cfg = get_cfg();
        if (!cfg) return std::nullopt;
        return index_route(*cfg).read(coord);
    };

    auto xpkgLuaMatchesCandidate =
        [](const mcpp::pm::DependencyCoordinate& coord,
           std::string_view luaContent,
           bool allowLegacyBareDefault) {
            // Single source of truth: the descriptor identity gate lives in
            // mcpp.manifest and is shared with the read_xpkg_lua family.
            return mcpp::manifest::xpkg_lua_identity_matches(
                luaContent, coord.namespace_, coord.shortName,
                allowLegacyBareDefault);
        };

    auto dependencyCoordinates =
        [](const mcpp::manifest::DependencySpec& spec,
           const std::string& depName) {
            if (!spec.candidates.empty()) return spec.candidates;
            std::vector<mcpp::pm::DependencyCoordinate> out;
            out.push_back({
                .namespace_ = spec.namespace_.empty()
                    ? std::string(mcpp::pm::kDefaultNamespace)
                    : spec.namespace_,
                .shortName = spec.shortName.empty() ? depName : spec.shortName,
            });
            return out;
        };

    auto selectDependencyCandidate =
        [&](mcpp::manifest::DependencySpec& spec,
            const std::string& depName) -> std::expected<void, std::string>
    {
        auto candidates = dependencyCoordinates(spec, depName);
        if (candidates.empty()) {
            return std::unexpected(
                std::format("dependency '{}' has no lookup candidates", depName));
        }

        auto selected = candidates.front();
        bool matched  = false;
        if (spec.isVersion() && candidates.size() > 1) {
            for (auto& candidate : candidates) {
                auto lua = readStrictLuaForCandidate(candidate);
                if (!lua || !xpkgLuaMatchesCandidate(
                        candidate, *lua, /*allowLegacyBareDefault=*/false)) {
                    continue;
                }

                // INV-RESOLVE (#278) — the discovery rung `(∅, name)` is the
                // "upstream package that declares no namespace" rung, NOT a
                // cross-namespace wildcard. The identity gate is intentionally
                // permissive here (`ns.empty() → name match is enough`, because
                // `mcpp new --template X` legitimately discovers by short name),
                // so the narrowing lives at THIS call site rather than in the
                // gate — tightening the gate would break template discovery.
                //
                // Rejecting the hit keeps a third-party-namespaced package from
                // being reachable by a bare name: resolution must not depend on
                // which indices happen to be present, or adding an index could
                // silently retarget an existing dependency (design §3.2).
                auto declaredNs =
                    mcpp::manifest::extract_xpkg_namespace(*lua);
                if (candidate.namespace_.empty() && !declaredNs.empty()) {
                    continue;
                }

                // P3 (#278) — resolve the discovery rung to a REAL identity
                // before anything downstream sees it. `selected.namespace_`
                // used to be the CANDIDATE's namespace, so a discovery hit
                // wrote an empty namespace into the spec and on into the
                // lockfile and install layer. Read the DECLARED one instead.
                //
                // An empty `declaredNs` is a legal identity here, not a hole to
                // fill: an upstream package with no `namespace` (xim `opencv`,
                // `musl-gcc`) is keyed by its bare name, and the derived
                // fqname == shortName is exactly right for it. Attributing such
                // a descriptor to its owning index (`xim-pkgindex → xim`) is
                // §4.1 of the 2026-06-26 design and is still unimplemented.
                selected = candidate;
                if (selected.namespace_.empty()) selected.namespace_ = declaredNs;
                matched = true;
                break;
            }

            // A custom GIT index is cloned lazily by xlings during install, so
            // at selection time its descriptors may legitimately not be on disk
            // yet. "Not found" is therefore not conclusive for those namespaces
            // — keep the historical fall-through rather than hard-failing on a
            // package that would have materialized a moment later. Local path
            // indices and the builtin index are both readable here, so they stay
            // under the strict rule below.
            bool anyLazyGitIndex = std::ranges::any_of(candidates,
                [&](const mcpp::pm::DependencyCoordinate& c) {
                    return index_route().lazy_git(c.namespace_);
                });

            // T9 (#278) — no candidate resolved. This used to fall through to
            // `candidates.front()` SILENTLY, so mcpp carried on with a namespace
            // it had invented, and the user met the failure much later (during
            // download/install) wrapped around that invented name. Fail here,
            // and say exactly which identities were tried.
            if (!matched && !anyLazyGitIndex) {
                std::string tried;
                for (auto& c : candidates) {
                    if (!tried.empty()) tried += ", ";
                    tried += c.namespace_.empty()
                        ? std::format("(no namespace, {})", c.shortName)
                        : std::format("({}, {})", c.namespace_, c.shortName);
                }

                // T12 — did-you-mean. DIAGNOSTIC ONLY: the scan runs solely on
                // this already-failed path and its result never leaves the
                // error string (see Fetcher::scan_fqns_with_short_name).
                std::string hint;
                if (auto cfg = get_cfg()) {
                    auto fqns = mcpp::pm::cross_namespace_matches(
                        index_route(*cfg), candidates.front().shortName);
                    if (!fqns.empty()) {
                        hint += "\n  a package with this name exists under "
                                "another namespace:";
                        for (auto& fqn : fqns) hint += "\n    " + fqn;
                        hint += std::format(
                            "\n  bare names only resolve to the `{}` / `{}` "
                            "namespaces. write it out:"
                            "\n    [dependencies]"
                            "\n    \"{}\" = \"{}\""
                            "\n  or:"
                            "\n    [dependencies.{}]"
                            "\n    {} = \"{}\"",
                            mcpp::pm::kDefaultNamespace,
                            mcpp::pm::kCompatNamespace,
                            fqns.front(), spec.version.empty() ? "<version>"
                                                              : spec.version,
                            fqns.front().substr(0, fqns.front().rfind('.')),
                            fqns.front().substr(fqns.front().rfind('.') + 1),
                            spec.version.empty() ? "<version>" : spec.version);
                    }
                }

                return std::unexpected(std::format(
                    "dependency '{}': no package found under the namespaces "
                    "mcpp searched\n  tried: {}{}",
                    depName, tried, hint));
            }
        }

        spec.namespace_ = std::move(selected.namespace_);
        spec.shortName = std::move(selected.shortName);
        spec.candidates = std::move(candidates);
        return {};
    };

    // 0.0.10+: loadVersionDep accepts structured (ns, shortName) for
    // namespace-aware lookup. depName is the map key (qualified or bare),
    // kept for install() target formatting and error messages.
    std::set<std::string> preinstallStack;
    std::set<std::string> preinstallDone;

    std::function<std::expected<LoadedDep, std::string>(
        const std::string&,
        const std::string&,
        const std::string&,
        const std::string&)> loadVersionDep;

    loadVersionDep = [&](const std::string& depName,
                         const std::string& ns,
                         const std::string& shortName,
                         const std::string& version)
        -> std::expected<LoadedDep, std::string>
    {
        auto cfg = get_cfg();
        if (!cfg) return std::unexpected(cfg.error());
        mcpp::fetcher::Fetcher fetcher(**cfg);

        // ─── Routing: check if this dep's namespace maps to a custom index ──
        auto* idxSpec = findIndexForNs(ns);

        const bool useProjectEnv = idxSpec && !idxSpec->is_builtin();

        auto readLuaContent = [&]() -> std::optional<std::string> {
            if (idxSpec && idxSpec->is_local()) {
                auto indexPath = mcpp::config::resolve_project_index_path(*root, *idxSpec);
                return mcpp::fetcher::Fetcher::read_xpkg_lua_from_path(
                    indexPath, ns, shortName);
            }
            if (idxSpec && !idxSpec->is_builtin()) {
                return mcpp::fetcher::Fetcher::read_xpkg_lua_from_project_data(
                    *root, ns, shortName);
            }
            return fetcher.read_xpkg_lua(ns, shortName);
        };

        auto luaContent = readLuaContent();
        if (idxSpec && idxSpec->is_local() && !luaContent) {
            auto indexPath = mcpp::config::resolve_project_index_path(*root, *idxSpec);
            return std::unexpected(std::format(
                "dependency '{}': not found in local index at '{}'",
                depName, indexPath.string()));
        }

        auto findRawInstalled = [&]() -> std::optional<std::filesystem::path> {
            if (useProjectEnv) {
                if (auto p = mcpp::fetcher::Fetcher::install_path_from_project_data(
                        *root, ns, shortName, version)) {
                    return p;
                }
            }
            return fetcher.install_path(ns, shortName, version);
        };

        auto installedLayoutMatchesIndex = [&](const std::filesystem::path& verRoot) -> bool {
            if (!luaContent) return false;

            auto field = mcpp::manifest::extract_mcpp_field(*luaContent);
            if (field.kind == mcpp::manifest::McppField::StringPath) {
                return !mcpp::modgraph::expand_glob(verRoot, field.value).empty();
            }
            if (field.kind == mcpp::manifest::McppField::TableBody) {
                auto dm = mcpp::manifest::synthesize_from_xpkg_lua(
                    *luaContent, depName, version, targetPlatform);
                if (!dm) return false;
                for (auto const& [generatedPath, _] : dm->buildConfig.generatedFiles) {
                    if (!generatedPath.empty()) return true;
                }
                for (auto const& glob : dm->modules.sources) {
                    if (!glob.empty() && glob.front() == '!') continue;
                    if (!mcpp::modgraph::expand_glob(verRoot, glob).empty()) {
                        return true;
                    }
                }
                return false;
            }

            for (auto pat : { "mcpp.toml", "*/mcpp.toml" }) {
                if (!mcpp::modgraph::expand_glob(verRoot, pat).empty()) {
                    return true;
                }
            }
            return false;
        };

        auto findCompleteInstalled = [&]() -> std::optional<std::filesystem::path> {
            auto p = findRawInstalled();
            if (!p) return std::nullopt;
            if (mcpp::fallback::is_install_complete(*p)) return p;
            if (installedLayoutMatchesIndex(*p)) {
                mcpp::fallback::mark_install_complete(*p);
                return p;
            }
            mcpp::fallback::clean_incomplete_install(*p);
            return std::nullopt;
        };

        auto markInstalled = [&](const std::filesystem::path& p) {
            mcpp::fallback::mark_install_complete(p);
        };

        // For custom indices, try project-level xlings data roots first.
        // Existing directories without the mcpp completion marker are treated
        // as stale/incomplete on this active resolve path and reinstalled.
        std::optional<std::filesystem::path> installed = findCompleteInstalled();

        // #278 masking guard. The hard INV-NAME check lives on the install path
        // below, so a machine that already has the package from an older index
        // snapshot keeps building. That asymmetry is exactly the trap the issue
        // names — local green, clean CI red — so make it visible here instead of
        // letting it stay silent.
        if (installed && luaContent) {
            if (auto violation = mcpp::manifest::
                    xpkg_name_form_violation_from_lua(*luaContent)) {
                mcpp::ui::warning(std::format(
                    "dependency '{}': {}\n"
                    "       resolving from the already-installed copy; a clean "
                    "environment (CI) will fail here",
                    depName, *violation));
            }
        }

        if (!installed) {
            if (luaContent) {
                auto field = mcpp::manifest::extract_mcpp_field(*luaContent);
                if (field.kind == mcpp::manifest::McppField::TableBody) {
                    auto depManifest = mcpp::manifest::synthesize_from_xpkg_lua(
                        *luaContent, depName, version, targetPlatform);
                    if (!depManifest) {
                        return std::unexpected(std::format(
                            "dependency '{}': {}", depName, depManifest.error().format()));
                    }
                    warn_unknown_xpkg_keys(*depManifest, depName);

                    auto preinstallKey = std::format("{}:{}@{}", ns, shortName, version);
                    if (preinstallStack.contains(preinstallKey)) {
                        return std::unexpected(std::format(
                            "dependency '{}': cyclic mcpp.deps while preparing install hooks",
                            depName));
                    }

                    if (!preinstallDone.contains(preinstallKey)) {
                        preinstallStack.insert(preinstallKey);
                        for (auto [childName, childSpec] : depManifest->dependencies) {
                            mcpp::pm::compat::normalize_nested_namespace(
                                childSpec.namespace_,
                                childSpec.shortName,
                                childSpec.legacyDottedKey);

                            if (auto r = selectDependencyCandidate(
                                    childSpec, childName); !r) {
                                preinstallStack.erase(preinstallKey);
                                return std::unexpected(r.error());
                            }

                            if (auto r = resolveSemver(childSpec, childName); !r) {
                                preinstallStack.erase(preinstallKey);
                                return std::unexpected(r.error());
                            }

                            if (!childSpec.isVersion()) continue;

                            ResolvedKey childKey{
                                childSpec.namespace_,
                                childSpec.shortName.empty() ? childName : childSpec.shortName,
                            };
                            if (auto child = loadVersionDep(
                                    childName,
                                    childKey.ns,
                                    childKey.shortName,
                                    childSpec.version); !child) {
                                preinstallStack.erase(preinstallKey);
                                return std::unexpected(child.error());
                            }
                        }
                        preinstallStack.erase(preinstallKey);
                        preinstallDone.insert(preinstallKey);
                    }
                }
            }

            // The address xlings is asked for is `<effectiveNamespace>:<literal
            // package.name>` (SPEC-001 §6), and BOTH halves come from the
            // descriptor the identity gate accepted — see
            // `mcpp::manifest::xpkg_wire_address` for why splitting the two
            // sources is the bug it is.
            auto wireAddr = mcpp::manifest::xpkg_wire_address(
                luaContent ? std::string_view(*luaContent) : std::string_view{},
                ns, shortName);
            if (luaContent) {
                if (auto violation = mcpp::manifest::
                        xpkg_name_form_violation_from_lua(*luaContent)) {
                    return std::unexpected(std::format(
                        "dependency '{}': {}", depName, *violation));
                }
            }
            // Human-facing name stays the resolved identity `<ns>.<short>` —
            // that is what the user wrote in [dependencies], so it is what the
            // progress line and errors should echo back.
            auto displayName = ns.empty() ? shortName
                : std::format("{}.{}", ns, shortName);
            mcpp::ui::info("Downloading", std::format("{} v{}", displayName, version));

            // #238: retain whatever error/warn text the child DID emit so we
            // can fold it into a diagnostic if install_packages exits non-zero.
            std::string capturedChildError;
            auto install_one = [&](std::string target) -> std::expected<mcpp::xlings::CallResult, mcpp::pm::CallError> {
                if (useProjectEnv) {
                    // Project/custom-index deps install into the project-local
                    // xlings data root (so a package's install hook can find
                    // sibling packages from the same index). The NDJSON
                    // interface honors this: in the pinned xlings the
                    // `install_packages` capability and the `install` CLI share
                    // `xim::cmd_install`, and the install destination is chosen
                    // by package *scope* (project vs global), not by transport.
                    // Using the interface (rather than the silenced direct CLI)
                    // restores the live `Downloading … [bar] X/Y Z/s` UI here,
                    // matching the toolchain and builtin-index paths.
                    auto projEnv = mcpp::config::make_project_xlings_env(**cfg, *root);
                    auto argsJson = std::format(
                        R"({{"targets":["{}"],"yes":true}})", target);
                    mcpp::fetcher::InstallProgressHandler progress;
                    auto r = mcpp::xlings::call(
                        projEnv, "install_packages", argsJson, &progress);
                    capturedChildError = progress.captured_error();
                    if (!r) return std::unexpected(mcpp::pm::CallError{r.error()});
                    return *r;
                }
                std::vector<std::string> targets{ std::move(target) };
                mcpp::fetcher::InstallProgressHandler progress;
                auto r = fetcher.install(targets, &progress);
                capturedChildError = progress.captured_error();
                return r;
            };
            // Target = `<namespace>:<literal name>@<version>` (SPEC-001 §6).
            //
            // The colon prefix is xlings' *effective namespace*, matched against
            // the descriptor's own `package.namespace` (xlings issue-381 design
            // §2.2) — NOT the index name. mcpp's `[indices] <ns> = {...}` keys
            // ARE namespaces, so the two coincide for a qualified request; for a
            // bare one they do NOT, which is exactly why the namespace has to be
            // read off the descriptor rather than off `ns`.
            //
            // A namespace-less upstream package (xim `opencv`) is addressed by
            // its bare literal name, with no prefix.
            auto target = std::format("{}@{}", wireAddr.target, version);
            // Keep every address we actually put on the wire. Diagnosing the
            // 2026-07-25 breakage needed MCPP_VERBOSE=1 to discover that mcpp
            // had asked for `mcpplibs:gtest` — the error itself only named the
            // dependency, which is the one thing nobody doubts.
            std::vector<std::string> attempted{ target };
            auto r = install_one(target);
            if (r && r->exitCode != 0 &&
                (ns.empty() || ns == mcpp::pm::kDefaultNamespace)) {
                // Compat retry for a bare/default-namespace request whose
                // descriptor could not be read (no `wireAddr` to trust): the
                // package may still be a `compat` one. Try BOTH spellings — a
                // SPEC-001 index keys it `compat:<short>`, a pre-SPEC-001 index
                // keys it by the literal `compat.<short>`. Sending only the
                // latter is what left the retry pointing at a name the migrated
                // index no longer has.
                for (auto&& compatTarget : {
                         std::format("compat:{}@{}", shortName, version),
                         std::format("compat.{}@{}", shortName, version) }) {
                    if (compatTarget == target) continue;
                    mcpp::ui::info("Downloading", std::format("{} v{}",
                        compatTarget.substr(0, compatTarget.rfind('@')), version));
                    attempted.push_back(compatTarget);
                    r = install_one(compatTarget);
                    if (!r || r->exitCode == 0) break;
                }
            }
            if (!r) return std::unexpected(std::format(
                "fetch '{}@{}': {}", depName, version, r.error().message));
            if (r->exitCode != 0) {
                // #238: the opaque `fetch failed (exit 1)` hid the actionable
                // context mcpp actually has. Reconstruct it: the target, the
                // configured index repos (read back from the seeded
                // .xlings.json — project scope when useProjectEnv, else the
                // global xlings home), any child error text we captured, plus
                // a hint about the known ≥2-repo xlings resolution gap. The
                // real fix lives in openxlings/xlings; this only surfaces WHY.
                auto xlingsJson = (useProjectEnv
                        ? (*root / ".mcpp")
                        : (*cfg)->xlingsHome())
                    / ".xlings.json";
                auto indexRepos = mcpp::pm::read_seeded_index_repos(xlingsJson);
                std::string childErr = capturedChildError;
                if (r->error) {
                    if (!childErr.empty()) childErr += "; ";
                    childErr += r->error->message;
                }
                auto target = std::format("{}@{}", depName, version);
                auto diag = mcpp::pm::format_install_failure_diagnostic(
                    target, r->exitCode, indexRepos, childErr);
                std::string tried;
                for (auto& a : attempted) {
                    if (!tried.empty()) tried += ", ";
                    tried += a;
                }
                diag += std::format("\n  wire address{} tried: {}",
                                    attempted.size() == 1 ? "" : "es", tried);
                return std::unexpected(std::move(diag));
            }
            // After install, check project data first for custom index packages.
            installed = findRawInstalled();
            if (!installed) return std::unexpected(std::format(
                "package '{}@{}' install path missing after fetch", depName, version));
            markInstalled(*installed);
        }
        std::filesystem::path verRoot = *installed;

        // Route xpkg.lua reading through the appropriate index.
        if (!luaContent) {
            luaContent = readLuaContent();
        }
        if (!luaContent) return std::unexpected(std::format(
            "dependency '{}': index entry not found in local clone", depName));
        auto field = mcpp::manifest::extract_mcpp_field(*luaContent);

        // 0.0.6+: read explicit namespace from xpkg lua if present.
        auto luaNs = mcpp::manifest::extract_xpkg_namespace(*luaContent);

        std::optional<mcpp::manifest::Manifest> manifest;
        std::filesystem::path effRoot = verRoot;
        auto loadFrom = [&](const std::filesystem::path& mcppToml)
            -> std::expected<void, std::string>
        {
            auto dm = mcpp::manifest::load(mcppToml);
            if (!dm) return std::unexpected(std::format(
                "dependency '{}' (at '{}'): {}",
                depName, mcppToml.string(), dm.error().format()));
            manifest = std::move(*dm);
            effRoot  = mcppToml.parent_path();
            return {};
        };
        if (field.kind == mcpp::manifest::McppField::StringPath) {
            auto matches = mcpp::modgraph::expand_glob(verRoot, field.value);
            if (matches.empty()) return std::unexpected(std::format(
                "dependency '{}': mcpp pointer '{}' did not match any "
                "file under '{}'", depName, field.value, verRoot.string()));
            if (matches.size() > 1) return std::unexpected(std::format(
                "dependency '{}': mcpp pointer '{}' matched {} files "
                "(expected exactly one)", depName, field.value, matches.size()));
            if (auto r = loadFrom(matches.front()); !r) return std::unexpected(r.error());
        } else if (field.kind == mcpp::manifest::McppField::TableBody) {
            auto dm = mcpp::manifest::synthesize_from_xpkg_lua(
                *luaContent, depName, version, targetPlatform);
            if (!dm) return std::unexpected(std::format(
                "dependency '{}': {}", depName, dm.error().format()));
            warn_unknown_xpkg_keys(*dm, depName);
            manifest = std::move(*dm);
            // effRoot stays as verRoot
        } else {
            std::vector<std::filesystem::path> matches;
            for (auto pat : { "mcpp.toml", "*/mcpp.toml" }) {
                matches = mcpp::modgraph::expand_glob(verRoot, pat);
                if (!matches.empty()) break;
            }
            if (matches.empty()) return std::unexpected(std::format(
                "dependency '{}': index entry has no `mcpp = ...` field, "
                "and no mcpp.toml was found at <verdir>/mcpp.toml or "
                "<verdir>/*/mcpp.toml — add an explicit `mcpp = \"<path>\"` "
                "or `mcpp = {{ ... }}` block to the .lua descriptor.",
                depName));
            if (matches.size() > 1) return std::unexpected(std::format(
                "dependency '{}': default mcpp.toml lookup matched {} "
                "files; pin one with explicit `mcpp = \"<path>\"`.",
                depName, matches.size()));
            if (auto r = loadFrom(matches.front()); !r) return std::unexpected(r.error());
        }
        // Propagate lua-level namespace into the loaded manifest when
        // the manifest itself doesn't carry one (Form A descriptors
        // whose upstream mcpp.toml predates the namespace field).
        // Guard: if the manifest's name already starts with luaNs+"."
        // (e.g. name="mcpplibs.tinyhttps" with luaNs="mcpplibs"),
        // the namespace is already embedded in the name — don't inject
        // it again or the scanner will produce a double-prefixed
        // qualified name like "mcpplibs.mcpplibs.tinyhttps".
        if (manifest->package.namespace_.empty() && !luaNs.empty()) {
            auto prefix = luaNs + ".";
            if (!manifest->package.name.starts_with(prefix)) {
                manifest->package.namespace_ = luaNs;
            }
        }

        if (auto r = materialize_generated_files(effRoot, *manifest); !r) {
            return std::unexpected(std::format(
                "dependency '{}': {}", depName, r.error()));
        }

        // Dependency-side L1 cfg merge (flags + sources): a descriptor's
        // `target_cfg` / a dep mcpp.toml's [target.'cfg(...)'.build] must
        // evaluate here too — before its globs expand. This is the version/
        // registry-dep half of the #229 funnel: every loadVersionDep() caller
        // (the main per-dependency loop, the multi-version mangling
        // secondary, and the SemVer-merge re-fetch) shares this one call site,
        // so a version dep is merged exactly once regardless of which of the
        // three paths loaded it. The path/git-dep half is the mirror-image
        // call right after ITS manifest load (dependency-manifest-acquisition
        // block below) — same function, same one-merge-per-package guarantee,
        // just keyed off a different loading branch since path/git deps never
        // pass through loadVersionDep.
        if (!manifest->conditionalConfigs.empty()) {
            merge_conditional_build_inputs(*manifest,
                                    cfgpred::context_for(overrides.target_triple),
                                    overrides.target_triple);
        }
        fold_build_defines_into_flags(manifest->buildConfig);

        return std::pair{effRoot, std::move(*manifest)};
    };

    struct DependencyEdge {
        std::size_t consumerPackageIndex = 0;
        std::size_t dependencyPackageIndex = 0;
        mcpp::modgraph::DependencyVisibility visibility =
            mcpp::modgraph::DependencyVisibility::Public;
        // #242/#243: the per-edge feature request that THIS consumer made of
        // THIS dependency. Feature activation must consume these off the edge
        // graph (union over all incoming edges) rather than re-scanning only
        // the root manifest's direct deps — otherwise a transitive dep's
        // requested features and its consumer's `default-features = false` are
        // silently dropped (resolution honors them per-edge; activation did not).
        std::vector<std::string> requestedFeatures;
        bool defaultFeatures = true;
    };
    std::vector<DependencyEdge> dependencyEdges;

    auto parseVisibility = [](std::string_view visibility) {
        if (visibility == "private")
            return mcpp::modgraph::DependencyVisibility::Private;
        if (visibility == "interface")
            return mcpp::modgraph::DependencyVisibility::Interface;
        return mcpp::modgraph::DependencyVisibility::Public;
    };

    auto packageIndexForConsumer = [&](std::size_t consumerDepIndex) {
        if (consumerDepIndex == kMainConsumer) return std::size_t{0};
        return consumerDepIndex + 1;
    };

    auto appendUniquePath =
        [](std::vector<std::filesystem::path>& dirs,
           const std::filesystem::path& dir) -> bool
    {
        if (std::find(dirs.begin(), dirs.end(), dir) != dirs.end()) return false;
        dirs.push_back(dir);
        return true;
    };

    auto appendUniquePaths =
        [&](std::vector<std::filesystem::path>& dirs,
            const std::vector<std::filesystem::path>& additions) -> bool
    {
        bool changed = false;
        for (auto const& dir : additions) {
            changed = appendUniquePath(dirs, dir) || changed;
        }
        return changed;
    };

    // ONE owner of "which compile-visible channels a build.mcpp directive
    // lands in" — shared by the dep loop and the root call site so the
    // next directive kind cannot be threaded through one and silently
    // missed in the other (the #242 two-derivations failure shape).
    // apply() mutates the manifest the program ran against; this folds
    // the NEW tail (recorded sizes → end) into the package's
    // usage-resolved privateBuild, which is what its TUs actually read.
    // Link/source/fingerprint residues stay at the call sites — they
    // genuinely differ between root and dep (see each).
    struct DirectiveMark { std::size_t c, cx, inc, incAfter; };
    auto markDirectiveTail = [](const mcpp::manifest::Manifest& mm) {
        return DirectiveMark{ mm.buildConfig.cflags.size(),
                              mm.buildConfig.cxxflags.size(),
                              mm.buildConfig.includeDirs.size(),
                              mm.buildConfig.includeDirsAfter.size() };
    };
    auto foldDirectiveTailIntoPrivateBuild =
        [&](auto& pkg, const mcpp::manifest::Manifest& ran,
            const DirectiveMark& t)
    {
        auto const& bc = ran.buildConfig;
        pkg.privateBuild.cflags.insert(pkg.privateBuild.cflags.end(),
            bc.cflags.begin() + t.c, bc.cflags.end());
        pkg.privateBuild.cxxflags.insert(pkg.privateBuild.cxxflags.end(),
            bc.cxxflags.begin() + t.cx, bc.cxxflags.end());
        // include-dir[/-after] directives are PRIVATE (design §3.1: Cargo
        // discipline — a build-time program must not widen the package's
        // public interface): privateBuild only, never publicUsage. The
        // after-dirs ride the typed #249 channel, which owns the
        // per-dialect degradations (cl.exe /I, NASM -I).
        for (auto it = bc.includeDirs.begin() + t.inc;
             it != bc.includeDirs.end(); ++it)
            appendUniquePath(pkg.privateBuild.includeDirs, *it);
        for (auto it = bc.includeDirsAfter.begin() + t.incAfter;
             it != bc.includeDirsAfter.end(); ++it)
            appendUniquePath(pkg.privateBuild.includeDirsAfter, *it);
    };


    auto appendUniqueFlags =
        [](std::vector<std::string>& flags,
           const std::vector<std::string>& additions) -> bool
    {
        bool changed = false;
        for (auto const& f : additions) {
            if (std::find(flags.begin(), flags.end(), f) != flags.end()) continue;
            flags.push_back(f);
            changed = true;
        }
        return changed;
    };

    auto expandIncludeDirs =
        [&](const std::filesystem::path& packageRoot,
            const mcpp::manifest::Manifest& manifest)
    {
        std::vector<std::filesystem::path> dirs;
        for (auto const& inc : manifest.buildConfig.includeDirs) {
            if (inc.is_absolute()) {
                appendUniquePath(dirs, inc);
                continue;
            }
            for (auto& dir : mcpp::modgraph::expand_dir_glob(
                     packageRoot, inc.generic_string())) {
                appendUniquePath(dirs, dir);
            }
        }
        return dirs;
    };

    // #249: same glob expansion for `include_dirs_after` (the -idirafter
    // channel — searched after the toolchain's system dirs).
    auto expandIncludeDirsAfter =
        [&](const std::filesystem::path& packageRoot,
            const mcpp::manifest::Manifest& manifest)
    {
        std::vector<std::filesystem::path> dirs;
        for (auto const& inc : manifest.buildConfig.includeDirsAfter) {
            if (inc.is_absolute()) {
                appendUniquePath(dirs, inc);
                continue;
            }
            for (auto& dir : mcpp::modgraph::expand_dir_glob(
                     packageRoot, inc.generic_string())) {
                appendUniquePath(dirs, dir);
            }
        }
        return dirs;
    };

    auto makePackageRoot =
        [&](const std::filesystem::path& packageRoot,
            const mcpp::manifest::Manifest& manifest)
    {
        mcpp::modgraph::PackageRoot pkg;
        pkg.root = packageRoot;
        pkg.manifest = manifest;
        pkg.usageResolved = true;

        pkg.privateBuild.includeDirs = expandIncludeDirs(packageRoot, manifest);
        pkg.privateBuild.includeDirsAfter = expandIncludeDirsAfter(packageRoot, manifest);
        pkg.privateBuild.cflags = manifest.buildConfig.cflags;
        pkg.privateBuild.cxxflags = manifest.buildConfig.cxxflags;
        pkg.publicUsage.includeDirs = pkg.privateBuild.includeDirs;
        pkg.publicUsage.includeDirsAfter = pkg.privateBuild.includeDirsAfter;
        pkg.linkUsage.ldflags = manifest.buildConfig.ldflags;
        return pkg;
    };

    packages[0] = makePackageRoot(*root, *m);

    auto recordDependencyEdge =
        [&](std::size_t consumerDepIndex,
            std::size_t dependencyPackageIndex,
            const mcpp::manifest::DependencySpec& spec)
    {
        const auto consumerPackageIndex = packageIndexForConsumer(consumerDepIndex);
        if (consumerPackageIndex >= packages.size()
            || dependencyPackageIndex >= packages.size()) {
            return;
        }
        const auto visibility = parseVisibility(spec.visibility);
        auto same = [&](const DependencyEdge& edge) {
            return edge.consumerPackageIndex == consumerPackageIndex
                && edge.dependencyPackageIndex == dependencyPackageIndex
                && edge.visibility == visibility;
        };
        if (std::find_if(dependencyEdges.begin(), dependencyEdges.end(), same)
            != dependencyEdges.end()) {
            return;
        }
        dependencyEdges.push_back(DependencyEdge{
            .consumerPackageIndex = consumerPackageIndex,
            .dependencyPackageIndex = dependencyPackageIndex,
            .visibility = visibility,
            .requestedFeatures = spec.features,
            .defaultFeatures = spec.defaultFeatures,
        });
    };

    auto computeUsageRequirements = [&] {
        bool changed = true;
        while (changed) {
            changed = false;
            for (auto const& edge : dependencyEdges) {
                if (edge.consumerPackageIndex >= packages.size()
                    || edge.dependencyPackageIndex >= packages.size()) {
                    continue;
                }
                auto& consumer = packages[edge.consumerPackageIndex];
                auto const& dependency = packages[edge.dependencyPackageIndex];

                if (edge.visibility == mcpp::modgraph::DependencyVisibility::Private
                    || edge.visibility == mcpp::modgraph::DependencyVisibility::Public) {
                    changed = appendUniquePaths(consumer.privateBuild.includeDirs,
                                                dependency.publicUsage.includeDirs)
                              || changed;
                    // #249: after-dirs ride the same edges but keep their
                    // after-ness — consumers receive them as -idirafter,
                    // never upgraded to -I.
                    changed = appendUniquePaths(consumer.privateBuild.includeDirsAfter,
                                                dependency.publicUsage.includeDirsAfter)
                              || changed;
                    // Interface defines (a dependency's active-feature `defines`)
                    // ride the same edges as include dirs: they must reach the
                    // consumer's own TUs so header-only switches like
                    // EIGEN_USE_BLAS take effect where the headers are used.
                    changed = appendUniqueFlags(consumer.privateBuild.cflags,
                                                dependency.publicUsage.cflags)
                              || changed;
                    changed = appendUniqueFlags(consumer.privateBuild.cxxflags,
                                                dependency.publicUsage.cxxflags)
                              || changed;
                }
                if (edge.visibility == mcpp::modgraph::DependencyVisibility::Public
                    || edge.visibility == mcpp::modgraph::DependencyVisibility::Interface) {
                    changed = appendUniquePaths(consumer.publicUsage.includeDirs,
                                                dependency.publicUsage.includeDirs)
                              || changed;
                    changed = appendUniquePaths(consumer.publicUsage.includeDirsAfter,
                                                dependency.publicUsage.includeDirsAfter)
                              || changed;
                    changed = appendUniqueFlags(consumer.publicUsage.cflags,
                                                dependency.publicUsage.cflags)
                              || changed;
                    changed = appendUniqueFlags(consumer.publicUsage.cxxflags,
                                                dependency.publicUsage.cxxflags)
                              || changed;
                }
            }
        }
    };

    auto normalizeDepLdflag = [](const std::filesystem::path& depRoot,
                                 const std::string& flag) {
        auto absolute_path = [&](std::string_view raw) {
            std::filesystem::path p{std::string(raw)};
            if (p.is_absolute() || raw.starts_with("$")) return p;
            return depRoot / p;
        };

        if (flag.starts_with("-L") && flag.size() > 2) {
            return "-L" + absolute_path(std::string_view(flag).substr(2)).string();
        }

        constexpr std::string_view rpathPrefix = "-Wl,-rpath,";
        if (flag.starts_with(rpathPrefix) && flag.size() > rpathPrefix.size()) {
            return std::string(rpathPrefix)
                 + absolute_path(std::string_view(flag).substr(rpathPrefix.size())).string();
        }

        return flag;
    };

    auto propagateLinkFlags = [&](const std::filesystem::path& depRoot,
                                  const mcpp::manifest::Manifest& depManifest)
        -> std::vector<std::string>
    {
        std::vector<std::string> added;
        for (auto const& flag : depManifest.buildConfig.ldflags) {
            auto normalized = normalizeDepLdflag(depRoot, flag);
            m->buildConfig.ldflags.push_back(normalized);
            added.push_back(std::move(normalized));
        }
        return added;
    };

    auto removeLinkFlags = [&](const std::vector<std::string>& flags) {
        auto& ldflags = m->buildConfig.ldflags;
        for (auto const& flag : flags) {
            auto pos = std::find(ldflags.begin(), ldflags.end(), flag);
            if (pos != ldflags.end()) ldflags.erase(pos);
        }
    };

    // Stage a dep's source files into a fresh directory, rewriting their
    // module / import declarations against `rename`. Used by the multi-
    // version mangling fallback (Level 1) so two cross-major copies of
    // the same package can coexist with distinct module names.
    //
    // Headers (referenced via `[build].include_dirs`) are NOT staged —
    // those keep pointing at the original install dir via absolutized
    // include paths.
    auto stage_with_rewrite = [](const std::filesystem::path& srcRoot,
                                  const std::filesystem::path& dstRoot,
                                  const mcpp::manifest::Manifest& depManifest,
                                  const std::map<std::string, std::string>& rename)
        -> std::expected<void, std::string>
    {
        std::error_code ec;
        std::filesystem::create_directories(dstRoot, ec);
        if (ec) return std::unexpected(std::format(
            "stage: cannot create '{}': {}", dstRoot.string(), ec.message()));

        // Resolve the source globs against the original root, falling
        // back to the convention default if the manifest didn't set any.
        std::vector<std::string> globs = depManifest.modules.sources;
        if (globs.empty()) {
            globs = { "src/**/*.cppm", "src/**/*.cpp",
                      "src/**/*.cc",   "src/**/*.c" };
        }
        // Glob exclusion (same as scan_one_into): `!` prefix removes.
        std::set<std::filesystem::path> sourceFiles;
        std::set<std::filesystem::path> excluded;
        for (auto const& g : globs) {
            if (!g.empty() && g[0] == '!') {
                for (auto& p : mcpp::modgraph::expand_glob(srcRoot, g.substr(1)))
                    excluded.insert(p);
            } else {
                for (auto& p : mcpp::modgraph::expand_glob(srcRoot, g))
                    sourceFiles.insert(p);
            }
        }
        for (auto& p : excluded) sourceFiles.erase(p);
        if (sourceFiles.empty()) {
            return std::unexpected(std::format(
                "stage: no source files found under '{}' (globs={})",
                srcRoot.string(), globs.size()));
        }

        for (auto const& f : sourceFiles) {
            auto rel = std::filesystem::relative(f, srcRoot, ec);
            if (ec) return std::unexpected(std::format(
                "stage: cannot relativize '{}': {}", f.string(), ec.message()));
            auto dst = dstRoot / rel;
            std::filesystem::create_directories(dst.parent_path(), ec);

            std::ifstream is(f);
            if (!is) return std::unexpected(std::format(
                "stage: cannot read '{}'", f.string()));
            std::stringstream buf; buf << is.rdbuf();
            std::string content = buf.str();

            std::string out = mcpp::pm::rewrite_module_decls(content, rename);
            std::ofstream os(dst);
            if (!os) return std::unexpected(std::format(
                "stage: cannot write '{}'", dst.string()));
            os << out;
        }
        return {};
    };

    // Stage 2a — feature-activated optional dependencies. Defined as local
    // lambdas (NOT file-scope functions): keeping their std::map instantiations
    // inside this implementation unit avoids polluting the exported module BMI,
    // which otherwise trips a GCC-16 modules bug ("failed to load pendings for
    // __normal_iterator") when other modules import std.
    auto activateFeatures = [](const mcpp::manifest::Manifest& pm,
                               const std::vector<std::string>& requested,
                               bool seedDefault = true) {
        return feature_closure(pm, requested, seedDefault); // single shared implementation
    };
    // Merge a manifest's active feature-deps into its `dependencies` map so the
    // worklist below pulls them like any normal dep. A top-level dep of the same
    // key is never overwritten; deps declared only under a feature appear only
    // when that feature is active. `seedDefault` carries consumer-side
    // `default-features = false` (#242): when a consumer opts out of this dep's
    // default set, feature-deps behind the default pseudo-feature stay dormant.
    auto mergeActiveFeatureDeps = [&](mcpp::manifest::Manifest& pm,
                                      const std::vector<std::string>& requested,
                                      bool seedDefault = true) {
        if (pm.featureDeps.empty()) return;
        for (auto& f : activateFeatures(pm, requested, seedDefault)) {
            auto it = pm.featureDeps.find(f);
            if (it == pm.featureDeps.end()) continue;
            for (auto& [k, spec] : it->second) pm.dependencies.try_emplace(k, spec);
        }
    };

    // #243: dep/feat forwarding. When a resolved package's feature F is active,
    // it may forward features to its dependencies (Cargo `[features] F =
    // ["dep/feat"]`). Injecting the forwarded feature into the child's request
    // BEFORE the child is pushed onto the worklist makes BOTH consumption points
    // observe it: resolution (mergeActiveFeatureDeps reads the child's
    // spec.features) and activation (recordDependencyEdge stores spec.features on
    // the P->D edge, which aggregatedRequest unions and apply() activates).
    // Transitive forwarding rides the BFS forward edge (root -> mid -> leaf).
    auto injectForwards = [](const mcpp::manifest::Manifest& parent,
                             const std::vector<std::string>& parentActive,
                             const std::string& childKey,
                             mcpp::manifest::DependencySpec& childSpec) {
        if (parent.featureForwards.empty()) return;
        for (auto const& f : parentActive) {
            auto it = parent.featureForwards.find(f);
            if (it == parent.featureForwards.end()) continue;
            for (auto const& [depKey, depFeat] : it->second) {
                if (depKey != childKey) continue;
                if (std::find(childSpec.features.begin(), childSpec.features.end(),
                              depFeat) == childSpec.features.end())
                    childSpec.features.push_back(depFeat);
            }
        }
    };
    // #243: a forward whose active feature targets a dependency that is not
    // declared (in [dependencies], [dev-dependencies], or an active
    // [feature-deps] already folded into `dependencies`) is a manifest bug —
    // name it instead of silently dropping. Only active features' forwards are
    // checked (lazy, like the unknown-requested-feature gate at ~2875).
    auto validateForwards = [&](const mcpp::manifest::Manifest& parent,
                                const std::vector<std::string>& parentActive,
                                std::string_view parentName)
        -> std::expected<void, std::string> {
        for (auto const& f : parentActive) {
            auto it = parent.featureForwards.find(f);
            if (it == parent.featureForwards.end()) continue;
            for (auto const& [depKey, depFeat] : it->second) {
                if (parent.dependencies.contains(depKey)
                    || parent.devDependencies.contains(depKey)) continue;
                auto msg = std::format(
                    "feature '{}' of '{}' forwards to dependency '{}' (as "
                    "'{}/{}') which is not declared in [dependencies] or "
                    "[feature-deps]", f, parentName, depKey, depKey, depFeat);
                if (overrides.strict) return std::unexpected(msg);
                mcpp::diag::warning("features/forwarding", msg);
            }
        }
        return {};
    };

    // Pull the root package's active feature-deps into its dependency set before
    // seeding, so `mcpp build --features X` resolves X's optional deps.
    std::vector<std::string> rootReq = parse_feature_request(overrides.features);
    mergeActiveFeatureDeps(*m, rootReq);
    // #243: the root's active features may forward features to its direct deps.
    std::vector<std::string> rootActive = feature_closure(*m, rootReq, true);
    if (auto fe = validateForwards(*m, rootActive, m->package.name); !fe)
        return std::unexpected(fe.error());

    // Seed the worklist from the main manifest. Dev-deps only when the
    // caller wants them; they're never propagated transitively.
    const std::string mainPkgLabel = m->package.name;
    for (auto& [n, s] : m->dependencies) {
        auto req = s;
        injectForwards(*m, rootActive, n, req);
        worklist.push_back({n, req, mainPkgLabel, req.version, kMainConsumer, {}});
    }
    if (includeDevDeps) {
        for (auto& [n, s] : m->devDependencies) {
            auto req = s;
            injectForwards(*m, rootActive, n, req);
            worklist.push_back({n, req, mainPkgLabel + " (dev-dep)",
                                req.version, kMainConsumer, {}});
        }
    }

    while (!worklist.empty()) {
        auto item = std::move(worklist.front());
        worklist.pop_front();

        const auto& name = item.name;
        auto& spec = item.spec;

        mcpp::pm::compat::normalize_nested_namespace(
            spec.namespace_, spec.shortName, spec.legacyDottedKey);
        if (spec.legacyDottedKey) {
            spec.candidates = {{
                .namespace_ = spec.namespace_,
                .shortName = spec.shortName,
            }};
        }

        if (auto r = selectDependencyCandidate(spec, name); !r) {
            return std::unexpected(r.error());
        }
        if (item.consumerDepIndex == kMainConsumer) {
            if (auto it = m->dependencies.find(name); it != m->dependencies.end()) {
                it->second.namespace_ = spec.namespace_;
                it->second.shortName = spec.shortName;
                it->second.candidates = spec.candidates;
            }
        }

        // Pin SemVer constraint before dedup/fetch.
        if (auto r = resolveSemver(spec, name); !r) {
            return std::unexpected(r.error());
        }

        ResolvedKey key{
            spec.namespace_,
            spec.shortName.empty() ? name : spec.shortName,
        };
        const std::string sourceKind =
            spec.isPath()    ? "path"
            : spec.isGit()    ? "git"
            : "version";

        if (auto it = resolved.find(key); it != resolved.end()) {
            // Conflict detection.
            if (it->second.source != sourceKind) {
                return std::unexpected(std::format(
                    "dependency '{}{}{}' is requested as both a {} dep "
                    "(by '{}') and a {} dep (by '{}'). Pick one.",
                    key.ns, key.ns.empty() ? "" : ".", key.shortName,
                    it->second.source, it->second.requestedBy,
                    sourceKind, item.requestedBy));
            }
            if (sourceKind == "version" && it->second.version != spec.version) {
                // SemVer merge attempt: AND-combine the two original
                // constraint strings and ask the index for a single version
                // satisfying both. Same-major caret/tilde/exact pairs that
                // overlap converge here; cross-major or otherwise
                // unsatisfiable pairs fall through to a hard error (a future
                // PR adds multi-version mangling as a Level-1 fallback).
                auto cfg = get_cfg();
                if (!cfg) return std::unexpected(cfg.error());
                mcpp::fetcher::Fetcher fetcher(**cfg);

                auto merged = mcpp::pm::try_merge_semver(
                    key.ns, key.shortName,
                    it->second.constraint,
                    item.originalConstraint,
                    fetcher, targetPlatform);
                if (!merged) {
                    // Level 1 fallback: multi-version mangling. Two
                    // versions can't be reconciled by SemVer, but they
                    // can coexist in the same build if we mangle the
                    // secondary copy's module name and rewrite the one
                    // consumer that asked for it. The primary keeps its
                    // authored module name so consumers that don't care
                    // about the secondary see no churn.
                    //
                    // MVP scope (these limits surface as clear errors):
                    //   * The conflicting consumer must be a dep, not
                    //     the main package — main-package mangling
                    //     would mean rewriting user-authored sources,
                    //     which is too surprising for a fallback path.
                    //   * The secondary version must be a leaf (no own
                    //     transitive deps) — recursive mangling is
                    //     deferred to a follow-up.
                    if (item.consumerDepIndex == kMainConsumer) {
                        return std::unexpected(std::format(
                            "dependency '{}{}{}' has irreconcilable versions:\n"
                            "  '{}' (constraint '{}') requested by '{}'\n"
                            "  '{}' (constraint '{}') requested by '{}'\n"
                            "SemVer merge: {}\n"
                            "Multi-version mangling can't help here — the conflict "
                            "involves the main package directly. Pin one version "
                            "explicitly in your mcpp.toml.",
                            key.ns, key.ns.empty() ? "" : ".", key.shortName,
                            it->second.version, it->second.constraint, it->second.requestedBy,
                            spec.version, item.originalConstraint, item.requestedBy,
                            merged.error()));
                    }

                    auto loaded = loadVersionDep(name, key.ns, key.shortName, spec.version);
                    if (!loaded) return std::unexpected(loaded.error());
                    auto& [secondaryRoot, secondaryManifest] = *loaded;

                    if (!secondaryManifest.dependencies.empty()) {
                        return std::unexpected(std::format(
                            "dependency '{}{}{}' has irreconcilable versions:\n"
                            "  '{}' requested by '{}'\n"
                            "  '{}' requested by '{}'\n"
                            "Multi-version mangling fallback only handles leaf "
                            "secondaries in 0.0.3 — but the secondary v{} declares "
                            "its own dependencies, which would need recursive "
                            "mangling. Pin one version explicitly, or wait for "
                            "the recursive-mangling extension.",
                            key.ns, key.ns.empty() ? "" : ".", key.shortName,
                            it->second.version, it->second.requestedBy,
                            spec.version, item.requestedBy,
                            spec.version));
                    }

                    // Module names in the source files use the dep's full
                    // [package].name (e.g. "mcpplibs.cmdline"), not the
                    // namespaced-subtable shortName. Use that for the
                    // rename key so the rewriter actually matches what the
                    // .cppm sources declare.
                    const std::string moduleName = secondaryManifest.package.name;
                    std::string mangled =
                        mcpp::pm::mangle_name(moduleName, spec.version);

                    // Stage layout:
                    //   <root>/target/.mangled/<consumerPkg>/<dep>__<version>/    ← rewritten secondary source
                    //   <root>/target/.mangled/<consumerPkg>/__self__/             ← rewritten consumer source
                    auto& consumerManifest = *dep_manifests[item.consumerDepIndex];
                    auto consumerRoot      = packages[item.consumerDepIndex + 1].root;
                    auto stageBase         = *root / "target" / ".mangled"
                                             / consumerManifest.package.name;
                    auto secStage          = stageBase
                                             / std::format("{}__{}", moduleName, spec.version);
                    auto consumerStage     = stageBase / "__self__";

                    std::map<std::string, std::string> rename{ {moduleName, mangled} };
                    if (auto r = stage_with_rewrite(secondaryRoot, secStage,
                                                     secondaryManifest, rename); !r)
                        return std::unexpected(r.error());
                    if (auto r = stage_with_rewrite(consumerRoot, consumerStage,
                                                     consumerManifest, rename); !r)
                        return std::unexpected(r.error());

                    // Re-anchor the consumer's PackageRoot at its staged copy
                    // so the modgraph scanner picks up the rewritten imports.
                    packages[item.consumerDepIndex + 1].root = consumerStage;

                    // Record the staged secondary as a brand-new dep entry
                    // under its mangled name, so future encounters of this
                    // exact (ns, mangled) pair dedup cleanly. The original
                    // primary entry (it->second) is untouched.
                    auto stagedManifest = secondaryManifest;
                    // Update [package].name to the mangled module name so
                    // the modgraph validator (which checks "exported module
                    // must be prefixed by package name") accepts the
                    // rewritten sources.
                    stagedManifest.package.name = mangled;
                    // Absolutize secondary's include_dirs against its original
                    // install root so the staged copy still finds headers.
                    for (auto& inc : stagedManifest.buildConfig.includeDirs) {
                        if (inc.is_relative()) inc = secondaryRoot / inc;
                    }
                    for (auto& inc : stagedManifest.buildConfig.includeDirsAfter) {
                        if (inc.is_relative()) inc = secondaryRoot / inc;
                    }

                    dep_manifests.push_back(
                        std::make_unique<mcpp::manifest::Manifest>(std::move(stagedManifest)));
                    dep_cache_identities.push_back({
                        .indexName   = cache_index_name(key.ns),
                        .packageName = mangled,
                        .version     = spec.version,
                    });
                    const auto depPackageIndex = packages.size();
                    packages.push_back(makePackageRoot(secStage, *dep_manifests.back()));
                    recordDependencyEdge(item.consumerDepIndex, depPackageIndex, spec);
                    auto linkFlagsAdded = propagateLinkFlags(secStage, *dep_manifests.back());

                    ResolvedKey mangledKey{key.ns, mangled};
                    resolved[mangledKey] = ResolvedRecord{
                        .version           = spec.version,
                        .constraint        = item.originalConstraint,
                        .requestedBy       = item.requestedBy,
                        .source            = "version",
                        .depIndex          = dep_manifests.size() - 1,
                        .linkFlagsAdded    = std::move(linkFlagsAdded),
                    };

                    mcpp::ui::info("Mangled",
                        std::format("{} v{} ↔ v{} → {} (cross-major fallback)",
                            moduleName, it->second.version, spec.version, mangled));
                    continue;
                }

                // Combine the constraint strings so future merges AND with
                // both. Empty originalConstraint means "any" — use "*".
                const std::string& addCstr =
                    item.originalConstraint.empty() ? std::string("*")
                                                    : item.originalConstraint;
                if (it->second.constraint.empty())
                    it->second.constraint = addCstr;
                else
                    it->second.constraint += "," + addCstr;

                if (*merged == it->second.version) {
                    // The existing pin already satisfies the new constraint —
                    // no re-fetch needed; just record this consumer edge.
                    recordDependencyEdge(item.consumerDepIndex,
                                         it->second.depIndex + 1,
                                         spec);
                    continue;
                }

                // Merged version differs from the previously-pinned one.
                // Re-fetch the dep at the merged version and replace the
                // earlier slot in dep_manifests / packages so the build plan
                // sees only one version. Old include_dir entries are evicted
                // and the new manifest's entries are appended.
                mcpp::ui::info("Merged",
                    std::format("{}{}{} {} ⨯ {} → v{}",
                        key.ns, key.ns.empty() ? "" : ".", key.shortName,
                        it->second.version, spec.version, *merged));
                auto reloaded = loadVersionDep(name, key.ns, key.shortName, *merged);
                if (!reloaded) return std::unexpected(reloaded.error());
                auto& [newRoot, newManifest] = *reloaded;

                // Name match against the re-loaded manifest.
                {
                    const std::string& expectedShort =
                        spec.shortName.empty() ? name : spec.shortName;
                    // Also accept the fully-qualified form (ns.short) since
                    // synthesize_from_xpkg_lua may set package.name to the
                    // composite name for backward compat.
                    auto expectedComposite = spec.namespace_.empty()
                        ? std::string{}
                        : std::format("{}.{}", spec.namespace_, expectedShort);
                    const bool nameOk =
                        newManifest.package.name == expectedShort
                        || newManifest.package.name == name
                        || (!expectedComposite.empty()
                            && newManifest.package.name == expectedComposite);
                    if (!nameOk) {
                        return std::unexpected(std::format(
                            "dependency '{}' (merged to v{}) resolved to "
                            "package '{}' (mismatch with declared name '{}')",
                            name, *merged, newManifest.package.name,
                            expectedShort));
                    }
                }

                removeLinkFlags(it->second.linkFlagsAdded);
                auto linkFlagsAdded = propagateLinkFlags(newRoot, newManifest);

                // Replace in dep_manifests + packages. depIndex is the slot
                // in dep_manifests; packages = [main, dep_0, dep_1, …], so
                // packages[depIndex+1] is the same dep.
                *dep_manifests[it->second.depIndex] = std::move(newManifest);
                packages[it->second.depIndex + 1] =
                    makePackageRoot(newRoot, *dep_manifests[it->second.depIndex]);
                recordDependencyEdge(item.consumerDepIndex,
                                     it->second.depIndex + 1,
                                     spec);

                it->second.version            = *merged;
                it->second.linkFlagsAdded     = std::move(linkFlagsAdded);
                if (it->second.depIndex < dep_cache_identities.size())
                    dep_cache_identities[it->second.depIndex].version = *merged;

                // Walk the *new* manifest's deps so their constraints feed
                // future merges. Already-resolved children dedup via the
                // resolved map.
                const std::string newLabel = std::format("{}{}{}@{}",
                    key.ns, key.ns.empty() ? "" : ".",
                    key.shortName, *merged);
                for (auto& [child_name, child_spec] :
                        dep_manifests[it->second.depIndex]->dependencies) {
                    worklist.push_back({child_name, child_spec, newLabel,
                                        child_spec.version,
                                        it->second.depIndex, {}});
                }
                continue;
            }
            // Same key, same version (or compatible path/git) — already
            // processed; still record the dependency edge before skipping.
            // Usage propagation is per edge, not per unique package: two
            // consumers can need the same dep's public surface even though
            // the dep itself is fetched/scanned once.
            if (it->second.depIndex + 1 < packages.size()) {
                recordDependencyEdge(item.consumerDepIndex,
                                     it->second.depIndex + 1,
                                     spec);
            }
            continue;
        }

        std::filesystem::path dep_root;

        if (spec.isPath()) {
            // Path-based: resolve relative to the consumer's root dir.
            // For top-level deps this is the project root; for transitive
            // deps it's the parent dep's directory (stored in resolveRoot).
            dep_root = spec.path;
            auto base = item.resolveRoot.empty() ? *root : item.resolveRoot;
            if (dep_root.is_relative()) dep_root = base / dep_root;
            dep_root = std::filesystem::weakly_canonical(dep_root);
        } else if (spec.isGit()) {
            // Git-based (M4 #5): clone into ~/.mcpp/git/<hash>/ and treat
            // as a path dep from there. Branch refs are floating, so resolve
            // them to a commit before forming the cache key; this lets
            // `mcpp update <dep>` pick up a moved branch without deleting
            // unrelated git caches.
            auto mcppHome = [] {
                if (auto* e = std::getenv("MCPP_HOME"); e && *e)
                    return std::filesystem::path(e);
                if (auto* e = std::getenv("HOME"); e && *e)
                    return std::filesystem::path(e) / ".mcpp";
                return std::filesystem::current_path() / ".mcpp";
            }();
            std::string resolvedGitRev = spec.gitRev;
            if (spec.gitRefKind == "branch") {
                auto ref = std::format("refs/heads/{}", spec.gitRev);
                auto cmd = std::format(
                    "git ls-remote {} {} 2>&1",
                    mcpp::platform::shell::quote(spec.git),
                    mcpp::platform::shell::quote(ref));
                auto r = mcpp::platform::process::capture(cmd);
                if (r.exit_code != 0) {
                    return std::unexpected(std::format(
                        "git ls-remote of '{}' failed:\n{}", spec.git, r.output));
                }
                std::istringstream is(r.output);
                is >> resolvedGitRev;
                if (resolvedGitRev.empty()) {
                    return std::unexpected(std::format(
                        "git branch '{}' not found in '{}'", spec.gitRev, spec.git));
                }
            }

            // Cache key: hash(url + refkind + declared ref + resolved commit).
            // For fixed rev/tag deps the declared ref is also the resolved ref.
            std::hash<std::string> H;
            auto urlHash = std::format("{:016x}",
                H(spec.git + "|" + spec.gitRefKind + "|" + spec.gitRev
                  + "|" + resolvedGitRev));
            auto gitRoot = mcppHome / "git" / urlHash;
            std::error_code ec;
            std::filesystem::create_directories(gitRoot.parent_path(), ec);
            if (!std::filesystem::exists(gitRoot / ".git")) {
                mcpp::ui::info("Cloning",
                    std::format("{} ({} = {})", spec.git, spec.gitRefKind, spec.gitRev));
                std::string cloneCmd;
                if (spec.gitRefKind == "branch") {
                    cloneCmd = std::format(
                        "git clone --depth 1 --branch {} {} {} && cd {} && git checkout --quiet {} 2>&1",
                        mcpp::platform::shell::quote(spec.gitRev),
                        mcpp::platform::shell::quote(spec.git),
                        mcpp::platform::shell::quote(gitRoot.string()),
                        mcpp::platform::shell::quote(gitRoot.string()),
                        mcpp::platform::shell::quote(resolvedGitRev));
                } else {
                    // For tag/rev: full clone, then checkout (depth-1 may miss the rev).
                    cloneCmd = std::format(
                        "git clone {} {} && cd {} && git checkout --quiet {} 2>&1",
                        mcpp::platform::shell::quote(spec.git),
                        mcpp::platform::shell::quote(gitRoot.string()),
                        mcpp::platform::shell::quote(gitRoot.string()),
                        mcpp::platform::shell::quote(spec.gitRev));
                }
                std::string out;
                {
                    auto r = mcpp::platform::process::capture(cloneCmd);
                    out = r.output;
                    int rc = r.exit_code;
                    if (rc != 0) {
                        std::filesystem::remove_all(gitRoot, ec);
                        return std::unexpected(std::format(
                            "git clone of '{}' failed:\n{}", spec.git, out));
                    }
                }
            }
            if (item.consumerDepIndex == kMainConsumer) {
                auto source = std::format("git+{}#{}={}",
                    spec.git, spec.gitRefKind, spec.gitRev);
                if (spec.gitRefKind == "branch") source += "@" + resolvedGitRev;
                root_git_lock_identities[name] = GitLockIdentity{
                    .source = std::move(source),
                    .hash = std::format("fnv1a:{:016x}", H(spec.git + "|"
                        + spec.gitRefKind + "|" + spec.gitRev + "|"
                        + resolvedGitRev)),
                };
            }
            dep_root = gitRoot;
        }
        // (version-source: dep_root + manifest are loaded together via
        // loadVersionDep below since the index entry drives both.)

        // Manifest acquisition.
        //   - Path/git dep: dep_root is the source tree, mcpp.toml at root.
        //   - Version dep: delegate to loadVersionDep — the index entry's
        //     `mcpp` field decides where mcpp.toml lives (StringPath /
        //     TableBody / default lookup).
        std::optional<mcpp::manifest::Manifest> dep_manifest;
        if (spec.isPath() || spec.isGit()) {
            if (!std::filesystem::exists(dep_root / "mcpp.toml")) {
                return std::unexpected(std::format(
                    "{} dependency '{}' (at '{}') has no mcpp.toml",
                    spec.isGit() ? "git" : "path", name, dep_root.string()));
            }
            auto dm = mcpp::manifest::load(dep_root / "mcpp.toml");
            if (!dm) {
                return std::unexpected(std::format(
                    "dependency '{}' (at '{}'): {}",
                    name, dep_root.string(), dm.error().format()));
            }
            dep_manifest = std::move(*dm);
            // #229: path/git-dep half of the L1 cfg funnel — mirrors the
            // loadVersionDep call site above (loadFrom's L1 cfg merge, ~1740
            // lines up). Before this fix, path/git deps never ran this merge
            // at all: their `[target.'cfg(...)'.build] sources` were parsed
            // into `conditionalConfigs` but never folded into
            // `buildConfig.sources` / `modules.sources`, so the modgraph scan
            // never saw the file — link-time `undefined reference`. Must run
            // BEFORE `propagateLinkFlags`/`makePackageRoot` below, which
            // snapshot this manifest's flags/sources into `packages[]`.
            if (!dep_manifest->conditionalConfigs.empty()) {
                merge_conditional_build_inputs(*dep_manifest,
                    cfgpred::context_for(overrides.target_triple),
                    overrides.target_triple);
            }
            fold_build_defines_into_flags(dep_manifest->buildConfig);
        } else {
            auto loaded = loadVersionDep(name, key.ns, key.shortName, spec.version);
            if (!loaded) return std::unexpected(loaded.error());
            dep_root     = std::move(loaded->first);
            dep_manifest = std::move(loaded->second);
        }

        // Name match via compat::resolve_package_name — handles both
        // canonical (explicit namespace field) and legacy (dotted name)
        // forms transparently.
        {
            auto resolved = mcpp::pm::compat::resolve_package_name(
                dep_manifest->package.name, dep_manifest->package.namespace_);
            const std::string& expectedShort =
                spec.shortName.empty() ? name : spec.shortName;
            const bool nameOk =
                resolved.shortName == expectedShort
                || dep_manifest->package.name == expectedShort
                || dep_manifest->package.name ==
                    mcpp::pm::compat::qualified_name(spec.namespace_, expectedShort);
            if (!nameOk) {
                return std::unexpected(std::format(
                    "dependency '{}' resolved to package '{}' (mismatch with declared name '{}')",
                    name, dep_manifest->package.name, expectedShort));
            }
        }

        // Stage 2a: merge this dependency's active feature-deps into its own
        // dependency set before its children are pushed, so a dep's feature can
        // transitively pull a provider. `spec.features` = features the consumer
        // requested for this dep.
        mergeActiveFeatureDeps(*dep_manifest, spec.features, spec.defaultFeatures);

        auto linkFlagsAdded = propagateLinkFlags(dep_root, *dep_manifest);

        // Move the manifest into stable storage so we can later look it up
        // by depIndex (the SemVer merger needs to overwrite the slot).
        dep_manifests.push_back(
            std::make_unique<mcpp::manifest::Manifest>(std::move(*dep_manifest)));
        dep_cache_identities.push_back({
            .indexName   = cache_index_name(key.ns),
            .packageName = name,
            .version     = sourceKind == "version"
                ? spec.version
                : dep_manifests.back()->package.version,
        });
        const auto depPackageIndex = packages.size();
        packages.push_back(makePackageRoot(dep_root, *dep_manifests.back()));
        recordDependencyEdge(item.consumerDepIndex, depPackageIndex, spec);

        // Record this dep as resolved so future encounters of the same
        // (ns, name) hit the fast path (skip / merge / conflict).
        resolved[key] = ResolvedRecord{
            .version           = sourceKind == "version" ? spec.version : "",
            .constraint        = sourceKind == "version" ? item.originalConstraint : "",
            .requestedBy       = item.requestedBy,
            .source            = sourceKind,
            .depIndex          = dep_manifests.size() - 1,
            .linkFlagsAdded    = std::move(linkFlagsAdded),
        };

        // Recurse: the dep's own [dependencies] become new worklist items.
        // dev-dependencies are intentionally NOT walked — those are
        // private to the dep's test runs, not part of its public ABI.
        const std::string thisDepLabel = std::format(
            "{}{}{}@{}",
            key.ns,
            key.ns.empty() ? "" : ".",
            key.shortName,
            sourceKind == "version" ? spec.version : sourceKind);
        const std::size_t selfIdx = dep_manifests.size() - 1;
        // #243: forward this dep's active features to ITS children before they
        // are pushed (transitive dep->dep forwarding rides the BFS forward
        // edge). Uses the SAME closure inputs as mergeActiveFeatureDeps above
        // (this edge's spec.features, already carrying any forward injected by
        // this dep's own consumer, + defaultFeatures), so activation agrees
        // with resolution.
        auto depActive = feature_closure(*dep_manifests.back(), spec.features,
                                         spec.defaultFeatures);
        if (auto fe = validateForwards(*dep_manifests.back(), depActive,
                                       dep_manifests.back()->package.name); !fe)
            return std::unexpected(fe.error());
        for (auto& [child_name, child_spec] : dep_manifests.back()->dependencies) {
            auto childReq = child_spec;
            injectForwards(*dep_manifests.back(), depActive, child_name, childReq);
            worklist.push_back({child_name, childReq, thisDepLabel,
                                childReq.version, selfIdx, dep_root});
        }
    }

    computeUsageRequirements();

    // ─── Feature activation (Cargo-style, additive) ────────────────────
    // activated(pkg) = pkg.[features].default ∪ features requested for it
    // (root: --features; deps: the root dep spec's `features = [...]`).
    // Implied features expand transitively. Each active feature becomes
    // -DMCPP_FEATURE_<NAME> on that package's compile flags.
    // (Transitive dep→dep feature requests are not yet propagated.)
    // Also captured here: the root package's active feature set, reused below
    // for the [targets.*] required_features gate.
    std::set<std::string> activeRootFeatures;
    // Capability accumulation (Stage 3): which packages provide each capability,
    // and which (capability, requiring-package) pairs need binding. Filled by
    // apply() as each package's features activate; bound after the loops below.
    std::map<std::string, std::vector<std::string>> capProviders;
    std::vector<std::pair<std::string, std::string>> capRequires;
    {
        auto sanitize = [](std::string f) {
            for (auto& c : f)
                c = std::isalnum(static_cast<unsigned char>(c))
                  ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : '_';
            return f;
        };
        auto activate = [](const mcpp::manifest::Manifest& pm,
                           const std::vector<std::string>& requested,
                           bool seedDefault = true) {
            return feature_closure(pm, requested, seedDefault); // single shared implementation
        };
        auto apply = [&](mcpp::modgraph::PackageRoot& pkg,
                         const std::vector<std::string>& requested,
                         bool seedDefault = true) {
            auto active = activate(pkg.manifest, requested, seedDefault);
            // Capability accumulation: package-level provides always count;
            // feature-scoped provides/requires count only when the feature is
            // active. Requirements are bound after all packages are processed.
            const auto& pcap = pkg.manifest.package.name;
            for (auto& cap : pkg.manifest.provides) capProviders[cap].push_back(pcap);
            for (auto& f : active) {
                if (auto it = pkg.manifest.featureProvides.find(f);
                    it != pkg.manifest.featureProvides.end())
                    for (auto& cap : it->second) capProviders[cap].push_back(pcap);
                if (auto it = pkg.manifest.featureRequires.find(f);
                    it != pkg.manifest.featureRequires.end())
                    for (auto& cap : it->second) capRequires.emplace_back(cap, pcap);
            }
            for (auto& f : active) {
                auto def = "-DMCPP_FEATURE_" + sanitize(f);
                pkg.manifest.buildConfig.cflags.push_back(def);
                pkg.manifest.buildConfig.cxxflags.push_back(def);
                pkg.privateBuild.cflags.push_back(def);
                pkg.privateBuild.cxxflags.push_back(def);
                // Feature System v2 Stage 1: package-owned `defines` declared on
                // this feature ride alongside the automatic MCPP_FEATURE_ macro.
                // Bare names desugar to -D<x>, matching [targets.*] `defines`.
                if (auto it = pkg.manifest.buildConfig.featureDefines.find(f);
                    it != pkg.manifest.buildConfig.featureDefines.end())
                    for (auto& d : it->second) {
                        auto fdef = "-D" + d;
                        pkg.manifest.buildConfig.cflags.push_back(fdef);
                        pkg.manifest.buildConfig.cxxflags.push_back(fdef);
                        pkg.privateBuild.cflags.push_back(fdef);
                        pkg.privateBuild.cxxflags.push_back(fdef);
                        // Interface-propagate the user-declared feature define:
                        // a header-only dependency's switch (e.g. EIGEN_USE_BLAS)
                        // only takes effect in the TU that includes its headers,
                        // so consumers that enable the feature must see it too.
                        // computeUsageRequirements() flows publicUsage flags into
                        // each consumer's privateBuild along Public/Interface
                        // edges, mirroring include_dirs. The automatic
                        // MCPP_FEATURE_<NAME> macro stays private to the owning
                        // package (it is a build signal, not a public contract).
                        pkg.publicUsage.cflags.push_back(fdef);
                        pkg.publicUsage.cxxflags.push_back(fdef);
                    }
            }
            // Feature-gated sources (e.g. gtest's gtest_main.cc behind "main"):
            // drop EVERY feature-listed glob from the default build, then add
            // back only the ones whose feature is active. Runs even when no
            // feature is active, so a gated source is excluded by default.
            //
            // The DROP is build-mode only (!includeDevDeps). `mcpp test`
            // (includeDevDeps) keeps the full surface so the dev-dependency
            // track's per-test main detection (run_tests / make_plan) still sees
            // gtest_main.cc and prunes it per test — the two tracks stay
            // decoupled; gtest's descriptor keeps gtest_main.cc in base `sources`
            // too, so skipping the drop leaves it visible.
            //
            // The ADD runs in BOTH modes. A descriptor may list a glob ONLY under
            // `features` and never in base `sources` (xpkg's `features.X.sources`
            // lands in featureSources alone — compat.spdlog's `compiled`,
            // compat.cjson's `utils`, compat.eigen's `eigen_blas`). Gating the add
            // on !includeDevDeps meant those sources were never compiled under
            // `mcpp test` → link-time `undefined reference` (the eigen_blas
            // `dgemm_` failure, long misread as a linking follow-up: it was
            // source-set resolution, not linking). Add is dedup'd so gtest's
            // doubly-listed gtest_main.cc cannot land twice.
            auto& bc = pkg.manifest.buildConfig;
            if (!bc.featureSources.empty()) {
                if (!includeDevDeps) {
                    // glob → owned by at least one ACTIVE feature?
                    std::set<std::string> activeNow(active.begin(), active.end());
                    std::map<std::string, bool> gated;
                    for (auto& [f, globs] : bc.featureSources)
                        for (auto& g : globs)
                            gated[g] = gated[g] || activeNow.contains(f);
                    auto drop = [&](std::vector<std::string>& v) {
                        std::erase_if(v, [&](const std::string& s) { return gated.contains(s); });
                    };
                    drop(bc.sources);
                    drop(pkg.manifest.modules.sources);
                    // Dropping the glob STRING is not enough: files it matches
                    // may still be covered by a broader base glob (the default
                    // src/** — the mcpp.toml G5 case). An inactive gate becomes
                    // a `!` exclusion so the gate actually gates; active gates
                    // are re-added below.
                    for (auto& [g, isActive] : gated) {
                        if (isActive || g.starts_with("!")) continue;
                        bc.sources.push_back("!" + g);
                        pkg.manifest.modules.sources.push_back("!" + g);
                    }
                }
                std::set<std::string> activeSet(active.begin(), active.end());
                auto add = [](std::vector<std::string>& v, const std::string& g) {
                    if (std::ranges::find(v, g) == v.end()) v.push_back(g);
                };
                for (auto& [f, globs] : bc.featureSources) {
                    if (!activeSet.contains(f)) continue;
                    for (auto& g : globs) {
                        add(bc.sources, g);
                        add(pkg.manifest.modules.sources, g);
                    }
                }
            }
            // #253: per-feature per-glob flags — fold each ACTIVE feature's
            // entries into the base globFlags funnel. Everything downstream
            // (scanner glob match, per-TU flag landing, zero-hit warning,
            // fingerprint serialization) consumes the ONE vector unchanged.
            // Appended AFTER base entries, features in map (= name) order, so
            // application order is deterministic and a feature rule wins over
            // a broader base rule via "last flag wins". An inactive feature
            // contributes nothing — its dead globs no longer exist to warn
            // about. Deliberately OUTSIDE any includeDevDeps gate: like the
            // sources ADD above, `mcpp build` and `mcpp test` must agree
            // (0.0.94 dual-path invariant). featureOrigin tags the entry so
            // the scanner's zero-hit warning can name the owning feature.
            //
            // Routed through the SAME append(BuildInputs&) the cfg axis uses
            // (#258): both axes are contributing additive build inputs, so
            // "how does a contribution combine with the base" must have one
            // answer. Only the flags half of the feature axis is expressible
            // that way — feature `sources` above carry DROP-then-ADD
            // semantics, and feature `defines` are interface contributions
            // that propagate along Public edges, so neither is a plain
            // append and neither belongs in BuildInputs.
            for (auto& [f, entries] : bc.featureFlags) {
                if (std::ranges::find(active, f) == active.end()) continue;
                mcpp::manifest::BuildInputs contribution;
                for (auto const& gf : entries) {
                    auto tagged = gf;
                    tagged.featureOrigin = f;
                    contribution.globFlags.push_back(std::move(tagged));
                }
                mcpp::manifest::append(bc, contribution);
            }
        };
        if (!packages.empty()) {
            auto rootReq = parse_feature_request(overrides.features);
            // Strict schema check: a requested feature must exist in the
            // target package's [features] table when one is declared (a
            // package with no [features] accepts any request — pure-define
            // usage). Covers backend= sugar (feature backend-<x>) too.
            auto unknown_requested = [](const mcpp::manifest::Manifest& pm,
                                        const std::vector<std::string>& requested)
                -> std::optional<std::string> {
                if (pm.featuresMap.empty()) return std::nullopt;
                for (auto& f : requested)
                    if (!pm.featuresMap.contains(f)) return f;
                return std::nullopt;
            };
            if (auto bad = unknown_requested(packages[0].manifest, rootReq)) {
                auto msg = std::format(
                    "--features requests '{}' which [features] does not declare", *bad);
                if (overrides.strict) return std::unexpected(msg);
                mcpp::diag::warning("features/request", msg);
            }
            apply(packages[0], rootReq);
            for (auto& f : activate(*m, rootReq)) activeRootFeatures.insert(f);
        }
        // #242/#243: the feature request for a dependency PACKAGE, aggregated
        // over ALL its incoming edges (a package may be depended on by several
        // consumers — diamond — or reached only transitively). Cargo semantics:
        // requested features UNION; default-features stays on unless EVERY
        // consumer opted out. Sourcing this from the authoritative edge graph —
        // rather than scanning only the root manifest's direct deps — makes
        // activation AGREE with resolution (mergeActiveFeatureDeps, which reads
        // the true per-edge spec): a transitive dep's requested features and its
        // consumer's `default-features = false` are no longer silently dropped.
        auto aggregatedRequest = [&](std::size_t depPkgIndex)
            -> std::pair<std::vector<std::string>, bool> {
            std::vector<std::string> feats;
            bool anyEdge = false, anyDefault = false;
            for (auto const& edge : dependencyEdges) {
                if (edge.dependencyPackageIndex != depPkgIndex) continue;
                anyEdge = true;
                if (edge.defaultFeatures) anyDefault = true;
                for (auto const& f : edge.requestedFeatures)
                    if (std::find(feats.begin(), feats.end(), f) == feats.end())
                        feats.push_back(f);
            }
            return { std::move(feats), anyEdge ? anyDefault : true };
        };
        for (std::size_t i = 1; i < packages.size(); ++i) {
            auto& pname = packages[i].manifest.package.name;
            auto [req, depDefaultFeatures] = aggregatedRequest(i);
            if (!req.empty() && !packages[i].manifest.featuresMap.empty()) {
                for (auto& f : req) {
                    if (packages[i].manifest.featuresMap.contains(f)) continue;
                    auto msg = std::format(
                        "dependency '{}' does not declare requested feature '{}' "
                        "in its [features] table", pname, f);
                    if (overrides.strict) return std::unexpected(msg);
                    mcpp::diag::warning("features/request", msg);
                }
            }
            // Always apply: even with no requested/default feature, a dep with
            // feature-gated sources must have those sources dropped by default.
            // depDefaultFeatures carries the consumer's `default-features = false`
            // (#242): when opted out, the dep's [features].default is not seeded.
            apply(packages[i], req, depDefaultFeatures);
        }

        // ── G2: dependency build.mcpp (Cargo build.rs model) ────────────────
        // Runs AFTER feature activation (the env contract exposes the dep's
        // active features) and BEFORE the modgraph scan (generated sources
        // must be visible to the glob walk). Scope is Cargo's: flag directives
        // land in the dep's own buildConfig (its TUs only); link directives
        // ride the dep's ldflags to the final link. Artifacts and generated
        // files live in the CONSUMING project's tree — a registry package
        // root is shared across projects and may be read-only; it is never
        // written to.
        for (std::size_t i = 1; i < packages.size(); ++i) {
            auto& pkg = packages[i];
            std::error_code bpEc;
            if (!std::filesystem::exists(pkg.root / "build.mcpp", bpEc)) continue;
            auto host = host_tc_for_build_program();
            if (!host) return std::unexpected(host.error());
            // Same edge-graph aggregation as feature activation above, so a
            // dep build.mcpp sees the SAME active feature set the dep is built
            // with (incl. transitive requests / default-features opt-out).
            auto [req, depDefaultFeatures] = aggregatedRequest(i);
            auto dirSafe = [](std::string s) {
                for (auto& c : s) if (c == '/' || c == '\\' || c == ':') c = '_';
                return s;
            };
            mcpp::build::BuildProgramEnv bpEnv;
            bpEnv.targetTriple = resolvedTargetCanonical;
            bpEnv.profile      = effectiveProfile;
            bpEnv.features     = feature_closure(pkg.manifest, req, depDefaultFeatures);
            bpEnv.artifactsDir = *root / "target" / ".build-mcpp" / "deps"
                / (dirSafe(pkg.manifest.package.name) + "@" + pkg.manifest.package.version);
            bpEnv.genBase      = bpEnv.artifactsDir / "out";
            // mcpp#241: expose this package's resolved dependencies (verdir /
            // payload root) as MCPP_DEP_<NAME>_DIR. Uses the authoritative
            // consumer→dep edge graph (no name-guessing); covers feature-
            // activated deps too (mergeActiveFeatureDeps folded them into
            // `dependencies` before the edges were recorded). A dep is emitted
            // under BOTH its canonical package name AND its namespace-stripped
            // short name, so `mcpp::dep_dir("compat.zlib")` and
            // `mcpp::dep_dir("zlib")` both resolve regardless of which spelling
            // the author used in `deps`. (The ROOT project's build.mcpp gets
            // the same treatment at its own call site right after this loop.)
            for (auto const& edge : dependencyEdges) {
                if (edge.consumerPackageIndex != i) continue;
                auto const& depPkg = packages[edge.dependencyPackageIndex];
                const auto& canon = depPkg.manifest.package.name;
                bpEnv.depDirs.emplace_back(canon, depPkg.root);
                if (auto dot = canon.rfind('.'); dot != std::string::npos
                        && dot + 1 < canon.size())
                    bpEnv.depDirs.emplace_back(canon.substr(dot + 1), depPkg.root);
            }
            auto& bcDep = pkg.manifest.buildConfig;
            const auto mark = markDirectiveTail(pkg.manifest);
            const auto ldN = bcDep.ldflags.size();
            if (auto r = mcpp::build::run_build_program(
                    pkg.manifest, pkg.root, host->first, host->second,
                    pkg.manifest.cppStandard.canonical, bpEnv);
                !r) {
                return std::unexpected(std::format(
                    "dependency '{}': {}", pkg.manifest.package.name, r.error()));
            }
            // Cargo scope wiring: compile-visible tail → privateBuild (the
            // shared fold above; the dep's TUs read privateBuild, not bc —
            // its consumers read publicUsage, which the fold never touches;
            // the bcDep entries themselves are inert here: the descriptor
            // include_dirs propagation snapshotted publicUsage at
            // makePackageRoot, long before this pass). Dep residue: link
            // flags — dep ldflags were propagated to the root during the
            // BFS walk, which ran before this pass — forward the new tail
            // (link-search paths are already absolute from parse_line).
            foldDirectiveTailIntoPrivateBuild(pkg, pkg.manifest, mark);
            m->buildConfig.ldflags.insert(m->buildConfig.ldflags.end(),
                bcDep.ldflags.begin() + ldN, bcDep.ldflags.end());
        }

        // apply() may have added interface defines to packages' publicUsage
        // flags (a dependency's active-feature `defines`). Re-run the usage
        // fixpoint so those flags flow into each consumer's privateBuild — the
        // first pass (above) ran before features were activated. Idempotent:
        // include-dir/flag propagation is unique-append.
        computeUsageRequirements();

        // ─── Capability binding (Stage 3) ──────────────────────────────────
        // For each required capability, bind exactly one provider from the
        // graph. Deterministic: an explicit [capabilities] pin wins; otherwise
        // 0 providers / ≥2 providers are hard errors (never a silent guess); a
        // single provider binds with no config. The provider's link/include
        // requirements already flow through normal dependency mechanics — this
        // pass is the selection-and-validation layer. See the capability-model
        // design doc.
        // --cap cap=provider[,cap=provider] overrides [capabilities] pins.
        for (std::size_t p = 0; p < overrides.capabilities.size();) {
            auto c = overrides.capabilities.find_first_of(", ", p);
            auto tok = overrides.capabilities.substr(
                p, c == std::string::npos ? std::string::npos : c - p);
            if (auto eq = tok.find('='); eq != std::string::npos)
                m->capabilityPins[tok.substr(0, eq)] = tok.substr(eq + 1);
            if (c == std::string::npos) break;
            p = c + 1;
        }

        std::set<std::string> boundCaps;
        for (auto& [cap, requirer] : capRequires) {
            if (!boundCaps.insert(cap).second) continue;   // one diagnosis per cap
            auto& pins = m->capabilityPins;
            // Dedup candidates, preserve first-seen order.
            std::vector<std::string> cands;
            if (auto it = capProviders.find(cap); it != capProviders.end())
                for (auto& p : it->second)
                    if (std::find(cands.begin(), cands.end(), p) == cands.end())
                        cands.push_back(p);
            if (auto pit = pins.find(cap); pit != pins.end()) {
                const auto& pin = pit->second;
                if (std::find(cands.begin(), cands.end(), pin) == cands.end()) {
                    std::string list;
                    for (auto& c : cands) list += (list.empty() ? "" : ", ") + c;
                    return std::unexpected(std::format(
                        "capability '{}' pinned to provider '{}' (via [capabilities]), "
                        "but no such provider is in the graph; candidates: [{}]",
                        cap, pin, list));
                }
                continue;   // pin satisfied
            }
            if (cands.empty())
                return std::unexpected(std::format(
                    "no package provides capability '{}' required by '{}'; add a "
                    "dependency that declares `provides = [\"{}\"]`", cap, requirer, cap));
            if (cands.size() > 1) {
                std::string list;
                for (auto& c : cands) list += (list.empty() ? "" : ", ") + c;
                return std::unexpected(std::format(
                    "capability '{}' has multiple providers in the graph: [{}]; select "
                    "one with [capabilities] {} = \"<provider>\" or --cap {}=<provider>",
                    cap, list, cap, cap));
            }
            // exactly one → bound implicitly.
        }
    }

    // ── L3: ROOT build.mcpp (moved after dependency resolution, design §3.1
    // item 4) ────────────────────────────────────────────────────────────────
    // Runs HERE — after dep resolution + feature activation (so the contract
    // env can expose MCPP_DEP_<NAME>_DIR exactly like the dep loop above does)
    // and BEFORE the modgraph scan / flag canonicalization / fingerprint (so
    // its generated=/source= sources and flag directives are fully visible).
    // Ordering invariants preserved relative to the pre-move call site:
    // materialize_generated_files (may produce build.mcpp itself) and the L1
    // cfg merge still run earlier — ONLY this call moved later.
    //
    // One wrinkle the old ordering hid: back then apply() mutated *m BEFORE
    // `packages[0] = makePackageRoot(*root, *m)` snapshotted buildConfig into
    // privateBuild/manifest — the copies the scan and per-TU flag assembly
    // actually read. Now the snapshot (and root feature activation on it)
    // already happened, so mirror the directive TAILS into packages[0]
    // explicitly, the same way the dep loop does for its package.
    if (std::filesystem::exists(*root / "build.mcpp")) {
        auto host = host_tc_for_build_program();
        if (!host) return std::unexpected(host.error());
        mcpp::build::BuildProgramEnv bpEnv;
        bpEnv.targetTriple = resolvedTargetCanonical;
        bpEnv.profile      = effectiveProfile;
        // Same expression as the pre-move call site (and same order), so the
        // contract hash — and therefore the build.mcpp cache — is unchanged
        // across the move for feature-identical builds.
        bpEnv.features     = feature_closure(*m, parse_feature_request(overrides.features));
        // mcpp#241 (root): the root's resolved direct deps, from the same
        // authoritative edge graph as the dep loop (consumer index 0 = root),
        // emitted under canonical AND namespace-stripped names.
        for (auto const& edge : dependencyEdges) {
            if (edge.consumerPackageIndex != 0) continue;
            auto const& depPkg = packages[edge.dependencyPackageIndex];
            const auto& canon = depPkg.manifest.package.name;
            bpEnv.depDirs.emplace_back(canon, depPkg.root);
            if (auto dot = canon.rfind('.'); dot != std::string::npos
                    && dot + 1 < canon.size())
                bpEnv.depDirs.emplace_back(canon.substr(dot + 1), depPkg.root);
        }
        auto& bcRoot = m->buildConfig;
        const auto mark = markDirectiveTail(*m);
        const auto rldN = bcRoot.ldflags.size(), rsrcN = bcRoot.sources.size(),
                   rmodN = m->modules.sources.size();
        if (auto bp = mcpp::build::run_build_program(
                *m, *root, host->first, host->second,
                m->cppStandard.canonical, bpEnv);
            !bp) {
            return std::unexpected(bp.error());
        }
        auto& pkg0 = packages[0];
        // Compile-visible tail → privateBuild: the shared fold (same owner
        // as the dep loop; the root's TUs read privateBuild).
        foldDirectiveTailIntoPrivateBuild(pkg0, *m, mark);
        // Root residues — apply() mutated *m, but packages[0].manifest is a
        // value-copy snapshot taken at makePackageRoot, so everything the
        // scan/fingerprint read from the snapshot needs the tail mirrored:
        // sources → the scan walks packages[0].manifest, not *m.
        pkg0.manifest.buildConfig.sources.insert(
            pkg0.manifest.buildConfig.sources.end(),
            bcRoot.sources.begin() + rsrcN, bcRoot.sources.end());
        pkg0.manifest.modules.sources.insert(
            pkg0.manifest.modules.sources.end(),
            m->modules.sources.begin() + rmodN, m->modules.sources.end());
        // Fingerprint metadata (canonical_package_build_metadata folds
        // packages[].manifest.buildConfig) — mirror the flag/include tails,
        // as the old pre-snapshot ordering implicitly did.
        pkg0.manifest.buildConfig.cflags.insert(
            pkg0.manifest.buildConfig.cflags.end(),
            bcRoot.cflags.begin() + mark.c, bcRoot.cflags.end());
        pkg0.manifest.buildConfig.cxxflags.insert(
            pkg0.manifest.buildConfig.cxxflags.end(),
            bcRoot.cxxflags.begin() + mark.cx, bcRoot.cxxflags.end());
        pkg0.manifest.buildConfig.includeDirs.insert(
            pkg0.manifest.buildConfig.includeDirs.end(),
            bcRoot.includeDirs.begin() + mark.inc, bcRoot.includeDirs.end());
        pkg0.manifest.buildConfig.includeDirsAfter.insert(
            pkg0.manifest.buildConfig.includeDirsAfter.end(),
            bcRoot.includeDirsAfter.begin() + mark.incAfter,
            bcRoot.includeDirsAfter.end());
        // Link flags → the final link reads *m (already applied); keep the
        // linkUsage snapshot and fingerprint metadata equivalent too.
        pkg0.linkUsage.ldflags.insert(pkg0.linkUsage.ldflags.end(),
            bcRoot.ldflags.begin() + rldN, bcRoot.ldflags.end());
        pkg0.manifest.buildConfig.ldflags.insert(
            pkg0.manifest.buildConfig.ldflags.end(),
            bcRoot.ldflags.begin() + rldN, bcRoot.ldflags.end());
    }

    // [targets.*] required_features gate: a target is emitted only when ALL its
    // required features are active in this build; otherwise it is silently
    // skipped. A pure build-selection knob — it runs before the modgraph/plan
    // so gated-out targets cost nothing.
    std::erase_if(m->targets, [&](const mcpp::manifest::Target& t) {
        for (auto const& rf : t.requiredFeatures)
            if (!activeRootFeatures.contains(rf)) return true;
        return false;
    });

    // The dialect-complete standard flag: spelled per-dialect and carrying
    // the module-graph-global dialect flags (issue #210). ONE string shared
    // by the p1689 scan and the std BMI prebuild so scan-time, prebuild-time
    // and compile-time dialect provably agree. Both this and make_plan go
    // through the same cppfly merge, so the c++fly gates (and the
    // c++latest/c++fly per-toolchain std spelling) stay graph-consistent.
    std::string stdFlagAndDialect = mcpp::toolchain::cppfly::std_flag(
        *tc, m->cppStandard.canonical, m->cppStandard.level);
    if (m->cppStandard.experimental) {
        // c++fly is best-effort by design: say exactly what this toolchain
        // got and what it lacks (the value's contract, design §5.4).
        auto fly = mcpp::toolchain::cppfly::resolve(*tc);
        std::string enabled, skipped;
        for (auto& f : fly.features) {
            auto& dst = f.enabled ? enabled : skipped;
            if (!dst.empty()) dst += ", ";
            dst += f.name;
            if (f.enabled && !f.flags.empty()) dst += std::format(" ({})", f.flags);
            if (!f.enabled) dst += std::format(" ({})", f.reason);
        }
        std::println("c++fly on {}: {}; enabled: {}; skipped: {}",
                     tc->label(), stdFlagAndDialect,
                     enabled.empty() ? "(none)" : enabled,
                     skipped.empty() ? "(none)" : skipped);
    }
    for (auto& f : mcpp::toolchain::cppfly::effective_dialect_flags(
             *tc, m->cppStandard.experimental,
             mcpp::manifest::dialect_flags(m->buildConfig))) {
        stdFlagAndDialect += ' ';
        stdFlagAndDialect += f;
    }

    // mcpp#225 (E2): observability marker for the source-discovery phase —
    // `mcpp run`'s fast path (build_run_target/try_fast_run in execute.cppm)
    // skips prepare_build ENTIRELY on a cache hit, so this line's absence
    // under MCPP_VERBOSE=1 on a second `mcpp run` is the "did we re-scan"
    // signal the e2e test asserts on (tests/e2e/114_run_scan_scope.sh).
    mcpp::log::verbose("scan", "scanning module sources");

    // Modgraph: regex scanner by default; opt-in to compiler-driven P1689
    // scanner via env var MCPP_SCANNER=p1689 (see docs/27).
    auto scan = [&] {
        const char* sel = std::getenv("MCPP_SCANNER");
        if (sel && std::string_view(sel) == "p1689") {
            auto tmp = std::filesystem::temp_directory_path()
                     / std::format("mcpp_p1689_{}", std::random_device{}());
            std::filesystem::create_directories(tmp);
            return mcpp::modgraph::scan_packages_p1689(packages, *tc, tmp,
                                                       stdFlagAndDialect);
        }
        return mcpp::modgraph::scan_packages(packages);
    }();
    if (!scan.errors.empty()) {
        std::string msg = "scanner errors:\n";
        for (auto& e : scan.errors) msg += "  " + e.format() + "\n";
        return std::unexpected(msg);
    }
    for (auto& w : scan.warnings) {
        mcpp::diag::warning("modgraph/scan", w.format());
    }

    auto report = mcpp::modgraph::validate(scan.graph, *m, *root);
    for (auto& w : report.warnings) {
        if (w.path.empty()) mcpp::diag::warning("modgraph/validate", w.message);
        else mcpp::diag::warning("modgraph/validate",
                                 std::format("{}: {}", w.path.string(), w.message));
    }
    if (!report.ok()) {
        std::string msg = "validation errors:\n";
        for (auto& e : report.errors) {
            if (e.path.empty()) msg += "  " + e.message + "\n";
            else msg += "  " + e.path.string() + ": " + e.message + "\n";
        }
        return std::unexpected(msg);
    }

    bool needsStdModule = graph_or_targets_import_std(scan.graph, *m, *root);
    if (needsStdModule && !tc->hasImportStd) {
        return std::unexpected(std::format(
            "source imports std but toolchain '{}' provides no std module source",
            tc->label()));
    }

    // Compute fingerprint (no lockfile in M1 → empty hash)
    mcpp::toolchain::FingerprintInputs fpi;
    fpi.toolchain            = *tc;
    fpi.cppStandard         = m->package.standard;
    fpi.compileFlags        = canonical_compile_flags(*m)
                              + canonical_package_build_metadata(packages);
    if (m->cppStandard.experimental) {
        // c++fly gate flags are derived (not manifest-declared): fold them in
        // so a cppfly table change across mcpp versions re-fingerprints.
        for (auto& f : mcpp::toolchain::cppfly::resolve(*tc).flags) {
            fpi.compileFlags += ' ';
            fpi.compileFlags += f;
        }
    }
    fpi.dependencyLockHash = "";    // M2
    fpi.stdBmiHash         = "";    // updated after stdmod build (chicken/egg ok for M1)
    auto fp = mcpp::toolchain::compute_fingerprint(fpi);

    // Pre-build std module only when the source graph actually imports it.
    std::filesystem::path stdBmiPath;
    std::filesystem::path stdObjectPath;
    std::filesystem::path stdCompatBmiPath;
    std::filesystem::path stdCompatObjectPath;
    if (needsStdModule) {
        // The std BMI must be compiled with the SAME dialect set its
        // importers use (issue #210: -freflection gates libstdc++'s <meta> —
        // a std BMI built without it structurally lacks std::meta). Both
        // pieces were already in the fingerprint; this fixes the COMMAND
        // construction the fingerprint promised (stdFlagAndDialect above).
        auto sm = mcpp::toolchain::ensure_built(
            *tc, fp.hex, m->package.standard, stdFlagAndDialect,
            mcpp::platform::macos::deployment_target(
                m->buildConfig.macosDeploymentTarget));
        if (!sm) return std::unexpected(sm.error().message);
        stdBmiPath = sm->bmiPath;
        stdObjectPath = sm->objectPath;
        stdCompatBmiPath = sm->compatBmiPath;
        stdCompatObjectPath = sm->compatObjectPath;
    }

    if (print_fingerprint) {
        std::println("Toolchain: {}", tc->label());
        std::println("Fingerprint: {}", fp.hex);
        for (std::size_t i = 0; i < fp.parts.size(); ++i) {
            std::println("  [{}] {}", i + 1, fp.parts[i]);
        }
    }

    BuildContext ctx;
    ctx.strict      = overrides.strict;
    ctx.manifest    = *m;
    ctx.tc          = *tc;
    ctx.fp          = fp;
    ctx.projectRoot= *root;
    ctx.outputDir  = target_dir(*tc, fp, *root);
    ctx.stdBmi     = stdBmiPath;
    ctx.stdObject  = stdObjectPath;
    auto planResult = mcpp::build::make_plan(*m, *tc, fp, scan.graph, report.topoOrder,
                                             packages, *root, ctx.outputDir,
                                             stdBmiPath, stdObjectPath);
    if (!planResult) return std::unexpected(planResult.error());
    ctx.plan        = std::move(*planResult);
    ctx.plan.stdCompatBmiPath = stdCompatBmiPath;
    ctx.plan.stdCompatObjectPath = stdCompatObjectPath;

    // Clang: discover clang-scan-deps for P1689 dyndep scanning.
    if (mcpp::toolchain::is_clang(*tc)) {
        if (auto sd = mcpp::toolchain::clang::find_scan_deps(*tc)) {
            ctx.plan.scanDepsPath = *sd;
        }
    }

    // ─── Assembly units: validate + resolve the assembler ─────────────
    // .S/.s ride the C driver (GAS) — the MSVC dialect has no such path.
    // .asm is NASM: x86-family only, and the binary is resolved LAZILY —
    // only when the plan actually contains .asm units — as a hard failure,
    // never a silent skip (a dropped .o surfaces as undefined references
    // much later; fail here with the real cause instead).
    {
        bool hasGas = false, hasNasm = false;
        for (auto& cu : ctx.plan.compileUnits) {
            auto ext = cu.source.extension();
            if (ext == ".S" || ext == ".s") hasGas = true;
            else if (ext == ".asm")         hasNasm = true;
        }
        if (hasGas && mcpp::toolchain::dialect_for(*tc).id == "msvc") {
            return std::unexpected(std::string(
                "GAS assembly sources (.S/.s) are not supported by the MSVC "
                "toolchain; use NASM syntax (.asm) or a MinGW/LLVM toolchain, "
                "or `!`-exclude them in [build].sources"));
        }
        if (hasNasm) {
            auto trip = mcpp::toolchain::triple::parse(tc->targetTriple)
                            .value_or(mcpp::toolchain::triple::host_triple());
            auto fmt = trip.nasm_format();
            if (!fmt) {
                return std::unexpected(std::format(
                    "NASM sources (.asm) are x86-only, but the target is {}; "
                    "gate them off non-x86 targets (a feature, or a "
                    "`!`-exclude glob in [build].sources)", trip.str()));
            }
            ctx.plan.nasmFormat = *fmt;

            // #232: nasm used to go through a bespoke `ensure_nasm` path
            // whose `if (cfgNasm)` guard silently swallowed a `get_cfg()`
            // bootstrap failure (misreporting it as "no nasm"), and whose
            // install fallback never refreshed the package index and
            // downgraded a failed install to a warning. Surface the real
            // config error, then provision through the SAME synchronous
            // gate the compiler toolchain uses (index refresh before
            // install, blocking install, hard error on failure) — see the
            // toolchain resolution block above (~line 872-899).
            auto cfgNasm = get_cfg();
            if (!cfgNasm) return std::unexpected(cfgNasm.error());

            std::optional<std::filesystem::path> nasmBin =
                mcpp::xlings::find_usable_nasm(mcpp::config::make_xlings_env(**cfgNasm));
            if (!nasmBin) {
                mcpp::fetcher::Fetcher nasmFetcher(**cfgNasm);
                mcpp::fetcher::InstallProgressHandler nasmProgress;
                auto nasmTarget = std::format("xim:nasm@{}",
                    mcpp::xlings::pinned::kNasmVersion);
                auto payload = nasmFetcher.resolve_xpkg_path(
                    nasmTarget, /*autoInstall=*/true, &nasmProgress);
                if (!payload) {
                    return std::unexpected(std::format(
                        "NASM sources (.asm) present but nasm provisioning "
                        "failed: {}", payload.error().message));
                }
                nasmBin = mcpp::xlings::find_sandbox_nasm(
                    mcpp::config::make_xlings_env(**cfgNasm));
            }
            if (!nasmBin) {
                return std::unexpected(std::string(
                    "NASM sources (.asm) present but no usable nasm (>= 2.16) "
                    "was found or installable; install one via `xlings install "
                    "nasm` or your system package manager"));
            }
            ctx.plan.nasmPath = *nasmBin;
        }
    }

    // ─── M3.2: BMI cache stage / populate-task collection ─────────────
    // For each version-based dep package (i.e. fetched from a registry,
    // not a path dep), check the global BMI cache. If cached → stage into
    // the project's target dir so ninja sees those outputs as up-to-date
    // and skips them. If not → record a populate task for AFTER build.
    //
    // Path deps don't go through the cache: their sources can change at
    // any time outside fingerprint awareness.
    auto cfg2 = get_cfg();
    if (cfg2) {
        std::error_code mkEc;
        std::filesystem::create_directories(ctx.outputDir, mkEc);
        auto usable_object_rel = [](const std::filesystem::path& rel)
            -> std::optional<std::string>
        {
            auto s = rel.generic_string();
            if (s.empty() || s == "." || s == ".." || s.starts_with("../")) {
                return std::nullopt;
            }
            return s;
        };
        auto object_cache_path = [&](const std::filesystem::path& objectPath) {
            if (objectPath.is_absolute()) {
                if (auto s = usable_object_rel(
                        objectPath.lexically_relative(ctx.outputDir / "obj"))) {
                    return *s;
                }
            }
            if (auto s = usable_object_rel(objectPath.lexically_relative("obj"))) {
                return *s;
            }
            return objectPath.filename().generic_string();
        };
        for (std::size_t i = 1; i < packages.size(); ++i) {  // skip [0] = main
            const auto& pkgRoot   = packages[i];
            const auto* depIdent  = i - 1 < dep_cache_identities.size()
                ? &dep_cache_identities[i - 1]
                : nullptr;
            const auto fallbackName = pkgRoot.manifest.package.namespace_.empty()
                ? pkgRoot.manifest.package.name
                : std::format("{}.{}", pkgRoot.manifest.package.namespace_,
                              pkgRoot.manifest.package.name);
            const auto& depName   = depIdent ? depIdent->packageName : fallbackName;
            const auto& depVer    = depIdent && !depIdent->version.empty()
                ? depIdent->version
                : pkgRoot.manifest.package.version;

            // Find this dep's spec from the consumer manifest to know
            // if it's path-based or version-based.
            auto specIt = m->dependencies.find(depName);
            // Path AND git deps bypass the BMI cache: their sources can
            // change outside the fingerprint's awareness.
            bool skipCache = (specIt != m->dependencies.end() &&
                              (specIt->second.isPath() || specIt->second.isGit()));
            if (specIt == m->dependencies.end()) {
                auto devIt = m->devDependencies.find(depName);
                if (devIt != m->devDependencies.end()) {
                    skipCache = devIt->second.isPath() || devIt->second.isGit();
                }
            }
            if (skipCache) continue;

            auto bmiT = mcpp::toolchain::bmi_traits(*tc);
            mcpp::bmi_cache::CacheKey key {
                .mcppHome    = (*cfg2)->mcppHome,
                .fingerprint = fp.hex,
                .indexName   = depIdent
                    ? depIdent->indexName
                    : (*cfg2)->defaultIndex,
                .packageName = depName,
                .version     = depVer,
                .bmiDirName  = std::string(bmiT.bmiDir),
                .manifestTag = std::string(bmiT.manifestPrefix),
            };

            // Compute the artifacts list from the build plan: every
            // CompileUnit whose source lies under this dep's root contributes.
            mcpp::bmi_cache::DepArtifacts arts;
            for (auto& cu : ctx.plan.compileUnits) {
                std::error_code ec;
                auto rel = std::filesystem::relative(cu.source, pkgRoot.root, ec);
                if (ec || rel.empty()) continue;
                auto rels = rel.string();
                if (rels.starts_with("..")) continue;       // not under depRoot

                if (cu.providesModule) {
                    std::string bmi;
                    for (char c : *cu.providesModule)
                        bmi.push_back(c == ':' ? '-' : c);
                    bmi += std::string(bmiT.bmiExt);
                    arts.bmiFiles.push_back(std::move(bmi));
                }
                arts.objFiles.push_back(object_cache_path(cu.object));
            }

            if (mcpp::bmi_cache::is_cached(key)) {
                auto staged = mcpp::bmi_cache::stage_into(key, ctx.outputDir);
                if (staged) {
                    ctx.cachedDepLabels.push_back(
                        std::format("{} v{}", depName, depVer));
                    continue;       // skip populate task; it's already cached
                }
                // stage failed — fall through to recompile + repopulate
            }
            ctx.depsToPopulate.push_back({ std::move(key), std::move(arts) });
        }
    }
    // ──────────────────────────────────────────────────────────────────

    // Write/update mcpp.lock for any version-based deps that succeeded.
    // Path deps are intentionally NOT locked — their source is local filesystem.
    {
        mcpp::lockfile::Lockfile lock;
        lock.schemaVersion = 2;

        // Lock custom index shas from manifest [indices] section.
        for (auto const& [idxName, spec] : m->indices) {
            if (spec.is_local() || spec.is_builtin()) continue;
            mcpp::lockfile::LockedIndex li;
            li.name = idxName;
            li.url  = spec.url;
            li.rev  = spec.rev;   // may be empty if not yet resolved
            lock.indices.push_back(std::move(li));
        }

        for (auto const& [name, spec] : m->dependencies) {
            if (spec.isPath()) continue;
            mcpp::lockfile::LockedPackage lp;
            lp.name       = name;
            if (spec.isGit()) {
                auto gitIt = root_git_lock_identities.find(name);
                lp.version = spec.gitRev;
                if (gitIt == root_git_lock_identities.end()) {
                    lp.source = std::format("git+{}#{}={}",
                        spec.git, spec.gitRefKind, spec.gitRev);
                    std::hash<std::string> hasher;
                    lp.hash = std::format("fnv1a:{:016x}", hasher(lp.source));
                } else {
                    lp.source = gitIt->second.source;
                    lp.hash = gitIt->second.hash;
                }
            } else {
                lp.namespace_ = spec.namespace_.empty()
                    ? std::string{}
                    : spec.namespace_;
                lp.version    = spec.version;
                // Use the namespace and resolved version as the source identifier.
                // For custom indices, include the index name for traceability.
                auto sourceIndex = lp.namespace_.empty()
                    ? std::string(mcpp::pm::kDefaultNamespace)
                    : lp.namespace_;
                lp.source     = std::format("index+{}@{}", sourceIndex, lp.version);
                // Use a deterministic hash based on namespace + name + version.
                // A future PR can replace this with a real content hash from the
                // xpkg.lua's declared sha256 or from the install plan.
                std::hash<std::string> hasher;
                auto hashInput = std::format("{}:{}@{}", sourceIndex, name, lp.version);
                lp.hash = std::format("fnv1a:{:016x}", hasher(hashInput));
            }
            lock.packages.push_back(std::move(lp));
        }
        if (!lock.packages.empty() || !lock.indices.empty()) {
            auto lockPath = *root / "mcpp.lock";
            (void)mcpp::lockfile::write(lock, lockPath);
        }
    }

    // Apply [runtime.<capability>] provider = "<pkg>" overrides: prefer the
    // named provider for matching capabilities (capability name prefix match).
    // Warn if the named provider isn't in the dependency graph.
    for (auto& [capKey, prov] : ctx.manifest.runtimeConfig.providerOverrides) {
        bool found = false;
        std::stable_partition(ctx.plan.runtimeProviders.begin(),
                              ctx.plan.runtimeProviders.end(),
                              [&](const auto& pr) {
            bool match = pr.capability.rfind(capKey, 0) == 0 && pr.provider == prov;
            found = found || match;
            return match;
        });
        if (!found) {
            std::println(stderr,
                "warning: [runtime.{}] provider = \"{}\" — no such provider in the "
                "dependency graph for that capability", capKey, prov);
        }
    }

    // Capability-driven ABI enforcement, dimensional (see src/toolchain/abi.cppm
    // and .agents/docs/2026-06-27-abi-compat-model-single-pr-design.md). Each
    // dependency may constrain specific toolchain dimensions via `abi:`
    // capabilities (libc / cxxstdlib / arch / os / cxxabi); UNSPECIFIED
    // DIMENSIONS ARE DON'T-CARE. The legacy bare form `abi:glibc` maps to the
    // libc dimension only — so a glibc *C library* (glfw) builds fine under a
    // clang+libc++ toolchain on `*-linux-gnu` (libc is still glibc), which the
    // previous single-axis check wrongly rejected. The toolchain is resolved
    // before the dep graph, so this enforces/diagnoses rather than reselects —
    // abi-driven reselection is a resolution-ordering follow-up.
    {
        const auto prof = mcpp::toolchain::abi_profile(ctx.tc);
        std::vector<mcpp::toolchain::AbiConstraint> constraints;
        for (auto& cap : ctx.plan.runtimeCapabilities) {
            std::string provider;
            for (auto& [c, p] : ctx.plan.runtimeProviders)
                if (c == cap) { provider = p; break; }
            if (auto con = mcpp::toolchain::parse_abi_capability(
                    cap, provider.empty() ? std::string_view{"?"} : std::string_view{provider}))
                constraints.push_back(std::move(*con));
        }
        if (auto mismatches = mcpp::toolchain::abi_check(prof, constraints);
            !mismatches.empty()) {
            const auto& mm = mismatches.front();
            return std::unexpected(std::format(
                "ABI incompatibility: dependency '{}' requires {}={}, but the "
                "resolved toolchain '{}' provides {}={}.\n"
                "       fix: select a {}-compatible toolchain "
                "(e.g. gcc@16.1.0 for glibc) or set [toolchain] in mcpp.toml.",
                mm.source, mcpp::toolchain::dim_name(mm.dim), mm.need,
                ctx.tc.label(), mcpp::toolchain::dim_name(mm.dim), mm.got,
                mm.need));
        }
    }

    // Per-build resolution manifest artifact: a machine-readable record of the
    // resolved plan (toolchain/abi, runtime closure, capabilities+providers,
    // deps) written next to the build outputs. Same data as `mcpp why`; usable
    // by CI/tooling. (capability -> plan, serialized.)
    {
        const std::string tcAbi =
            ctx.tc.targetTriple.find("musl") != std::string::npos ? "musl"
            : ctx.tc.stdlibId == "libc++"                          ? "libc++"
            : ctx.tc.compiler == mcpp::toolchain::CompilerId::MSVC ? "msvc"
            :                                                         "glibc";
        nlohmann::json j;
        j["toolchain"] = {
            {"spec", ctx.tc.label()}, {"abi", tcAbi},
            {"triple", ctx.tc.targetTriple}, {"stdlib", ctx.tc.stdlibId},
        };
        nlohmann::json dirs = nlohmann::json::array();
        for (auto& d : ctx.plan.runtimeLibraryDirs) dirs.push_back(d.string());
        nlohmann::json caps = nlohmann::json::array();
        for (auto& [cap, prov] : ctx.plan.runtimeProviders)
            caps.push_back({{"capability", cap}, {"provider", prov}});
        j["runtime"] = {
            {"library_dirs", dirs},
            {"dlopen_libs", ctx.plan.runtimeDlopenLibs},
            {"capabilities", caps},
        };
        std::error_code ec;
        std::filesystem::create_directories(ctx.plan.outputDir, ec);
        if (std::ofstream js(ctx.plan.outputDir / "resolution.json"); js)
            js << j.dump(2) << "\n";
    }

    return ctx;
}


} // namespace mcpp::build
