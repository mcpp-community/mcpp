// mcpp.pack.manifest_emit — the `mcpp.toml` that travels inside a library
// package.
//
// IT IS AN ORDINARY MANIFEST, AND THAT IS THE DESIGN
//
// Two earlier drafts of this format invented somewhere to put the packaging
// facts: first a sibling `MCPP-PACKAGE.toml`, then a `[distribution]` section.
// Both were deleted, because every fact already had a home:
//
//   what it is        where it goes                       already parsed by
//   ----------------- ----------------------------------- -----------------
//   the interface     [build] sources / include_dirs      yes
//   how to link it    [target.'cfg(...)'.build] ldflags   yes
//   its dependencies  [dependencies]                      yes
//   which modules     [modules] exports                   yes
//   the C++ runtime   [build] cxx_runtime                 yes
//   the artifacts     [[runtime.artifacts]]               yes
//
// So a consumer needs no new code path to USE one of these packages: mcpp's
// existing "the payload carries its own mcpp.toml" route (Form A) reads it,
// whether it arrives as a path dependency, a git dependency, a file, or an
// index tarball. And an mcpp too old to run the gate still BUILDS against the
// package, because none of the keys are new — it just does not check them.
// A new section would have been silently skipped instead, leaving no record
// at all; `[[runtime.artifacts]]` is a section old clients already write into
// `resolution.json`.
//
// WHAT IS EVIDENCE, AND WHY IT LOOKS LIKE THE REST
//
// `[[runtime.artifacts]]` carries `role`, `abi`, `digest`, `provenance` and
// `host_fingerprint` — the exact fields the gate needs, and documented as
// "optional evidence" since they were introduced. `provenance` starting with
// `mcpp-pack` is what MARKS a directory as a distribution package; nothing
// else needs to say so.
//
// Design: .agents/docs/2026-08-17-library-distribution-design.md §2.4.

export module mcpp.pack.manifest_emit;

import std;

export namespace mcpp::pack {

// One target triple's worth of the package: the artifact built for it and the
// conditional block that selects it.
struct PackageLeg {
    std::string triple;        // canonical, e.g. "x86_64-linux-gnu"
    std::string libFile;       // "libmathkit.a" — as it sits in lib/<triple>/
    // The file a CONSUMER links. Same as libFile except for a PE shared library,
    // where the loader opens the `.dll` and the linker consumes the import
    // library beside it.
    std::string linkFile;
    std::string linkName;      // "mathkit" — the -l argument
    std::string abiTag;        // "x86_64-linux-gnu-gcc16-libstdcxx16-c++23"
    std::string digest;        // "sha256:…"
    std::string buildKey;      // cache_key::key_hex, or empty
    bool        shared = false;
};

struct PackageDoc {
    std::string namespace_;
    std::string name;
    std::string version;
    std::string builtBy;                       // "mcpp 2026.8.17.1"

    // Package-relative, e.g. "interface/mathkit.cppm". Empty is legal and
    // meaningful: a header-only package compiles nothing, and the emitted
    // `sources = []` says exactly that (an omitted key would be filled with
    // the default glob and would sweep up whatever sits under src/).
    std::vector<std::string> interfaceFiles;
    // Module-interface extensions the PUBLISHED SET uses beyond the built-in
    // `.cppm` — emitted as `[build] module_extensions`.
    //
    // A function of what is in the package, not a copy of what the producer
    // declared: the packer publishes a computed subset, so the extensions it
    // needs are computed from that subset too. Without this the consumer is
    // handed `sources = ["interface/mathkit.ixx"]` and no way to know what an
    // `.ixx` is, which is the producer remembering to configure something the
    // package could state for itself.
    std::vector<std::string> moduleExtensions;
    bool        hasIncludeDir = false;
    std::string interfaceDigest;               // over the ordered interface set

    std::string cxxRuntime;                    // empty = do not write the key
    std::vector<std::string> exportsModules;

    std::string targetName;
    bool        targetShared = false;

    std::vector<PackageLeg> legs;
    // Raw `[dependencies]` keys as the producer wrote them, paired with the
    // version. Path/git deps are the caller's to drop — they are local-only
    // and cannot be resolved by anyone downstream.
    std::vector<std::pair<std::string, std::string>> dependencies;
};

// The cfg() predicate that selects exactly `triple`.
//
// NOT a bare `[target.'<triple>'.build]` key. A bare triple is only matched
// when the user passes `--target`; a plain `mcpp build` resolves the host and
// used to compare it against an empty string, so the section was silently
// inert. That is fixed (mcpp.build.prepare_inputs), but generating cfg() is
// still the right output: it is what the same statement means on every mcpp,
// including the ones already installed.
std::string cfg_predicate_for(std::string_view triple);

std::string emit_package_manifest(const PackageDoc& doc);

} // namespace mcpp::pack

namespace mcpp::pack {

namespace {

// TOML basic-string escaping, restricted to what a manifest can contain.
std::string quote(std::string_view s) {
    std::string out = "\"";
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;      break;
        }
    }
    out += '"';
    return out;
}

std::string join_quoted(const std::vector<std::string>& v) {
    std::string out;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i) out += ", ";
        out += quote(v[i]);
    }
    return out;
}

} // namespace

std::string cfg_predicate_for(std::string_view triple) {
    // arch-os[-env]; the canonical spelling from mcpp.toolchain.triple.
    std::vector<std::string_view> seg;
    for (std::size_t i = 0; i <= triple.size(); ) {
        auto j = triple.find('-', i);
        if (j == std::string_view::npos) { seg.push_back(triple.substr(i)); break; }
        seg.push_back(triple.substr(i, j - i));
        i = j + 1;
    }
    if (seg.size() < 2) return std::format("cfg(arch = \"{}\")", triple);

    std::string p = std::format("cfg(all(arch = \"{}\", os = \"{}\"", seg[0], seg[1]);
    // env is named only when the triple names it. Writing `env = ""` would be
    // a constraint the triple did not make, and the evaluator has no spelling
    // for "unset" anyway.
    if (seg.size() >= 3 && !seg[2].empty())
        p += std::format(", env = \"{}\"", seg[2]);
    p += "))";
    return p;
}

std::string emit_package_manifest(const PackageDoc& doc) {
    std::string o;
    o += std::format(
        "# Generated by `{}`. Do not edit.\n"
        "#\n"
        "# Editing anything under interface/ invalidates the digest recorded in\n"
        "# the `role = \"interface\"` artifact below, and mcpp refuses to build\n"
        "# against a package whose interface no longer matches its binaries.\n\n",
        doc.builtBy.empty() ? "mcpp pack" : doc.builtBy);

    o += "[package]\n";
    if (!doc.namespace_.empty()) o += std::format("namespace = {}\n", quote(doc.namespace_));
    o += std::format("name      = {}\n", quote(doc.name));
    o += std::format("version   = {}\n\n", quote(doc.version));

    // ── the interface ──────────────────────────────────────────────────
    o += "[build]\n";
    o += std::format("sources      = [{}]\n", join_quoted(doc.interfaceFiles));
    // Emitted only when the published set actually uses one, so a `.cppm`
    // package's manifest is byte-identical to before. A package whose interface
    // is `.ixx` states that itself rather than requiring the consumer to have
    // guessed the producer's convention.
    if (!doc.moduleExtensions.empty())
        o += std::format("module_extensions = [{}]\n", join_quoted(doc.moduleExtensions));
    if (doc.hasIncludeDir) o += "include_dirs = [\"include\"]\n";
    if (!doc.cxxRuntime.empty())
        o += std::format("cxx_runtime  = {}\n", quote(doc.cxxRuntime));
    o += "\n";

    if (!doc.exportsModules.empty()) {
        o += "[modules]\n";
        o += std::format("exports = [{}]\n\n", join_quoted(doc.exportsModules));
    }

    o += std::format("[targets.{}]\n", doc.targetName);
    o += std::format("kind = \"{}\"\n\n", doc.targetShared ? "shared" : "lib");

    // ── how to link each leg ───────────────────────────────────────────
    for (auto const& leg : doc.legs) {
        o += std::format("[target.'{}'.build]\n", cfg_predicate_for(leg.triple));
        // `-L<dir>` + `-l<name>`, on every target including PE.
        //
        // NAMING THE FILE BY PATH INSTEAD DOES NOT WORK, and it is worth
        // writing down because it looks strictly better. A bare path is the one
        // spelling every driver accepts (cl, clang, gcc, link.exe) and it names
        // the exact file rather than asking the linker to search. Measured: it
        // fails with `ld: cannot find lib/x86_64-windows-gnu/libmathkit.a`,
        // because ninja runs link commands with cwd = the OUTPUT dir and only
        // the include-family prefixes (`-I`, `-L`, …) get absolutized against
        // the package root by normalize_include_flags. A prefix-less token has
        // nothing to hook that on, and the manifest cannot carry an absolute
        // path without ceasing to be relocatable.
        //
        // `-l` resolves the right file on PE too: ld tries `libX.dll.a` before
        // `libX.a`, and lld-link/clang tries `X.lib` — which is exactly how
        // `import_library_for` names them. What was actually missing was the
        // import library being IN the package at all.
        //
        // Consequence to know: a consumer driven by NATIVE cl.exe cannot use
        // these flags (cl rejects `-L`). Recorded in docs/12 rather than papered
        // over — mcpp's own Windows default is clang, which takes them.
        //
        // `-Wl,-Bdynamic` is REQUIRED for a PE shared leg, and only there.
        // mcpp gives PE executables `-static` (the self-contained C++ runtime
        // contract: no libstdc++-6.dll beside the exe), and `-static` puts ld in
        // static-only mode, where it refuses an import library and reports
        // `cannot find -lmathkit / have you installed the static version of the
        // mathkit library?` — a message that names neither the DLL nor `-static`.
        // Switching to dynamic mode just before this `-l` fixes it; `-static`
        // arrives later on the line and still governs the runtime libraries
        // after it. Measured: without it the link fails, with it the program
        // links, deploys and runs.
        //
        // Only for env=gnu: an msvc-ABI shared leg cannot exist (make_plan
        // refuses it — MSVC exports nothing without dllexport), and lld-link
        // would not understand the flag if one did.
        const bool peGnuShared = leg.shared
            && leg.triple.find("windows-gnu") != std::string::npos;
        o += std::format("ldflags = [\"-Llib/{}\", {}\"-l{}\"]\n\n",
                         leg.triple,
                         peGnuShared ? "\"-Wl,-Bdynamic\", " : "",
                         leg.linkName);

        // …and, where it says the SAME thing, the dialect-neutral form. mcpp
        // renders these as `/LIBPATH:` + `<n>.lib` or `-L` + `-l<n>` from the
        // target, which is what lets a consumer driven by native `cl.exe` link
        // this package at all — cl rejects `-L`.
        //
        // BOTH are emitted, deliberately. An older mcpp reads only the ldflags
        // above and silently ignores this block (measured), so dropping the
        // ldflags would leave every older client with no link line at all. A
        // newer mcpp seeing this block drops that leg's library references and
        // uses these instead — see merge_conditional_config.
        //
        // NOT FOR A PE/MinGW SHARED LEG, and this was measured rather than
        // reasoned. That leg's line is `-L… -Wl,-Bdynamic -lmathkit`, and
        // `-Wl,-Bdynamic` only works IMMEDIATELY BEFORE the `-l` it enables:
        // mcpp gives PE executables `-static`, which leaves ld in static-only
        // mode where it refuses an import library. The neutral form has no way
        // to say "and switch link mode first", and rendering the two halves
        // through different slots separates the flag from its argument — e2e
        // 257 fails with `have you installed the static version of the mathkit
        // library?`. So that one leg keeps the spelling that works, and cl.exe
        // never sees it: a PE/GNU leg is not an MSVC-ABI leg.
        if (!peGnuShared) {
            o += std::format("[target.'{}'.runtime]\n", cfg_predicate_for(leg.triple));
            o += std::format("link_library_dirs = [\"lib/{}\"]\n", leg.triple);
            o += std::format("libraries         = [\"{}\"]\n\n", leg.linkName);
        }
    }
    // A shared library has to be FOUND at run time as well as linked, and the
    // two are different search paths — `link_library_dirs` is not rpath.
    if (doc.targetShared && !doc.legs.empty()) {
        o += "[runtime]\n";
        std::vector<std::string> dirs;
        for (auto const& leg : doc.legs) dirs.push_back(std::format("lib/{}", leg.triple));
        o += std::format("runtime_search_dirs = [{}]\n\n", join_quoted(dirs));
    }

    // ── dependencies ───────────────────────────────────────────────────
    //
    // A static archive does NOT carry its dependencies' code, so a consumer
    // that does not resolve them cannot link. Carrying the producer's
    // `[dependencies]` verbatim is what makes the package usable at all.
    if (!doc.dependencies.empty()) {
        o += "[dependencies]\n";
        for (auto const& [k, v] : doc.dependencies)
            o += std::format("{} = {}\n", quote(k), quote(v));
        o += "\n";
    }

    // ── evidence ───────────────────────────────────────────────────────
    for (auto const& leg : doc.legs) {
        o += "[[runtime.artifacts]]\n";
        o += std::format("role             = \"{}\"\n",
                         leg.shared ? "shared-library" : "static-library");
        o += std::format("path             = {}\n",
                         quote(std::format("lib/{}/{}", leg.triple, leg.libFile)));
        o += std::format("provenance       = {}\n",
                         quote(std::format("mcpp-pack {}", doc.builtBy)));
        if (!leg.abiTag.empty())   o += std::format("abi              = {}\n", quote(leg.abiTag));
        if (!leg.digest.empty())   o += std::format("digest           = {}\n", quote(leg.digest));
        if (!leg.buildKey.empty()) o += std::format("host_fingerprint = {}\n", quote(leg.buildKey));
        o += "\n";
    }
    if (!doc.interfaceDigest.empty()) {
        // One entry for the whole set, not one per file. What it defends
        // against is post-extraction editing, and "interface/ no longer
        // matches" is already actionable; per-file digests would add a line
        // per file to a document whose whole point is to stay readable.
        o += "[[runtime.artifacts]]\n";
        o += "role       = \"interface\"\n";
        o += std::format("path       = {}\n",
                         quote(doc.interfaceFiles.empty() ? "include" : "interface"));
        o += std::format("provenance = {}\n",
                         quote(std::format("mcpp-pack {}", doc.builtBy)));
        o += std::format("digest     = {}\n\n", quote(doc.interfaceDigest));
    }
    return o;
}

} // namespace mcpp::pack
