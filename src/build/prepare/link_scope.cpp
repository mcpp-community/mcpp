// link_scope.cpp -- P13: what an image links with beyond its objects, decided
// once the plan exists (2026.10.5.2): the statement that the root's private
// link flags no longer reach a dependency's shared library, and where LTO
// cannot reach. Declared in `:state`.

module mcpp.build.prepare;
import :state;

import std;
import mcpp.diag;
import mcpp.build.refusal;
import mcpp.build.plan;
import mcpp.toolchain.model;

namespace mcpp::build {

// A STATEMENT FOR THE RELEASE THAT CHANGED THE RULE (2026.10.5.2, D7b).
//
// Until 2026.10.5.2 a dependency's shared library linked with the root's
// `[build] ldflags`, and a project could rely on that without knowing it: a
// search path the root names for a library the dependency links. Such a link
// now fails, and the linker names the library rather than the cause. The
// words that can be missed this way, search paths and libraries, are named
// once beside the shared libraries they no longer reach. Other words reaching
// a dependency's image were defects (a version script, an entry point) and
// are not mentioned. To be removed in the release after next.
void note_root_link_words_withheld(const PrepareState& state, const BuildContext& ctx) {
    if (state.workspacePlan() || state.packages.empty()) return;
    std::vector<std::string> shared;
    for (auto const& lu : ctx.plan.linkUnits)
        if (lu.dependencyOwned && lu.kind == mcpp::build::LinkUnit::SharedLibrary)
            shared.push_back(lu.targetName);
    if (shared.empty()) return;
    const auto& graph = state.m->buildConfig.graphLdflags;
    auto linkInput = [](std::string_view w) {
        return w.starts_with("-L") || w.starts_with("-l") || w.starts_with("-Wl,-L")
            || w.starts_with("/LIBPATH:") || w.starts_with("-LIBPATH:")
            || w.ends_with(".lib") || w.ends_with(".a") || w.ends_with(".so");
    };
    std::string words;
    for (auto const& w : state.packages[0].linkUsage.ldflags) {
        if (std::ranges::find(graph, w) != graph.end() || !linkInput(w)) continue;
        words += (words.empty() ? "`" : ", `") + w + "`";
    }
    if (words.empty()) return;
    std::string libs;
    for (auto const& n : shared) libs += (libs.empty() ? "" : ", ") + n;
    mcpp::diag::note("link/root-flags", std::format(
        "the root package's [build] ldflags {} no longer reach the shared "
        "libraries of its dependencies ({}) since 2026.10.5.2; a dependency "
        "that needs a search path or a library states it in its own [build] "
        "ldflags or build.mcpp", words, libs));
}

// WHERE LTO CANNOT REACH (2026.10.5.2, D5 and D6 of the 2026-10-05 design).
//
// Two kinds of image cannot hold LTO intermediate code, and in both the
// objects that feed them are compiled without it, which is said once:
//
//   * a PE shared library whose exports cl.exe's `/GL` would hide. Its
//     exports are discovered from the symbol tables of its objects, and a
//     `/GL` object has none. The packages whose objects it links (its own and
//     the statics placed in it) are compiled with `/GL-`, and the DLL is still
//     linked with `/LTCG`. A target that STATES `windows_auto_export = true`
//     is refused instead, as is `/GL` written into those packages' flags:
//     both are the author's words, and one of them has to change.
//   * a static library `mcpp pack` ships. Intermediate code is readable only
//     by the compiler that wrote it, so a prebuilt archive carries objects.
//
// A source is compiled once in a build, so an object that also reaches a
// program is compiled the same way there. The summary says `+ lto (partial)`.
std::expected<void, std::string> scope_lto(PrepareState& state, BuildContext& ctx) {
    auto& plan = ctx.plan;
    auto& bc   = plan.manifest.buildConfig;
    const bool cl = plan.toolchain.compiler == mcpp::toolchain::CompilerId::MSVC;
    std::map<std::filesystem::path, std::size_t> unitOf;
    for (std::size_t i = 0; i < plan.compileUnits.size(); ++i)
        unitOf.emplace(plan.compileUnits[i].object, i);
    auto packages_of = [&](const mcpp::build::LinkUnit& lu) {
        std::set<std::string> out;
        for (auto const& o : lu.objects)
            if (auto it = unitOf.find(o); it != unitOf.end())
                out.insert(plan.compileUnits[it->second].packageName);
        return out;
    };
    auto writes_gl = [](const std::vector<std::string>& flags) {
        return std::ranges::any_of(flags, [](std::string_view w) {
            return w == "/GL" || w == "-GL";
        });
    };
    auto discovers = [](const mcpp::build::LinkUnit& lu) {
        return lu.kind == mcpp::build::LinkUnit::SharedLibrary && !lu.defFile.empty();
    };

    if (cl) {
        for (auto const& lu : plan.linkUnits) {
            if (!discovers(lu)) continue;
            const auto pkgs = packages_of(lu);
            for (auto const& cu : plan.compileUnits) {
                if (!pkgs.contains(cu.packageName)) continue;
                if (!writes_gl(cu.packageCxxflags) && !writes_gl(cu.packageCflags)) continue;
                refusal::record(refusal::Code::LtoExportDiscovery);
                return std::unexpected(std::format(
                    "package '{}' compiles with /GL, and its objects are linked into the "
                    "shared library '{}', whose exports are discovered from their symbol "
                    "tables; a /GL object has none.\n"
                    "       Set `windows_auto_export = false` on '{}' and mark its exports "
                    "with __declspec(dllexport), or remove /GL (`lto = true` in a profile "
                    "applies it where it can).",
                    cu.packageName, lu.targetName, lu.targetName));
            }
        }
    }
    if (!bc.lto) return {};

    std::set<std::string> withoutLto;
    std::vector<std::string> dlls, archives;
    if (cl) {
        for (auto const& lu : plan.linkUnits) {
            if (!discovers(lu)) continue;
            if (lu.autoExportStated) {
                refusal::record(refusal::Code::LtoExportDiscovery);
                return std::unexpected(std::format(
                    "the shared library '{}' states `windows_auto_export = true`, and "
                    "`lto = true` compiles its objects with /GL on cl.exe, which leaves "
                    "no symbol table to discover its exports from.\n"
                    "       Set `windows_auto_export = false` and mark the exports with "
                    "__declspec(dllexport), or omit the key to have this library's "
                    "objects compiled without LTO.", lu.targetName));
            }
            dlls.push_back(lu.targetName);
            for (auto const& p : packages_of(lu)) withoutLto.insert(p);
        }
    }
    if (state.overrides.no_lto_in_archives) {
        for (auto const& lu : plan.linkUnits) {
            if (lu.kind != mcpp::build::LinkUnit::StaticLibrary) continue;
            archives.push_back(lu.targetName);
            for (auto const& p : packages_of(lu)) withoutLto.insert(p);
        }
    }
    if (withoutLto.empty()) return {};

    const std::string off = cl ? "/GL-" : "-fno-lto";
    for (auto& cu : plan.compileUnits) {
        if (!withoutLto.contains(cu.packageName)) continue;
        cu.packageCxxflags.push_back(off);
        cu.packageCflags.push_back(off);
    }
    bc.ltoPartial = true;
    auto join = [](const auto& v) {
        std::string s;
        for (auto const& x : v) s += (s.empty() ? "" : ", ") + std::string(x);
        return s;
    };
    if (!dlls.empty())
        mcpp::diag::degraded("build/lto", std::format(
            "LTO is not applied to the packages linked into the shared libraries {} "
            "({}), whose exports are discovered from symbol tables that /GL objects "
            "do not have", join(dlls), join(withoutLto)),
            "those packages' code is optimized per translation unit; the rest of the "
            "build is link-time optimized",
            "set `windows_auto_export = false` on each library and mark its exports "
            "with __declspec(dllexport) to apply LTO to it as well");
    if (!archives.empty())
        mcpp::diag::degraded("pack/lto", std::format(
            "LTO is not applied to the objects of the packed static libraries {}",
            join(archives)),
            "a shipped archive holds machine code, because LTO intermediate code can "
            "be read only by the compiler version that wrote it");
    return {};
}

} // namespace mcpp::build
