// mcpp.build.plan — backend-agnostic representation of "what to build".
//
// The pipeline is:
//   manifest + modgraph + toolchain + fingerprint  →  BuildPlan  →  Backend.build()

export module mcpp.build.plan;

import std;
import mcpp.manifest;
import mcpp.modgraph.graph;
import mcpp.modgraph.scanner;
import mcpp.toolchain.cppfly;
import mcpp.toolchain.detect;
import mcpp.toolchain.dialect;
import mcpp.toolchain.fingerprint;
import mcpp.platform;

export namespace mcpp::build {

struct CompileUnit {
    std::filesystem::path           source;
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
};

struct LinkUnit {
    std::string                     targetName;
    enum Kind { Binary, StaticLibrary, SharedLibrary, TestBinary } kind = Binary;
    std::vector<std::filesystem::path> objects;        // relative to plan.outputDir
    std::vector<std::filesystem::path> implicitInputs; // relative to plan.outputDir
    std::vector<std::string>        linkFlags;          // per-link edge flags
    std::filesystem::path           output;            // relative to plan.outputDir
    std::string                     soname;            // ABI name for shared libraries
    std::vector<std::filesystem::path> runtimeAliases; // relative aliases, e.g. bin/libfoo.so.1
    std::optional<std::filesystem::path> entryMain;   // src path of main.cpp for bin
};

struct BuildPlan {
    mcpp::manifest::Manifest        manifest;
    mcpp::toolchain::Toolchain      toolchain;
    mcpp::toolchain::Fingerprint    fingerprint;
    std::string                     cppStandard = "c++23";
    std::string                     cppStandardFlag = "-std=c++23";
    // Module-graph-global dialect flags (issue #210), pre-joined with a
    // leading space per flag (e.g. " -freflection"). Rides -std='s channels:
    // global $cxxflags (all TUs incl. deps), std BMI prebuild, scans.
    std::string                     dialectFlags;

    std::filesystem::path           projectRoot;      // where mcpp.toml lives
    std::filesystem::path           outputDir;        // target/<triple>/<fp>/
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

    std::vector<CompileUnit>        compileUnits;     // topologically sorted
    std::vector<LinkUnit>           linkUnits;
    std::vector<std::filesystem::path> runtimeLibraryDirs;
    // ONLY the dependency packages' [runtime] library_dirs (not toolchain/
    // payload dirs). These are the dirs that must be baked into the produced
    // binary's RUNPATH (e.g. compat.glx-runtime). Kept separate so static/musl
    // links don't pull the glibc payload dir.
    std::vector<std::filesystem::path> depRuntimeLibraryDirs;
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
    struct CapabilityProvider {
        std::string capability;
        std::string provider;
    };
    std::vector<CapabilityProvider>    runtimeProviders;
};

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
         const std::filesystem::path&            stdObjectPath);

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
    auto ext = src.extension();
    // Assembly siblings of a C/C++ TU commonly share its stem (foo.c +
    // foo.asm); keep the full extension in the object name so they can never
    // collide — the per-package collision prefix can't help two same-stem
    // files in the same directory.
    if (ext == ".S" || ext == ".s" || ext == ".asm") {
        return src.filename().string() + std::string(objExt);
    }
    auto stem = src.stem().string();
    // distinguish .cppm vs .cpp by extension prefix to avoid collisions
    return stem + (ext == ".cppm"
                       ? ".m" + std::string(objExt)
                       : std::string(objExt));
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

std::filesystem::path target_output(const mcpp::manifest::Target& t) {
    if (t.kind == mcpp::manifest::Target::Library) {
        return std::filesystem::path("bin") /
               std::format("{}{}{}", mcpp::platform::lib_prefix, t.name,
                           mcpp::platform::static_lib_ext);
    }
    if (t.kind == mcpp::manifest::Target::SharedLibrary) {
        return std::filesystem::path("bin") /
               std::format("{}{}{}", mcpp::platform::lib_prefix, t.name,
                           mcpp::platform::shared_lib_ext);
    }
    return std::filesystem::path("bin") /
           std::format("{}{}", t.name, mcpp::platform::exe_suffix);
}

std::vector<std::filesystem::path> runtime_aliases_for_target(
    const mcpp::manifest::Target& t) {
    std::vector<std::filesystem::path> aliases;
    if (t.kind != mcpp::manifest::Target::SharedLibrary || t.soname.empty()) {
        return aliases;
    }

    auto output = target_output(t);
    if (t.soname != output.filename().string()) {
        aliases.push_back(output.parent_path() / t.soname);
    }
    return aliases;
}

bool is_implementation_source(const std::filesystem::path& source) {
    auto ext = source.extension();
    return ext == ".cpp" || ext == ".cc" || ext == ".cxx" || ext == ".c" || ext == ".m"
        || ext == ".S" || ext == ".s" || ext == ".asm";
}

std::vector<std::string> shared_library_link_flags(const mcpp::manifest::Target& t) {
    std::vector<std::string> flags;
    if constexpr (mcpp::platform::is_windows) {
        flags.push_back(target_output(t).generic_string());
    } else {
        flags.push_back("-L" + target_output(t).parent_path().generic_string());
        if constexpr (mcpp::platform::supports_rpath) {
            if constexpr (mcpp::platform::is_macos) {
                flags.push_back("-Wl,-rpath,@loader_path");
            } else {
                flags.push_back("-Wl,-rpath,'$$ORIGIN'");
            }
        }
        flags.push_back("-l" + t.name);
    }
    return flags;
}

// #249 consistency fix: expand include_dirs entries with the same
// `expand_dir_glob` the dep path (prepare.cppm) uses, so a main-manifest
// `include_dirs = ["*/include"]` glob works identically here. For a literal
// (wildcard-free) entry expand_dir_glob only returns EXISTING directories,
// whereas this helper historically joined unconditionally — keep the plain
// join as a fallback so an -I for a dir created later (e.g. by a build
// step) isn't silently dropped.
std::vector<std::filesystem::path>
expand_manifest_include_entry(const std::filesystem::path& root,
                              const std::filesystem::path& inc)
{
    if (inc.is_absolute()) return { inc };
    const auto glob = inc.generic_string();
    auto expanded = mcpp::modgraph::expand_dir_glob(root, glob);
    if (expanded.empty() && glob.find('*') == std::string::npos)
        expanded.push_back(root / inc);
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

void append_unique_path(std::vector<std::filesystem::path>& out,
                        std::filesystem::path path)
{
    if (path.empty()) return;
    if (std::find(out.begin(), out.end(), path) == out.end())
        out.push_back(std::move(path));
}

} // namespace

// True if `src` defines a top-level `int main(` / `auto main(` entry point.
// Comments and string/char/raw-string literals are stripped first, so test
// fixtures that embed `"int main() {...}"` or R"(int main(){})" don't
// false-positive (that misfire chose archive linking for a no-main test →
// gtest_main.o not pulled by MSVC lld-link → LNK1561). Heuristic but robust;
// worst case is a sub-optimal archive-vs-inline choice, never a miscompile.
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
         const std::filesystem::path&            stdObjectPath)
{
    BuildPlan plan;
    plan.manifest         = manifest;
    plan.toolchain        = tc;
    plan.fingerprint      = fp;
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

    for (auto const& package : packages) {
        for (auto const& dir : package.manifest.runtimeConfig.libraryDirs) {
            auto abs = dir.is_absolute() ? dir : package.root / dir;
            append_unique_path(plan.runtimeLibraryDirs, abs);
            append_unique_path(plan.depRuntimeLibraryDirs, abs);
            // Windows runtime-DLL deployment: stage each *.dll from this dir
            // beside the produced executable (bin/). The *.dll filter — not a
            // platform guard — keeps this inert for real .so/.dylib deps, so
            // non-Windows builds are unchanged. See BuildPlan::DeployFile.
            std::error_code dirEc;
            if (std::filesystem::is_directory(abs, dirEc)) {
                for (auto const& entry :
                         std::filesystem::directory_iterator(abs, dirEc)) {
                    if (!entry.is_regular_file()) continue;
                    auto ext = entry.path().extension().string();
                    std::ranges::transform(ext, ext.begin(),
                        [](unsigned char c){ return std::tolower(c); });
                    if (ext != ".dll") continue;
                    std::filesystem::path dest =
                        std::filesystem::path("bin") / entry.path().filename();
                    if (std::ranges::none_of(plan.runtimeDeployFiles,
                            [&](auto const& d){ return d.dest == dest; }))
                        plan.runtimeDeployFiles.push_back({entry.path(), dest});
                }
            }
        }
        for (auto const& lib : package.manifest.runtimeConfig.dlopenLibs) {
            if (std::ranges::find(plan.runtimeDlopenLibs, lib) == plan.runtimeDlopenLibs.end())
                plan.runtimeDlopenLibs.push_back(lib);
        }
        for (auto const& cap : package.manifest.runtimeConfig.capabilities) {
            if (std::ranges::find(plan.runtimeCapabilities, cap) == plan.runtimeCapabilities.end())
                plan.runtimeCapabilities.push_back(cap);
        }
    }
    // Provider mapping (capability -> package), strongest first: packages
    // that explicitly `provides` a capability win over packages that merely
    // list it in `capabilities` (weak/back-compat providers). Downstream
    // lookups take the first match.
    for (auto const& package : packages) {
        for (auto const& cap : package.manifest.runtimeConfig.provides)
            plan.runtimeProviders.push_back({cap, package.manifest.package.name});
    }
    for (auto const& package : packages) {
        for (auto const& cap : package.manifest.runtimeConfig.capabilities) {
            bool dup = false;
            for (auto& pr : plan.runtimeProviders)
                if (pr.capability == cap
                    && pr.provider == package.manifest.package.name) { dup = true; break; }
            if (!dup) plan.runtimeProviders.push_back({cap, package.manifest.package.name});
        }
    }
    // The same private runtime directories embedded as executable RUNPATH are
    // also needed in the process environment for libraries reached only via
    // dlopen(), because their own DT_NEEDED closure does not consult the main
    // executable's RUNPATH.
    for (auto const& dir : tc.linkRuntimeDirs) {
        append_unique_path(plan.runtimeLibraryDirs, dir);
    }
    // The private glibc payload is the ONE entry that is not also in the
    // executable's RUNPATH (flags.cppm excludes it deliberately, so static and
    // musl links stay clean). It is here purely so a dlopen()'d library — whose
    // own DT_NEEDED closure never consults the main executable's RUNPATH — can
    // still resolve the same libc the executable was linked against.
    //
    // So add it ONLY when this build actually has such a library. mcpp#291:
    // LD_LIBRARY_PATH is inherited by the whole process subtree, and a child
    // that is a HOST binary (/bin/sh, reached via a provider's popen()) loads
    // the HOST loader — PT_INTERP is baked into the executable and no
    // environment variable can override it — while this variable hands it the
    // payload libc.so.6. libc and ld.so are version-locked to each other
    // through GLIBC_PRIVATE, so on any host whose glibc differs from the
    // payload's the shell dies of SIGSEGV inside the dynamic linker, before
    // main, with empty stdout and no diagnostic. (It does NOT reproduce when
    // host and payload glibc happen to match, which is why this survived.)
    //
    // process.cppm's strip_private_glibc already removes this entry from
    // mcpp's OWN children. It cannot help one hop further out: mcpp sets the
    // variable for the target deliberately, and what the target then spawns is
    // beyond mcpp's reach. Not emitting it unless it is needed is.
    if (tc.payloadPaths && !plan.depRuntimeLibraryDirs.empty()) {
        append_unique_path(plan.runtimeLibraryDirs, tc.payloadPaths->glibcLib);
    }

    // 1a. Detect basename collisions (both cross-package AND intra-package:
    //     ftxui ships dom/color.cpp + screen/color.cpp, for instance).
    //     For colliding files the object path gets a per-unit prefix.
    //
    //     mcpp#233: the prefix used to be derived from just the file's
    //     IMMEDIATE parent directory name (`<pkg>_<parent-dir>`), which
    //     itself collides whenever two files share a parent dir NAME at
    //     different depths — e.g. a/src/util.cpp and b/src/util.cpp both
    //     fold to `<pkg>_src/util.o`, and ninja rejects the plan with
    //     "multiple rules generate obj/...". The prefix now mirrors the
    //     unit's FULL relative directory instead (SourceUnit::relPath, set
    //     by the scanner against the unit's own package root), which is
    //     unique by construction: two distinct files under one package
    //     root can never share both relPath and basename. Non-colliding
    //     files keep the pre-existing flat `obj/<name>` layout untouched
    //     (back-compat for the overwhelmingly common single-file-per-
    //     basename project).
    std::set<std::filesystem::path> scannedSources;
    std::map<std::string, int> basenameCount;
    for (auto idx : topoOrder) {
        basenameCount[object_filename_for(graph.units[idx].path, objExt)]++;
        scannedSources.insert(graph.units[idx].path);
    }
    // mcpp#240: entry `main` sources are synthesized into compile units later
    // (during link assembly), NOT part of topoOrder — but they still occupy an
    // object path and must share ONE disambiguation census with everything
    // else. Count each root target's entry that isn't already scanned (a globbed
    // main IS scanned, so counting it again would falsely disambiguate the
    // common single-binary project). This makes "consumer main not globbed +
    // dependency ships a same-named main" disambiguate correctly too.
    for (auto& t : manifest.targets) {
        if (t.main.empty()) continue;
        if (t.kind != mcpp::manifest::Target::Binary
            && t.kind != mcpp::manifest::Target::TestBinary) continue;
        auto entry = projectRoot / t.main;
        if (scannedSources.contains(entry)) continue;
        basenameCount[object_filename_for(entry, objExt)]++;
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
    // mcpp#233/#240: the single source of truth for a compile unit's object
    // path — scanned units AND the synthesized entry main go through here, so
    // the link input can never diverge from the compile edge.
    auto object_for = [&](const std::filesystem::path& src,
                          const std::string& pkg,
                          const std::filesystem::path& relPath)
        -> std::filesystem::path {
        const auto fname = object_filename_for(src, objExt);
        if (basenameCount[fname] > 1)
            return std::filesystem::path("obj")
                 / safe_object_prefix(pkg, relPath.parent_path()) / fname;
        return std::filesystem::path("obj") / fname;
    };

    // 1. Compile units in topological order
    for (auto idx : topoOrder) {
        auto& u = graph.units[idx];
        CompileUnit cu;
        cu.source = u.path;
        cu.packageName = u.packageName;
        cu.localIncludeDirs = u.localIncludeDirs;
        cu.localIncludeDirsAfter = u.localIncludeDirsAfter;
        cu.packageCflags = u.packageCflags;
        cu.packageCxxflags = u.packageCxxflags;
        cu.packageAsmflags = u.packageAsmflags;
        cu.object = object_for(u.path, u.packageName, u.relPath);
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
                .output      = target_output(t),
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
        if (!is_implementation_source(cu.source)) continue;
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
                for (auto const& alias : runtime_aliases_for_target(dep.target))
                    lu.implicitInputs.push_back(alias);
                auto flags = shared_library_link_flags(dep.target);
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
            if (cu.source.extension() == ".cppm") {
                lu.objects.push_back(cu.object);
            }
        }
        for (auto& cu : plan.compileUnits) {
            if (cu.packageName != packageName) continue;
            if (!is_implementation_source(cu.source)) continue;
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
        lu.runtimeAliases = runtime_aliases_for_target(dep.target);
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
            lu.output = target_output(t);
        } else if (t.kind == mcpp::manifest::Target::SharedLibrary) {
            lu.kind   = LinkUnit::SharedLibrary;
            lu.output = target_output(t);
            lu.soname = t.soname;
            lu.runtimeAliases = runtime_aliases_for_target(t);
        } else if (t.kind == mcpp::manifest::Target::TestBinary) {
            lu.kind   = LinkUnit::TestBinary;
            lu.output = target_output(t);
            if (!t.main.empty()) lu.entryMain = projectRoot / t.main;
        } else {
            lu.kind   = LinkUnit::Binary;
            lu.output = target_output(t);
            if (!t.main.empty()) lu.entryMain = projectRoot / t.main;
        }

        // Include all module units' objects (they may be needed at runtime via global init).
        // For binary target, also include main.cpp's object if main is present.
        for (auto& cu : plan.compileUnits) {
            if (sharedDepPackages.contains(cu.packageName)) continue;
            if (cu.source.extension() == ".cppm") {
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
                main_cu.object = object_for(
                    main_cu.source, main_cu.packageName,
                    std::filesystem::relative(main_cu.source, projectRoot));
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
            if (!is_implementation_source(cu.source)) continue;
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
