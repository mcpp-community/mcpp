// mcpp.build.plan — backend-agnostic representation of "what to build".
//
// The pipeline is:
//   manifest + modgraph + toolchain + fingerprint  →  BuildPlan  →  Backend.build()

export module mcpp.build.plan;

import std;
import mcpp.manifest;
import mcpp.modgraph.graph;
import mcpp.toolchain.detect;
import mcpp.toolchain.fingerprint;

export namespace mcpp::build {

struct CompileUnit {
    std::filesystem::path           source;
    std::filesystem::path           object;            // relative to plan.outputDir
    std::optional<std::string>      providesModule;   // logical name, if .cppm export
    std::vector<std::string>        imports;           // logical names imported
};

struct LinkUnit {
    std::string                     targetName;
    enum Kind { Binary, StaticLibrary, SharedLibrary, TestBinary } kind = Binary;
    std::vector<std::filesystem::path> objects;        // relative to plan.outputDir
    std::filesystem::path           output;            // relative to plan.outputDir
    std::optional<std::filesystem::path> entryMain;   // src path of main.cpp for bin
};

struct BuildPlan {
    mcpp::manifest::Manifest        manifest;
    mcpp::toolchain::Toolchain      toolchain;
    mcpp::toolchain::Fingerprint    fingerprint;

    std::filesystem::path           projectRoot;      // where mcpp.toml lives
    std::filesystem::path           outputDir;        // target/<triple>/<fp>/
    std::filesystem::path           stdBmiPath;      // absolute path to prebuilt std.gcm
    std::filesystem::path           stdObjectPath;   // absolute path to prebuilt std.o

    std::vector<CompileUnit>        compileUnits;     // topologically sorted
    std::vector<LinkUnit>           linkUnits;
};

// Build a BuildPlan from already-validated inputs.
BuildPlan make_plan(const mcpp::manifest::Manifest&         manifest,
                    const mcpp::toolchain::Toolchain&       tc,
                    const mcpp::toolchain::Fingerprint&     fp,
                    const mcpp::modgraph::Graph&            graph,
                    const std::vector<std::size_t>&         topoOrder,
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

} // namespace

BuildPlan make_plan(const mcpp::manifest::Manifest&         manifest,
                    const mcpp::toolchain::Toolchain&       tc,
                    const mcpp::toolchain::Fingerprint&     fp,
                    const mcpp::modgraph::Graph&            graph,
                    const std::vector<std::size_t>&         topoOrder,
                    const std::filesystem::path&            projectRoot,
                    const std::filesystem::path&            outputDir,
                    const std::filesystem::path&            stdBmiPath,
                    const std::filesystem::path&            stdObjectPath)
{
    BuildPlan plan;
    plan.manifest         = manifest;
    plan.toolchain        = tc;
    plan.fingerprint      = fp;
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
            lu.output = std::filesystem::path("bin") / std::format("lib{}.a", t.name);
        } else if (t.kind == mcpp::manifest::Target::SharedLibrary) {
            lu.kind   = LinkUnit::SharedLibrary;
            lu.output = std::filesystem::path("bin") / std::format("lib{}.so", t.name);
        } else if (t.kind == mcpp::manifest::Target::TestBinary) {
            lu.kind   = LinkUnit::TestBinary;
            lu.output = std::filesystem::path("bin") / t.name;
            if (!t.main.empty()) lu.entryMain = projectRoot / t.main;
        } else {
            lu.kind   = LinkUnit::Binary;
            lu.output = std::filesystem::path("bin") / t.name;
            if (!t.main.empty()) lu.entryMain = projectRoot / t.main;
        }

        // Include all module units' objects (they may be needed at runtime via global init).
        // For binary target, also include main.cpp's object if main is present.
        for (auto& cu : plan.compileUnits) {
            if (cu.source.extension() == ".cppm") {
                lu.objects.push_back(cu.object);
            }
        }

        if ((lu.kind == LinkUnit::Binary || lu.kind == LinkUnit::TestBinary) && lu.entryMain) {
            // Add main.cpp -> obj/main.o
            CompileUnit main_cu;
            main_cu.source = *lu.entryMain;
            main_cu.object = std::filesystem::path("obj") / object_filename_for(*lu.entryMain);

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
            auto ext = cu.source.extension();
            if (ext != ".cpp" && ext != ".cc" && ext != ".cxx" && ext != ".c") continue;
            if (lu.entryMain && cu.source == *lu.entryMain) continue;     // own entry: already added above
            if (entryFilesAcrossTargets.contains(cu.source)) continue;     // foreign entry: skip
            lu.objects.push_back(cu.object);
        }

        plan.linkUnits.push_back(std::move(lu));
    }

    return plan;
}

} // namespace mcpp::build
