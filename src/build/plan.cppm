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
};

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

} // namespace

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
        }

        // Also include implementation .cpp/.cc/.cxx/.c units, but EXCLUDE any
        // file registered as another target's entryMain (each binary's main()
        // is exclusive to that binary).
        for (auto& cu : plan.compileUnits) {
            if (sharedDepPackages.contains(cu.packageName)) continue;
            if (!is_implementation_source(cu.source)) continue;
            if (lu.entryMain && cu.source == *lu.entryMain) continue;     // own entry: already added above
            if (entryFilesAcrossTargets.contains(cu.source)) continue;     // foreign entry: skip
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
