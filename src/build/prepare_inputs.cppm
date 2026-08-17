// mcpp.build.prepare_inputs — the inputs a build plan is derived FROM, split out
// of mcpp.build.prepare.
//
// WHY. `prepare.cppm` was 6521 lines and 16.4s to compile — 22% of this
// repository's critical build path and its only real outlier. A module that
// large is worth splitting on architecture grounds alone; the build-time effect
// is a bonus and, since the split schedule landed, a smaller one (an interface
// now blocks importers for ~22% of its compile rather than all of it).
//
// ⚠️ WHAT A SPLIT HAS TO BE TO HELP THE CRITICAL PATH. Extracting a piece that
// `prepare` then imports makes the chain LONGER, not shorter: `... -> this ->
// prepare -> ...` is still serial, and prepare only sheds the cost this module
// now pays. It shortens the path only for consumers that can import THIS
// instead of prepare — which is why the pieces chosen here are the ones with no
// dependency on the rest of prepare: cfg() predicate evaluation and the
// fingerprint canonicalisers.
//
// They are re-exported from `mcpp.build.prepare`, so no caller had to change.
export module mcpp.build.prepare_inputs;

import std;
import mcpp.diag;
import mcpp.manifest;
import mcpp.modgraph.graph;
import mcpp.modgraph.scanner;
import mcpp.platform;
import mcpp.toolchain.model;
import mcpp.toolchain.fingerprint;
import mcpp.toolchain.triple;
import mcpp.ui;

export namespace mcpp::build {

// ── L1 platform-conditional config: cfg() predicate evaluation ──────────────
// Context = the RESOLVED target's coordinates. A `[target.'cfg(...)'.build]`
// predicate is evaluated against this (target triple for a cross build, host
// for a native build), so conditional flags follow what the binary will run on
// — not the build host. See the manifest design doc.
namespace cfgpred {

// `triple` is the RESOLVED target — the host's for a native build, the
// --target one for a cross build. It is a member rather than a parameter of
// `matches()` because a bare-triple predicate and a `cfg(...)` predicate are
// two spellings of ONE question, and they must be answered from one value.
//
// They were not. `matches()` used to take the raw `--target` string alongside
// this context and short-circuit on `if (triple.empty()) return false;`, while
// `context_for()` below fell back to the host. So `cfg(linux)` matched a native
// build and `[target.'x86_64-linux-gnu'.build]` — the same statement about the
// same machine — did not, silently. That shape is the worst kind: CI passes
// `--target` and is green, the developer's plain `mcpp build` drops the flags,
// and the failure surfaces at link time naming a symbol instead of a predicate.
// manifest/types.cppm's ConditionalConfig has documented the fallback since it
// was written; this makes the bare-triple branch honour it.
struct Ctx { std::string os, arch, family, env, triple; };

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
        // Canonical spelling on BOTH sides of the later comparison, so an
        // `x86_64-w64-mingw32` key and an `x86_64-windows-gnu` build agree.
        c.triple = t->str();
    } else {
        // Escape-hatch triple outside the language: only the leading arch
        // segment is derivable; other dimensions stay empty (never match).
        auto dash = targetTriple.find('-');
        c.arch = std::string(dash == std::string_view::npos ? targetTriple
                                                            : targetTriple.substr(0, dash));
        // Unparseable: keep it verbatim so the exact-string fallback in
        // `matches()` can still hit an explicit escape-hatch section.
        c.triple = std::string(targetTriple);
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
//
// The resolved triple comes from `c`, never from a second parameter: see the
// note on Ctx for what having two of them cost.
inline bool matches(const std::string& predicate, const Ctx& c) {
    const std::string_view triple = c.triple;
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
    //
    // `c.triple` is populated for every build, native included, so this is a
    // guard and no longer a behaviour: it used to be the line that made a
    // bare-triple section silently inert without `--target`.
    if (triple.empty()) return false;
    if (auto p = mcpp::toolchain::triple::parse(predicate)) {
        if (auto rt = mcpp::toolchain::triple::parse(triple))
            return p->str() == rt->str();
    }
    return predicate == triple;
}

}  // namespace cfgpred

std::filesystem::path target_dir(const mcpp::toolchain::Toolchain& tc,
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
// Exported so the "every build-variant knob is in here" invariant is machine-
// checkable: the profile knobs were absent for a long time precisely because
// nothing could assert on this string.
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
    // [build] module_extensions changes WHICH FILES ARE MODULE INTERFACES,
    // i.e. the shape of the graph: which units emit a BMI, which objects link
    // unconditionally, which ninja rule each unit gets. That is a build
    // variant, so it belongs in the fingerprint — mcpp.toml's mtime alone only
    // protects the fast path within one output dir, not the BMI cache.
    //
    // Contrast [build] build_program_timeout, which is deliberately absent:
    // it changes no edge. See BuildConfig::buildProgramTimeoutSecs.
    for (auto const& e : m.buildConfig.moduleExtensions) {
        s += " modext:";
        s += e;
    }
    // The resolved [profile] knobs. These are NOT in cflags/cxxflags: the
    // profile block (see the profile resolution below) lands them in
    // buildConfig.optLevel/debug/lto/strip and flags.cppm turns them into
    // -O<n>/-g/-flto at command-construction time. Leaving them out made
    // `--dev`, `--release` and `--profile dist` share ONE fingerprint, hence
    // one target/<triple>/<fp>/ directory AND one global cache entry — so a
    // release build could be served -O0 -g dependency objects. They are
    // build-variant by definition; they belong here.
    s += " opt=";   s += m.buildConfig.optLevel;
    s += " debug="; s += m.buildConfig.debug ? "1" : "0";
    s += " lto=";   s += m.buildConfig.lto   ? "1" : "0";
    s += " strip="; s += m.buildConfig.strip ? "1" : "0";
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
        s += " source=";
        s += pkg.manifest.package.sourceProvenance;
        auto const& runtime = pkg.manifest.runtimeConfig;
        for (auto const& requirement : runtime.requirements) {
            s += " runtime-need:";
            s += requirement.kind;
            s += ':';
            s += requirement.value;
            s += ':';
            s += requirement.phase;
            s += requirement.required ? ":required" : ":optional";
        }
        for (auto const& artifact : runtime.artifacts) {
            s += " runtime-artifact:";
            s += artifact.role;
            s += ':';
            s += artifact.path.generic_string();
            s += ':';
            s += artifact.provenance;
            s += ':';
            s += artifact.abi;
            s += ':';
            s += artifact.digest;
            s += ':';
            s += artifact.hostFingerprint;
        }
        for (auto const& value : runtime.linkIntent.libraries)
            s += " link-library:" + value;
        for (auto const& value : runtime.linkIntent.linkLibraryDirs)
            s += " link-dir:" + value.generic_string();
        for (auto const& value : runtime.linkIntent.transitiveNeededDirs)
            s += " needed-dir:" + value.generic_string();
        for (auto const& value : runtime.linkIntent.runtimeSearchDirs)
            s += " runtime-dir:" + value.generic_string();
        for (auto const& value : runtime.linkIntent.frameworks)
            s += " framework:" + value;
        for (auto const& value : runtime.linkIntent.deployFiles)
            s += " deploy:" + value.generic_string();
        // Legacy fields remain fingerprinted while they are readable.
        for (auto const& value : runtime.libraryDirs)
            s += " legacy-runtime-dir:" + value.generic_string();
        for (auto const& value : runtime.dlopenLibs)
            s += " legacy-soname:" + value;
        for (auto const& value : runtime.capabilities)
            s += " legacy-capability:" + value;
        for (auto const& value : runtime.provides)
            s += " legacy-provides:" + value;
        for (auto const& [capability, provider] : runtime.providerOverrides)
            s += " provider-override:" + capability + '=' + provider;
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
        // Same reason as the root block, and it cannot be skipped on the
        // grounds that "a descriptor is frozen per version": path and git
        // dependencies are not frozen, and this key changes their products.
        for (auto const& e : pkg.manifest.buildConfig.moduleExtensions) {
            s += " modext:";
            s += e;
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

}  // namespace mcpp::build
