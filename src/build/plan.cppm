// mcpp.build.plan — backend-agnostic representation of "what to build".
//
// The pipeline is:
//   manifest + modgraph + toolchain + fingerprint  →  BuildPlan  →  Backend.build()

export module mcpp.build.plan;

import std;
import mcpp.manifest;
import mcpp.modgraph.graph;
import mcpp.modgraph.scanner;
import mcpp.toolchain.detect;
import mcpp.toolchain.fingerprint;
import mcpp.platform;

export namespace mcpp::build {

struct CompileUnit {
    std::filesystem::path           source;
    std::filesystem::path           object;            // relative to plan.outputDir
    std::string                     packageName;
    std::vector<std::filesystem::path> localIncludeDirs;
    std::vector<std::string>        packageCflags;
    std::vector<std::string>        packageCxxflags;
    std::optional<std::string>      providesModule;   // logical name, if .cppm export
    std::vector<std::string>        imports;           // logical names imported
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

    std::filesystem::path           projectRoot;      // where mcpp.toml lives
    std::filesystem::path           outputDir;        // target/<triple>/<fp>/
    std::filesystem::path           stdBmiPath;      // absolute path to prebuilt std.gcm
    std::filesystem::path           stdObjectPath;   // absolute path to prebuilt std.o
    std::filesystem::path           stdCompatBmiPath;    // absolute path to prebuilt std.compat.pcm
    std::filesystem::path           stdCompatObjectPath; // absolute path to prebuilt std.compat.o
    std::filesystem::path           scanDepsPath;    // clang-scan-deps binary (Clang only)

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

// Build a BuildPlan from already-validated inputs.
BuildPlan make_plan(const mcpp::manifest::Manifest&         manifest,
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

std::string object_filename_for(const std::filesystem::path& src) {
    auto stem = src.stem().string();
    // distinguish .cppm vs .cpp by extension prefix to avoid collisions
    return stem + (src.extension() == ".cppm" ? ".m.o" : ".o");
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
    return ext == ".cpp" || ext == ".cc" || ext == ".cxx" || ext == ".c" || ext == ".m";
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

std::vector<std::filesystem::path>
local_include_dirs_for_manifest(const std::filesystem::path& root,
                                const mcpp::manifest::Manifest& manifest)
{
    std::vector<std::filesystem::path> dirs;
    for (auto const& inc : manifest.buildConfig.includeDirs) {
        dirs.push_back(inc.is_absolute() ? inc : root / inc);
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

BuildPlan make_plan(const mcpp::manifest::Manifest&         manifest,
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
    if (auto stdCfg = mcpp::manifest::normalize_cpp_standard(manifest.package.standard)) {
        plan.cppStandard = stdCfg->canonical;
        plan.cppStandardFlag = stdCfg->flag;
    }
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
    if (tc.payloadPaths) {
        append_unique_path(plan.runtimeLibraryDirs, tc.payloadPaths->glibcLib);
    }

    // 1a. Detect basename collisions (both cross-package AND intra-package:
    //     ftxui ships dom/color.cpp + screen/color.cpp, for instance).
    //     For colliding files the object path gets a per-unit prefix
    //     derived from `<pkg>/<parent-dir>` so collisions are impossible.
    std::map<std::string, int> basenameCount;
    for (auto idx : topoOrder) {
        basenameCount[object_filename_for(graph.units[idx].path)]++;
    }
    auto sanitize = [](const std::string& s) {
        std::string out; out.reserve(s.size());
        for (char c : s) out += (c == '.' || c == '/' ? '_' : c);
        return out;
    };

    // 1. Compile units in topological order
    for (auto idx : topoOrder) {
        auto& u = graph.units[idx];
        CompileUnit cu;
        cu.source = u.path;
        cu.packageName = u.packageName;
        cu.localIncludeDirs = u.localIncludeDirs;
        cu.packageCflags = u.packageCflags;
        cu.packageCxxflags = u.packageCxxflags;
        const auto fname = object_filename_for(u.path);
        if (basenameCount[fname] > 1) {
            // Use <sanitized-pkg>/<parent-dir-name> as prefix to handle
            // both cross-package (multi-version mangling) and intra-package
            // (e.g. ftxui dom/color.cpp vs screen/color.cpp) collisions.
            auto parentDir = u.path.parent_path().filename().string();
            auto prefix = u.packageName.empty()
                ? parentDir
                : sanitize(u.packageName) + "_" + parentDir;
            cu.object = std::filesystem::path("obj") / prefix / fname;
        } else {
            cu.object = std::filesystem::path("obj") / fname;
        }
        if (u.provides) {
            cu.providesModule = u.provides->logicalName;
        }
        for (auto& req : u.requires_) cu.imports.push_back(req.logicalName);
        plan.compileUnits.push_back(std::move(cu));
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
            // Add main.cpp -> obj/main.o
            CompileUnit main_cu;
            main_cu.source = *lu.entryMain;
            main_cu.object = std::filesystem::path("obj") / object_filename_for(*lu.entryMain);
            main_cu.packageName = qualified_package_name(manifest);
            if (!packages.empty() && packages[0].usageResolved) {
                main_cu.localIncludeDirs = packages[0].privateBuild.includeDirs;
                main_cu.packageCflags = packages[0].privateBuild.cflags;
                main_cu.packageCxxflags = packages[0].privateBuild.cxxflags;
            } else {
                main_cu.localIncludeDirs = local_include_dirs_for_manifest(projectRoot, manifest);
                main_cu.packageCflags = manifest.buildConfig.cflags;
                main_cu.packageCxxflags = manifest.buildConfig.cxxflags;
            }

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

            // Avoid duplicate insert if main was already scanned
            bool already = false;
            for (auto& cu : plan.compileUnits) {
                if (cu.source == main_cu.source) { already = true; break; }
            }
            if (!already) {
                plan.compileUnits.push_back(main_cu);
            }
            lu.objects.push_back(main_cu.object);

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
