// windows_resources.cpp -- P13: the Windows resources of a plan's images
// (mcpp#365): author scripts, the synthesised version block and icon, and the
// application manifest of `windows_code_page`. Split from plan.cpp at a phase
// step boundary (2026.10.5.2). Declared in `:state`.

module mcpp.build.prepare;
import :state;
import mcpp.build.prepare_inputs;
import std;
import mcpp.diag;
import mcpp.build.stage;
import mcpp.build.refusal;
import mcpp.build.version_floor;
import mcpp.home;
import mcpp.platform.axis;
import mcpp.libs.json;
import mcpp.log;
import mcpp.manifest;
import mcpp.source_kind;
import mcpp.toolchain.clang;
import mcpp.toolchain.hostflags;   // the compile-token producer the package std module reuses
import mcpp.toolchain.cppfly;
import mcpp.toolchain.detect;
import mcpp.toolchain.dialect;
import mcpp.toolchain.model;      // is_msvc_target — the MSVC-ABI default (#718)
import mcpp.toolchain.fingerprint;
import mcpp.toolchain.registry;
import mcpp.toolchain.linkmodel;
import mcpp.toolchain.lifecycle;
import mcpp.toolchain.stdmod;
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
import mcpp.graph;
import mcpp.pack.abi_tag;      // the tag a prebuilt dependency is checked against
import mcpp.pack.prebuilt;     // …and the check itself
import mcpp.pack.stage_tree;   // where `${mcpp.stage_dir}` points, and its manifest
import mcpp.build.build_program;
import mcpp.build.resources;    // #365 Windows resources: synthesise / scan / find rc
import mcpp.build.backend;      // BuildOptions for the tool sub-build
import mcpp.build.ninja;        // make_ninja_backend — driving that sub-build
import mcpp.lockfile;
import mcpp.config;
import mcpp.xlings;
import mcpp.runtime.binding;
import mcpp.platform.runtime_search;
import mcpp.toolchain.post_install;
import mcpp.platform;
import mcpp.build.runner_lookup;
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
import mcpp.bmi_cache;

namespace mcpp::build {

// The resource compiler of the plan's resource units, and each unit's flags.
static std::expected<void, std::string>
step13_resource_compiler(PrepareState& state, BuildContext& ctx) {
    if (ctx.plan.resourceUnits.empty()) return {};
    namespace rsrc = mcpp::build::resources;
    const auto trip = mcpp::toolchain::triple::parse(state.tc->targetTriple)
                          .value_or(mcpp::toolchain::triple::host_triple());
    const auto  dialectId = mcpp::toolchain::dialect_for(*state.tc).id;
    const bool msvcStyle = (dialectId == "msvc");

    // Lazy + hard failure, exactly like nasm: a dropped resource
    // surfaces as "where did my icon go", which is unattributable.
    auto tool = rsrc::find_rc_tool(*state.tc, dialectId);
    if (!tool) {
        return std::unexpected(std::format(
            "[resources] needs a Windows resource compiler for the "
            "{} toolchain targeting {}, and none was found next to "
            "{}.\n  Expected {} in the toolchain's own bin directory "
            "(mcpp does not search PATH for build tools).",
            dialectId, trip.str(), state.tc->binaryPath.string(),
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
    //
    // Include search: the script's package first, then whatever the
    // toolchain puts on INCLUDE. llvm-rc preprocesses but does NOT read
    // INCLUDE (rc.exe does), so the SDK dirs have to be spelled out for it --
    // that is what makes `#include <windows.h>` work, and it is the
    // supported way to get VS_VERSION_INFO defined. Each unit carries the
    // flags of its package; the plan's flags are the first unit's, so a
    // plan with one package states them once.
    const std::string ip = msvcStyle ? "/I" : "-I";
    std::vector<std::string> systemIncludes;
    if (msvcStyle && tool->name().find("llvm-rc") != std::string::npos) {
        for (auto const& ev : state.tc->envOverrides) {
            if (ev.key != "INCLUDE") continue;
            // Shared splitter: `;` only. See rsrc::split_env_list --
            // the drive colon is not a separator.
            for (auto dir : rsrc::split_env_list(ev.value))
                systemIncludes.push_back(ip + std::string(dir));
        }
    }
    for (auto& ru : ctx.plan.resourceUnits) {
        ru.flags.push_back(msvcStyle ? "/C" : "--codepage=65001");
        if (msvcStyle) ru.flags.push_back("65001");
        for (auto const& d : ru.includeDirs) ru.flags.push_back(ip + d.string());
        ru.flags.insert(ru.flags.end(), systemIncludes.begin(), systemIncludes.end());
    }
    ctx.plan.rcFlags = ctx.plan.resourceUnits.front().flags;
    return {};
}

std::expected<void, std::string> step13_windows_resources(PrepareState& state, BuildContext& ctx) {
    // ─── Windows resources: [resources] → a tracked link input (mcpp#365) ──
    //
    // Four rules, in this order:
    //   1. Only the [resources] of the package being built is read: the root,
    //      or in a workspace plan each selected member, whose resources reach
    //      that member's images only. A dependency's version resource would
    //      fight its consumer's for ordinal 1, and a dependency that produces
    //      no PE image of its own has nothing to embed into.
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
    //
    // The same pipeline carries the application manifest of `windows_code_page`
    // (#693). A PE executable embeds one that makes its process ANSI code page
    // UTF-8 when its target says `windows_code_page = "utf-8"`, or, with nothing
    // said, when it is built as a host tool (D6): such a tool receives mcpp's
    // UTF-8 paths on its command line. `legacy` opts out, and an ordinary target
    // that says nothing embeds nothing (M6: the program's encoding is its own).
    //
    // The host-tool default yields to a manifest the package embeds itself
    // through `[resources] files`: both would sit at ordinal 1, the package
    // said nothing about code pages, and its own manifest is the one it ships.
    // A DECLARED `utf-8` beside such a manifest is refused below instead.
    //
    // A SUBJECT is one package whose resources are planned: its manifest, the
    // directory its paths were written in, and the images it owns. Outside a
    // workspace plan the root is the only subject and owns every image that
    // is not a dependency's program, which is the historical rule.
    struct Subject {
        const mcpp::manifest::Manifest* m;
        std::filesystem::path           dir;
        std::string                     owner;   // empty: the root
    };
    std::vector<Subject> subjects;
    if (!state.workspacePlan()) {
        subjects.push_back({&*state.m, *state.root, {}});
    } else {
        for (auto const& pkg : state.packages)
            if (pkg.selectedMember)
                subjects.push_back({&pkg.manifest, pkg.root,
                                    mcpp::build::qualified_package_name(pkg.manifest)});
    }
    const bool hostToolBuild = state.overrides.tool_depth > 0;
    const auto trip = mcpp::toolchain::triple::parse(state.tc->targetTriple)
                          .value_or(mcpp::toolchain::triple::host_triple());
    const auto  dialectId = mcpp::toolchain::dialect_for(*state.tc).id;
    const bool msvcStyle = (dialectId == "msvc");

    for (auto const& S : subjects) {
        const auto& M = *S.m;
        const bool ownManifest = hostToolBuild
            && std::ranges::any_of(M.resources.files, [&](const auto& f) {
                   const auto abs = (f.is_absolute() ? f : (S.dir / f)).lexically_normal();
                   return mcpp::build::resources::scan_rc(abs).declaresManifest;
               });
        auto codePageOf = [&](const mcpp::manifest::Target& t) -> std::string_view {
            if (!t.windowsCodePage.empty()) return t.windowsCodePage;
            return (hostToolBuild && t.is_program() && !ownManifest) ? "utf-8" : "legacy";
        };
        // A test program carries the code page `[test] windows_code_page`
        // gave it, and nothing by default: the host-tool default is a
        // program's.
        auto takesCodePage = [](const mcpp::manifest::Target& t) {
            return t.is_program() || t.kind == mcpp::manifest::Target::TestBinary;
        };
        const bool anyUtf8Image = std::ranges::any_of(M.targets, [&](const auto& t) {
            return takesCodePage(t) && codePageOf(t) == "utf-8";
        });
        // Author scripts reach programs and shared libraries, never a test
        // program, so only a program's manifest can collide with one.
        const bool anyUtf8Program = std::ranges::any_of(M.targets, [&](const auto& t) {
            return t.is_program() && codePageOf(t) == "utf-8";
        });
        if (M.resources.declared() || anyUtf8Image) {
            namespace rsrc = mcpp::build::resources;
            const auto& R = M.resources;

            // Rule 2 — target-independent, so it runs before the is_pe() gate.
            auto resolve_declared = [&](const std::filesystem::path& p,
                                        std::string_view key)
                -> std::expected<std::filesystem::path, std::string>
            {
                // Lexical, not weakly_canonical: canonicalising resolves symlinks,
                // and a symlinked source tree would then bake a different path into
                // the generated script than the one the user wrote. (Same reason
                // mcpp#344 made the cache anchor lexical.)
                auto abs = (p.is_absolute() ? p : (S.dir / p)).lexically_normal();
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

            // Rules 3 and 4 are early returns rather than nesting: the body below is
            // ~150 lines and an `else` around all of it reads as an accident.
            auto plan_resources = [&]() -> std::expected<void, std::string> {
                const std::string_view outExt = msvcStyle ? ".res" : ".o";
                // A member's resources are compiled in a directory of its own, so
                // two members' scripts and synthesised scripts never share a name.
                const auto resRel = S.owner.empty() ? std::filesystem::path("res")
                                                    : std::filesystem::path("res") / S.owner;
                const auto resDir = ctx.plan.outputDir / resRel;
                // Where the resource compiler looks for a script's includes and
                // files: the package's directory, then its include_dirs. The
                // scan resolves them the same way.
                std::vector<std::filesystem::path> rcIncludes{S.dir};
                for (auto const& d : M.buildConfig.includeDirs)
                    rcIncludes.push_back(d.is_absolute() ? d : (S.dir / d));
                std::error_code mkEc;
                std::filesystem::create_directories(resDir, mkEc);

                // Which link units embed resources: images, not archives. A `.res`
                // inside a static library is dropped by every linker that reads one.
                // Test binaries are images too, but deliberately excluded: an icon
                // and an OriginalFilename belong to what the project SHIPS, and a
                // test executable is not that. (`role = "object"` makes the opposite
                // call, for the opposite reason — see its note above.)
                // In a workspace plan a member's images are its link units and
                // its shared libraries, which are linked with the graph's.
                auto owns = [&](const mcpp::build::LinkUnit& lu) {
                    if (S.owner.empty()) return true;
                    if (lu.memberOf == S.owner) return true;
                    return lu.kind == mcpp::build::LinkUnit::SharedLibrary
                        && std::ranges::any_of(M.targets, [&](const auto& t) {
                               return t.kind == mcpp::manifest::Target::SharedLibrary
                                   && t.name == lu.targetName;
                           });
                };
                std::vector<std::size_t> peUnits;
                for (std::size_t i = 0; i < ctx.plan.linkUnits.size(); ++i) {
                    auto k = ctx.plan.linkUnits[i].kind;
                    // A dependency's program (mcpp#711) carries its own package's
                    // identity, not this one's.
                    if (!ctx.plan.linkUnits[i].artifactOf.empty()) continue;
                    if (!owns(ctx.plan.linkUnits[i])) continue;
                    if (k == mcpp::build::LinkUnit::Binary ||
                        k == mcpp::build::LinkUnit::SharedLibrary)
                        peUnits.push_back(i);
                }
                // Nothing to embed into. Compiling the scripts anyway would leave
                // orphan edges nothing depends on, and demanding a resource
                // compiler for them would fail a build that has no use for one.
                // A degradation, not a warning: the user asked for something and
                // got nothing, so `--strict` should see it.
                const bool utf8Tests = std::ranges::any_of(ctx.plan.linkUnits, [&](const auto& lu) {
                    return lu.kind == mcpp::build::LinkUnit::TestBinary && owns(lu)
                        && std::ranges::any_of(M.targets, [&](const auto& t) {
                               return t.name == lu.targetName && takesCodePage(t)
                                   && codePageOf(t) == "utf-8";
                           });
                });
                if (peUnits.empty()) {
                    if (R.declared())
                        mcpp::diag::degraded("resources/no-image", std::format(
                            "[resources] is declared but '{}' produces no executable or "
                            "shared library for {}", M.package.name, trip.str()),
                            "nothing embeds the icon or the version metadata",
                            "add a [targets.<name>] with kind = \"bin\" or \"shared\", "
                            "or drop the [resources] section");
                    // A test program's code page still has an image to go to.
                    if (!utf8Tests) return {};
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
                    ru.package = mcpp::build::qualified_package_name(M);
                    ru.source = src;
                    ru.output = resRel / (std::string(stem) + std::string(outExt));
                    ru.includeDirs = rcIncludes;
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
                for (auto const& rcSrc : peUnits.empty() ? decltype(scriptFiles){} : scriptFiles) {
                    auto scan = rsrc::scan_rc(rcSrc, rcIncludes);
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
                    if (scan.declaresManifest && anyUtf8Program)
                        return std::unexpected(std::format(
                            "[resources] {} embeds an application manifest, and "
                            "`windows_code_page = \"utf-8\"` embeds another at the same "
                            "ordinal (1).\n  Keep one: add `<activeCodePage "
                            "xmlns=\"http://schemas.microsoft.com/SMI/2019/WindowsSettings\">"
                            "UTF-8</activeCodePage>` to your manifest and set "
                            "`windows_code_page = \"legacy\"`, or drop your manifest.",
                            rcSrc.filename().generic_string()));
                    auto inputs = std::move(scan.inputs);
                    inputs.insert(inputs.end(), extraInputs.begin(), extraInputs.end());
                    if (auto a = add_unit(rcSrc, rcSrc.stem().string(),
                                          std::move(inputs),
                                          static_cast<std::size_t>(-1)); !a)
                        return std::unexpected(a.error());
                }

                // The synthesised script: per image, because OriginalFilename and
                // the version block belong to a specific artifact, and the
                // manifest to a specific executable.
                const bool synthVersion = R.declared() && R.synthesize_version_info();
                auto wantsUtf8 = [&](const mcpp::build::LinkUnit& lu) {
                    if (lu.kind != mcpp::build::LinkUnit::Binary
                        && lu.kind != mcpp::build::LinkUnit::TestBinary) return false;
                    if (!lu.artifactOf.empty()) return false;
                    for (auto const& t : M.targets)
                        if (t.name == lu.targetName)
                            return takesCodePage(t) && codePageOf(t) == "utf-8";
                    return false;
                };
                if (!iconAbs.empty() || synthVersion || anyUtf8Image) {
                    // A version key mcpp cannot order (an upstream build number)
                    // leaves FILEVERSION's four numeric fields at zero while the
                    // string fields keep the real text. Say so — the properties
                    // dialog will disagree with `[package].version` and nothing
                    // else would explain why.
                    if (synthVersion && !M.package.version.empty()
                        && !mcpp::version_req::parse_version(M.package.version)) {
                        mcpp::diag::degraded("resources/version",
                            std::format("[package].version = \"{}\" has no numeric "
                                        "form", M.package.version),
                            "the embedded FILEVERSION / PRODUCTVERSION fields are "
                            "0,0,0,0 (the string fields keep the real version)",
                            "set [resources.version-info] explicitly, or use a "
                            "dotted numeric version");
                    }
                    // The images that receive a synthesised script: every
                    // program and shared library, and a test program that asked
                    // for a code page, which receives the manifest and nothing
                    // else (see `owns` above for why).
                    std::vector<std::size_t> synthUnits = peUnits;
                    for (std::size_t i = 0; i < ctx.plan.linkUnits.size(); ++i) {
                        const auto& lu = ctx.plan.linkUnits[i];
                        if (lu.kind == mcpp::build::LinkUnit::TestBinary && owns(lu)
                            && wantsUtf8(lu))
                            synthUnits.push_back(i);
                    }
                    for (auto i : synthUnits) {
                        const auto& lu = ctx.plan.linkUnits[i];
                        const bool utf8 = wantsUtf8(lu);
                        const bool testImage = lu.kind == mcpp::build::LinkUnit::TestBinary;
                        if (!testImage && iconAbs.empty() && !synthVersion && !utf8) continue;
                        // A test's name is a path (`unit/test_span`) and may
                        // equal a program's, so its files live under `tests/`.
                        const std::string imageName =
                            testImage ? "tests/" + lu.targetName : lu.targetName;
                        std::filesystem::path manifestAbs;
                        if (utf8) {
                            manifestAbs = resDir / (imageName + ".mcpp.manifest");
                            std::filesystem::create_directories(manifestAbs.parent_path(), mkEc);
                            const auto manifestText = rsrc::utf8_code_page_manifest();
                            std::string had;
                            if (std::ifstream in(manifestAbs, std::ios::binary); in)
                                had.assign(std::istreambuf_iterator<char>(in), {});
                            if (had != manifestText) {
                                std::ofstream os(manifestAbs, std::ios::binary);
                                if (!os) return std::unexpected(std::format(
                                    "cannot write the application manifest '{}'",
                                    manifestAbs.string()));
                                os << manifestText;
                            }
                        }
                        // A script synthesised for the manifest alone carries
                        // nothing else: a package that declares no [resources]
                        // asked for no version resource.
                        mcpp::manifest::Resources forScript = R;
                        if (!synthVersion || testImage) forScript.versionInfo = false;
                        auto text = rsrc::synthesize_rc(
                            M.package, forScript, lu.output.filename().string(),
                            testImage ? std::filesystem::path{} : iconAbs, manifestAbs);
                        if (!text) return std::unexpected(text.error());
                        // A stable path, so `cp` + `files = [...]` reproduces the
                        // same resource byte for byte (the L0→L1 escape hatch).
                        auto rcPath = resDir / (imageName + ".mcpp.rc");
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
                        if (!iconAbs.empty() && !testImage) inputs.push_back(iconAbs);
                        if (!manifestAbs.empty()) inputs.push_back(manifestAbs);
                        inputs.insert(inputs.end(), extraInputs.begin(), extraInputs.end());
                        if (auto a = add_unit(rcPath, imageName + ".mcpp",
                                              std::move(inputs), i); !a)
                            return std::unexpected(a.error());
                    }
                }

                return {};
            };

            if (trip.is_pe())
                if (auto r = plan_resources(); !r) return std::unexpected(r.error());
        }
    }

    return step13_resource_compiler(state, ctx);
}

// The member path (relative to the workspace root) of a package root, when the
// root is a member of the workspace this build runs in; empty otherwise.
// Read by W3 (a member's non-public modules).
std::string workspace_member_of(const PrepareState& state, const std::filesystem::path& root) {
    if (!state.wsManifest || state.runtimeWorkspaceRoot.empty()) return {};
    const auto rel = root.lexically_normal()
                         .lexically_relative(state.runtimeWorkspaceRoot.lexically_normal())
                         .generic_string();
    if (rel.empty() || rel == "." || rel.starts_with("..")) return {};
    for (auto const& m : state.wsManifest->workspace.members) {
        if (m == rel) return rel;
        if (m.ends_with("/*") && rel.starts_with(m.substr(0, m.size() - 1))
            && rel.find('/', m.size() - 1) == std::string::npos)
            return rel;
    }
    return {};
}

} // namespace mcpp::build
