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
// The five target-side layers (docs/14) join the triple coordinates here, but
// they arrive LATER and from a different place: the triple is known before
// dependency resolution, a layer only after it, because a package in the graph
// may supply the C library. `layersKnown` is the difference, and it is a member
// rather than an inference from emptiness because "no layer resolved" and "not
// resolved yet" are different answers and only one of them may be reported.
//
// ⚠️ `compiler` carries the FAMILY (`llvm`), never the driver (`clang`) — #494
// settled that, on the grounds that every place a user writes the name they
// write the family, and reporting the driver would make
// `requires = ["mcpp:compiler=llvm"]` permanently unsatisfiable.
struct Ctx {
    std::string os, arch, family, env, triple;
    bool        layersKnown = false;
    std::string compiler, compilerRuntime, kernelAbi, cAbi, cxxAbi;
    // The first MULTI-VALUED layer. One build can enable several accelerator
    // backends at once, which is what an inference framework shipping CUDA and
    // ROCm device code in one artifact requires, so this layer holds a set
    // rather than the single answer the other five hold.
    std::vector<std::string> accelerators;

    std::string_view layer_value(std::string_view k) const {
        if (k == "compiler")         return compiler;
        if (k == "compiler-runtime") return compilerRuntime;
        if (k == "kernel-abi")       return kernelAbi;
        if (k == "c-abi")            return cAbi;
        if (k == "c++-abi")          return cxxAbi;
        return {};
    }

    // THE SINGLE PLACE THE MULTI-VALUED CASE DIFFERS.
    //
    // A multi-valued layer compares by MEMBERSHIP, and it does so everywhere —
    // not only inside `any(...)`. The alternative, letting `any(...)` mean
    // membership while a bare key meant set equality, would make a combinator
    // change the meaning of its operand: `all(accelerator = "cuda",
    // accelerator = "rocm")` would then be unsatisfiable rather than "both
    // backends are enabled". Membership everywhere keeps `any`/`all`/`not`
    // pure boolean combinators, and a single-backend build still answers
    // `accelerator = "cuda"` true and `accelerator = "rocm"` false.
    bool layer_matches(std::string_view k, std::string_view v) const {
        if (k == "accelerator")
            return std::ranges::find(accelerators, v) != accelerators.end();
        return layer_value(k) == v;
    }
};

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

// ── The cfg() vocabulary ────────────────────────────────────────────────────
//
// ⚠️ ONE LIST PER CATEGORY, AND EVERY READER READS IT. #540 found four
// hand-written copies of other vocabularies in this repository, all drifted;
// the diagnostic added below would have been the fifth if it had transcribed
// these names instead of sharing them.
//
// TRIPLE keys are answerable from the target triple alone, which is what the
// conditional merge has before dependency resolution. LAYER keys name a
// target-side layer (docs/14) and are answerable only after the graph is
// resolved — see `merge_layer_conditional_config` in prepare.cppm for the
// second pass that evaluates them.
inline constexpr std::string_view kCfgTripleKeys[] = {
    "arch", "env", "family", "os",
};
inline constexpr std::string_view kCfgLayerKeys[] = {
    "accelerator", "c++-abi", "c-abi", "compiler", "compiler-runtime",
    "kernel-abi",
};
inline constexpr std::string_view kCfgBarewords[] = {
    "linux", "macos", "unix", "windows",
};

inline bool is_cfg_layer_key(std::string_view k) {
    return std::ranges::find(kCfgLayerKeys, k) != std::end(kCfgLayerKeys);
}

// Recursive-descent evaluator over the inside of `cfg(...)`:
//   expr := all(list) | any(list) | not(expr) | key="value" | bareword
//   key  ∈ kCfgTripleKeys ∪ kCfgLayerKeys   bareword ∈ kCfgBarewords
//
// ⚠️ THE EVALUATOR IS ALSO THE VALIDATOR. `seenKeys`/`seenWords` let one
// traversal answer three questions — does it match, does it name a layer, does
// it name anything at all — because a separate validator would be a SECOND
// parser of the same grammar, and this repository has already paid for one of
// those (`[hooks]` re-parsing mcpp.toml and reporting every TOML error as an
// invalid hook configuration).
struct Parser {
    std::string_view s; std::size_t i = 0; const Ctx& c;
    std::vector<std::string>* seenKeys  = nullptr;  // every `key=` key, in order
    std::vector<std::string>* seenWords = nullptr;  // every bareword
    void ws() { while (i < s.size() && std::isspace((unsigned char)s[i])) ++i; }
    bool eat(char ch) { ws(); if (i < s.size() && s[i] == ch) { ++i; return true; } return false; }
    std::string ident() {
        ws(); std::size_t b = i;
        // ⚠️ `-` and `+` ARE IDENTIFIER CHARACTERS, because the layer names are
        // `c-abi`, `c++-abi`, `compiler-runtime` and `kernel-abi`. Without them
        // `cfg(c-abi = "musl")` scanned as the bareword `c` followed by
        // garbage, so the one thing a diagnostic could report was the letter
        // `c`. No valid pre-existing predicate contains either character
        // outside a quoted value, so widening the scanner changes nothing that
        // used to parse.
        while (i < s.size() && (std::isalnum((unsigned char)s[i])
                                || s[i] == '_' || s[i] == '-' || s[i] == '+')) ++i;
        return std::string(s.substr(b, i - b));
    }
    std::string str() {
        ws(); if (i >= s.size() || s[i] != '"') return {};
        ++i; std::size_t b = i; while (i < s.size() && s[i] != '"') ++i;
        auto v = std::string(s.substr(b, i - b)); if (i < s.size()) ++i; return v;
    }
    bool match_alias(const std::string& a) {
        if (seenWords) seenWords->push_back(a);
        if (a == "windows") return c.os == "windows";
        if (a == "linux")   return c.os == "linux";
        if (a == "macos")   return c.os == "macos";
        if (a == "unix")    return c.family == "unix";
        return false;  // unknown bareword → no match, and `seenWords` reports it
    }
    bool match_kv(const std::string& k, const std::string& v) {
        if (seenKeys) seenKeys->push_back(k);
        if (k == "os")     return c.os == v;
        if (k == "arch")   return c.arch == v;
        if (k == "family") return c.family == v;
        if (k == "env")    return c.env == v;
        // A layer key is not answerable until the target side is resolved. In
        // the first (triple-only) pass this returns false and the section is
        // skipped — which is correct, because the second pass owns it and would
        // otherwise append the same inputs twice through `append()`.
        if (is_cfg_layer_key(k))
            return c.layersKnown && c.layer_matches(k, v);
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

// ── One traversal, three answers ────────────────────────────────────────────
//
// `scan_predicate` runs the REAL evaluator with a throwaway context purely to
// record which tokens the predicate names. Everything below derives from it, so
// the grammar has exactly one implementation and a predicate that the evaluator
// cannot answer is, by construction, a predicate the diagnostic reports.
struct PredicateScan {
    std::vector<std::string> keys;       // every `key=` key, in order
    std::vector<std::string> barewords;  // every bareword
};

inline PredicateScan scan_predicate(const std::string& predicate) {
    PredicateScan out;
    std::string_view k = predicate;
    // Only the `cfg(...)` namespace. A bare alias or a bare triple is the
    // documented escape hatch — `matches()` falls back to an exact string
    // comparison for keys it cannot parse — and validating it would reject the
    // explicit-section spelling that hatch exists to allow.
    if (k.starts_with("cfg(") && k.ends_with(")")) {
        Ctx scratch;
        Parser p{ k.substr(4, k.size() - 5), 0, scratch, &out.keys, &out.barewords };
        (void)p.expr();
    }
    return out;
}

// True when the predicate names a target-side layer and therefore cannot be
// answered before dependency resolution. This is the classifier that keeps the
// two merge passes disjoint: `append()` is additive, so a section evaluated by
// both would contribute its inputs twice.
inline bool uses_layer(const std::string& predicate) {
    auto scan = scan_predicate(predicate);
    return std::ranges::any_of(scan.keys,
                               [](auto const& k) { return is_cfg_layer_key(k); });
}

// Tokens outside the vocabulary. A predicate naming one of these used to
// evaluate to false in silence, which is indistinguishable from a predicate
// that correctly did not apply — so `[target.'cfg(c-abi = "musl")'.build]` was
// dropped without a word for the entire time docs/14 documented it.
inline std::vector<std::string> unknown_tokens(const std::string& predicate) {
    auto scan = scan_predicate(predicate);
    std::vector<std::string> out;
    auto add = [&](const std::string& t) {
        if (t.empty()) return;
        if (std::ranges::find(out, t) == out.end()) out.push_back(t);
    };
    for (auto const& k : scan.keys)
        if (std::ranges::find(kCfgTripleKeys, k) == std::end(kCfgTripleKeys)
            && !is_cfg_layer_key(k))
            add(k);
    for (auto const& w : scan.barewords)
        if (std::ranges::find(kCfgBarewords, w) == std::end(kCfgBarewords))
            add(w);
    return out;
}

// The message body, built FROM the vocabulary rather than beside it.
inline std::string vocabulary_sentence() {
    std::string keys, words;
    for (auto k : kCfgTripleKeys) { if (!keys.empty()) keys += ", "; keys += k; }
    for (auto k : kCfgLayerKeys)  { if (!keys.empty()) keys += ", "; keys += k; }
    for (auto w : kCfgBarewords)  { if (!words.empty()) words += ", "; words += w; }
    return std::format("Supported keys: {}. Supported barewords: {}.", keys, words);
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
    // #519 — the same reasoning as the profile knobs above, one axis later.
    // The REQUEST is folded in rather than the derived `-fPIC`, because this
    // string is built before the plan exists; the request is what a user
    // edits and the flag is a function of it. Without this, flipping
    // `dependency_linkage` reuses the previous configuration's output
    // directory — measured on a two-package fixture, where both builds landed
    // in `target/x86_64-linux-gnu/5d4a4a8a584ba471/` and the shared build's
    // `libcore.so` was left sitting in the static build's `bin/`.
    //
    // Only appended when non-empty, so every existing build directory keeps
    // its identity and this release rebuilds nothing.
    if (!m.buildConfig.dependencyLinkage.empty()) {
        s += " deplinkage=";
        s += m.buildConfig.dependencyLinkage;
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
        s += " source=";
        s += pkg.manifest.package.sourceProvenance;
        // ⭐⭐ WHAT THIS PACKAGE IS BUILT WITH, AND NOT ONLY WHAT IT ASKS THE
        // RUNTIME FOR.
        //
        // Only the root's compile inputs used to reach the fingerprint, through
        // `canonical_compile_flags` on the root manifest. A DEPENDENCY's
        // `[build] cflags` / `defines` / `sources` / per-glob flags reached
        // nothing — so editing one left the fingerprint unchanged, the consumer
        // kept the same output directory, and the fast path replayed a
        // build.ninja generated before the edit.
        //
        // ⚠️ AND THE WAY THAT SHOWS IS THAT THE EDIT APPEARS TO HAVE HAD NO
        // EFFECT. Measured 2026-08-23 on a path dependency: a flag added to
        // `[build] cflags` was absent from the generated `unit_cflags` after a
        // rebuild, absent after touching the sources, and present the moment
        // `target/` was removed. The first two observations are what a reader
        // uses to conclude the flag is being filtered, and one was concluded
        // and written down before the third measurement was taken.
        //
        // The comment beside the root-flag tail merge in prepare.cppm has said
        // "canonical_package_build_metadata folds packages[].manifest.
        // buildConfig" since before this fix. It now does.
        //
        // packages[0] is the root, whose flags `canonical_compile_flags`
        // already folds; serialising it twice is harmless and keeps this loop
        // one rule rather than one rule and an exception.
        s += ' ';
        s += canonical_compile_flags(pkg.manifest);
        for (auto const& src : pkg.manifest.buildConfig.sources) {
            s += " src:";
            s += src;
        }
        for (auto const& dir : pkg.manifest.buildConfig.includeDirs) {
            s += " inc:";
            s += dir.generic_string();
        }
        for (auto const& dir : pkg.manifest.buildConfig.includeDirsAfter) {
            s += " inca:";
            s += dir.generic_string();
        }
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
