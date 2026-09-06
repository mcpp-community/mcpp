// mcpp.build.prepare — BuildContext + prepare_build: the build-orchestration
// core (workspace -> toolchain -> dependency resolution -> features ->
// modgraph -> fingerprint -> plan -> lockfile).
// Bodies moved verbatim from the CLI layer. Zero behavior change.

module;
#include <cstdio>
#include <cstdlib>

export module mcpp.build.prepare;

// The cfg() predicate evaluator and the fingerprint canonicalisers moved out —
// see mcpp.build.prepare_inputs. Re-exported so every existing caller of
// `target_dir` / `canonical_compile_flags` keeps working: a split whose only
// visible effect is that other files stop compiling is not an improvement.
export import mcpp.build.prepare_inputs;

import std;
import mcpp.targetside;
import mcpp.diag;
import mcpp.build.refusal;
import mcpp.build.version_floor;
import mcpp.home;
import mcpp.platform.axis;
import mcpp.libs.json;
import mcpp.log;
import mcpp.manifest;
import mcpp.source_kind;
import mcpp.modgraph.glob;
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
import mcpp.toolchain.linkmodel;
import mcpp.toolchain.gcc;
// For `resolve_version_match` / `list_installed_versions`: a bare compiler
// family named by the dependency graph resolves to a concrete version through
// exactly the path `mcpp toolchain default <family>` uses.
import mcpp.toolchain.lifecycle;
import mcpp.toolchain.stdmod;
import mcpp.freestanding.target;   // the target sysroot layout (libdir)
import mcpp.freestanding.linkline; // the ISA profile, for the std module command
import mcpp.toolchain.post_install;
import mcpp.toolchain.abi;
import mcpp.toolchain.triple;
import mcpp.build.linkage_form;   // #519 — which form each dependency takes
import mcpp.build.plan;
import mcpp.build.schedule.policy;
import mcpp.build.flags;          // compute_flags — the per-role contracts (#418)
import mcpp.build.distribution;   // dist::Role / dist::Contract to_string
import mcpp.platform.capacity;   // the host fallback handed to schedule::decide
import mcpp.build.graph_shape;  // #407: the graph says which mode wrote it
import mcpp.build.runtime_validation;  // declared artifact -> identity verdict
import mcpp.build.cache_key;
import mcpp.pack.abi_tag;      // the tag a prebuilt dependency is checked against
import mcpp.pack.prebuilt;     // …and the check itself
import mcpp.build.build_program;
import mcpp.build.directives;   // directive table: mark / fold_private_tail
import mcpp.build.tool_store;   // #355 host tools: store layout + key + overrides
import mcpp.build.dep_graph;    // queries over the resolved edge graph
import mcpp.build.provisions;   // #359 build-time provisions: table + propagation
import mcpp.build.resources;    // #365 Windows resources: synthesise / scan / find rc
import mcpp.build.backend;      // BuildOptions for the tool sub-build
import mcpp.build.ninja;        // make_ninja_backend — driving that sub-build
import mcpp.lockfile;
import mcpp.config;
import mcpp.xlings;
import mcpp.xlings.subos_info;
import mcpp.xlings.runtime_selection;
import mcpp.runtime.binding;
import mcpp.platform.runtime_search;
import mcpp.toolchain.post_install;
import mcpp.platform;
import mcpp.fetcher;
import mcpp.fetcher.progress;
import mcpp.pm.resolver;
import mcpp.pm.index_spec;
import mcpp.pm.index_contract;
import mcpp.pm.index_route;
import mcpp.pm.index_refresh;
import mcpp.pm.mangle;
import mcpp.pm.compat;
import mcpp.pm.dep_spec;
import mcpp.pm.dependency_selector;
import mcpp.pm.lock_io;
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
    // A LAYER NAME THIS ENGINE DOES NOT KNOW IS A VERSION GAP, NOT A TYPO,
    // WHEN IT ARRIVES FROM A DEPENDENCY.
    //
    // The reserved `mcpp:` prefix is a closed set so a misspelling cannot
    // silently disable a behaviour. Refusing a DEPENDENCY's manifest for it made
    // the set closed in a second sense nobody intended: a published package
    // could never declare a layer named after the reader was released.
    // Ignoring the layer and saying so is what this engine already does for
    // every other unknown key, and it is the only response that lets the
    // vocabulary grow.
    for (auto const& cap : dm.unknownCapabilities) {
        auto why = mcpp::targetside::parse_capability(cap);
        mcpp::ui::warning(std::format(
            "dependency '{}': {}\n"
            "       Ignored, and this build proceeds without that layer. "
            "A newer mcpp may resolve it.",
            depLabel,
            why ? std::format("`{}` names no capability mcpp knows.", cap)
                : why.error()));
    }
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
// The dependency MAPS ride the same funnel (#359). They used to be merged by
// a hand-written loop at the root call site only, with a comment declaring a
// dependency's own conditional deps "out of scope". That was the #229 shape
// one level up: three call sites merged build inputs, ONE of them also merged
// deps, and nothing said why. A package's `[target.windows.dependencies]` is
// its own statement about itself and means the same thing whether the package
// is the root or someone's dependency.
// The resolved triple travels INSIDE `ctx` (cfgpred::Ctx::triple). It used to
// be a third parameter here too, which is how a bare-triple predicate came to
// disagree with a cfg() one about the same native build — see the note on Ctx.
void merge_conditional_config(mcpp::manifest::Manifest& m,
                             const cfgpred::Ctx& ctx)
{
    // A DISTRIBUTION package may carry a leg's link line twice: as `ldflags`
    // (GNU spelling, which is all an older mcpp reads) and as the neutral
    // `[target.<pred>.runtime]` pair, which mcpp renders per dialect. Applying
    // both would put `-L` on a native `cl.exe` command line, which is exactly
    // what the neutral form exists to avoid — so where the neutral form is
    // present it REPLACES the ldflags rather than adding to them.
    //
    // Scoped to distribution packages on purpose: a hand-written manifest that
    // states both may well mean both (`ldflags` also carries things like
    // `-Wl,--as-needed`), and silently dropping half of it would be its own
    // silent failure.
    const bool generatedPackage = mcpp::pack::is_distribution_package(m);

    for (auto const& cc : m.conditionalConfigs) {
        // THE TWO PASSES MUST BE DISJOINT, AND `matches()` ALONE DOES NOT
        // MAKE THEM SO. A layer key answers false here because `layersKnown` is
        // false — but `cfg(any(linux, c-abi = "musl"))` still matches on its
        // triple leg, and the second pass would match it again and `append()`
        // the same inputs twice. Membership, not the answer, decides ownership:
        // a predicate that NAMES a layer belongs to the second pass entirely.
        if (cfgpred::uses_layer(cc.predicate)) continue;
        if (!cfgpred::matches(cc.predicate, ctx)) continue;
        const bool neutralWins = generatedPackage
                              && (!cc.linkLibraryDirs.empty() || !cc.libraries.empty());
        // One append() for every field the axis may carry (#258). Matching
        // sections land AFTER the base entries, so a conditional rule beats
        // a broader unconditional one under GNU last-wins — which is what
        // makes an off-OS REMOVAL expressible (`-U` after the base `-D`).
        if (neutralWins) {
            // Drop the LIBRARY REFERENCES, not the whole ldflags list.
            //
            // Clearing it outright was a measured regression: a PE/MinGW shared
            // leg's ldflags also carry `-Wl,-Bdynamic`, without which `-static`
            // leaves ld in static-only mode and it refuses the import library
            // with `have you installed the static version of the mathkit
            // library?`. e2e 257 caught it.
            //
            // The neutral form replaces exactly what it can express — a library
            // and where to find it. Anything else in that block says something
            // it cannot say, and must survive.
            auto inputs = cc.inputs;
            std::erase_if(inputs.ldflags, [](std::string_view f) {
                return f.starts_with("-L") || f.starts_with("-l")
                    || f.starts_with("/LIBPATH:");
            });
            mcpp::manifest::append(m.buildConfig, inputs);
        } else {
            mcpp::manifest::append(m.buildConfig, cc.inputs);
        }
        // The neutral half goes where `render_link_intent_flags` will find it.
        for (auto const& d : cc.linkLibraryDirs)
            m.runtimeConfig.linkIntent.linkLibraryDirs.push_back(d);
        for (auto const& l : cc.libraries)
            m.runtimeConfig.linkIntent.libraries.push_back(l);
        // `modules.sources` is the scanner's own view and is not part of
        // BuildInputs, so conditional sources are mirrored into it here.
        for (auto const& s : cc.inputs.sources)
            m.modules.sources.push_back(s);
        // insert() keeps an existing unconditional entry: a conditional
        // section adds a dependency, it never silently overrides one.
        m.dependencies.insert(cc.dependencies.begin(), cc.dependencies.end());
        m.devDependencies.insert(cc.devDependencies.begin(), cc.devDependencies.end());
        m.buildDependencies.insert(cc.buildDependencies.begin(),
                                   cc.buildDependencies.end());
        // #359: `[target.<sel>.feature-deps.<feature>]`. The feature is
        // registered by the parser regardless of the predicate; only what it
        // pulls in is conditional.
        for (auto const& [fname, deps] : cc.featureDeps) {
            auto& dst = m.featureDeps[fname];
            dst.insert(deps.begin(), deps.end());
        }
    }
}

// Desugar `[build].defines` into `-D<x>` on both C and C++ flag channels.
//
// ORDER (both halves are load-bearing): this must run AFTER
// merge_conditional_config — `defines` is a BuildInputs member, so a
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

// ── The SECOND conditional pass: predicates that name a target-side layer ────
//
// #540/#494. `docs/14` documents a package adapting to the C library it was
// built over — `[target.'cfg(c-abi = "musl")'.build] std-module-flags =
// ["-D_GNU_SOURCE"]`, wrong for picolibc — and `stdModuleFlags` was moved onto
// BuildInputs FOR this, its member comment saying membership "is what makes the
// cfg axis carry it". Nothing evaluated the predicate: `cfgpred::Ctx` was built
// from the triple alone, so every such section was dropped in silence and the
// package built with the wrong C-library configuration, successfully.
//
// WHY A SECOND PASS AND NOT AN EARLIER CONTEXT. A layer is answerable only
// after dependency resolution — a package in the graph may supply the C library
// (openkal-musl under a `-gnu` triple), which is exactly why the triple's `env`
// segment is a REQUEST and not the answer (docs/spec/target-side.md §3.4). The
// first merge runs before resolution because conditional DEPENDENCIES have to.
//
// WHERE IT RUNS. Between `tsd::resolve` and the P1689 scan — the same window in
// which build.mcpp already contributes build inputs by mirroring into
// `packages[0]`. Everything downstream reads the snapshot from there on: the
// scan, `stdModuleFlags` collection, the fingerprint, and `compute_flags`.
//
// SCOPE. Build INPUTS only, which is what docs/14 promises ("available in
// [build] sections only"). Dependencies are excluded by construction — they are
// already resolved by now — and a section that tries is reported rather than
// silently ignored; see `warn_layer_predicate_dependencies`.
bool merge_layer_conditional_config(mcpp::manifest::Manifest& m,
                                    const cfgpred::Ctx& ctx)
{
    bool any = false;
    for (auto const& cc : m.conditionalConfigs) {
        if (!cfgpred::uses_layer(cc.predicate)) continue;
        if (!cfgpred::matches(cc.predicate, ctx)) continue;
        any = true;
        mcpp::manifest::append(m.buildConfig, cc.inputs);
        // Same mirror the first pass does: `modules.sources` is the scanner's
        // own view and is not a BuildInputs member.
        for (auto const& s : cc.inputs.sources)
            m.modules.sources.push_back(s);
        for (auto const& d : cc.linkLibraryDirs)
            m.runtimeConfig.linkIntent.linkLibraryDirs.push_back(d);
        for (auto const& l : cc.libraries)
            m.runtimeConfig.linkIntent.libraries.push_back(l);
    }
    // Re-fold only when something was added. The call is safe either way — the
    // function clears `defines` after folding and says so — but skipping it
    // keeps this pass a no-op for the overwhelming majority of manifests, which
    // name no layer at all.
    if (any) fold_build_defines_into_flags(m.buildConfig);
    return any;
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

// How this invocation may use the global dependency cache.
//
//   Global  read + write  (default)
//   Local   neither — every dependency is compiled inside this project's
//           target/, which is what every build did before the cache worked
//   Off     neither, and this build's target/<triple>/<fp>/ directory is
//           cleared first (a full cold rebuild). Sibling build dirs — other
//           profiles, other targets — are left alone.
//
// `--no-cache` used to be the only switch and it meant "clear the build dir",
// which says nothing about a cache (and its help text claimed all of target/);
// it stays as a deprecated alias for Off.
// Where the resolved toolchain spec came from.
//
// This exists so mcpp can tell its own guesses apart from the user's
// instructions. When a resolved toolchain turns out to be unusable on this
// machine (the motivating case: a Windows default that targets the MSVC ABI
// on a box with no Visual Studio), mcpp may quietly revise a default it
// picked itself — but a spec the user wrote into mcpp.toml must produce an
// error instead. A project that needs the MSVC ABI to link vcpkg-built .lib
// files is worse off with a silent ABI swap than with a failed build.
//
// Deliberately derived from the two config layers that already exist rather
// than persisted: no new field, nothing to keep in sync on disk.
export enum class TcOrigin {
    None,               // nothing resolved yet
    ManifestToolchain,  // mcpp.toml [toolchain]           — user explicit
    TargetSection,      // mcpp.toml [target.X].toolchain  — user explicit
    GlobalDefault,      // `mcpp toolchain default`        — user explicit
    TargetPin,          // triple.cppm vocabulary convention
    GraphRequirement,   // `requires = ["mcpp:compiler=…"]` in the graph
    FirstRun,           // chosen and persisted by this very invocation
};

// `GlobalDefault` IS DELIBERATELY NOT LISTED, AND THE REASON IS A MEASURED
// REGRESSION RATHER THAN A JUDGEMENT ABOUT WHOSE OPINION COUNTS.
//
// A target row's pin does not name a preferred compiler. It names the payload
// that supplies THAT TARGET'S C library — the mingw payload for
// `x86_64-windows-gnu`, the musl-gcc payload for `*-linux-musl`. Whether the
// user's own default can serve the target instead depends on whether something
// ELSE supplies the target side, and that is knowable only after the dependency
// graph is resolved, which is after this line.
//
// Making the global default outrank the pin was tried and measured: a project
// with no dependencies, a global default of `llvm@22.1.8` and
// `--target x86_64-windows-gnu` stopped building, because clang alone carries no
// C runtime for that target while the payload the row names does. That is a
// working build turned into a failing one by an upgrade.
//
// What the user actually loses is ergonomics, and that is addressed where it is
// visible: when the pin replaces a default the user wrote down, the status line
// SAYS SO and names the one-line override. The structural fix is to defer the
// pin the way the target side itself was deferred — resolve it after the graph,
// where the question it answers has an answer.
export inline bool tc_origin_is_user_explicit(TcOrigin o) {
    return o == TcOrigin::ManifestToolchain || o == TcOrigin::TargetSection;
}

// MAY A BUILD THAT RESOLVED THIS WAY WRITE THE MACHINE'S DEFAULT?
//
// `GraphRequirement` is the one origin that must not: it is a property of a
// package this project depends on, not of this machine. Two branches persist a
// default — the Windows first-run diversion, whose condition is
// `tcSpec.has_value()`, and the MSVC repair, whose gate is "mcpp chose this
// itself" — and a compiler chosen by `requires = ["mcpp:compiler=…"]` satisfies
// both. Measured against the design rather than a run, because it needs a
// Windows box with no toolchain: a bare machine building ONE llvm-requiring
// project would have handed llvm to every later project that asked for nothing.
//
// NAMED RATHER THAN SPELLED INLINE AT EACH SITE. There are two today; the
// third would be written by someone who never read this note, and a predicate
// with a name is something they can find.
export inline bool tc_origin_may_persist(TcOrigin o) {
    return o != TcOrigin::GraphRequirement;
}

// How a resolution came about, for the status line. A convention that replaced
// nothing needs no explanation; one that replaced a user's stated preference is
// a decision the user did not make and must be told about.
export constexpr std::string_view tc_origin_name(TcOrigin o) {
    switch (o) {
        case TcOrigin::ManifestToolchain: return "[toolchain] in mcpp.toml";
        case TcOrigin::TargetSection:     return "[target.<triple>] in mcpp.toml";
        case TcOrigin::GlobalDefault:     return "your default";
        case TcOrigin::TargetPin:         return "target default";
        case TcOrigin::GraphRequirement:  return "required by the dependency graph";
        case TcOrigin::FirstRun:          return "first-run default";
        case TcOrigin::None:              break;
    }
    return {};
}

// What to tell a user whose build targets the MSVC ABI on a machine that
// cannot serve it. Two shapes, because the two states need different fixes:
//
//   • cl.exe was found but the Windows SDK was not — a half-installed VS.
//     Point at the missing SDK component; switching toolchains would be an
//     over-correction for someone who clearly wants MSVC.
//   • nothing usable at all — the bare-Windows case. Lead with the MinGW-w64
//     route, which needs no Visual Studio and is already a verified target,
//     and keep the "install the C++ workload" option second.
export std::string msvc_unavailable_guidance(const mcpp::toolchain::Toolchain& tc) {
    namespace pins = mcpp::toolchain::triple::pins;
    const bool haveVcTools = tc.compiler == mcpp::toolchain::CompilerId::MSVC;
    if (haveVcTools && mcpp::toolchain::msvc::find_msvc_tools_dir()) {
        return std::format(
            "msvc {} was detected at {}, but no Windows SDK was found —\n"
            "       cl.exe cannot compile without the UCRT/SDK headers.\n"
            "       Install the 'Windows 11 SDK' component via the Visual Studio\n"
            "       Installer (it is part of the Desktop development with C++\n"
            "       workload), then retry.",
            tc.version, tc.binaryPath.string());
    }
    return std::format(
        "this build targets the MSVC ABI, which needs Visual Studio /\n"
        "       Build Tools (MSVC STL + Windows SDK) — neither was found.\n"
        "\n"
        "       No Visual Studio? Use the self-contained MinGW-w64 toolchain\n"
        "       (no Visual Studio required, `import std` works):\n"
        "         mcpp toolchain default {} --target {}\n"
        "\n"
        "       Have Visual Studio? Install the 'Desktop development with C++'\n"
        "       workload — it provides the MSVC STL and the Windows SDK.",
        pins::kSuggestGccMingw, pins::kFirstRunWinGnuTarget);
}

export enum class CacheMode { Global, Local, Off };

export std::optional<CacheMode> parse_cache_mode(std::string_view v) {
    if (v == "global") return CacheMode::Global;
    if (v == "local")  return CacheMode::Local;
    if (v == "off" || v == "none") return CacheMode::Off;
    return std::nullopt;
}

export std::string_view cache_mode_name(CacheMode m) {
    switch (m) {
        case CacheMode::Local: return "local";
        case CacheMode::Off:   return "off";
        default:               return "global";
    }
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
    mcpp::xlings::runtime::RuntimeSelection runtimeSelection;
    mcpp::platform::runtime::RuntimeBinding runtimeBinding;
    std::filesystem::path           projectRoot;
    // THE SOURCE TREES THIS BUILD READ THAT ARE NOT UNDER `projectRoot`.
    //
    // A `path` dependency — which is what every workspace member is to its
    // siblings — contributes translation units from a directory the fast path
    // has no way to name. `sources_newer_than` sweeps the project being built,
    // so a NEW FILE appearing in such a tree is invisible to it: ninja cannot
    // report an edge that was never emitted, and the fast path replays a
    // build.ninja that predates the file. Measured before this field existed:
    // `mcpp build` printed `Finished dev in 0.00s` and the module was never
    // compiled.
    //
    // Recorded rather than re-derived, because the authoritative answer is
    // which packages this build ACTUALLY read from source — the fast path
    // cannot resolve dependencies without becoming prepare_build, and a second
    // derivation would drift from the first exactly when a resolution rule
    // changes. Written into `.build_cache`; see BuildCacheEntry::depSourceRoots.
    std::vector<std::filesystem::path> depSourceRoots;
    // `<payload>/bin` of every installed `[xlings] deps` payload of the
    // runtime-owner manifest, in declaration order (#544). Read by
    // choose_runner's lookup (mcpp.build.runner_lookup) so a runner may name
    // a program the project declared without writing the payload's
    // home-and-version path into the manifest. Computed by the same
    // resolution `fillXpkgDirs` uses for build programs; a payload that is
    // declared but not installed contributes nothing, and the lookup then
    // continues to PATH.
    std::vector<std::filesystem::path> xlingsDepBinDirs;
    // True when the graph declared a `when = "run"` tool that THIS invocation
    // did not provision, because it was not going to execute anything. The
    // build cache records it so `mcpp run`'s fast path declines an entry a
    // plain `mcpp build` wrote — see BuildCacheEntry::runTierPending.
    bool runTierPending = false;
    // What `--features` asked for, verbatim. Carried so the build cache entry
    // can record the set its artefacts were built with — the output directory
    // is keyed on a fingerprint that includes the features and the entry was
    // not, which let a plain build serve a featured artefact.
    std::string activeFeatureRequest;
    std::filesystem::path           outputDir;
    std::filesystem::path           stdBmi;
    std::filesystem::path           stdObject;
    mcpp::build::BuildPlan          plan;
    // The scanned module graph. Only `mcpp pack` reads it — see the note at
    // the assignment for why the plan cannot answer its question.
    mcpp::modgraph::Graph           graph;
    // Resolved profile name (resolve_profile_name). Carried so run_build_plan
    // can record it in .build_cache — without it the fast path cannot tell
    // whether a cached build.ninja was generated for the profile being asked
    // for — and so `Finished <profile>` stops being a hardcoded "release".
    std::string                     profile;
    // WHY THIS COMPILER — carried so the QUERY can answer it too.
    //
    // A build says so on its status line. `mcpp why toolchain --format json`
    // exists precisely to answer "what would this resolve to, and why", and a
    // consumer that had to parse the prose to learn that a dependency chose the
    // compiler would be doing the substring matching the machine interface was
    // introduced to remove.
    struct CompilerChoice {
        std::string origin;      // tc_origin_name(): who decided
        std::string requiredBy;  // the package, when the graph decided
        std::string replaced;    // the spec displaced, when one was
    };
    CompilerChoice                  compilerChoice;
    // Resolved global-cache mode. Read side is honored in prepare_build; write
    // side in run_build_plan.
    CacheMode                       cacheMode = CacheMode::Global;

    // M3.2 BMI cache: deps that did NOT hit cache and therefore need
    // populate_from(...) AFTER backend.build succeeds.
    struct CacheTask {
        mcpp::bmi_cache::CacheKey       key;
        mcpp::bmi_cache::DepArtifacts   artifacts;
    };
    std::vector<CacheTask>          depsToPopulate;

    // Deps that DID hit the global cache, and how many compile units each one
    // spared. run_build_plan reports the count so the "Cached" line cannot be
    // true-looking and empty at the same time.
    struct CachedDep {
        std::string name;
        std::string version;
        std::size_t units = 0;
    };
    std::vector<CachedDep>          cachedDeps;

    // What the dependency walk actually RESOLVED, keyed by the root manifest's
    // dependency map key. The "Compiling <dep> v<version>" banner used to read
    // `manifest.dependencies[...].version` — the constraint as authored — so a
    // caret dep announced itself as `v^1.92.8` (mcpp#363). The resolution result
    // already existed inside prepare_build; the banner and mcpp.lock were simply
    // reading the input instead of the output. Both now read this.
    std::map<std::string, std::string> resolvedVersions;
};

// The ONE cache-mode resolver, for the same reason resolve_profile_name exists:
// execute.cppm's fast paths deliberately skip prepare_build, so they need to
// settle the mode from the same rule. Pure in (manifest, override, environment).
//
// `--cache` on the command line already bypasses the fast path, so the override
// argument is empty there; it is threaded anyway so there is exactly one place
// where precedence is written down.
//
// Precedence: --cache > MCPP_BUILD_CACHE > [build] cache > global. An
// unparseable value falls through to the next source rather than silently
// meaning "global" — see prepare_build, which also reports it.
export CacheMode resolve_cache_mode(const mcpp::manifest::Manifest& m,
                                    std::string_view override_mode) {
    if (auto v = parse_cache_mode(override_mode)) return *v;
    if (const char* e = std::getenv("MCPP_BUILD_CACHE"); e && *e)
        if (auto v = parse_cache_mode(e)) return *v;
    if (auto v = parse_cache_mode(m.buildConfig.cacheMode)) return *v;
    return CacheMode::Global;
}

// The ONE profile-name resolver. Shared with execute.cppm's fast paths:
// they deliberately skip prepare_build, so before this existed they had no
// idea which profile the request meant — and `.build_cache` keyed entries by
// target triple alone. Net effect: `mcpp build --release` followed by a bare
// `mcpp build` reported success in 0.00s and left the RELEASE artifacts in
// place. The rule is pure (manifest + one override string), so both sides can
// evaluate it without resolving a toolchain or scanning the module graph.
//
// Precedence: --profile/--release/--dev > [build].default-profile > `fallback`.
// The global default is "dev" (-O0 -g) per the dominant convention
// (Cargo/Meson/CMake/Zig/Bazel/MSBuild all default to debug).
//
// `fallback` exists for ONE caller: `mcpp pack`, where the artifact leaves this
// machine and an unoptimized build with the publisher's absolute source paths
// in it is never what was meant. It changes the LAST step only, so a manifest
// that states `[build] default-profile` still decides — packaging an artifact
// with different flags than `mcpp build` produces would be its own surprise.
// Adding a parameter here rather than a second resolver keeps the precedence
// rule in one function, which is why this function exists at all.
export std::string resolve_profile_name(const mcpp::manifest::Manifest& m,
                                        std::string_view override_name,
                                        std::string_view fallback = "dev") {
    if (!override_name.empty())                 return std::string(override_name);
    if (!m.buildConfig.defaultProfile.empty())  return m.buildConfig.defaultProfile;
    return fallback.empty() ? std::string("dev") : std::string(fallback);
}

// Command-level overrides (--target / --static).
// Empty defaults preserve pre-existing behaviour exactly.
export struct BuildOverrides {
    // Where the package being built LIVES (its mcpp.toml). Empty = walk up from
    // the process cwd, which is what every user-facing invocation does. Set by
    // the tool-provisioning pass, which builds a package that lives in the
    // registry rather than under the cwd.
    std::filesystem::path project_root;
    // Where mcpp WRITES. Empty = the project root, which is the historical
    // (and for a normal build, correct) behaviour.
    //
    // The two are separate because a registry package root is shared across
    // projects and may be read-only — build_program.cppm has said so in a
    // comment since G2, and until now nothing could honour it for anything
    // bigger than build.mcpp's own scratch dir. Splitting "source" from "work"
    // is what lets mcpp build such a package at all.
    //
    // EVERYTHING derived from it moves together: target/, mcpp.lock,
    // compile_commands.json, .mcpp/, and build.mcpp's artifact dir. Moving
    // only some would be worse than moving none — a half-redirected build
    // writes into the shared root anyway, just less visibly.
    std::filesystem::path work_dir;
    // #355 tool provisioning re-enters prepare_build for the tool package. A
    // tool package's own build.mcpp may legitimately want another tool (gRPC's
    // wants protoc), so the depth cannot be 1 — but an unbounded chain is a
    // bug, and hanging is a worse diagnostic than a named cycle.
    int         tool_depth = 0;
    // The request chain, for that diagnostic. "root → grpc:grpc_cpp_plugin → …"
    std::string tool_chain;
    // Use THIS manifest instead of reading `<project_root>/mcpp.toml`.
    //
    // Required for a `compat`-style registry package (Form B), which ships no
    // mcpp.toml at all — its manifest is synthesized from the `.lua`
    // descriptor during resolution. Without this the tool sub-build could only
    // ever handle packages that carry their own manifest (Form A), which
    // excludes most of the index, protobuf among them.
    //
    // Must be the PRISTINE manifest, before feature activation: the sub-build
    // activates its own feature set, and starting from an already-activated
    // copy would fold the same feature sources in twice.
    // A shared_ptr rather than an optional<Manifest>: BuildOverrides is an
    // EXPORTED struct, and embedding a large value type in the module
    // interface made GCC fail to write the cluster at all
    // ('failed to read compiled module cluster ...: Bad file data' when
    // mcpp.build.execute imported it). A pointer keeps the exported layout
    // trivial, and it also avoids copying the manifest per tool build.
    std::shared_ptr<const mcpp::manifest::Manifest> preloaded_manifest;
    // Nested source/tool builds inherit the consumer root's local development
    // OS. A dependency's own [xlings].subos is never consulted or propagated.
    std::shared_ptr<const mcpp::xlings::runtime::RuntimeSelection>
        inherited_runtime_selection;
    std::shared_ptr<const mcpp::platform::runtime::RuntimeBinding>
        inherited_runtime_binding;
    std::string target_triple;       // empty = host triple, fall through to [toolchain]
    // --accel: the device backends and architectures this build targets, in
    // the wire form mcpp.pack.abi_tag reads. Overrides `[build] accel`, the
    // same relationship --target has with [toolchain].
    std::string accel;
    bool        force_static = false; // --static (or implied by musl target)
    std::string package_filter;      // -p <name>: only build this workspace member
    // --profile <name>. Empty = fall through to `[build] default-profile`, then
    // to `profile_fallback` below, whose own default is "dev". The comment here
    // said "release" for as long as `mcpp build --help` did, and neither had
    // been true since the global default moved (see resolve_profile_name and
    // tests/e2e/87_build_default_profile.sh).
    std::string profile;
    // What `resolve_profile_name` falls back to when neither the command line
    // nor `[build] default-profile` says. Empty = "dev", which is every
    // interactive command. `mcpp pack` sets "release": see resolve_profile_name.
    std::string profile_fallback;
    std::string features;            // --features a,b,c (root package activation)
    bool        strict = false;      // --strict: schema warnings become errors
    std::string capabilities;        // --cap blas=openblas,lapack=mkl (provider pins)
    std::string cache_mode;          // --cache global|local|off ("" = unset)
    // Whether the caller intends to EXECUTE what it builds, which is what
    // decides the `when = "run"` tool tier. `mcpp run` and `mcpp test` set it;
    // `mcpp build`, `mcpp pack` and every internal sub-build do not.
    //
    // A SEPARATE FLAG FROM `includeDevDeps`, THOUGH `mcpp test` SETS BOTH.
    // One says which PACKAGES enter the graph, the other which TOOLS are
    // installed, and `mcpp run` needs the second without the first.
    bool        will_run = false;
};

// ── git dependency helpers ──────────────────────────────────────────────────

// Is this git remote reachable without a network round-trip?
//
// `--offline` means "never touch the network" (docs/05-mcpp-toml.md), and its
// standing promise is that anything already on disk still builds. A remote that
// names a local directory — or a file:// URL — is served by plain filesystem
// reads, so refusing it would break that promise without buying any isolation.
// The dependency-download gate further down draws the same line.
//
// Recognising a scheme (`https://`, `ssh://`, `git://`) or scp-like syntax
// (`git@host:path`) as remote first keeps a Windows drive letter (`C:\repo`,
// which contains a colon but no `@`) on the local side.
bool is_local_git_remote(std::string_view url) {
    if (url.starts_with("file://"))                     return true;
    if (url.contains("://"))                            return false;
    if (url.contains('@') && url.contains(':'))         return false;
    std::error_code ec;
    return std::filesystem::exists(std::filesystem::path(url), ec);
}

// The commit a cached clone is actually parked on, or "" if it cannot be read.
//
// Used to detect a clone that was interrupted between `git clone` and
// `git checkout` — the directory exists and looks like a repository, but sits
// on the wrong commit. Only meaningful when the expected revision is a sha,
// i.e. for branch deps after resolution.
//
// stderr is folded in so a git warning cannot leak to the user's terminal;
// the last line is taken so such a warning cannot corrupt the sha either.
std::string git_cache_head(const std::filesystem::path& gitRoot) {
    auto r = mcpp::platform::process::capture(std::format(
        "git -C {} rev-parse HEAD 2>&1",
        mcpp::platform::shell::quote(gitRoot.string())));
    if (r.exit_code != 0) return {};
    std::string out = r.output;
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r'
                            || out.back() == ' '  || out.back() == '\t'))
        out.pop_back();
    if (auto nl = out.find_last_of("\r\n"); nl != std::string::npos)
        out.erase(0, nl + 1);
    return out;
}

// `prepare_build` builds the BuildContext for any verb that compiles.
//   includeDevDeps: when true, dev-dependencies are also fetched + scanned
//                   into the modgraph. mcpp test passes true; build/run pass false.
//   extraTargets:   additional Target entries (e.g. synthetic test targets)
//                   appended to the manifest before the modgraph runs.
//   overrides:      --target / --static.
namespace {
// A dependency that "cannot be found" while an index is unreadable is almost
// never missing — it is unreachable, and the two need different actions from
// the user (publish it vs upgrade mcpp). The floor error is printed when the
// index is first opened, which can be hundreds of lines earlier; the message
// that STOPS the build has to carry the cause, because that is the one a user
// reads. See mcpp::pm::unusable_index_hint.
// Spelling-independent `[target.<triple>]` lookup.
//
// A section keyed `x86_64-w64-mingw32` matches a resolved `x86_64-windows-gnu`,
// and unparseable keys compare exactly (the escape hatch for custom triples).
// Factored out of the toolchain-override path because the sysroot override must
// use the SAME matching: two lookups that disagreed about spelling would give a
// section that applies to `toolchain` and not to `sysroot`, which is a defect
// nobody would think to look for.
const mcpp::manifest::TargetEntry*
find_target_entry(const mcpp::manifest::Manifest& m,
                  const mcpp::toolchain::triple::Triple& t)
{
    if (auto it = m.targetOverrides.find(t.str()); it != m.targetOverrides.end())
        return &it->second;
    for (auto const& [key, entry] : m.targetOverrides) {
        if (auto k = mcpp::toolchain::triple::parse(key); k && k->str() == t.str())
            return &entry;
    }
    return nullptr;
}

// The project's `[target.<triple>].sysroot`, or nullptr when it declared none.
const std::string*
sysroot_override(const mcpp::manifest::Manifest& m,
                 const mcpp::toolchain::triple::Triple& t)
{
    auto* e = find_target_entry(m, t);
    return (e && e->sysrootDeclared) ? &e->sysroot : nullptr;
}

// The target-facing answers a `build.mcpp` may ask the engine for.
//
// ONE function because there are TWO call sites — the root project and each
// dependency — and four values derived independently in two places is the
// shape this codebase keeps paying for. A board package that got the right
// answer as a root project and a stale one as a dependency would fail only in
// the consuming build, which is the harder direction to debug.
// A NETWORK STEP OF A BUILD, RETRIED — AND IT HAD NO RETRY AT ALL.
//
// A dependency resolved by `git` is fetched on every machine that has not
// cached it, and a transport that hiccups once failed the whole build:
//
//     error: git clone of 'https://github.com/…' failed:
//     Cloning into '/home/runner/.mcpp/git/63269d80b47f71e6'...
//
// — no message from git, which is what a connection that dies mid-transfer
// looks like. Measured twice on 2026-08-23: once in continuous integration and
// once locally as `TLS connect error: … unexpected eof while reading`.
//
// THREE ATTEMPTS, AND THE LAST FAILURE IS REPORTED UNCHANGED. A wrong URL
// and a missing branch fail exactly as a transient fault does, so this cannot
// tell them apart and does not try: a permanent failure costs three seconds and
// produces the message it always did. Hiding a real error behind a retry is the
// worse trade, which is why the count is small and the report is untouched.
//
// BOTH NETWORK STEPS, not one. The first version retried only the clone —
// and a probe with a nonexistent repository failed in ONE second, because the
// step that runs first is `git ls-remote` and it was still bare. A retry on
// half of a path is a retry that reports success at having been added.
//
// `between` runs after a failed attempt: the clone needs the partial directory
// removed, or git's next attempt fails with "already exists and is not an empty
// directory" — a second, different error that says nothing about the first.
mcpp::platform::process::RunResult run_with_network_retry(
        std::string_view command,
        const std::function<void()>& between = {}) {
    mcpp::platform::process::RunResult r{};
    for (int attempt = 1; attempt <= 3; ++attempt) {
        r = mcpp::platform::process::capture(command);
        if (r.exit_code == 0) return r;
        if (between) between();
        if (attempt < 3)
            std::this_thread::sleep_for(std::chrono::seconds(attempt));
    }
    return r;
}

void fill_target_build_env(mcpp::build::BuildProgramEnv& e,
                           const mcpp::toolchain::Toolchain* tc)
{
    e.toolchainDir  = (tc && !tc->binaryPath.empty())
        ? tc->binaryPath.parent_path().parent_path().string() : std::string{};
    e.targetSysroot = tc ? tc->targetSysrootRoot.string() : std::string{};
    e.compilerId    = !tc ? std::string{}
        : tc->compiler == mcpp::toolchain::CompilerId::GCC   ? "gcc"
        : tc->compiler == mcpp::toolchain::CompilerId::Clang ? "clang"
        : tc->compiler == mcpp::toolchain::CompilerId::MSVC  ? "msvc"
        : std::string{};
    e.targetLibc    = tc ? tc->targetSysrootPkg : std::string{};
    if (!tc) return;

    // The two flags mcpp passes to ITS OWN compiler, so a rule package driving
    // a second compiler passes the same two. Both read from the single
    // producer that already decides them for the engine's own command lines —
    // `resolve_link_model` for the sysroot, `gcc::binutils_prefix_dir` for the
    // `-B` — rather than a fifth re-derivation of either.
    if (auto lm = mcpp::toolchain::resolve_link_model(*tc);
        lm.mode == mcpp::toolchain::CLibMode::Sysroot)
        e.toolchainSysroot = lm.sysroot.string();
    e.toolchainBinutilsDir = mcpp::toolchain::gcc::binutils_prefix_dir(*tc).string();

    // The C LIBRARY's sub-directory for this ISA profile, from the freestanding
    // table — the same single read point the compile flags use.
    //
    // Gated on there being a C library at all, and the gate is the point: the
    // value is a multilib convention, so on the zero-libc tier there is nothing
    // for it to be a convention OF. Emitting `rv64gc/lp64d` there would hand a
    // kernel a path into a directory that does not exist, and the name of the
    // accessor would be a lie. All three libc-facing answers are empty together.
    if (!e.targetSysroot.empty())
        if (auto spec = mcpp::freestanding::resolve(tc->targetTriple))
            e.targetLibcProfile = std::string(spec->libdir);

    // Which builtins library the RESOLVED toolchain ships. Freestanding only:
    // on a hosted target the driver links them without being asked, and
    // handing a package a name it must not use would invite it to.
    if (auto t = mcpp::toolchain::triple::parse(tc->targetTriple);
        t && t->is_freestanding()) {
        e.targetBuiltinsLib = mcpp::toolchain::is_clang(*tc)
            ? "clang_rt.builtins-" + t->arch
            : std::string("gcc");
    }
}

// ── Tool tiers: which of a manifest's declared packages this verb needs ─────
//
// THE AXIS PACKAGE DEPENDENCIES HAVE HAD SINCE THE BEGINNING, AND TOOLS
// DID NOT.
//
// A board-support package names an emulator (needed to run) and a debug probe
// (needed to reach real hardware). Before this, declaring either installed
// both, for everyone, on every `mcpp build` — including a consumer who only
// wanted the library to compile. Packages have `[dependencies]`,
// `[build-dependencies]` and `[dev-dependencies]`; tools had one list.
//
// The tier is written on the ENTRY (`when = "run"`), not as a second table:
// `[xlings.workspace]` was made the one table on purpose, and the entry-level
// spelling is the one `[dependencies]` already uses for the same kind of
// refinement.
//
// `Dev` IS THE ONLY TIER THAT DOES NOT PROPAGATE. It means "when the package
// that declared it is itself being developed", so `isRoot` decides it. Every
// other tier reaches a consumer, which is the whole point of a board package
// knowing its own machine.
enum class ToolPurpose { Build, Run };

std::vector<std::string>
applicable_xlings_addresses(const mcpp::manifest::Manifest& man,
                            const std::vector<std::string>& activeFeatures,
                            ToolPurpose purpose, bool isRoot) {
    using W = mcpp::manifest::ToolWhen;
    std::vector<std::string> out;
    auto wanted = [&](const std::string& address) {
        switch (man.xlings.when_of(address)) {
            case W::Always: return true;
            case W::Build:  return true;
            case W::Run:    return purpose == ToolPurpose::Run;
            case W::Dev:    return isRoot;
        }
        return true;
    };
    auto add = [&](const std::string& address) {
        if (!wanted(address)) return;
        if (std::ranges::find(out, address) == out.end()) out.push_back(address);
    };
    for (auto const& a : man.xlings.deps) add(a);
    // `[feature-xlings.<f>]` contributes only while `<f>` is active. A consumer
    // that never asks for `hardware` never downloads a probe driver — which is
    // the same mechanism `[feature-deps]` gives a package, applied to tools.
    for (auto const& f : activeFeatures)
        if (auto it = man.xlings.featureDeps.find(f);
            it != man.xlings.featureDeps.end())
            for (auto const& a : it->second) add(a);
    return out;
}

// Install a set of `[xlings.workspace]` addresses, and record that the list was
// done.
//
// EXTRACTED SO THE DEPENDENCY GRAPH CAN USE THE SAME PATH. This was the
// root project's provisioning, inline and reachable only from there. A
// board-support package that declares the emulator its machine needs is
// precisely the thing that should say so once, and a consumer that has to
// repeat the declaration to get it installed is the duplication such a package
// exists to remove — so the graph pass calls this with what the dependencies
// declared, under the same stamp discipline and the same auto-install gate.
//
// `label` names the caller in every message, because "which of the two passes
// is this" is the first thing a reader of the failure needs.
std::expected<void, std::string>
provision_xlings_addresses(const mcpp::config::GlobalConfig& cfg,
                           const std::vector<std::string>& declaredDeps,
                           const std::filesystem::path& legacyStampRoot,
                           std::string_view label) {
    if (declaredDeps.empty()) return {};
            // THE STAMP RECORDS A GLOBAL EFFECT, SO IT LIVES WHERE THE
            // EFFECT DOES. It used to sit in `<project>/.mcpp/`, while the
            // installation goes to the registry a few lines below — the
            // scope difference is deliberate and explained there. Two
            // consequences followed from the mismatch: wiping or replacing
            // `MCPP_HOME` left a project still claiming the packages were
            // installed, and `mcpp clean` (which removes `target/` and
            // never `.mcpp/`) could not clear it. Keyed by the LIST, not by
            // the project, because the installation is shared: two projects
            // declaring the same packages should pay for it once.
            //
            // This still does not survive a user's `xlings remove`. No
            // stamp does; the honest fix is a presence check, and it is
            // blocked on `resolve_xpkg_path` requiring `<name>@<version>`
            // while a manifest is entitled to name a package unpinned.
            const auto stampDir = mcpp::home::root() / "provisioned";
            // `std::uint64_t`, not `std::size_t`: the offset basis below is
            // a 64-bit constant and a 32-bit host would truncate it, giving
            // that host a different key space for no reason anyone could
            // see. A collision is not a correctness problem either way —
            // the file stores the LIST and the comparison below is against
            // its content, so two lists sharing a key re-provision rather
            // than silently adopt each other's record.
            auto stamp_key = [&] {
                std::uint64_t h = 1469598103934665603ull;   // FNV-1a
                for (auto const& d : declaredDeps)
                    for (unsigned char ch : d + "\n")
                        { h ^= ch; h *= 1099511628211ull; }
                return std::format("xlings-deps-{:016x}", h);
            };
            const auto stamp = stampDir / stamp_key();
            // Idempotence by CONTENT, not by existence: editing the list
            // has to re-provision, and an unchanged list must not pay for
            // an xlings round-trip on every build.
            auto join_deps = [&](std::string_view sep) {
                std::string out;
                for (auto const& d : declaredDeps) {
                    if (!out.empty()) out += sep;
                    out += d;
                }
                return out;
            };
            std::string want;
            for (auto const& d : declaredDeps) { want += d; want += '\n'; }
            std::string have;
            if (std::ifstream in{stamp}; in)
                have.assign(std::istreambuf_iterator<char>(in), {});
            // The stamp the previous location left behind. Read, never
            // deleted: an older mcpp sharing the checkout still uses it,
            // and a stale extra file is cheaper than a downgrade that
            // re-provisions on every build.
            //
            // IT DOES NOT MEAN "PROVISIONED SUCCESSFULLY". The release
            // that wrote it did not read the result — that is the defect
            // above — so it means only "this list was attempted". Treating
            // it as proof would carry the bug across the very upgrade that
            // fixes it: a project whose dependency never installed would
            // adopt the stamp and stay silently broken.
            //
            // So it is consulted in ONE place, below, where the alternative
            // is worse: the auto-install gate. Online, nothing is adopted
            // and every project re-provisions once, which is a cheap round
            // trip that re-validates the claim.
            const auto legacyStamp = legacyStampRoot / ".mcpp" / ".xlings-deps.stamp";
            auto legacy_stamp_matches = [&] {
                std::string legacy;
                if (std::ifstream in{legacyStamp}; in)
                    legacy.assign(std::istreambuf_iterator<char>(in), {});
                return legacy == want;
            };
            bool needProvision = (have != want);
            if (needProvision) {
                // THE AUTO-INSTALL GATE, WHICH THIS PATH DID NOT HAVE.
                //
                // `[toolchain]` is the precedent this whole mechanism cites
                // ("the same 'declare it and mcpp provisions it on first
                // use' contract"), and that path refuses on either knob and
                // names the one that fired — see the auto-install branch
                // above. This one honoured neither, so a CI exporting
                // MCPP_NO_AUTO_INSTALL specifically to prevent an unasked
                // download got one anyway, from a path that had never heard
                // of the variable.
                //
                // Placed inside `have != want`, so it gates the ATTEMPT and
                // not the block: a project whose packages are already
                // provisioned still builds offline, which is the behaviour
                // that would otherwise regress.
                if (mcpp::platform::env::offline_mode()
                    || mcpp::platform::env::no_auto_install()) {
                    // THE ONE PLACE THE LEGACY STAMP IS TRUSTED, and the
                    // reason is that relocating a record must not refuse a
                    // build that worked yesterday. Every project that had
                    // already provisioned carries the old stamp and no new
                    // one, so on the first build after upgrading it reads
                    // as un-provisioned — and here, with the network shut
                    // off, there is no way to find out otherwise. Refusing
                    // would be a regression caused entirely by moving a
                    // file, which is the least defensible kind.
                    //
                    // Proceeding is the pre-upgrade behaviour exactly: if
                    // the packages really are missing, the build fails
                    // downstream on a missing header, as it did before.
                    // The registry stamp is NOT written — nothing here
                    // verified anything.
                    if (!legacy_stamp_matches()) {
                        std::string_view release =
                            mcpp::platform::env::offline_mode()
                            ? "drop --offline / unset MCPP_OFFLINE"
                            : "unset MCPP_NO_AUTO_INSTALL";
                        return std::unexpected(std::format(
                            "{} are declared but not provisioned, "
                            "and auto-install is off.\n"
                            "       declared: {}\n"
                            "       install them yourself with:\n"
                            "         xlings install {}\n"
                            "       or {} to let mcpp do it.",
                            label, join_deps(", "), join_deps(" "), release));
                    }
                    mcpp::log::verbose("xlings",
                        std::format("{}: auto-install is off and this project "
                        "carries a pre-2026.9.1.1 provisioning stamp for the "
                        "same list; proceeding without re-checking", label));
                    // Deliberately NOT writing the registry stamp: nothing
                    // here verified anything, and a record of a check that
                    // did not happen is the defect this release removes.
                    needProvision = false;
                }
            }
            if (needProvision) {
                mcpp::ui::status("Provisioning",
                    std::format("{} ({})", label, join_deps(", ")));
                // GLOBAL scope, and the scope is the whole point.
                //
                // The obvious alternative -- `install_packages` against
                // `make_project_xlings_env` -- installs at PROJECT scope,
                // and that measurably does not work: on a fresh MCPP_HOME
                // the headers land in
                // `<proj>/.mcpp/.xlings/subos/_/usr/include` while
                // `--sysroot` names `<MCPP_HOME>/registry/subos/default`,
                // so `#include <gbm.h>` still failed with the dependency
                // installed and declared. Two SubOS views, and the payload
                // in the one the compiler does not read.
                //
                // `make_xlings_env` is the GLOBAL env, so this lands in the
                // registry whose SubOS *is* mcpp's sysroot -- the same
                // place `[toolchain]` has always installed into. A project
                // dependency and a toolchain dependency now agree on where
                // they live, which is the only arrangement in which one
                // `--sysroot` can see both.
                //
                // `install_packages` rather than `resolve_xpkg_path`: the
                // latter requires `<name>@<version>` and rejects a bare
                // `mesa`, while a manifest is entitled to name a package
                // without pinning it. install_packages resolves the version
                // itself and reports an ambiguous name with its candidates,
                // which is the error the author can act on.
                // Built with the JSON library rather than by formatting
                // the strings in. `deps` is manifest input, so a name
                // containing a quote or a backslash would otherwise emit
                // malformed JSON and the failure would surface as an
                // unrelated xlings parse error naming neither the manifest
                // nor the key.
                nlohmann::json args;
                args["targets"] = declaredDeps;
                args["yes"]     = true;

                mcpp::fetcher::InstallProgressHandler progress;
                auto r = mcpp::xlings::call(
                    mcpp::config::make_xlings_env(cfg), "install_packages",
                    args.dump(), &progress);
                // `if (!r)` IS NOT THE FAILURE TEST, AND TESTING ONLY
                // IT MADE THIS PATH REPORT SUCCESS FOR EVERY FAILURE XLINGS
                // CAN REPORT.
                //
                // `xlings::call` returns `expected<CallResult, string>` and
                // is in the VALUE state whenever the child ran at all — the
                // error channel means "the call did not happen". A
                // capability's own status arrives inside `CallResult`,
                // parsed off the NDJSON `{"kind":"result","exitCode":N}`
                // line, because the xlings process itself exits 0 by design
                // once it has spoken the protocol.
                //
                // Measured before this fix: a manifest declaring a package
                // that cannot exist printed `Provisioning [xlings] deps
                // (…)`, xlings answered `E_NOT_FOUND` with `exitCode: 1`,
                // and mcpp stamped it as done and reported a successful
                // build. #531 was written because "the declaration looked
                // accepted and did nothing" is the worst shape a config key
                // can have; unread, its own fix reproduced that shape and
                // the stamp made it permanent.
                //
                // The correct idiom is not new — the dependency install
                // path in this same file reads `r->exitCode` — it was
                // simply not applied here.
                const bool called   = r.has_value();
                const int  childRc  = called ? r->exitCode : -1;
                if (!called || childRc != 0) {
                    // Prefer xlings' own message: for an unresolvable name
                    // it names the repos it searched and whether the index
                    // is current, which is the part the author can act on.
                    std::string why = !called ? r.error()
                        : (r->error ? r->error->message
                                    : std::format("xlings exited {}", childRc));
                    if (auto captured = progress.captured_error();
                        !captured.empty() && called && !r->error)
                        why = captured;
                    // The hint is where "run `xlings update` if the package
                    // was just published" lives, and for the commonest
                    // failure — a name that is not in the synced index —
                    // it is the whole of the actionable content.
                    if (called && r->error && !r->error->hint.empty())
                        why += "\n       " + r->error->hint;
                    // Shaped like the toolchain failure: say what failed and
                    // hand back a command the user can run themselves. An
                    // ambiguous bare name ("mesa" matching two repos) lands
                    // here, and xlings' own message names the candidates.
                    return std::unexpected(std::format(
                        "provisioning {} failed: {}\n"
                        "       you can install them manually with:\n"
                        "         xlings install {}",
                        label, why, join_deps(" ")));
                }
                // Written only on success, for the same reason the check
                // above exists: a stamp is a record that the effect
                // happened, and recording an effect that did not is worse
                // than not recording it — the next build skips the attempt.
                std::error_code sec;
                std::filesystem::create_directories(stamp.parent_path(), sec);
                if (std::ofstream out{stamp}; out) out << want;
            }
    return {};
}

std::string with_index_cause(std::string msg) {
    if (auto hint = mcpp::pm::unusable_index_hint(); !hint.empty())
        msg += "\n" + hint;
    return msg;
}
} // namespace

export std::expected<BuildContext, std::string>

prepare_build(bool print_fingerprint,
              bool includeDevDeps = false,
              std::vector<mcpp::manifest::Target> extraTargets = {},
              BuildOverrides overrides = {}) {
    // Which tool tiers this invocation needs. Named once so the two
    // provisioning passes cannot disagree — a `mcpp build` that installed the
    // run tier and a `mcpp run` that did not would be the same defect twice.
    const ToolPurpose toolPurpose =
        overrides.will_run ? ToolPurpose::Run : ToolPurpose::Build;

    // A refusal decided early and released late. `host_can_serve` answers
    // "does a payload on this machine produce this target", which is knowable
    // before dependency resolution and is only half the question: a package in
    // the graph can supply the target's system, and the graph is not known
    // here. Held until it is, and released only if nothing supplies it.
    std::string unservedTargetDiagnosis;
    // Non-empty when a target row's convention replaced a toolchain the user
    // had set with `mcpp toolchain default`. Reported on the status line,
    // because a substitution nobody is told about is a rule that can only be
    // learned by experiment — writing the same value a second time in
    // `[target.<triple>]` and observing that it works.
    std::string pinReplacedDefault;
    // THE PACKAGE WHOSE `requires` CHOSE THE COMPILER, AND WHAT IT ASKED FOR.
    //
    // Non-empty only when the graph's requirement actually changed the answer.
    // Reported on the status line for the same reason `pinReplacedDefault` is:
    // a compiler the user did not name is a decision they did not make, and one
    // reported without its reason is a rule learned by experiment.
    std::string graphCompilerRequiredBy;   // "openkal-llvm-runtime@0.1.3"
    std::string graphCompilerFamily;       // "llvm"
    std::string graphCompilerReplaced;     // the spec it displaced, for the line
    // The C library the target triple asked for, taken before the triple is
    // canonicalised. Empty when the project declined to name one.
    std::string requestedCAbi;
    // The target as the project spelled it, when that differs from the
    // canonical identity. Report only; empty means they coincide.
    std::string targetDisplayName;
    // The target row's toolchain convention, held until the graph is known.
    // Empty when the row names none or the project named its own.
    std::string targetPinCandidate;
    // AND WHETHER THAT PIN IS A CONVENTION OR A CAPABILITY, RECORDED AT
    // THE SAME READ.
    //
    // A hosted row's pin answers "which payload supplies this target's C
    // library", so a graph that supplies one instead makes it inapplicable.
    // A freestanding row's pin answers a different question — the table says
    // so in its own words: "the pin is llvm on every host because clang/lld
    // are cross-compilers by construction". A host g++ cannot emit
    // riscv64-none-elf at all, and no dependency changes that.
    //
    // Taken here rather than re-derived at the decision point, because the row
    // is read exactly once and both facts come out of that read.
    bool targetPinIsCapability = false;
    // THE ROW'S PIN, KEPT EVEN WHEN THE PROJECT NAMED ITS OWN COMPILER —
    // which is exactly when `targetPinCandidate` above is left empty.
    //
    // The candidate answers "should mcpp apply its convention"; this answers
    // "what does the convention SAY", and the two differ precisely in the case
    // that needs a diagnosis: a project that overrode the convention and has
    // nothing supplying what the convention was there to supply.
    std::string targetRowPin;
    std::string targetRowName;
    // Whether the resolved toolchain spec names the machine's own Visual
    // Studio. Decided inside `resolve_target_toolchain`, read by
    // `host_tc_for_build_program`, which is why it is declared out here.
    bool tcSpecIsMsvc = false;

    auto root = overrides.project_root.empty()
        ? mcpp::project::find_manifest_root(std::filesystem::current_path())
        : std::optional<std::filesystem::path>(overrides.project_root);
    if (!root) {
        return std::unexpected("no mcpp.toml found in current directory or any parent");
    }
    // NOTE: `workRoot` is deliberately NOT derived here. `root` is not final
    // yet — the workspace block below reassigns it to the selected member
    // (`root = memberDir`), and anchoring the write root to the pre-switch
    // value puts a member's target/, mcpp.lock and .mcpp/ at the WORKSPACE
    // root. See the derivation right after that block.

    // A registry package in `compat` form (Form B) ships NO mcpp.toml — its
    // manifest is synthesized from the `.lua` descriptor by the resolver. So a
    // nested build of such a package cannot re-read one off disk, and the
    // caller hands over the manifest it already synthesized instead.
    //
    // Passing it in rather than re-deriving it is also the more correct of the
    // two: re-deriving could produce a DIFFERENT manifest than the one the
    // parent resolved against (the L1 cfg merge and feature-activated deps
    // have already been folded in by then).
    auto m = overrides.preloaded_manifest
        ? std::expected<mcpp::manifest::Manifest, mcpp::manifest::ManifestError>(
              *overrides.preloaded_manifest)
        : mcpp::manifest::load(*root / "mcpp.toml");
    // A COMMAND ISSUED INSIDE A MEMBER DIRECTORY loads that member's manifest
    // here, before anything knows a workspace is above it — so a member relying
    // on `[workspace.package]` for a required field would be refused by the
    // parser before inheritance could supply it. Retried, not reordered: the
    // workspace lookup walks the tree reading manifests, and paying that on
    // every build to serve the error path would be the wrong trade. The
    // requirement still holds; it is enforced after inheritance, where "still
    // missing" is knowable.
    if (!m && !overrides.preloaded_manifest
           && !mcpp::project::find_workspace_root(*root).empty())
        m = mcpp::manifest::load(*root / "mcpp.toml", {.insideWorkspace = true});
    if (!m) return std::unexpected(m.error().format());

    // AND ONLY FOR THE ROOT. A layer name this engine does not know is a
    // typo in the manifest the author is looking at, and a version gap in a
    // dependency's. The reserved `mcpp:` prefix exists so the first is an error
    // rather than a silently disabled behaviour; refusing the second as well
    // meant the layer vocabulary could never be extended by a published package
    // (`warn_unknown_xpkg_keys` carries that half).
    if (!m->unknownCapabilities.empty()) {
        auto const& cap = m->unknownCapabilities.front();
        auto why = mcpp::targetside::parse_capability(cap);
        return std::unexpected(std::format(
            "{}: {}", (*root / "mcpp.toml").string(),
            why ? std::format("`{}` names no capability mcpp knows.", cap)
                : why.error()));
    }

    // A DISTRIBUTION package is not a source tree, and building "in" one is a
    // failure that looks like a success: `interface/` holds declarations whose
    // definitions are in the prebuilt archive, so the build compiles the
    // declarations, produces a near-empty library, links nothing, and reports
    // Finished. The archive it was supposed to carry never enters the picture.
    //
    // Only the ROOT is refused. As a dependency this is exactly what the
    // package is for — the consumer compiles the interface and links the
    // artifact, which is the whole design.
    if (!overrides.preloaded_manifest && mcpp::pack::is_distribution_package(*m)) {
        return std::unexpected(std::format(
            "'{}' is a distribution package produced by `mcpp pack`, not a source tree.\n"
            "  Its sources are interface declarations; the definitions are in the\n"
            "  prebuilt artifacts beside them, so building here would produce an\n"
            "  empty library and say it succeeded.\n"
            "  Use it: add it to a project as a dependency —\n"
            "      [dependencies]\n"
            "      {} = {{ path = \"{}\" }}",
            root->string(), m->package.name, root->string()));
    }

    // ─── Workspace handling ────────────────────────────────────────────
    // If the manifest has [workspace] and is a virtual workspace (no [package]),
    // or if -p filter is set, switch to the target member's manifest.
    std::optional<mcpp::manifest::Manifest> wsManifest;  // keep workspace manifest alive
    std::filesystem::path runtimeWorkspaceRoot;
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
                auto mm = mcpp::manifest::load(memberDir / "mcpp.toml",
                                               {.insideWorkspace = true});
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
            runtimeWorkspaceRoot = *root;
            wsManifest = std::move(*m);  // preserve workspace manifest
            m = mcpp::manifest::load(memberDir / "mcpp.toml",
                                     {.insideWorkspace = true});
            if (!m) return std::unexpected(std::format(
                "workspace member '{}': {}", targetMember, m.error().format()));

            // ONE call, not a hand-copied list. `*root` is still the WORKSPACE
            // root here (the `root = memberDir` reassignment below has not
            // happened yet), which is what a relative `[indices].path` or
            // `[workspace.dependencies] path` was written against (#224).
            mcpp::project::inherit_workspace_config(*m, *wsManifest, *root);
            if (auto bad = mcpp::project::workspace_inheritance_error(*m, memberDir))
                return std::unexpected(*bad);

            mcpp::ui::status("Workspace", std::format("building member '{}'", targetMember));
            root = memberDir;
        }
    } else {
        // Not at workspace root — check if we're inside a workspace
        auto wsRoot = mcpp::project::find_workspace_root(*root);
        if (!wsRoot.empty()) {
            auto wsm = mcpp::manifest::load(wsRoot / "mcpp.toml");
            if (wsm && wsm->workspace.present) {
                runtimeWorkspaceRoot = wsRoot;
                wsManifest = std::move(*wsm);
                // The SECOND inheritance site, and it calls the same function
                // as the first for that reason. #224: relative `path` and
                // `[indices].path` anchor to the workspace root, not to this
                // member's own directory.
                mcpp::project::inherit_workspace_config(*m, *wsManifest, wsRoot);
                if (auto bad = mcpp::project::workspace_inheritance_error(*m, *root))
                    return std::unexpected(*bad);
            }
        }
    }

    mcpp::xlings::runtime::RuntimeSelection runtimeSelection;
    if (overrides.inherited_runtime_selection) {
        runtimeSelection = *overrides.inherited_runtime_selection;
    } else {
        std::optional<std::reference_wrapper<const mcpp::manifest::Manifest>> wsRef;
        if (wsManifest) wsRef = std::cref(*wsManifest);
        auto selected = mcpp::xlings::runtime::select_runtime(
            *m, wsRef, *root, runtimeWorkspaceRoot);
        if (!selected) return std::unexpected(selected.error());
        runtimeSelection = std::move(*selected);
    }

    // Where mcpp WRITES — derived here because `root` is only final now: the
    // workspace block above may have moved it to the selected member. Defaults
    // to the project root, so every existing invocation is byte-for-byte
    // unchanged; the tool-provisioning pass points it at the tool store
    // instead (BuildOverrides::work_dir).
    const std::filesystem::path workRoot =
        overrides.work_dir.empty() ? *root : overrides.work_dir;
    {
        std::error_code wdEc;
        std::filesystem::create_directories(workRoot, wdEc);
    }

    if (m->package.sourceProvenance.empty()) {
        m->package.sourceProvenance =
            "path+" + root->lexically_normal().generic_string();
    }

    // A `compat`-form (Form B) package's sources live under a wrap directory
    // inside the version dir, which is why its descriptor writes globs like
    // `*/src/foo.cc` — the `*` stands for the tarball's top-level folder,
    // whose name the descriptor cannot know. `[build] sources` has always
    // expanded those; `targets.<x>.main` did NOT, so a bin target in such a
    // package handed ninja a literal `*` and died with
    // `missing and no known rule to make it`.
    //
    // Nothing could reach that path before #355 (a dependency's bin targets
    // were never built), which is why it went unnoticed. Resolve it here, once
    // the manifest is final and before anything reads `t.main`.
    for (auto& t : m->targets) {
        if (t.main.empty() || t.main.find('*') == std::string::npos) continue;
        auto hits = mcpp::modgraph::expand_glob(*root, t.main);
        if (hits.size() == 1) {
            t.main = std::filesystem::relative(hits.front(), *root).generic_string();
        } else {
            return std::unexpected(std::format(
                "target '{}': `main = \"{}\"` matched {} files; it must name "
                "exactly one entry source",
                t.name, t.main, hits.size()));
        }
    }

    // Inject synthetic targets (e.g. test binaries from `mcpp test`).
    for (auto& t : extraTargets) m->targets.push_back(t);

    // #540: a cfg() predicate mcpp cannot evaluate must say so.
    //
    // A PREDICATE THAT ANSWERS FALSE AND A PREDICATE THAT WAS NEVER
    // UNDERSTOOD USED TO READ THE SAME. `cfgpred` returns false for an unknown
    // key and for an unknown bareword, and a `[target.<pred>.build]` section
    // whose predicate is false is dropped without a word — so a typo, and every
    // `cfg(c-abi = …)` section docs/14 documented before this release, produced
    // a successful build configured as if the section had not been written.
    //
    // Reported here rather than in the manifest parser because the vocabulary
    // lives with the evaluator, and a second copy of it in `toml.cppm` is the
    // exact defect this release is fixing four other instances of.
    //
    // Scoped to the root manifest by where it sits, which matches the existing
    // policy for every other schema warning: a dependency may adopt a predicate
    // a consumer's older mcpp does not know, and its build stays quiet.
    for (auto const& cc : m->conditionalConfigs) {
        auto unknown = cfgpred::unknown_tokens(cc.predicate);
        if (!unknown.empty()) {
            std::string names;
            for (auto const& u : unknown) {
                if (!names.empty()) names += ", ";
                names += '\'' + u + '\'';
            }
            m->schemaWarnings.push_back(std::format(
                "[target.'{}'] names {} in its cfg() predicate, which mcpp does "
                "not know, so the section never applies (ignored). {}",
                cc.predicate, names, cfgpred::vocabulary_sentence()));
        }
        // A layer is resolved AFTER dependency resolution, so a dependency
        // selected by one would form a cycle with the resolution that produces
        // the answer — docs/14 states this. The section's build inputs are
        // honoured by the second pass; its dependencies cannot be, and saying
        // so is the difference between a documented limit and a silent drop.
        if (cfgpred::uses_layer(cc.predicate)
            && !(cc.dependencies.empty() && cc.devDependencies.empty()
                 && cc.buildDependencies.empty() && cc.featureDeps.empty())) {
            m->schemaWarnings.push_back(std::format(
                "[target.'{}'] conditions dependencies on a target-side layer "
                "(ignored). A layer is resolved from the dependency graph, so a "
                "dependency chosen by one would decide the answer it is asking "
                "for. Build inputs under this predicate DO apply; move the "
                "dependency to an unconditional [dependencies] entry, or "
                "condition it on the triple instead.",
                cc.predicate));
        }
    }

    // Surface non-fatal manifest schema warnings (e.g. unsupported [targets.*]
    // keys). Under --strict they become errors — same policy as the
    // feature/platform schema checks below.
    for (auto const& w : m->schemaWarnings) {
        if (overrides.strict) return std::unexpected(w);
        mcpp::diag::warning("manifest/schema", w);
    }

    // Load mcpp.lock once, up front: it is a resolution input for git deps
    // (#329), which decide the commit to build long before anything is
    // fetched. Keyed by package name — the same key the writer at the end of
    // this function emits, both taken from the root manifest's [dependencies].
    std::map<std::string, mcpp::pm::LockedGitSource> gitLockAnchors;
    std::map<std::string, std::string> packageIdentityLockAnchors;
    {
        auto lockPath = workRoot / "mcpp.lock";
        if (std::filesystem::exists(lockPath)) {
            if (auto lock = mcpp::pm::load(lockPath); lock) {
                for (auto const& p : lock->packages) {
                    if (!p.namespace_.empty())
                        packageIdentityLockAnchors.emplace(
                            p.name, p.namespace_);
                    if (auto parsed = mcpp::pm::parse_git_source(p.source); parsed)
                        gitLockAnchors.emplace(p.name, std::move(*parsed));
                }
            } else {
                // Degraded, not a plain warning: the engine silently does less
                // than asked — every git branch dep falls back to `ls-remote`
                // and may advance past the commit the lock recorded.
                mcpp::diag::degraded("lockfile",
                    std::format("mcpp.lock could not be read: {}",
                                lock.error().message),
                    "git branch dependencies are re-resolved over the network "
                    "and may move onto a newer commit than the one recorded",
                    "delete mcpp.lock and rebuild to regenerate it");
            }
        }
    }

    // Global-cache mode: --cache > MCPP_BUILD_CACHE > [build] cache > global.
    // An unparseable value is a warning (error under --strict) and falls
    // through to the next source rather than silently meaning "global" — a typo
    // that quietly re-enabled the cache would be the hardest kind of surprise
    // to attribute.
    // Selection lives in resolve_cache_mode (above) so the fast paths settle it
    // identically. This block only adds the diagnostics, which the fast paths
    // have no business emitting: an unparseable value must be reported once, by
    // the invocation that actually resolves the build.
    const CacheMode cacheMode = resolve_cache_mode(*m, overrides.cache_mode);
    {
        const char* envMode = std::getenv("MCPP_BUILD_CACHE");
        for (auto [value, origin] : std::initializer_list<
                 std::pair<std::string_view, std::string_view>>{
                 {overrides.cache_mode,        "--cache"},
                 {envMode ? envMode : "",      "MCPP_BUILD_CACHE"},
                 {m->buildConfig.cacheMode,    "[build] cache"}}) {
            if (value.empty() || parse_cache_mode(value)) continue;
            auto msg = std::format(
                "{} has unknown cache mode '{}' (expected: global | local | off)",
                origin, value);
            if (overrides.strict) return std::unexpected(msg);
            mcpp::diag::warning("build/cache-mode", msg);
        }
    }

    // ─── Toolchain resolution (docs/21) ────────────────────────────────
    //
    // THE WHOLE CHAIN, in the order it is applied. It was documented twice, as
    // "3 steps" here and "4 steps" further down, and neither list had been
    // true for a long time — between them they named five of the nine inputs
    // below and disagreed about two. A comment that undercounts the inputs to
    // a decision is worse than none: it tells the next reader they have seen
    // the whole thing.
    //
    // The WHAT (which spec) is settled first, then the HOW (which binary).
    // Anything that WRITES `tcSpec` also writes `tcOrigin`, and that is the
    // invariant this table rests on — the enumerator names below are real, so
    // this comment cannot quietly stop matching the code.
    //
    //   WHICH SPEC                                       tcOrigin
    //   1. mcpp.toml [toolchain].<platform> / .default   ManifestToolchain
    //   2. global config.toml [toolchain] default        GlobalDefault
    //   3. mcpp.toml [target.<triple>].toolchain         TargetSection
    //      (--target / [build] target / config default
    //       select the section; 3 outranks 1 and 2)
    //   4. the target vocabulary's pin (triple.cppm)     TargetPin
    //      — a convention, and it stands down when a
    //        REMEMBERED target would overrule a spec the
    //        user wrote down
    //   5. the platform's first-run default, installed   FirstRun
    //      and persisted by this very invocation
    //
    //   WHICH BINARY, from the spec settled above
    //   6. `msvc@system`  → probe the machine (no xim package exists)
    //   7. `<family>@<v>` → xim payload; msvc resolves through
    //                       resolve_managed_msvc, everything else through
    //                       the bin/-shaped frontend lookup
    //   8. bare `system`  → the PATH compiler. A deliberate escape hatch, and
    //                       the ONLY host-compiler route: there is no
    //                       `gcc@system` (see parse_toolchain_spec)
    //   9. offline / MCPP_NO_AUTO_INSTALL → hard error rather than a silent
    //                       ~800 MB download
    //
    // AND ONE REVISION, after 1-9 have produced a toolchain: a spec targeting
    // the MSVC ABI on a machine with no usable MSVC is switched to MinGW-w64
    // — but only when `tc_origin_is_user_explicit` says mcpp chose it itself.
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

    // Resolve one exact runtime contract before resolving/fixing a toolchain.
    // The fixup is itself a consumer of RuntimeBinding: doing it first would
    // recreate #392 by letting directory order choose a libc and only later
    // discovering what the project selected.
    mcpp::platform::runtime::RuntimeBinding runtimeBindingSnapshot;
    if (overrides.inherited_runtime_binding) {
        runtimeBindingSnapshot = *overrides.inherited_runtime_binding;
    } else {
        auto cfgRuntime = get_cfg();
        if (!cfgRuntime) return std::unexpected(cfgRuntime.error());
        auto resolved = mcpp::platform::runtime::resolve_runtime_binding(
            runtimeSelection, {}, (**cfgRuntime).xlingsHome());
        if (!resolved) return std::unexpected(resolved.error());
        runtimeBindingSnapshot = std::move(*resolved);
        // A degradation that nobody prints is indistinguishable from no
        // degradation, which is the failure this whole area keeps paying for.
        // A note is not a warning: nothing is wrong with the build, some facts
        // are simply unavailable — so it is reported once, at info level.
        if (!runtimeBindingSnapshot.note.empty())
            mcpp::ui::info("Runtime", runtimeBindingSnapshot.note);
    }
    // THE `bin` THIS PROJECT'S BUILD PROGRAMS SEE FIRST — derived ONCE,
    // here, from the selection that has just been resolved.
    //
    // Empty unless the manifest declared `[xlings].subos`. That is deliberate:
    // prepending the SHARED `subos/default/bin` would make what a build sees
    // depend on what else has been installed on this machine, so a project
    // that has not asked for an environment of its own gets the `PATH` mcpp
    // was started with, byte for byte.
    //
    // NOT RE-DERIVED AT THE TWO DELIVERY SITES BELOW, AND NOT FROM
    // `[xlings] deps`. `mcpp::xlings::runtime` is the sole runtime-selection
    // policy and `RuntimeBinding::subosDir` is its resolved answer; a second
    // derivation is how a build ends up with two subos and no way to say which
    // one it used. The per-package payload paths a program may also need are
    // already answered, separately, by `MCPP_XPKG_*_DIR`.
    const std::string projectSubosBin = [&]() -> std::string {
        using Mode = mcpp::xlings::runtime::RuntimeSelection::Mode;
        if (runtimeBindingSnapshot.selection.mode != Mode::NamedSubos)
            return {};
        auto bin = runtimeBindingSnapshot.subosDir / "bin";
        std::error_code ec;
        if (!std::filesystem::is_directory(bin, ec)) return {};
        return bin.string();
    }();

    const auto runtimePayload = runtimeBindingSnapshot.libc.value_or("");
    const auto runtimeLibDir = runtimeBindingSnapshot.libraryDirs.empty()
        ? std::filesystem::path{} : runtimeBindingSnapshot.libraryDirs.front();

    // mcpp#427: a toolchain fixup that could not run is a DEGRADATION, not a
    // failure — the build continues without it. But it has to be said, or the
    // eventual `stdlib.h: No such file or directory` arrives with no way to
    // connect it to its cause.
    //
    // Deduplicated by payload: `ensure_post_install_fixup` is called from up
    // to four seams in one build (manifest toolchain, default toolchain,
    // MinGW first-run, build.mcpp host toolchain) and they routinely resolve
    // the SAME payload. Saying it once is the rule mcpp#417 already paid for.
    auto fixupNoticed = std::make_shared<std::set<std::string>>();
    auto report_fixup = [fixupNoticed](
            const mcpp::toolchain::FixupOutcome& outcome,
            const std::filesystem::path& payloadRoot) {
        if (outcome.skippedReason.empty()) return;
        if (!fixupNoticed->insert(payloadRoot.generic_string()).second) return;
        // Only the fact this line ADDS. The `Runtime` note above already gave
        // the cause and the remedy for the same absence; repeating them here
        // would be the second copy of one message, which is the habit mcpp#417
        // exists to break.
        mcpp::ui::info("Toolchain", std::format(
            "used as installed — not patched against a C runtime ({})",
            outcome.skippedReason));
    };

    constexpr std::string_view kCurrentPlatform = mcpp::platform::name;

    // Toolchain resolution priority: see the table at the top of this
    // function. Stated once, where `tcOrigin` is introduced — this used to be
    // a second, shorter and differently-wrong list of the same thing.
    //
    // Resolve the build profile, overlaid by any [profile.<name>] from the
    // manifest → buildConfig. `effectiveProfile` outlives the block: the
    // build.mcpp env contract exposes it as MCPP_PROFILE.
    std::string effectiveProfile;
    {
        auto& pname = effectiveProfile;
        // Precedence lives in resolve_profile_name (above) so execute.cppm's
        // fast paths settle it identically without running prepare_build.
        // Release is opt-in via --release / --profile release; a project that
        // wants its plain `mcpp build` optimized sets
        // [build].default-profile = "release" (mcpp's own mcpp.toml does this,
        // so the released binary stays -O2).
        pname = resolve_profile_name(*m, overrides.profile, overrides.profile_fallback);
        mcpp::manifest::Profile pr;
        if (pname == "dev" || pname == "debug") { pr.optLevel = "0"; pr.debug = true; }
        else if (pname == "dist")               { pr.optLevel = "3"; pr.strip = true; }
        // (built-in dist intentionally leaves lto off: several packaged gcc
        //  payloads ship without the LTO plugin; enable via [profile.dist].)
        else                                    { pr.optLevel = "2"; } // release
        if (auto it = m->profiles.find(pname); it != m->profiles.end()) pr = it->second;
        // #519 — a profile may override the whole-graph form. OPTIONAL, so a
        // profile that does not mention it leaves `[build]` standing; a plain
        // value would reset it, because the block above REPLACES `pr` wholesale
        // with the declared profile.
        if (pr.dependencyLinkage)
            m->buildConfig.dependencyLinkage = *pr.dependencyLinkage;
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

    // Every directory a package payload may legitimately have been INSTALLED
    // into: the global registry, plus the two project-local data roots a custom
    // git index installs into. Defined HERE, above its first use, because three
    // separate questions now depend on the same answer — where a dependency's
    // cache address is anchored, whether its sources came from a store at all,
    // and whether a `standard` declaration in its manifest was written by an
    // author or by a descriptor generator. One definition, three uses; deriving
    // the same fact twice is how two of them start disagreeing.
    const auto storeRoots = [&]() -> std::vector<std::filesystem::path> {
        std::vector<std::filesystem::path> roots;
        if (auto c = get_cfg()) roots.push_back((*c)->xlingsHome() / "data" / "xpkgs");
        for (auto& d : mcpp::config::project_xlings_data_roots(workRoot))
            roots.push_back(d / "xpkgs");
        return roots;
    }();

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
    // Where the spec came from decides whether mcpp may later revise it.
    // See TcOrigin: mcpp can rewrite a default it chose itself, but must not
    // silently overrule one the user wrote down.
    auto tcOrigin = tcSpec.has_value() ? TcOrigin::ManifestToolchain
                                       : TcOrigin::None;
    // `--toolchain` (arriving as MCPP_TOOLCHAIN, the same side channel
    // `--offline` and `--jobs` use) beats everything, including the manifest.
    //
    // This is the usable form of "which compiler". On this repository the
    // choice is worth 2.48x — gcc@16.1.0 builds mcpp in 79.9s, llvm@22.1.8 in
    // 32.2s — but CHANGING THE DEFAULT is an ecosystem decision, not a
    // performance one: it invalidates every published package's fingerprint and
    // the three platforms do not yet ship the same llvm. Selecting per build
    // costs nobody anything and needs no coordination.
    //
    // It counts as user-explicit, so mcpp will not quietly revise it.
    if (const char* tcEnv = std::getenv("MCPP_TOOLCHAIN"); tcEnv && *tcEnv) {
        tcSpec   = std::string(tcEnv);
        tcOrigin = TcOrigin::ManifestToolchain;
    }
    if (!tcSpec.has_value()) {
        auto cfg = get_cfg();
        if (cfg && !(*cfg)->defaultToolchain.empty()) {
            tcSpec   = (*cfg)->defaultToolchain;
            tcOrigin = TcOrigin::GlobalDefault;
        }
    }

    // ─── Windows first run without Visual Studio ────────────────────────
    // The host triple on Windows is MSVC-ABI, so the historical default
    // (llvm) resolves to clang targeting MSVC — which uses the MSVC STL and
    // the Windows SDK. Neither ships with Windows; both arrive only with
    // Visual Studio's "Desktop development with C++" workload. On a bare box
    // that default installs fine and then fails at compile time with no
    // actionable message.
    //
    // Seed only the TARGET axis and let the block right below derive the
    // rest: the vocabulary table already maps x86_64-windows-gnu to its pin
    // (winlibs GCC) and to static linkage, so the toolchain answer stays a
    // single derivation instead of being spelled out a second time here.
    // "Is MSVC usable here" — either origin. Asking `has_usable_msvc()` (which
    // probes the machine) would answer "no" on a box that has a pinned
    // msvc@<toolset> payload and no Visual Studio, and every decision below
    // would then divert a perfectly good toolchain to mingw.
    auto msvc_usable_either_origin = [&]() -> bool {
        auto c = get_cfg();
        if (!c) return mcpp::toolchain::msvc::has_usable_msvc();
        return mcpp::toolchain::msvc::msvc_available_here(
            (*c)->xlingsHome() / "data" / "xpkgs");
    };

    bool windowsGnuFirstRun = false;
    if constexpr (mcpp::platform::is_windows) {
        if (!tcSpec.has_value() && overrides.target_triple.empty()
            && m->buildConfig.target.empty()
            && !msvc_usable_either_origin()) {
            auto cfgW = get_cfg();
            if (!cfgW || (*cfgW)->defaultTarget.empty()) {
                overrides.target_triple =
                    std::string(mcpp::toolchain::triple::pins::kFirstRunWinGnuTarget);
                windowsGnuFirstRun = true;
            }
        }
    }

    // ─── --target / --static overrides ──────────────────────────────────
    // Target-axis default resolution when no --target flag was passed:
    // [build] target (project default, ≙ cargo build.target) >
    // [toolchain] default_target (global config) > host.
    if (overrides.target_triple.empty() && !m->buildConfig.target.empty())
        overrides.target_triple = m->buildConfig.target;
    // Remembered, not requested: this one came out of the global config, so
    // it must not outrank anything the user wrote down (see the pin below).
    bool targetFromGlobalDefault = false;
    if (overrides.target_triple.empty()) {
        if (auto cfg = get_cfg(); cfg && !(*cfg)->defaultTarget.empty()) {
            overrides.target_triple = (*cfg)->defaultTarget;
            targetFromGlobalDefault = true;
        }
    }
    // Normalize the triple (alias spellings → canonical), validate against
    // the known-target vocabulary, then apply the manifest [target.<triple>]
    // override and the vocabulary-table convention (pin + default linkage).
    if (!overrides.target_triple.empty()) {
        namespace triple = mcpp::toolchain::triple;
        // THE SPELLING THE PROJECT WROTE, KEPT FOR EVERY DIAGNOSTIC BELOW.
        // `overrides.target_triple` is canonicalised further down, and until
        // this variable existed the refusals quoted the canonical form:
        // `--target aarch64-linux` produced "target 'aarch64-linux-gnu' is
        // registered but not yet supported", a string the reader never typed
        // and cannot find in their own command.
        const std::string requestedSpelling = overrides.target_triple;
        auto parsed = triple::parse(overrides.target_triple);

        // THE REQUEST IS COMPLETED FROM THE VOCABULARY BEFORE ANYTHING
        // READS IT, AND THE ORDER RELATIVE TO THE `[target.X]` LOOKUP IS PART
        // OF THE CONTRACT.
        //
        // `parse` fills a missing env segment lexically so the identity stays
        // total — `x86_64-linux` IS `x86_64-linux-gnu`, and a unit test says so.
        // Every gate below then asked about the filled value instead of about
        // the request. See `triple::resolve_request` for the two measurements.
        //
        // The lookup that follows keys on `parsed->str()`, so completing after
        // it would match sections against a triple this build is not going to
        // use. A project wanting the `planned` row keeps its escape hatch by
        // WRITING the segment: `--target aarch64-linux-gnu` skips completion
        // entirely, because a written segment is a request rather than a gap.
        triple::RequestResolution req;
        if (parsed) {
            req    = triple::resolve_request(*parsed);
            parsed = req.triple;
        }

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
        // Several rows serve this (arch, os) and the lexical default names none
        // of them, so there is nothing to complete the request WITH. Refusing
        // and listing them is the only honest answer; picking one would be an
        // invented convention. No group has this shape today — the rule is here
        // so the first one that does gets a diagnosis rather than a guess.
        if (parsed && req.ambiguous && !hasExplicitSection) {
            std::string opts;
            for (auto s : req.supported) {
                if (!opts.empty()) opts += ", ";
                opts += std::string(s);
            }
            refusal::record(refusal::Code::AmbiguousRequest);
            return std::unexpected(std::format(
                "target '{}' does not say which C library, and several are "
                "supported here.\n"
                "       candidates: {}\n"
                "       Name one of them.",
                requestedSpelling, opts));
        }
        if (!known && !hasExplicitSection) {
            // "UNKNOWN" IS A CLAIM ABOUT THE VOCABULARY, AND IT WAS FALSE FOR
            // A WHOLE arch+os FAMILY.
            //
            // Measured on 2026.8.26.1: `--target riscv64-linux` reported
            // `unknown target 'riscv64-linux'` while `riscv64-linux-musl` was
            // sitting in `kKnownTargets` as `planned`. The lexical fill had
            // produced `riscv64-linux-gnu` — a row that genuinely does not
            // exist — and the gate reported on the fill.
            //
            // A non-empty sibling group means the family IS registered, so this
            // is the planned refusal wearing the wrong word. It names the row
            // that exists, which is also the one the reader would have to write
            // to opt in.
            if (!req.siblings.empty()) {
                std::string rows;
                for (auto s : req.siblings) {
                    if (!rows.empty()) rows += ", ";
                    rows += std::string(s);
                }
                refusal::record(refusal::Code::TierPlanned);
                return std::unexpected(std::format(
                    "target '{}' is registered but not yet supported (planned) — "
                    "no toolchain is published for it yet.\n"
                    "       registered rows for this system: {}\n"
                    "       An explicit [target.<triple>] toolchain override can "
                    "opt in early.",
                    requestedSpelling, rows));
            }
            auto sug = triple::did_you_mean(requestedSpelling);
            refusal::record(refusal::Code::UnknownTarget);
            return std::unexpected(std::format(
                "unknown target '{}'{}\n"
                "       known targets: `mcpp toolchain list`; a custom triple needs an\n"
                "       explicit [target.{}] section in mcpp.toml",
                requestedSpelling,
                sug ? std::format(" — did you mean '{}'?", *sug) : "",
                requestedSpelling));
        }
        if (known && known->tier == "planned" && !hasToolchainOverride) {
            refusal::record(refusal::Code::TierPlanned);
            // The subject is what the user wrote. When completion filled a
            // segment, both are shown — otherwise the sentence is about a
            // string that appears nowhere in their command.
            const std::string subject =
                requestedSpelling == parsed->str()
                    ? std::format("'{}'", requestedSpelling)
                    : std::format("'{}' (which resolves to '{}')",
                                  requestedSpelling, parsed->str());
            return std::unexpected(std::format(
                "target {} is registered but not yet supported (planned) — "
                "no toolchain is published for it yet.\n"
                "       An explicit [target.{}] toolchain override can opt in early.",
                subject, parsed->str()));
        }
        // Known, supported — and IMPOSSIBLE ON THIS HOST.
        //
        // Without this the target falls through to the host toolchain and the
        // build SUCCEEDS, which is the failure the check above calls the worst
        // one, arriving through a different door. Measured on Linux:
        //
        //   $ mcpp build --target x86_64-windows-msvc
        //       Resolved gcc@16.1.0 → x86_64-windows-msvc → …/xim-x-gcc/bin/g++
        //       Finished dev [unoptimized + debuginfo] in 0.07s
        //   $ ls target/
        //       x86_64-linux-gnu/          ← an ELF, reported as a Windows build
        //
        // The vocabulary tier says "mcpp supports this target"; it never said
        // "this machine can produce it". `host_can_serve` is the answer to the
        // second question and lives beside the payload resolution it has to
        // agree with.
        //
        // The escape hatch stays open on purpose: an explicit `[target.X]`
        // toolchain override means the author is supplying the cross toolchain
        // themselves, and mcpp's payload matrix has no standing to refuse it.
        // DIAGNOSED HERE, REPORTED LATER, AND THE DIFFERENCE IS THE POINT.
        //
        // Whether a payload on this machine produces this target is knowable
        // now. Whether anything ELSE produces it is not: a dependency can
        // supply the target's platform interface and C library, and the
        // dependency graph does not exist yet at this line. Refusing here
        // therefore answered a narrower question than the one it claimed —
        // measured, a project that only had to add a dependency was told its
        // machine could not build the target at all.
        //
        // The refusal is kept in full, because it is right whenever nothing
        // supplies the target side, which remains the ordinary case. It is
        // carried to where the graph is known and released there. Nothing
        // between here and there consumes the answer: what follows is toolchain
        // and dependency resolution, and a target no payload serves resolves to
        // a driver that simply will not be asked to emit anything.
        //
        // The escape hatch stays open on purpose: an explicit `[target.X]`
        // toolchain override means the author is supplying the cross toolchain
        // themselves, and mcpp's payload matrix has no standing to refuse it.
        if (known && known->tier != "planned" && !hasToolchainOverride
            && parsed
            && !mcpp::toolchain::host_can_serve(*parsed)) {
            std::string servable;
            for (auto const& info : triple::known_targets()) {
                auto t = triple::parse(info.canonical);
                if (!t || info.tier == "planned") continue;
                if (!mcpp::toolchain::host_can_serve(*t)) continue;
                if (!servable.empty()) servable += ", ";
                servable += t->str();
            }
            unservedTargetDiagnosis = std::format(
                "target '{}' cannot be built on this host.\n"
                "       No toolchain payload here produces it, and nothing in "
                "the dependency graph\n"
                "       supplies its system side.\n"
                "       this host can build with the payload alone: {}\n"
                "       To build it anyway, depend on a package that implements "
                "the target's system\n"
                "       (its kernel interface and C library), or supply your own "
                "cross toolchain with\n"
                "       an explicit [target.{}] toolchain = \"…\" section.",
                parsed->str(),
                servable.empty() ? "(nothing — `mcpp toolchain list`)" : servable,
                parsed->str());
        }
        // CAPTURED BEFORE CANONICALISATION, BECAUSE CANONICALISATION IS
        // EXACTLY WHAT DESTROYS IT.
        //
        // `str()` renders the filled-in identity, so `x86_64-linux` becomes
        // `x86_64-linux-gnu` here and every later `parse` of that string reports
        // an env segment the project never wrote. The request has to be taken
        // from the ONLY triple that still knows the difference: this one.
        if (parsed && parsed->envExplicit) requestedCAbi = parsed->env;
        // AND THE SPELLING THE PROJECT USED, FOR THE REPORT ONLY.
        //
        // The canonical form is the identity — the output directory, the cache
        // key, the subject of a `cfg()` — and it must stay filled. The REPORT is
        // a different thing: it says what was asked for and what resolved, and
        // heading it `x86_64-linux-gnu` above a line reading `c-abi musl` states
        // a contradiction the build does not actually contain. A project that
        // declined to name a C library is shown as having declined.
        if (parsed && !parsed->envExplicit && !parsed->env.empty()) {
            auto asWritten = *parsed;
            asWritten.env.clear();
            targetDisplayName = asWritten.str();
        }

        // Canonical from here on: cfg evaluation, spec attachment and the
        // target/ output directory all see one spelling.
        if (parsed) overrides.target_triple = parsed->str();

        if (hasExplicitSection) {
            if (!it->second.toolchain.empty()) {
                tcSpec   = it->second.toolchain;
                tcOrigin = TcOrigin::TargetSection;
            }
            if (!it->second.linkage.empty())   m->buildConfig.linkage = it->second.linkage;
            // #336: a per-target C++ runtime contract overrides the project
            // default, so "self-contained everywhere except this triple" is
            // expressible without touching the cfg() input channel.
            if (!it->second.cxxRuntime.empty())
                m->buildConfig.cxxRuntime = it->second.cxxRuntime;
        }
        // Convention from the vocabulary table (triple.cppm): the target's
        // pinned toolchain (host-awareness — native musl-gcc vs triple-named
        // cross, winlibs mingw vs Linux-hosted cross — lives in the payload
        // mapping, not here) and its default linkage. GCC 16 pin rationale:
        // GCC 15 drops module template instantiations at link (remediation
        // doc A2; packages shipped 2026-07-08/09, GitHub+GitCode).
        // A convention, not an instruction: on the Windows-GNU first-run path
        // this is what turns the seeded target into `gcc@16.1.0`.
        //
        // It must not fire when it would overrule a toolchain the user wrote
        // down. The pin is mcpp's own default for a target row — `gcc@16.1.0`
        // for Windows-GNU, because the mingw payload is what supplies that
        // target's headers and C library — and an explicit `[toolchain]` line
        // is not a default. This is the promise the no-Visual-Studio fallback
        // is built on: mcpp revises its own defaults, never yours.
        //
        // HOW THE TARGET WAS NAMED IS NOT PART OF THE QUESTION, and it used to
        // be. The guard read `targetFromGlobalDefault && user_explicit`, so a
        // target given on the command line disabled it — and then the row's pin
        // replaced a toolchain the project had stated. Measured 2026-08-23:
        // `--target x86_64-windows-gnu` with an explicit `llvm@22.1.8` resolved
        // `x86_64-w64-mingw32-g++`, and gcc cannot compile libc++'s std module.
        //
        // A project that means to use a different compiler for a pinned target
        // is stating something about its own build, and a project whose target
        // side comes from its dependency graph is the ordinary reason to do so:
        // the payload the row names supplies headers and a C library that such
        // a project does not use. The narrower reading of this guard was
        // patched with an openkal-specific exception; stating the rule
        // correctly removes the need for one.
        // RECORDED, NOT APPLIED. The convention answers "which payload
        // supplies this target's C library", and whether it is needed depends on
        // whether the dependency graph supplies one instead. That is knowable
        // only after resolution, so the decision waits for
        // `resolve_target_toolchain` and only the candidate is kept here.
        if (known && !known->pin.empty() && parsed
            && !parsed->pin_is_capability()) {
            targetRowPin  = std::string(known->pin);
            targetRowName = parsed->str();
        }
        if (known && !hasToolchainOverride && !known->pin.empty()
            && !tc_origin_is_user_explicit(tcOrigin)) {
            targetPinCandidate = std::string(known->pin);
            targetPinIsCapability = parsed && parsed->pin_is_capability();
        }
        // A USER'S EXPLICIT TOOLCHAIN OVERRIDES A CONVENTION, NOT A
        // CAPABILITY — AND UNTIL THIS LINE IT OVERRODE BOTH.
        //
        // The block above deliberately steps aside for an explicit
        // `[toolchain] default`: a hosted row's pin says "this payload supplies
        // the target's C library", and an author who names their own compiler
        // has said they will supply it instead. A bare-metal row's pin says
        // something the author cannot override — the table's own words: "the
        // pin is llvm on every host because clang/lld are cross-compilers by
        // construction". A host g++ does not emit riscv64 whatever anyone
        // declares.
        //
        // Measured 2026-08-26:
        //
        //     [toolchain] default = "gcc@16.1.0"
        //     $ mcpp build --target riscv64-none-elf
        //       g++: error: unrecognized argument in option '-mabi=lp64d'
        //       g++: note: valid arguments to '-mabi=' are: ms sysv
        //
        // — a message about an option, for a decision made here. Refusing at
        // the decision costs one line; the alternative is a compiler complaining
        // about flags the reader never wrote.
        if (known && parsed && parsed->pin_is_capability()
            && tc_origin_is_user_explicit(tcOrigin) && tcSpec.has_value()) {
            auto declared = mcpp::toolchain::parse_toolchain_spec(*tcSpec);
            if (declared && declared->family != mcpp::toolchain::Family::Llvm) {
                // THE REASON TRAVELS WITH THE ROW. Both rows refuse for the
                // same rule and NOT for the same reason, and one sentence
                // covering both would be wrong about one of them: a PE+musl
                // target is not bare metal, and a reader told it is stops
                // reading.
                std::string_view why = parsed->is_freestanding()
                    ? "A freestanding target has no per-host cross payload: "
                      "clang and lld are\n"
                      "       cross-compilers by construction and gcc is not."
                    : "No gcc payload emits a PE with a musl C library — the "
                      "mingw payload emits\n"
                      "       PE with the MinGW CRT, which is the separate "
                      "`-gnu` row.";
                refusal::record(refusal::Code::CapabilityPin);
                return std::unexpected(std::format(
                    "target '{}' cannot be emitted by '{}'.\n"
                    "       {}\n"
                    "       The row names llvm as a capability rather than as a "
                    "preference, so this\n"
                    "       one line is not a convention you can override.\n"
                    "       remove the `[toolchain]` line for this target, or set "
                    "it to `{}`.",
                    parsed->str(), *tcSpec, why,
                    known->pin.empty() ? std::string_view("llvm") : known->pin));
            }
        }
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
    // ── The device axis, resolved ONCE ────────────────────────────────────
    //
    // `--accel` / `--no-accel` over `[build] accel`. `--no-accel` arrives as the
    // sentinel "(none)", which parse_accel reads as nothing, and printing the
    // parsed form back normalises the spelling -- so every reader below sees
    // one string, and a build program sees the same one in MCPP_ACCEL. Read
    // at call time rather than captured: a `[target.'cfg(...)'.build]` section
    // may set `accel`, and the merge that applies it runs a few lines down.
    //
    // "NO ACCELERATOR" IS THE EMPTY STRING HERE, NOT `accel_str`'s "(none)".
    //
    // `accel_str` is a DISPLAY function: it prints `(none)` for an empty set so
    // an ABI tag reads as a sentence. Handing that spelling on as a value made
    // two readers wrong at once. A build program saw `MCPP_ACCEL=(none)` while
    // the manual promised an empty string, so a rule package asking "is there
    // an accelerator" got a yes and a backend named `(none)`; and the
    // fingerprint's own guard, `if (!accel.empty())`, was true for every
    // project on earth, appending `#accel=(none)` to builds that had asked for
    // nothing. Measured 2026-09-05 with a build program that wrote the value to
    // a file, which is the only way to see it -- a program's stdout is shown
    // only when it fails.
    auto resolvedAccel = [&]() -> std::string {
        const auto sets = mcpp::pack::parse_accel(
            overrides.accel.empty() ? m->buildConfig.accel : overrides.accel);
        return sets.empty() ? std::string{} : mcpp::pack::accel_str(sets);
    };
    // The cfg context, with the accelerator layer filled from the resolved
    // accel's backend names. `cfg(accelerator = "cuda")` is a membership test
    // over these (prepare_inputs::Ctx::layer_matches); before this the field
    // was declared, documented, and never written, so the key matched nothing.
    auto cfgCtx = [&]() {
        auto c = cfgpred::context_for(overrides.target_triple);
        for (auto const& set : mcpp::pack::parse_accel(resolvedAccel()))
            c.accelerators.push_back(set.backend);
        return c;
    };
    const auto targetPlatform = mcpp::platform::TargetPlatform::for_os(cfgCtx().os);

    // ── L1: merge conditional [target.'cfg(...)'] sections ───────────────────
    // Evaluated now (target resolved) against the resolved target — the
    // --target triple for a cross build, else the host.
    //
    // #229: merge_conditional_config MUST run here — before
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
        merge_conditional_config(*m, cfgCtx());
    }
    // `[build].defines` must reach the scanner (P1689) and the compile edge,
    // and must participate in the fingerprint. Fold before dependency
    // resolution / fingerprinting.
    fold_build_defines_into_flags(m->buildConfig);

    // ORIGIN, RESOLVED ONCE.
    //
    // The spec used to be parsed TWICE from the same string a dozen lines
    // apart — once to ask "is this msvc@system", once to get the package —
    // and each call site drew its own conclusions from the result. Two parses
    // of one string is two places for the answer to differ, which is the shape
    // §1 of the three-axes design is about: a platform special case whose cost
    // is paid at every site that has to know about it.
    //
    // `Origin::SystemMsvc` is located on the machine and never resolved
    // through an xim package — mcpp does not install the machine's Visual
    // Studio. `Origin::Managed` is everything else, including a VERSIONED
    // msvc spec, and that is the point: what the manifest says is what gets
    // used, on every machine, instead of whatever this one happens to have.
    // RESOLVED HERE, RUN AFTER THE DEPENDENCY GRAPH — AND THE SPLIT IS THE
    // WHOLE POINT.
    //
    // A target row's convention does not name a preferred compiler. It names
    // the payload that supplies THAT TARGET'S C library. Whether the user's own
    // toolchain can serve the target instead depends on whether something ELSE
    // supplies the target side — and that is knowable only once the graph is
    // resolved, which is after this point in the function.
    //
    // Deciding early was measured to be wrong in both directions. Applying the
    // convention unconditionally replaced a toolchain the user had set with
    // `mcpp toolchain default`, for a payload their project never used. NOT
    // applying it turned a working zero-dependency cross build into a failing
    // one, because clang alone carries no C runtime for `x86_64-windows-gnu`
    // while the payload the row names does.
    //
    // The body does not MOVE; only its execution does. Everything between
    // here and the call site was measured to read `tc` exactly once, and that
    // one read wanted the target triple rather than the compiler.
    std::optional<mcpp::toolchain::Toolchain> tc;
    // `std::function` AND NOT `auto`, BECAUSE THE FIRST-RUN BRANCH INSIDE
    // CALLS BACK INTO IT. That branch installs a host default and then has to
    // resolve THAT default for the requested target — which is what the top of
    // this same function does. Recursing reuses it; writing it a second time
    // there would be a second answer to one question. Depth is one: the second
    // pass takes the `tcSpec.has_value()` branch that the first-run path just
    // made true.
    bool firstRunNeedsTargetPass = false;
    // Guards the one recursive call below. Set before the call so the second
    // pass cannot reach it, whatever else changed in between.
    bool targetPassDone = false;
    std::function<std::expected<void, std::string>()> resolve_target_toolchain;
    resolve_target_toolchain = [&]() -> std::expected<void, std::string> {
      std::optional<mcpp::toolchain::ToolchainSpec> parsedSpec;
      auto tcOriginAxis = mcpp::toolchain::Origin::Managed;
      if (tcSpec.has_value() && *tcSpec != "system") {
        // A parse FAILURE is not the same as an unparseable spec being
        // absent: `gcc@system` now fails here by name (see
        // parse_toolchain_spec), and swallowing that would put the error back
        // where it used to happen — somewhere else, saying something else.
        auto s = mcpp::toolchain::parse_toolchain_spec(*tcSpec);
        if (!s) return std::unexpected(std::format(
            "[toolchain].{} = '{}': {}", kCurrentPlatform, *tcSpec, s.error()));
        parsedSpec   = std::move(*s);
        tcOriginAxis = mcpp::toolchain::origin_of(*parsedSpec);
      }
      // ASSIGNED, NOT DECLARED. `host_tc_for_build_program` reads it and is
      // defined outside this lambda, so the declaration lives in the enclosing
      // scope; the value is still decided here, where the spec is parsed.
      tcSpecIsMsvc =
        parsedSpec && tcOriginAxis == mcpp::toolchain::Origin::SystemMsvc;

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
      } else if (parsedSpec) {
        auto spec = parsedSpec;
        if (spec->version.empty()) {
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

        // AND NOT INSTALLED WHEN NO PAYLOAD HERE COULD SERVE THE TARGET.
        //
        // `unservedTargetDiagnosis` is decided a thousand lines above and
        // released a thousand lines below — deliberately, because whether the
        // dependency GRAPH supplies the target's system is not knowable until
        // it is resolved. This install sits between the two, and it does not
        // need to wait: if no payload here serves the target, then either the
        // graph supplies the system (and this payload is not wanted) or the
        // build refuses later (and it is not wanted then either).
        //
        // Measured on ubuntu-24.04-arm, `--target x86_64-linux-musl`:
        //
        //     error: toolchain 'gcc@16.1.0': xlings install of
        //       'xim:x86_64-linux-musl-gcc@16.1.0' failed …
        //
        // — the cross-musl packages are published per host arch and that one is
        // x86_64-only. The refusal that names this correctly never ran, because
        // the install failed first and failed hard.
        //
        // Skipping leaves BOTH later paths intact; attempting cannot help
        // either of them.
        const bool targetPayloadUnservable =
            !unservedTargetDiagnosis.empty() && !spec->target.empty();

        auto cfg = get_cfg();
        if (!cfg) return std::unexpected(cfg.error());
        mcpp::fetcher::Fetcher fetcher(**cfg);

        mcpp::ui::info("Resolving", "toolchain");
        mcpp::fetcher::InstallProgressHandler progress;
        auto payload = fetcher.resolve_xpkg_path(
            pkg.target(), /*autoInstall=*/!targetPayloadUnservable, &progress);
        if (!payload && targetPayloadUnservable) {
            // The held diagnosis is already the right words for this; releasing
            // it here rather than at its usual site keeps one sentence per cause.
            refusal::record(refusal::Code::HostCannotServe);
            return std::unexpected(unservedTargetDiagnosis);
        }
        if (!payload) {
            // `windows = "msvc@19.44"` in a manifest is the retired
            // cl-version spelling; saying "no such xim package" would send
            // the reader looking for a toolset that cannot exist.
            if (spec->family == mcpp::toolchain::Family::Msvc) {
                if (auto hint = mcpp::toolchain::msvc::cl_version_spelling_hint(
                        spec->version))
                    return std::unexpected(*hint);
            }
            return std::unexpected(std::format(
                "toolchain '{}': {}", *tcSpec, payload.error().message));
        }

        // A pinned MSVC toolset: the payload root IS a VS-shaped root and the
        // package version IS the toolset directory name, so cl.exe is
        // derived, not searched for. Nothing here can silently pick a
        // different toolset — which is the defect this path exists to close.
        //
        // It also skips the two steps below: the bin/-shaped frontend lookup
        // (cl.exe is four levels deeper) and the ELF post-install fixup
        // (there is nothing to patchelf on a PE toolchain).
        if (spec->family == mcpp::toolchain::Family::Msvc) {
            // One rule, one place: where a managed toolset lives and why the
            // fetcher's `root` must not be used for it (mcpp.toolchain.
            // registry). Install and build asked the same question and each
            // answered it in its own words.
            auto inst = mcpp::toolchain::resolve_managed_msvc(
                mcpp::config::make_xlings_env(**cfg), pkg);
            if (!inst) return std::unexpected(inst.error());
            explicit_compiler = inst->clPath;
            mcpp::ui::info("Resolved", std::format(
                "{} → msvc {} ({})", spec->display(),
                inst->display_version(), inst->clPath.string()));
        } else {
            explicit_compiler = mcpp::toolchain::toolchain_frontend(payload->binDir, pkg);
            if (!std::filesystem::exists(explicit_compiler)) {
                return std::unexpected(std::format(
                    "toolchain payload '{}' has no known C++ frontend in {}",
                    pkg.target(), payload->binDir.string()));
            }
            // Same post-install fixup as `mcpp toolchain install` — this
            // manifest [toolchain] path previously ran none, so a freshly
            // auto-installed payload kept its stale install-time cfg /
            // unpatched runtime libs.
            if (auto fixed = mcpp::toolchain::ensure_post_install_fixup(
                    **cfg, payload->root, pkg,
                    runtimeBindingSnapshot.runtimeId, runtimeLibDir); !fixed)
                return std::unexpected(std::format(
                    "toolchain post-install fixup: {}", fixed.error()));
            else report_fixup(*fixed, payload->root);
            // Canonical rendering, whatever spelling the manifest/config used:
            // "Resolved gcc@16.1.0 → x86_64-linux-musl → <frontend>".
            //
            // AND IT SAYS SO WHEN MCPP CHOSE. A toolchain the user wrote down
            // needs no explanation — they can read their own manifest. One this
            // engine selected from a target row is a decision the user did not
            // make, and a status line that reports the outcome without the
            // reason leaves them to discover the rule by experiment.
            std::string chosenBy;
            // A COMPILER THE GRAPH ASKED FOR IS ANNOUNCED WITH THE PACKAGE
            // THAT ASKED. Without the name this reads as mcpp ignoring the
            // user's default; with it, it reads as the dependency it is.
            // The second line appears only when something was displaced —
            // "replacing nothing" is not worth a line.
            if (!graphCompilerRequiredBy.empty())
                chosenBy = std::format(
                    "\n             required by {} (`requires = "
                    "[\"mcpp:compiler={}\"]`){}",
                    graphCompilerRequiredBy, graphCompilerFamily,
                    graphCompilerReplaced.empty()
                        ? std::string{}
                        : std::format(", not your {} — this project only",
                                      graphCompilerReplaced));
            else if (!pinReplacedDefault.empty())
                chosenBy = std::format(
                    "\n             target default for {}, replacing your "
                    "{} — override with `[target.{}] toolchain`",
                    overrides.target_triple, pinReplacedDefault,
                    overrides.target_triple);
            else if (tcOrigin == TcOrigin::TargetPin
                  || tcOrigin == TcOrigin::FirstRun)
                chosenBy = std::format("  ({})", tc_origin_name(tcOrigin));
            mcpp::ui::info("Resolved",
                std::format("{} → {}{}", spec->display(),
                    mcpp::ui::shorten_path(explicit_compiler,
                        mcpp::fetcher::make_path_ctx(&**get_cfg(), *root)),
                    chosenBy));
        }
      } else if (tcSpec.has_value() && *tcSpec == "system") {
        // REFUSED. THE COMPILER IS THE ONE AXIS THAT IS NOT THE PROJECT'S TO
        // TAKE FROM THE HOST.
        //
        // mcpp's host-dependence policy is not uniform across axes, and the
        // split is the point rather than an inconsistency:
        //
        //   LIBRARIES are the program's business. A project may link a host
        //   library or its own `.so`; mcpp says what that costs and what the
        //   supported route is, and does not refuse as long as the result
        //   builds and runs. The developer owns the artifact and guarantees it.
        //
        //   THE TOOLCHAIN is mcpp's own contract. Everything mcpp promises —
        //   that `import std` is available, that the runtime closure is
        //   computable, that two machines and CI produce the same build — is a
        //   statement about a compiler mcpp resolved and can identify. A
        //   compiler picked off `PATH` makes every one of those promises
        //   unverifiable, and a build tool that cannot state what it built with
        //   is answering in the wrong version (see
        //   `.agents/docs/…a-build-must-be-able-to-state-its-own-version`).
        //
        // So this is refused rather than warned about, and it is refused HERE,
        // before any resolution work, so the message is the first thing the
        // user sees rather than a consequence three layers down.
        //
        // `msvc@system` is a different spelling and stays supported: it names a
        // FAMILY whose installation mcpp locates and identifies, on the one
        // platform where the compiler cannot be redistributed.
        return std::unexpected(std::format(
            "[toolchain] {} = \"system\" is not supported: mcpp builds only "
            "with toolchains it manages.\n"
            "       A compiler taken from PATH cannot be identified or "
            "reproduced, so `import std` availability, the runtime closure and "
            "\"the same build on another machine\" all stop being things mcpp "
            "can promise.\n"
            "       Name one instead — mcpp installs it on first use:\n"
            "\n"
            "         [toolchain]\n"
            "         {} = \"gcc@16.1.0\"\n"
            "\n"
            "       or set a machine default with `mcpp toolchain default "
            "gcc@16.1.0`, and see `mcpp toolchain list` for what is available.\n"
            "       (On Windows, `msvc@system` is different and remains "
            "supported: it names a family whose installation mcpp locates.)\n"
            "       Host LIBRARIES are a separate question and are not refused "
            "— a project may link them and owns the result.",
            kCurrentPlatform, kCurrentPlatform));
      } else if (mcpp::platform::env::offline_mode()
               || mcpp::platform::env::no_auto_install()) {
        // CI / offline / test opt-out: hard-error instead of silently
        // pulling ~800 MB of toolchain. Preserves the original M5.5
        // contract for environments that need it.
        //
        // `--offline` / MCPP_OFFLINE subsumes MCPP_NO_AUTO_INSTALL: the older
        // name only ever covered this one gate, which made "don't use the
        // network" three separate concepts with three spellings. The old var is
        // kept working (it predates offline mode and CI still exports it).
        namespace pins = mcpp::toolchain::triple::pins;
        // Name the knob that actually fired, not a fixed one: telling a user
        // who passed `--offline` to unset MCPP_NO_AUTO_INSTALL sends them
        // looking for a variable they never set.
        std::string_view release = mcpp::platform::env::offline_mode()
            ? "or drop --offline / unset MCPP_OFFLINE to let mcpp auto-install."
            : "or unset MCPP_NO_AUTO_INSTALL to let mcpp auto-install.";
        // Windows without a usable MSVC must not be told to install llvm:
        // that default resolves to clang targeting the MSVC ABI, which is
        // exactly what this machine cannot build. Name the toolchain that
        // will actually work there instead.
        if (mcpp::platform::is_windows
            && !msvc_usable_either_origin()) {
            return std::unexpected(std::format(
                "no toolchain configured (and no Visual Studio found).\n"
                "       run one of:\n"
                "         mcpp toolchain install {} --target {}\n"
                "         mcpp toolchain default {} --target {}\n"
                "       {}",
                pins::kSuggestGccMingw, pins::kFirstRunWinGnuTarget,
                pins::kFirstRunWinGnu,  pins::kFirstRunWinGnuTarget, release));
        }
        if constexpr (mcpp::platform::is_macos || mcpp::platform::is_windows) {
            return std::unexpected(std::format(
                "no toolchain configured.\n"
                "       run one of:\n"
                "         mcpp toolchain install {}\n"
                "         mcpp toolchain default {}\n"
                "       {}",
                pins::kSuggestLlvm, pins::kFirstRunMac, release));
        } else {
            return std::unexpected(std::format(
                "no toolchain configured.\n"
                "       run one of:\n"
                "         mcpp toolchain install {}\n"
                "         mcpp toolchain default {}\n"
                "       {}",
                pins::kSuggestGccMusl, pins::kFirstRunLinuxOther, release));
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
        if constexpr (mcpp::platform::is_macos) {
            defaultSpec = std::string(pins::kFirstRunMac);
        } else if constexpr (mcpp::platform::is_windows) {
            // Reaching here means msvc_usable_either_origin() was true — the seed above
            // diverts the no-Visual-Studio case onto the windows-gnu target
            // before the target block runs, so it never gets this far.
            defaultSpec = std::string(pins::kFirstRunWinMsvc);
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
        // kernel headers). One derivation, shared with `toolchain install` —
        // see registry.cppm for what the two spellings used to disagree about.
        if (mcpp::toolchain::needs_linux_sysroot_payloads(defaultParsed->target)) {
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
        if (auto fixed = mcpp::toolchain::ensure_post_install_fixup(
                **cfg, payload->root, defaultPkg,
                runtimeBindingSnapshot.runtimeId, runtimeLibDir); !fixed)
            return std::unexpected(std::format(
                "default toolchain post-install fixup: {}", fixed.error()));
        else report_fixup(*fixed, payload->root);

        // Persist the default so we don't ask again next time.
        if (auto wr = mcpp::config::write_default_toolchain(**cfg, defaultSpec); wr) {
            (*cfg)->defaultToolchain = defaultSpec;
            mcpp::ui::status("Default", std::format("set to {}", defaultSpec));
        } // best-effort: a failed config write only loses the persistence,
          // not the running build.
        tcSpec   = defaultSpec;
        tcOrigin = TcOrigin::FirstRun;

        // AND IF A TARGET WAS ASKED FOR, RESOLVE FOR IT — THIS BRANCH JUST
        // INSTALLED A HOST COMPILER AND WAS ABOUT TO BUILD WITH IT.
        //
        // Everything above answers "this machine has no toolchain, give it
        // one", and the answer is a HOST payload. `--target` was never read
        // here, so on a machine that had never built anything,
        // `mcpp build --target x86_64-windows-gnu` installed a native gcc and
        // compiled Windows sources with it. Measured in CI 2026-08-25:
        //
        //     First run  no toolchain configured — installing gcc@16.1.0 …
        //      Resolved  gcc@16.1.0 → …/xim-x-gcc/16.1.0/bin/g++
        //                                        ↑ no target in the path
        //
        // against the same command on a machine that already had one:
        //
        //      Resolved  gcc@16.1.0 → x86_64-windows-gnu → …/mingw-cross-gcc/…
        //
        // REUSES THE PATH THAT ALREADY KNOWS HOW, rather than repeating what
        // it does. `resolve_target_toolchain` maps a spec plus a target onto a
        // payload and installs it; the default just chosen is the spec. A
        // second implementation here would be a second answer to one question,
        // which is the shape this release exists to remove.
        // RECORDED HERE, ACTED ON BELOW — the Windows first-run block that
        // follows SETS `overrides.target_triple` itself, and returning from
        // here would skip it. Its own comment says why that matters: it
        // persists BOTH axes, and persisting only the target leaves
        // `mcpp toolchain list` disagreeing with what the build used.
        firstRunNeedsTargetPass = !overrides.target_triple.empty();
      }

      // Windows first run that got diverted to winlibs GCC: announce it and
      // persist BOTH axes, so the next invocation is silent and
      // `mcpp toolchain list` shows the same pair the build actually used.
      // Persisting only the target would leave the toolchain axis implicit
      // (derived from the vocabulary pin) and the two views would disagree.
      //
      // NOT WHEN THE DEPENDENCY GRAPH SUPPLIED THE ANSWER. This branch's
      // condition is `tcSpec.has_value()`, and since 2026.8.26.2 a package's
      // `requires = ["mcpp:compiler=…"]` can be what made it true — so a bare
      // Windows box building ONE project with an llvm-requiring dependency
      // would have persisted llvm as the MACHINE's default, and the next
      // project, which asked for nothing, would inherit it.
      //
      // A requirement is a property of the package that states it. It decides
      // this build and nothing else; the first-run answer for the machine is
      // still the one this branch was written for.
      if (windowsGnuFirstRun && tcSpec.has_value()
          && tc_origin_may_persist(tcOrigin)) {
        mcpp::ui::info("First run",
            std::format("no toolchain configured and no Visual Studio found — "
                        "using {} for {} (MinGW-w64, self-contained)",
                        *tcSpec, overrides.target_triple));
        if (auto cfgW = get_cfg(); cfgW) {
            if (mcpp::config::write_default_toolchain(**cfgW, *tcSpec))
                (*cfgW)->defaultToolchain = *tcSpec;
            if (mcpp::config::write_default_target(**cfgW, overrides.target_triple))
                (*cfgW)->defaultTarget = overrides.target_triple;
            mcpp::ui::status("Default",
                std::format("set to {} → {}", *tcSpec, overrides.target_triple));
        }
        tcOrigin = TcOrigin::FirstRun;
      }

      // AND NOW RESOLVE FOR THE TARGET, IF ONE WAS ASKED FOR.
      //
      // The first-run branch above answers "this machine has no toolchain, give
      // it one", and the answer is a HOST payload; `--target` was never read
      // there. On a machine that had never built anything,
      // `mcpp build --target x86_64-windows-gnu` therefore installed a native
      // gcc and compiled Windows sources with it — measured in CI 2026-08-25:
      //
      //     First run  no toolchain configured — installing gcc@16.1.0 …
      //      Resolved  gcc@16.1.0 → …/xim-x-gcc/16.1.0/bin/g++
      //                                        ↑ no target in the path
      //
      // against the same command where one already existed:
      //
      //      Resolved  gcc@16.1.0 → x86_64-windows-gnu → …/mingw-cross-gcc/…
      //
      // REUSES THE PATH THAT ALREADY KNOWS HOW rather than repeating it. The
      // default just chosen is the spec; mapping a spec plus a target onto a
      // payload (installing it if absent — `autoInstall` was always true there)
      // is what the top of this function does. Depth is one: the second pass
      // takes the `tcSpec.has_value()` branch the first run just made true.
      // ONE-SHOT, AND THE FLAG IS SET BEFORE THE CALL, NOT AFTER.
      //
      // This line sits OUTSIDE the first-run branch — it has to, because the
      // Windows block just above sets the target itself — so it is evaluated on
      // every pass. The first version relied on `firstRunNeedsTargetPass` being
      // false on the second pass; it is a captured variable that nothing
      // resets, so every pass recursed again. Measured in a consumer's CI as
      // the same `Resolved` line four times and then
      //
      //     ##[error]Process completed with exit code 139
      //
      // — SIGSEGV, a stack that ran out. A recursion whose termination depends
      // on state the recursive call does not change is not a depth-one
      // recursion, however its comment reads.
      if (!targetPassDone
          && (firstRunNeedsTargetPass
              || (windowsGnuFirstRun && tcSpec.has_value()))) {
        targetPassDone = true;
        return resolve_target_toolchain();
      }

      auto detected = mcpp::toolchain::detect(
          explicit_compiler, runtimePayload, runtimeBindingSnapshot.contractHash);
      if (!detected) return std::unexpected(detected.error().message);
      tc = std::move(*detected);

      // Something about the resolution the user has to be told, but which is
      // not a failure. Today's only producer is the Windows SDK axis: a managed
      // toolset binds the SDK it was installed with, so a `WindowsSdkDir` in
      // the environment does not apply — and an override that is ignored
      // SILENTLY is indistinguishable from one that was never set.
      if (!tc->resolutionNote.empty())
          mcpp::ui::info("note", tc->resolutionNote);

      // ── A retargetable driver has to be TOLD what it is targeting ────────
      //
      // `tc.targetTriple` comes from `-dumpmachine`, and for every cross target
      // that worked before this it was right for a reason that does not
      // generalise: those targets use a DISTINCT compiler binary
      // (`x86_64-w64-mingw32-g++`, `aarch64-linux-musl-g++`), whose own
      // -dumpmachine reports the cross triple. Clang is ONE binary that emits
      // every target it was built with, so -dumpmachine always answers with the
      // host — and nothing downstream ever learns otherwise.
      //
      // Measured before this line existed:
      //
      //   $ mcpp build --target riscv64-none-elf
      //       Resolved llvm@22.1.8 → riscv64-none-elf → …/bin/clang++
      //       Finished dev [unoptimized + debuginfo] in 0.47s
      //   $ ls target/
      //       x86_64-linux-gnu/          ← an ELF for the host, reported as riscv64
      //
      // That is E1: success reported, host artifact produced. The output
      // directory, the fingerprint, the cache key and the flag layer all read
      // `tc.targetTriple`, so correcting it here corrects all of them at once —
      // which is the point of there being one field rather than five answers.
      //
      // THIS USED TO BE SCOPED TO FREESTANDING, WITH THIS REASON:
      //
      //     The hosted cross targets already resolve a per-target binary, and
      //     overwriting their probed triple would replace a measured fact with
      //     an assumed one for no gain.
      //
      // That was true while every hosted cross was served by a payload. It
      // stops being true when the TARGET SIDE comes from the dependency graph:
      // the C library, the C++ runtime and the platform's own implementation are
      // then packages built from source, and the compiler is an ordinary clang —
      // whose `-dumpmachine` answers the host, exactly as the paragraph above
      // describes for freestanding.
      //
      // Measured 2026-08-23, with an explicit `[target.aarch64-macos]
      // toolchain = "llvm@…"`. The manifest's cfg evaluation used the REQUESTED
      // target, so the C library's aarch64 headers were on the command line; the
      // toolchain's own triple was still the host's, so code generation was
      // x86_64. Two answers to one question, in one command:
      //
      //     okm_float_assert.c: the C library and the compiler disagree about
      //     LDBL_DIG  ('33 == 18')          33 = aarch64 binary128, 18 = x87
      //
      // ⇒ The condition is now the property the first paragraph of this comment
      // already names: a RETARGETABLE driver has to be told. gcc is not one — a
      // gcc payload IS its target — so the mingw and musl-gcc crosses keep
      // answering from `-dumpmachine`, which for them remains a measured fact.
      if (!overrides.target_triple.empty()) {
          if (auto want = mcpp::toolchain::triple::parse(overrides.target_triple);
              want && (want->is_freestanding()
                       || tc->compiler == mcpp::toolchain::CompilerId::Clang))
          {
              tc->targetTriple = want->str();

              // And the flag that says it to the driver — for a HOSTED target
              // only. Freestanding already emits its own `--target`, together
              // with the ISA flags that must accompany it
              // (freestanding/target.cppm); a second one here would be the same
              // decision in two places.
              if (!want->is_freestanding()
                  && tc->compiler == mcpp::toolchain::CompilerId::Clang) {
                  tc->crossTargetFlag =
                      "--target=" + want->llvm_triple(
                          mcpp::platform::macos::deployment_target(
                              m->buildConfig.macosDeploymentTarget));
              }
          }
          if (auto want = mcpp::toolchain::triple::parse(overrides.target_triple);
              want && want->is_freestanding())
          {
              // `import std` is structurally hosted, and turning it off is the
              // SAME fact as the line above, not a second policy: libc++'s
              // std.cppm is one module over the whole library, including the
              // parts that are threads, filesystem and iostreams. There is no
              // subset of it to precompile.
              //
              // Left on, the failure is neither early nor legible — measured:
              //
              //   error: std module precompile failed (rc=1):
              //   .../include/c++/v1/__config:13:10: fatal error:
              //       '__config_site' file not found
              //
              // which reads as a broken toolchain payload and says nothing about
              // the target. The freestanding std subset a user actually wants is
              // an ordinary package (`mcpplibs.std.freestanding`), so mcpp's job
              // here is to stop pretending the hosted one exists and to say
              // where the other one is.
              tc->hasImportStd = false;
              tc->stdModuleSource.clear();
              tc->stdCompatSource.clear();

              // ── The target's C library, resolved like its compiler ─────────
              //
              // The row in kKnownTargets names it, exactly as it names the
              // toolchain pin, and it is installed through the same channel a
              // project's `[xlings] deps` use (see the materialization above).
              // Resolved HERE because the config is already open; the flag
              // builder only reads the result.
              //
              // Absent is not an error at this point: the install happens
              // earlier in this function and may legitimately not have run yet
              // on a first pass. What follows would then simply not add the
              // paths, and the link fails naming the missing libc — which is the
              // truthful message either way.
              if (const std::string want_sysroot =
                      mcpp::toolchain::triple::effective_sysroot(
                          *want, sysroot_override(*m, *want));
                  !want_sysroot.empty()) {
                  if (auto cfg3 = get_cfg(); cfg3) {
                      auto ref = mcpp::xlings::paths::parse_xpkg_ref(want_sysroot);
                      auto xl  = mcpp::config::make_xlings_env(**cfg3);
                      // INSTALLED, NOT MERELY LOOKED UP — THE SAME CHANNEL
                      // THE ROW'S TOOLCHAIN PIN GOES THROUGH.
                      //
                      // The row names two things and only one of them used to
                      // be made to exist: `pin` went through
                      // `resolve_xpkg_path(…, autoInstall=true, …)` while
                      // `sysroot` was a pure lookup that returned nullopt and
                      // let the whole block below be skipped without a word.
                      //
                      // Measured 2026-08-26 in a clean environment (an empty
                      // home, so mcpp's registry starts fresh):
                      //
                      //     Target riscv64-none-elf
                      //            c-abi  picolibc-riscv (…, prebuilt)
                      //     error: 'stdio.h' file not found
                      //
                      // The report named the C library and the build could not
                      // find its headers. mcpp's own bare-metal CI installs it
                      // by hand, which is why no test ever saw this — every
                      // bare-metal e2e runs on a machine where the gap has
                      // already been papered over.
                      //
                      // OFFLINE AND `MCPP_NO_AUTO_INSTALL` ARE THE FETCHER'S
                      // DECISION, not re-derived here. One question, one place
                      // that answers it — asking it twice is the shape this
                      // whole release exists to remove.
                      mcpp::fetcher::Fetcher srFetcher(**cfg3);
                      mcpp::fetcher::InstallProgressHandler srProgress;
                      std::optional<std::filesystem::path> dir;
                      if (auto p = srFetcher.resolve_xpkg_path(
                              want_sysroot, /*autoInstall=*/true, &srProgress))
                          dir = p->root;
                      else
                          dir = mcpp::xlings::paths::xpkg_payload(xl, ref);
                      if (dir) {
                          if (auto spec = mcpp::freestanding::resolve(*want)) {
                              const auto inc =
                                  *dir / "include" / std::string(spec->libdir);
                              const auto lib =
                                  *dir / "lib"     / std::string(spec->libdir);
                              std::error_code ec2;
                              tc->targetSysrootRoot = *dir;
                              tc->targetSysrootPkg  = ref.name;
                              if (std::filesystem::is_directory(inc, ec2))
                                  tc->targetSysrootInclude = inc;
                              if (std::filesystem::is_directory(lib, ec2))
                                  tc->targetSysrootLib = lib;
                          }
                      }
                  }
              }
          }
      }

      // The Windows runtime identity, flowing BACK into the contract.
      //
      // Everything else about the runtime is known before a toolchain is
      // resolved, and deliberately so (see the RuntimeBinding block above). The
      // Windows SDK is the exception: it is a property of the toolchain, and
      // until it reached the contract hash the version axis simply did not
      // exist one layer below the compiler — two SDKs produced one cache key.
      //
      // `ucrt@<v>` is a COMPATIBILITY FLOOR, not a payload binding like
      // `glibc@<v>`: ucrtbase.dll is an OS component and mcpp ships no
      // redistributable for it. See mcpp.runtime.binding.
      if (!tc->windowsSdkVersion.empty()) {
          mcpp::platform::runtime::bind_windows_ucrt(
              runtimeBindingSnapshot, tc->windowsSdkVersion);
          tc->runtimeContractHash = runtimeBindingSnapshot.contractHash;
      }

      // ── Targeting the MSVC ABI without a usable MSVC ─────────────────────
      //
      // One judgement, one place. This used to be two separate concerns and
      // only one of them was implemented: `msvc@system` with no Windows SDK
      // was caught here, while clang-targeting-MSVC on a machine with no
      // Visual Studio at all — the default on every bare Windows box — fell
      // straight through to clang's own "'vector' file not found", from which
      // no user could infer that a working alternative was one flag away.
      // Deriving the same judgement in two places is how the second case went
      // unnoticed, so they are now one condition with two outcomes.
      const bool targetsMsvcAbi =
          tc->compiler == mcpp::toolchain::CompilerId::MSVC
          || mcpp::toolchain::is_msvc_target(*tc);
      if (targetsMsvcAbi && !msvc_usable_either_origin()) {
          // Native cl.exe is ALWAYS a deliberate choice: mcpp never selects
          // msvc@system on its own — it cannot install one — so the only way it
          // reaches config.toml is a user typing `mcpp toolchain default msvc`.
          // Without this, that user (who evidently wants MSVC and is probably
          // just missing the SDK component) would be silently moved to MinGW
          // instead of being told which component to install.
          //
          // The residual imprecision is deliberate and bounded: a *global*
          // default of llvm@20.1.7 is indistinguishable from the one mcpp used
          // to write itself, so an explicitly-typed one gets repaired too. The
          // value is identical either way and the machine cannot build with it;
          // a user who wants that failure can pin it in mcpp.toml, which is
          // honoured exactly.
          const bool userChoseMsvcItself =
              tc->compiler == mcpp::toolchain::CompilerId::MSVC;
          // AND NOT A COMPILER THE GRAPH REQUIRED. The repair below rewrites
          // the machine's default to winlibs GCC, which is right when mcpp's
          // own default cannot work here. A family a package REQUIRED is not
          // mcpp's default to revise: switching to gcc would satisfy nothing —
          // `check_requirements` refuses the build three thousand lines later —
          // while having changed the user's configuration on the way there.
          // Refusing at the decision is what the rest of this release is about.
          const bool mayRepair =
              !tc_origin_is_user_explicit(tcOrigin)
              && tc_origin_may_persist(tcOrigin)
              && !userChoseMsvcItself
              && !mcpp::platform::env::offline_mode()
              && !mcpp::platform::env::no_auto_install()
              && mcpp::platform::is_windows;
          if (!mayRepair) {
              return std::unexpected(msvc_unavailable_guidance(*tc));
          }
          // mcpp chose this default itself and it cannot work on this machine.
          // Revise it — including for users who already have `llvm@20.1.7`
          // persisted by an older mcpp: the first-run branch never fires again
          // for them, so this gate (which runs on EVERY build) is what repairs
          // them without a single manual command.
          namespace pins = mcpp::toolchain::triple::pins;
          mcpp::ui::info("Toolchain",
              std::format("{} targets the MSVC ABI but no Visual Studio "
                          "(MSVC STL + Windows SDK) was found — switching to {} → {}",
                          tcSpec.value_or("the configured default"),
                          pins::kFirstRunWinGnu, pins::kFirstRunWinGnuTarget));

          overrides.target_triple = std::string(pins::kFirstRunWinGnuTarget);
          // The x86_64-windows-gnu row is defaultStatic; the target block that
          // normally applies that already ran, so mirror just this one field.
          if (m->buildConfig.linkage.empty()) m->buildConfig.linkage = "static";

          auto gnuSpec = mcpp::toolchain::parse_toolchain_spec(
              std::string(pins::kFirstRunWinGnu));
          if (!gnuSpec) return std::unexpected(gnuSpec.error());
          if (auto t = mcpp::toolchain::triple::parse(overrides.target_triple))
              gnuSpec->target = *t;
          auto gnuPkg = mcpp::toolchain::to_xim_package(*gnuSpec);

          auto cfgR = get_cfg();
          if (!cfgR) return std::unexpected(cfgR.error());
          mcpp::fetcher::Fetcher fetcherR(**cfgR);
          mcpp::fetcher::InstallProgressHandler progressR;
          auto payloadR = fetcherR.resolve_xpkg_path(gnuPkg.target(),
                              /*autoInstall=*/true, &progressR);
          if (!payloadR) {
              return std::unexpected(std::format(
                  "switching to the MinGW-w64 toolchain ({}) failed: {}\n"
                  "       install it manually with:\n"
                  "         mcpp toolchain install {} --target {}",
                  pins::kFirstRunWinGnu, payloadR.error().message,
                  pins::kSuggestGccMingw, pins::kFirstRunWinGnuTarget));
          }
          explicit_compiler =
              mcpp::toolchain::toolchain_frontend(payloadR->binDir, gnuPkg);
          if (!std::filesystem::exists(explicit_compiler)) {
              return std::unexpected(std::format(
                  "MinGW-w64 payload {} has no known C++ frontend in {}",
                  gnuPkg.target(), payloadR->binDir.string()));
          }
          if (auto fixed = mcpp::toolchain::ensure_post_install_fixup(
                  **cfgR, payloadR->root, gnuPkg,
                  runtimeBindingSnapshot.runtimeId, runtimeLibDir); !fixed)
              return std::unexpected(std::format(
                  "MinGW toolchain post-install fixup: {}", fixed.error()));
          else report_fixup(*fixed, payloadR->root);

          // Persist both axes so the repair happens once, not on every build.
          if (mcpp::config::write_default_toolchain(**cfgR, pins::kFirstRunWinGnu))
              (*cfgR)->defaultToolchain = std::string(pins::kFirstRunWinGnu);
          if (mcpp::config::write_default_target(**cfgR, overrides.target_triple))
              (*cfgR)->defaultTarget = overrides.target_triple;

          tcSpec   = std::string(pins::kFirstRunWinGnu);
          tcOrigin = TcOrigin::FirstRun;
          auto redetected = mcpp::toolchain::detect(
              explicit_compiler, runtimePayload,
              runtimeBindingSnapshot.contractHash);
          if (!redetected) return std::unexpected(redetected.error().message);
          tc = std::move(*redetected);
      }

      // For musl-gcc the toolchain is fully self-contained
      // (`<root>/x86_64-linux-musl/{include,lib}` is its own sysroot).
      // musl-gcc's `-dumpmachine` reports `x86_64-linux-musl`.
      bool isMuslTc = mcpp::toolchain::is_musl_target(*tc);

      // A musl toolchain only really makes sense with static linkage —
      // dynamic-musl binaries depend on a system /lib/ld-musl-x86_64.so.1
      // that most distros don't ship. Default linkage to "static" when
      // the resolved toolchain is musl, unless the user has already opted
      // out via `--static` or [target.<triple>].linkage. (There is no
      // [build].linkage — the parser only reads it under a target section.)
      if (isMuslTc && m->buildConfig.linkage.empty()) {
          m->buildConfig.linkage = "static";
      }
    return {};
    };

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
        // A HOST TOOLCHAIN'S C LIBRARY IS THE PAYLOAD'S, WHATEVER THE
        // PROJECT'S TARGET SIDE IS.
        //
        // `build.mcpp` is compiled AND RUN on the machine doing the build. Its
        // C library therefore comes from the compiler payload — even for a
        // project whose TARGET takes its C library from the dependency graph.
        // The two are different machines and this function's whole job is to
        // keep them apart.
        //
        // AND THE NATIVE BRANCH BELOW RETURNS THE MAIN `tc`, WHICH CARRIES
        // THE OTHER ANSWER. `build_program.cppm`'s own header states the
        // invariant — "`tc` is always a HOST-targeting toolchain" — and for
        // every field but this one the native branch satisfied it, because on a
        // native build the compiler IS the host compiler. `cAbiPrebuilt` is the
        // first field where "same compiler" and "same target side" come apart.
        //
        // AN INVARIANT, NOT A BUG FIX FOR ANY MEASURED FAILURE. It was
        // written while chasing a `features.h: No such file` on openkal-musl's
        // CI and it is NOT that failure's cause: measured on `origin/main` and
        // on this branch, the gcc std module carries zero `-isystem`/
        // `-idirafter` rows either way — that toolchain reaches its C library
        // through the specs the post-install fixup rewrites, and the real
        // defect was in resolving WHICH glibc payload those specs name.
        //
        // Kept because the invariant is worth being true: a helper compiled and
        // run on the build machine must not inherit the target's C-library
        // origin, and the next field that comes apart would find no rule here.
        //
        // ⇒ Stated once, so every consumer (the std module build,
        // `host_base_flags`) gets it without asking.
        auto as_host = [](mcpp::toolchain::Toolchain t) {
            t.cAbiPrebuilt = true;
            return t;
        };
        // `explicit_compiler` IS EMPTY FOR ONE RESOLUTION PATH, AND THIS IS
        // THE ONLY CALLER THAT NOTICED BY CRASHING (#527).
        //
        // Every branch that resolves a toolchain from the index assigns
        // `explicit_compiler`; the `[toolchain] system` branch does not, because
        // it has nothing to assign yet — `detect` finds the PATH compiler a few
        // hundred lines below and stores the resolved ABSOLUTE path in
        // `tc->binaryPath`. The main build reads the compiler from `tc` and is
        // fine; this closure returned the local variable and handed "" to
        // `posix_spawnp`, which is `exit 127: posix_spawnp('') failed`.
        //
        // AND THE FIX IS NOT "SUPPORT THE HOST". `tc->binaryPath` is the
        // compiler this build is ALREADY using for every other translation
        // unit; build.mcpp is compiled with the project's toolchain by
        // definition (see this lambda's header). Reading it from the place it
        // was resolved makes the two paths agree — it grants no capability the
        // project did not already have, and the host-dependence warning at the
        // `system` branch is what states the cost.
        //
        // The CROSS branch below is a different question and deliberately
        // unchanged: there `explicit_compiler` is empty because NO host
        // toolchain was resolved at all, and its classified refusal is correct.
        if (overrides.target_triple.empty())
            return std::pair{
                explicit_compiler.empty() ? tc->binaryPath : explicit_compiler,
                as_host(*tc)};
        if (hostTcCache)
            return std::pair{hostTcCache->first, as_host(hostTcCache->second)};
        if (!tcSpec || *tcSpec == "system" || tcSpecIsMsvc) {
            // A READABLE REFUSAL THAT HAD NO CODE, so the target matrix
            // recorded four identical `other` cells for it. The sentence was
            // right; the classification was missing. Measured on windows-2022
            // with `msvc@system` declared and any cross target.
            refusal::record(refusal::Code::HostToolToolchain);
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
        if (auto fixed = mcpp::toolchain::ensure_post_install_fixup(
                **cfgH, payload->root, pkg,
                runtimeBindingSnapshot.runtimeId, runtimeLibDir); !fixed)
            return std::unexpected(std::format(
                "host toolchain post-install fixup: {}", fixed.error()));
        else report_fixup(*fixed, payload->root);
        auto htc = mcpp::toolchain::detect(frontend);
        if (!htc) return std::unexpected(htc.error().message);
        mcpp::ui::info("Resolved", std::format(
            "host toolchain for build.mcpp: {}", htc->label()));
        hostTcCache = std::pair{frontend, *htc};
        return std::pair{hostTcCache->first, as_host(hostTcCache->second)};
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

    // Refresh the builtin package index only when a dependency cannot be
    // resolved from the local copy (#315).
    //
    // This used to fire whenever the refresh marker was older than an hour,
    // whether or not anything was actually missing — so every build with a
    // registry dependency paid a multi-repo network sync once an hour, which is
    // minutes on a slow or blocked network for data it already had. The policy
    // now lives in mcpp.pm.index_refresh and is shared with `mcpp add` and the
    // xim install gate, which had each derived their own (and disagreed).
    //
    // Nothing here decides anything itself — in particular the "a miss proves
    // nothing for this namespace" rule must not be re-derived; see that module.
    if (!m->dependencies.empty()) {
        if (auto cfg2 = get_cfg()) {
            auto xlEnv  = mcpp::config::make_xlings_env(**cfg2);
            auto policy = mcpp::pm::policy_for(**cfg2);
            // Same routing the dependency walk below uses (the `index_route`
            // lambda is declared further down; this is the identical value).
            mcpp::pm::IndexRoute route{ &m->indices, *root, *cfg2 };
            for (auto& [depName, spec] : m->dependencies) {
                auto decision = mcpp::pm::decide_for_dependency(
                    route, depName, spec, xlEnv, targetPlatform, policy);
                if (!decision.shouldRefresh) {
                    mcpp::log::verbose("index", std::format(
                        "{}: {}", decision.subject,
                        mcpp::pm::reason_text(decision.reason)));
                    continue;
                }
                // A failed refresh is not a failed build: the dependency walk
                // below may still resolve everything from what is on disk, and
                // if it cannot, it reports the actual missing package with the
                // index's age attached. Failing here instead would turn a
                // transient network blip into a hard stop for a build that
                // needed no network at all.
                if (auto r = mcpp::pm::apply(decision, xlEnv); !r)
                    mcpp::ui::warning(r.error());
                break;   // one sync covers every dependency
            }
        }
    }

    // Set up project-level .mcpp/ directory for custom indices and/or the
    // [xlings] build environment (L-1). This creates .mcpp/.xlings.json with
    // custom non-builtin index entries (so xlings can clone them) plus the
    // [xlings] deps/workspace/subos/envs materialized verbatim.
    const auto& runtimeOwnerManifest = wsManifest ? *wsManifest : *m;
    // The TARGET's C library, if this target has one. Resolved here and not by
    // any package, for the same reason the compiler pin is: it is a property
    // of the target.
    //
    // It rides the SAME channel as `[xlings] deps` rather than getting an
    // install path of its own — one materialization, one place that can be
    // wrong. What it must NOT do is depend on the project having an `[xlings]`
    // section: a bare-metal project written to the template has none, and the
    // whole point is that it never mentions a libc.
    // FROM THE REQUESTED TRIPLE, NOT FROM THE TOOLCHAIN — AND THE TWO WERE
    // THE SAME VALUE ALL ALONG.
    //
    // This read of `tc->targetTriple` was the ONLY thing tying the compiler's
    // resolution to a point before dependency resolution, and it never wanted
    // the compiler: `tc->targetTriple` is corrected to the requested triple a
    // few lines after the toolchain is detected, so the value here is the one
    // `--target` named. Taking it from the request instead lets the toolchain
    // be resolved where the information it needs actually exists.
    std::string targetSysroot;
    {
        auto tt = overrides.target_triple.empty()
            ? std::optional{mcpp::toolchain::triple::host_triple()}
            : mcpp::toolchain::triple::parse(overrides.target_triple);
        if (tt)
            targetSysroot = mcpp::toolchain::triple::effective_sysroot(
                *tt, sysroot_override(*m, *tt));
    }
    const bool materializeRootRuntime =
        !overrides.inherited_runtime_binding
        && (!runtimeOwnerManifest.xlings.empty() || !targetSysroot.empty());
    if (!m->indices.empty() || materializeRootRuntime) {
        auto cfg2 = get_cfg();
        if (cfg2) {
            mcpp::xlings::ProjectEnv penv;
            if (materializeRootRuntime) {
                penv.deps  = runtimeOwnerManifest.xlings.deps;
                // Appended, never substituted: a project may legitimately
                // declare other xim packages, and a target sysroot is one more
                // entry rather than a replacement for the list. Deduplicated
                // because a manifest written before this axis existed still
                // names it, and declaring it twice is not an error the author
                // should have to hear about.
                if (!targetSysroot.empty()
                    && std::ranges::find(penv.deps, targetSysroot) == penv.deps.end())
                    penv.deps.push_back(targetSysroot);
                penv.subos = runtimeOwnerManifest.xlings.subos;
                for (auto const& [k, v] : runtimeOwnerManifest.xlings.workspace)
                    penv.workspace.emplace_back(k, v);
                // `[feature-xlings.<f>]` becomes part of the project's
                // environment only while `<f>` is active. It is written into
                // the same two fields, because from xlings' side there is no
                // such thing as a feature: the file states what this project
                // uses, and the feature decided that.
                for (auto const& f :
                         feature_closure(runtimeOwnerManifest,
                                         parse_feature_request(overrides.features)))
                    if (auto it = runtimeOwnerManifest.xlings.featureDeps.find(f);
                        it != runtimeOwnerManifest.xlings.featureDeps.end())
                        for (auto const& address : it->second) {
                            if (std::ranges::find(penv.deps, address) == penv.deps.end())
                                penv.deps.push_back(address);
                            const auto entry =
                                mcpp::manifest::parse_address(address);
                            if (std::ranges::none_of(penv.workspace,
                                    [&](auto const& kv) { return kv.first == entry.target; }))
                                penv.workspace.emplace_back(entry.target, entry.pin());
                        }
            }
            if (runtimeSelection.ownerRoot == workRoot) {
                mcpp::config::ensure_project_index_dir(
                    **cfg2, workRoot, m->indices, penv);
            } else {
                if (!m->indices.empty())
                    mcpp::config::ensure_project_index_dir(
                        **cfg2, workRoot, m->indices, {});
                if (materializeRootRuntime)
                    mcpp::config::ensure_project_index_dir(
                        **cfg2, runtimeSelection.ownerRoot, {}, penv);
            }

            // `[xlings] deps` are DECLARED above and, until now, nothing
            // installed them (mcpp-index #281 §9).
            //
            // `ensure_project_index_dir` writes them into `.mcpp/.xlings.json`
            // verbatim and stops there, so a manifest saying
            // `deps = ["xim:mesa"]` produced a file naming mesa, no project
            // SubOS, and `fatal error: gbm.h: No such file or directory`. The
            // declaration looked accepted and did nothing — which is the worst
            // shape a config key can have.
            //
            // This is the same "declare it and mcpp provisions it on first use"
            // contract `[toolchain]` has had all along; that path is a few
            // hundred lines up ("First run — no toolchain configured …
            // installing … as default"). A build environment should not have
            // two grades of declaration.
            //
            // ORDER IS LOAD-BEARING: this must run BEFORE the runtime binding
            // resolves, because a named `[xlings] subos` that does not exist
            // yet is a hard error ("selected SubOS '…' does not exist;
            // create/bootstrap that environment"), and provisioning is what
            // creates it. Placed here, next to the index sync below, both
            // first-use provisioning steps sit in one place.
            //
            // `install_packages` rather than `fetcher.install`: the install
            // DESTINATION is chosen by package scope (project vs global), and
            // the project scope is what materializes the project SubOS. It also
            // carries the live progress UI and captured child errors, matching
            // the toolchain and custom-index paths.
            // Only what the MANIFEST declared, deliberately not `penv.deps`.
            //
            // A cross-compilation target sysroot is APPENDED to that list a few
            // lines up, and provisioning it here would change behaviour for
            // projects that never asked for it: a name that does not resolve
            // would turn a build that used to proceed into a hard failure. The
            // contract being added is "what you declared gets installed", and
            // the sysroot entry is mcpp's own inference rather than the
            // author's declaration.
            // Only the tiers this verb needs, and only what the ROOT
            // declared. The graph's own declarations are provisioned after
            // resolution, which is the first moment they are known — see the
            // second pass near `xlingsDepBinDirs`.
            const auto declaredDeps = applicable_xlings_addresses(
                runtimeOwnerManifest,
                feature_closure(runtimeOwnerManifest,
                                parse_feature_request(overrides.features)),
                toolPurpose, /*isRoot=*/true);
            if (materializeRootRuntime && !declaredDeps.empty()) {
                if (auto pv = provision_xlings_addresses(
                        **cfg2, declaredDeps, runtimeSelection.ownerRoot,
                        "[xlings.workspace] entries");
                    !pv) return std::unexpected(pv.error());
            }

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
    // The features each package ends up built with, index-aligned with
    // `packages`. Recorded at activation because the passes that run after it
    // — `[feature-xlings]` provisioning among them — otherwise have no way to
    // ask, and re-deriving it there would be a second copy of the aggregation
    // rule.
    std::vector<std::vector<std::string>> activeFeaturesByPackage;
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
        // "version" | "path" | "git". Only "version" is cacheable: an index
        // package's payload lives in the immutable xpkgs store under a
        // version-keyed directory, so name@version identifies its sources.
        // Path and git checkouts can change under an unchanged identity.
        std::string sourceKind;
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
        // Reached ONLY through [dev-dependencies]. mcpp.lock excludes these:
        // dev-deps are resolved under `mcpp test` and not under `mcpp build`, so
        // recording them makes a VCS-committed file depend on which command ran
        // last and ping-pong between the two. The lock must be a function of the
        // MANIFEST, not of the command. Cleared the moment a non-dev consumer
        // asks for the same package.
        bool        devOnly = false;
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
        bool                                 devOnly = false;     // seeded from [dev-dependencies]; inherited by children
        // Seeded from `[build-dependencies]`, and inherited by children the
        // same way `devOnly` is. It answers "does this serve the build or the
        // target", which is a different question from "which build-time
        // product do I want" — that one is answered per edge by `tools` and
        // `host-module`, and the two are orthogonal. A package linked into the
        // target that also provides a tool is written once, in
        // `[dependencies]`, with a `tools` request on it.
        bool                                 buildOnly = false;
    };
    std::deque<WorkItem> worklist;

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
        // 0.0.10+: use structured namespace from DependencySpec. The route (not
        // a bare Fetcher) is what reaches a descriptor served by a project
        // `[indices]` entry — see #308.
        auto resolved = mcpp::pm::resolve_semver(
            s.namespace_, s.shortName.empty() ? depName : s.shortName,
            s.version, index_route(*cfg), targetPlatform);
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
        [&](const mcpp::pm::DependencyCoordinate& coord,
            std::string_view luaContent,
            bool allowLegacyBareDefault) {
            // Single source of truth: the descriptor identity gate lives in
            // mcpp.manifest and is shared with the read_xpkg_lua family.  A
            // descriptor served by a declared project index inherits that
            // index's namespace when package.namespace is omitted; preserve
            // the same owner context during this second, stricter check.
            const auto route = index_route();
            const auto* owner = route.find_for_ns(coord.namespace_);
            const std::string_view ownerNs = owner
                ? std::string_view{owner->name} : std::string_view{};
            return mcpp::manifest::xpkg_lua_identity_matches(
                luaContent, coord.namespace_, coord.shortName,
                allowLegacyBareDefault, ownerNs);
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

    std::set<std::string> selectorMigrationWarnings;

    auto selectDependencyCandidate =
        [&](mcpp::manifest::DependencySpec& spec,
            const std::string& depName) -> std::expected<void, std::string>
    {
        auto candidates = dependencyCoordinates(spec, depName);
        if (candidates.empty()) {
            return std::unexpected(
                with_index_cause(std::format(
                    "dependency '{}' has no lookup candidates", depName)));
        }

        // One release train of migration support for the former dotted
        // candidate search. A lockfile records the identity an existing
        // project already selected, so keep that identity stable until the
        // user rewrites the selector explicitly. Without a lock anchor, never
        // fall back: only diagnose a valid old-primary package and continue
        // with the new exact coordinate.
        if (spec.legacyCandidateSearch) {
            const auto exact = candidates.front();
            bool lockExpressesIntent = false;
            if (auto locked = packageIdentityLockAnchors.find(depName);
                locked != packageIdentityLockAnchors.end()) {
                lockExpressesIntent = true;
                if (locked->second != exact.namespace_) {
                    mcpp::pm::DependencyCoordinate lockedCoordinate{
                        .namespace_ = locked->second,
                        .shortName = exact.shortName,
                    };
                    if (selectorMigrationWarnings.insert(depName).second) {
                        mcpp::ui::warning(std::format(
                            "dependency selector '{}' now means exact package "
                            "'{}', but mcpp.lock records '{}'; keeping the "
                            "locked identity for this migration release. "
                            "Write '{}' to keep it explicitly, or remove the "
                            "lock and keep '{}' to migrate",
                            depName,
                            mcpp::pm::format_package_selector(exact),
                            mcpp::pm::format_package_selector(lockedCoordinate),
                            mcpp::pm::format_package_selector(lockedCoordinate),
                            mcpp::pm::format_package_selector(exact)));
                    }
                    candidates.assign(1, std::move(lockedCoordinate));
                }
            }

            if (!lockExpressesIntent && spec.isVersion()) {
                if (auto old = mcpp::pm::legacy_prefixed_coordinate(exact)) {
                    auto oldLua = readStrictLuaForCandidate(*old);
                    if (oldLua && xpkgLuaMatchesCandidate(
                            *old, *oldLua,
                            /*allowLegacyBareDefault=*/false)
                        && selectorMigrationWarnings.insert(depName).second) {
                        mcpp::ui::warning(std::format(
                            "dependency selector '{}' now resolves exactly to "
                            "'{}'; an older mcpp would select the existing "
                            "package '{}'. Write '{}' to keep the old identity "
                            "or keep '{}' for the new exact identity",
                            depName,
                            mcpp::pm::format_package_selector(exact),
                            mcpp::pm::format_package_selector(*old),
                            mcpp::pm::format_package_selector(*old),
                            mcpp::pm::format_package_selector(exact)));
                    }
                }
            }
        }

        auto selected = candidates.front();
        bool matched  = false;
        if (spec.isVersion()) {
            for (auto& candidate : candidates) {
                auto lua = readStrictLuaForCandidate(candidate);
                if (!lua) continue;
                if (auto violation = mcpp::manifest::
                        xpkg_name_form_violation_from_lua(*lua)) {
                    return std::unexpected(std::format(
                        "dependency '{}': {}", depName, *violation));
                }
                if (!xpkgLuaMatchesCandidate(
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

            // One-release bare-name migration. Namespace omission means exactly
            // `mcpplibs`, but every published `compat.*` package and every user
            // manifest written before this release spells the dependency bare —
            // `gtest = "1.15.2"`, `ftxui = "6.1.9"`. Making that an immediate
            // hard error means an mcpp upgrade breaks builds against data that
            // is already published and cannot be edited retroactively; the
            // symmetric rule ("published data must not break the program") is
            // why the index floor degrades instead of bricking.
            //
            // The defect #278 removed was the SILENCE, not the reach: mcpp used
            // to continue with a namespace the user never wrote and never say
            // so. A hit here is announced, is recorded downstream under its
            // canonical identity, and names the exact edit that removes the
            // warning. Only a selector whose namespace was OMITTED is eligible —
            // `mcpplibs.gtest` states an identity and must still miss.
            if (!matched && spec.isVersion() && spec.namespaceOmitted) {
                for (auto& legacy :
                         mcpp::pm::legacy_bare_candidates(candidates.front())) {
                    auto lua = readStrictLuaForCandidate(legacy);
                    if (!lua) continue;
                    if (mcpp::manifest::xpkg_name_form_violation_from_lua(*lua))
                        continue;
                    if (!xpkgLuaMatchesCandidate(
                            legacy, *lua, /*allowLegacyBareDefault=*/false))
                        continue;
                    auto declaredNs =
                        mcpp::manifest::extract_xpkg_namespace(*lua);
                    // Same narrowing as the exact loop: the namespace-less rung
                    // is "upstream package that declares no namespace", not a
                    // cross-namespace wildcard.
                    if (legacy.namespace_.empty() && !declaredNs.empty())
                        continue;

                    selected = legacy;
                    if (selected.namespace_.empty())
                        selected.namespace_ = declaredNs;
                    matched = true;
                    // Downstream — lock, install, cache label — must see the
                    // canonical identity, so the ambiguous spelling survives in
                    // exactly one place: the user's manifest, until they edit it.
                    candidates.assign(1, selected);

                    if (selectorMigrationWarnings.insert(depName).second) {
                        mcpp::ui::warning(std::format(
                            "dependency '{}' resolved to '{}' through the "
                            "deprecated bare-name search; namespace omission "
                            "means `{}` only. Write the exact package:"
                            "\n    [dependencies.{}]"
                            "\n    {} = \"{}\""
                            "\n  (or run `mcpp add {}@{}`). This fallback is "
                            "removed in {}.",
                            depName,
                            mcpp::pm::format_package_selector(selected),
                            mcpp::pm::kDefaultNamespace,
                            selected.namespace_, selected.shortName,
                            spec.version,
                            mcpp::pm::format_package_selector(selected),
                            spec.version,
                            mcpp::pm::kBareNameFallbackRemovedIn));
                    }
                    break;
                }
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

            // An exact coordinate that a readable index cannot serve fails at
            // resolution. Never carry it into install-time compatibility
            // retries, which would reintroduce cross-namespace guessing.
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
                // error string (see Fetcher::scan_short_name_matches).
                std::string hint;
                if (auto cfg = get_cfg()) {
                    auto suggestions = mcpp::pm::cross_namespace_suggestions(
                        index_route(*cfg), candidates.front().shortName);
                    if (!suggestions.empty()) {
                        hint += "\n  a package with this name exists under "
                                "another namespace:";
                        for (auto& suggestion : suggestions)
                            hint += "\n    " + suggestion.fqn
                                  + suggestion.versions_label();
                        if (auto suggested = mcpp::pm::parse_package_selector(
                                suggestions.front().fqn); suggested
                            && suggested->namespace_) {
                            hint += std::format(
                                "\n  namespace omission means `{}`. write the "
                                "exact package:"
                                "\n    [dependencies.{}]"
                                "\n    {} = \"{}\"",
                                mcpp::pm::kDefaultNamespace,
                                *suggested->namespace_, suggested->name,
                                spec.version.empty() ? "<version>"
                                                     : spec.version);
                        }
                    }
                }

                // Advisory, never a gate (#315): now that a build only refreshes
                // the index on a miss, "not found" and "your copy of the index
                // is from last month" are easy to confuse. State which index
                // answered and how old it is, so the next step is obvious
                // instead of guessed at.
                if (auto cfgA = get_cfg()) {
                    hint += std::format("\n  index: {}\n  hint: `mcpp index update` "
                                        "if it was published recently",
                        mcpp::pm::staleness_note(
                            mcpp::config::make_xlings_env(**cfgA)));
                }
                return std::unexpected(with_index_cause(std::format(
                    "dependency '{}': no package found for exact selector"
                    "\n  tried: {}{}",
                    depName, tried, hint)));
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
            return std::unexpected(with_index_cause(std::format(
                "dependency '{}': not found in local index at '{}'",
                depName, indexPath.string())));
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
                    *luaContent, shortName, version, targetPlatform);
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
                        *luaContent, shortName, version, targetPlatform);
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

            // Offline (#315). Checked HERE, at the point of download, and not
            // any earlier: everything above this line — reading descriptors,
            // resolving versions, reusing an already-installed package — is
            // local, and an offline build that has its dependencies must
            // succeed. Only the download itself is refused, and it names the
            // package rather than surfacing a socket error from three layers
            // down. (The toolchain payload path has its own gate; this is the
            // dependency path, which does not go through resolve_xpkg_path.)
            if (mcpp::platform::env::offline_mode()) {
                return std::unexpected(std::format(
                    "offline mode: dependency '{}' v{} is not installed and "
                    "cannot be downloaded\n"
                    "       run without --offline (or unset MCPP_OFFLINE) to fetch it",
                    displayName, version));
            }
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
                        ? (workRoot / ".mcpp")
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
        if (!luaContent) return std::unexpected(with_index_cause(std::format(
            "dependency '{}': index entry not found in local clone", depName)));
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
                *luaContent, shortName, version, targetPlatform);
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
            // Name the directory actually searched. `<verdir>` was a literal
            // placeholder, so the message could not distinguish "the package
            // is Form B and you forgot the mcpp field" from "the verdir mcpp
            // resolved is not this package's at all" — the second is what a
            // cross-namespace install_path hit produces, and it sent this
            // investigation down the wrong path for a while.
            if (matches.empty()) return std::unexpected(std::format(
                "dependency '{}': index entry has no `mcpp = ...` field, "
                "and no mcpp.toml was found at '{}/mcpp.toml' or "
                "'{}/*/mcpp.toml' — add an explicit `mcpp = \"<path>\"` "
                "or `mcpp = {{ ... }}` block to the .lua descriptor. "
                "(If that directory belongs to a DIFFERENT package, the "
                "install step resolved the wrong verdir.)",
                depName, verRoot.string(), verRoot.string()));
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
            merge_conditional_config(*manifest,
                                    cfgCtx());
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
        // #355: HOST tools this consumer asked the dependency for. Aggregated
        // off the edge graph exactly like requestedFeatures — a transitive
        // consumer's request must not be silently dropped, which is the
        // #242/#243 failure shape.
        std::vector<std::string> requestedTools;
        // #355 step 5 / #359: does this edge ask for the dependency's lib-root
        // interface as a HOST module, and does it hand its build-time
        // provisions on to this consumer's own consumers?
        bool hostModule = false;
        bool reexport = false;
        // Did this edge come from `[build-dependencies]`? Such an edge serves
        // the BUILD and never the target, and the property is inherited by
        // everything reachable through it. It is a property of the edge and
        // not of the package: the same package may be an ordinary dependency
        // of someone else in the same build, and then it does reach the
        // target.
        bool buildOnly = false;
    };
    std::vector<DependencyEdge> dependencyEdges;
    namespace dg = mcpp::build::dep_graph;
    // #355: consumer package index → (env var, absolute path) for each host
    // tool that consumer requested. Filled by the provisioning pass below;
    // read by BOTH build.mcpp call sites (the dependency loop and the root),
    // which is why it lives out here rather than inside the resolution block.
    std::map<std::size_t, std::vector<std::pair<std::string, std::string>>>
        toolEnvByConsumer;
    // #355 step 5: consumer package index → (logical module name, interface
    // path) for each dependency that offers HOST build rules. Same fan-out
    // shape as toolEnvByConsumer, and read by the same two call sites.
    std::map<std::size_t,
             std::vector<mcpp::build::BuildProgramEnv::HostModuleRef>>
        hostModulesByConsumer;
    // #359: who can see which build-time provision. Computed once by the
    // provisioning pass below (a fixpoint over `dependencyEdges`, the same
    // shape as computeUsageRequirements) and read by every consumer of the
    // three env channels above. Declared here because `fillDepDirs` closes
    // over it and is defined long before the pass runs; every call site is
    // after it.
    namespace prov = mcpp::build::provisions;
    prov::Propagation provisionGraph;
    // The spellings a given consumer may address a provider by. The qualified
    // name always works; the bare tail only when the namespace ladder binds it
    // to exactly this package FOR THIS CONSUMER. Scoped per consumer rather
    // than globally because two packages sharing a tail only collide inside an
    // environment that contains both.
    auto bareBindingsFor = [&](std::size_t consumer) {
        std::vector<std::string> fqns;
        if (consumer < provisionGraph.visible.size())
            for (auto const& pr : provisionGraph.visible[consumer]) {
                if (pr.provider >= packages.size()) continue;
                auto const& n = packages[pr.provider].manifest.package.name;
                if (std::find(fqns.begin(), fqns.end(), n) == fqns.end())
                    fqns.push_back(n);
            }
        return prov::bind_bare_names(fqns);
    };

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

    // "Which compile-visible channels a build.mcpp directive lands in" is a
    // property of the DIRECTIVE TABLE, not of this call site, so both the mark
    // and the fold now live with the table in mcpp.build.directives. This pair
    // used to be defined here and was already incomplete — the comment it
    // replaced admitted that link/source residues stayed at the call sites,
    // which is the #242 two-derivations shape.
    //
    // The fold is PRIVATE by design (Cargo discipline — a build-time program
    // must not widen the package's public interface): privateBuild only, never
    // publicUsage. The after-dirs ride the typed #249 channel, which owns the
    // per-dialect degradations (cl.exe /I, NASM -I).
    using DirectiveMark = mcpp::build::directives::Mark;
    auto markDirectiveTail = [](const mcpp::manifest::Manifest& mm) {
        return mcpp::build::directives::mark(mm);
    };
    auto foldDirectiveTailIntoPrivateBuild =
        [](auto& pkg, const mcpp::manifest::Manifest& ran,
           const DirectiveMark& t)
    {
        mcpp::build::directives::fold_private_tail(pkg.privateBuild, ran, t);
    };

    // mcpp#241: the (name → dir) pairs a package's build.mcpp receives as
    // MCPP_DEP_<NAME>_DIR. ONE owner: the dependency loop and the root call
    // site had drifted into two near-identical copies of this, and #355 was
    // about to add a third. Each dependency is emitted under BOTH its
    // canonical name and its namespace-stripped tail, so
    // `mcpp::dep_dir("compat.zlib")` and `mcpp::dep_dir("zlib")` both resolve
    // regardless of which spelling the author used in `deps`.
    //
    // #359: the set is now the consumer's VISIBLE provisions rather than its
    // direct edges, so a re-exported dependency's directory reaches it too.
    // That is what makes a rule package able to find data files belonging to a
    // dependency the user never declared — protoc's well-known .proto files
    // are exactly such a directory, and `grpcgen` reads them through dep_dir.
    //
    // The bare tail is emitted only when the namespace ladder binds it here.
    // Emitting it unconditionally was safe while only the root's own
    // declarations reached build.mcpp; with re-export, two packages that never
    // heard of each other can share a tail and the later emplace_back would
    // silently win.
    // The xlings half of fillDepDirs. Same question ("where did my declared
    // dependency's payload land"), different namespace and store layout, so it
    // cannot ride the mcpp dependency channel — but it must be an INTERFACE on
    // the build.mcpp side for the same reason that one is: a program that
    // reconstructs the store path is coupled to internals mcpp is free to
    // change. See mcpp::build::hostprogram::xpkg_dir.
    // Which dependency supplied the runner, for the exactly-one-provider
    // error below. A name rather than a bool: the message has to name both.
    std::string runnerProvider;
    // ONE PROVIDER PER RUNNER NAME. `runner` has had this rule since #544;
    // a NAMED runner inherits it per name, because a board may legitimately
    // supply `flash` while a different package supplies `monitor`.
    std::map<std::string, std::string> namedRunnerProvider;

    auto fillXpkgDirs = [&](mcpp::build::BuildProgramEnv& e,
                            const mcpp::manifest::Manifest& owner) {
        // `[feature-xlings.<f>]` is provisioned when `<f>` is active, so it has
        // to be answerable here too. Before this, a tool a feature declared was
        // downloaded and installed and then `mcpp::xpkg_dir` returned "" for it
        // — the build program was told to declare a package it had already
        // declared, which is a diagnostic pointing at the wrong file.
        //
        // The set is taken from the SAME env the caller already computed, so
        // "which features are on" is answered once. Installation stays the
        // filter below: a declared address whose payload is absent answers "",
        // which is what a `when = "dev"` entry looks like to a consumer.
        std::vector<std::string> declared = owner.xlings.deps;
        for (auto const& f : e.features)
            if (auto it = owner.xlings.featureDeps.find(f);
                it != owner.xlings.featureDeps.end())
                for (auto const& address : it->second)
                    if (std::ranges::find(declared, address) == declared.end())
                        declared.push_back(address);
        if (declared.empty()) return;
        auto cfg = get_cfg();
        if (!cfg) return;
        auto xlEnv = mcpp::config::make_xlings_env(**cfg);
        for (auto const& spec : declared) {
            auto ref = mcpp::xlings::paths::parse_xpkg_ref(spec);
            auto dir = mcpp::xlings::paths::xpkg_payload(xlEnv, ref);
            if (!dir) continue;   // declared but not installed: "" is the answer
            // Namespaced first — it is the exact spelling, and the bare form
            // below must not shadow it (the receiver keeps the first value it
            // is given for a name).
            e.xpkgDirs.emplace_back(
                mcpp::build::xpkg_env_var(ref.ns, ref.name), dir->string());
            e.xpkgDirs.emplace_back(
                mcpp::build::xpkg_env_var("", ref.name), dir->string());
        }
    };

    auto fillDepDirs = [&](mcpp::build::BuildProgramEnv& e, std::size_t consumer) {
        if (consumer >= provisionGraph.visible.size()) return;
        auto bind = bareBindingsFor(consumer);
        for (auto const& [tail, b] : bind) {
            if (auto note = prov::contest_note(tail, b); !note.empty())
                mcpp::diag::warning("provisions/ambiguous", note);
        }
        for (auto const& pr : provisionGraph.visible[consumer]) {
            if (pr.kind != prov::Kind::DepDir) continue;
            if (pr.provider >= packages.size()) continue;
            auto const& depPkg = packages[pr.provider];
            auto const& canon  = depPkg.manifest.package.name;
            e.depDirs.emplace_back(canon, depPkg.root);
            auto tail = prov::tail_of(canon);
            if (tail == canon) continue;
            auto it = bind.find(tail);
            if (it != bind.end() && it->second.owner == canon)
                e.depDirs.emplace_back(tail, depPkg.root);
        }
    };

    // A declared build-graph node's Source outputs must be visible to the
    // scan, so they are materialized as placeholders and joined to the source
    // set here — the same two lists `generated=` feeds, for the same reason
    // (the scanner walks the legacy modules.sources mirror). ninja overwrites
    // the placeholder before the compile edge runs, because that compile
    // depends on the action's output.
    auto adoptActionOutputs = [](mcpp::manifest::Manifest& mm,
                                 const std::filesystem::path& pkgRoot,
                                 std::size_t firstNewAction) {
        if (firstNewAction >= mm.buildConfig.actions.size()) return;
        std::vector<mcpp::manifest::BuildAction> fresh(
            mm.buildConfig.actions.begin()
                + static_cast<std::ptrdiff_t>(firstNewAction),
            mm.buildConfig.actions.end());
        // The package that DECLARED the outputs classifies them: a dependency
        // generating a `.ixx` asks its own manifest, not the root project's.
        // Built once per package, not once per output — and BEFORE
        // `prepare_actions`, which needs the same table to decide which
        // outputs get a placeholder (a header does not; see mcpp#534).
        const auto pkgExtTable =
            mcpp::extension_table_for(mm.buildConfig.moduleExtensions);
        mcpp::build::directives::prepare_actions(fresh, pkgRoot, pkgExtTable);
        std::copy(fresh.begin(), fresh.end(),
                  mm.buildConfig.actions.begin()
                      + static_cast<std::ptrdiff_t>(firstNewAction));
        for (auto const& a : fresh) {
            if (a.role != mcpp::manifest::BuildAction::Role::Source) continue;
            for (auto const& o : a.outputs) {
                if (o.find("${mcpp.") != std::string::npos) continue;
                // Companion outputs (protoc's .pb.h next to its .pb.cc) are
                // produced by the edge but are NOT translation units.
                if (!mcpp::build::directives::is_compilable_output(o, pkgExtTable))
                    continue;
                mm.buildConfig.sources.push_back(o);
                mm.modules.sources.push_back(o);
            }
        }
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
                // Native spelling: a TOML `C:/SDL2/include` stays mixed on
                // MSVC and leaks into the CDB's -I otherwise. Direct
                // make_preferred — no generic_string round trip, which can
                // throw for names the ANSI codepage cannot spell (mcpp#230).
                auto n = inc;
                n.make_preferred();
                appendUniquePath(dirs, std::move(n));
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
                auto n = inc;
                n.make_preferred();
                appendUniquePath(dirs, std::move(n));
                continue;
            }
            for (auto& dir : mcpp::modgraph::expand_dir_glob(
                     packageRoot, inc.generic_string())) {
                appendUniquePath(dirs, dir);
            }
        }
        return dirs;
    };

    // The same expansion for `private_include_dirs`, so a private entry may be
    // a glob and still name exactly the directories it expands to.
    auto expandPrivateIncludeDirs =
        [&](const std::filesystem::path& packageRoot,
            const mcpp::manifest::Manifest& manifest)
    {
        std::vector<std::filesystem::path> dirs;
        for (auto const& inc : manifest.buildConfig.privateIncludeDirs) {
            if (inc.is_absolute()) {
                auto n = inc;
                n.make_preferred();
                appendUniquePath(dirs, std::move(n));
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
            const mcpp::manifest::Manifest& manifestIn)
    {
        // `[workspace.build]` APPLIES TO EVERY MEMBER, INCLUDING ONE REACHED AS
        // ANOTHER MEMBER'S `path` DEPENDENCY — WHICH IS THE ORDINARY SHAPE.
        //
        // Inheritance runs where the command's own manifest is loaded, so
        // `mcpp build -p appb` gave `appb` the workspace flags and gave `liba`
        // none, even though `liba` is a member of the same workspace and is
        // being compiled by the same command. Measured before this: `-DWS_FLAG`
        // on the consumer's TUs and not on the sibling's.
        //
        // `[workspace.package] standard` did not have the problem, because the
        // standard is imposed graph-wide from the root for BMI-compatibility
        // reasons — which is exactly why the gap was invisible until a
        // `[build]` flag was inheritable too.
        //
        // Applied HERE because this is the one funnel both dependency-assembly
        // sites go through, and because the include directories a few lines
        // below are captured from the manifest at this moment: a later mutation
        // would reach the flags and silently not the include dirs.
        //
        // Only for MEMBERS. An index or git dependency is not part of the
        // workspace and must not acquire its flags.
        mcpp::manifest::Manifest manifest = manifestIn;
        if (wsManifest && !runtimeWorkspaceRoot.empty()
            && mcpp::project::is_workspace_member(*wsManifest,
                                                  runtimeWorkspaceRoot,
                                                  packageRoot)) {
            mcpp::project::inherit_workspace_build(manifest, *wsManifest,
                                                   runtimeWorkspaceRoot);
        }

        mcpp::modgraph::PackageRoot pkg;
        pkg.root = packageRoot;
        pkg.manifest = manifest;
        pkg.usageResolved = true;

        pkg.privateBuild.includeDirs = expandIncludeDirs(packageRoot, manifest);
        pkg.privateBuild.includeDirsAfter = expandIncludeDirsAfter(packageRoot, manifest);
        pkg.privateBuild.cflags = manifest.buildConfig.cflags;
        pkg.privateBuild.cxxflags = manifest.buildConfig.cxxflags;
        // NOT `= privateBuild` ANY MORE — a package may now say which of
        // its include directories stop at its own boundary.
        //
        // This line took the whole set for as long as the two were the same
        // set, which they are for almost every package. The one shape where
        // they are not is a package that vendors a library with an internal
        // header overlay: musl's `src/include` adds `hidden`, `weak` and
        // `weak_alias` for musl's own sources, and publishing it hands those
        // names to every consumer. See BuildInputs::privateIncludeDirs.
        //
        // THE FILTER IS APPLIED AFTER GLOB EXPANSION, so a private entry may
        // itself be a glob and still name exactly the directories it expands
        // to. Comparing the unexpanded spellings would let `musl/src/*` be
        // published because it is not literally equal to `musl/src/include`.
        {
            const auto privateExpanded =
                expandPrivateIncludeDirs(packageRoot, manifest);
            for (auto const& d : pkg.privateBuild.includeDirs)
                if (std::ranges::find(privateExpanded, d) == privateExpanded.end())
                    pkg.publicUsage.includeDirs.push_back(d);

            // AN ENTRY THAT WITHHOLDS NOTHING IS REPORTED, because the way
            // it fails is the very defect this key exists to prevent: a
            // directory the author believes is private stays published, and
            // nothing about the build looks different until a consumer trips
            // over a name months later.
            //
            // A WARNING AND NOT AN ERROR, for consistency with `include_dirs`
            // itself: that key silently ignores a glob matching nothing, and a
            // conditional manifest can legitimately name a directory that
            // exists on one platform only. Refusing here would be stricter
            // than the list this one filters.
            for (auto const& want : privateExpanded) {
                if (std::ranges::find(pkg.privateBuild.includeDirs, want)
                    != pkg.privateBuild.includeDirs.end())
                    continue;
                mcpp::diag::warning("manifest", std::format(
                    "package '{}': `private_include_dirs` names '{}', which is "
                    "not among this package's `include_dirs`.\n"
                    "       It withholds nothing — `private_include_dirs` says "
                    "which entries OF `include_dirs`\n"
                    "       stop at this package's boundary, and an entry that "
                    "is not one of them is published\n"
                    "       exactly as before.",
                    manifest.package.name, want.generic_string()));
            }
        }
        pkg.publicUsage.includeDirsAfter = pkg.privateBuild.includeDirsAfter;
        pkg.linkUsage.ldflags = manifest.buildConfig.ldflags;
        return pkg;
    };

    packages[0] = makePackageRoot(*root, *m);

    auto recordDependencyEdge =
        [&](std::size_t consumerDepIndex,
            std::size_t dependencyPackageIndex,
            const mcpp::manifest::DependencySpec& spec,
            bool buildOnly = false)
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
        auto it = std::find_if(dependencyEdges.begin(), dependencyEdges.end(), same);
        if (it != dependencyEdges.end()) {
            // One consumer naming one dependency in BOTH tables. The ordinary
            // declaration wins, because the build-time path never subtracts
            // from what the project asked to link — stating the rule the other
            // way round would let a `[build-dependencies]` line quietly drop a
            // library the target needs.
            if (!buildOnly) it->buildOnly = false;
            return;
        }
        dependencyEdges.push_back(DependencyEdge{
            .consumerPackageIndex = consumerPackageIndex,
            .dependencyPackageIndex = dependencyPackageIndex,
            .visibility = visibility,
            .requestedFeatures = spec.features,
            .defaultFeatures = spec.defaultFeatures,
            .requestedTools = spec.tools,
            .hostModule = spec.hostModule,
            .reexport = spec.reexport,
            .buildOnly = buildOnly,
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

    auto package_source_files = [](
        const std::filesystem::path& srcRoot,
        const mcpp::manifest::Manifest& depManifest)
        -> std::expected<std::set<std::filesystem::path>, std::string>
    {
        // Resolve the source globs against the original root, falling
        // back to the convention default if the manifest didn't set any.
        std::vector<std::string> globs = depManifest.modules.sources;
        if (globs.empty()) {
            // Was a fourth hand-written copy of the convention default, and it
            // had already drifted: all three assembly extensions were missing,
            // so staging a dependency with .S/.s/.asm silently dropped them.
            globs = mcpp::default_source_globs(
                mcpp::extension_table_for(depManifest.buildConfig.moduleExtensions));
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
        return sourceFiles;
    };

    // Stage a dep's source files into a fresh directory, rewriting their
    // module / import declarations against `rename`. Used by the multi-
    // version mangling fallback (Level 1) so two cross-major copies of
    // the same package can coexist with distinct module names.
    //
    // Headers reached through `[build].include_dirs` are NOT staged — those
    // keep pointing at the original install dir via absolutized include paths.
    //
    // HEADERS BESIDE A SOURCE ARE A DIFFERENT CASE, AND THEY ARE STAGED.
    //
    // `#include "detail.h"` is resolved relative to the directory of the file
    // holding the directive, so moving the source moves the search. No
    // `include_dirs` entry is involved and absolutizing one cannot help: the
    // package never declared a path because it never needed one. Measured
    // before this, on a package whose `src/time.cpp` includes `src/sbi.h`:
    //
    //     target/.mangled/openkal-opensbi/__self__/src/time.cpp:44:10:
    //         fatal error: 'sbi.h' file not found
    //
    // THE DIAGNOSIS THIS PRODUCES POINTS AT THE WRONG THING. The path in it
    // is a staging directory the author never wrote, for a header sitting
    // exactly where the source expects it, and the build that triggered it
    // asked for nothing unusual — two majors of one dependency is a supported
    // arrangement, and this is its most ordinary consequence.
    //
    // What is copied is every file in a directory that contains a staged
    // source and is not itself staged, verbatim: rewriting applies to module
    // declarations, and a header has none. Directories with no staged source
    // are not visited, so this stays proportional to what is being staged.
    auto stage_with_rewrite = [&](const std::filesystem::path& srcRoot,
                                  const std::filesystem::path& dstRoot,
                                  const mcpp::manifest::Manifest& depManifest,
                                  const std::map<std::string, std::string>& rename)
        -> std::expected<void, std::string>
    {
        std::error_code ec;
        std::filesystem::create_directories(dstRoot, ec);
        if (ec) return std::unexpected(std::format(
            "stage: cannot create '{}': {}", dstRoot.string(), ec.message()));

        auto sources = package_source_files(srcRoot, depManifest);
        if (!sources) return std::unexpected(sources.error());

        for (auto const& f : *sources) {
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

        // The files beside those sources, carried across unchanged so a quoted
        // include still finds what it named.
        std::set<std::filesystem::path> sourceDirs;
        for (auto const& f : *sources) sourceDirs.insert(f.parent_path());
        for (auto const& dir : sourceDirs) {
            for (auto const& entry : std::filesystem::directory_iterator(dir, ec)) {
                if (ec) break;
                if (!entry.is_regular_file()) continue;
                if (sources->contains(entry.path())) continue;
                auto rel = std::filesystem::relative(entry.path(), srcRoot, ec);
                if (ec) continue;
                auto dst = dstRoot / rel;
                std::filesystem::create_directories(dst.parent_path(), ec);
                std::filesystem::copy_file(
                    entry.path(), dst,
                    std::filesystem::copy_options::overwrite_existing, ec);
                if (ec) return std::unexpected(std::format(
                    "stage: cannot copy '{}': {}",
                    entry.path().string(), ec.message()));
            }
            ec.clear();
        }
        return {};
    };

    auto declared_modules_for = [&](const std::filesystem::path& srcRoot,
                                    const mcpp::manifest::Manifest& depManifest)
        -> std::expected<std::vector<std::string>, std::string>
    {
        auto sources = package_source_files(srcRoot, depManifest);
        if (!sources) return std::unexpected(sources.error());
        std::vector<std::string> modules;
        for (auto const& file : *sources) {
            std::ifstream is(file);
            if (!is) return std::unexpected(std::format(
                "mangle: cannot read '{}'", file.string()));
            std::stringstream buf; buf << is.rdbuf();
            for (auto& name : mcpp::pm::declared_module_roots(buf.str())) {
                if (std::ranges::find(modules, name) == modules.end())
                    modules.push_back(std::move(name));
            }
        }
        if (modules.empty()) return std::unexpected(std::format(
            "mangle: package '{}' declares no named C++ module to rewrite",
            depManifest.package.name));
        return modules;
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
            for (auto& [k, spec] : it->second) {
                auto [pos, fresh] = pm.dependencies.try_emplace(k, spec);
                if (fresh) continue;
                // #359: the key already exists unconditionally, and dropping
                // the feature's spec here loses REQUESTS the feature exists to
                // make. gRPC is the shape: it depends on compat.protobuf
                // always, and its `codegen` feature has to add
                // `tools = ["protoc"], reexport = true` to that same edge —
                // which is precisely what must NOT be paid for by a consumer
                // who did not ask for codegen, so moving it to the
                // unconditional entry is not an option either.
                //
                // Additive fields merge; identity fields (version/path/git) do
                // not, keeping "a conditional section never silently
                // overrides an unconditional one" intact. Same rule the
                // per-edge feature request already follows.
                auto& dst = pos->second;
                for (auto const& t : spec.tools)
                    if (std::find(dst.tools.begin(), dst.tools.end(), t)
                        == dst.tools.end())
                        dst.tools.push_back(t);
                for (auto const& f2 : spec.features)
                    if (std::find(dst.features.begin(), dst.features.end(), f2)
                        == dst.features.end())
                        dst.features.push_back(f2);
                dst.hostModule = dst.hostModule || spec.hostModule;
                dst.reexport   = dst.reexport   || spec.reexport;
            }
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
    activeFeaturesByPackage.assign(1, rootActive);

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
                                req.version, kMainConsumer, {}, /*devOnly=*/true});
        }
    }
    // `[build-dependencies]`. Parsed since 0.0.x, merged across workspace
    // members, conditionalised by target predicate — and until now read by
    // nothing that made a decision, so writing it produced a manifest that
    // loaded, no diagnostic, and no effect. Seeded here, and unlike dev-deps
    // it IS walked transitively: a build dependency's own dependencies are
    // what make it work, and they inherit its build-only nature.
    for (auto& [n, s] : m->buildDependencies) {
        auto req = s;
        injectForwards(*m, rootActive, n, req);
        worklist.push_back({n, req, mainPkgLabel + " (build-dep)",
                            req.version, kMainConsumer, {}, /*devOnly=*/false,
                            /*buildOnly=*/true});
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
            // A package is dev-only until some non-dev consumer wants it. Order
            // of arrival must not decide, so this is an AND over every request.
            it->second.devOnly = it->second.devOnly && item.devOnly;
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

                auto merged = mcpp::pm::try_merge_semver(
                    key.ns, key.shortName,
                    it->second.constraint,
                    item.originalConstraint,
                    index_route(*cfg), targetPlatform);
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

                    // Module names are authored API and are not required to
                    // mirror package identity. Discover every provided module
                    // root from the secondary's source text, then rewrite the
                    // same map in both the secondary and its consumer.
                    auto moduleNames = declared_modules_for(
                        secondaryRoot, secondaryManifest);
                    // The two branches above name both versions and who asked
                    // for them; this one used to report only that the package
                    // declares no named C++ module, which is a true statement
                    // about a package the reader never asked to be staged. A
                    // C package -- compat.vulkan-runtime is one -- reaches
                    // here whenever a manifest pins one version of it and
                    // another dependency asks for a second, and the message
                    // has to say that before it says anything about modules.
                    if (!moduleNames) return std::unexpected(std::format(
                        "dependency '{}{}{}' has irreconcilable versions:\n"
                        "  '{}' requested by '{}'\n"
                        "  '{}' requested by '{}'\n"
                        "Multi-version mangling cannot separate them: {}.\n"
                        "A package with no named C++ module has nothing to "
                        "rewrite, so the two requests must agree. Align the "
                        "pin in your mcpp.toml with the version the other "
                        "dependency asks for.",
                        key.ns, key.ns.empty() ? "" : ".", key.shortName,
                        it->second.version, it->second.requestedBy,
                        spec.version, item.requestedBy,
                        moduleNames.error()));
                    std::map<std::string, std::string> rename;
                    for (auto const& module : *moduleNames) {
                        rename.emplace(module,
                            mcpp::pm::mangle_name(module, spec.version));
                    }
                    const auto& moduleName = moduleNames->front();
                    const auto& mangledModule = rename.at(moduleName);
                    const std::string mangledPackage = mcpp::pm::mangle_name(
                        key.shortName, spec.version);

                    // Stage layout:
                    //   <root>/target/.mangled/<consumerPkg>/<dep>__<version>/    ← rewritten secondary source
                    //   <root>/target/.mangled/<consumerPkg>/__self__/             ← rewritten consumer source
                    auto& consumerManifest = *dep_manifests[item.consumerDepIndex];
                    auto consumerRoot      = packages[item.consumerDepIndex + 1].root;
                    auto stageBase         = *root / "target" / ".mangled"
                                             / consumerManifest.package.name;
                    auto secStage          = stageBase
                                             / std::format("{}__{}", key.shortName, spec.version);
                    auto consumerStage     = stageBase / "__self__";

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
                    // Give the staged package a distinct atomic identity too;
                    // authored module names remain independent and are carried
                    // exclusively by the rename map above.
                    stagedManifest.package.name = mangledPackage;
                    if (stagedManifest.package.namespace_.empty()) {
                        stagedManifest.package.namespace_ = key.ns.empty()
                            ? std::string(mcpp::pm::kDefaultNamespace) : key.ns;
                    }
                    stagedManifest.package.sourceProvenance = std::format(
                        "index+{}@{}", cache_index_name(key.ns), spec.version);
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
                        .packageName = mangledPackage,
                        .version     = spec.version,
                        .sourceKind  = "version",
                    });
                    const auto depPackageIndex = packages.size();
                    packages.push_back(makePackageRoot(secStage, *dep_manifests.back()));
                    recordDependencyEdge(item.consumerDepIndex, depPackageIndex,
                                         spec, item.buildOnly);
                    auto linkFlagsAdded = propagateLinkFlags(secStage, *dep_manifests.back());

                    ResolvedKey mangledKey{key.ns, mangledPackage};
                    resolved[mangledKey] = ResolvedRecord{
                        .version           = spec.version,
                        .constraint        = item.originalConstraint,
                        .requestedBy       = item.requestedBy,
                        .source            = "version",
                        .devOnly           = item.devOnly,
                        .depIndex          = dep_manifests.size() - 1,
                        .linkFlagsAdded    = std::move(linkFlagsAdded),
                    };

                    mcpp::ui::info("Mangled",
                        std::format("{} v{} ↔ v{} → {} (cross-major fallback)",
                            moduleName, it->second.version, spec.version,
                            mangledModule));
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
                                         spec, item.buildOnly);
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
                if (newManifest.package.namespace_.empty()) {
                    newManifest.package.namespace_ = key.ns.empty()
                        ? std::string(mcpp::pm::kDefaultNamespace) : key.ns;
                }
                newManifest.package.sourceProvenance = std::format(
                    "index+{}@{}", cache_index_name(key.ns), *merged);

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
                                     spec, item.buildOnly);

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
                                        it->second.depIndex, {}, item.devOnly});
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
                                     spec, item.buildOnly);
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
            // as a path dep from there.
            //
            // Two independent questions, each answered by at most one network
            // operation and therefore guarded by exactly one --offline gate:
            //
            //   1. WHICH COMMIT?  `tag`/`rev` name one outright. A `branch` is
            //      floating: mcpp.lock answers it, else `git ls-remote` does.
            //   2. IS IT ON DISK? The commit selects the cache directory, so a
            //      miss — or a clone parked on the wrong commit — is a clone.
            //
            // mcpp.lock is authoritative for (1), not a hint that (2) has to
            // confirm: a recorded commit is used whether or not the clone
            // survived, so evicting ~/.mcpp/git can never quietly move a build
            // onto a newer branch tip. `mcpp update <dep>` drops the entry and
            // stays the one way a branch advances.
            auto mcppHome = mcpp::home::root();   // single resolver (#311)

            const bool remoteIsLocal = is_local_git_remote(spec.git);
            auto refuse_offline = [&](std::string_view need,
                                      std::string_view why,
                                      std::string_view verb) {
                return std::unexpected(std::format(
                    "offline mode: git dependency '{}' needs {} of '{}'\n"
                    "       {}\n"
                    "       run without --offline (or unset MCPP_OFFLINE) to {} it",
                    name, need, spec.git, why, verb));
            };
            const bool offline =
                !remoteIsLocal && mcpp::platform::env::offline_mode();

            // ── 1. which commit ──
            std::string resolvedGitRev = spec.gitRev;
            bool fromLock = false;
            if (spec.gitRefKind == "branch") {
                auto it = gitLockAnchors.find(name);
                if (it != gitLockAnchors.end()
                    && it->second.url     == spec.git
                    && it->second.refKind == spec.gitRefKind
                    && it->second.ref     == spec.gitRev
                    && it->second.resolvedCommit) {
                    resolvedGitRev = *it->second.resolvedCommit;
                    fromLock = true;
                } else {
                    if (offline)
                        return refuse_offline("`git ls-remote`",
                            std::format("mcpp.lock records no commit for branch "
                                        "'{}'", spec.gitRev),
                            "resolve");
                    // The FIRST network step of a git dependency, and therefore
                    // the one a transient fault is most likely to meet.
                    auto r = run_with_network_retry(std::format(
                        "git ls-remote {} {} 2>&1",
                        mcpp::platform::shell::quote(spec.git),
                        mcpp::platform::shell::quote(
                            std::format("refs/heads/{}", spec.gitRev))));
                    if (r.exit_code != 0)
                        return std::unexpected(std::format(
                            "git ls-remote of '{}' failed:\n{}",
                            spec.git, r.output));
                    // Cleared first: `operator>>` leaves the target untouched
                    // when the stream is already at EOF, which would otherwise
                    // let the declared branch name pass the emptiness check.
                    resolvedGitRev.clear();
                    std::istringstream is(r.output);
                    is >> resolvedGitRev;
                    if (resolvedGitRev.empty())
                        return std::unexpected(std::format(
                            "git branch '{}' not found in '{}'",
                            spec.gitRev, spec.git));
                }
            }

            // ── 2. is it on disk ──
            // Cache key: hash(url + refkind + declared ref + resolved commit).
            // For fixed rev/tag deps the declared ref is also the resolved ref.
            std::hash<std::string> H;
            auto gitRoot = mcppHome / "git" / std::format("{:016x}",
                H(spec.git + "|" + spec.gitRefKind + "|" + spec.gitRev
                  + "|" + resolvedGitRev));
            std::error_code ec;
            std::filesystem::create_directories(gitRoot.parent_path(), ec);

            // A branch's resolved rev is always a sha by now, so the clone can
            // be checked against it — catching one killed between `git clone`
            // and `git checkout`, which would otherwise serve the wrong commit
            // from a correctly-named directory forever. tag/rev keep their ref
            // name as the identity, so there is nothing to compare.
            bool cachePresent = std::filesystem::exists(gitRoot / ".git");
            if (cachePresent && spec.gitRefKind == "branch"
                && git_cache_head(gitRoot) != resolvedGitRev) {
                std::filesystem::remove_all(gitRoot, ec);
                cachePresent = false;
            }

            // Reported before the clone, not instead of it: when the cache is
            // gone this line is the whole explanation for why the build is on
            // an older commit than the branch now points at.
            if (fromLock)
                mcpp::ui::info("Resolved",
                    std::format("{} (branch = {}) from mcpp.lock",
                        spec.git, spec.gitRev));

            if (!cachePresent) {
                if (offline)
                    return refuse_offline("a clone",
                        std::format("no cached clone at {}", gitRoot.string()),
                        "fetch");
                mcpp::ui::info("Cloning",
                    std::format("{} ({} = {})", spec.git, spec.gitRefKind, spec.gitRev));
                // A commit taken from the lock may sit behind the branch tip,
                // and a tag/rev may sit anywhere in history — both need full
                // history before the checkout. Only a tip just read from
                // ls-remote is guaranteed present in a depth-1 clone.
                //
                // `git -C` rather than `cd <dir> &&`: on Windows `cd` does not
                // change drive without /d, and the cache root routinely lives
                // on a different one than the project.
                auto cloneCmd = (spec.gitRefKind == "branch" && !fromLock)
                    ? std::format(
                        "git clone --depth 1 --branch {} {} {} && "
                        "git -C {} checkout --quiet {} 2>&1",
                        mcpp::platform::shell::quote(spec.gitRev),
                        mcpp::platform::shell::quote(spec.git),
                        mcpp::platform::shell::quote(gitRoot.string()),
                        mcpp::platform::shell::quote(gitRoot.string()),
                        mcpp::platform::shell::quote(resolvedGitRev))
                    : std::format(
                        "git clone {} {} && git -C {} checkout --quiet {} 2>&1",
                        mcpp::platform::shell::quote(spec.git),
                        mcpp::platform::shell::quote(gitRoot.string()),
                        mcpp::platform::shell::quote(gitRoot.string()),
                        mcpp::platform::shell::quote(resolvedGitRev));
                // See `run_with_network_retry` for why, and for what the
                // callback is removing between attempts.
                auto r = run_with_network_retry(cloneCmd, [&] {
                    std::filesystem::remove_all(gitRoot, ec);
                });
                if (r.exit_code != 0) {
                    std::filesystem::remove_all(gitRoot, ec);
                    return std::unexpected(std::format(
                        "git clone of '{}' failed:\n{}", spec.git, r.output));
                }
            }
            if (item.consumerDepIndex == kMainConsumer) {
                // Only root deps are locked: the writer below walks the root
                // manifest's [dependencies], so a transitive git branch dep
                // has no anchor and still resolves over the network.
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
            // A MEMBER IS A MEMBER HOWEVER IT IS REACHED.
            //
            // A workspace member that omits `package.version` because
            // `[workspace.package]` supplies it is legal — and it is reached
            // here as a sibling's `path` dependency, which is the ordinary
            // shape rather than an exotic one. Loading it as an anonymous path
            // dependency would refuse it for a field the workspace does
            // provide, and the message would name the member's manifest rather
            // than the table that answers.
            //
            // `is_workspace_member` asks the workspace's own `members` list, so
            // a vendored copy or an example living inside the tree is still
            // refused for a missing version, exactly as before.
            const bool depIsMember =
                wsManifest && !runtimeWorkspaceRoot.empty()
                && mcpp::project::is_workspace_member(
                       *wsManifest, runtimeWorkspaceRoot, dep_root);
            auto dm = mcpp::manifest::load(
                dep_root / "mcpp.toml",
                {.insideWorkspace = depIsMember});
            if (!dm) {
                return std::unexpected(std::format(
                    "dependency '{}' (at '{}'): {}",
                    name, dep_root.string(), dm.error().format()));
            }
            dep_manifest = std::move(*dm);
            // The metadata half of the inheritance. The `[build]` half runs in
            // `makePackageRoot`, where the include directories are captured;
            // splitting them is what keeps each one at the point its consumer
            // reads it.
            if (depIsMember) {
                mcpp::project::inherit_workspace_package(
                    *dep_manifest, *wsManifest);
                if (auto bad = mcpp::project::workspace_inheritance_error(
                        *dep_manifest, dep_root))
                    return std::unexpected(*bad);
            }
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
                merge_conditional_config(*dep_manifest,
                    cfgCtx());
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

        // Stamp the identity with the resolver's exact coordinate and source.
        // A descriptor that omitted namespace inherits the coordinate that
        // answered it; otherwise two indices containing the same short name
        // collapse in runtime provenance even though resolution distinguished
        // them correctly.
        if (dep_manifest->package.namespace_.empty()) {
            dep_manifest->package.namespace_ = key.ns.empty()
                ? std::string(mcpp::pm::kDefaultNamespace) : key.ns;
        }
        if (sourceKind == "version") {
            dep_manifest->package.sourceProvenance = std::format(
                "index+{}@{}", cache_index_name(key.ns), spec.version);
        } else if (sourceKind == "git") {
            dep_manifest->package.sourceProvenance = std::format(
                "git+{}#{}={}", spec.git, spec.gitRefKind, spec.gitRev);
        } else {
            dep_manifest->package.sourceProvenance =
                "path+" + dep_root.lexically_normal().generic_string();
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
            .sourceKind  = sourceKind,
        });
        const auto depPackageIndex = packages.size();
        packages.push_back(makePackageRoot(dep_root, *dep_manifests.back()));
        recordDependencyEdge(item.consumerDepIndex, depPackageIndex, spec,
                             item.buildOnly);

        // Record this dep as resolved so future encounters of the same
        // (ns, name) hit the fast path (skip / merge / conflict).
        resolved[key] = ResolvedRecord{
            .version           = sourceKind == "version" ? spec.version : "",
            .constraint        = sourceKind == "version" ? item.originalConstraint : "",
            .requestedBy       = item.requestedBy,
            .source            = sourceKind,
            .devOnly           = item.devOnly,
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
                                childReq.version, selfIdx, dep_root,
                                item.devOnly, item.buildOnly});
        }
        // A dependency's own `[build-dependencies]` — the only channel through
        // which a package can speak about what IT needs at build time. Both
        // live channels (`tools`, `host-module`) are written by the CONSUMER
        // on an edge, so before this a build rule could not request anything
        // on its own behalf. That, and not a design decision, is why a rule
        // was a leaf.
        //
        // These are build-only regardless of how this package was reached: a
        // library's build dependency has no business in its consumer's binary
        // either.
        for (auto& [child_name, child_spec] :
                 dep_manifests.back()->buildDependencies) {
            auto childReq = child_spec;
            injectForwards(*dep_manifests.back(), depActive, child_name, childReq);
            worklist.push_back({child_name, childReq,
                                thisDepLabel + " (build-dep)",
                                childReq.version, selfIdx, dep_root,
                                item.devOnly, /*buildOnly=*/true});
        }
    }

    computeUsageRequirements();

    // ─── The toolchain, resolved now that the graph exists ──────────────────
    //
    // THE TARGET AND THE COMPILER ARE NOT BOUND TOGETHER, AND THE ROW'S
    // CONVENTION IS A FALLBACK RATHER THAN A RULE.
    //
    // `x86_64-linux-musl → gcc@16.1.0` does not say "prefer gcc". It says "the
    // musl-gcc payload is what supplies this target's C library". A project
    // whose C library comes from its dependency graph does not use that payload,
    // and for it the convention is not a default but a substitution — measured,
    // it replaced a toolchain the user had set with `mcpp toolchain default` and
    // said nothing.
    //
    // The discriminator is whether anything in the graph supplies the system,
    // which is what these few lines ask. It is the same question
    // `mcpp.targetside` answers in full further down; asked here it needs only
    // the answer's shape, so it reads the manifests rather than resolving them.
    {
        bool graphSuppliesSystem = false;
        // `requires` IS READ HERE TOO, AND UNTIL THIS LOOP IT WAS ONLY EVER
        // CHECKED — A THOUSAND LINES LATER, AGAINST A DECISION THIS BLOCK HAD
        // ALREADY MADE WITHOUT IT.
        //
        // `provides` and `requires` are the two halves of one vocabulary and
        // they were read at opposite ends of the function: this block consulted
        // the first to decide the compiler, and `check_requirements` used the
        // second only to reject the outcome. Measured on 2026.8.26.1, one
        // three-line manifest, `llvm@22.1.8` already installed:
        //
        //     [dependencies]
        //     openkal-llvm-runtime = "0.1.3"     # requires mcpp:compiler=llvm
        //
        //     $ mcpp build                       # global default gcc@16.1.0
        //       error: `openkal-llvm-runtime@0.1.3` requires the compiler to be `llvm`.
        //              Select that compiler …  mcpp toolchain default llvm
        //     $ MCPP_TOOLCHAIN=llvm@22.1.8 mcpp build
        //       Finished dev [unoptimized + debuginfo] in 1.02s
        //
        // Nothing was missing. The engine knew which compiler was wanted, the
        // payload was on the machine, and the remedy it printed was to change
        // the default for EVERY project on the box because ONE project's
        // dependency asked.
        //
        // AND THIS IS THE PLACE, NOT MERELY *A* PLACE. `resolve_target_toolchain`
        // has exactly two call sites — its own one-shot recursion, and the one
        // at the bottom of this block — so every branch inside it, INCLUDING the
        // first-run install-and-persist path and all three
        // `write_default_toolchain` calls, is downstream of this line. Setting
        // `tcSpec` here therefore selects the compiler without writing anything:
        // on a machine with no toolchain at all the first-run branch is not even
        // reached, because its condition is `!tcSpec.has_value()`.
        //
        // That is the whole design. "Do not touch the user's configuration" is
        // not a rule anyone has to remember here — the writes live on a branch
        // this no longer enters.
        std::string reqCompiler, reqCompilerBy;

        // A FAMILY NAME BECOMES A CONCRETE SPEC THE SAME WAY IT DOES FOR
        // `mcpp toolchain default <family>`, AND FOR THE SAME REASON.
        //
        // `requires = ["mcpp:compiler=llvm"]` names a family; the build path
        // needs `<family>@<version>` and refuses anything else
        // (`expected '<pkg>@<version>'`). There are two honest sources for the
        // missing half and they are tried in this order:
        //
        //   1. what is already installed — highest version wins, nothing is
        //      downloaded, and it is literally the same two functions
        //      `toolchain_set_default` calls;
        //   2. the vocabulary's own pins — the version this ecosystem ships for
        //      that family, already written down once per row. Deriving it from
        //      there rather than from a fresh constant means the answer moves
        //      when the ecosystem moves, with nobody having to remember a
        //      second place.
        //
        // NOT `pins::kFirstRun*`. Those are per-HOST first-run defaults —
        // `llvm@20.1.7` on macOS, `gcc@16.1.0` on Linux x86_64 — so reading them
        // would make the version a package requires depend on which machine
        // built it. A requirement is a property of the package.
        auto resolve_required_family =
            [&](const std::string& family)
            -> std::expected<std::string, std::string> {
            auto spec = mcpp::toolchain::parse_toolchain_spec(family);
            if (!spec) {
                refusal::record(refusal::Code::CompilerRequirementConflict);
                return std::unexpected(std::format(
                    "`{}` requires the compiler to be `{}`, and mcpp has no "
                    "compiler family by that name.\n"
                    "       known families: gcc, llvm, msvc.",
                    reqCompilerBy, family));
            }

            if (auto cfg = get_cfg(); cfg) {
                auto pkg = mcpp::toolchain::to_xim_package(*spec);
                if (auto picked = mcpp::toolchain::resolve_version_match(
                        "", mcpp::toolchain::list_installed_versions(
                                (*cfg)->xlingsHome() / "data" / "xpkgs",
                                pkg.ximName)))
                    return std::format("{}@{}", family, *picked);
            }

            std::vector<std::string> fromVocabulary;
            for (auto const& row : mcpp::toolchain::triple::known_targets()) {
                if (row.pin.empty()) continue;
                auto p = mcpp::toolchain::parse_toolchain_spec(
                    std::string(row.pin));
                if (!p || p->version.empty()) continue;
                if (mcpp::toolchain::family_name(p->family) != family) continue;
                fromVocabulary.push_back(p->version);
            }
            if (auto picked = mcpp::toolchain::resolve_version_match(
                    "", std::move(fromVocabulary)))
                return std::format("{}@{}", family, *picked);

            // Neither source has one. Saying which family and which two places
            // were consulted is the difference between an actionable message
            // and "something went wrong".
            // RECORDED, like every other refusal in this function. An
            // unnamed branch reports `other`, and this release exists partly
            // because one of those had a perfectly good name.
            refusal::record(refusal::Code::CompilerRequirementConflict);
            return std::unexpected(std::format(
                "`{}` requires the compiler to be `{}`, and mcpp has no version "
                "of it to use.\n"
                "       none is installed, and no target row pins one.\n"
                "       install one — `mcpp toolchain install {} <version>` "
                "(`mcpp toolchain list --available {}`).",
                reqCompilerBy, family, family, family));
        };

        for (auto const& pkg : packages) {
            for (auto const& entry : pkg.manifest.provides) {
                auto cap = mcpp::targetside::parse_capability(entry);
                if (!cap || !*cap) continue;
                if ((*cap)->layer == mcpp::targetside::CapLayer::KernelAbi
                 || (*cap)->layer == mcpp::targetside::CapLayer::CAbi) {
                    graphSuppliesSystem = true;
                }
            }
            for (auto const& entry : pkg.manifest.requires_) {
                auto cap = mcpp::targetside::parse_capability(entry);
                if (!cap || !*cap) continue;
                if ((*cap)->layer != mcpp::targetside::CapLayer::Compiler) continue;
                // A bare `mcpp:compiler` asks only that one exist, which it
                // always does. Only a named family selects anything.
                if ((*cap)->interfaceName.empty()) continue;
                const auto pkgId = pkg.manifest.package.version.empty()
                    ? pkg.manifest.package.name
                    : std::format("{}@{}", pkg.manifest.package.name,
                                  pkg.manifest.package.version);
                // TWO DIFFERENT FAMILIES IS AN ERROR RATHER THAN A PICK, the
                // same rule `provides` already follows one screen down. Choosing
                // by graph-traversal order would make the answer depend on an
                // order the author neither writes nor can predict — and unlike a
                // conflicting `provides`, this one would silently satisfy one
                // package's requirement and fail the other's inside a header.
                if (!reqCompiler.empty() && reqCompiler != (*cap)->interfaceName) {
                    refusal::record(refusal::Code::CompilerRequirementConflict);
                    return std::unexpected(std::format(
                        "two packages require different compilers, and a build "
                        "has only one.\n"
                        "         {:<28} requires `{}`\n"
                        "         {:<28} requires `{}`\n"
                        "       Both cannot hold. Drop one of them, or take a "
                        "version of one that is\n"
                        "       configured for the other's compiler.",
                        reqCompilerBy, reqCompiler, pkgId,
                        (*cap)->interfaceName));
                }
                if (reqCompiler.empty()) {
                    reqCompiler   = (*cap)->interfaceName;
                    reqCompilerBy = pkgId;
                }
            }
        }
        // AND A FREESTANDING PIN SURVIVES IT. `graphSuppliesSystem` spans
        // kernel-abi and c-abi, and it correctly cancels a HOSTED row's
        // convention — that row names the payload the graph is replacing.
        // A bare-metal row names the only compiler that emits the target.
        //
        // Measured 2026-08-25, on a three-line manifest:
        //
        //     provides = ["mcpp:kernel-abi=openkal"]
        //     $ mcpp build --target riscv64-none-elf
        //       Resolved gcc@16.1.0 → riscv64-none-elf → …/bin/g++
        //       g++: error: unrecognized argument in option '-mabi=lp64d'
        //       g++: error: unrecognized command-line option
        //                   '--target=riscv64-none-elf'
        //
        // A package saying which layer it supplies made the host compiler be
        // chosen for a target it cannot produce. Same shape as the four
        // defects 2026.8.25.1 fixed: a predicate spanning two layers deciding
        // something that does not depend on either of them.
        if (!targetPinCandidate.empty()
            && (!graphSuppliesSystem || targetPinIsCapability)) {
            if (tcOrigin == TcOrigin::GlobalDefault && tcSpec.has_value()
                && *tcSpec != targetPinCandidate)
                pinReplacedDefault = *tcSpec;
            tcSpec   = targetPinCandidate;
            tcOrigin = TcOrigin::TargetPin;
        }

        // THE GRAPH'S REQUIREMENT, TAKEN AS AN INSTRUCTION RATHER THAN AS A
        // TEST TO FAIL LATER.
        //
        // Everything above this line decides the compiler from what mcpp knows
        // about the TARGET. A package saying `requires = ["mcpp:compiler=llvm"]`
        // is saying something about ITSELF — its C++ runtime was configured for
        // one family and its headers record that configuration — and it is the
        // most specific statement in the build. Below the user's own word, above
        // every default mcpp keeps.
        //
        // THE RANK IS NOT NEW. `TcOrigin` already sorts these, and
        // `tc_origin_is_user_explicit` already answers "may mcpp revise this".
        // The defect was never that the answer was wrong; it was that nobody
        // asked. `GlobalDefault` is deliberately not user-explicit — see the
        // note on that function — so a remembered default is exactly the kind of
        // value this may replace.
        // `system` IS LEFT ALONE, AND IT IS THE ONE VALUE HERE THAT IS AN
        // ESCAPE HATCH RATHER THAN AN ANSWER.
        //
        // It means "the PATH compiler, whatever it is" — a deliberate opt-out
        // of the payload model. Substituting a payload for it would defeat
        // exactly what the user asked for, and mcpp cannot even tell whether
        // the requirement is already satisfied: the family of a PATH compiler
        // is not knowable from the spec. `check_requirements` reports the
        // mismatch further down against what the driver actually turned out to
        // be, which is the only place that answer exists.
        const bool tcIsSystemEscapeHatch =
            tcSpec.has_value() && *tcSpec == "system";
        if (!reqCompiler.empty() && !tcIsSystemEscapeHatch) {
            std::string haveFamily;
            if (tcSpec.has_value())
                if (auto s = mcpp::toolchain::parse_toolchain_spec(*tcSpec); s)
                    haveFamily =
                        std::string(mcpp::toolchain::family_name(s->family));

            if (haveFamily != reqCompiler) {
                // THE PROJECT'S OWN WORD IS NOT REVISED, AND THIS IS THE ONLY
                // CASE THAT STILL REFUSES. `[toolchain]`, `[target.X].toolchain`
                // and `MCPP_TOOLCHAIN` are statements about THIS build; the
                // graph disagreeing with one of them is a real contradiction and
                // `check_requirements` reports it further down with both names.
                // Nothing to do here but leave the value alone.
                if (tc_origin_is_user_explicit(tcOrigin)) {
                    // fall through to check_requirements
                }
                // A ROW'S PIN THAT SURVIVED TO HERE CANNOT BE OVERRIDDEN BY
                // A REQUIREMENT, AND THE REASON IS THE SAME ONE THE PIN EXISTS
                // FOR.
                //
                // The block above applied it only when the graph does NOT supply
                // the system, or when the row names a capability. In the first
                // case the row's payload is what carries this target's headers
                // and C library, and a different compiler brings none — measured
                // as `crtbeginT.o (bare name)` and as a host `crtbegin.o`, both
                // accurate about the symptom and silent about the decision. In
                // the second the row names the only compiler that emits the
                // target at all.
                //
                // Either way the requirement cannot be honoured, and saying so
                // here — where both halves are known — beats a compiler
                // complaining about a file the reader never named.
                else if (tcOrigin == TcOrigin::TargetPin) {
                    // THE TWO ROWS REFUSE UNDER ONE RULE AND FOR TWO
                    // REASONS, AND ONE REMEDY DOES NOT SERVE BOTH.
                    //
                    // A CONVENTION pin is cancelled by a graph that supplies the
                    // target's system — that is `graphSuppliesSystem`, one
                    // screen up — so "depend on a package that supplies it" is
                    // exactly the way out.
                    //
                    // A CAPABILITY pin is not: `targetPinIsCapability` keeps it
                    // applied no matter what the graph supplies, because no
                    // other family emits the target at all. Offering the same
                    // remedy there prints an instruction that the sentence
                    // directly above it has already ruled out — the failure
                    // this release removes from `check_requirements`, reproduced
                    // three screens away.
                    std::string_view why = targetPinIsCapability
                        ? "The row names its compiler as a capability: no other "
                          "family emits this target."
                        : "The row's payload is what supplies this target's "
                          "headers and C library,\n       and nothing in the "
                          "dependency graph supplies them instead.";
                    std::string remedy = targetPinIsCapability
                        ? std::format(
                              "       Drop the package that requires `{}`, or "
                              "take a version of it built\n"
                              "       for `{}`.",
                              reqCompiler, targetPinCandidate)
                        : std::format(
                              "       Depend on a package that supplies this "
                              "target's system (its kernel\n"
                              "       interface and C library) so the row's "
                              "payload is not needed, or drop\n"
                              "       the package that requires `{}`.",
                              reqCompiler);
                    refusal::record(refusal::Code::CompilerRequirementConflict);
                    return std::unexpected(std::format(
                        "`{}` requires the compiler to be `{}`, and target '{}' "
                        "cannot be built with it here.\n"
                        "         target row       {:<14} ({})\n"
                        "         required         {:<14} (required by {})\n"
                        "       {}\n{}",
                        reqCompilerBy, reqCompiler,
                        targetRowName.empty() ? overrides.target_triple
                                              : targetRowName,
                        targetPinCandidate,
                        targetPinIsCapability ? "capability" : "convention",
                        reqCompiler, reqCompilerBy,
                        why, remedy));
                }
                // Free to take it. `tcSpec` is either absent (nothing configured
                // anywhere) or one of mcpp's own remembered answers.
                else {
                    auto pickedSpec = resolve_required_family(reqCompiler);
                    if (!pickedSpec)
                        return std::unexpected(pickedSpec.error());
                    graphCompilerReplaced   = tcSpec.value_or("");
                    graphCompilerRequiredBy = reqCompilerBy;
                    graphCompilerFamily     = reqCompiler;
                    tcSpec   = *pickedSpec;
                    tcOrigin = TcOrigin::GraphRequirement;
                }
            }
        }
        // OVERRIDING THE CONVENTION IS ALLOWED; OVERRIDING IT AND SUPPLYING
        // NOTHING IN ITS PLACE IS NOT, AND UNTIL THIS BLOCK IT LOOKED THE SAME.
        //
        // A hosted row's pin names the payload that supplies the target's C
        // library. A project may name a different compiler — that is the escape
        // hatch the whole convention/capability distinction exists to protect —
        // and the ordinary reason to do so is that its dependency graph supplies
        // the C library instead. `examples/06-openkal-cross` is exactly that:
        // `llvm@22.1.8` plus `openkal-llvm-runtime`, and `graphSuppliesSystem`
        // is true there.
        //
        // WITH NEITHER, THE BUILD USED TO RUN ANYWAY AND FAIL SOMEWHERE ELSE.
        // Measured 2026-08-26 on Linux, `[toolchain] default = "llvm@22.1.8"`
        // and no dependencies:
        //
        //   --target x86_64-linux-musl
        //     hermetic link check failed … crtbeginT.o (bare name)
        //   --target x86_64-windows-gnu
        //     hermetic link check failed …
        //     /usr/lib/gcc/x86_64-w64-mingw32/13-win32/crtbegin.o (outside)
        //
        // Both are accurate about the symptom and silent about the decision:
        // clang is retargetable and brings no C library, so it reached for a
        // gcc installation — one that does not exist under the payload prefix
        // in the first case, and that belongs to the HOST in the second. There
        // is no llvm payload supplying either target's C library today.
        //
        // THE REFUSAL IS DECIDED HERE BECAUSE ONLY HERE ARE BOTH HALVES
        // KNOWN. The row is read a thousand lines earlier and the graph does
        // not exist then; `host_can_serve` is family-agnostic and would answer
        // "yes, some payload here produces it" — the same shape as the family
        // this release is about, a predicate answering a question narrower than
        // the one it is asked.
        if (!targetRowPin.empty() && !graphSuppliesSystem
            && tc_origin_is_user_explicit(tcOrigin) && tcSpec.has_value()) {
            auto declared = mcpp::toolchain::parse_toolchain_spec(*tcSpec);
            auto rowTc    = mcpp::toolchain::parse_toolchain_spec(targetRowPin);
            if (declared && rowTc && declared->family != rowTc->family) {
                refusal::record(refusal::Code::ConventionUnreplaced);
                return std::unexpected(std::format(
                    "target '{}' takes its C library from the '{}' payload, and "
                    "'{}' has none here.\n"
                    "       The row's toolchain is a convention, so naming your "
                    "own compiler overrides it —\n"
                    "       but the convention is what supplies this target's "
                    "headers and C library, and\n"
                    "       nothing in the dependency graph supplies them "
                    "instead.\n"
                    "       depend on a package that implements the target's C "
                    "library (openkal-musl and\n"
                    "       openkal-llvm-runtime are the ones in the index), or "
                    "remove the `[toolchain]`\n"
                    "       line so `{}` is used for this target.",
                    targetRowName, targetRowPin, *tcSpec, targetRowPin));
            }
        }
        if (auto r = resolve_target_toolchain(); !r)
            return std::unexpected(r.error());
    }

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
    // Who claimed sole provision of what. Separate from capProviders because
    // the question it answers is different: capProviders asks "can this
    // requirement be satisfied", this asks "can these two coexist at all".
    std::map<std::string, std::vector<std::string>> capExclusive;
    // Callable twice: once here, for what the manifests and the
    // dependencies' build programs declared, and once more after the
    // root's build program has run -- a rule package it imports states
    // its facts and floors from there (`mcpp::fact` / `mcpp::floor`),
    // and a check that ran only before it would never see them.
    // package name -> device-kind sources of its effective source set, filled
    // by the narrowing pass after feature application and read at both
    // build-program run sites (MCPP_DEVICE_SOURCES).
    // Keyed by the package's ROOT DIRECTORY, not by its name. Two packages in
    // one graph may share a bare name and differ only by namespace — that is
    // what namespaces are for — and a name key would hand one package's
    // device sources to the other's build program with nothing reporting it.
    std::map<std::string, std::vector<std::string>> deviceSourcesByPackage;
    auto checkVersionFloors = [&]() -> std::optional<std::string> {
        std::map<std::string, std::pair<std::string, std::string>> facts;  // name -> (version, who)
        for (std::size_t pi = 0; pi < packages.size(); ++pi) {
            // The root's claims live in *m: its build program mutates
            // *m, and packages[0] is a snapshot taken before it ran.
            const auto& mf  = pi == 0 ? *m : packages[pi].manifest;
            const auto who = mf.package.name;
            for (auto const& entry : mf.runtimeConfig.provides) {
                auto fact = mcpp::build::parse_version_fact(entry);
                if (fact.valid()) facts.emplace(fact.name, std::pair{fact.version, who});
            }
        }
        for (std::size_t pi = 0; pi < packages.size(); ++pi) {
            // The root's claims live in *m: its build program mutates
            // *m, and packages[0] is a snapshot taken before it ran.
            const auto& mf  = pi == 0 ? *m : packages[pi].manifest;
            const auto who = mf.package.name;
            for (auto const& req : mf.runtimeConfig.requirements) {
                if (req.kind != "version-floor") continue;
                auto floor = mcpp::build::parse_version_floor(req.value);
                if (!floor.valid()) {
                    return std::format(
                        "`{}` declares a version-floor requirement mcpp "
                        "cannot read: '{}'.\n"
                        "       The shape is `<name> >= <version>`, e.g. "
                        "`cuda.driver >= 12.0`.", who, req.value);
                }
                auto it = facts.find(floor.name);
                if (it == facts.end()) continue;      // nobody stated it
                auto met = mcpp::build::version_at_least(it->second.first,
                                                        floor.version);
                if (!met || *met) continue;
                refusal::record(refusal::Code::VersionFloorUnmet);
                return std::format(
                    "`{}` requires {} >= {}, and this machine has {}.\n"
                    "         stated by: {}\n"
                    "       This is checked before anything is compiled "
                    "because the failure it prevents is not:\n"
                    "       a build against too-new a runtime links "
                    "cleanly and fails at first use.",
                    who, floor.name, floor.version, it->second.first,
                    it->second.second);
            }
        }
        return std::nullopt;
    };
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
            for (auto& cap : pkg.manifest.exclusive) capExclusive[cap].push_back(pcap);
            for (auto& f : active) {
                if (auto it = pkg.manifest.featureProvides.find(f);
                    it != pkg.manifest.featureProvides.end())
                    for (auto& cap : it->second) capProviders[cap].push_back(pcap);
                if (auto it = pkg.manifest.featureRequires.find(f);
                    it != pkg.manifest.featureRequires.end())
                    for (auto& cap : it->second) capRequires.emplace_back(cap, pcap);
            }
            // `[targets.*] required_features` on a DEPENDENCY.
            //
            // THIS GATE EXISTED ONLY FOR THE ROOT. The root's targets are
            // filtered further down against the root's own active features;
            // a dependency's were never filtered at all, so a descriptor that
            // wrote `required_features` on a target got the opposite of what
            // it asked for: the target was built for EVERY consumer, whether
            // or not the feature was active. For a `kind = "shared"` target
            // that is not a cosmetic difference — its mere presence changes
            // how the whole package is linked into every consumer.
            //
            // Gated against THIS package's active set, not the root's. A
            // feature name is package-scoped, so the root's set is a different
            // vocabulary that happens to share a type.
            //
            // LIBRARY TARGETS ONLY, and the exclusion is load-bearing.
            //
            // A target requested as a HOST TOOL is what was ASKED FOR, so its
            // `required_features` become that sub-build's INPUTS instead of a
            // gate — docs/05 §2.2 says so in as many words. An earlier
            // revision of this gate erased every kind, with a comment claiming
            // the tool path "re-enters prepare_build with the dependency as
            // the ROOT, so it never reaches this code". That was written from
            // memory rather than read: the tool LOOKUP runs several hundred
            // lines BELOW this point, against this very manifest, and it found
            // an empty target list. `187_dep_host_tool.sh` caught it.
            //
            // Restricting the gate to libraries is not a workaround, it is the
            // rule: a dependency's `bin` target produces no link unit in this
            // build (make_plan only walks the ROOT's targets), so leaving it in
            // place costs nothing. What the gate exists for is the shape where
            // a target's mere presence changes how the package is linked into
            // every consumer — and that is exactly a `shared` or `lib` target.
            std::erase_if(pkg.manifest.targets,
                          [&](const mcpp::manifest::Target& t) {
                if (t.kind != mcpp::manifest::Target::Library
                    && t.kind != mcpp::manifest::Target::SharedLibrary)
                    return false;
                for (auto const& rf : t.requiredFeatures)
                    if (std::find(active.begin(), active.end(), rf) == active.end())
                        return true;
                return false;
            });

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
                // WHETHER A FEATURE *GATES* A SOURCE OR *PROVIDES* IT, AND
                // THE ANSWER IS WRITTEN IN THE MANIFEST ALREADY.
                //
                // Two families of package reach this code and they want
                // opposite things under `mcpp test`:
                //
                //   gtest         lists `*/googletest/src/gtest_main.cc` in
                //                 base `sources` AND under `features.main`.
                //                 The package provides the file unconditionally;
                //                 the feature is a gate over it. The
                //                 dev-dependency track's per-test main detection
                //                 must still SEE it in order to prune it per
                //                 test, so an inactive gate must not make it
                //                 vanish.
                //
                //   riscv-virt-rt names `src/kal/**` under `features.openkal`
                //                 and nowhere else. The package does not provide
                //                 those files at all without the feature — the
                //                 headers they include arrive through that
                //                 feature's `[feature-deps]` — so compiling them
                //                 fails on `'openkal/abort.h' file not found`.
                //
                // The discriminator is membership in base `sources`, evaluated
                // BEFORE the drop below removes it. A glob in both places is a
                // gate; a glob in one place is a provider.
                //
                // THIS IS THE FOURTH ATTEMPT, AND THE THIRD WAS ABANDONED ON
                // A MISTAKEN READING. It was recorded as failing because
                // "gtest's base entry is a glob that MATCHES the file rather
                // than the same string". Measured against the descriptor the
                // index actually carries, the two entries are byte-identical
                // (`compat.gtest.lua` lines 71 and 90). The criterion was
                // sound; what it was applied to was not — the earlier attempt
                // compared against `bc.sources` AFTER `drop()` had already
                // removed the entry, so the membership test could only ever be
                // false.
                std::set<std::string> baseGlobs(bc.sources.begin(), bc.sources.end());
                baseGlobs.insert(pkg.manifest.modules.sources.begin(),
                                 pkg.manifest.modules.sources.end());
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
                else {
                    // `mcpp test`. The gate that build mode applies wholesale is
                    // applied here only to the globs the package provides
                    // NOWHERE ELSE, which leaves gtest's doubly-listed source
                    // visible and stops riscv-virt-rt's feature-only sources
                    // from being compiled without their feature.
                    //
                    // THE `!` EXCLUSION IS THE WHOLE MECHANISM, NOT THE GLOB
                    // REMOVAL. `src/kal/**` is never IN `bc.sources` — the
                    // package declares no `sources` at all and its files are
                    // matched by the inferred `src/**`. Erasing the string
                    // erases nothing; only an exclusion gates.
                    std::set<std::string> activeNow(active.begin(), active.end());
                    std::map<std::string, bool> gated;
                    for (auto& [f, globs] : bc.featureSources)
                        for (auto& g : globs)
                            gated[g] = gated[g] || activeNow.contains(f);
                    for (auto& [g, isActive] : gated) {
                        if (isActive || g.starts_with("!")) continue;
                        if (baseGlobs.contains(g)) continue;   // a gate, not a provider
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
            if (activeFeaturesByPackage.size() <= i)
                activeFeaturesByPackage.resize(i + 1);
            activeFeaturesByPackage[i] =
                feature_closure(packages[i].manifest, req, depDefaultFeatures);
        }

        // ── Constrained source globs: narrow to what this build targets ────
        //
        // A `{ glob = "...", accel = "..." }` entry in `[build] sources` says
        // what its files are FOR. Three outcomes, all decided here and none in
        // the scanner, which keeps reading a plain list of globs:
        //
        //   - the glob matches nothing: refused, naming the glob. An empty
        //     match is a typo or a moved directory, not a no-op, and the
        //     failure it would otherwise become is a kernel that is never
        //     compiled and a link that resolves nothing.
        //   - the build asks for no accelerator: the glob is EXCLUDED, with the
        //     same `!` mechanism feature gates use -- removing the string is not
        //     enough when a broader glob (the default `src/**`) covers the same
        //     files. This is how `--no-accel` yields the CPU-only variant.
        //   - the build asks for one: the constraint must lie within it, or the
        //     build is refused naming both. A file compiled for sm_89 under a
        //     build that targets sm_80 is not a variant, it is a mismatch.
        //
        // Device-kind files the effective set still matches are collected per
        // package for the build program (MCPP_DEVICE_SOURCES); the engine has
        // no compile rule for them and never will.
        {
            const auto buildAccel = mcpp::pack::parse_accel(resolvedAccel());
            for (std::size_t i = 0; i < packages.size(); ++i) {
                auto& pkg = packages[i];
                auto& bc  = pkg.manifest.buildConfig;
                std::set<std::string> excludedGlobs;
                for (auto const& sc : bc.sourceConstraints) {
                    const auto hits = mcpp::modgraph::expand_glob(pkg.root, sc.glob);
                    if (hits.empty()) {
                        return std::unexpected(std::format(
                            "`{}`: [build] sources entry '{}' (accel = \"{}\") matches no file.\n"
                            "       A constrained glob names the files a device build needs; an\n"
                            "       empty match would leave nothing to compile for that device\n"
                            "       and say so only at the link, or never.",
                            pkg.manifest.package.name, sc.glob, sc.accel));
                    }
                    if (buildAccel.empty()) { excludedGlobs.insert(sc.glob); continue; }
                    const auto want = mcpp::pack::parse_accel(sc.accel);
                    if (!mcpp::pack::accel_accepts(buildAccel, want)) {
                        refusal::record(refusal::Code::AccelMismatch);
                        return std::unexpected(std::format(
                            "`{}`: [build] sources entry '{}' is constrained to accel \"{}\",\n"
                            "       which this build does not cover.\n"
                            "         this build targets: {}\n"
                            "       fix: build with `--accel` covering it, or `--no-accel` to\n"
                            "       leave every constrained glob out (the CPU-only variant).",
                            pkg.manifest.package.name, sc.glob,
                            mcpp::pack::accel_str(want),
                            mcpp::pack::accel_str(buildAccel)));
                    }
                }
                for (auto const& g : excludedGlobs) {
                    bc.sources.push_back("!" + g);
                    pkg.manifest.modules.sources.push_back("!" + g);
                }
                // The device-kind files the EFFECTIVE set matches, for the
                // build program. Exclusions are honoured the way the scanner
                // honours them: positives first, then `!` entries removed.
                const auto extTable = mcpp::extension_table_for(bc.moduleExtensions);
                std::set<std::filesystem::path> matched, dropped;
                for (auto const& g : pkg.manifest.modules.sources) {
                    if (g.empty()) continue;
                    if (g[0] == '!') { for (auto& f : mcpp::modgraph::expand_glob(pkg.root, g.substr(1))) dropped.insert(f); }
                    else if (!std::filesystem::path(g).is_absolute())
                        for (auto& f : mcpp::modgraph::expand_glob(pkg.root, g)) matched.insert(f);
                }
                std::vector<std::string> device;
                for (auto const& f : matched) {
                    if (dropped.contains(f)) continue;
                    if (mcpp::classify(f, extTable) != mcpp::SourceKind::Device) continue;
                    device.push_back(f.lexically_relative(pkg.root).generic_string());
                }
                deviceSourcesByPackage[pkg.root.string()] = std::move(device);
            }
        }
        activeFeaturesByPackage.resize(packages.size());

        // ── #355: HOST tool provisioning ────────────────────────────────────
        //
        // Runs AFTER feature activation (a tool target's gate is a feature) and
        // BEFORE any build.mcpp (which is what consumes the tools). That
        // ordering is the whole point: build.mcpp runs inside prepare, so a
        // tool produced by the main ninja graph would arrive far too late —
        // and under --target it would be the wrong architecture besides.
        //
        // Each tool is built by re-entering prepare_build with the DEPENDENCY
        // as the root and no --target, i.e. for the build machine. That is
        // Cargo's [build-dependencies] / Bazel's exec configuration shape.
        // It is affordable because an executable has zero ABI contact with the
        // main build: the sub-build may use the tool package's own toolchain,
        // its own profile, and its own resolution — none of it has to agree
        // with the consumer.
        {
            // Aggregate off the authoritative edge graph, exactly like feature
            // activation — a transitive consumer's request must not be
            // silently dropped (#242/#243).
            std::map<std::size_t, std::set<std::string>> toolRequests;
            for (auto const& edge : dependencyEdges)
                for (auto const& t : edge.requestedTools)
                    toolRequests[edge.dependencyPackageIndex].insert(t);

            // #359: one fixpoint decides who SEES what. `toolRequests` above
            // still decides what gets BUILT — the two questions are separate,
            // and conflating them is what made a re-exported tool impossible:
            // the tool was built, but its path was recorded against the library
            // that asked for it rather than the project that needs it.
            provisionGraph = prov::propagate(dependencyEdges, packages.size());

            // #355 step 5: dependencies offering HOST build rules. Nothing is
            // compiled here — the interface is handed to build_program.cppm,
            // which compiles it in the SAME command as build.mcpp so the BMI
            // and its consumer agree on standard, dialect and compiler by
            // construction rather than by luck.
            //
            // Driven off the visible set rather than the root manifest, so a
            // rule a library re-exports is importable from the consumer's
            // build.mcpp without the consumer naming it. The name matching the
            // old loop needed is gone with it: the edge already knows which
            // package it points at.
            //
            // The registered name is the one the rule's SOURCE declares, not
            // the package's name. See provisions::host_module_name for why the
            // two had drifted apart and what that cost on Clang and MSVC.
            std::set<std::string> prefixWarned;
            // Providers of host modules that THIS package sees directly.
            auto directHostProviders = [&](std::size_t p) {
                std::vector<std::size_t> out;
                if (p >= provisionGraph.visible.size()) return out;
                for (auto const& pr : provisionGraph.visible[p]) {
                    if (pr.kind != prov::Kind::HostModule) continue;
                    if (pr.provider >= packages.size()) continue;
                    out.push_back(pr.provider);
                }
                return out;
            };
            auto identity = [&](std::size_t p) {
                auto const& pkg = packages[p].manifest.package;
                return pkg.namespace_.empty()
                     ? pkg.name : pkg.namespace_ + "." + pkg.name;
            };
            // Every host module one package contributes, the lib root first.
            //
            // The lib root is what a rule package has always been: one unit,
            // compiled alone, registered under the name it declares. A package
            // that offers several rules through features (mcpp 2026.9.5.3+)
            // lists their sources under `[features.<f>] sources`, and those
            // globs have been folded into `buildConfig.sources` by now for
            // exactly the features the consumer activated. Every module
            // INTERFACE unit among them is therefore a host module of its own,
            // under its own declared name, and nothing else in the host-module
            // path assumes one unit per package: `build_host_module` is per
            // unit and the compile loop accumulates BMIs in list order, so a
            // feature unit may import the lib root, which precedes it.
            //
            // Only sources the manifest LISTS take part. The inferred `src/**`
            // of a package with no `sources` is not consulted, so a rule
            // package published before this round exposes exactly what it
            // exposed then; widening that implicitly would compile units that
            // were written to be part of an ordinary library, alone.
            auto units = [&](std::size_t p) {
                auto const& depPkg = packages[p];
                auto const& pkg    = depPkg.manifest.package;
                std::vector<prov::HostModule> out;
                auto push = [&](std::filesystem::path iface, std::string name) {
                    prov::HostModule hm;
                    hm.module    = std::move(name);
                    hm.package   = identity(p);
                    hm.nameSpace = pkg.namespace_;
                    hm.interface = std::move(iface);
                    out.push_back(std::move(hm));
                };
                // PROBING form: a host-module dependency whose interface is
                // `.ixx` resolves to a `src/<tail>.cppm` that does not exist,
                // and the consumer's build.mcpp is then handed a path to
                // nothing.
                auto rel   = mcpp::manifest::resolve_lib_root_path(
                    depPkg.manifest, depPkg.root);
                auto iface = depPkg.root / rel;
                push(iface, prov::host_module_name(iface, pkg.name));
                // A missing lib root is reported as such by build_host_module,
                // and that has to stay the diagnostic. Enumerating the listed
                // units first would let one of them collide with the missing
                // root's fallback name and report a collision between a file
                // and a file that does not exist.
                std::error_code ec;
                if (!std::filesystem::exists(iface, ec)) return out;

                std::set<std::filesystem::path> matched, dropped;
                for (auto const& g : depPkg.manifest.buildConfig.sources) {
                    if (g.empty()) continue;
                    if (g[0] == '!') {
                        for (auto& f : mcpp::modgraph::expand_glob(depPkg.root, g.substr(1)))
                            dropped.insert(f.lexically_normal());
                    } else {
                        for (auto& f : mcpp::modgraph::expand_glob(depPkg.root, g))
                            matched.insert(f.lexically_normal());
                    }
                }
                const auto root = iface.lexically_normal();
                for (auto const& f : matched) {          // std::set: sorted
                    if (dropped.contains(f)) continue;
                    if (std::filesystem::equivalent(f, root, ec)) continue;
                    std::ifstream is(f);
                    if (!is) continue;
                    std::stringstream buf;
                    buf << is.rdbuf();
                    auto name = prov::declared_interface_name(buf.str());
                    if (name.empty()) continue;
                    push(f, std::move(name));
                }
                return out;
            };
            for (std::size_t c = 0; c < provisionGraph.visible.size(); ++c) {
                const auto direct = directHostProviders(c);
                if (direct.empty()) continue;
                std::set<std::size_t> isDirect(direct.begin(), direct.end());

                // Post-order DFS, so a rule's own host modules are compiled
                // BEFORE it. That ordering is the entire mechanism: the
                // compile loop in build_program.cppm accumulates the module
                // flags as it goes, so each entry sees the BMIs of everything
                // ahead of it, and "a rule may import another rule" needs no
                // second machinery — only this sort.
                std::vector<prov::HostModule> ordered;
                std::set<std::size_t> done;
                std::vector<std::size_t> path;   // for the cycle diagnostic
                auto visit = [&](auto&& self, std::size_t p) -> std::expected<void, std::string> {
                    if (done.contains(p)) return {};
                    if (std::ranges::find(path, p) != path.end()) {
                        // A cycle, reported AS a cycle and naming the packages
                        // on it. A depth limit would answer a different
                        // question and would answer it later.
                        std::string ring;
                        bool started = false;
                        for (auto q : path) {
                            if (q == p) started = true;
                            if (!started) continue;
                            ring += identity(q);
                            ring += " -> ";
                        }
                        ring += identity(p);
                        return std::unexpected(std::format(
                            "build rules form an import cycle: {}\n"
                            "       A rule's host modules are compiled before "
                            "it, so a cycle has no order that could satisfy "
                            "all of them.", ring));
                    }
                    path.push_back(p);
                    for (auto q : directHostProviders(p))
                        if (auto r = self(self, q); !r) return r;
                    path.pop_back();
                    done.insert(p);
                    for (auto& hm : units(p)) {
                        hm.importable = isDirect.contains(p);
                        ordered.push_back(std::move(hm));
                    }
                    return {};
                };
                for (auto p : direct)
                    if (auto r = visit(visit, p); !r)
                        return std::unexpected(r.error());

                if (auto clash = prov::host_module_collision(ordered))
                    return std::unexpected(*clash);
                for (auto const& hm : ordered) {
                    // Warned once per (package, module), not once per consumer:
                    // a rule re-exported down a chain is visible to every
                    // package on it, and repeating one naming remark N times
                    // reads as N problems.
                    if (auto w = prov::reserved_prefix_warning(
                            hm.module, hm.nameSpace, hm.package)) {
                        if (prefixWarned.insert(hm.package + "\x1e" + hm.module).second)
                            mcpp::diag::warning("build/rule-namespace", *w);
                    }
                    hostModulesByConsumer[c].push_back(
                        {hm.module, hm.interface, hm.importable});
                }
            }

            // A build rule is BUILD-TIME ONLY. Registering the module is not
            // enough: the package is still an ordinary node of the consumer's
            // graph, so its interface was ALSO compiled as a normal library and
            // linked into the target. That is wrong on its own terms — a rule
            // has no business in the consumer's binary — and it made the
            // feature nearly unusable, because in that second compile the
            // bundled `mcpp` module does not exist: any rule that actually used
            // the API it exists to wrap died with `fatal error: module 'mcpp'
            // not found` (2026.8.5.1).
            //
            // Emptying the source globs is how a package is removed from the
            // compile set here — the same mechanism the feature-gated-sources
            // drop above uses. Resolution is untouched: the package still lands
            // on disk, which is what `resolve_lib_root_path` just read.
            //
            // Guarded on EVERY edge into the package being a host-module edge.
            // A package can legitimately be both a rule and a library, and
            // silently dropping its objects then would surface as an undefined
            // reference far from here. (The predicate used to be "no consumer
            // other than the root", which said the same thing only while the
            // root was the only possible requester.)
            //
            // Stated as FORWARD REACHABILITY rather than as exclusion, and the
            // difference is not cosmetic.
            //
            // The predicate used to be per-edge: "every in-edge into this
            // package is a host-module edge". That is right about the rule
            // itself and wrong about everything BEHIND it — a rule's own
            // `[dependencies]` are reached by ordinary edges, so they kept
            // their globs and were compiled and LINKED INTO THE CONSUMER'S
            // BINARY, while the rule could not even import them. Both halves
            // were wrong, and the sharper harm was that a rule's dependency
            // versions took part in the consumer's real resolution, so a rule
            // could create a version conflict in a project that never asked
            // for one.
            //
            // Exclusion would also get the dual-role case backwards. A package
            // the project depends on directly must stay in the target even
            // when some rule's build dependencies also reach it: the
            // build-time path never subtracts from what the project asked to
            // link. Asking "can the target reach it" answers both cases with
            // one rule and no special case.
            {
                auto reachable = [&](bool targetEdgesOnly) {
                    std::vector<bool> seen(packages.size(), false);
                    if (packages.empty()) return seen;
                    std::vector<std::size_t> stack{0};
                    seen[0] = true;
                    while (!stack.empty()) {
                        auto p = stack.back();
                        stack.pop_back();
                        for (auto const& e : dependencyEdges) {
                            if (e.consumerPackageIndex != p) continue;
                            if (targetEdgesOnly && (e.hostModule || e.buildOnly))
                                continue;
                            auto d = e.dependencyPackageIndex;
                            if (d >= seen.size() || seen[d]) continue;
                            seen[d] = true;
                            stack.push_back(d);
                        }
                    }
                    return seen;
                };
                const auto viaTarget = reachable(/*targetEdgesOnly=*/true);
                const auto viaAny    = reachable(/*targetEdgesOnly=*/false);
                for (std::size_t d = 1; d < packages.size(); ++d) {
                    // `viaAny` is the guard that keeps this from acting on a
                    // package no edge ever mentioned. Such a package is a
                    // bookkeeping gap, not a build dependency, and clearing
                    // its sources would turn that gap into an undefined
                    // reference a long way from here.
                    if (viaTarget[d] || !viaAny[d]) continue;
                    auto& dm = packages[d].manifest;
                    dm.buildConfig.sources.clear();
                    dm.buildConfig.featureSources.clear();
                    dm.modules.sources.clear();
                }
            }

            if (overrides.tool_depth >= mcpp::build::tool_store::kMaxDepth
                && !toolRequests.empty()) {
                return std::unexpected(std::format(
                    "tool provisioning nested more than {} levels deep — this is "
                    "almost certainly a cycle.\n  chain: {}",
                    mcpp::build::tool_store::kMaxDepth, overrides.tool_chain));
            }

            for (auto const& [depIdx, wanted] : toolRequests) {
                auto& depPkg = packages[depIdx];
                const auto& depName = depPkg.manifest.package.name;
                std::string depShort = depName;
                if (auto dot = depName.rfind('.');
                    dot != std::string::npos && dot + 1 < depName.size())
                    depShort = depName.substr(dot + 1);

                for (auto const& toolName : wanted) {
                    // The target must exist and be a binary. Naming the
                    // alternatives matters: the consumer wrote a string, and a
                    // typo is the likeliest cause.
                    const mcpp::manifest::Target* tgt = nullptr;
                    std::string binList;
                    for (auto const& t : depPkg.manifest.targets) {
                        if (t.kind != mcpp::manifest::Target::Binary) continue;
                        if (!binList.empty()) binList += ", ";
                        binList += t.name;
                        if (t.name == toolName) tgt = &t;
                    }
                    if (!tgt) {
                        // A package may declare a bin target on some platforms
                        // only. When the request came from a LIBRARY rather
                        // than from the user, the user cannot edit it away, so
                        // point at the knob that library needs (#359 D3a).
                        return std::unexpected(std::format(
                            "dependency '{}' has no `kind = \"bin\"` target named "
                            "'{}' (requested via tools = [...]).\n"
                            "  available bin targets: [{}]\n"
                            "  If the requesting package is a library, it can "
                            "scope the request per platform with\n"
                            "  [target.'cfg(...)'.feature-deps.<feature>].",
                            depName, toolName,
                            binList.empty() ? std::string("none") : binList));
                    }

                    auto var = mcpp::build::tool_store::env_var_name(depName, toolName);
                    auto varShort =
                        mcpp::build::tool_store::env_var_name(depShort, toolName);

                    // #359: every consumer that can SEE this tool gets it, not
                    // just the one whose edge asked for it. The bare spelling
                    // is emitted only where the namespace ladder binds the tail
                    // to this package — otherwise two libraries re-exporting a
                    // same-tailed tool would decide the winner by append order.
                    const prov::Provision want{ prov::Kind::Tool, depIdx, toolName };
                    auto record = [&](const std::filesystem::path& p) {
                        for (std::size_t c = 0; c < provisionGraph.visible.size(); ++c) {
                            if (!provisionGraph.visible[c].contains(want)) continue;
                            auto& v = toolEnvByConsumer[c];
                            v.emplace_back(var, p.string());
                            if (varShort == var) continue;
                            auto bind = bareBindingsFor(c);
                            auto it = bind.find(depShort);
                            if (it != bind.end() && it->second.owner == depName)
                                v.emplace_back(varShort, p.string());
                        }
                    };

                    // Escape hatch first: it is the cheapest resolution and the
                    // one a user reaches for precisely when building is not an
                    // option. Deliberately not part of the store key — see
                    // tool_store.cppm.
                    if (auto ovr = mcpp::build::tool_store::find_override(
                            *m, depName, depShort, toolName)) {
                        if (!std::filesystem::exists(*ovr)) {
                            return std::unexpected(std::format(
                                "tool override for '{}:{}' points at '{}', which "
                                "does not exist", depName, toolName, ovr->string()));
                        }
                        mcpp::ui::info("Tool", std::format(
                            "{}:{} → {} (override)", depName, toolName, ovr->string()));
                        record(*ovr);
                        continue;
                    }

                    // Build it. The feature set is the tool package's own
                    // defaults PLUS the target's required_features — in a tool
                    // sub-build the target is what was ASKED FOR, so its
                    // requirements are inputs rather than a gate. (Same field,
                    // opposite resolution direction; docs/05 says so.)
                    std::vector<std::string> feats = tgt->requiredFeatures;
                    auto closure = feature_closure(depPkg.manifest, feats, true);

                    auto hostTc = host_tc_for_build_program();
                    if (!hostTc) return std::unexpected(hostTc.error());

                    mcpp::build::tool_store::Key key;
                    key.indexName = depIdx >= 1 && depIdx - 1 < dep_cache_identities.size()
                                  ? dep_cache_identities[depIdx - 1].indexName
                                  : std::string(mcpp::pm::kDefaultNamespace);
                    key.packageName      = depName;
                    key.version          = depPkg.manifest.package.version;
                    key.targetName       = toolName;
                    key.hostTriple       = mcpp::toolchain::triple::host_triple().str();
                    key.compilerIdentity = std::format("{}|{}|{}",
                        hostTc->second.label(), hostTc->second.version,
                        hostTc->first.string());
                    key.profile          = "release";
                    key.features         = closure;
                    std::ranges::sort(key.features);
                    // The tool package's TRANSITIVE dependency closure, not just
                    // its direct edges. Direct-only would be enough for index
                    // packages (a frozen version cannot change its own deps),
                    // but a path dependency can: bump something two levels down
                    // and the tool's direct list is unchanged, so a stale binary
                    // stays in the store — a silently wrong artifact.
                    for (auto up : dg::transitive_dependencies(dependencyEdges, depIdx))
                        key.upstreamKeys.push_back(std::format("{}@{}",
                            packages[up].manifest.package.name,
                            packages[up].manifest.package.version));
                    std::ranges::sort(key.upstreamKeys);

                    const auto cacheRoot = mcpp::home::cache_root();
                    const auto entry     = mcpp::build::tool_store::entry_dir(cacheRoot, key);
                    const auto exeSuffix = std::string(mcpp::platform::exe_suffix);
                    const auto binOut    = mcpp::build::tool_store::bin_path(
                        entry, toolName, exeSuffix);

                    if (mcpp::build::tool_store::entry_valid(entry, key, toolName,
                                                             exeSuffix)) {
                        record(binOut);
                        continue;
                    }

                    mcpp::ui::status("Building", std::format(
                        "host tool {}:{} from {} v{} (once per package version × "
                        "host toolchain)", depName, toolName, depName,
                        depPkg.manifest.package.version));

                    BuildOverrides sub;
                    sub.project_root = depPkg.root;
                    // Never the package root: it is shared across projects and
                    // may be read-only. This is the reason work_dir exists.
                    //
                    // Scratch is keyed on the CONSUMING project, not shared:
                    // the store is GLOBAL, so two projects can want the same
                    // tool at once. A single `<entry>/build` would have them
                    // writing one ninja tree concurrently, and whichever
                    // finished first would `remove_all` it out from under the
                    // other. The published binary is what gets shared; the
                    // scratch is not.
                    //
                    // Hashed rather than random so a re-run reuses its own
                    // scratch (ninja stays incremental if the publish step
                    // never got to delete it).
                    sub.work_dir     = entry / std::format("build-{}",
                        mcpp::toolchain::hash_string(workRoot.string()));
                    sub.target_triple = "";            // HOST — the whole point
                    sub.profile       = "release";
                    sub.cache_mode    = overrides.cache_mode;
                    sub.tool_depth    = overrides.tool_depth + 1;
                    // The PRISTINE manifest the resolver produced for this
                    // package — `packages[depIdx].manifest` is a copy that
                    // feature activation has already mutated, and re-activating
                    // on top of it would fold the same feature sources in
                    // twice. A `compat` (Form B) package has no mcpp.toml on
                    // disk at all, so without this the sub-build could not read
                    // a manifest for it in the first place.
                    if (depIdx >= 1 && depIdx - 1 < dep_manifests.size()
                        && dep_manifests[depIdx - 1])
                        sub.preloaded_manifest =
                            std::make_shared<const mcpp::manifest::Manifest>(
                                *dep_manifests[depIdx - 1]);
                    sub.inherited_runtime_selection = std::make_shared<
                        const mcpp::xlings::runtime::RuntimeSelection>(
                            runtimeSelection);
                    sub.inherited_runtime_binding = std::make_shared<
                        const mcpp::platform::runtime::RuntimeBinding>(
                            runtimeBindingSnapshot);
                    sub.tool_chain    = overrides.tool_chain.empty()
                        ? std::format("root → {}:{}", depName, toolName)
                        : std::format("{} → {}:{}", overrides.tool_chain, depName,
                                      toolName);
                    for (auto const& f : closure) {
                        if (!sub.features.empty()) sub.features += ",";
                        sub.features += f;
                    }

                    // #359 (D3b): a sub-build failure must be attributable and
                    // REPRODUCIBLE. The Windows tool sub-build has been failing
                    // on three abseil TUs since #355 and is still unlocated,
                    // because what reached the log was a one-line summary with
                    // no scratch path, no chain, and — on the ninja branch below
                    // — a filtered view of the inner output. Naming the scratch
                    // directory is what lets a maintainer re-run the exact inner
                    // build; MCPP_TOOL_BUILD_VERBOSE turns off the filtering.
                    auto subContext = [&] {
                        return std::format(
                            "\n  chain: {}\n  sub-build scratch: {}\n"
                            "  re-run it directly:  mcpp build -p {} --release\n"
                            "  (set MCPP_TOOL_BUILD_VERBOSE=1 for the inner "
                            "build's unfiltered output)",
                            sub.tool_chain, sub.work_dir.string(),
                            depPkg.root.string());
                    };
                    auto subCtx = prepare_build(/*print_fingerprint=*/false,
                                                /*includeDevDeps=*/false,
                                                /*extraTargets=*/{}, sub);
                    if (!subCtx) {
                        return std::unexpected(std::format(
                            "building host tool '{}:{}' failed: {}{}",
                            depName, toolName, subCtx.error(), subContext()));
                    }

                    // Build ONLY the requested target (#274 gave the backend
                    // explicit goals) — a tool request must not drag the whole
                    // package's other artifacts along.
                    std::filesystem::path goal;
                    for (auto const& lu : subCtx->plan.linkUnits) {
                        if (lu.targetName == toolName) { goal = lu.output; break; }
                    }
                    if (goal.empty()) {
                        return std::unexpected(std::format(
                            "host tool '{}:{}' produced no link unit — its "
                            "required_features may not be satisfiable on this "
                            "platform", depName, toolName));
                    }

                    auto be = mcpp::build::make_ninja_backend();
                    mcpp::build::BuildOptions bopt;
                    bopt.ninjaTargets = { goal.generic_string() };
                    // Unfiltered inner output on demand: the filter drops
                    // ninja's own progress and command echoes, which is right
                    // for a normal build and wrong when the question is "what
                    // did the inner build actually do".
                    if (const char* v = std::getenv("MCPP_TOOL_BUILD_VERBOSE");
                        v && *v && std::string_view(v) != "0")
                        bopt.verbose = true;
                    auto br = be->build(subCtx->plan, bopt);
                    if (!br) {
                        auto diag = br.error().diagnosticOutput;
                        if (diag.empty())
                            diag = "(the inner build produced no diagnostic "
                                   "output; re-run with MCPP_TOOL_BUILD_VERBOSE=1)";
                        return std::unexpected(std::format(
                            "building host tool '{}:{}' failed: {}{}\n{}",
                            depName, toolName, br.error().message,
                            subContext(), diag));
                    }
                    if (br->exitCode != 0) {
                        return std::unexpected(std::format(
                            "building host tool '{}:{}' failed (exit {}){}",
                            depName, toolName, br->exitCode, subContext()));
                    }

                    // Publish into the store: build out of place, then move —
                    // the same discipline mcpp.build.stage follows, so a
                    // concurrent consumer never observes a half-written entry.
                    std::error_code cpEc;
                    auto produced = subCtx->plan.outputDir / goal;
                    if (!std::filesystem::exists(produced, cpEc)) {
                        return std::unexpected(std::format(
                            "host tool '{}:{}' built but '{}' is missing",
                            depName, toolName, produced.string()));
                    }
                    std::filesystem::create_directories(binOut.parent_path(), cpEc);
                    auto tmp = binOut;
                    tmp += ".tmp";
                    std::filesystem::remove(tmp, cpEc);
                    std::filesystem::copy_file(produced, tmp,
                        std::filesystem::copy_options::overwrite_existing, cpEc);
                    if (cpEc) {
                        return std::unexpected(std::format(
                            "staging host tool '{}:{}' failed: {}",
                            depName, toolName, cpEc.message()));
                    }
                    std::filesystem::permissions(tmp,
                        std::filesystem::perms::owner_exec
                        | std::filesystem::perms::group_exec
                        | std::filesystem::perms::others_exec,
                        std::filesystem::perm_options::add, cpEc);
                    std::filesystem::rename(tmp, binOut, cpEc);
                    if (cpEc) {
                        return std::unexpected(std::format(
                            "publishing host tool '{}:{}' failed: {}",
                            depName, toolName, cpEc.message()));
                    }
                    mcpp::build::tool_store::write_entry(entry, key);
                    // The sub-build tree is large (protoc is several hundred
                    // objects) and the key covers every input, so a hit never
                    // needs it again. Removes only THIS consumer's scratch.
                    std::filesystem::remove_all(sub.work_dir, cpEc);
                    record(binOut);
                }
            }
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
            // The payload ROOT (not the driver), the target's C library, and
            // the three answers that keep a board package from hardcoding a
            // toolchain or a libc. All four in one call — see
            // fill_target_build_env.
            fill_target_build_env(bpEnv, tc ? &*tc : nullptr);
            bpEnv.toolsBin = projectSubosBin;
            bpEnv.profile      = effectiveProfile;
            bpEnv.accel        = resolvedAccel();
            if (auto dit = deviceSourcesByPackage.find(pkg.root.string()); dit != deviceSourcesByPackage.end())
                bpEnv.deviceSources = dit->second;
            bpEnv.features     = feature_closure(pkg.manifest, req, depDefaultFeatures);
            bpEnv.artifactsDir = workRoot / "target" / ".build-mcpp" / "deps"
                / (dirSafe(pkg.manifest.package.name) + "@" + pkg.manifest.package.version);
            bpEnv.genBase      = bpEnv.artifactsDir / "out";
            // mcpp#241: this package's resolved dependencies as
            // MCPP_DEP_<NAME>_DIR, from the authoritative edge graph (no
            // name-guessing); covers feature-activated deps too
            // (mergeActiveFeatureDeps folded them in before the edges were
            // recorded). Shared owner — see fillDepDirs.
            fillDepDirs(bpEnv, i);
            // …and the xlings packages this package itself declared. Its own
            // manifest, not the root's: a dependency's `[xlings] deps` is what
            // its build.mcpp asks about.
            fillXpkgDirs(bpEnv, packages[i].manifest);
            // #355: the host tools THIS package requested (resolved above).
            if (auto tit = toolEnvByConsumer.find(i); tit != toolEnvByConsumer.end())
                bpEnv.toolPaths = tit->second;
            bpEnv.hostModules = hostModulesByConsumer.count(i)
                ? hostModulesByConsumer.at(i)
                : decltype(bpEnv.hostModules){};
            auto& bcDep = pkg.manifest.buildConfig;
            const auto mark = markDirectiveTail(pkg.manifest);
            const auto ldN = bcDep.ldflags.size();
            const auto actN = bcDep.actions.size();
            const auto runnerN = bcDep.runner.size();
            auto namedBefore = bcDep.namedRunners;   // by value: the delta below
            const bool exclusiveBefore = bcDep.runExclusive;
            if (auto r = mcpp::build::run_build_program(
                    pkg.manifest, pkg.root, host->first, host->second,
                    pkg.manifest.cppStandard, bpEnv);
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
            adoptActionOutputs(pkg.manifest, pkg.root, actN);

            // Scope::RunGlobal — how the artifact is EXECUTED, forwarded to
            // the root like link flags but with the opposite merge rule.
            //
            // EXACTLY ONE provider. Link flags from two dependencies
            // concatenate and that is correct; two runners cannot — appending
            // produces an argv that is neither one's and fails at exec time
            // with nothing to say which package contributed which token. So
            // the second provider is a hard error that names BOTH, because
            // naming only the loser tells the reader half of what they need.
            // THE DEFAULT RUNNER AND EVERY NAMED ONE, BY ONE RULE.
            //
            // Link flags from two dependencies concatenate and that is correct;
            // two runners for the same name cannot — appending produces an argv
            // that is neither one's and fails at exec with nothing to say which
            // package contributed which token.
            //
            // MISSING THIS SITE IS HOW THE FEATURE FAILED FIRST. `apply()`
            // merges a package's directives into its OWN config; this is where a
            // dependency's RunGlobal entries reach the ROOT. Wiring only the
            // first left `mcpp run --runner flash` reporting "no such runner"
            // while `mcpp run` found the runner the same build program emitted
            // three lines away — measured.
            if (bcDep.runner.size() > runnerN) {
                std::vector<std::string> supplied(
                    bcDep.runner.begin() + static_cast<std::ptrdiff_t>(runnerN),
                    bcDep.runner.end());
                if (!m->buildConfig.runner.empty() && !runnerProvider.empty()) {
                    return std::unexpected(std::format(
                        "two dependencies both supply a runner for this target: "
                        "'{}' and '{}'.\n"
                        "       A runner is how the artifact is reached — there "
                        "can only be one.\n"
                        "       Drop one of them, or override both with an "
                        "explicit [target.<triple>].runner.",
                        runnerProvider, pkg.manifest.package.name));
                }
                m->buildConfig.runner = std::move(supplied);
                runnerProvider = pkg.manifest.package.name;
            }
            for (auto const& [name, nr] : bcDep.namedRunners) {
                auto before = namedBefore.find(name);
                const bool grew = (before == namedBefore.end())
                               || nr.argv.size() > before->second.argv.size()
                               || (nr.longLived && !before->second.longLived);
                if (!grew) continue;
                auto& slot = m->buildConfig.namedRunners[name];
                auto& who  = namedRunnerProvider[name];
                if (!slot.argv.empty() && !who.empty()) {
                    return std::unexpected(std::format(
                        "two dependencies both supply a runner named '{}' for "
                        "this target: '{}' and '{}'.\n"
                        "       Drop one of them, or override both with an "
                        "explicit [target.<triple>.runners].{}.",
                        name, who, pkg.manifest.package.name, name));
                }
                slot = nr;
                who  = pkg.manifest.package.name;
            }
            // A CLAIM THAT ONLY EVER TIGHTENS.
            if (bcDep.runExclusive && !exclusiveBefore)
                m->buildConfig.runExclusive = true;
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

        // EXCLUSIVE CAPABILITIES, CHECKED BEFORE REQUIREMENTS ARE BOUND.
        //
        // Ordering is deliberate. A requirement conflict is reported by naming
        // the requirement; this one exists whether or not anything requires the
        // capability, because the defect is that two implementations of one
        // interface are in the same link. Reporting it first means the message
        // names the real problem rather than a symptom of it.
        for (auto const& [cap, claimers] : capExclusive) {
            auto it = capProviders.find(cap);
            if (it == capProviders.end()) continue;
            std::vector<std::string> providers;
            for (auto const& p : it->second)
                if (std::find(providers.begin(), providers.end(), p) == providers.end())
                    providers.push_back(p);
            if (providers.size() < 2) continue;

            std::string list, claimed;
            for (auto const& p : providers) list += (list.empty() ? "" : ", ") + p;
            for (auto const& c : claimers) claimed += (claimed.empty() ? "" : ", ") + c;
            refusal::record(refusal::Code::ExclusiveCapability);
            return std::unexpected(std::format(
                "capability '{}' is provided by more than one package, and {} "
                "declares it EXCLUSIVE.\n"
                "         providers: [{}]\n"
                "         exclusive: [{}]\n"
                "       Two implementations of one interface define the same "
                "symbols, so the link would\n"
                "       resolve every call to whichever archive it reached "
                "first. Keep one of them —\n"
                "       a `[capabilities]` pin selects a provider for a "
                "REQUIREMENT and cannot make two\n"
                "       definitions of one symbol safe.",
                cap, claimers.size() == 1 ? "it" : "they", list, claimed));
        }

        // VERSION FLOORS. A package states what it needs of the machine; a
        // package that established a fact about the machine states it. Neither
        // string means anything to this code -- `cuda.driver` is data flowing
        // through -- which is why a second backend needs no change here and why
        // `test_runtime_contract`'s gate stays satisfied.
        //
        // A FLOOR WITH NO FACT IS SILENT. A machine that never declared what
        // it has is not a machine that fails the floor; it is one nobody asked.
        // Reporting a refusal there would turn "we do not know" into "no", and
        // the whole reason this exists is that a wrong answer is worse than no
        // answer.
        if (auto err = checkVersionFloors(); err) return std::unexpected(*err);

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

    mcpp::targetside::TargetSide resolvedTargetSide;
    // Whether the block below ran at all. `resolvedTargetSide` is default
    // constructed, so "no layer resolved" and "resolution has not happened"
    // read identically off its members — and the layer-conditional pass must
    // tell them apart: the first is an answer a predicate may legitimately
    // fail to match, the second means the pass has no business running.
    bool targetSideResolved = false;

    // What the packages supplying the target side's layers publish: the header
    // directories and interface flags the whole build is compiled against.
    //
    // ONE SET, TWO READERS, and that is deliberate: it is merged into every
    // package's `privateBuild` (so every compile edge sees it) and handed to
    // the `std` module's own command line (which is one more translation unit
    // of the same build). Before this existed, only the second reader was
    // written, and it derived the set itself — which is how the two could
    // describe different worlds.
    mcpp::modgraph::UsageRequirements targetSideUsage;

    // ── THE TARGET SIDE, RESOLVED ONCE ───────────────────────────────────────
    //
    // HERE AND NOT EARLIER, AND THAT IS THE WHOLE POINT.
    //
    // mcpp serves two ways of supplying a target's platform interface, C
    // library and C++ runtime, and the moment each becomes knowable is
    // opposite: a prebuilt directory is known before dependency resolution, a
    // set of packages only after it. Until now three separate derivations ran
    // at the earlier moment and guessed the later answer — the family name in
    // this file, `graphTargetSide` in flags, `graphCxxRuntime` in the contract
    // — and they disagreed on the case none of them was written for. Measured:
    //
    //   ld64.lld: error: …/lib/x86_64-unknown-linux-gnu/libc++.so:
    //                    unhandled file type
    //
    // for a pure C program crossed to macOS, whose graph supplies a C library
    // and no C++ runtime at all.
    //
    // Placing the resolution after capability binding and before the root
    // build.mcpp means every later consumer reads one value, and a build
    // program can be told what was resolved rather than re-deriving it.
    {
        namespace tsd = mcpp::targetside;

        // Scan the graph once for every layer. A package declares the layer it
        // supplies and, optionally, the interface name it answers to:
        //
        //     provides = ["mcpp:kernel-abi=openkal"]
        //
        // The engine knows the five layer names and nothing about the
        // implementations that fill them. `hosted-standard-library` is accepted
        // for the C++ layer as the spelling that shipped before this one, so an
        // existing package keeps working unchanged.
        //
        // ONE SUPPLIER PER LAYER, AND TWO IS AN ERROR RATHER THAN A PICK.
        // A C library, a kernel interface and a C++ runtime are mutually
        // exclusive choices; the same rule already governs `[build] runner` for
        // the same reason. Until this scan collected candidates instead of
        // keeping the first acceptable one, two suppliers resolved by graph
        // traversal order — an order the author neither writes nor can predict —
        // and the loser's `[build]` section still reached the command line.
        // `index` — WHICH PACKAGE this candidate is, not just its name.
        //
        // Needed once resolution is done: a layer supplied from the graph
        // publishes an include set the WHOLE build must see (see
        // `targetSideUsage` below), and reaching that package by name would be
        // a second lookup of something already in hand.
        struct Candidate { tsd::Provider p; bool direct; std::size_t index = 0; };
        std::map<int, std::vector<Candidate>> byLayer;
        std::vector<tsd::Requirement>         requirements;

        const auto& rootDeps = m->dependencies;
        auto is_direct = [&](std::string_view name) {
            for (auto const& [k, _] : rootDeps) {
                if (k == name) return true;
                // Selectors are `<namespace>.<name>` or a bare tail; a tail
                // match is what the author sees in their own manifest.
                if (k.size() > name.size() && k.ends_with(name)
                    && k[k.size() - name.size() - 1] == '.')
                    return true;
            }
            return false;
        };

        for (std::size_t pkgIndex = 0; pkgIndex < packages.size(); ++pkgIndex) {
            auto const& pkg = packages[pkgIndex];
            const auto pkgId = pkg.manifest.package.version.empty()
                ? pkg.manifest.package.name
                : std::format("{}@{}", pkg.manifest.package.name,
                              pkg.manifest.package.version);

            // EVERY PACKAGE KIND, NOT ONLY THE ONES WITH AN XPKG
            // DESCRIPTOR. `warn_unknown_xpkg_keys` reaches a dependency
            // resolved through the index; a path or git dependency carries a
            // manifest of its own and reached no warning at all, so a layer
            // this engine does not know went by in silence. This loop sees
            // every package in the graph.
            for (auto const& cap : pkg.manifest.unknownCapabilities) {
                if (&pkg == &packages.front()) continue;   // root: already refused
                // The same text the root's refusal carries, including the list
                // of layers that do exist. A warning that says less than the
                // error it replaced would be a worse diagnostic wearing a
                // milder severity.
                auto why = tsd::parse_capability(cap);
                mcpp::ui::warning(std::format(
                    "package '{}': {}\n"
                    "       Ignored, and this build proceeds without that layer. "
                    "A newer mcpp may resolve it.",
                    pkgId,
                    why ? std::format("`{}` names no capability mcpp knows.", cap)
                        : why.error()));
            }

            for (auto const& entry : pkg.manifest.provides) {
                std::optional<tsd::CapDecl> decl;
                if (auto parsed = tsd::parse_capability(entry); parsed && *parsed)
                    decl = **parsed;
                else if (entry == "hosted-standard-library")
                    decl = tsd::CapDecl{ tsd::CapLayer::CxxAbi, {} };
                if (!decl) continue;
                if (!tsd::layer_is_suppliable_by_package(decl->layer)) {
                    return std::unexpected(std::format(
                        "package '{}' declares `provides = [\"{}\"]`, and the "
                        "compiler is not a layer a package can supply.\n"
                        "       A compiler is a payload this engine installs and "
                        "drives; the differences between families are things the "
                        "engine must know rather than data a package can "
                        "describe.\n"
                        "       A package may REQUIRE one: `requires = "
                        "[\"mcpp:compiler=<family>\"]`.",
                        pkgId, entry));
                }

                tsd::Provider p;
                p.name          = pkg.manifest.package.name;
                p.version       = pkg.manifest.package.version;
                p.interfaceName = decl->interfaceName;
                p.hasStdModule  = !pkg.manifest.stdModule.empty();

                auto& slot = byLayer[static_cast<int>(decl->layer)];
                // A package may carry both spellings during the transition, and
                // the array order is the author's, not a preference. Two entries
                // from the SAME package are one supplier; the current spelling
                // names the interface and the older one cannot, so the entry
                // that carries an interface name wins.
                auto same = std::find_if(slot.begin(), slot.end(),
                    [&](const Candidate& c){ return c.p.name == p.name; });
                if (same != slot.end()) {
                    // `index` MOVES WITH `p` AND NOT ON ITS OWN. The two
                    // describe one package, and this branch is reached only
                    // from the same `pkgIndex` today — a package carrying both
                    // spellings — so they cannot differ yet. Tying them keeps
                    // it that way if a second package ever reaches here.
                    if (same->p.interfaceName.empty() && !p.interfaceName.empty()) {
                        same->p     = p;
                        same->index = pkgIndex;
                    }
                } else {
                    slot.push_back({ p, is_direct(p.name), pkgIndex });
                }
            }

            // `requires` — the symmetric half. An entry naming a layer this
            // engine does not know is an error for the same reason a `provides`
            // one is: a typo would otherwise disable a check silently.
            for (auto const& entry : pkg.manifest.requires_) {
                auto parsed = tsd::parse_capability(entry);
                // An unknown layer name is reported where the manifest was
                // read — as an error for the root and a warning for a
                // dependency — so it is skipped rather than refused twice.
                if (!parsed || !*parsed) continue;
                requirements.push_back({ pkgId, (*parsed)->layer,
                                         (*parsed)->interfaceName });
            }
        }

        for (auto const& [layerInt, slot] : byLayer) {
            if (slot.size() < 2) continue;
            tsd::Conflict c;
            c.layer     = static_cast<tsd::CapLayer>(layerInt);
            c.first     = slot[0].p.id();
            c.firstVia  = slot[0].direct ? "" : "a transitive dependency";
            c.second    = slot[1].p.id();
            c.secondVia = slot[1].direct ? "" : "a transitive dependency";
            return std::unexpected(tsd::format_conflict(c));
        }

        auto provider_of = [&](tsd::CapLayer want)
            -> std::optional<tsd::Provider> {
            auto it = byLayer.find(static_cast<int>(want));
            if (it == byLayer.end() || it->second.empty()) return std::nullopt;
            return it->second.front().p;
        };

        tsd::Inputs in;
        if (tc) {
            if (auto tt = mcpp::toolchain::triple::parse(tc->targetTriple)) {
                in.llvmTriple         = tt->llvm_triple(
                    mcpp::platform::macos::deployment_target(
                        m->buildConfig.macosDeploymentTarget));
                in.targetOs           = tt->os;
                in.targetEnv          = tt->env;
                in.freestandingTarget = tt->is_freestanding();
                // NOT `tt->envExplicit`. By this line the triple has been
                // canonicalised, and the canonical form of `x86_64-linux` is
                // `x86_64-linux-gnu` — re-parsing it reports a segment the
                // project never wrote. The request was captured upstream, where
                // the distinction still existed.
                in.requestedCAbi = requestedCAbi;
                if (!requestedCAbi.empty()) {
                    auto bare = *tt; bare.env.clear();
                    in.requestFreeTarget = bare.str();
                }
                // The segment names a different axis on each platform, and
                // saying WHICH lets the report gloss it instead of merely
                // withholding a warning. Only the C-library case can contradict
                // what the graph resolved; the other two are simply a different
                // question, and the report says so.
                in.envAxis =
                    tt->os == "linux"   ? tsd::EnvAxis::CLibrary
                  : tt->os == "windows" ? tsd::EnvAxis::ObjectAbi
                  : tt->is_freestanding() ? tsd::EnvAxis::ObjectFormat
                                          : tsd::EnvAxis::Unknown;

                // `sysroot = ""` and "no sysroot key" are different answers and
                // must not be collapsed: the first says this project wants no
                // prebuilt C library, the second says it did not say.
                if (auto const* ovr = sysroot_override(*m, *tt); ovr && ovr->empty())
                    in.sysrootDeclaredEmpty = true;
                else
                    in.sysrootXpkg = mcpp::toolchain::triple::effective_sysroot(
                        *tt, sysroot_override(*m, *tt));
            }
            in.payloadLibcRef      = tc->targetSysrootPkg;
            in.payloadCxxInterface = tc->stdlibId;
            // The compiler is a layer, and it is the one layer no package can
            // supply. It enters here so that a requirement has something to be
            // checked against and so the report can show the whole stack.
            in.compilerFamily  = std::string(tc->compiler_family());
            in.compilerVersion = tc->version;
        }
        in.compilerRuntime = provider_of(tsd::CapLayer::CompilerRuntime);
        in.kernelAbi       = provider_of(tsd::CapLayer::KernelAbi);
        in.cAbi            = provider_of(tsd::CapLayer::CAbi);
        in.cxxAbi          = provider_of(tsd::CapLayer::CxxAbi);

        // A ROW THAT LINKS THROUGH lld DIRECTLY, ON A PAYLOAD WITH NO lld.
        //
        // `x86_64-none-elf` is the only row carrying an `lldEmulation`, and its
        // column comment in mcpp.freestanding.target says why the driver is
        // bypassed for it: the driver "would hand the link to a host `g++` that
        // cannot take our linker's path".
        //
        // MEASURED TWICE ON windows-2022, AND THE SECOND TIME WAS MY OWN
        // FALLBACK. First, an empty `resolve_lld` left linker vocabulary on a
        // driver line:
        //
        //     clang++: error: unknown argument: '-m'
        //
        // Then, falling back to the driver line reproduced exactly what the
        // bypass exists to prevent:
        //
        //     clang++: error: linker (via gcc) command failed
        //     collect2.exe: error: ld returned 1 exit status
        //
        // There is no third shape. The row needs lld by name; when the
        // payload has none, the answer is a refusal at the decision, not a
        // different link.
        if (auto fsT = tc.has_value()
                     ? mcpp::toolchain::triple::parse(tc->targetTriple)
                     : std::nullopt;
            fsT && fsT->is_freestanding()) {
            auto fsSpec = mcpp::freestanding::resolve(*fsT);
            if (fsSpec && !fsSpec->lldEmulation.empty()
                && mcpp::freestanding::resolve_lld(tc->binaryPath).empty()) {
                refusal::record(refusal::Code::LldRequiredAbsent);
                return std::unexpected(std::format(
                    "target '{}' links through lld directly, and this toolchain "
                    "payload ships none.\n"
                    "       The row carries an lld emulation ('{}'), which means "
                    "the compiler driver is\n"
                    "       bypassed — for this target it would hand the link to "
                    "a host linker that\n"
                    "       cannot take a freestanding ELF.\n"
                    "       install a toolchain whose payload contains ld.lld, "
                    "or build this target\n"
                    "       from a host that has one.",
                    fsT->str(), fsSpec->lldEmulation));
            }
        }

        resolvedTargetSide = tsd::resolve(in);
        targetSideResolved = true;

        // RECORDED ON THE TOOLCHAIN THE MOMENT IT IS KNOWN, because three
        // producers of a compile line need it and only one of them can see
        // `resolvedTargetSide`.
        //
        // `flags.cppm` reads `plan.targetSide` directly; the std module build
        // (`mcpp.toolchain.stdmod`) and the build.mcpp host helper cannot —
        // they are in the toolchain layer and take a `Toolchain`. Giving them a
        // second way to derive the answer is exactly the shape this release
        // exists to remove, so the answer travels on the value they already
        // share.
        //
        // HERE AND NOT LATER: `ensure_built` runs at :7368 and every compile
        // line is assembled after it. A std BMI built against a different C
        // library than its importers is what e2e 181 catches.
        if (tc) tc->cAbiPrebuilt = resolvedTargetSide.cAbi.prebuilt();

        // ── The target side's include set is a property of the BUILD ─────────
        //
        // IT WAS ALREADY COMPUTED, AND IT REACHED EXACTLY ONE TRANSLATION
        // UNIT.
        //
        // A package that supplies a target-side layer publishes the headers the
        // whole target is built against — libc++'s, the C library's, the
        // architecture's. Those travel today as an ordinary `publicUsage`,
        // which propagates ALONG DEPENDENCY EDGES. So a workspace member that
        // depends on the provider receives them and a SIBLING DEPENDENCY
        // PACKAGE does not: `nlohmann.json` is not downstream of
        // `openkal-llvm-runtime`, it is beside it.
        //
        // The result is two flavours of BMI in one build — `std` compiled over
        // the target's libc++ (correct: the block at :7232 hands it exactly
        // this set) and the dependency packages compiled over the payload's.
        // Any unit importing both fails at the first template instantiation
        // that touches a declaration present in both header sets:
        //
        //     istream:1245: error: reference to 'space' is ambiguous
        //     note: candidate … xim-x-llvm/…/__locale:321
        //     note: candidate … openkal-llvm-runtime/…/__locale:302
        //
        // mcpp#514. Reproduced in twenty lines with no openkal at all: a path
        // package declaring `provides = ["mcpp:c++-abi=libc++"]` and one
        // `include_dirs` entry reaches the root and its own units, and reaches
        // no sibling dependency package.
        //
        // THE FIX IS THE ONE `mcpp.targetside` OPENS WITH: resolve once,
        // after the graph is known, and have every consumer read that one
        // value. A `publicUsage` describes what a library asks of ITS USERS; a
        // target side is beneath everything. Modelling the second as the first
        // is what made it edge-scoped.
        //
        // ONLY LAYERS THE GRAPH SUPPLIES. `Layer::fromGraph()` is the whole
        // condition. A payload-supplied layer already reaches every unit
        // through `mcpp.toolchain.hostflags`, and emitting it twice would put
        // the ordering of one decision in two places.
        {
            std::set<std::size_t> layerProviderIndices;
            auto note_layer = [&](tsd::CapLayer which, const tsd::Layer& resolved) {
                if (!resolved.fromGraph()) return;
                auto it = byLayer.find(static_cast<int>(which));
                if (it != byLayer.end() && !it->second.empty())
                    layerProviderIndices.insert(it->second.front().index);
            };
            note_layer(tsd::CapLayer::CompilerRuntime, resolvedTargetSide.compilerRuntime);
            note_layer(tsd::CapLayer::KernelAbi,       resolvedTargetSide.kernelAbi);
            note_layer(tsd::CapLayer::CAbi,            resolvedTargetSide.cAbi);
            note_layer(tsd::CapLayer::CxxAbi,          resolvedTargetSide.cxx);

            for (auto idx : layerProviderIndices) {
                if (idx >= packages.size()) continue;
                auto const& provider = packages[idx];
                appendUniquePaths(targetSideUsage.includeDirs,
                                  provider.publicUsage.includeDirs);
                appendUniquePaths(targetSideUsage.includeDirsAfter,
                                  provider.publicUsage.includeDirsAfter);
                appendUniqueFlags(targetSideUsage.cflags,
                                  provider.publicUsage.cflags);
                appendUniqueFlags(targetSideUsage.cxxflags,
                                  provider.publicUsage.cxxflags);
            }

            // Into `privateBuild` and NOT into `publicUsage`.
            //
            // It is visible to the whole graph already, so it needs no further
            // propagation; and writing it into `publicUsage` would fold the
            // target side into the usage requirements of any library this
            // build packages — a promise about a different machine.
            //
            // APPENDED, so a package's own directories keep coming first.
            // The target side only has to precede the DRIVER's own defaults,
            // and those are always searched last.
            if (!targetSideUsage.includeDirs.empty()
                || !targetSideUsage.includeDirsAfter.empty()
                || !targetSideUsage.cflags.empty()
                || !targetSideUsage.cxxflags.empty()) {
                for (auto& p : packages) {
                    appendUniquePaths(p.privateBuild.includeDirs,
                                      targetSideUsage.includeDirs);
                    appendUniquePaths(p.privateBuild.includeDirsAfter,
                                      targetSideUsage.includeDirsAfter);
                    appendUniqueFlags(p.privateBuild.cflags,
                                      targetSideUsage.cflags);
                    appendUniqueFlags(p.privateBuild.cxxflags,
                                      targetSideUsage.cxxflags);
                }
            }
        }

        if (auto why = tsd::check_layering(resolvedTargetSide)) {
            refusal::record(refusal::Code::LayerOrdering);
            return std::unexpected(*why);
        }
        // REQUIREMENTS ARE CHECKED BEFORE ANYTHING IS COMPILED, WHICH IS THE
        // WHOLE POINT OF DECLARING THEM. The combination this rejects — a C++
        // runtime configured for one compiler family being handed to another —
        // otherwise fails inside that runtime's own headers, in a message that
        // names a file the reader has never opened and no decision mcpp made.
        // The origin travels with the check: reaching a compiler-layer refusal
        // now means the project stated its own compiler, and the remedy has to
        // name that statement rather than a global default it is not using.
        if (auto why = tsd::check_requirements(
                resolvedTargetSide, requirements,
                tc_origin_is_user_explicit(tcOrigin) ? tc_origin_name(tcOrigin)
                                                     : std::string_view{})) {
            refusal::record(refusal::Code::LayerRequirement);
            return std::unexpected(*why);
        }
        // A WARNING, NOT A REFUSAL. The graph decides the C library either
        // way, so the segment is ignored rather than violated and the artifact
        // is the same with or without it. Refusing was tried and broke every
        // project spelling the host target `x86_64-linux-gnu` — which is what
        // `mcpp toolchain list` prints, and therefore what people write.
        if (auto why = tsd::check_request(resolvedTargetSide))
            mcpp::diag::warning("target", *why);

        // The refusal held since toolchain resolution, released now that the
        // other half of its question has an answer. A payload on this machine
        // does not produce this target; if the graph does not supply the
        // target's system either, then nothing does and the diagnosis stands.
        if (!unservedTargetDiagnosis.empty()
            && !resolvedTargetSide.system_from_graph()) {
            refusal::record(refusal::Code::HostCannotServe);
            return std::unexpected(unservedTargetDiagnosis);
        }

        // THE TARGET AND THE COMPILER ARE NOT BOUND TOGETHER, AND THE
        // TARGET ROW'S CONVENTION IS A FALLBACK RATHER THAN A RULE.
        //
        // A row pins a toolchain because the payload that toolchain belongs to
        // is what supplies THAT TARGET'S C library. A project whose target side
        // comes from its dependency graph does not use that payload, so the
        // substitution was unnecessary — and this is the first line at which
        // that is knowable, because it is the first line at which the graph
        // exists.
        //
        // The decision itself is NOT revised here. `tc` has been read and
        // mutated at 39 sites between its resolution and this point — the
        // effective triple, the cross flag, the target sysroot, the MSVC
        // runtime contract — and re-resolving it here would redo all of them
        // out of order. Deferring the CHOICE the way the target side itself was
        // deferred is the structural fix and is its own change; until then the
        // user is told what happened and how to state the preference once.
        if (!pinReplacedDefault.empty()
            && resolvedTargetSide.system_from_graph()) {
            mcpp::diag::warning("toolchain", std::format(
                "this project's target side comes from its dependency graph, so "
                "{} would have served {}.\n"
                "       mcpp used the target row's convention because the graph "
                "is not known when the\n"
                "       toolchain is chosen. State the preference for this "
                "target to skip the substitution:\n"
                "           [target.{}]\n"
                "           toolchain = \"{}\"",
                pinReplacedDefault, resolvedTargetCanonical,
                resolvedTargetCanonical, pinReplacedDefault));
        }

        // A request that cannot be honoured is said so rather than dropped.
        //
        // Measured 2026-08-23: `linkage = "dynamic"` on a project whose system
        // comes from the graph produced a statically linked artifact and
        // printed nothing. The outcome is correct — the graph supplies its
        // libraries as objects compiled into this build, and there is no shared
        // object for a loader to resolve at run time — but a directive that has
        // no effect and no diagnostic is indistinguishable from one that was
        // never read.
        //
        // THE C LIBRARY IS THE LAYER THIS DEPENDS ON, NOT "THE SYSTEM".
        // `system_from_graph()` spans two layers, and the arrangement that
        // separates them is real: a backend running ON a platform takes its
        // kernel interface from the graph while the C library stays the
        // payload's. Measured 2026-08-25 on exactly that project — the
        // predicate was true, this warning printed "The artifact is static",
        // and the artifact had three DT_NEEDED entries including `libc.so.6`.
        // The reason the message gives is a property of the C library alone:
        // a payload libc has a shared object, so `dynamic` is honoured and
        // there is nothing to warn about. Same shape as the three defects this
        // release fixes — see `TargetSide::system_from_graph`'s own note.
        if (resolvedTargetSide.cAbi.fromGraph()
            && m->buildConfig.linkage == "dynamic")
            mcpp::ui::warning(
                "`linkage = \"dynamic\"` has no effect when the "
                "target's system comes from the dependency graph: those "
                "packages are compiled into this build as objects, and there "
                "is no shared object to link against. The artifact is static.");

        // Reported, and reported HERE rather than recorded in a manifest field.
        //
        // A line a project writes states an intention, and it goes stale the
        // moment the packages beneath it change — a program that names its C
        // library by name is naming a transitive dependency it did not choose.
        // This states the outcome, so it cannot be stale, and it answers a
        // question that until now had no answer at all: reading every manifest
        // in the graph did not tell anyone what would end up on the link line,
        // because three places derived it separately and could disagree.
        //
        // AND IT PRINTS ONLY WHAT IS NOT ORDINARY. A zero-configuration build
        // resolves all five layers from one compiler payload, and five lines
        // reading `(payload)` answer a question nobody asked. `MCPP_VERBOSE`
        // prints them all; a diagnostic always does.
        // THE REQUESTED TARGET AND THE RESOLVED ONE MUST NAME THE SAME
        // OPERATING SYSTEM, AND UNTIL THIS LINE NOTHING CHECKED.
        //
        // Measured 2026-08-25 in CI, on a machine that had installed only a
        // native gcc — the report itself said it, and the build carried on:
        //
        //     Target x86_64-windows-gnu → x86_64-unknown-linux-gnu
        //     …
        //     src/stream.cpp:68:9: error: 'GetFileType' was not declared
        //
        // Two operating systems on one line. The cross payload was absent, so
        // resolution fell back to the host compiler, and Windows sources were
        // compiled for Linux; the failure surfaced a hundred lines later as an
        // undeclared identifier, naming a symbol rather than the decision.
        // openkal-uefi hit the same fallback at the linker
        // (`ld: unrecognized option '--subsystem'`).
        //
        // THE REPORT ALREADY HELD THE EVIDENCE — this asserts on it rather
        // than deriving the question again. A refusal here costs one line; the
        // alternative is a message about a Win32 function, in a file the reader
        // did not write, for a decision made in this one.
        //
        // Scope is deliberately the OS and not the whole triple: an ABI or
        // vendor difference between `x86_64-windows-gnu` and
        // `x86_64-w64-windows-gnu` is the normalisation this very line reports,
        // and refusing on it would reject every correct cross build.
        // THE NAME THE REPORT PRINTS, DERIVED ONCE AND USED BY BOTH.
        //
        // The first version of this guard read `resolvedTargetCanonical`
        // directly while the report below chose among three sources. They
        // agreed on the machine it was written on and disagreed in CI, where
        // the canonical string was empty and the report still named the target
        // from `targetDisplayName` — so the report showed the mismatch and the
        // guard, asking a different variable, saw nothing to refuse. One fact,
        // derived twice: the shape this whole release exists to remove.
        const std::string reportedTargetName =
            !targetDisplayName.empty()
                ? targetDisplayName
                : (resolvedTargetCanonical.empty()
                       ? (tc ? tc->targetTriple : std::string{})
                       : resolvedTargetCanonical);
        if (!resolvedTargetSide.llvmTriple.empty()
            && !reportedTargetName.empty()) {
            auto want = mcpp::toolchain::triple::parse(reportedTargetName);
            auto got  = mcpp::toolchain::triple::parse(
                            resolvedTargetSide.llvmTriple);
            // The inputs, when asked for. A guard that declines to fire and a
            // guard that was never reached read the same from outside.
            if (mcpp::log::is_verbose())
                mcpp::ui::info("Target", std::format(
                    "same-OS check: '{}'(os={}) vs '{}'(os={})",
                    reportedTargetName, want ? want->os : "<unparsed>",
                    resolvedTargetSide.llvmTriple, got ? got->os : "<unparsed>"));
            if (want && got && !want->os.empty() && !got->os.empty()
                && want->os != got->os) {
                refusal::record(refusal::Code::OsMismatch);
                return std::unexpected(std::format(
                    "target '{}' resolved to a toolchain for '{}'.\n"
                    "       Those are different operating systems, so nothing "
                    "built here would be for\n"
                    "       the target that was asked for. No payload on this "
                    "host produces '{}',\n"
                    "       and mcpp will not substitute the host's.\n"
                    "       install one with `mcpp toolchain install <family> "
                    "<version>`, or name it\n"
                    "       explicitly with `[target.{}] toolchain = \"…\"`.",
                    reportedTargetName, resolvedTargetSide.llvmTriple,
                    reportedTargetName, reportedTargetName));
            }
        }
        mcpp::ui::info("Target", tsd::format_report(
            resolvedTargetSide, reportedTargetName, mcpp::log::is_verbose()));
    }

    // ── L1b: conditional sections whose predicate names a target-side layer ──
    //
    // The second half of the conditional axis, and it runs HERE for the same
    // reason the root build.mcpp below does: the target side is now resolved,
    // and from this point on everything that consumes build inputs — the P1689
    // scan, the `stdModuleFlags` collection, the fingerprint, `compute_flags` —
    // reads `packages[]` and `*m`, both of which are still writable.
    //
    // EVERY PACKAGE, NOT JUST THE ROOT. The build.mcpp mirror below patches
    // `packages[0]`, which is right for build.mcpp because a build program
    // speaks for its own package and the dep loop already handled the others.
    // Here the motivating case IS a dependency — a package supplying one C++
    // runtime over several C libraries — so patching only the root would leave
    // the one package this feature exists for unserved.
    //
    // `*m` as well as the snapshots: `canonical_compile_flags(*m)` feeds the
    // fingerprint, so a contribution reaching the snapshots and not the
    // manifest would compile with flags the fingerprint does not describe.
    // MUTATING `pkg.manifest` IS NOT ENOUGH, AND THAT IS THE WHOLE
    // DIFFICULTY OF A LATE PRODUCER. `makePackageRoot` snapshots the manifest's
    // build inputs into `privateBuild` / `linkUsage`, and the compile and link
    // edges read THOSE. The build.mcpp tail below solves the identical problem
    // with `directives::mark` + `fold_private_tail`, so this uses the same two
    // helpers rather than a second mechanism — measured first: writing only
    // `pkg.manifest.buildConfig` produced a build in which every layer
    // predicate matched and no flag reached the compiler.
    if (targetSideResolved) {
        auto layerCtx = cfgCtx();
        layerCtx.layersKnown     = true;
        layerCtx.compiler        = resolvedTargetSide.compiler.interfaceName;
        layerCtx.compilerRuntime = resolvedTargetSide.compilerRuntime.interfaceName;
        layerCtx.kernelAbi       = resolvedTargetSide.kernelAbi.interfaceName;
        layerCtx.cAbi            = resolvedTargetSide.cAbi.interfaceName;
        layerCtx.cxxAbi          = resolvedTargetSide.cxx.interfaceName;
        // The root manifest feeds `canonical_compile_flags`, and therefore the
        // fingerprint: a contribution reaching the snapshots but not `*m` would
        // compile with flags the fingerprint does not describe, and the next
        // build would call that a cache hit.
        merge_layer_conditional_config(*m, layerCtx);
        for (auto& pkg : packages) {
            const auto mark  = markDirectiveTail(pkg.manifest);
            const auto ldN   = pkg.manifest.buildConfig.ldflags.size();
            const auto privN = pkg.manifest.buildConfig.privateIncludeDirs.size();
            if (!merge_layer_conditional_config(pkg.manifest, layerCtx)) continue;
            // cflags / cxxflags / include_dirs / include_dirs_after.
            foldDirectiveTailIntoPrivateBuild(pkg, pkg.manifest, mark);
            // ldflags: the link reads linkUsage.
            pkg.linkUsage.ldflags.insert(
                pkg.linkUsage.ldflags.end(),
                pkg.manifest.buildConfig.ldflags.begin()
                    + static_cast<std::ptrdiff_t>(ldN),
                pkg.manifest.buildConfig.ldflags.end());
            // private_include_dirs: expanded at makePackageRoot and folded into
            // privateBuild.includeDirs, which is what keeps them OUT of
            // publicUsage. A conditional entry has to take the same route or a
            // vendored header overlay would reach every consumer — the blast
            // radius e2e 304 exists to hold.
            for (auto it = pkg.manifest.buildConfig.privateIncludeDirs.begin()
                            + static_cast<std::ptrdiff_t>(privN);
                 it != pkg.manifest.buildConfig.privateIncludeDirs.end(); ++it) {
                if (it->is_absolute()) {
                    auto n = *it; n.make_preferred();
                    if (std::ranges::find(pkg.privateBuild.includeDirs, n)
                        == pkg.privateBuild.includeDirs.end())
                        pkg.privateBuild.includeDirs.push_back(std::move(n));
                    continue;
                }
                for (auto& dir : mcpp::modgraph::expand_dir_glob(
                         pkg.root, it->generic_string()))
                    if (std::ranges::find(pkg.privateBuild.includeDirs, dir)
                        == pkg.privateBuild.includeDirs.end())
                        pkg.privateBuild.includeDirs.push_back(dir);
            }
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
        // The payload ROOT (not the driver), the target's C library, and the
        // three answers that keep a board package from hardcoding a toolchain
        // or a libc. All four in one call — see fill_target_build_env.
        fill_target_build_env(bpEnv, tc ? &*tc : nullptr);
        bpEnv.toolsBin = projectSubosBin;
        bpEnv.profile      = effectiveProfile;
        bpEnv.accel        = resolvedAccel();
        if (auto dit = deviceSourcesByPackage.find(root->string()); dit != deviceSourcesByPackage.end())
            bpEnv.deviceSources = dit->second;
        // Set explicitly rather than relying on build_dir()'s root-relative
        // default: under BuildOverrides::work_dir the package root is shared
        // and may be read-only, and the default would write the compiled
        // helper straight into it. Same value as the default when work_dir is
        // unset, so an ordinary build is unchanged.
        bpEnv.artifactsDir = workRoot / "target" / ".build-mcpp";
        // Root mode keeps genBase empty: a relative `generated=` from the ROOT
        // package resolves against the package root (the documented contract),
        // not against OUT_DIR.
        // Same expression as the pre-move call site (and same order), so the
        // contract hash — and therefore the build.mcpp cache — is unchanged
        // across the move for feature-identical builds.
        bpEnv.features     = feature_closure(*m, parse_feature_request(overrides.features));
        // mcpp#241 (root): consumer index 0, same owner as the dep loop.
        fillDepDirs(bpEnv, 0);
        fillXpkgDirs(bpEnv, *m);
        // #355: the host tools the ROOT package requested (consumer index 0).
        if (auto tit = toolEnvByConsumer.find(0u); tit != toolEnvByConsumer.end())
            bpEnv.toolPaths = tit->second;
        bpEnv.hostModules = hostModulesByConsumer.count(0u)
            ? hostModulesByConsumer.at(0u)
            : decltype(bpEnv.hostModules){};
        auto& bcRoot = m->buildConfig;
        const auto mark = markDirectiveTail(*m);
        const auto rldN = bcRoot.ldflags.size(), rsrcN = bcRoot.sources.size(),
                   rmodN = m->modules.sources.size();
        const auto ractN = bcRoot.actions.size();
        if (auto bp = mcpp::build::run_build_program(
                *m, *root, host->first, host->second,
                m->cppStandard, bpEnv);
            !bp) {
            return std::unexpected(bp.error());
        }
        auto& pkg0 = packages[0];
        // Compile-visible tail → privateBuild: the shared fold (same owner
        // as the dep loop; the root's TUs read privateBuild).
        foldDirectiveTailIntoPrivateBuild(pkg0, *m, mark);
        // Before the source residues are mirrored below: adopting an action's
        // outputs APPENDS to bcRoot.sources, and those appends must be inside
        // the tail that gets copied into the packages[0] snapshot the scan reads.
        adoptActionOutputs(*m, *root, ractN);
        // The root's build program has spoken; a floor it stated is checked
        // now, with the facts every package (it included) established.
        if (auto err = checkVersionFloors(); err) return std::unexpected(*err);
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
            bcRoot.cflags.begin() + static_cast<std::ptrdiff_t>(mark.cflags),
            bcRoot.cflags.end());
        pkg0.manifest.buildConfig.cxxflags.insert(
            pkg0.manifest.buildConfig.cxxflags.end(),
            bcRoot.cxxflags.begin() + static_cast<std::ptrdiff_t>(mark.cxxflags),
            bcRoot.cxxflags.end());
        pkg0.manifest.buildConfig.includeDirs.insert(
            pkg0.manifest.buildConfig.includeDirs.end(),
            bcRoot.includeDirs.begin() + static_cast<std::ptrdiff_t>(mark.includeDirs),
            bcRoot.includeDirs.end());
        pkg0.manifest.buildConfig.includeDirsAfter.insert(
            pkg0.manifest.buildConfig.includeDirsAfter.end(),
            bcRoot.includeDirsAfter.begin()
                + static_cast<std::ptrdiff_t>(mark.includeDirsAfter),
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

    // A DEPENDENCY THAT DECLARED A HIGHER STANDARD THAN THE GRAPH IS BUILT AT.
    //
    // A C++ module graph has ONE standard — cross-level BMIs are hard
    // incompatible — so the root package's level is imposed graph-wide, and a
    // dependency's `standard` is parsed and then discarded. That is correct and
    // is not the defect. The defect is the silence: a package that declared
    // c++26 because it needs c++26 is compiled at whatever the consumer says,
    // and fails — if it fails at all — with a compiler error inside a
    // translation unit the user does not own, naming neither package nor the
    // mechanism.
    //
    // SCOPED TO MANIFESTS THE PROJECT AUTHOR CONTROLS, and that scope is the
    // whole reason this check is shippable. The cpp20 design doc's §9-Q3
    // declined it because the default and a declaration were indistinguishable;
    // `standardDeclared` fixes that for `mcpp.toml`, and NOT for the index:
    // measured over the local registry, every descriptor with an mcpp segment
    // declares `language` (782 of 782), and 756 of those 774 packages are C
    // libraries with `import_std = false` carrying a boilerplate "c++23". A
    // check that trusted declaredness everywhere would fire against essentially
    // the whole index for any root at c++20 — exactly the outcome §9-Q3
    // refused, reached through a different door.
    //
    // DEGRADED, NOT AN ERROR. The condition is not a proven failure: a package
    // declaring c++26 compiles perfectly well at c++23 whenever it happens not
    // to use a C++26 construct, and that is a working configuration today for
    // anyone who wrote the key aspirationally. `--strict` promotes it.
    {
        const auto graphLevel = m->cppStandard.level;
        for (std::size_t i = 1; i < packages.size(); ++i) {
            auto const& pkg = packages[i];
            if (!pkg.manifest.package.standardDeclared) continue;
            // The scope gate. A package whose root is under a store directory
            // arrived from an index and its declaration was written by a
            // descriptor generator, not by the person reading this diagnostic.
            if (mcpp::build::path_is_under_any(pkg.root, storeRoots))
                continue;
            auto declared = mcpp::manifest::normalize_cpp_standard(
                pkg.manifest.package.standard);
            if (!declared || declared->level <= graphLevel) continue;
            mcpp::diag::degraded(
                "build/standard",
                std::format("dependency `{}` declares standard = \"{}\", and "
                            "this graph is built at {}",
                            pkg.manifest.package.name,
                            declared->canonical, m->cppStandard.canonical),
                "a C++ module graph has one standard, so the dependency's "
                "declaration is not applied and its sources are compiled at the "
                "graph's level",
                std::format(
                    "raise the consumer's standard to \"{}\", or declare it "
                    "once for every member:\n\n  [workspace.package]\n  "
                    "standard = \"{}\"", declared->canonical, declared->canonical));
        }
    }

    // A DIALECT FLAG THAT REACHES EVERY TU AND NOT THE `import std` PREBUILD
    // IS A BUILD THAT CANNOT SUCCEED, AND MCPP KNOWS IT BEFORE COMPILING.
    //
    // `[build] cxxflags = ["-fno-exceptions"]` is applied to each translation
    // unit; the std BMI in `stdFlagAndDialect` is precompiled without it,
    // because only `dialect_flags()` rides that channel. Every importer then
    // fails inside a file mcpp generated:
    //
    //   std: error: language dialect differs 'C++23', expected
    //               'C++23/no-exceptions'
    //   std: error: failed to read compiled module: Bad file data
    //
    // The message names the mechanism and not the key, so the way out
    // (`dialect_cxxflags`, which IS applied to the prebuild, the scan and every
    // TU) is not discoverable from it. Both facts are known here: whether the
    // graph imports `std`, and which flags reached the prebuild.
    //
    // REFUSED RATHER THAN WARNED, and that is the same rule the host-dependence
    // diagnostics follow from the other side: this build provably cannot
    // succeed, so there is no user decision to respect. Contrast
    // `[toolchain] system`, which builds and runs and is therefore warned about.
    //
    // GATED ON `needsStdModule` — without `import std` in the graph there is no
    // prebuilt BMI to disagree with, and `-fno-exceptions` is then an ordinary
    // per-unit flag that works. A check that refused in both cases would have
    // stopped testing the condition it claims to test.
    if (needsStdModule) {
        const auto prebuilt = mcpp::toolchain::cppfly::effective_dialect_flags(
            *tc, m->cppStandard.experimental,
            mcpp::manifest::dialect_flags(m->buildConfig));
        // THE ROOT PACKAGE ONLY, and the narrowing is a correctness bound
        // rather than a shortcut.
        //
        // A dependency carrying the same flag fails identically — but only if
        // ITS OWN translation units import `std`. `needsStdModule` is a
        // property of the whole graph: a C++ wrapper package that uses no std
        // module can carry `-fno-exceptions` in its `[build] cxxflags` and
        // compile perfectly well inside a graph whose ROOT imports std.
        // Refusing there would stop a build that works, which is the one thing
        // a refusal must never do — the rule is "provably cannot build", and
        // for a dependency this evidence does not prove it.
        //
        // Extending it needs a per-package answer to "does this package import
        // std", which the scan graph holds and does not expose in that shape.
        // Recorded here so the next person meets the reason and not the gap.
        for (auto const& pkg : std::span{packages}.first(1)) {
            auto missing = mcpp::manifest::dialect_flags_missing_from_prebuild(
                pkg.manifest.buildConfig.cxxflags, prebuilt);
            if (missing.empty()) continue;
            std::string list;
            for (auto const& f : missing) {
                if (!list.empty()) list += ", ";
                list += '`'; list += f; list += '`';
            }
            // NAMES THE FLAG, NOT THE TABLE IT CAME FROM. The same flag
            // reaches the compile line from `[build] cxxflags`, from
            // `[profile.<name>] cxxflags` and from a `[target.…]` / `cfg(...)`
            // block; by the time it is read here they have been merged, and
            // asserting one of them would be wrong two times in three.
            return std::unexpected(std::format(
                "{} changes the language dialect{}, but the `import std` BMI is "
                "precompiled without it, so every importing translation unit "
                "will fail with \"language dialect differs\".\n"
                "       Declare it as a dialect flag instead — that channel is "
                "applied to the std BMI prebuild, the module scan and every TU "
                "in the graph:\n"
                "\n"
                "         [build]\n"
                "         dialect_cxxflags = [{}]\n"
                "\n"
                "       It belongs in `[build]` and not in a profile or a "
                "per-target block: a dialect the standard library was not built "
                "with cannot be held by one package or one profile alone.",
                list, std::string{},
                [&] {
                    std::string q;
                    for (auto const& f : missing) {
                        if (!q.empty()) q += ", ";
                        q += '"'; q += f; q += '"';
                    }
                    return q;
                }()));
        }
    }

    // A standard library that came from a PACKAGE brings its own module
    // source, because the compiler cannot be asked for one it does not have.
    //
    // `-print-library-module-manifest-path' is the right question when the
    // standard library is the compiler's own. It is the wrong question when
    // the library was configured by a package for a target the compiler
    // knows nothing about: the source exists, and the compiler has never
    // heard of it. So the package says where it is, and what it needs ---
    // its include path and its own __config_site, neither of which the
    // compiler would find.
    //
    // Both are read only from a package that ALSO provides the capability
    // below. A package that named a std module without supplying the
    // library would be describing something it does not have.
    for (auto& pkg : packages) {
        if (pkg.manifest.stdModule.empty()) continue;
        const auto& provs = pkg.manifest.provides;
        if (std::find(provs.begin(), provs.end(),
                      std::string{"hosted-standard-library"}) == provs.end())
            continue;
        auto src = pkg.root / pkg.manifest.stdModule;
        if (!std::filesystem::exists(src)) {
            return std::unexpected(std::format(
                "package '{}' declares [package].std-module = '{}', and there "
                "is no such file under '{}'",
                pkg.manifest.package.name, pkg.manifest.stdModule,
                pkg.root.string()));
        }
        tc->stdModuleSource   = src;
        // AND THE COMPAT MODULE, FROM THE SAME PACKAGE OR NOT AT ALL.
        //
        // `std.compat` is a second module over the SAME library. Leaving it
        // pointing at the toolchain's copy does not fail where it is set — it
        // fails later, in that copy's own headers, against a configuration that
        // was never generated for this target. Measured on a macOS cross:
        //
        //   error: std module precompile failed (rc=1):
        //     …/xim-x-llvm/22.1.8/share/libc++/v1/std.compat.cppm
        //     …/include/c++/v1/__config:13: '__config_site' file not found
        //
        // — which reads as a broken toolchain payload and says nothing about
        // the two libraries having been mixed. A package that supplies one
        // module supplies both, or the pair is not offered.
        if (!pkg.manifest.stdCompatModule.empty()) {
            auto csrc = pkg.root / pkg.manifest.stdCompatModule;
            if (!std::filesystem::exists(csrc)) {
                return std::unexpected(std::format(
                    "package '{}' declares [package].std-compat-module = '{}', "
                    "and there is no such file under '{}'",
                    pkg.manifest.package.name, pkg.manifest.stdCompatModule,
                    pkg.root.string()));
            }
            tc->stdCompatSource = csrc;
        } else {
            tc->stdCompatSource.clear();
        }
        tc->targetCxxRuntime  = true;
        tc->hasImportStd      = true;
        tc->importStdMinLevel = 20;   // libc++'s own floor; see clang.cppm
        // The target, first. On a freestanding target that means the whole ISA
        // profile --- `--target', `-march', `-mabi', `-mcmodel' --- because a
        // module built without them disagrees with every unit that imports it,
        // and clang reports that as an ABI mismatch naming a .pcm file rather
        // than the flag that split them. On a hosted one it is the triple alone.
        std::string flags;
        if (auto fs = mcpp::toolchain::triple::parse(tc->targetTriple);
            fs && fs->is_freestanding()) {
            if (auto spec = mcpp::freestanding::resolve(*fs))
                flags += mcpp::freestanding::compile_prefix(*spec, true);
        } else if (!tc->crossTargetFlag.empty()) {
            // `crossTargetFlag` and not `targetTriple`. The triple is mcpp's
            // vocabulary (`aarch64-macos`); the flag carries the spelling a
            // compiler takes (`arm64-apple-macos14.0`). Measured: emitting the
            // first produced `--target=aarch64-macos`, which clang accepts as a
            // triple it has never heard of and then treats as a bare-metal
            // aarch64 — the module and its importers would agree with each
            // other and with nothing else.
            flags += " " + tc->crossTargetFlag;
            // AND THE SECOND CHANNEL. `hostflags.cppm` reaches every ordinary
            // translation unit; this command is assembled here instead, so a
            // `std.pcm` built with SEH would be imported by units built with
            // DWARF. Same function, not a second copy of the decision.
            for (auto& f : mcpp::toolchain::graph_runtime_compile_flags(*tc))
                flags += " " + f;
        }
        // Everything up to here says which machine the module is for; what
        // follows says where its headers are. The codegen step needs only the
        // first — see Toolchain::stdModuleTargetFlags.
        tc->stdModuleTargetFlags = flags;
        for (auto& f : pkg.manifest.buildConfig.stdModuleFlags) {
            // A flag naming a path is relative to the package that named it,
            // for the same reason the module source is.
            auto candidate = pkg.root / f;
            flags += " " + mcpp::xlings::shq(
                std::filesystem::exists(candidate) ? candidate.string() : f);
        }
        // AND THE HEADERS THIS PACKAGE ITSELF IS BUILT AGAINST.
        //
        // The std module source is one of this package's translation units in
        // every way that matters, and it reaches the C library's headers the
        // same way the rest of them do --- through the requirements the packages
        // BENEATH this one publish. A package cannot name those in its own
        // manifest: they belong to its dependencies, and their paths are known
        // only after resolution.
        //
        // Measured: without them the module compiles until libc++ includes
        // <bits/alltypes.h>, which is the C library's, and stops there.
        // publicUsage rather than privateBuild: the module is compiled once and
        // imported by consumers, so the headers it must see are the ones the
        // package PUBLISHES, not the ones it happens to build itself against.
        // The two differ, and the difference is not cosmetic --- a package's own
        // build path carries directories that exist for its .cpp files and that
        // shadow the library's headers when a module is compiled against them.
        //
        // AND IT IS `targetSideUsage`, NOT THIS PACKAGE'S `publicUsage`.
        //
        // The two are the same set whenever one package supplies every layer,
        // which is the arrangement this block was written for — so reading the
        // package directly was correct and stayed correct until a second
        // provider appeared. `openkal-llvm-runtime` supplies the C++ runtime
        // while `openkal-musl` supplies the C library, and the std module needs
        // both: libc++'s own headers reach `<bits/alltypes.h>`, which is the C
        // library's.
        //
        // Reading the assembled set also makes this site and every compile
        // edge read ONE value. Deriving it here a second time is the shape
        // #233/#240/#242/#344 each cost a release, and the same set has to
        // reach both or the `std` BMI describes a different world than the
        // units importing it — which is mcpp#514 exactly.
        for (auto& d : targetSideUsage.includeDirs)
            flags += " -isystem " + mcpp::xlings::shq(d.string());
        for (auto& d : targetSideUsage.includeDirsAfter)
            flags += " -idirafter " + mcpp::xlings::shq(d.string());
        // And the definitions, for the same reason as the directories: a C
        // library's headers show a different library depending on which feature
        // macros are set, and the ones this package is built with are the ones
        // its own translation units see. Measured: without them the module
        // reaches musl's <time.h> and stops on `clockid_t', a name that header
        // declares only under the macro the package carries.
        for (auto& f : targetSideUsage.cxxflags)
            flags += " " + mcpp::xlings::shq(f);
        tc->stdModuleFlags = flags;
        break;
    }

    if (needsStdModule && !tc->hasImportStd) {
        // A freestanding target reaches here for a reason the generic message
        // gets wrong. Nothing is missing from the toolchain — libc++'s std
        // module is right there — it is that `std` is ONE module over the whole
        // library, threads and filesystem and iostreams included, so there is
        // no subset of it to build without an OS. Saying "provides no std
        // module source" sends the reader to look for a broken payload.
        //
        // The line below is copy-pasteable, and that is a PROMISE: it has
        // to resolve today. It briefly did not — an earlier version of this
        // message named `mcpplibs.std.freestanding` before any such package
        // existed, so following the advice failed at the very next command
        // with "package not found" and sent the reader off to debug their
        // index. The package is published now (103 of libc++'s 110 headers,
        // measured; the 7 that fail fail on a hosted x86_64 too), so the line
        // is back. If it is ever removed from the index, this must change with
        // it.
        //
        // And the VERSION is part of the promise, not decoration — which is
        // how the same defect recurred in a second form. The line said "0.1.0"
        // after 0.2.0 superseded it in the index, and 0.1.0 is not published,
        // so pasting it produced
        //
        //     E_NOT_FOUND: package 'compat.std-freestanding@0.1.0' not found
        //     in the synced index
        //
        // measured 2026-08-20 while documenting this message. A floor would
        // not fix it either: the request has to name a version the index
        // actually carries. Publishing a new std-freestanding means updating
        // this literal in the same change.
        // THE QUESTION IS WHETHER A HOSTED STANDARD LIBRARY IS PRESENT, NOT
        // WHETHER THE TARGET IS FREESTANDING.
        //
        // Those were the same question for as long as no one had built one for
        // such a target, and they stopped being the same when someone did:
        // `mcpplibs/openkal-llvm-runtime' configures libc++, libc++abi and
        // libunwind for a machine with no operating system, and a program above
        // it has the library this refusal says it cannot have.
        //
        // The refusal is kept, because it is right in every case where nothing
        // supplies one --- which is still the ordinary case, and the advice
        // below is still the advice. What changes is that a package can now say
        // otherwise, and it says so the way every other capability is declared:
        //
        //     provides = ["hosted-standard-library"]
        //
        // A capability rather than a triple, because the fact is a property of
        // the graph and not of the target, and because dependency resolution is
        // the earliest time at which it is known.
        const bool hostedStdProvided =
            capProviders.find("hosted-standard-library") != capProviders.end();
        if (auto ft = mcpp::toolchain::triple::parse(tc->targetTriple);
            ft && ft->is_freestanding() && !hostedStdProvided)
        {
            return std::unexpected(std::format(
                "`import std;` is not available on '{}' — a freestanding target "
                "has no hosted standard library.\n"
                "       `std` is one module over the entire library (threads, "
                "filesystem, iostreams\n"
                "       included), so there is no subset of it to build without "
                "an OS underneath.\n"
                "       Use the freestanding subset instead — an ordinary "
                "dependency carrying\n"
                "       the parts of the library that need no OS "
                "(array, span, optional, atomic,\n"
                "       string_view, ranges, expected, charconv, coroutines):\n"
                "\n"
                "           [dependencies]\n"
                "           std-freestanding = \"0.2.0\"\n"
                "\n"
                "       then `import mcpplibs.std.freestanding;` in place of "
                "`import std;`.\n"
                "       The target's C library itself comes from the BOARD "
                "package (riscv-virt-rt\n"
                "       exports `mcpplibs.riscv_virt_rt`).",
                tc->targetTriple));
        }
        return std::unexpected(std::format(
            "source imports std but toolchain '{}' provides no std module source",
            tc->label()));
    }
    // `import std` availability is two-dimensional once C++20 is a legal level:
    // having a std module source is not the same as being able to build it at
    // the project's level. Every toolchain mcpp ships answers 20; only an MSVC
    // STL older than microsoft/STL#3977 answers 23, and those users would
    // otherwise get an error from inside std.ixx.
    if (needsStdModule && tc->importStdMinLevel > 0
        && m->cppStandard.level < tc->importStdMinLevel) {
        return std::unexpected(std::format(
            "source imports std but toolchain '{}' provides the std module only "
            "from {} up, while [package].standard resolves to '{}'; raise the "
            "standard or drop `import std;`",
            tc->label(),
            mcpp::manifest::cpp_standard_level_name(tc->importStdMinLevel),
            m->package.standard));
    }

    // Compute fingerprint (no lockfile in M1 → empty hash)
    mcpp::toolchain::FingerprintInputs fpi;
    fpi.toolchain            = *tc;
    fpi.cppStandard         = m->package.standard;
    fpi.compileFlags        = canonical_compile_flags(*m)
                              + canonical_package_build_metadata(packages);
    // The module-edge schedule changes the SHAPE of build.ninja, and the fast
    // path replays that file without a plan to compare against. Folding the
    // switch into the fingerprint puts a differently-scheduled build in a
    // different directory, which makes replaying the wrong shape structurally
    // impossible instead of merely guarded. Only appended when non-default, so
    // existing build directories keep their identity.
    if (const auto sched = mcpp::build::schedule::requested_switch(*m);
        sched != "auto") {
        fpi.compileFlags += " #schedule=";
        fpi.compileFlags += sched;
    }
    // The device axis decides which sources compile and which cfg sections
    // apply, so two builds that differ only in it are two builds. Appended
    // only when set, so a project that asks for no accelerator keeps the
    // build directory it has.
    if (const auto accel = resolvedAccel(); !accel.empty()) {
        fpi.compileFlags += " #accel=";
        fpi.compileFlags += accel;
    }
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
        // #422: the CRT model reaches the std module too. Derived from the
        // SAME expression the project's TUs use (flags.cppm), through the one
        // helper, so the two cannot drift. Non-MSVC dialects yield "" and the
        // command is unchanged.
        const auto& stdDialect = mcpp::toolchain::dialect_for(*tc);
        const auto stdCrt = mcpp::toolchain::msvc_crt_flag(
            stdDialect, mcpp::toolchain::msvc_wants_static_crt(
                            m->buildConfig.linkage, m->buildConfig.cxxRuntime));
        auto sm = mcpp::toolchain::ensure_built(
            *tc, m->package.standard, stdFlagAndDialect,
            mcpp::platform::macos::deployment_target(
                m->buildConfig.macosDeploymentTarget),
            mcpp::toolchain::default_cache_root(), stdCrt);
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
    ctx.runtimeSelection = runtimeSelection;
    ctx.runtimeBinding = runtimeBindingSnapshot;
    ctx.profile     = effectiveProfile;
    ctx.activeFeatureRequest = overrides.features;
    ctx.compilerChoice = { std::string(tc_origin_name(tcOrigin)),
                           graphCompilerRequiredBy,
                           graphCompilerReplaced.empty() ? pinReplacedDefault
                                                         : graphCompilerReplaced };
    ctx.cacheMode   = cacheMode;
    ctx.projectRoot= *root;
    ctx.outputDir  = target_dir(*tc, fp, workRoot);
    ctx.stdBmi     = stdBmiPath;
    ctx.stdObject  = stdObjectPath;
    // Every directory a package payload may legitimately have been INSTALLED
    // into. There is more than one: the global registry, plus the two
    // project-local data roots a custom git index installs into
    // (`config::project_xlings_data_roots`). make_plan uses these to anchor the
    // cache address of a dependency source that lives outside its own package
    // root, and the cacheability gate below uses the same list to decide
    // whether a package's sources really came from a store. ONE definition,
    // two uses — deriving the same fact twice is how the object layout and the
    // cache key drifted apart in the first place (#344).
    // Which source trees does the fast path have to watch besides this one?
    //
    // A package whose root is neither under `projectRoot` nor under a directory
    // mcpp OWNS is a `path` dependency — the shape every workspace member takes
    // towards its siblings — and its sources are read on every build. See
    // BuildContext::depSourceRoots for what the list is for.
    //
    // WHAT IS EXCLUDED, AND WHY IT IS "WHO WROTE THE DIRECTORY" RATHER THAN
    // "WHICH KIND OF DEPENDENCY". An xpkg payload under the store is written
    // once at install time and never edited. A git checkout under
    // `<mcpp home>/git/<hash>` is a pinned revision in a hash-addressed
    // directory: changing the revision changes the directory name, and the
    // manifest that names it is already swept. Neither can change under a warm
    // build, so sweeping them would buy nothing and cost a directory walk per
    // dependency on every invocation — which is the fast path this whole change
    // exists to keep.
    //
    // A `path` dependency is the opposite on both counts: it is the user's
    // working tree, and editing it is the point.
    {
        std::vector<std::filesystem::path> owned = storeRoots;
        owned.push_back(mcpp::home::root());
        std::vector<std::filesystem::path> roots;
        for (std::size_t i = 1; i < packages.size(); ++i) {
            const auto& pkgRoot = packages[i].root;
            if (pkgRoot.empty()) continue;
            if (mcpp::build::path_is_under_any(pkgRoot, owned)) continue;
            auto normalized = pkgRoot.lexically_normal();
            if (normalized == root->lexically_normal()) continue;
            if (std::find(roots.begin(), roots.end(), normalized) == roots.end())
                roots.push_back(std::move(normalized));
        }
        ctx.depSourceRoots = std::move(roots);
    }
    // Where a runner may find the programs this project declared (#544). The
    // same resolution `fillXpkgDirs` hands to build programs, kept as
    // directories rather than env vars because the reader is mcpp's own
    // lookup, not a child process. See BuildContext::xlingsDepBinDirs.
    //
    // AND EVERY PACKAGE IN THE GRAPH, NOT ONLY THE ROOT — WHICH IS THE
    // CASE THIS FEATURE EXISTS FOR.
    //
    // A board-support package is precisely the thing that knows which emulator
    // or probe reaches its machine, and it declares that emulator under its own
    // `[xlings] deps`. Collecting only the ROOT's declarations meant a runner
    // could name a program by bare name only when the CONSUMER had also
    // declared it — which is the duplication the board package exists to
    // remove. Measured on `mcpplibs/aarch64-virt-rt`: with the board naming
    // `qemu-system-aarch64` bare, `mcpp run` searched PATH, found the shim or
    // nothing, and reported a missing runner while the emulator sat installed
    // in the payload the board had declared.
    //
    // Ordering is root-first: a consumer that declares its own payload gets to
    // decide, and a dependency supplies the answer when the consumer said
    // nothing. A payload that is declared but not installed contributes
    // nothing, and the lookup continues to PATH.
    //
    // AND THE SET COLLECTED HERE IS ALSO THE SET PROVISIONED. Looking in a
    // directory that nothing installed is a lookup that can only fail, and the
    // engine had exactly that shape: a dependency's declaration was searched
    // and never acted on. The two definitions are one expression below, so
    // they cannot drift — the third of the three hazards §12.6 named.
    {
        std::vector<std::string> xlingsSpecs = applicable_xlings_addresses(
            runtimeOwnerManifest, activeFeaturesByPackage.empty()
                ? std::vector<std::string>{} : activeFeaturesByPackage[0],
            toolPurpose, /*isRoot=*/true);
        // Everything the GRAPH declared, on the tiers this verb needs. `isRoot`
        // is false for every one of them, which is what makes `when = "dev"`
        // stop at the package that wrote it.
        std::vector<std::string> fromGraph;
        for (std::size_t i = 0; i < packages.size(); ++i) {
            const auto& man = packages[i].manifest;
            const auto feats = i < activeFeaturesByPackage.size()
                ? activeFeaturesByPackage[i] : std::vector<std::string>{};
            for (auto const& spec : applicable_xlings_addresses(
                     man, feats, toolPurpose, /*isRoot=*/i == 0)) {
                if (std::ranges::find(xlingsSpecs, spec) != xlingsSpecs.end())
                    continue;
                if (std::ranges::find(fromGraph, spec) == fromGraph.end())
                    fromGraph.push_back(spec);
            }
        }
        // THE ROOT'S OWN PASS RAN LONG AGO, AND THIS ONE MUST NOT REPEAT IT.
        // The stamp is keyed by the LIST, so provisioning root+graph together
        // would key a different list than the early pass wrote and re-run an
        // xlings round trip on every build. Only what the graph added is
        // provisioned here, under its own key.
        if (!fromGraph.empty()) {
            if (auto cfg = get_cfg()) {
                if (auto pv = provision_xlings_addresses(
                        **cfg, fromGraph, *root,
                        "[xlings.workspace] entries declared by dependencies");
                    !pv) return std::unexpected(pv.error());
            }
        }
        xlingsSpecs.insert(xlingsSpecs.end(), fromGraph.begin(), fromGraph.end());
        // What a RUN would additionally have asked for. Recorded rather than
        // installed: this verb is not running anything, and installing it
        // anyway is the behaviour the tier exists to remove.
        if (toolPurpose == ToolPurpose::Build) {
            for (std::size_t i = 0; i < packages.size() && !ctx.runTierPending; ++i) {
                const auto& man = packages[i].manifest;
                const auto feats = i < activeFeaturesByPackage.size()
                    ? activeFeaturesByPackage[i] : std::vector<std::string>{};
                for (auto const& spec : applicable_xlings_addresses(
                         man, feats, ToolPurpose::Run, /*isRoot=*/i == 0))
                    if (std::ranges::find(xlingsSpecs, spec) == xlingsSpecs.end())
                        { ctx.runTierPending = true; break; }
            }
        }
        if (!xlingsSpecs.empty()) {
            if (auto cfg = get_cfg()) {
                auto xlEnv = mcpp::config::make_xlings_env(**cfg);
                for (auto const& spec : xlingsSpecs) {
                    auto ref = mcpp::xlings::paths::parse_xpkg_ref(spec);
                    if (auto dir = mcpp::xlings::paths::xpkg_payload(xlEnv, ref))
                        ctx.xlingsDepBinDirs.push_back(*dir / "bin");
                }
            }
        }
    }
    // ─── Prebuilt dependencies: check before planning to link them ─────
    //
    // Here rather than at each place a dependency manifest is loaded, because
    // there are three of those and the check needs the RESOLVED toolchain,
    // which only exists by now. One pass over the assembled package list is
    // also the only spelling under which a package cannot be checked twice
    // with two different answers.
    //
    // The current tag's SHAPE follows the package's: a package that publishes
    // a triple-only tag is saying its interface is `extern "C"`, and comparing
    // it against a full tag would refuse a combination it explicitly allows.
    // `tag_check` already treats an unnamed dimension as don't-care, so one
    // full tag on this side is correct for both.
    {
        const auto canonicalTriple = tc->targetTriple.empty()
            ? mcpp::toolchain::triple::host_triple().str()
            : [&] {
                  auto t = mcpp::toolchain::triple::parse(tc->targetTriple);
                  return t ? t->str() : tc->targetTriple;
              }();
        auto currentTag = mcpp::pack::cxx_surface_tag(
            *tc, canonicalTriple, m->cppStandard.level);
        // What THIS build targets on the device axis. Absent means it asks for
        // no accelerator, and every artifact then satisfies it vacuously —
        // which is correct, and is why a descriptor lists its CPU-only variant
        // first: the first accepted artifact wins.
        currentTag.accel = mcpp::pack::parse_accel(resolvedAccel());
        for (std::size_t i = 1; i < packages.size(); ++i) {
            auto const& pkg = packages[i];
            if (!mcpp::pack::is_distribution_package(pkg.manifest)) continue;
            mcpp::pack::PrebuiltCheck chk{
                .packageRoot  = pkg.root,
                .packageLabel = mcpp::manifest::package_id(pkg.manifest.package).canonical(),
                .current      = currentTag,
            };
            if (auto ok = mcpp::pack::check_prebuilt(pkg.manifest, chk); !ok)
                return std::unexpected(ok.error());
        }
    }

    // ── #519: which FORM does each dependency take in this build ────────────
    //
    // The decision itself lives in `mcpp.build.linkage_form`, which is a pure,
    // table-driven function with no filesystem and no manifest knowledge. What
    // happens here is only the two halves that need this scope: collecting the
    // facts, and MATERIALISING the answer.
    //
    // MATERIALISED AS A TARGET KIND, on purpose. A dependency resolved to
    // the shared form becomes an ordinary `SharedLibrary` target, so every
    // emitter mcpp already has applies to it unchanged — the ELF soname and
    // `$ORIGIN`, the PE import library and generated `.def`, the Mach-O
    // install name. That is the whole reason this axis needs no new backend
    // code on any of the three formats. It also means `make_plan` READS the
    // answer instead of deriving it a second time.
    {
        namespace lf = mcpp::build::linkage_form;

        lf::Request request;
        if (auto parsed = lf::parse(m->buildConfig.dependencyLinkage))
            request.whole = *parsed;
        request.wholeIsExplicit = !m->buildConfig.dependencyLinkage.empty();
        // ONLY THE ROOT MANIFEST'S EDGES. See DependencySpec::linkage — a
        // package deep in the graph imposing a whole-image layout on its
        // consumer is a supply-chain property, not a convenience.
        for (auto const& [depName, spec] : m->dependencies) {
            if (spec.linkage.empty()) continue;
            if (auto parsed = lf::parse(spec.linkage)) {
                request.perPackage[depName] = *parsed;
                auto shortKey = spec.shortName.empty() ? depName : spec.shortName;
                request.perPackage.emplace(shortKey, *parsed);
            }
        }
        // A non-root edge that writes the key gets its request IGNORED, and
        // says so — a silently dropped knob is how a knob becomes decoration.
        for (std::size_t i = 1; i < packages.size(); ++i)
            for (auto const& [depName, spec] : packages[i].manifest.dependencies)
                if (!spec.linkage.empty())
                    mcpp::diag::warning("build/dependency-linkage", std::format(
                        "'{}' asks for dependency '{}' to be linked as '{}'; only "
                        "the root project decides link forms, so this is ignored",
                        packages[i].manifest.package.name, depName, spec.linkage));

        lf::TargetFacts targetFacts;
        if (auto t = mcpp::toolchain::triple::parse(tc->targetTriple))
            targetFacts.hasLoader = !t->is_freestanding();
        // The libc axis. Spelled exactly as `compute_flags` spells it, because
        // the two must agree about what `-static` means: an image linked that
        // way has no interpreter, so no shared object can ever be loaded into
        // it. Two keys with `linkage` in the name, and they are NOT independent.
        targetFacts.fullStaticLibc =
            m->buildConfig.linkage == "static"
            && mcpp::toolchain::target_supports_full_static(
                   tc->targetTriple, mcpp::platform::supports_full_static);

        std::set<std::string> packagesWithSources;
        for (auto const& unit : scan.graph.units)
            packagesWithSources.insert(unit.packageName);

        for (std::size_t i = 1; i < packages.size(); ++i) {
            auto& pkg = packages[i].manifest;
            const std::string fq = pkg.package.namespace_.empty()
                ? pkg.package.name
                : std::format("{}.{}", pkg.package.namespace_, pkg.package.name);

            lf::PackageFacts facts;
            facts.label = std::format("{}@{}", fq, pkg.package.version);
            facts.hasSources = packagesWithSources.contains(fq)
                            || packagesWithSources.contains(pkg.package.name);
            facts.carriesForeignLinkInputs =
                lf::carries_foreign_link_inputs(pkg.buildConfig.ldflags);
            facts.isDistribution = mcpp::pack::is_distribution_package(pkg);
            for (auto const& artifact : pkg.runtimeConfig.artifacts) {
                if (artifact.role == "static-library") facts.shipsStatic = true;
                if (artifact.role == "shared-library") facts.shipsShared = true;
            }
            std::vector<mcpp::manifest::Target*> libraryTargets;
            for (auto& t : pkg.targets) {
                if (t.kind == mcpp::manifest::Target::SharedLibrary)
                    facts.declaredShared = true;
                if (t.kind == mcpp::manifest::Target::Library)
                    libraryTargets.push_back(&t);
            }

            // A consumer addresses a dependency by whatever it wrote in
            // `[dependencies]` — the fully-qualified name or the bare one —
            // while every message wants the version too. Rather than swapping
            // the label to whichever spelling matches (which drops the version
            // from every refusal), make the request answer to the descriptive
            // label as well.
            for (auto const& key : { fq, pkg.package.name }) {
                if (auto it = request.perPackage.find(key);
                    it != request.perPackage.end()) {
                    request.perPackage.emplace(facts.label, it->second);
                    break;
                }
            }
            auto allowed = lf::admissible(facts, targetFacts);
            auto answer  = lf::resolve(facts, allowed, request);

            if (!answer.diagnostic.empty())
                mcpp::diag::degraded("build/dependency-linkage", answer.diagnostic,
                    "this dependency is linked in the other form, which changes "
                    "whether its code travels inside the images that use it");

            if (answer.linkage != lf::DepLinkage::Shared) continue;
            if (facts.isDistribution) continue;   // nothing here to build
            // A package that ALREADY declares a shared target has decided
            // for itself, and its remaining library targets are not part of
            // that decision. Flipping them would change what such a package
            // builds under the DEFAULT request, which is the one property this
            // axis promises never to touch. (No package in mcpp-index has both
            // shapes at once — compat.vulkan's `lib` is overridden to `shared`
            // on Linux rather than joined by it — but "unreachable today" is
            // how the last few of these got in.)
            if (facts.declaredShared) continue;
            for (auto* t : libraryTargets)
                t->kind = mcpp::manifest::Target::SharedLibrary;
        }
    }

    auto planResult = mcpp::build::make_plan(*m, *tc, fp, scan.graph, report.topoOrder,
                                             packages, *root, ctx.outputDir,
                                             stdBmiPath, stdObjectPath, storeRoots);
    if (!planResult) return std::unexpected(planResult.error());
    ctx.plan        = std::move(*planResult);
    // Resolved far above, where the dependency graph first exists. It is
    // attached here rather than threaded through `make_plan` because nothing
    // that function does depends on it: the flag assembly that does reads the
    // plan, and every reader of `compute_flags` runs after this line.
    ctx.plan.targetSide = resolvedTargetSide;
    // The module graph outlives the plan for one consumer: `mcpp pack`, which
    // has to know which units are INTERFACE (published as source) and which
    // are implementation (published only as an object). The plan flattens that
    // away — a CompileUnit records what to compile, not what it provides — so
    // the packer would otherwise have to scan the tree a second time and could
    // then disagree with the build about what the package even contains.
    ctx.graph       = std::move(scan.graph);
    // mcpp#407. Both callers that produce a non-plain graph arrive here the
    // same way: dev-dependencies enabled, synthetic test targets appended. The
    // resulting `default` line names the test binaries and omits the package's
    // own target, and the output directory is shared with plain builds because
    // the fingerprint covers neither input. Stamping it on the plan is what
    // lets the graph say so about itself.
    ctx.plan.graphShape = (includeDevDeps || !extraTargets.empty())
        ? mcpp::build::GraphShape::WithTests
        : mcpp::build::GraphShape::Normal;
    // The device variant an override chose is stamped for the same reason: the
    // fast path runs without overrides, so a graph written under one must not
    // be the graph it replays.
    ctx.plan.accelOverridden = !overrides.accel.empty();
    // Resolve the module-edge schedule ONCE, here, where both the toolchain and
    // the manifest are in hand. The backend writes the graph in this shape, the
    // graph records the tag, and `mcpp build --verbose` prints the reason — all
    // three read this, none of them re-derives it.
    {
        const auto decision = mcpp::build::schedule::decide(
            ctx.plan.toolchain,
            // Warned HERE and not at the fingerprint call above, which reads the
            // same switch a few hundred lines earlier: both get the normalised
            // value, only one of them says anything, so a typo produces exactly
            // one warning rather than two identical ones.
            mcpp::build::schedule::requested_switch(*m, [](std::string_view bad) {
                mcpp::ui::warning(std::format(
                    "ignoring invalid bmi_schedule '{}' (expected \"auto\", \"on\" or \"off\")", bad));
            }),
            mcpp::build::schedule::resolve_jobs(*m, [](std::string_view bad) {
                mcpp::ui::warning(std::format(
                    "ignoring invalid job count '{}' (expected a positive number or 'auto')", bad));
            }),
            // What this machine would pick if asked. Impure, so it is resolved
            // here and handed to the pure `decide`. Only DetachCodegen uses it,
            // and only when the user gave no job count — without it that
            // strategy ships `sched_cap = 0`, which disables the semaphore that
            // is its ONLY bound on how many compilers run at once.
            mcpp::platform::capacity::recommended_jobs(
                mcpp::platform::capacity::host_capacity()));
        ctx.plan.scheduleTag         = std::string(mcpp::build::schedule::to_string(decision.strategy));
        ctx.plan.scheduleNinjaJobs   = decision.ninjaJobs;
        ctx.plan.scheduleCompilerCap = decision.compilerCap;
        mcpp::log::verbose("build", std::format("schedule: {} — {}",
                                                ctx.plan.scheduleTag, decision.reason));

    }
    ctx.plan.runtimeBinding = runtimeBindingSnapshot;
    mcpp::build::merge_runtime_binding_contract(
        ctx.plan, runtimeBindingSnapshot);
    ctx.plan.compileDbPath = workRoot / "compile_commands.json";
    // GCC: a clean `*link:` for this build, so the payload's specs cannot
    // inject other homes' rpath entries into the artifact. AFTER the plan is
    // moved in — an earlier assignment was silently overwritten by that move,
    // which produced a generated file that nothing ever passed to the driver.
    // Generated here rather than in compute_flags, which runs twice per build.
    if (tc->compiler == mcpp::toolchain::CompilerId::GCC)
        ctx.plan.gccCleanSpecs = mcpp::toolchain::write_clean_link_specs(
            tc->binaryPath, ctx.outputDir);

    // ── Declared build-graph nodes → the plan ───────────────────────────────
    //
    // Collected here rather than inside make_plan because the engine-variable
    // vocabulary an action may reference includes values that only exist once
    // the plan does (outputDir is fingerprint-derived; a target's file name is
    // a link unit's output).
    //
    // The vocabulary is CLOSED on purpose. An action's command is an argv, not
    // a shell string, and the only interpolations are these four — which is
    // what makes an action portable (Windows has no shell to assume) and
    // cacheable (nothing can smuggle in ambient state).
    {
        // An engine variable that resolves to nothing must be an ERROR, not an
        // empty string: `${mcpp.target_file:tpyo}` would otherwise silently
        // become an edge with a blank path, and ninja reports that far away
        // from the typo that caused it.
        std::set<std::string> unresolvedTargets;
        auto substitute = [&](std::string s) {
            auto rep = [&](std::string_view what, const std::string& with) {
                for (std::size_t p; (p = s.find(what)) != std::string::npos; )
                    s.replace(p, what.size(), with);
            };
            rep("${mcpp.out_dir}",    ctx.plan.outputDir.string());
            rep("${mcpp.bin_dir}",    (ctx.plan.outputDir / "bin").string());
            rep("${mcpp.compile_db}", ctx.plan.compileDbPath.string());
            constexpr std::string_view kTf = "${mcpp.target_file:";
            for (std::size_t p; (p = s.find(kTf)) != std::string::npos; ) {
                auto close = s.find('}', p);
                if (close == std::string::npos) break;
                auto name = s.substr(p + kTf.size(), close - p - kTf.size());
                // The link unit's BUILD-DIR-RELATIVE output, not an absolute
                // path. ninja identifies a file by the string an edge declares,
                // and the link edge declares `bin/app`; an absolute reference
                // to the same bytes is a DIFFERENT node, which ninja reports as
                // "missing and no known rule to make it". Commands run with
                // cwd = the build dir, so the relative form is also what the
                // tool being invoked should receive.
                std::string resolved;
                for (auto const& lu : ctx.plan.linkUnits)
                    if (lu.targetName == name)
                        resolved = lu.output.generic_string();
                if (resolved.empty()) unresolvedTargets.insert(name);
                s.replace(p, close - p + 1, resolved);
            }
            return s;
        };
        auto collect = [&](const mcpp::manifest::Manifest& mm) {
            // The declaring package, recorded here because this is the only
            // place that knows it: the build program emitted the action, and a
            // program has no idea which package the engine loaded it for.
            // mcpp#534's ordering edge is scoped to this name.
            auto owner = mcpp::build::qualified_package_name(mm);
            for (auto a : mm.buildConfig.actions) {
                for (auto& x : a.inputs)  x = substitute(x);
                for (auto& x : a.outputs) x = substitute(x);
                for (auto& x : a.command) x = substitute(x);
                a.packageName = owner;
                ctx.plan.actions.push_back(std::move(a));
            }
        };
        collect(*m);
        for (std::size_t i = 1; i < packages.size(); ++i)
            collect(packages[i].manifest);
        if (!unresolvedTargets.empty()) {
            std::string bad, known;
            for (auto const& n : unresolvedTargets) bad += (bad.empty() ? "" : ", ") + n;
            for (auto const& lu : ctx.plan.linkUnits)
                known += (known.empty() ? "" : ", ") + lu.targetName;
            return std::unexpected(std::format(
                "build.mcpp action references unknown target(s) via "
                "${{mcpp.target_file:...}}: {}\n"
                "  targets in this build: [{}]\n"
                "  (a target gated by required_features is absent unless those "
                "features are active)",
                bad, known.empty() ? std::string("none") : known));
        }

        // role = "object": the outputs are LINK inputs, so attach them to the
        // link units that should receive them.
        //
        // The strings are pushed VERBATIM. ninja identifies a file by the string
        // an edge declares, and the action edge declares whatever
        // prepare_actions produced (an absolute path); handing the link edge a
        // prettier relative spelling of the same bytes creates a second node and
        // "missing and no known rule to make it" — the same trap
        // ${mcpp.target_file:} documents just above.
        std::set<std::string> unknownObjectTargets;
        for (auto const& a : ctx.plan.actions) {
            if (a.role != mcpp::manifest::BuildAction::Role::Object) continue;

            // Validate EVERY named target, not just the case where none of them
            // matched. Gating the check on "nothing attached" meant
            // `.target("app").target("aap")` attached to `app` and dropped the
            // typo without a word — while both the type comment and the docs
            // promise an unknown name is an error. A per-name check is also the
            // only one that scales: the failure it catches is a target that
            // exists in one configuration and not another.
            for (auto const& t : a.targets) {
                bool known = false;
                for (auto const& lu : ctx.plan.linkUnits)
                    if (lu.targetName == t) { known = true; break; }
                if (!known) unknownObjectTargets.insert(t);
            }

            bool attached = false;
            for (auto& lu : ctx.plan.linkUnits) {
                // Empty targets = every LINKED IMAGE, and a test binary is one.
                // Excluding it made `mcpp build` succeed while `mcpp test` died
                // with `undefined symbol` on the very symbol the action exists
                // to provide — the library code under test links the same
                // objects, so a blob/`.def`/pre-built `.o` has to reach it too.
                // Naming the test target instead is not a workaround: test link
                // units are DISCOVERED from tests/*.cpp, so their names are not
                // in mcpp.toml and a build.mcpp that spells one stops building
                // under plain `mcpp build`, where that unit does not exist.
                // (`[resources]` makes the opposite call on purpose: an icon
                // belongs to what ships, not to a test runner.)
                //
                // A STATIC LIBRARY IS ONE OF THEM, and leaving it out was
                // the whole of what C-6 needed. A package whose device code is
                // its point -- ggml's CUDA backend is 305 `.cu` files behind a
                // `kind = "lib"` target -- emitted its actions, watched every
                // one of them be dropped with a warning, and produced an
                // archive with no device code in it. The archive rule already
                // consumes `lu.objects`, so the objects an action produced
                // belong there for exactly the reason a compiled `.cpp`'s do:
                // the target's content is what it was told to contain.
                const bool image = lu.kind == mcpp::build::LinkUnit::Binary
                                || lu.kind == mcpp::build::LinkUnit::SharedLibrary
                                || lu.kind == mcpp::build::LinkUnit::StaticLibrary
                                || lu.kind == mcpp::build::LinkUnit::TestBinary;
                const bool wanted = a.targets.empty()
                    ? image
                    : std::find(a.targets.begin(), a.targets.end(),
                                lu.targetName) != a.targets.end();
                if (!wanted) continue;
                for (auto const& o : a.outputs) lu.objects.emplace_back(o);
                attached = true;
            }

            // No consumer at all. The edge is excluded from `actionDefaults`
            // (its outputs are supposed to be reachable through a link edge), so
            // this is not "builds but unused" — the command never runs and the
            // build says nothing. Same shape, and same diagnostic, as
            // `resources/no-image`.
            if (!attached && a.targets.empty()) {
                mcpp::diag::degraded("action/no-target", std::format(
                    "build.mcpp action '{}' has role = \"object\" but this build "
                    "produces no target to put its outputs into",
                    a.id.empty() ? "<unnamed>" : a.id),
                    "the action never runs and its outputs are never produced",
                    "add a [targets.<name>] — a bin, a lib, a shared lib or a "
                    "test all take one — or name the targets explicitly with "
                    ".target(\"…\")");
            }
        }
        if (!unknownObjectTargets.empty()) {
            std::string bad, known;
            for (auto const& n : unknownObjectTargets) bad += (bad.empty() ? "" : ", ") + n;
            for (auto const& lu : ctx.plan.linkUnits)
                known += (known.empty() ? "" : ", ") + lu.targetName;
            return std::unexpected(std::format(
                "build.mcpp action with role = \"object\" names unknown "
                "target(s): {}\n"
                "  targets in this build: [{}]\n"
                "  (a target gated by required_features is absent unless those "
                "features are active; test binaries exist only under `mcpp "
                "test`, so name none and the outputs reach every target "
                "including them)",
                bad, known.empty() ? std::string("none") : known));
        }
    }
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
            if (cu.kind == mcpp::SourceKind::GasAsm)       hasGas = true;
            else if (cu.kind == mcpp::SourceKind::NasmAsm) hasNasm = true;
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

    // ─── Windows resources: [resources] → a tracked link input (mcpp#365) ──
    //
    // Four rules, in this order:
    //   1. Only the ROOT package's [resources] is read. A dependency's version
    //      resource would fight its consumer's for ordinal 1, and a dependency
    //      that produces no PE image of its own has nothing to embed into.
    //   2. A DECLARED FILE THAT DOES NOT EXIST IS AN ERROR — on EVERY target.
    //      Whether a path exists is a fact about the working tree, not about
    //      the target; gating it on is_pe() meant a Linux or macOS CI could not
    //      see a typo in `icon = …` at all and only the Windows job went red,
    //      which is the same "find out late" failure the hard error exists to
    //      remove. Existence is checked everywhere; only COMPILATION is PE-only.
    //   3. On a non-PE target nothing is compiled — no units, no warning,
    //      byte-identical build. This is what makes `cfg(windows)` unnecessary
    //      (and it could not be used anyway: the conditional channel carries
    //      BuildInputs only).
    //   4. Nothing to embed into (an archive-only package) → say so and stop.
    if (m->resources.declared()) {
        namespace rsrc = mcpp::build::resources;
        const auto& R = m->resources;

        // Rule 2 — target-independent, so it runs before the is_pe() gate.
        auto resolve_declared = [&](const std::filesystem::path& p,
                                    std::string_view key)
            -> std::expected<std::filesystem::path, std::string>
        {
            // Lexical, not weakly_canonical: canonicalising resolves symlinks,
            // and a symlinked source tree would then bake a different path into
            // the generated script than the one the user wrote. (Same reason
            // mcpp#344 made the cache anchor lexical.)
            auto abs = (p.is_absolute() ? p : (*root / p)).lexically_normal();
            std::error_code ec;
            if (!std::filesystem::is_regular_file(abs, ec))
                return std::unexpected(std::format(
                    "[resources] {} = \"{}\" does not exist (looked at {}).\n"
                    "  A declared resource is a build input like any other "
                    "source: mcpp will not quietly ship a binary without it. "
                    "Remove the key if the resource is not wanted.",
                    key, p.generic_string(), abs.generic_string()));
            return abs;
        };

        std::filesystem::path iconAbs;
        if (!R.icon.empty()) {
            auto r = resolve_declared(R.icon, "icon");
            if (!r) return std::unexpected(r.error());
            iconAbs = *r;
        }
        std::vector<std::filesystem::path> extraInputs;
        for (auto const& e : R.extraInputs) {
            auto r = resolve_declared(e, "extra-inputs");
            if (!r) return std::unexpected(r.error());
            extraInputs.push_back(*r);
        }
        std::vector<std::filesystem::path> scriptFiles;
        for (auto const& f : R.files) {
            auto r = resolve_declared(f, "files");
            if (!r) return std::unexpected(r.error());
            scriptFiles.push_back(*r);
        }

        const auto trip = mcpp::toolchain::triple::parse(tc->targetTriple)
                              .value_or(mcpp::toolchain::triple::host_triple());

        // Rules 3 and 4 are early returns rather than nesting: the body below is
        // ~150 lines and an `else` around all of it reads as an accident.
        auto plan_resources = [&]() -> std::expected<void, std::string> {
            const auto  dialectId = mcpp::toolchain::dialect_for(*tc).id;
            const bool msvcStyle = (dialectId == "msvc");
            const std::string_view outExt = msvcStyle ? ".res" : ".o";
            const auto resDir = ctx.plan.outputDir / "res";
            std::error_code mkEc;
            std::filesystem::create_directories(resDir, mkEc);

            // Which link units embed resources: images, not archives. A `.res`
            // inside a static library is dropped by every linker that reads one.
            // Test binaries are images too, but deliberately excluded: an icon
            // and an OriginalFilename belong to what the project SHIPS, and a
            // test executable is not that. (`role = "object"` makes the opposite
            // call, for the opposite reason — see its note above.)
            std::vector<std::size_t> peUnits;
            for (std::size_t i = 0; i < ctx.plan.linkUnits.size(); ++i) {
                auto k = ctx.plan.linkUnits[i].kind;
                if (k == mcpp::build::LinkUnit::Binary ||
                    k == mcpp::build::LinkUnit::SharedLibrary)
                    peUnits.push_back(i);
            }
            // Nothing to embed into. Compiling the scripts anyway would leave
            // orphan edges nothing depends on, and demanding a resource
            // compiler for them would fail a build that has no use for one.
            // A degradation, not a warning: the user asked for something and
            // got nothing, so `--strict` should see it.
            if (peUnits.empty()) {
                mcpp::diag::degraded("resources/no-image", std::format(
                    "[resources] is declared but '{}' produces no executable or "
                    "shared library for {}", m->package.name, trip.str()),
                    "nothing embeds the icon or the version metadata",
                    "add a [targets.<name>] with kind = \"bin\" or \"shared\", "
                    "or drop the [resources] section");
                return {};
            }

            // Two scripts with the same stem in different directories would
            // otherwise write the same artifact — a silent "multiple rules
            // generate" that ninja reports far from the cause.
            std::set<std::string> usedStems;
            auto add_unit = [&](const std::filesystem::path& src,
                                std::string_view stem,
                                std::vector<std::filesystem::path> inputs,
                                std::size_t attachTo)
                -> std::expected<void, std::string>
            {
                if (!usedStems.insert(std::string(stem)).second)
                    return std::unexpected(std::format(
                        "[resources] two resource scripts are named '{}.rc'; "
                        "they would produce the same artifact. Rename one.", stem));
                mcpp::build::ResourceUnit ru;
                ru.source = src;
                ru.output = std::filesystem::path("res") /
                            (std::string(stem) + std::string(outExt));
                ru.implicitInputs = std::move(inputs);
                ctx.plan.resourceUnits.push_back(std::move(ru));
                const auto& out = ctx.plan.resourceUnits.back().output;
                if (attachTo == static_cast<std::size_t>(-1)) {
                    for (auto i : peUnits) ctx.plan.linkUnits[i].objects.push_back(out);
                } else {
                    ctx.plan.linkUnits[attachTo].objects.push_back(out);
                }
                return {};
            };

            // Author-written scripts: compiled once, linked into every image.
            for (auto const& rcSrc : scriptFiles) {
                auto scan = rsrc::scan_rc(rcSrc);
                if (scan.versionInfoNamedByString) {
                    // The mcpp#365 silent failure, caught on the way in. A
                    // degradation rather than a warning: the impact is exactly
                    // the thing this feature exists to remove — a shipped binary
                    // whose version metadata Windows cannot read — so a build
                    // that asked for `--strict` must not pass over it.
                    mcpp::diag::degraded("resources/versioninfo", std::format(
                        "{}: `{} VERSIONINFO` names the version resource '{}' "
                        "instead of ordinal 1",
                        rcSrc.filename().generic_string(), scan.versionInfoName,
                        scan.versionInfoName),
                        "Windows will not find it — GetFileVersionInfo looks up "
                        "MAKEINTRESOURCE(1) and every field comes back empty, "
                        "while every tool that prints the resource TYPE still "
                        "says it is fine",
                        "VS_VERSION_INFO is a macro from <windows.h>; add "
                        "`#include <windows.h>` to the script, or write "
                        "`1 VERSIONINFO`");
                }
                for (auto const& g : scan.gaps) {
                    mcpp::diag::degraded("resources/inputs",
                        std::format("{}: `{}` names its file through a macro, so "
                                    "mcpp cannot track it",
                                    rcSrc.filename().generic_string(), g),
                        "editing that file will not trigger a rebuild",
                        "list it in [resources] extra-inputs = [...]");
                }
                auto inputs = std::move(scan.inputs);
                inputs.insert(inputs.end(), extraInputs.begin(), extraInputs.end());
                if (auto a = add_unit(rcSrc, rcSrc.stem().string(),
                                      std::move(inputs),
                                      static_cast<std::size_t>(-1)); !a)
                    return std::unexpected(a.error());
            }

            // The synthesised script: per image, because OriginalFilename and
            // the version block belong to a specific artifact.
            if (!iconAbs.empty() || R.synthesize_version_info()) {
                // A version key mcpp cannot order (an upstream build number)
                // leaves FILEVERSION's four numeric fields at zero while the
                // string fields keep the real text. Say so — the properties
                // dialog will disagree with `[package].version` and nothing
                // else would explain why.
                if (R.synthesize_version_info() && !m->package.version.empty()
                    && !mcpp::version_req::parse_version(m->package.version)) {
                    mcpp::diag::degraded("resources/version",
                        std::format("[package].version = \"{}\" has no numeric "
                                    "form", m->package.version),
                        "the embedded FILEVERSION / PRODUCTVERSION fields are "
                        "0,0,0,0 (the string fields keep the real version)",
                        "set [resources.version-info] explicitly, or use a "
                        "dotted numeric version");
                }
                for (auto i : peUnits) {
                    const auto& lu = ctx.plan.linkUnits[i];
                    auto text = rsrc::synthesize_rc(
                        m->package, R, lu.output.filename().string(), iconAbs);
                    if (!text) return std::unexpected(text.error());
                    // A stable path, so `cp` + `files = [...]` reproduces the
                    // same resource byte for byte (the L0→L1 escape hatch).
                    auto rcPath = resDir / (lu.targetName + ".mcpp.rc");
                    // Write only on change: rewriting unconditionally would
                    // relink on every build.
                    std::string existing;
                    if (std::ifstream in(rcPath, std::ios::binary); in)
                        existing.assign(std::istreambuf_iterator<char>(in), {});
                    if (existing != *text) {
                        std::ofstream os(rcPath, std::ios::binary);
                        if (!os) return std::unexpected(std::format(
                            "cannot write generated resource script '{}'",
                            rcPath.string()));
                        os << *text;
                    }
                    std::vector<std::filesystem::path> inputs;
                    if (!iconAbs.empty()) inputs.push_back(iconAbs);
                    inputs.insert(inputs.end(), extraInputs.begin(), extraInputs.end());
                    if (auto a = add_unit(rcPath, lu.targetName + ".mcpp",
                                          std::move(inputs), i); !a)
                        return std::unexpected(a.error());
                }
            }

            if (ctx.plan.resourceUnits.empty()) return {};

            // Lazy + hard failure, exactly like nasm: a dropped resource
            // surfaces as "where did my icon go", which is unattributable.
            auto tool = rsrc::find_rc_tool(*tc, dialectId);
            if (!tool) {
                return std::unexpected(std::format(
                    "[resources] needs a Windows resource compiler for the "
                    "{} toolchain targeting {}, and none was found next to "
                    "{}.\n  Expected {} in the toolchain's own bin directory "
                    "(mcpp does not search PATH for build tools).",
                    dialectId, trip.str(), tc->binaryPath.string(),
                    msvcStyle ? "rc.exe or llvm-rc"
                              : "<triple>-windres, windres or llvm-windres"));
            }
            ctx.plan.rcPath  = tool->path;
            ctx.plan.rcStyle = tool->style;

            // UTF-8 input, always. `[package]` metadata is user text and
            // routinely non-ASCII; without this llvm-rc refuses the script
            // outright ("Non-ASCII 8-bit codepoint can't be interpreted in
            // the current codepage") rather than mangling it, so a project
            // with a Chinese description could not build at all.
            ctx.plan.rcFlags.push_back(msvcStyle ? "/C" : "--codepage=65001");
            if (msvcStyle) ctx.plan.rcFlags.push_back("65001");

            // Include search: the project first, then whatever the toolchain
            // puts on INCLUDE. llvm-rc preprocesses but does NOT read INCLUDE
            // (rc.exe does), so the SDK dirs have to be spelled out for it —
            // that is what makes `#include <windows.h>` work, and it is the
            // supported way to get VS_VERSION_INFO defined.
            const std::string ip = msvcStyle ? "/I" : "-I";
            ctx.plan.rcFlags.push_back(ip + root->string());
            for (auto const& d : m->buildConfig.includeDirs) {
                auto abs = d.is_absolute() ? d : (*root / d);
                ctx.plan.rcFlags.push_back(ip + abs.string());
            }
            if (msvcStyle && tool->name().find("llvm-rc") != std::string::npos) {
                for (auto const& ev : tc->envOverrides) {
                    if (ev.key != "INCLUDE") continue;
                    // Shared splitter: `;` only. See rsrc::split_env_list —
                    // the drive colon is not a separator.
                    for (auto dir : rsrc::split_env_list(ev.value))
                        ctx.plan.rcFlags.push_back(ip + std::string(dir));
                }
            }
            return {};
        };

        if (trip.is_pe())
            if (auto r = plan_resources(); !r) return std::unexpected(r.error());
    }

    // ─── Global dependency cache: per-package keys, hit → stage edges ──
    //
    // Every index package gets a key over the axes that actually reach its
    // compiler command lines (mcpp.build.cache_key), computed bottom-up so a
    // package's key includes its direct dependencies' keys. A hit marks that
    // package's compile units `servedFromCache`, and the ninja backend emits
    // `stage_file` edges instead of compile edges for them — which is the only
    // way ninja will accept a cached artifact. A miss records a populate task
    // for after the build.
    //
    // `--cache=local|off` skips this block entirely: nothing is read and, in
    // run_build_plan, nothing is written.
    auto cfg2 = get_cfg();
    if (cfg2 && ctx.cacheMode == CacheMode::Global) {
        std::error_code mkEc;
        std::filesystem::create_directories(ctx.outputDir, mkEc);

        // NOTE (mcpp#344): there is deliberately no local "derive the entry
        // address from the object path" helper here any more. There used to be
        // one, and it was the SECOND derivation of a fact plan.cppm already
        // owns — it stripped `obj/` off the consumer's build path, so the entry
        // layout followed the consumer's package mix while the key did not.
        // `CompileUnit::packageObjectRel` is now the only answer to "where does
        // this object live inside a cache entry", and it is computed in exactly
        // one place. Do not reintroduce a second one.

        // ── Per-package keys, bottom-up ──────────────────────────────────
        // Axes A/B/C are whole-graph, so they are computed once. Axes D/E are
        // per package. Axis F is each direct dependency's key, which forces a
        // bottom-up order: `dependencyEdges` is a DAG (the modgraph validator
        // rejects cycles), so a simple memoized recursion suffices — with an
        // explicit in-progress guard so a cycle that slipped past validation
        // fails loudly instead of recursing until the stack dies.
        namespace ck = mcpp::build::cache_key;
        auto axes = ck::build_axes(
            *tc, *m, stdFlagAndDialect,
            mcpp::toolchain::cppfly::effective_dialect_flags(
                *tc, m->cppStandard.experimental,
                mcpp::manifest::dialect_flags(m->buildConfig)),
            mcpp::platform::macos::deployment_target(
                m->buildConfig.macosDeploymentTarget),
            // The GLOBAL registry root — the same one `fill_package_config`
            // relativizes against below, so both halves of the key describe
            // payload paths the same way.
            storeRoots.empty() ? std::filesystem::path{} : storeRoots.front(),
            // The bit `make_plan` decided and `compute_flags` emits. Reading
            // it here rather than re-deriving is what keeps the objects a
            // cache entry HOLDS and the objects a build ASKS FOR describable
            // by one sentence.
            ctx.plan.needsPic);

        // Sources belonging to each package, package-root-relative and sorted.
        std::vector<std::vector<std::string>> pkgSources(packages.size());
        for (auto& cu : ctx.plan.compileUnits) {
            // Longest matching root wins. Package roots can nest — a workspace
            // member lives under the workspace root — and taking the first match
            // would file the member's sources under the outer package, putting
            // them in the wrong key. (Index payloads live in the xpkgs store and
            // cannot be shadowed this way, so no cached entry is affected today;
            // resolving it by specificity rather than by iteration order is what
            // keeps that true if roots ever move.)
            std::size_t best = packages.size();
            std::size_t bestLen = 0;
            std::string bestRel;
            for (std::size_t p = 0; p < packages.size(); ++p) {
                std::error_code ec;
                auto rel = std::filesystem::relative(cu.source, packages[p].root, ec);
                if (ec || rel.empty()) continue;
                auto rels = rel.generic_string();
                if (rels.starts_with("..")) continue;
                auto len = packages[p].root.generic_string().size();
                if (best == packages.size() || len > bestLen) {
                    best = p; bestLen = len; bestRel = std::move(rels);
                }
            }
            if (best != packages.size()) pkgSources[best].push_back(std::move(bestRel));
        }
        for (auto& v : pkgSources) std::ranges::sort(v);

        std::vector<std::string>    pkgKeys(packages.size());
        std::vector<nlohmann::json> pkgInputs(packages.size(),
                                              nlohmann::json::object());
        std::vector<int>            keyState(packages.size(), 0); // 0 new/1 busy/2 done
        std::string                 keyCycleError;
        // Does this package's own transitive upstream contain anything that is
        // not an immutable index payload? If so it cannot be cached either, even
        // when the package itself is an index package.
        //
        // A key covers an upstream package by folding in that package's KEY, and
        // a local package's key covers its file list but not its file CONTENTS —
        // nothing could, without hashing a tree that may change between the hash
        // and the compile. So editing a local upstream's source would leave a
        // downstream entry looking valid. No index descriptor can declare a path
        // dependency today, which makes this shape unreachable in practice; it is
        // enforced structurally anyway, because "unreachable today" is how the
        // transitive path-dep leak got in.
        std::vector<char>           localTaint(packages.size(), 0);
        auto compute_key = [&](auto&& self, std::size_t idx) -> const std::string& {
            static const std::string kEmpty;
            if (keyState[idx] == 2) return pkgKeys[idx];
            if (keyState[idx] == 1) {
                if (keyCycleError.empty()) {
                    keyCycleError = std::format(
                        "dependency cycle through package '{}' while computing "
                        "its build-cache key", packages[idx].manifest.package.name);
                }
                return kEmpty;
            }
            keyState[idx] = 1;

            ck::PackageAxes pa;
            if (idx > 0 && idx - 1 < dep_cache_identities.size()) {
                pa.indexName   = dep_cache_identities[idx - 1].indexName;
                pa.packageName = dep_cache_identities[idx - 1].packageName;
                pa.version     = dep_cache_identities[idx - 1].version;
            }
            if (pa.packageName.empty()) {
                // The root package, or a package with no resolution identity.
                // It is never cached, but its key still has to exist because
                // downstream packages fold it in via axis F.
                pa.packageName = packages[idx].manifest.package.namespace_.empty()
                    ? packages[idx].manifest.package.name
                    : std::format("{}.{}", packages[idx].manifest.package.namespace_,
                                  packages[idx].manifest.package.name);
            }
            if (pa.version.empty()) pa.version = packages[idx].manifest.package.version;
            // The GLOBAL registry root — index 0 by construction above. Include
            // dirs are relativized against it so a key survives a different
            // MCPP_HOME; a project-local payload falls back to the `<pkg>`
            // prefix inside fill_package_config and is equally stable.
            ck::fill_package_config(pa, packages[idx],
                                    storeRoots.empty() ? std::filesystem::path{}
                                                       : storeRoots.front());
            pa.sources = pkgSources[idx];
            const bool selfIsIndex = idx > 0
                && idx - 1 < dep_cache_identities.size()
                && dep_cache_identities[idx - 1].sourceKind == "version";
            if (!selfIsIndex) localTaint[idx] = 1;
            for (auto& e : dependencyEdges) {
                if (e.consumerPackageIndex != idx) continue;
                auto& up = self(self, e.dependencyPackageIndex);
                if (!up.empty()) pa.upstreamKeys.push_back(up);
                if (localTaint[e.dependencyPackageIndex]) localTaint[idx] = 1;
                for (auto& f : e.requestedFeatures) pa.features.push_back(f);
            }
            std::ranges::sort(pa.upstreamKeys);
            pa.upstreamKeys.erase(std::unique(pa.upstreamKeys.begin(),
                                              pa.upstreamKeys.end()),
                                  pa.upstreamKeys.end());
            std::ranges::sort(pa.features);
            pa.features.erase(std::unique(pa.features.begin(), pa.features.end()),
                              pa.features.end());

            pkgKeys[idx]     = ck::key_hex(axes, pa);
            pkgInputs[idx]   = ck::to_json(axes, pa);
            keyState[idx]    = 2;
            return pkgKeys[idx];
        };
        for (std::size_t i = 0; i < packages.size(); ++i)
            (void)compute_key(compute_key, i);
        if (!keyCycleError.empty()) return std::unexpected(keyCycleError);

        for (std::size_t i = 1; i < packages.size(); ++i) {  // skip [0] = main
            const auto& pkgRoot   = packages[i];
            const auto* depIdent  = i - 1 < dep_cache_identities.size()
                ? &dep_cache_identities[i - 1]
                : nullptr;
            // Only index ("version") packages are cacheable, and the identity
            // recorded at resolution time is the ONLY admissible evidence.
            //
            // The predicate this replaces looked the package up in the ROOT
            // manifest's dependencies/dev-dependencies and skipped it when the
            // spec was path/git. A transitively-reached package is in neither
            // map, so `specIt == end()` left skipCache false and local sources
            // were cached — with `indexName` falling back to defaultIndex, so a
            // workspace member `B` landed on disk as `mcpplibs/B@0.1.0`. Its
            // sources can then change without changing name@version, i.e. the
            // cache key cannot see the change.
            //
            // Note the direction of the judgment: `mcpp add`'s existence gate
            // admits anything it cannot disprove. A build cache must do the
            // opposite — anything it cannot prove came from the immutable
            // xpkgs store stays out, because the failure mode here is a
            // silently wrong object rather than a rejected command.
            if (!depIdent || depIdent->sourceKind != "version") continue;
            // ...and neither may anything it was built against be local.
            if (localTaint[i]) continue;
            // ...and the package's sources must ACTUALLY be in the immutable
            // store, not merely labelled as coming from it.
            //
            // The rule stated three paragraphs up is about provenance on disk;
            // `sourceKind` is a label recorded at resolution time, which is a
            // weaker proxy — and there is already a case where the two
            // disagree. Multi-version mangling re-anchors a consumer package's
            // root at `<project>/target/.mangled/<pkg>/__self__` and REWRITES
            // its sources (module/import declarations renamed) while leaving
            // `sourceKind == "version"` and `localTaint` clear. Nothing about
            // that copy is immutable or shareable. It stays out of the cache
            // today only because axis F happens to fold in the mangled
            // secondary's differing key — one axis away from serving objects
            // compiled against renamed modules, which is the silent-wrong-`.o`
            // failure this gate exists to prevent.
            //
            // Judge the location, not the label.
            //
            // LEXICALLY, not via std::filesystem::relative. `relative()` runs
            // weakly_canonical on both sides, which RESOLVES SYMLINKS — and a
            // store whose entries are symlinks into another store is ordinary
            // (tests/e2e/_inherit_toolchain.sh builds exactly that, and so do
            // CI caches that link a warm payload tree into a fresh
            // MCPP_HOME). Canonicalizing turns
            // `<home>/registry/data/xpkgs/<pkg>` into wherever the link points
            // and the package stops looking like a store package at all. The
            // question here is where the payload was INSTALLED, which is a
            // statement about the path, not about the inode.
            if (!mcpp::build::path_is_under_any(pkgRoot.root, storeRoots))
                continue;

            const auto& depName = depIdent->packageName;
            const auto& depVer  = depIdent->version.empty()
                ? pkgRoot.manifest.package.version
                : depIdent->version;

            auto bmiT = mcpp::toolchain::bmi_traits(*tc);
            mcpp::bmi_cache::CacheKey key {
                .cacheRoot   = mcpp::home::cache_root(),
                .indexName   = depIdent->indexName,
                .packageName = depName,
                .version     = depVer,
                .keyHex      = pkgKeys[i],
                .inputs      = pkgInputs[i],
                .bmiDirName  = std::string(bmiT.bmiDir),
                .manifestTag = std::string(bmiT.manifestPrefix),
            };

            // The artifacts this package contributes, and the compile units
            // that produce them. Collected together so a hit can mark exactly
            // those units — the artifact list alone would not say which edges
            // must stop being compile edges.
            mcpp::bmi_cache::DepArtifacts arts;
            std::vector<std::size_t> unitIdx;
            bool addressable = true;
            for (std::size_t u = 0; u < ctx.plan.compileUnits.size(); ++u) {
                auto& cu = ctx.plan.compileUnits[u];
                std::error_code ec;
                auto rel = std::filesystem::relative(cu.source, pkgRoot.root, ec);
                if (ec || rel.empty()) continue;
                auto rels = rel.string();
                if (rels.starts_with("..")) continue;       // not under depRoot

                // ALL OR NOTHING. A unit plan.cppm could not give a
                // machine-independent entry address to takes its whole package
                // out of the cache, rather than leaving the package half
                // staged. Mixing cached and freshly built artifacts within one
                // package is the case GCC reports as a BMI CRC mismatch in a
                // consumer three edges away, which is far harder to read than
                // one extra compile.
                if (cu.packageObjectRel.empty()) { addressable = false; break; }

                if (cu.providesModule) {
                    std::string bmi;
                    for (char c : *cu.providesModule)
                        bmi.push_back(c == ':' ? '-' : c);
                    bmi += std::string(bmiT.bmiExt);
                    arts.bmiFiles.push_back(std::move(bmi));
                }
                arts.objFiles.push_back({cu.packageObjectRel.generic_string(),
                                         cu.object});
                unitIdx.push_back(u);
            }
            if (!addressable) continue;

            // Validate the entry against THIS build's artifact list, not
            // against the entry's own (mcpp#344). Anything short of a full
            // match is a miss — never a failure: the stage edges below are
            // simply not emitted and the units compile normally.
            auto probe = mcpp::bmi_cache::probe_cached(key, arts);
            if (probe.ok) {
                // Mark the units. The backend turns each into a stage_file
                // edge; nothing is copied here. Copying behind ninja's back is
                // exactly what made the old cache a no-op: the staged file was
                // still declared as a compile edge's output, and an output with
                // no .ninja_log command-line record is dirty, so every unit was
                // recompiled while the CLI printed "Cached".
                for (auto u : unitIdx) {
                    auto& cu = ctx.plan.compileUnits[u];
                    cu.servedFromCache = true;
                    cu.cachedObject = mcpp::bmi_cache::cached_obj_path(
                        key, cu.packageObjectRel.generic_string());
                    if (cu.providesModule) {
                        std::string bmi;
                        for (char c : *cu.providesModule)
                            bmi.push_back(c == ':' ? '-' : c);
                        bmi += std::string(bmiT.bmiExt);
                        cu.cachedBmi = mcpp::bmi_cache::cached_bmi_path(key, bmi);
                    }
                }
                mcpp::bmi_cache::touch_accessed(key);
                ctx.cachedDeps.push_back({depName, depVer, unitIdx.size()});
                continue;       // no populate task; it is already cached
            }
            // A valid entry that does not hold what we asked for means the
            // entry and this build disagree about the layout under one key.
            // After #344 that is unreachable; say so out loud if it ever
            // happens again, because the alternative presentation is "the
            // cache silently never hits", and a cache that lies about its own
            // effectiveness went unnoticed for three months once already.
            if (!probe.layoutMismatch.empty()) {
                mcpp::ui::warning(std::format(
                    "build cache entry for {}@{} [{}] does not contain the "
                    "artifacts this build needs ({} of {} missing, e.g. `{}`); "
                    "treating it as a miss. Run `mcpp cache verify` for details.",
                    depName, depVer, key.keyHex,
                    probe.layoutMismatch.size(),
                    arts.bmiFiles.size() + arts.objFiles.size(),
                    probe.layoutMismatch.front()));
            }
            ctx.depsToPopulate.push_back({ std::move(key), std::move(arts) });
        }
    }
    // ──────────────────────────────────────────────────────────────────

    // Write/update mcpp.lock for any version-based deps that succeeded.
    // Path deps are intentionally NOT locked — their source is local filesystem.
    //
    // mcpp#363: the version entries come from `resolved` — what the walk
    // actually picked — not from `m->dependencies`, which still holds the
    // constraint the user wrote and only covers DIRECT deps. Reading the input
    // instead of the output made the lock record `^1.92.8` (a range locks
    // nothing) and omit the transitive graph entirely. Git entries deliberately
    // stay on `m->dependencies`: their lock line is read back as a resolution
    // anchor (#329), keyed by the root manifest's map key, and that contract is
    // unchanged here.
    {
        mcpp::lockfile::Lockfile lock;
        lock.schemaVersion = 2;

        // The lock key for a dep the ROOT declares is the map key it declared
        // it under (`compat.imgui`, `gtest`) — that is the key #329's git anchor
        // lookup uses, and changing it would silently unpin every branch dep.
        // A dep reached only transitively has no such key, so it is written
        // under its fully-qualified identity.
        auto lock_name_for = [&](const ResolvedKey& k) -> std::string {
            for (auto const& [n, s] : m->dependencies) {
                const std::string sn = s.shortName.empty() ? n : s.shortName;
                if (s.namespace_ == k.ns && sn == k.shortName) return n;
            }
            return mcpp::pm::compat::qualified_name(k.ns, k.shortName);
        };

        // Lock custom index shas from manifest [indices] section.
        for (auto const& [idxName, spec] : m->indices) {
            if (spec.is_local() || spec.is_builtin()) continue;
            mcpp::lockfile::LockedIndex li;
            li.name = idxName;
            li.url  = spec.url;
            li.rev  = spec.rev;   // may be empty if not yet resolved
            lock.indices.push_back(std::move(li));
        }

        // Git deps: root-declared only, unchanged (see the note above).
        for (auto const& [name, spec] : m->dependencies) {
            if (!spec.isGit()) continue;
            mcpp::lockfile::LockedPackage lp;
            lp.name    = name;
            lp.version = spec.gitRev;
            auto gitIt = root_git_lock_identities.find(name);
            if (gitIt == root_git_lock_identities.end()) {
                lp.source = std::format("git+{}#{}={}",
                    spec.git, spec.gitRefKind, spec.gitRev);
                std::hash<std::string> hasher;
                lp.hash = std::format("fnv1a:{:016x}", hasher(lp.source));
            } else {
                lp.source = gitIt->second.source;
                lp.hash = gitIt->second.hash;
            }
            lock.packages.push_back(std::move(lp));
        }

        // Version deps: the whole resolved graph, at the versions actually
        // chosen. `resolved` is an ordered map, so the file is deterministic.
        for (auto const& [key, rec] : resolved) {
            if (rec.source != "version") continue;   // path / git handled elsewhere
            if (rec.version.empty()) continue;
            // See ResolvedRecord::devOnly: `mcpp test` resolves dev-deps and
            // `mcpp build` does not, so writing them would make the file depend
            // on which command ran last.
            if (rec.devOnly) continue;
            mcpp::lockfile::LockedPackage lp;
            lp.name       = lock_name_for(key);
            lp.namespace_ = key.ns;
            lp.version    = rec.version;
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
            auto hashInput = std::format("{}:{}@{}", sourceIndex, lp.name, lp.version);
            lp.hash = std::format("fnv1a:{:016x}", hasher(hashInput));
            lock.packages.push_back(std::move(lp));
        }
        if (!lock.packages.empty() || !lock.indices.empty()) {
            auto lockPath = workRoot / "mcpp.lock";
            // `--locked` ASSERTS THAT THIS RESOLUTION IS THE RECORDED ONE.
            //
            // The file has always been written after the walk and never read
            // back as a constraint; its own header says so ("does not yet pin
            // future builds"). Making it an input to resolution is a change to
            // the resolver. Making it an ASSERTION is not, and it is the half
            // that reproducibility actually needs: a release build, a CI job or
            // an audit can demand that what resolved today is what was recorded,
            // and find out when it is not.
            //
            // THE FAILURE NAMES THE DIFFERENCE. "The lock is out of date" is
            // true and useless; which package moved, from which version to
            // which, is what the reader does something about.
            if (mcpp::platform::env::get("MCPP_LOCKED").value_or("") == "1") {
                auto prior = mcpp::lockfile::load(lockPath);
                if (!prior) {
                    return std::unexpected(std::format(
                        "--locked was given and there is no readable mcpp.lock at {}\n"
                        "       Run the same command without --locked once to record "
                        "this resolution, then commit mcpp.lock.",
                        lockPath.string()));
                }
                auto key = [](const mcpp::lockfile::LockedPackage& p) {
                    return p.namespace_.empty() ? p.name
                                                : p.namespace_ + "." + p.name;
                };
                std::map<std::string, std::string> was, now;
                for (auto const& p : prior->packages) was[key(p)] = p.version;
                for (auto const& p : lock.packages)    now[key(p)] = p.version;
                std::vector<std::string> drift;
                for (auto const& [k, v] : now) {
                    auto it = was.find(k);
                    if (it == was.end())      drift.push_back(k + " " + v + " (not in the lock)");
                    else if (it->second != v) drift.push_back(k + " " + it->second + " -> " + v);
                }
                for (auto const& [k, v] : was)
                    if (!now.contains(k)) drift.push_back(k + " " + v + " (no longer resolved)");
                if (!drift.empty()) {
                    std::string msg = "--locked was given and this resolution "
                                      "differs from mcpp.lock:";
                    for (auto const& d : drift) msg += "\n         " + d;
                    msg += "\n       Re-run without --locked to update the lock, "
                           "or pin the dependency that moved.";
                    return std::unexpected(msg);
                }
            }
            (void)mcpp::lockfile::write(lock, lockPath);
        }

        // Same data, second consumer: the "Compiling <dep> v<version>" banner.
        // It reads this rather than re-deriving from the manifest, so the banner
        // and the lock cannot disagree about what was built.
        for (auto const& [key, rec] : resolved) {
            if (rec.source != "version" || rec.version.empty()) continue;
            ctx.resolvedVersions[lock_name_for(key)] = rec.version;
        }
    }

    // Apply [runtime.<capability>] provider = "<pkg>" overrides. Canonical
    // identity wins; the old short spelling is accepted only when it denotes
    // exactly one provider.  A same-short-name collision is never guessed.
    for (auto& [capKey, prov] : ctx.manifest.runtimeConfig.providerOverrides) {
        std::vector<mcpp::manifest::PackageId> candidates;
        for (auto const& entry : ctx.plan.runtimeProviders) {
            if (!entry.capability.starts_with(capKey)) continue;
            const auto withoutVersion = entry.provider.namespace_.empty()
                ? entry.provider.name
                : entry.provider.namespace_ + "." + entry.provider.name;
            if (entry.provider.canonical() == prov || withoutVersion == prov)
                candidates = {entry.provider};
        }
        if (candidates.empty()) {
            for (auto const& entry : ctx.plan.runtimeProviders) {
                if (entry.capability.starts_with(capKey)
                    && entry.provider.name == prov)
                    candidates.push_back(entry.provider);
            }
        }
        std::ranges::sort(candidates);
        candidates.erase(std::ranges::unique(candidates).begin(), candidates.end());
        if (candidates.empty()) {
            return std::unexpected(std::format(
                "[runtime.{}] provider = \"{}\" does not name a provider in "
                "the resolved dependency graph", capKey, prov));
        }
        if (candidates.size() != 1) {
            std::string choices;
            for (auto const& candidate : candidates)
                choices += (choices.empty() ? "" : ", ") + candidate.canonical();
            return std::unexpected(std::format(
                "[runtime.{}] provider = \"{}\" is ambiguous; use one exact "
                "canonical identity: [{}]", capKey, prov, choices));
        }
        const auto selected = candidates.front();
        std::stable_partition(ctx.plan.runtimeProviders.begin(),
                              ctx.plan.runtimeProviders.end(),
                              [&](const auto& pr) {
            return pr.capability.starts_with(capKey) && pr.provider == selected;
        });
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
                if (c == cap) { provider = p.canonical(); break; }
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

    // Per-build resolution manifest: the durable, provider-neutral facts that
    // `mcpp why runtime` interprets without resolving again or probing the
    // current host.  The post-link validator replaces `validation.pending`
    // with the exact artifact verdict produced at the link seam.
    {
        const std::string tcAbi =
            ctx.tc.targetTriple.find("musl") != std::string::npos ? "musl"
            : ctx.tc.stdlibId == "libc++"                          ? "libc++"
            : ctx.tc.compiler == mcpp::toolchain::CompilerId::MSVC ? "msvc"
            :                                                         "glibc";
        auto package_json = [](const mcpp::manifest::PackageId& id) {
            return nlohmann::json{
                {"canonical", id.canonical()},
                {"namespace", id.namespace_},
                {"name", id.name},
                {"version", id.version},
                {"source", id.sourceProvenance},
            };
        };
        auto path_array = [](auto const& paths) {
            nlohmann::json values = nlohmann::json::array();
            for (auto const& path : paths)
                values.push_back(path.lexically_normal().generic_string());
            return values;
        };
        nlohmann::json j;
        j["schema_version"] = 2;
        j["toolchain"] = {
            {"spec", ctx.tc.label()}, {"abi", tcAbi},
            {"triple", ctx.tc.targetTriple}, {"stdlib", ctx.tc.stdlibId},
        };
        nlohmann::json dirs = nlohmann::json::array();
        for (auto& d : ctx.plan.runtimeLibraryDirs) dirs.push_back(d.string());
        nlohmann::json legacyCaps = nlohmann::json::array();
        nlohmann::json providers = nlohmann::json::array();
        for (auto& [cap, prov] : ctx.plan.runtimeProviders)
        {
            legacyCaps.push_back({{"capability", cap},
                                  {"provider", prov.canonical()}});
            providers.push_back({{"capability", cap},
                                 {"provider", package_json(prov)}});
        }
        nlohmann::json requirements = nlohmann::json::array();
        for (auto const& requirement : ctx.plan.runtimeRequirements) {
            requirements.push_back({
                {"kind", requirement.kind},
                {"value", requirement.value},
                {"phase", requirement.phase},
                {"requester", package_json(requirement.requester)},
                {"required", requirement.required},
            });
        }
        nlohmann::json artifacts = nlohmann::json::array();
        for (auto const& artifact : ctx.plan.runtimeArtifacts) {
            artifacts.push_back({
                {"role", artifact.role},
                {"provider", package_json(artifact.provider)},
                {"path", artifact.path.lexically_normal().generic_string()},
                {"provenance", artifact.provenance},
                {"abi", artifact.abi},
                {"digest", artifact.digest},
                {"host_fingerprint", artifact.hostFingerprint},
                // A requirement must land on a THING, and the thing must be
                // the one that was declared. mcpp already enforces this for
                // the private libc; recording it per artifact makes a stale
                // binding visible instead of leaving `providers:` naming
                // something nobody checked.
                {"identity", std::string(
                    mcpp::build::runtime_validation::to_string(
                        mcpp::build::runtime_validation
                            ::artifact_identity_verdict(artifact)))},
            });
        }
        nlohmann::json binding = nlohmann::json::parse(
            mcpp::platform::runtime::serialize_runtime_binding(
                ctx.plan.runtimeBinding), nullptr, false);
        if (binding.is_discarded()) binding = nlohmann::json::object();

        auto triple = ctx.tc.targetTriple;
        std::ranges::transform(triple, triple.begin(),
            [](unsigned char c) { return std::tolower(c); });
        const bool pe = triple.find("windows") != std::string::npos
                     || triple.find("mingw") != std::string::npos;
        const bool macho = triple.find("darwin") != std::string::npos
                        || triple.find("apple") != std::string::npos;
        std::string format = pe ? "pe" : macho ? "macho" : "elf";
        // The ORDERED run-time search closure with provenance. Order is
        // semantics here, not presentation: it is what the loader will walk,
        // and the mutable SubOS farm sitting last is the invariant that keeps
        // libc resolving from the pinned payload. Recorded so "why does my GL
        // program find its driver" is answerable without readelf, and so a
        // regression in the ordering is visible to CI and to `mcpp why`.
        nlohmann::json closure = nlohmann::json::array();
        for (auto const& dir : ctx.plan.runtimeSearch) {
            closure.push_back({
                {"path", dir.path.generic_string()},
                {"origin", std::string(
                    mcpp::platform::search::to_string(dir.origin))},
                {"machine_local",
                    mcpp::platform::search::is_machine_local(dir.origin)},
            });
        }
        nlohmann::json search = {
            {"format", format},
            {"link_library", pe ? "libpath" : "library_path"},
            {"transitive_needed", format == "elf" ? "rpath_link" : "none"},
            {"runtime", format == "pe" ? "deploy"
                         : format == "macho" ? "loader_rpath" : "runpath"},
            {"closure", closure},
        };
        // #418 — the contract each ROLE actually got, after any downgrade.
        //
        // `CompileFlags::contractByRole` was written and never read: a valuable
        // observation with no way out of the process. Since #414 the shared
        // library role can legitimately end up on a different contract from the
        // binaries beside it, so "which one did my .so actually get?" is a
        // question a user has, and the only answer available was to run
        // `readelf` and infer.
        //
        // Recorded as the RESOLVED value, not the requested one — a request
        // that was downgraded is exactly the case worth being able to see.
        // `compute_flags` is pure in the plan; prepare does not otherwise hold
        // the result, and threading it through just for this would widen a
        // signature for one field.
        const auto roleFlags = mcpp::build::compute_flags(ctx.plan);
        nlohmann::json contracts = nlohmann::json::object();
        for (std::size_t i = 0; i < mcpp::build::dist::kRoleCount; ++i) {
            contracts[std::string(mcpp::build::dist::to_string(
                          static_cast<mcpp::build::dist::Role>(i)))] =
                std::string(mcpp::build::dist::to_string(roleFlags.contractByRole[i]));
        }

        j["runtime"] = {
            {"cxx_runtime_by_role", contracts},
            {"library_dirs", dirs},
            {"dlopen_libs", ctx.plan.runtimeDlopenLibs},
            {"capabilities", legacyCaps},
            {"binding", binding},
            {"requirements", requirements},
            {"artifacts", artifacts},
            {"providers", providers},
            {"link_intent", {
                {"libraries", ctx.plan.linkIntent.libraries},
                {"link_library_dirs",
                    path_array(ctx.plan.linkIntent.linkLibraryDirs)},
                {"transitive_needed_dirs",
                    path_array(ctx.plan.linkIntent.transitiveNeededDirs)},
                {"runtime_search_dirs",
                    path_array(ctx.plan.linkIntent.runtimeSearchDirs)},
                {"frameworks", ctx.plan.linkIntent.frameworks},
                {"deploy_files", path_array(ctx.plan.linkIntent.deployFiles)},
            }},
            {"search", search},
            {"validation", {
                {"status", format == "elf" ? "pending" : "not_exercised"},
                {"source", "post_link"},
                {"artifacts", nlohmann::json::array()},
            }},
        };
        std::error_code ec;
        std::filesystem::create_directories(ctx.plan.outputDir, ec);
        auto path = ctx.plan.outputDir / "resolution.json";
        auto tmp = path;
        tmp += ".tmp";
        if (std::ofstream js(tmp); js) {
            js << j.dump(2) << "\n";
            js.close();
            std::filesystem::rename(tmp, path, ec);
            if (ec) {
                ec.clear();
                std::filesystem::remove(path, ec);
                ec.clear();
                std::filesystem::rename(tmp, path, ec);
            }
        }
    }

    // ── A link unit with no inputs is not a build (mcpp#533) ────────────────
    //
    // Checked HERE, last, because objects arrive from three places and each
    // one is legitimate: the compile set, a `role = "object"` action
    // (`lu.objects.emplace_back` above), and a Windows resource unit. A check
    // placed before any of them would refuse a unit that was about to be
    // filled. If a fourth source is ever added, it must land before this line.
    //
    // WHY THIS IS AN ERROR AND NOT A WARNING. The two library kinds fail
    // differently and BOTH failures are worse than this message:
    //
    //   shared — `$cc -shared` over an empty response file. Measured on
    //            gcc 16.1.0: `gcc: fatal error: no input files`, which names
    //            the driver and not the target. Before `cc` was emitted
    //            unconditionally it was `/bin/sh: 1: -shared: not found`,
    //            which names neither.
    //   static — `ar rcs libfoo.a` with no members. Measured: exit 0, an
    //            8-byte archive, and a build that REPORTS SUCCESS. Every
    //            consumer then fails with undefined symbols, one repository
    //            further from the cause.
    //
    // The silent one is why this is not merely a nicer diagnostic. mcpp#533
    // reached here because a dependency's `install()` was skipped over a
    // package-identity collision, leaving a version directory with no source
    // tree; the shape is the same for any package whose sources fail to
    // materialise, which is why the check is on the link unit rather than on
    // the install path.
    for (auto const& lu : ctx.plan.linkUnits) {
        if (!lu.objects.empty()) continue;
        const char* kindName =
              lu.kind == mcpp::build::LinkUnit::SharedLibrary ? "shared library"
            : lu.kind == mcpp::build::LinkUnit::StaticLibrary ? "static library"
            : lu.kind == mcpp::build::LinkUnit::TestBinary    ? "test binary"
                                                              : "binary";
        return std::unexpected(std::format(
            "target '{}' ({}) has no inputs to link\n"
            "       no translation unit and no `role = \"object\"` action "
            "output reached it, and an empty link is not a build: `ar` writes "
            "an empty archive and reports success, so this would otherwise "
            "surface as undefined symbols in whatever consumes '{}'\n"
            "       if '{}' is an installed dependency, its package directory "
            "has no sources — reinstall it and check that its descriptor's "
            "install step ran",
            lu.targetName, kindName, lu.output.generic_string(),
            lu.targetName));
    }

    return ctx;
}


} // namespace mcpp::build
