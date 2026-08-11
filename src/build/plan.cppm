// mcpp.build.plan — backend-agnostic representation of "what to build".
//
// The pipeline is:
//   manifest + modgraph + toolchain + fingerprint  →  BuildPlan  →  Backend.build()

export module mcpp.build.plan;

import std;
import mcpp.build.graph_shape;
import mcpp.build.loader_contract;
import mcpp.manifest;
import mcpp.source_kind;
import mcpp.modgraph.graph;
import mcpp.modgraph.scanner;
import mcpp.toolchain.cppfly;
import mcpp.toolchain.detect;
import mcpp.toolchain.dialect;
import mcpp.toolchain.fingerprint;
import mcpp.toolchain.triple;
import mcpp.platform;
import mcpp.platform.runtime_binding;
import mcpp.platform.runtime_env_contract;
import mcpp.xlings.subos_info;

export namespace mcpp::build {

struct CompileUnit {
    std::filesystem::path           source;
    // The unit's ROLE. Copied from the SourceUnit the scanner classified, and
    // read by the object namer, the link-object collectors, the ninja rule
    // picker, the CDB emitter and the assembly dialect check — none of which
    // look at the extension any more. See mcpp.source_kind.
    //
    // Declared second so a hand-built unit reads `.source` then `.kind`. There
    // is no safe default: a unit whose kind was never set would route to the
    // generic C++ rule, which is a silent misroute rather than an error. Any
    // producer other than make_plan (i.e. a test) must state it.
    mcpp::SourceKind                kind = mcpp::SourceKind::Other;
    std::filesystem::path           object;            // relative to plan.outputDir
    std::string                     packageName;
    std::vector<std::filesystem::path> localIncludeDirs;
    // #249: emitted as -idirafter (searched after the toolchain's system
    // dirs) — a dep source root on this list can't shadow standard headers.
    std::vector<std::filesystem::path> localIncludeDirsAfter;
    std::vector<std::string>        packageCflags;
    std::vector<std::string>        packageCxxflags;
    std::vector<std::string>        packageAsmflags;   // per-glob asmflags (G4)
    std::optional<std::string>      providesModule;   // logical name, if .cppm export
    std::vector<std::string>        imports;           // logical names imported
    // Unit came from a scan_overrides declaration — plan-vs-ddi
    // verification is mandatory for it (ninja_backend emits --expect-*).
    bool                            scanOverridden = false;
    // This unit's outputs are already in the global cache: the backend emits
    // `stage_file` edges from the cache instead of a compile edge (and skips
    // the P1689 scan for it entirely). The unit itself stays in the plan so
    // compile_commands.json keeps an entry for it and clangd does not lose the
    // dependency's sources.
    bool                            servedFromCache = false;
    std::filesystem::path           cachedObject;   // absolute, inside the cache
    std::filesystem::path           cachedBmi;      // absolute; empty if no module
    // mcpp#344: this object's address INSIDE a global-cache entry — relative to
    // `<entry>/obj/`, and a pure function of the owning package (its source's
    // path relative to its own package root). Distinct from `object`, which is
    // a build-dir path and therefore depends on which other packages this
    // particular build contains.
    //
    // Empty means "no admissible cache address": the root package (never
    // cached), or a dependency source that could not be anchored to anything
    // machine-independent. prepare.cppm drops the WHOLE package out of the
    // cache when any of its units has an empty address — a half-staged package
    // mixes cached and freshly built BMIs, which is the mismatch GCC reports as
    // a CRC error three edges later.
    std::filesystem::path           packageObjectRel;
};

struct LinkUnit {
    std::string                     targetName;
    enum Kind { Binary, StaticLibrary, SharedLibrary, TestBinary } kind = Binary;
    // Normally relative to plan.outputDir. A `role = "object"` action's outputs
    // land here ABSOLUTE, on purpose: ninja identifies a file by the string an
    // edge declares, and the action edge declares whatever prepare_actions
    // produced — respelling it here would create a second node and "missing and
    // no known rule to make it". Do not normalise this vector.
    std::vector<std::filesystem::path> objects;
    std::vector<std::filesystem::path> implicitInputs; // relative to plan.outputDir
    std::vector<std::string>        linkFlags;          // per-link edge flags
    // The loader-tag flag for THIS unit's form (mcpp.build.loader_contract).
    // Separate from linkFlags because its correctness is positional: gcc specs
    // and clang config files hand ld `--enable-new-dtags`, last occurrence
    // wins, so the emitter puts this after every other linker argument.
    // Deciding it stays here; placing it is the emitter's business.
    std::string                     loaderTagFlag;
    std::filesystem::path           output;            // relative to plan.outputDir
    std::string                     soname;            // ABI name for shared libraries
    std::vector<std::filesystem::path> runtimeAliases; // relative aliases, e.g. bin/libfoo.so.1
    std::optional<std::filesystem::path> entryMain;   // src path of main.cpp for bin
};

// One Windows resource script compiled into one linkable resource artifact
// (mcpp#365).
//
// Deliberately NOT a CompileUnit. A `.rc` has no module semantics, so putting it
// there would drag it into the module graph, the topological order, the cache
// key and compile_commands.json — where clangd would be handed a file no C++
// frontend can parse. It is its own edge whose output joins `LinkUnit::objects`,
// which is the one thing `[build].ldflags` could never do: ldflags is a flat
// string in the link command, so a `.res` named there is invisible to ninja and
// changing it produced "no work to do".
struct ResourceUnit {
    std::filesystem::path              source;    // absolute; synthesised ones live under outputDir
    std::filesystem::path              output;    // relative to plan.outputDir
    // The `.rc`'s own inputs: quoted #includes and the data files named by its
    // resource statements. Neither windres nor llvm-rc can emit a depfile
    // (verified against llvm-rc 22.1.8: /I, /D, no dependency output), so these
    // come from a text scan plus `[resources].extra-inputs`.
    std::vector<std::filesystem::path> implicitInputs;
};

struct RuntimeCapabilityProvider {
    std::string                       capability;
    mcpp::manifest::PackageId         provider;
};

struct ResolvedRuntimeContract {
    std::vector<mcpp::manifest::RuntimeRequirement> requirements;
    std::vector<mcpp::manifest::RuntimeArtifact>    artifacts;
    mcpp::manifest::LinkIntent                      linkIntent;
    std::vector<RuntimeCapabilityProvider>           providers;
};

// Normalize structured and legacy runtime metadata and stamp every fact with
// the exact package identity that supplied it.  Exported as a pure seam for
// contract tests and machine-readable tooling; it performs no host probing.
ResolvedRuntimeContract resolve_runtime_contract(
    const std::vector<mcpp::modgraph::PackageRoot>& packages);

struct BuildPlan {
    mcpp::manifest::Manifest        manifest;
    mcpp::toolchain::Toolchain      toolchain;
    mcpp::toolchain::Fingerprint    fingerprint;
    // Which graph this plan will write into build.ninja. The fingerprint does
    // NOT cover dev-deps or test targets, so `mcpp build` and `mcpp test`
    // share an output directory and overwrite each other's graph; this is what
    // lets a fast path tell them apart (mcpp#407, mcpp.build.graph_shape).
    GraphShape                      graphShape = GraphShape::Normal;
    // One immutable snapshot selected before workspace member substitution.
    // Build/run/test and cache fast paths consume this value; none may re-read
    // xlings active/current state.
    mcpp::platform::runtime::RuntimeBinding runtimeBinding;
    std::string                     cppStandard = "c++23";
    std::string                     cppStandardFlag = "-std=c++23";
    // Module-graph-global dialect flags (issue #210), pre-joined with a
    // leading space per flag (e.g. " -freflection"). Rides -std='s channels:
    // global $cxxflags (all TUs incl. deps), std BMI prebuild, scans.
    std::string                     dialectFlags;

    std::filesystem::path           projectRoot;      // where mcpp.toml lives
    std::filesystem::path           outputDir;        // target/<triple>/<fp>/
    // Where compile_commands.json goes. Carried rather than derived from
    // projectRoot: under BuildOverrides::work_dir the package root is a shared
    // (possibly read-only) registry directory, and deriving the path would put
    // an IDE database there. Empty → projectRoot, the historical default.
    std::filesystem::path           compileDbPath;
    // GCC only: a specs file that replaces the pristine `*link:`, so the
    // payload's own (patched by every home that ever installed against it)
    // cannot inject rpath entries into this build's artifacts. Empty for
    // clang, which bypasses its cfg with --no-default-config instead.
    std::filesystem::path           gccCleanSpecs;
    std::filesystem::path           stdBmiPath;      // absolute path to prebuilt std.gcm
    std::filesystem::path           stdObjectPath;   // absolute path to prebuilt std.o
    std::filesystem::path           stdCompatBmiPath;    // absolute path to prebuilt std.compat.pcm
    std::filesystem::path           stdCompatObjectPath; // absolute path to prebuilt std.compat.o
    std::filesystem::path           scanDepsPath;    // clang-scan-deps binary (Clang only)
    // NASM assembly (.asm sources). Both resolved in prepare AFTER the plan
    // exists — only when the plan actually contains .asm units (lazy, hard
    // failure when unavailable; never a silent skip).
    std::filesystem::path           nasmPath;        // nasm binary (empty → no .asm units)
    std::string                     nasmFormat;      // -f value derived from the target triple
    // Windows resources (mcpp#365). Resolved in prepare AFTER the plan exists
    // and only when the plan actually has resource units — same lazy, hard-fail
    // shape as nasm above: a resource that silently vanished would show up as
    // "my icon is gone" with nothing to attribute it to.
    std::vector<ResourceUnit>       resourceUnits;
    std::filesystem::path           rcPath;          // windres / llvm-rc / rc.exe
    // "gnu"  → windres, emits a COFF object (ld cannot consume a .res)
    // "msvc" → rc.exe / llvm-rc, emits a .res (link.exe and lld-link take it)
    std::string                     rcStyle;
    std::vector<std::string>        rcFlags;         // -I / -D, target-shaped

    std::vector<CompileUnit>        compileUnits;     // topologically sorted
    std::vector<LinkUnit>           linkUnits;
    // Build-graph nodes declared by build programs (`mcpp:action=`). Paths are
    // absolute and engine variables already substituted by the time they get
    // here, so the backend only has to spell edges.
    std::vector<mcpp::manifest::BuildAction> actions;
    std::vector<std::filesystem::path> runtimeLibraryDirs;
    // ONLY the dependency packages' [runtime] library_dirs (not toolchain/
    // payload dirs). These are the dirs that must be baked into the produced
    // binary's RUNPATH (e.g. compat.glx-runtime). Kept separate so static/musl
    // links don't pull the glibc payload dir.
    std::vector<std::filesystem::path> depRuntimeLibraryDirs;
    std::vector<mcpp::manifest::RuntimeRequirement> runtimeRequirements;
    std::vector<mcpp::manifest::RuntimeArtifact>    runtimeArtifacts;
    mcpp::manifest::LinkIntent                      linkIntent;
    // Windows runtime-DLL deployment. On PE (`supports_rpath` is false) a
    // directly-launched .exe cannot RUNPATH-locate a dependency's DLL, so each
    // *.dll found in a dependency's [runtime] library_dir is copied beside the
    // produced executable (into bin/). The filter is the *.dll extension, not a
    // platform `if constexpr`: a real Linux/macOS dependency ships .so/.dylib
    // (never .dll), so this list is empty there and non-Windows builds are
    // byte-for-byte unchanged; only a Windows prebuilt-DLL package (or a test
    // that ships a .dll) populates it. dest is relative to outputDir.
    struct DeployFile {
        std::filesystem::path source;   // absolute source DLL
        std::filesystem::path dest;     // relative to outputDir, e.g. bin/libopenblas.dll
    };
    std::vector<DeployFile>            runtimeDeployFiles;
    // Aggregated host-runtime requirements from dependency packages'
    // [runtime] metadata. Capability/provider-driven — no platform special-casing
    // in mcpp: providers (e.g. compat.glx-runtime) declare these per platform.
    std::vector<std::string>           runtimeDlopenLibs;   // union of deps' dlopen sonames
    std::vector<std::string>           runtimeCapabilities; // union of host capabilities
    // (capability, provider package). A named aggregate instead of std::pair:
    // musl-gcc 15.1 modules failed to emit vector<pair<string,string>>'s
    // move-ctor instantiation across the module boundary (release link error).
    std::vector<RuntimeCapabilityProvider> runtimeProviders;
};

// Merge the generic facts exported by the already-selected xlings
// RuntimeBinding.  This is data ingestion only: xlings has already selected
// providers and materialized artifacts before mcpp sees this snapshot.
void merge_runtime_binding_contract(
    BuildPlan& plan,
    const mcpp::platform::runtime::RuntimeBinding& binding);

// Is `p` inside one of `roots`, judged LEXICALLY?
//
// Lexical is the whole point (mcpp#344). std::filesystem::relative() runs
// weakly_canonical on both sides and therefore RESOLVES SYMLINKS, and a payload
// store whose entries are symlinks into another store is ordinary — e2e's
// _inherit_toolchain.sh builds one, and so does any CI cache that links a warm
// payload tree into a fresh MCPP_HOME. Under canonicalization those packages
// stop looking like store packages and silently drop out of the build cache.
// Both the cacheability gate and the cache-address anchor ask "where was this
// installed", which is a question about the path, not about the inode — and
// they must answer it the same way, so there is one function.
bool path_is_under_any(const std::filesystem::path&              p,
                       const std::vector<std::filesystem::path>& roots);

// True if a source file defines a top-level `int main(`/`auto main(` entry,
// ignoring comments and string/raw-string literals. Drives the archive-vs-inline
// choice for kind="lib" dependencies (see plan.cppm).
bool source_defines_main(const std::filesystem::path& src);

// Build a BuildPlan from already-validated inputs. Fails (mcpp#233) only
// when the object-path uniqueness assertion below finds a residual
// collision after the relPath-mirroring scheme — a would-be ninja
// "multiple rules generate X" turned into a diagnosable mcpp error.
std::expected<BuildPlan, std::string>
make_plan(const mcpp::manifest::Manifest&         manifest,
         const mcpp::toolchain::Toolchain&       tc,
         const mcpp::toolchain::Fingerprint&     fp,
         const mcpp::modgraph::Graph&            graph,
         const std::vector<std::size_t>&         topoOrder,
         const std::vector<mcpp::modgraph::PackageRoot>& packages,
         const std::filesystem::path&            projectRoot,
         const std::filesystem::path&            outputDir,
         const std::filesystem::path&            stdBmiPath,
         const std::filesystem::path&            stdObjectPath,
         // Roots of the immutable xpkgs payload stores (there is more than
         // one: the global registry, plus the project-local `.mcpp/**/data`
         // roots a custom git index installs into). Used ONLY to anchor the
         // cache address of a dependency source that lives outside its own
         // package root (a build.mcpp OUT_DIR product). Empty is legal and
         // simply makes those units uncacheable.
         const std::vector<std::filesystem::path>& storeRoots = {});

// Expand one manifest `include_dirs` entry against the project root — the
// #249 consistency join + the expand_dir_glob the dep path uses. Exported
// (like modgraph's glob_literal_prefix) so unit tests can assert its
// native-separator contract directly; see the definition below.
std::vector<std::filesystem::path>
expand_manifest_include_entry(const std::filesystem::path& root,
                              const std::filesystem::path& inc);

} // namespace mcpp::build

namespace mcpp::build {

namespace {

std::string sanitize_for_path(std::string_view module_name) {
    std::string s;
    s.reserve(module_name.size());
    for (char c : module_name) {
        if (c == ':') s.push_back('-');
        else          s.push_back(c);
    }
    return s;
}

std::string object_filename_for(const std::filesystem::path& src,
                                std::string_view objExt = ".o") {
    // The naming POLICY lives in mcpp.source_kind (see ObjectNaming there for
    // why every historical name is frozen and only new extensions get the
    // collision-proof form). This function only formats it.
    switch (mcpp::object_naming_for(src)) {
        case mcpp::ObjectNaming::StemDotM:
            return src.stem().string() + ".m" + std::string(objExt);
        case mcpp::ObjectNaming::Stem:
            return src.stem().string() + std::string(objExt);
        case mcpp::ObjectNaming::FullFilename:
            break;
    }
    // Assembly siblings of a C/C++ TU commonly share its stem (foo.c +
    // foo.asm); keeping the full extension means they can never collide —
    // the per-package collision prefix can't help two same-stem files in the
    // same directory. Every extension a project adds via
    // `[build] module_extensions` lands here for the same reason.
    return src.filename().string() + std::string(objExt);
}

std::string qualified_package_name(const mcpp::manifest::Manifest& manifest) {
    if (!manifest.package.namespace_.empty()
        && manifest.package.name.starts_with(manifest.package.namespace_ + ".")) {
        return manifest.package.name;
    }
    if (manifest.package.namespace_.empty()) return manifest.package.name;
    return manifest.package.namespace_ + "." + manifest.package.name;
}

std::vector<std::string> dependency_name_candidates(
    const std::string& depName,
    const mcpp::manifest::DependencySpec& spec)
{
    std::vector<std::string> out;
    auto push = [&](std::string value) {
        if (value.empty()) return;
        if (std::find(out.begin(), out.end(), value) == out.end())
            out.push_back(std::move(value));
    };

    push(depName);
    if (!spec.shortName.empty()) push(spec.shortName);
    if (!spec.namespace_.empty() && !spec.shortName.empty()) {
        push(spec.namespace_ + "." + spec.shortName);
    }
    return out;
}

// The naming this MACHINE would use for its own binaries. Correct only for a
// host-target build; passed to artifact_naming() as the fallback for an empty
// triple, and never consulted directly when a target triple is present.
mcpp::toolchain::triple::ArtifactNaming host_artifact_naming() {
    return {
        .exeSuffix            = mcpp::platform::exe_suffix,
        .libPrefix            = mcpp::platform::lib_prefix,
        .staticLibExt         = mcpp::platform::static_lib_ext,
        .sharedLibExt         = mcpp::platform::shared_lib_ext,
        .sharedNeedsImportLib = mcpp::platform::is_windows,
    };
}

mcpp::toolchain::triple::ArtifactNaming naming_for(const mcpp::toolchain::Toolchain& tc) {
    auto t = mcpp::toolchain::triple::parse(tc.targetTriple);
    return mcpp::toolchain::triple::artifact_naming(
        t ? *t : mcpp::toolchain::triple::Triple{}, host_artifact_naming());
}

// What the artifact is CALLED — a property of the target, not of this machine.
// Reading the host constants here made ninja declare an output the compiler
// never writes (Linux -> PE: declared `bin/foo`, produced `bin/foo.exe`), so
// the link edge could never be satisfied and reran on every build.
std::filesystem::path target_output(const mcpp::manifest::Target& t,
                                    const mcpp::toolchain::triple::ArtifactNaming& n) {
    if (t.kind == mcpp::manifest::Target::Library) {
        return std::filesystem::path("bin") /
               std::format("{}{}{}", n.libPrefix, t.name, n.staticLibExt);
    }
    if (t.kind == mcpp::manifest::Target::SharedLibrary) {
        return std::filesystem::path("bin") /
               std::format("{}{}{}", n.libPrefix, t.name, n.sharedLibExt);
    }
    return std::filesystem::path("bin") /
           std::format("{}{}", t.name, n.exeSuffix);
}

std::vector<std::filesystem::path> runtime_aliases_for_target(
    const mcpp::manifest::Target& t,
    const mcpp::toolchain::triple::ArtifactNaming& n) {
    std::vector<std::filesystem::path> aliases;
    if (t.kind != mcpp::manifest::Target::SharedLibrary || t.soname.empty()) {
        return aliases;
    }

    auto output = target_output(t, n);
    if (t.soname != output.filename().string()) {
        aliases.push_back(output.parent_path() / t.soname);
    }
    return aliases;
}

// A unit whose object is linked because it CONTRIBUTES CODE, as opposed to a
// module interface (linked unconditionally, because its global initializers
// can matter even when no symbol of it is referenced).
//
// Kind-based rather than extension-based. One incidental fix comes with it:
// `.mm` (Objective-C++) was missing from the old list, so an Objective-C++
// object was compiled and then never linked.
bool is_implementation_source(mcpp::SourceKind kind) {
    return kind == mcpp::SourceKind::Cxx    || kind == mcpp::SourceKind::C
        || kind == mcpp::SourceKind::GasAsm || kind == mcpp::SourceKind::NasmAsm;
}

// How a CONSUMER links against a shared library. Also a target property: PE has
// no rpath and wants an import library, Mach-O uses @loader_path, ELF uses
// $ORIGIN. Keying this on the host pointed it the wrong way under cross builds.
//
// NOTE: shared libraries have never been verified end to end on PE or Mach-O —
// every shared-library e2e declares `# requires: elf`, and that capability is
// only granted on Linux. The PE branch here (linking the .dll path directly)
// is therefore unproven: mingw's ld tolerates it, MSVC's link.exe cannot.
// make_plan() rejects SharedLibrary targets on non-ELF targets rather than
// emitting something unverifiable — see the guard there.
std::vector<std::string> shared_library_link_flags(
    const mcpp::manifest::Target& t,
    const mcpp::toolchain::triple::ArtifactNaming& n,
    const mcpp::toolchain::triple::Triple& target) {
    std::vector<std::string> flags;
    const bool pe    = n.sharedNeedsImportLib;
    const bool macho = target.empty() ? bool(mcpp::platform::is_macos)
                                      : target.os == "macos";
    if (pe) {
        flags.push_back(target_output(t, n).generic_string());
    } else {
        flags.push_back("-L" + target_output(t, n).parent_path().generic_string());
        flags.push_back(macho ? "-Wl,-rpath,@loader_path"
                              : "-Wl,-rpath,'$$ORIGIN'");
        flags.push_back("-l" + t.name);
    }
    return flags;
}

}  // namespace

// #249 consistency fix: expand include_dirs entries with the same
// `expand_dir_glob` the dep path (prepare.cppm) uses, so a main-manifest
// `include_dirs = ["*/include"]` glob works identically here. For a literal
// (wildcard-free) entry expand_dir_glob only returns EXISTING directories,
// whereas this helper historically joined unconditionally — keep the plain
// join as a fallback so an -I for a dir created later (e.g. by a build
// step) isn't silently dropped.
//
// Deliberately OUTSIDE the anonymous namespace: it is exported for its unit
// test (like modgraph's glob_literal_prefix), and the two
// local_include_dirs_*_for_manifest consumers below ride along so a single
// namespace split serves the whole trio.
std::vector<std::filesystem::path>
expand_manifest_include_entry(const std::filesystem::path& root,
                              const std::filesystem::path& inc)
{
    if (inc.is_absolute()) {
        // A TOML value like `C:/SDL2/include` keeps its `/` on MSVC — make
        // it native so the CDB's -I (via local_include_args) is uniform.
        auto n = inc;
        n.make_preferred();
        return { std::move(n) };
    }
    const auto glob = inc.generic_string();
    auto expanded = mcpp::modgraph::expand_dir_glob(root, glob);
    if (expanded.empty() && glob.find('*') == std::string::npos) {
        // Same native-spelling rule for the bare join (see above): `root / p`
        // with a multi-segment `generated/inc` is MIXED on MSVC, and this
        // fallback exists precisely for dirs like `generated/` that a later
        // build step creates — the #390 shape.
        auto joined = root / inc;
        joined.make_preferred();
        expanded.push_back(std::move(joined));
    }
    return expanded;
}

std::vector<std::filesystem::path>
local_include_dirs_for_manifest(const std::filesystem::path& root,
                                const mcpp::manifest::Manifest& manifest)
{
    std::vector<std::filesystem::path> dirs;
    for (auto const& inc : manifest.buildConfig.includeDirs) {
        for (auto& d : expand_manifest_include_entry(root, inc))
            dirs.push_back(std::move(d));
    }
    return dirs;
}

// #249: same, for the -idirafter channel.
std::vector<std::filesystem::path>
local_include_dirs_after_for_manifest(const std::filesystem::path& root,
                                      const mcpp::manifest::Manifest& manifest)
{
    std::vector<std::filesystem::path> dirs;
    for (auto const& inc : manifest.buildConfig.includeDirsAfter) {
        for (auto& d : expand_manifest_include_entry(root, inc))
            dirs.push_back(std::move(d));
    }
    return dirs;
}

namespace {

void append_unique_path(std::vector<std::filesystem::path>& out,
                        std::filesystem::path path)
{
    if (path.empty()) return;
    if (std::find(out.begin(), out.end(), path) == out.end())
        out.push_back(std::move(path));
}

} // namespace

ResolvedRuntimeContract resolve_runtime_contract(
    const std::vector<mcpp::modgraph::PackageRoot>& packages)
{
    ResolvedRuntimeContract out;
    auto append_string = [](std::vector<std::string>& values, std::string value) {
        if (!value.empty() && std::ranges::find(values, value) == values.end())
            values.push_back(std::move(value));
    };
    auto absolute_from = [](const std::filesystem::path& root,
                            const std::filesystem::path& value) {
        return (value.is_absolute() ? value : root / value).lexically_normal();
    };
    auto append_requirement = [&](mcpp::manifest::RuntimeRequirement value) {
        const bool duplicate = std::ranges::any_of(out.requirements,
            [&](auto const& existing) {
                return existing.kind == value.kind
                    && existing.value == value.value
                    && existing.phase == value.phase
                    && existing.requester == value.requester
                    && existing.required == value.required;
            });
        if (!duplicate) out.requirements.push_back(std::move(value));
    };

    for (auto const& package : packages) {
        const auto id = mcpp::manifest::package_id(package.manifest.package);
        auto const& runtime = package.manifest.runtimeConfig;

        for (auto requirement : runtime.requirements) {
            requirement.requester = id;
            append_requirement(std::move(requirement));
        }
        // One compatibility train: old soname/capability lists are normalized
        // into the structured requirement shape.  They do NOT imply provider
        // ownership; only `provides` below creates a provider fact.
        for (auto const& soname : runtime.dlopenLibs) {
            append_requirement({
                .kind = "soname", .value = soname, .phase = "run",
                .requester = id, .required = true,
            });
        }
        for (auto const& capability : runtime.capabilities) {
            append_requirement({
                .kind = "capability", .value = capability, .phase = "run",
                .requester = id, .required = true,
            });
        }

        for (auto artifact : runtime.artifacts) {
            artifact.provider = id;
            artifact.path = absolute_from(package.root, artifact.path);
            const bool duplicate = std::ranges::any_of(out.artifacts,
                [&](auto const& existing) {
                    return existing.role == artifact.role
                        && existing.provider == artifact.provider
                        && existing.path == artifact.path
                        && existing.provenance == artifact.provenance
                        && existing.abi == artifact.abi
                        && existing.digest == artifact.digest
                        && existing.hostFingerprint == artifact.hostFingerprint;
                });
            if (!duplicate) out.artifacts.push_back(std::move(artifact));
        }

        for (auto const& capability : runtime.provides) {
            const bool duplicate = std::ranges::any_of(out.providers,
                [&](auto const& existing) {
                    return existing.capability == capability
                        && existing.provider == id;
                });
            if (!duplicate) out.providers.push_back({capability, id});
        }

        for (auto const& library : runtime.linkIntent.libraries) {
            const std::filesystem::path asPath(library);
            const bool explicitPath = !library.starts_with('-')
                && (asPath.is_absolute() || asPath.has_parent_path()
                    || asPath.has_extension());
            append_string(out.linkIntent.libraries,
                explicitPath ? absolute_from(package.root, asPath).string()
                             : library);
        }
        for (auto const& framework : runtime.linkIntent.frameworks)
            append_string(out.linkIntent.frameworks, framework);
        auto append_paths = [&](auto const& input, auto& output) {
            for (auto const& path : input)
                append_unique_path(output, absolute_from(package.root, path));
        };
        append_paths(runtime.linkIntent.linkLibraryDirs,
                     out.linkIntent.linkLibraryDirs);
        append_paths(runtime.linkIntent.transitiveNeededDirs,
                     out.linkIntent.transitiveNeededDirs);
        append_paths(runtime.linkIntent.runtimeSearchDirs,
                     out.linkIntent.runtimeSearchDirs);
        append_paths(runtime.linkIntent.deployFiles,
                     out.linkIntent.deployFiles);
        // Legacy library_dirs means run-time discovery only.  It deliberately
        // does not enter linkLibraryDirs; callers that need -L must opt into
        // the structured field.
        append_paths(runtime.libraryDirs, out.linkIntent.runtimeSearchDirs);
    }
    return out;
}

void merge_runtime_binding_contract(
    BuildPlan& plan,
    const mcpp::platform::runtime::RuntimeBinding& binding) {
    auto package_id = [](const mcpp::xlings::subos::PackageIdentity& value) {
        return mcpp::manifest::PackageId{
            .namespace_ = value.namespace_,
            .name = value.name,
            .version = value.version,
            .sourceProvenance = value.source,
        };
    };

    std::vector<RuntimeCapabilityProvider> selected;
    for (auto const& provider : binding.runtimeProviders) {
        RuntimeCapabilityProvider value{
            .capability = provider.capability,
            .provider = package_id(provider.provider),
        };
        if (std::ranges::none_of(selected, [&](auto const& existing) {
                return existing.capability == value.capability
                    && existing.provider == value.provider;
            }))
            selected.push_back(std::move(value));
    }
    // xlings' selected providers are authoritative and therefore precede
    // descriptor-declared fallback/provider facts.
    for (auto it = selected.rbegin(); it != selected.rend(); ++it) {
        if (std::ranges::none_of(plan.runtimeProviders, [&](auto const& existing) {
                return existing.capability == it->capability
                    && existing.provider == it->provider;
            }))
            plan.runtimeProviders.insert(plan.runtimeProviders.begin(), *it);
    }

    for (auto const& artifact : binding.runtimeArtifacts) {
        mcpp::manifest::RuntimeArtifact value{
            .role = artifact.role,
            .provider = package_id(artifact.provider),
            .path = artifact.path.lexically_normal(),
            .provenance = artifact.provenance,
            .abi = artifact.abi,
            .digest = artifact.digest,
            .hostFingerprint = artifact.hostFingerprint,
        };
        if (std::ranges::none_of(plan.runtimeArtifacts, [&](auto const& existing) {
                return existing.role == value.role
                    && existing.provider == value.provider
                    && existing.path == value.path
                    && existing.provenance == value.provenance
                    && existing.abi == value.abi
                    && existing.digest == value.digest
                    && existing.hostFingerprint == value.hostFingerprint;
            }))
            plan.runtimeArtifacts.push_back(std::move(value));
    }
}

// True if `src` defines a top-level `int main(` / `auto main(` entry point.
// Comments and string/char/raw-string literals are stripped first, so test
// fixtures that embed `"int main() {...}"` or R"(int main(){})" don't
// false-positive (that misfire chose archive linking for a no-main test →
// gtest_main.o not pulled by MSVC lld-link → LNK1561). Heuristic but robust;
// worst case is a sub-optimal archive-vs-inline choice, never a miscompile.
bool path_is_under_any(const std::filesystem::path&              p,
                       const std::vector<std::filesystem::path>& roots)
{
    // Empty = unrelated (different roots/drives). ".." or a "../" prefix =
    // outside. Everything else — including "." for the root itself — is in.
    auto inside = [](const std::filesystem::path& a,
                     const std::filesystem::path& b) {
        auto s = a.lexically_normal()
                  .lexically_relative(b.lexically_normal())
                  .generic_string();
        return !s.empty() && s != ".." && !s.starts_with("../");
    };
    for (auto const& root : roots) {
        if (root.empty()) continue;
        if (inside(p, root)) return true;
        // Retry on canonicalized paths. Lexical is the PRIMARY answer (it is
        // the only one that survives a symlinked store), but it also requires
        // the two paths to be spelled the same way, and mcpp's home is reached
        // through more than one spelling on Windows (HOME vs USERPROFILE, drive
        // letter case, 8.3 names). Both comparisons answer the same question
        // under different equivalence relations, and either "yes" is sufficient
        // evidence that the payload was installed into a store — so a spelling
        // difference degrades to a slower build, never to a wrong one.
        std::error_code e1, e2;
        auto cp = std::filesystem::weakly_canonical(p, e1);
        auto cr = std::filesystem::weakly_canonical(root, e2);
        if (!e1 && !e2 && inside(cp, cr)) return true;
    }
    return false;
}

bool source_defines_main(const std::filesystem::path& src) {
    std::ifstream is(src);
    if (!is) return false;
    std::string raw((std::istreambuf_iterator<char>(is)),
                    std::istreambuf_iterator<char>());
    std::string code;
    code.reserve(raw.size());
    enum State { Normal, Line, Block, Str, Chr, RawStr } st = Normal;
    std::string rawEnd;  // ")delim\"" terminator for the active raw string
    for (std::size_t i = 0; i < raw.size(); ++i) {
        char c = raw[i];
        char n = (i + 1 < raw.size()) ? raw[i + 1] : '\0';
        switch (st) {
        case Normal:
            if (c == 'R' && n == '"') {
                std::size_t j = i + 2;
                std::string delim;
                while (j < raw.size() && raw[j] != '(') delim.push_back(raw[j++]);
                rawEnd = ")" + delim + "\"";
                st = RawStr;
                i = j;  // sit on '(' ; loop ++ moves past
            } else if (c == '/' && n == '/') { st = Line; ++i; }
            else if (c == '/' && n == '*') { st = Block; ++i; }
            else if (c == '"')  { st = Str; }
            else if (c == '\'') { st = Chr; }
            else { code.push_back(c); }
            break;
        case Line:  if (c == '\n') { st = Normal; code.push_back(c); } break;
        case Block: if (c == '*' && n == '/') { st = Normal; ++i; } break;
        case Str:   if (c == '\\') ++i; else if (c == '"')  st = Normal; break;
        case Chr:   if (c == '\\') ++i; else if (c == '\'') st = Normal; break;
        case RawStr:
            if (raw.compare(i, rawEnd.size(), rawEnd) == 0) {
                st = Normal;
                i += rawEnd.size() - 1;
            }
            break;
        }
    }
    auto isws = [](char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
    };
    for (std::size_t i = 0; i + 4 <= code.size(); ++i) {
        if (code.compare(i, 4, "main") != 0) continue;
        std::size_t p = i;
        bool sawWs = false;
        while (p > 0 && isws(code[p - 1])) { --p; sawWs = true; }
        bool prevOk = sawWs && (
            (p >= 3 && code.compare(p - 3, 3, "int") == 0) ||
            (p >= 4 && code.compare(p - 4, 4, "auto") == 0));
        std::size_t q = i + 4;
        while (q < code.size() && isws(code[q])) ++q;
        bool nextOk = q < code.size() && code[q] == '(';
        if (prevOk && nextOk) return true;
    }
    return false;
}

std::expected<BuildPlan, std::string>
make_plan(const mcpp::manifest::Manifest&         manifest,
         const mcpp::toolchain::Toolchain&       tc,
         const mcpp::toolchain::Fingerprint&     fp,
         const mcpp::modgraph::Graph&            graph,
         const std::vector<std::size_t>&         topoOrder,
         const std::vector<mcpp::modgraph::PackageRoot>& packages,
         const std::filesystem::path&            projectRoot,
         const std::filesystem::path&            outputDir,
         const std::filesystem::path&            stdBmiPath,
         const std::filesystem::path&            stdObjectPath,
         const std::vector<std::filesystem::path>& storeRoots)
{
    BuildPlan plan;
    plan.manifest         = manifest;
    plan.toolchain        = tc;
    plan.fingerprint      = fp;

    // The ROOT package's extension table. Only the synthesized entry main
    // needs it — every scanned unit arrives with its kind already set by the
    // scanner, using its OWN package's table.
    const auto rootExtTable =
        mcpp::extension_table_for(manifest.buildConfig.moduleExtensions);

    // Artifact naming and shared-library link shape are properties of the
    // TARGET. Resolved once here from tc.targetTriple (empty = host target, in
    // which case the host constants ARE the right answer) and threaded down,
    // so nothing below reaches for mcpp::platform to describe an output.
    const auto targetTriple = [&] {
        auto t = mcpp::toolchain::triple::parse(tc.targetTriple);
        return t ? *t : mcpp::toolchain::triple::Triple{};
    }();
    const auto naming = naming_for(tc);

    // The loader-tag contract exists only where DT_RPATH/DT_RUNPATH do.
    // Mach-O and PE have neither, so they get no flag rather than a branch in
    // every consumer.
    const bool elfTarget = targetTriple.empty()
        ? bool(mcpp::platform::is_linux)
        : (targetTriple.os != "macos" && targetTriple.os != "windows");
    auto loader_tag_flag = [&](LinkUnit::Kind kind) -> std::string {
        if (!elfTarget) return {};
        using mcpp::build::loader::Form;
        Form form;
        switch (kind) {
            case LinkUnit::Binary:
            case LinkUnit::TestBinary:    form = Form::Executable; break;
            case LinkUnit::SharedLibrary: form = Form::SharedLibrary; break;
            // An archive has no dynamic section; whoever links it gets the
            // tag for their own form.
            case LinkUnit::StaticLibrary: return {};
        }
        auto flag = mcpp::build::loader::link_flag(
            mcpp::build::loader::required_tag(form));
        return flag ? std::string(*flag) : std::string{};
    };

    // Shared libraries have never been verified end to end on PE or Mach-O:
    // every shared-library e2e declares `# requires: elf`, and run_all.sh only
    // grants that capability on Linux. The non-ELF paths through
    // shared_library_link_flags are therefore unproven — mingw's ld tolerates
    // linking a .dll directly, MSVC's link.exe cannot, and neither has an
    // import library to link against because mcpp does not model one.
    //
    // Refusing is strictly better than emitting something unverifiable: a
    // branch that is neither tested nor willing to say no is the hardest kind
    // of debt, because it can be neither trusted nor deleted.
    if (!targetTriple.empty() && targetTriple.os != "linux") {
        for (auto const& t : manifest.targets) {
            if (t.kind != mcpp::manifest::Target::SharedLibrary) continue;
            return std::unexpected(std::format(
                "target '{}': shared libraries are only supported for Linux (ELF) "
                "targets today.\n"
                "  target '{}' is kind=\"shared\"; build it as kind=\"lib\" "
                "(static) for this target,\n"
                "  or build it for a linux target.\n"
                "  note: PE consumers need an import library and Mach-O needs "
                "install-name handling;\n"
                "        neither is modelled yet, so mcpp refuses rather than "
                "producing an artifact\n"
                "        nothing has ever verified.",
                targetTriple.str(), t.name));
        }
    }

    bool experimentalStd = false;
    if (auto stdCfg = mcpp::manifest::normalize_cpp_standard(manifest.package.standard)) {
        plan.cppStandard = stdCfg->canonical;
        experimentalStd  = stdCfg->experimental;
        // Spelled per-dialect ("-std=c++26" gnu vs "/std:c++latest" msvc) AND
        // per-toolchain-latest for c++latest/c++fly (raw canonical is not a
        // valid -std= spelling on GNU).
        plan.cppStandardFlag = mcpp::toolchain::cppfly::std_flag(
            tc, stdCfg->canonical, stdCfg->level);
    }
    // Graph-global dialect flags: manifest-declared ∪ c++fly gates — the same
    // merge prepare.cppm feeds the scan/std-BMI with (single source, #210).
    for (auto& f : mcpp::toolchain::cppfly::effective_dialect_flags(
             tc, experimentalStd,
             mcpp::manifest::dialect_flags(manifest.buildConfig))) {
        plan.dialectFlags += ' ';
        plan.dialectFlags += f;
    }
    // Object extension is dialect-spelled (.o vs .obj).
    const std::string_view objExt = mcpp::toolchain::dialect_for(tc).objExt;
    plan.projectRoot     = projectRoot;
    plan.outputDir       = outputDir;
    plan.stdBmiPath     = stdBmiPath;
    plan.stdObjectPath  = stdObjectPath;

    auto runtimeContract = resolve_runtime_contract(packages);
    plan.runtimeRequirements = std::move(runtimeContract.requirements);
    plan.runtimeArtifacts = std::move(runtimeContract.artifacts);
    plan.linkIntent = std::move(runtimeContract.linkIntent);
    plan.runtimeProviders = std::move(runtimeContract.providers);

    for (auto const& dir : plan.linkIntent.runtimeSearchDirs) {
        append_unique_path(plan.runtimeLibraryDirs, dir);
        append_unique_path(plan.depRuntimeLibraryDirs, dir);
    }
    for (auto const& requirement : plan.runtimeRequirements) {
        // Optional requirements remain first-class provenance in
        // resolution.json, but they cannot become hard ABI/doctor inputs.
        if (!requirement.required) continue;
        if (requirement.kind == "soname") {
            if (std::ranges::find(plan.runtimeDlopenLibs, requirement.value)
                == plan.runtimeDlopenLibs.end())
                plan.runtimeDlopenLibs.push_back(requirement.value);
        } else if (requirement.kind == "capability") {
            if (std::ranges::find(plan.runtimeCapabilities, requirement.value)
                == plan.runtimeCapabilities.end())
                plan.runtimeCapabilities.push_back(requirement.value);
        }
    }

    auto add_deploy = [&](const std::filesystem::path& source)
        -> std::optional<std::string> {
        const auto normalized = source.lexically_normal();
        const auto dest = std::filesystem::path("bin") / source.filename();
        auto existing = std::ranges::find_if(plan.runtimeDeployFiles,
            [&](auto const& value) { return value.dest == dest; });
        if (existing != plan.runtimeDeployFiles.end()) {
            if (existing->source.lexically_normal() != normalized) {
                return std::format(
                    "runtime deploy collision: '{}' and '{}' both target '{}'",
                    existing->source.string(), normalized.string(), dest.string());
            }
            return std::nullopt;
        }
        plan.runtimeDeployFiles.push_back({normalized, dest});
        return std::nullopt;
    };
    // Structured deploy files are explicit and platform-neutral.  Legacy
    // library_dirs keeps its one-train DLL discovery behavior below.
    for (auto const& source : plan.linkIntent.deployFiles) {
        if (auto collision = add_deploy(source))
            return std::unexpected(std::move(*collision));
    }
    for (auto const& dir : plan.linkIntent.runtimeSearchDirs) {
        std::error_code dirEc;
        if (!std::filesystem::is_directory(dir, dirEc)) continue;
        for (auto const& entry : std::filesystem::directory_iterator(dir, dirEc)) {
            if (!entry.is_regular_file()) continue;
            auto ext = entry.path().extension().string();
            std::ranges::transform(ext, ext.begin(),
                [](unsigned char c){ return std::tolower(c); });
            if (ext != ".dll") continue;
            if (auto collision = add_deploy(entry.path()))
                return std::unexpected(std::move(*collision));
        }
    }
    // The same private runtime directories embedded as executable RUNPATH are
    // also needed in the process environment for libraries reached only via
    // dlopen(), because their own DT_NEEDED closure does not consult the main
    // executable's RUNPATH.
    for (auto const& dir : tc.linkRuntimeDirs) {
        append_unique_path(plan.runtimeLibraryDirs, dir);
    }
    // The private glibc payload exists here for ONE reason: a dlopen()'d
    // library, whose own DT_NEEDED closure never consults the main
    // executable's RUNPATH, must still resolve the same libc the executable
    // was linked against. So it is only published when this build actually has
    // such a library.
    //
    // It is published through the ARTIFACT — the link model already emits
    // `-Wl,-rpath,<glibc>` alongside --dynamic-linker wherever a payload
    // exists — and never through the environment.
    // LD_LIBRARY_PATH is inherited by the whole process subtree, and a child
    // that is a HOST binary (/bin/sh, reached via popen()) loads the HOST
    // loader — PT_INTERP is baked in and no environment variable overrides it
    // — while the variable hands it the payload libc.so.6. libc and ld.so are
    // version-locked through GLIBC_PRIVATE, so the shell dies during
    // relocation, before main: glibc 2.44's libc.so.6 needs
    // `__pointer_chk_guard` from its own loader (mcpp#401), and older payloads
    // segfault in the linker instead (mcpp#291). It does NOT reproduce when
    // host and payload glibc happen to match, which is how it survived.
    //
    // process.cppm's strip_private_glibc already removes this entry from
    // mcpp's OWN children. It cannot help one hop further out — what the
    // target spawns is beyond mcpp's reach. Putting the directory in the
    // artifact's RUNPATH instead is: DT_RUNPATH reaches the object that
    // carries it and the dlopen() it performs, and nothing else.
    if constexpr (mcpp::platform::publishes_via_environment(
                      mcpp::platform::kPrivateLibcSearchScope)) {
        if (tc.payloadPaths && !plan.depRuntimeLibraryDirs.empty()) {
            append_unique_path(plan.runtimeLibraryDirs,
                               tc.payloadPaths->glibcLib);
        }
    }

    // 1a. Object addressing.
    //
    //     TWO addresses come out of one derivation here, and keeping it ONE
    //     derivation is the point (mcpp#344):
    //
    //       cu.object           where this build writes the object
    //       cu.packageObjectRel where the global cache stores it, if cacheable
    //
    //     The rule that makes them safe:
    //
    //       **A package's object layout may depend on that package and nothing
    //       else.**
    //
    //     It used to depend on the whole build. Basename disambiguation
    //     (mcpp#233) was driven by a census over EVERY unit in the graph, so
    //     `compat.zlib`'s compress.o was `obj/compress.o` in a project that
    //     pulled zlib alone and `obj/compat_zlib/zlib-1.3.2/compress.o` in one
    //     that also pulled `compat.bzip2` (which ships its own compress.c).
    //     The global cache key deliberately excludes the consumer — that is
    //     what makes cross-project sharing sound — so both layouts landed under
    //     one key and whichever project ran SECOND asked the entry for a file
    //     the first had never written. ninja rejects that at graph load with
    //     "missing and no known rule to make it", before any command runs.
    //
    //     #233 (compile edges collided), #240 (link inputs didn't follow the
    //     rename) and #344 are three products of the same machine: a layout
    //     decided by a global census. So the fix is not another place to keep
    //     in sync — it is to take dependencies out of the census entirely.
    //
    //       root package (never cached):
    //           obj/<name>.o                                  (historical)
    //           obj/<root-slug>/<mirrored relDir>/<name>.o    when the ROOT's
    //                                                         own sources
    //                                                         collide
    //       dependency package:
    //           obj/<pkg-slug>/<mirrored relDir>/<name>.o     unconditionally
    //
    //     Dependencies get no census at all. A conditional layout is exactly
    //     the state that generated this bug family, and all it buys is shorter
    //     paths; the mirrored relDir is unique by construction (two distinct
    //     files under one package root cannot share both relPath and basename),
    //     which is what the L1b assertion below still backstops.
    //
    //     The root keeps its flat layout because it is never cached and because
    //     `obj/main.o` is what every project has looked like since 0.0.1.
    //     Its census now spans only root-owned units, which is both correct
    //     (a dependency can no longer force the root to disambiguate) and
    //     sufficient (dependencies live in their own subtrees).

    // Owning package of a source. Longest matching root wins: package roots
    // nest (a workspace member lives under the workspace root) and the first
    // match would file the member's sources under the outer package. Index 0 is
    // the root project; `packages.size()` means "outside every known root",
    // which is treated as root-owned and never cached.
    auto owner_of = [&](const std::filesystem::path& src) -> std::size_t {
        std::size_t best = 0;
        std::size_t bestLen = 0;
        bool found = false;
        for (std::size_t p = 0; p < packages.size(); ++p) {
            std::error_code ec;
            auto rel = std::filesystem::relative(src, packages[p].root, ec);
            if (ec || rel.empty()) continue;
            if (rel.generic_string().starts_with("..")) continue;
            auto len = packages[p].root.generic_string().size();
            if (!found || len > bestLen) { best = p; bestLen = len; found = true; }
        }
        return found ? best : 0;
    };

    std::set<std::filesystem::path> scannedSources;
    std::map<std::string, int> rootBasenameCount;
    std::vector<std::size_t> unitOwner(graph.units.size(), 0);
    for (auto idx : topoOrder) {
        unitOwner[idx] = owner_of(graph.units[idx].path);
        scannedSources.insert(graph.units[idx].path);
        if (unitOwner[idx] == 0)
            rootBasenameCount[object_filename_for(graph.units[idx].path, objExt)]++;
    }
    // mcpp#240: entry `main` sources are synthesized into compile units later
    // (during link assembly), NOT part of topoOrder — but they still occupy an
    // object path and must share ONE disambiguation census with everything
    // else root-owned. Count each root target's entry that isn't already
    // scanned (a globbed main IS scanned, so counting it again would falsely
    // disambiguate the common single-binary project).
    for (auto& t : manifest.targets) {
        if (t.main.empty()) continue;
        if (t.kind != mcpp::manifest::Target::Binary
            && t.kind != mcpp::manifest::Target::TestBinary) continue;
        auto entry = projectRoot / t.main;
        if (scannedSources.contains(entry)) continue;
        rootBasenameCount[object_filename_for(entry, objExt)]++;
    }
    auto sanitize = [](const std::string& s) {
        std::string out; out.reserve(s.size());
        for (char c : s) out += (c == '.' || c == '/' ? '_' : c);
        return out;
    };
    // mcpp#239: fold a source's package-relative directory into an object
    // subdir that is ALWAYS downward AND shell-safe. relPath may be absolute or
    // carry `..` when the source lives outside its package root (e.g. a
    // dependency build.mcpp's OUT_DIR-generated source under
    // `.../<name>@<ver>/out/`) — pasting it straight into `obj/` both climbs
    // out of the build tree AND drags shell-hostile chars (the `@` in the deps
    // dir) into the object path, which ninja then single-quotes, breaking the
    // #235 `"$out.d"` depfile redirect. Map each component: drop the root
    // (`/`, drive) and `.`, turn `..` into `__up`, and replace any char outside
    // the portable set `[A-Za-z0-9._+-]` with `_`. The mapping is injective
    // enough to preserve the uniqueness the relPath-mirroring scheme (mcpp#233)
    // relies on (the L1b assertion backstops the residual).
    auto safe_component = [](std::string s) {
        if (s == "..") return std::string("__up");
        for (auto& c : s) {
            const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                         || (c >= '0' && c <= '9')
                         || c == '.' || c == '_' || c == '+' || c == '-';
            if (!ok) c = '_';
        }
        return s;
    };
    auto safe_object_prefix = [&](const std::string& pkg,
                                  const std::filesystem::path& relDir)
        -> std::filesystem::path {
        std::filesystem::path safe;
        for (auto const& comp : relDir) {
            if (comp.has_root_name() || comp.has_root_directory()) continue;
            auto s = comp.string();
            if (s.empty() || s == ".") continue;
            safe /= safe_component(s);
        }
        return pkg.empty() ? safe
                           : std::filesystem::path(sanitize(pkg)) / safe;
    };
    // mcpp#233/#240/#344: the single source of truth for a compile unit's
    // object addresses — scanned units AND the synthesized entry main go
    // through here, so neither the link input nor the cache address can
    // diverge from the compile edge.
    struct ObjectAddress {
        std::filesystem::path object;      // relative to outputDir
        std::filesystem::path packageRel;  // inside a cache entry; empty = uncacheable
    };
    auto object_for = [&](const std::filesystem::path& src,
                          const std::string& pkg,
                          const std::filesystem::path& relPath,
                          std::size_t owner) -> ObjectAddress
    {
        const auto fname = object_filename_for(src, objExt);
        if (owner == 0) {
            // Root project: never cached, historical layout preserved.
            if (rootBasenameCount[fname] > 1)
                return { std::filesystem::path("obj")
                       / safe_object_prefix(pkg, relPath.parent_path()) / fname,
                         {} };
            return { std::filesystem::path("obj") / fname, {} };
        }

        auto slug = sanitize(pkg.empty()
            ? qualified_package_name(packages[owner].manifest)
            : pkg);
        auto mirrored = safe_object_prefix({}, relPath.parent_path()) / fname;

        ObjectAddress addr;
        addr.object = std::filesystem::path("obj") / slug / mirrored;

        // The cache address additionally has to be MACHINE-independent: another
        // machine computes the same key and reads the same entry. A relPath
        // that stays inside the package root already is. One that escapes (a
        // build.mcpp OUT_DIR product living beside the payload) is re-anchored
        // at the xpkgs store root — the same `<store>`-relative trick
        // cache_key.cppm uses for include dirs — and when even that fails the
        // unit gets no address at all rather than one carrying this machine's
        // absolute paths.
        auto rels = relPath.generic_string();
        if (!relPath.empty() && !relPath.is_absolute() && !rels.starts_with("..")) {
            addr.packageRel = mirrored;
        } else {
            // Lexically — see path_is_under_any: a store built out of symlinks
            // is ordinary, and canonicalizing here would answer a different
            // question than the cacheability gate does.
            auto norm = src.lexically_normal();
            for (auto const& storeRoot : storeRoots) {
                if (storeRoot.empty()) continue;
                auto sr  = norm.lexically_relative(storeRoot.lexically_normal());
                auto srs = sr.generic_string();
                if (srs.empty() || srs == ".." || srs.starts_with("../")) continue;
                addr.packageRel = std::filesystem::path("__store")
                                / safe_object_prefix({}, sr.parent_path()) / fname;
                break;
            }
        }
        return addr;
    };

    // 1. Compile units in topological order
    for (auto idx : topoOrder) {
        auto& u = graph.units[idx];
        CompileUnit cu;
        cu.source = u.path;
        cu.packageName = u.packageName;
        cu.kind        = u.kind;
        cu.localIncludeDirs = u.localIncludeDirs;
        cu.localIncludeDirsAfter = u.localIncludeDirsAfter;
        cu.packageCflags = u.packageCflags;
        cu.packageCxxflags = u.packageCxxflags;
        cu.packageAsmflags = u.packageAsmflags;
        {
            auto addr = object_for(u.path, u.packageName, u.relPath, unitOwner[idx]);
            cu.object           = std::move(addr.object);
            cu.packageObjectRel = std::move(addr.packageRel);
        }
        if (u.provides) {
            cu.providesModule = u.provides->logicalName;
        }
        for (auto& req : u.requires_) cu.imports.push_back(req.logicalName);
        cu.scanOverridden = u.scanOverridden;
        plan.compileUnits.push_back(std::move(cu));
    }

    // 1b. mcpp#233: uniqueness assertion. The relDir-mirroring prefix above
    // is unique by construction for any two distinct source files (see the
    // reasoning in 1a), so this should never fire — it is a defensive
    // backstop turning a would-be ninja "multiple rules generate X" hard
    // stop into a diagnosable mcpp error that names the colliding sources,
    // for any input the reasoning above didn't anticipate.
    {
        std::map<std::string, std::vector<std::filesystem::path>> byObject;
        for (auto& cu : plan.compileUnits) {
            byObject[cu.object.generic_string()].push_back(cu.source);
        }
        std::string collisions;
        for (auto& [obj, srcs] : byObject) {
            if (srcs.size() < 2) continue;
            if (!collisions.empty()) collisions += "; ";
            collisions += obj + " <- ";
            for (std::size_t i = 0; i < srcs.size(); ++i) {
                if (i) collisions += ", ";
                collisions += srcs[i].string();
            }
        }
        if (!collisions.empty()) {
            return std::unexpected(std::format(
                "internal error: object path collision after uniqueness "
                "pass (please report): {}", collisions));
        }
    }

    // 2. Build map of module-name → compile unit (for inter-unit dep resolution)
    std::map<std::string, std::size_t> producerOf;
    for (std::size_t i = 0; i < plan.compileUnits.size(); ++i) {
        if (plan.compileUnits[i].providesModule) {
            producerOf[*plan.compileUnits[i].providesModule] = i;
        }
    }

    // 3. Compute the set of all targets' entry .cpp files. Each entry is
    //    exclusive to its target — when assembling another target's link
    //    image we must NOT pull in foreign entries (they each define
    //    `int main(...)`, causing multiple-definition link errors).
    std::set<std::filesystem::path> entryFilesAcrossTargets;
    for (auto& t : manifest.targets) {
        if (!t.main.empty()) {
            entryFilesAcrossTargets.insert(projectRoot / t.main);
        }
    }
    for (auto const& p : packages) {
        for (auto const& t : p.manifest.targets) {
            if (!t.main.empty()) {
                entryFilesAcrossTargets.insert(p.root / t.main);
            }
        }
    }

    struct SharedDepTarget {
        std::size_t                   packageIndex = 0;
        std::string                   packageName;
        mcpp::manifest::Target        target;
        std::filesystem::path         output;
    };
    std::vector<SharedDepTarget> sharedDepTargets;
    std::set<std::string> sharedDepPackages;
    std::map<std::size_t, std::vector<std::size_t>> sharedTargetsByPackage;
    std::map<std::string, std::size_t, std::less<>> packageIndexByName;
    for (std::size_t i = 0; i < packages.size(); ++i) {
        auto const& p = packages[i];
        packageIndexByName[qualified_package_name(p.manifest)] = i;
        packageIndexByName[p.manifest.package.name] = i;
    }

    for (std::size_t i = 1; i < packages.size(); ++i) {
        auto const& p = packages[i];
        auto qname = qualified_package_name(p.manifest);
        for (auto const& t : p.manifest.targets) {
            if (t.kind != mcpp::manifest::Target::SharedLibrary) continue;
            sharedDepPackages.insert(qname);
            const auto targetIndex = sharedDepTargets.size();
            sharedDepTargets.push_back(SharedDepTarget{
                .packageIndex = i,
                .packageName = qname,
                .target      = t,
                .output      = target_output(t, naming),
            });
            sharedTargetsByPackage[i].push_back(targetIndex);
        }
    }

    // Dependency-provided optional entry objects (e.g. gtest's gtest_main.cc,
    // which defines its own `main`). A consumer must link such an object ONLY
    // when it has no `main` of its own — otherwise `duplicate symbol: main`.
    //
    // We keep ALL dependency objects INLINED (the long-standing model) and just
    // drop these specific entry objects from self-main consumers. An earlier
    // attempt linked kind="lib" deps as static archives instead, but that is not
    // viable on Windows/MSVC lld-link: (1) it won't pull an archive member just
    // to satisfy the entry point (LNK1561), and (2) archiving regular libs broke
    // transitive symbol resolution order (libarchive→lzma LNK2019 in xlings).
    // Inlining + dropping only the entry object is portable and minimal — it
    // leaves every other dependency's linkage byte-for-byte unchanged.
    //
    // SCOPE: only DEV-dependencies are considered. Test frameworks (gtest, future
    // mcpplibs/native frameworks) are dev-deps; regular deps (libarchive, lzma,
    // …) must NEVER be touched — a false-positive there would drop a needed
    // object (e.g. archive_entry.o) and break normal binaries like xlings. Dev-
    // deps are absent from `mcpp build` (includeDevDeps=false) entirely, so plain
    // builds are unaffected by construction.
    //
    // Detected by scanning each dev-dep implementation source for a top-level
    // main (gtest_main.cc has one; gtest-all.cc does not). Generic: no per-
    // framework knowledge — any framework's main-providing object is handled the
    // same way.
    std::set<std::string> devDepPackages;
    for (auto const& [depName, spec] : manifest.devDependencies) {
        for (auto const& candidate : dependency_name_candidates(depName, spec)) {
            auto it = packageIndexByName.find(candidate);
            if (it != packageIndexByName.end()) {
                devDepPackages.insert(qualified_package_name(packages[it->second].manifest));
                break;
            }
        }
    }
    std::set<std::filesystem::path> depEntryMainSources;
    for (auto& cu : plan.compileUnits) {
        if (!devDepPackages.contains(cu.packageName)) continue;
        if (!is_implementation_source(cu.kind)) continue;
        if (source_defines_main(cu.source)) depEntryMainSources.insert(cu.source);
    }

    std::map<std::size_t, std::vector<std::size_t>> directPackageDeps;
    for (std::size_t i = 0; i < packages.size(); ++i) {
        for (auto const& [depName, spec] : packages[i].manifest.dependencies) {
            for (auto const& candidate : dependency_name_candidates(depName, spec)) {
                auto it = packageIndexByName.find(candidate);
                if (it == packageIndexByName.end() || it->second == i) continue;
                auto& deps = directPackageDeps[i];
                if (std::find(deps.begin(), deps.end(), it->second) == deps.end())
                    deps.push_back(it->second);
                break;
            }
        }
    }

    auto append_direct_shared_deps = [&](LinkUnit& lu, std::size_t packageIndex) {
        auto depsIt = directPackageDeps.find(packageIndex);
        if (depsIt == directPackageDeps.end()) return;
        for (auto depIndex : depsIt->second) {
            auto targetsIt = sharedTargetsByPackage.find(depIndex);
            if (targetsIt == sharedTargetsByPackage.end()) continue;
            for (auto targetIndex : targetsIt->second) {
                auto const& dep = sharedDepTargets[targetIndex];
                lu.implicitInputs.push_back(dep.output);
                // The SONAME alias is a prerequisite too, not a by-product: the
                // library is written as bin/libX11.so but records SONAME
                // libX11.so.6, so both the linker (resolving a transitive
                // NEEDED) and the loader look for the alias, never for the
                // plain name. Hanging it off the consumer keeps it correct
                // under explicit ninja goals — `mcpp test` names the test
                // binaries, and an alias edge that nothing depends on is
                // reachable only through `default`, so it was silently skipped
                // (0.0.104-0.0.106). The failure surfaced far away, as
                // `libX11.so: undefined reference to xcb_connect` or a test
                // exiting 127.
                for (auto const& alias : runtime_aliases_for_target(dep.target, naming))
                    lu.implicitInputs.push_back(alias);
                auto flags = shared_library_link_flags(dep.target, naming, targetTriple);
                lu.linkFlags.insert(lu.linkFlags.end(), flags.begin(), flags.end());
            }
        }
    };

    auto append_shared_deps_for_linked_objects = [&](LinkUnit& lu) {
        std::set<std::size_t> linkedPackages;
        linkedPackages.insert(0);
        for (auto& cu : plan.compileUnits) {
            if (sharedDepPackages.contains(cu.packageName)) continue;
            auto it = packageIndexByName.find(cu.packageName);
            if (it == packageIndexByName.end()) continue;
            linkedPackages.insert(it->second);
        }

        for (auto packageIndex : linkedPackages) {
            append_direct_shared_deps(lu, packageIndex);
        }
    };

    auto append_package_objects = [&](LinkUnit& lu, const std::string& packageName) {
        for (auto& cu : plan.compileUnits) {
            if (cu.packageName != packageName) continue;
            if (mcpp::links_unconditionally(cu.kind)) {
                lu.objects.push_back(cu.object);
            }
        }
        for (auto& cu : plan.compileUnits) {
            if (cu.packageName != packageName) continue;
            if (!is_implementation_source(cu.kind)) continue;
            if (lu.entryMain && cu.source == *lu.entryMain) continue;
            if (entryFilesAcrossTargets.contains(cu.source)) continue;
            lu.objects.push_back(cu.object);
        }
    };

    for (auto const& dep : sharedDepTargets) {
        LinkUnit lu;
        lu.targetName = dep.target.name;
        lu.kind       = LinkUnit::SharedLibrary;
        lu.output     = dep.output;
        lu.soname     = dep.target.soname;
        lu.runtimeAliases = runtime_aliases_for_target(dep.target, naming);
        lu.loaderTagFlag = loader_tag_flag(lu.kind);
        append_package_objects(lu, dep.packageName);
        append_direct_shared_deps(lu, dep.packageIndex);
        plan.linkUnits.push_back(std::move(lu));
    }

    // 4. Link units (one per [targets.X])
    // When any TestBinary target exists, skip Binary/Library/SharedLibrary
    // targets — `mcpp test` only cares about the test binaries, and pulling
    // dev-deps' .o (e.g. gtest_main.cc with its own main()) into the
    // project's regular bin would cause `multiple definition of 'main'`.
    bool inTestMode = false;
    for (auto& t : manifest.targets) {
        if (t.kind == mcpp::manifest::Target::TestBinary) { inTestMode = true; break; }
    }
    for (auto& t : manifest.targets) {
        if (inTestMode && t.kind != mcpp::manifest::Target::TestBinary) continue;
        LinkUnit lu;
        lu.targetName = t.name;
        if (t.kind == mcpp::manifest::Target::Library) {
            lu.kind   = LinkUnit::StaticLibrary;
            lu.output = target_output(t, naming);
        } else if (t.kind == mcpp::manifest::Target::SharedLibrary) {
            lu.kind   = LinkUnit::SharedLibrary;
            lu.output = target_output(t, naming);
            lu.soname = t.soname;
            lu.runtimeAliases = runtime_aliases_for_target(t, naming);
        } else if (t.kind == mcpp::manifest::Target::TestBinary) {
            lu.kind   = LinkUnit::TestBinary;
            lu.output = target_output(t, naming);
            if (!t.main.empty()) lu.entryMain = projectRoot / t.main;
        } else {
            lu.kind   = LinkUnit::Binary;
            lu.output = target_output(t, naming);
            if (!t.main.empty()) lu.entryMain = projectRoot / t.main;
        }
        lu.loaderTagFlag = loader_tag_flag(lu.kind);

        // Include all module units' objects (they may be needed at runtime via global init).
        // For binary target, also include main.cpp's object if main is present.
        for (auto& cu : plan.compileUnits) {
            if (sharedDepPackages.contains(cu.packageName)) continue;
            if (mcpp::links_unconditionally(cu.kind)) {
                lu.objects.push_back(cu.object);
            }
        }

        // Whether this consumer's own entry source defines `main`. Decides how
        // kind="lib" dependencies are linked (archive vs inline) so the
        // gtest_main-style optional entry works on EVERY linker — see the
        // dependency-linking block further below. Can't tell (no entry) →
        // false → inline (the pre-archive behavior, always provides the entry).
        bool entryDefinesMain = lu.entryMain && source_defines_main(*lu.entryMain);

        if ((lu.kind == LinkUnit::Binary || lu.kind == LinkUnit::TestBinary) && lu.entryMain) {
            // Synthesize the entry main's compile unit. Its object path is
            // NOT computed here — it comes from the shared `object_for`
            // disambiguator below, so the link input matches the compile edge
            // even when the entry collides with a dependency's same-named
            // source (mcpp#240).
            CompileUnit main_cu;
            main_cu.source = *lu.entryMain;
            main_cu.packageName = qualified_package_name(manifest);
            // Synthesized outside the scanner, so it has no SourceUnit to copy
            // from — classify it here with the ROOT package's table (an entry
            // main is resolved against projectRoot by definition).
            main_cu.kind = mcpp::classify(main_cu.source, rootExtTable);
            if (!packages.empty() && packages[0].usageResolved) {
                main_cu.localIncludeDirs = packages[0].privateBuild.includeDirs;
                main_cu.localIncludeDirsAfter = packages[0].privateBuild.includeDirsAfter;
                main_cu.packageCflags = packages[0].privateBuild.cflags;
                main_cu.packageCxxflags = packages[0].privateBuild.cxxflags;
            } else {
                main_cu.localIncludeDirs = local_include_dirs_for_manifest(projectRoot, manifest);
                main_cu.localIncludeDirsAfter =
                    local_include_dirs_after_for_manifest(projectRoot, manifest);
                main_cu.packageCflags = manifest.buildConfig.cflags;
                main_cu.packageCxxflags = manifest.buildConfig.cxxflags;
            }
            // Root-relative -I flags → absolute (G8b), mirroring the scanner's
            // treatment of every scanned unit.
            mcpp::modgraph::normalize_include_flags(projectRoot, main_cu.packageCflags);
            mcpp::modgraph::normalize_include_flags(projectRoot, main_cu.packageCxxflags);

            // We didn't scan main.cpp earlier (it's not in scanner output unless globbed in).
            // Best-effort: scan its imports here.
            std::ifstream is(*lu.entryMain);
            std::string line;
            while (std::getline(is, line)) {
                auto trim = [](std::string s) {
                    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(0, 1);
                    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))  s.pop_back();
                    return s;
                };
                line = trim(line);
                if (line.starts_with("import ")) {
                    std::string name;
                    std::size_t i = 7;
                    while (i < line.size() && (std::isalnum(static_cast<unsigned char>(line[i]))
                                               || line[i] == '_' || line[i] == '.')) {
                        name.push_back(line[i]);
                        ++i;
                    }
                    if (!name.empty()) main_cu.imports.push_back(name);
                }
            }

            // mcpp#240: the entry main may ALSO have been scanned (globbed into
            // [modules].sources — the near-universal `src/**/*.cpp`). When it
            // was, reuse THAT compile unit's already-disambiguated object; the
            // synthesized main_cu is a duplicate and must not be linked under a
            // divergent (flat) path. When it wasn't scanned, route the
            // synthesized unit through the same `object_for` census so it, too,
            // disambiguates against any same-named dependency source.
            std::filesystem::path entryObject;
            bool already = false;
            for (auto& cu : plan.compileUnits) {
                if (cu.source == main_cu.source) {
                    already = true;
                    entryObject = cu.object;
                    break;
                }
            }
            if (!already) {
                // Entry mains belong to the root project (`t.main` is resolved
                // against projectRoot), which is owner 0 and never cached.
                main_cu.object = object_for(
                    main_cu.source, main_cu.packageName,
                    std::filesystem::relative(main_cu.source, projectRoot),
                    /*owner=*/0).object;
                plan.compileUnits.push_back(main_cu);
                entryObject = main_cu.object;
            }
            lu.objects.push_back(entryObject);

            // Per-target entry-scoped flags (issue #131). Applied to the compile
            // unit that actually builds this target's entry — which may be the
            // one just inserted, or a unit the source scan already produced when
            // main was globbed into [build].sources. SCOPE: a target's entry is
            // exclusive to it (distinct `main` per target, and foreign entries
            // are excluded from every other target's object set below), so these
            // flags never color a shared module/impl object. `defines` desugar
            // to -D on both the C and C++ entry compile.
            if (!t.defines.empty() || !t.cflags.empty() || !t.cxxflags.empty()) {
                for (auto& cu : plan.compileUnits) {
                    if (cu.source != main_cu.source) continue;
                    for (auto const& d : t.defines) {
                        cu.packageCflags.push_back("-D" + d);
                        cu.packageCxxflags.push_back("-D" + d);
                    }
                    for (auto const& f : t.cflags)   cu.packageCflags.push_back(f);
                    for (auto const& f : t.cxxflags) cu.packageCxxflags.push_back(f);
                    break;
                }
            }
        }

        // Also include implementation .cpp/.cc/.cxx/.c units, but EXCLUDE any
        // file registered as another target's entryMain (each binary's main()
        // is exclusive to that binary).
        for (auto& cu : plan.compileUnits) {
            if (sharedDepPackages.contains(cu.packageName)) continue;
            if (!is_implementation_source(cu.kind)) continue;
            if (lu.entryMain && cu.source == *lu.entryMain) continue;     // own entry: already added above
            if (entryFilesAcrossTargets.contains(cu.source)) continue;     // foreign entry: skip
            // A dependency's own main-providing object (e.g. gtest_main.o): link
            // it ONLY when this consumer has no main of its own. With its own
            // main, including it would be `duplicate symbol: main`; without one,
            // it supplies the entry (gtest-style). Works on every linker — the
            // object is linked directly, never relying on archive member pulling.
            if (entryDefinesMain && depEntryMainSources.contains(cu.source)) continue;
            lu.objects.push_back(cu.object);
        }

        if (lu.kind == LinkUnit::Binary || lu.kind == LinkUnit::TestBinary
            || lu.kind == LinkUnit::SharedLibrary) {
            append_shared_deps_for_linked_objects(lu);
        }

        plan.linkUnits.push_back(std::move(lu));
    }

    return plan;
}

} // namespace mcpp::build
