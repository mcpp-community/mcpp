// toolchain_env.cpp -- target rows, sysroot overrides, the MSVC toolset
// binding and the environment a build program is given, plus the toolchain a
// host tool chose for itself (#710). Declared in `:state`, or exported from
// prepare.cppm.

module mcpp.build.prepare;
import :state;

import mcpp.build.prepare_inputs;
import mcpp.diag;

import std;
import mcpp.build.version_floor;
import mcpp.platform.axis;
import mcpp.manifest;
import mcpp.source_kind;
import mcpp.toolchain.hostflags;   // the compile-token producer the package std module reuses
import mcpp.toolchain.detect;
import mcpp.toolchain.dialect;
import mcpp.toolchain.fingerprint;
import mcpp.toolchain.msvc;
import mcpp.toolchain.registry;
import mcpp.toolchain.linkmodel;
import mcpp.toolchain.gcc;
// For `resolve_version_match` / `list_installed_versions`: a bare compiler
// family named by the dependency graph resolves to a concrete version through
// exactly the path `mcpp toolchain default <family>` uses.
import mcpp.toolchain.lifecycle;
import mcpp.toolchain.stdmod;
import mcpp.freestanding.target;   // the target sysroot layout (libdir)
import mcpp.freestanding.linkline; // the ISA profile, for the std module command
import mcpp.toolchain.post_install;
import mcpp.toolchain.abi;
import mcpp.toolchain.triple;
import mcpp.build.plan;
import mcpp.build.flags;          // compute_flags — the per-role contracts (#418)
import mcpp.build.graph_shape;  // #407: the graph says which mode wrote it
import mcpp.build.build_program;
import mcpp.build.backend;      // BuildOptions for the tool sub-build
import mcpp.build.ninja;        // make_ninja_backend — driving that sub-build
import mcpp.config;
import mcpp.xlings;
import mcpp.build.resources;   // the resource compiler the row uses (#734 E2)
import mcpp.toolchain.post_install;
import mcpp.platform;
import mcpp.platform.macos;
import mcpp.fetcher;
import mcpp.fetcher.progress;
import mcpp.ui;
import mcpp.project;

namespace mcpp::build {

// `prepare_build` builds the BuildContext for any verb that compiles.
//   includeDevDeps: when true, dev-dependencies are also fetched + scanned
//                   into the modgraph. mcpp test passes true; build/run pass false.
//   extraTargets:   additional Target entries (e.g. synthetic test targets)
//                   appended to the manifest before the modgraph runs.
//   overrides:      --target / --static.
// A dependency that "cannot be found" while an index is unreadable is almost
// never missing — it is unreachable, and the two need different actions from
// the user (publish it vs upgrade mcpp). The floor error is printed when the
// index is first opened, which can be hundreds of lines earlier; the message
// that STOPS the build has to carry the cause, because that is the one a user
// reads. See mcpp::pm::unusable_index_hint.
// Spelling-independent `[target.<triple>]` lookup.
//
// A section keyed `x86_64-w64-mingw32` matches a resolved `x86_64-windows-gnu`,
// and unparseable keys compare exactly (the escape hatch for custom triples).
// Factored out of the toolchain-override path because the sysroot override must
// use the SAME matching: two lookups that disagreed about spelling would give a
// section that applies to `toolchain` and not to `sysroot`, which is a defect
// nobody would think to look for.
const mcpp::manifest::TargetEntry*
find_target_entry(const mcpp::manifest::Manifest& m,
                  const mcpp::toolchain::triple::Triple& t) {
    if (auto it = m.targetOverrides.find(t.str()); it != m.targetOverrides.end())
        return &it->second;
    for (auto const& [key, entry] : m.targetOverrides) {
        if (auto k = mcpp::toolchain::triple::parse(key); k && k->str() == t.str())
            return &entry;
    }
    return nullptr;
}

// The project's `[target.<triple>].sysroot`, or nullptr when it declared none.
const std::string*
sysroot_override(const mcpp::manifest::Manifest& m,
                 const mcpp::toolchain::triple::Triple& t) {
    auto* e = find_target_entry(m, t);
    return (e && e->sysrootDeclared) ? &e->sysroot : nullptr;
}

// THE MSVC TOOLSET A CLANG `*-windows-msvc` BUILD COMPILES AGAINST.
//
// On an MSVC-ABI row the compiler is the toolchain and the MSVC toolset -- its
// STL and CRT, and the Windows SDK that follows it -- is the sysroot, named by
// `[target.<triple>].sysroot` (default `msvc@system`). Until this existed the
// clang driver searched the machine for headers and libraries while mcpp
// searched it again for `std.ixx`, by a different order, so a machine with two
// installations could compile one toolset's `std.ixx` against another's
// headers, and the choice reached neither the cache key nor any report. The
// choice is made here once, recorded on the toolchain, and handed to the
// driver by the link model.
//
// `msvc@system` that finds nothing returns without binding, so the build
// reaches the "targeting the MSVC ABI without a usable MSVC" diagnosis that
// already names the alternatives.
std::expected<void, std::string>
bind_msvc_sysroot(mcpp::toolchain::Toolchain& tc,
                  const mcpp::manifest::Manifest& m,
                  const std::function<std::expected<mcpp::config::GlobalConfig*,
                                                    std::string>()>& cfgOf) {
    namespace msvc = mcpp::toolchain::msvc;
    auto tt = mcpp::toolchain::triple::parse(tc.targetTriple);
    if (!tt) return {};
    const std::string* declared = sysroot_override(m, *tt);
    const std::string text = declared ? *declared : std::string("msvc@system");
    auto spec = mcpp::toolchain::parse_toolchain_spec(text);
    if (!spec)
        return std::unexpected(std::format(
            "[target.{}].sysroot = '{}': {}", tt->str(), text, spec.error()));
    if (spec->family != mcpp::toolchain::Family::Msvc)
        return std::unexpected(std::format(
            "[target.{}].sysroot = '{}': on an MSVC-ABI row the sysroot is an "
            "MSVC toolset (msvc@system, msvc@<toolset> or xim:msvc@<toolset>)",
            tt->str(), text));

    msvc::ToolsetNeeds needs;
    needs.cl = false;   // clang compiles against the toolset; it does not run cl.exe
    const bool systemSel = spec->version.empty() || spec->version == "system";
    std::vector<msvc::VsInstance> instances;
    std::optional<msvc::ToolsetChoice> choice;
    if (!spec->ecosystemOnly) {
        instances = msvc::enumerate_vs_instances();
        choice = msvc::select_system_toolset(
            instances, msvc::msvc_env_snapshot(),
            systemSel ? std::string_view("system") : std::string_view(spec->version),
            needs);
        if (!choice && systemSel) {
            // Not a refusal: clang's own detection may still find a toolset
            // (a developer prompt's INCLUDE and LIB). Without one the first
            // standard header fails -- `'cstdio' file not found` while the
            // `mcpp` module precompiles -- which names neither the cause nor
            // the remedy, so the resolution states both here (#734, measured
            // on a runner whose Visual Studio was masked).
            mcpp::diag::warning("toolchain/msvc",
                std::format("clang on the MSVC ABI compiles against an MSVC toolset, and "
                            "`msvc@system` found none on this machine (no Visual Studio "
                            "instance with the C++ tools)"),
                std::format("install one and name it: `mcpp toolchain install msvc "
                            "14.44.35207`, then `[target.{}] sysroot = \"xim:msvc@14.44.35207\"` "
                            "or `--toolchain xim:msvc@14.44.35207`",
                            tt->str()));
            return {};
        }
    }

    std::string origin = "system";
    if (!choice) {
        // THE PACKAGE: `xim:` asked for it, or no installed toolset matched.
        auto cfg = cfgOf();
        if (!cfg) return std::unexpected(cfg.error());
        mcpp::toolchain::ToolchainSpec pkgSpec = *spec;
        pkgSpec.target = {};
        auto pkg = mcpp::toolchain::to_xim_package(pkgSpec);
        mcpp::fetcher::Fetcher fetcher(**cfg);
        mcpp::fetcher::InstallProgressHandler progress;
        auto payload = fetcher.resolve_xpkg_path(pkg.target(), /*autoInstall=*/true,
                                                 &progress);
        if (!payload) {
            // `xim:` never looked at the machine, so the refusal does not
            // report on it.
            if (spec->ecosystemOnly)
                return std::unexpected(std::format(
                    "[target.{}].sysroot = '{}': the package could not be "
                    "provided: {}\n"
                    "  packages: `mcpp toolchain list --available msvc`",
                    tt->str(), text, payload.error().message));
            std::string onMachine;
            for (auto const& line : msvc::describe_system_toolsets(instances, needs))
                onMachine += "\n    " + line;
            return std::unexpected(std::format(
                "[target.{}].sysroot = '{}' matches no toolset on this machine, "
                "and the package could not be provided: {}\n"
                "  installed on this machine:{}\n"
                "  packages: `mcpp toolchain list --available msvc`",
                tt->str(), text, payload.error().message,
                onMachine.empty() ? std::string(" none") : onMachine));
        }
        auto inst = mcpp::toolchain::resolve_managed_msvc(
            mcpp::config::make_xlings_env(**cfg), pkg, /*identifyVersion=*/false);
        if (!inst) return std::unexpected(inst.error());
        choice.emplace();
        choice->vsRoot   = inst->vsRoot;
        choice->version  = inst->toolsVersion;
        choice->toolsDir = inst->vsRoot / "VC" / "Tools" / "MSVC" / inst->toolsVersion;
        choice->product  = "xim:msvc@" + inst->toolsVersion;
        choice->via      = "package";
        origin = "managed";
    }
    for (auto const& n : choice->notes) mcpp::ui::info("note", n);

    // THE SDK FOLLOWS THE TOOLSET'S ORIGIN: a package binds the windows-sdk
    // installed with it, a machine's toolset takes the machine's SDK by the
    // search `msvc@system` has always used. The same function the cl.exe row
    // uses, asked about the toolset directory rather than a cl.exe.
    auto sdk = msvc::resolve_sdk_for(choice->toolsDir / "bin");
    if (!sdk.note.empty()) mcpp::ui::info("note", sdk.note);

    tc.msvcToolsDir     = choice->toolsDir;
    tc.msvcToolsVersion = choice->version;
    // The version of this toolset's cl.exe, read from the file (this row runs
    // no cl.exe) and said to the driver by the link model, so it is on every
    // command and in every key rather than chosen by the driver (mcpp#746).
    tc.msvcCompilerVersion = msvc::compiler_version_in_tools_dir(choice->toolsDir, tt->arch);
    tc.msvcOrigin       = origin;
    tc.msvcProduct      = choice->product;
    // The toolset's own redistributable CRT (#718), reached from the
    // sysroot rather than from a cl.exe path — this row runs no cl.exe.
    tc.msvcRedistDir    = msvc::vc_redist_dir_for_tools_dir(
        choice->toolsDir, tt->arch);
    if (sdk.sdk) {
        tc.windowsSdkRoot    = sdk.sdk->root;
        tc.windowsSdkVersion = sdk.sdk->version;
    }
    // The STL is this toolset's, so its version is the standard library's.
    tc.stdlibVersion = choice->version;

    // THE STD MODULE OF THIS TOOLSET, replacing the `std.ixx` detection found
    // by its own search. A toolset without one leaves `import std` unavailable
    // rather than borrowing another toolset's.
    //
    // Bind both standard modules to this toolset. A compat module from another
    // STL cannot safely import the selected std BMI.
    std::error_code ec;
    const auto ixx = choice->toolsDir / "modules" / "std.ixx";
    const bool msvcStl = tc.stdModuleSource.empty()
                      || tc.stdModuleSource.filename() == "std.ixx";
    if (msvcStl && std::filesystem::exists(ixx, ec)) {
        const auto compat = ixx.parent_path() / "std.compat.ixx";
        tc.set_std_modules(ixx, std::filesystem::exists(compat, ec)
                                    ? compat : std::filesystem::path{});
        tc.importStdMinLevel = msvc::std_module_min_level_for_stl(ixx);
    } else if (msvcStl && !tc.stdModuleSource.empty()) {
        tc.clear_std_modules();
    }

    mcpp::ui::info("Resolved", std::format(
        "sysroot {} → MSVC {} ({}: {}){}", spec->spec_str(), choice->version,
        origin, choice->product,
        tc.windowsSdkVersion.empty()
            ? std::string{}
            : std::format(" · Windows SDK {}", tc.windowsSdkVersion)));
    return {};
}

// ON THE CL.EXE ROW THE COMPILER IS ITS OWN SYSROOT: cl.exe cannot compile
// against another toolset's STL. A declared sysroot is therefore a second
// statement of the compiler's toolset, and one that names a different
// toolset is refused rather than silently ignored.
std::expected<void, std::string>
check_cl_row_sysroot(const mcpp::toolchain::Toolchain& tc,
                     const mcpp::manifest::Manifest& m) {
    auto tt = mcpp::toolchain::triple::parse(tc.targetTriple);
    if (!tt) return {};
    const std::string* declared = sysroot_override(m, *tt);
    if (!declared) return {};
    auto spec = mcpp::toolchain::parse_toolchain_spec(*declared);
    if (!spec || spec->version.empty() || spec->version == "system") return {};
    // <tools>/bin/Host<h>/<t>/cl.exe → <tools> is named by the toolset.
    const auto toolset = tc.binaryPath.parent_path().parent_path()
                             .parent_path().parent_path().filename().string();
    if (mcpp::toolchain::msvc::toolset_version_matches(spec->version, toolset))
        return {};
    return std::unexpected(std::format(
        "[target.{}].sysroot = '{}' names a different toolset than the "
        "compiler ({}, toolset {}). With cl.exe the compiler is its own "
        "sysroot: pin the toolset in the toolchain (`msvc@<toolset>`) and "
        "drop `sysroot`, or build with clang to compile against another "
        "toolset.",
        tt->str(), *declared, tc.binaryPath.string(), toolset));
}

// `[package]`, for the build program of the package that declares it.
//
// ONE CALL RATHER THAN A FIELD PER SITE. Two places build a
// `BuildProgramEnv` -- the dependency loop and the root -- and the values a
// build program is told about its own package are the same question in both.
// Setting them field by field at each site is how the two answers drift: the
// root gained `packageName` and the dependency loop gained it separately, and
// a value added to only one of them is a rule package that works for a root
// project and not for a dependency, with nothing failing to say so.
void fill_package_build_env(mcpp::build::BuildProgramEnv& e,
                            const mcpp::manifest::Manifest& m) {
    e.packageName        = m.package.name;
    e.packageNamespace   = m.package.namespace_;
    e.packageVersion     = m.package.version;
    e.packageDescription = m.package.description;
    e.packageLicense     = m.package.license;
    e.packageRepo        = m.package.repo;
    // ';' rather than ',': an author entry is conventionally `Name <mail@host>`
    // and a name may carry a comma, so a comma-joined list cannot be split back
    // into the entries it was made from.
    e.packageAuthors.clear();
    for (auto const& a : m.package.authors) {
        if (!e.packageAuthors.empty()) e.packageAuthors += ';';
        e.packageAuthors += a;
    }
}

namespace {

// #734 E2: the MSVC architecture directory name of a target triple.
std::string msvc_arch_of(std::string_view triple) {
    const auto tt = mcpp::toolchain::triple::parse(triple);
    const auto arch = tt ? tt->msvc_arch() : std::string_view{};
    return arch.empty() ? "x64" : std::string(arch);
}

// The `bin/Host<h>/<arch>` directory of an MSVC toolset that holds cl.exe,
// preferring the host's own architecture as the engine's toolset search does.
std::filesystem::path msvc_bin_dir(const std::filesystem::path& tools, std::string_view arch) {
    std::error_code ec;
    const bool armHost = mcpp::platform::host_arch == std::string_view("aarch64")
                      || mcpp::platform::host_arch == std::string_view("arm64");
    const std::array<std::string_view, 3> hosts = armHost
        ? std::array<std::string_view, 3>{"Hostarm64", "Hostx64", "Hostx86"}
        : std::array<std::string_view, 3>{"Hostx64", "Hostarm64", "Hostx86"};
    for (auto h : hosts) {
        auto dir = tools / "bin" / std::string(h) / std::string(arch);
        if (std::filesystem::exists(dir / "cl.exe", ec)) return dir;
    }
    return {};
}

// #734 E2: the build information of the resolved toolchain. Each value comes
// from the producer the engine's own command lines read: the C compiler from
// `derive_c_compiler`, the archiver from `archive_tool`, the resource compiler
// from `find_rc_tool`, the MSVC environment from `build_env_for_cl`, the ninja
// from `ninja_program_for`, the runtime contract from `program_cxx_runtime`.
std::map<std::string, std::string>
build_information(const mcpp::manifest::Manifest& m, const mcpp::toolchain::Toolchain& tc) {
    std::map<std::string, std::string> info;
    auto put = [&](std::string_view k, const std::filesystem::path& v) {
        if (!v.empty()) info[std::string(k)] = v.string();
    };
    const bool msvcCompiler = tc.compiler == mcpp::toolchain::CompilerId::MSVC;
    const bool msvcAbi      = mcpp::toolchain::is_msvc_target(tc);
    const auto& dial        = mcpp::toolchain::dialect_for(tc);

    // The row's tools.
    const auto cxx = tc.binaryPath;
    const auto cc  = mcpp::toolchain::derive_c_compiler(tc);
    put("MCPP_TOOL_CXX", cxx);
    put("MCPP_TOOL_CC",  cc.empty() ? cxx : cc);
    put("MCPP_TOOL_AR",  mcpp::toolchain::archive_tool(tc));
    if (auto rc = mcpp::build::resources::find_rc_tool(tc, dial.id)) put("MCPP_TOOL_RC", rc->path);

    // The MSVC ABI's native tools, from the resolved toolset and its SDK.
    std::filesystem::path clBin;
    if (msvcAbi) {
        const auto arch  = msvc_arch_of(tc.targetTriple);
        clBin = msvcCompiler ? tc.binaryPath.parent_path()
                             : (tc.msvcToolsDir.empty() ? std::filesystem::path{}
                                                        : msvc_bin_dir(tc.msvcToolsDir, arch));
        if (!clBin.empty()) {
            const auto cl = clBin / "cl.exe";
            put("MCPP_ABI_TOOL_CXX", cl);
            put("MCPP_ABI_TOOL_CC",  cl);
            put("MCPP_ABI_TOOL_LD",  clBin / "link.exe");
            put("MCPP_ABI_TOOL_AR",  clBin / "lib.exe");
            put("MCPP_ABI_TOOL_AS",  clBin / (arch == "arm64" ? "armasm64.exe"
                                            : arch == "x86"   ? "ml.exe" : "ml64.exe"));
            auto sdk = mcpp::toolchain::msvc::resolve_sdk_for(cl);
            if (sdk.sdk) {
                const auto sdkBin = sdk.sdk->root / "bin" / sdk.sdk->version / arch;
                put("MCPP_ABI_TOOL_RC", sdkBin / "rc.exe");
                put("MCPP_ABI_TOOL_MT", sdkBin / "mt.exe");
                // The environment the engine itself runs this toolset with.
                std::string env;
                for (auto const& ev : msvcCompiler && !tc.envOverrides.empty()
                                          ? tc.envOverrides
                                          : mcpp::toolchain::msvc::build_env_for_cl(cl, arch, *sdk.sdk)) {
                    if (!env.empty()) env += '\n';
                    env += ev.key + "=" + ev.value;
                }
                info["MCPP_TOOL_ENV"] = env;
                info["MCPP_TOOLSET_IDENTITY"] = std::format(
                    "msvc {}; sdk {}",
                    tc.msvcToolsVersion.empty() ? clBin.parent_path().parent_path()
                                                       .parent_path().filename().string()
                                                : tc.msvcToolsVersion,
                    sdk.sdk->version);
            }
            // A Visual Studio instance, when the toolset came from one: the
            // tools directory is <instance>/VC/Tools/MSVC/<v>.
            const auto tools = clBin.parent_path().parent_path().parent_path();
            const auto vc    = tools.parent_path().parent_path().parent_path();
            std::error_code ec;
            if (tc.msvcOrigin != "managed" && vc.filename() == "VC"
                && std::filesystem::exists(vc / "Auxiliary" / "Build", ec))
                put("MCPP_MSVC_INSTANCE_DIR", vc.parent_path());
        }
        // On the cl.exe row the row's tools are the toolset's.
        if (msvcCompiler) {
            for (auto role : {"LD", "AS", "MT"}) {
                auto it = info.find(std::string("MCPP_ABI_TOOL_") + role);
                if (it != info.end()) info[std::string("MCPP_TOOL_") + role] = it->second;
            }
            if (auto it = info.find("MCPP_ABI_TOOL_AR"); it != info.end()) info["MCPP_TOOL_AR"] = it->second;
        } else {
            put("MCPP_TOOL_LD", cxx);
            put("MCPP_TOOL_AS", cc.empty() ? cxx : cc);
            if (auto it = info.find("MCPP_ABI_TOOL_MT"); it != info.end()) info["MCPP_TOOL_MT"] = it->second;
        }
    } else {
        // Off the MSVC ABI the driver links and assembles, and the ABI's tools
        // are the row's.
        put("MCPP_TOOL_LD", cxx);
        put("MCPP_TOOL_AS", cc.empty() ? cxx : cc);
        for (auto role : {"CC", "CXX", "LD", "AR", "RC", "AS", "MT"}) {
            auto it = info.find(std::string("MCPP_TOOL_") + role);
            if (it != info.end()) info[std::string("MCPP_ABI_TOOL_") + role] = it->second;
        }
        const std::string_view family =
              tc.compiler == mcpp::toolchain::CompilerId::GCC   ? "gcc"
            : tc.compiler == mcpp::toolchain::CompilerId::Clang ? "clang" : "unknown";
        info["MCPP_TOOLSET_IDENTITY"] = std::format("{} {}", family, tc.version);
    }

    info["MCPP_NINJA"]            = mcpp::build::ninja_program_for(tc);
    info["MCPP_CXX_RUNTIME"]      = mcpp::build::program_cxx_runtime(m, tc);
    info["MCPP_MSVC_CRT_LINKAGE"] = mcpp::build::program_msvc_crt_linkage(m, tc);
    return info;
}

} // namespace

void fill_target_build_env(mcpp::build::BuildProgramEnv& e,
                           const mcpp::manifest::Manifest& m,
                           const mcpp::toolchain::Toolchain* tc,
                           const mcpp::config::GlobalConfig* cfg) {
    // The registry SubOS is where payloads are installed, whichever
    // toolchain or link mode resolved, so this is set before the toolchain
    // gate below.
    if (cfg) {
        const auto view = mcpp::xlings::paths::sysroot(mcpp::config::make_xlings_env(*cfg));
        e.pkgConfigLibdir = (view / "usr" / "lib" / "pkgconfig").generic_string()
            + mcpp::platform::env::path_list_separator()
            + (view / "usr" / "share" / "pkgconfig").generic_string();
    }
    e.toolchainDir  = (tc && !tc->binaryPath.empty())
        ? tc->binaryPath.parent_path().parent_path().string() : std::string{};
    e.targetSysroot = tc ? tc->targetSysrootRoot.string() : std::string{};
    e.compilerId    = !tc ? std::string{}
        : tc->compiler == mcpp::toolchain::CompilerId::GCC   ? "gcc"
        : tc->compiler == mcpp::toolchain::CompilerId::Clang ? "clang"
        : tc->compiler == mcpp::toolchain::CompilerId::MSVC  ? "msvc"
        : std::string{};
    e.targetLibc    = tc ? tc->targetSysrootPkg : std::string{};
    // Read from the toolchain the engine resolved, the same field the cache
    // key, the ABI tag and the toolchain fingerprint read. Not re-derived from
    // `compilerId`: clang answers "libc++" or "libstdc++" depending on how the
    // payload was configured, and deriving it here would restate an assumption
    // the resolver already measured.
    e.cxxStdlib     = tc ? tc->stdlibId : std::string{};
    if (!tc) return;
    e.buildInfo     = build_information(m, *tc);

    // The two flags mcpp passes to ITS OWN compiler, so a rule package driving
    // a second compiler passes the same two. Both read from the single
    // producer that already decides them for the engine's own command lines —
    // `resolve_link_model` for the sysroot, `gcc::binutils_prefix_dir` for the
    // `-B` — rather than a fifth re-derivation of either.
    if (auto lm = mcpp::toolchain::resolve_link_model(*tc);
        lm.mode == mcpp::toolchain::CLibMode::Sysroot)
        e.toolchainSysroot = lm.sysroot.string();
    e.toolchainBinutilsDir = mcpp::toolchain::gcc::binutils_prefix_dir(*tc).string();

    // The C LIBRARY's sub-directory for this ISA profile, from the freestanding
    // table — the same single read point the compile flags use.
    //
    // Gated on there being a C library at all, and the gate is the point: the
    // value is a multilib convention, so on the zero-libc tier there is nothing
    // for it to be a convention OF. Emitting `rv64gc/lp64d` there would hand a
    // kernel a path into a directory that does not exist, and the name of the
    // accessor would be a lie. All three libc-facing answers are empty together.
    if (!e.targetSysroot.empty())
        if (auto spec = mcpp::freestanding::resolve(tc->targetTriple))
            e.targetLibcProfile = std::string(spec->libdir);

    // Which builtins library the RESOLVED toolchain ships. Freestanding only:
    // on a hosted target the driver links them without being asked, and
    // handing a package a name it must not use would invite it to.
    if (auto t = mcpp::toolchain::triple::parse(tc->targetTriple);
        t && t->is_freestanding()) {
        e.targetBuiltinsLib = mcpp::toolchain::is_clang(*tc)
            ? "clang_rt.builtins-" + t->arch
            : std::string("gcc");
    }

    // #622 A11: MCPP_TARGET_MIN_PLATFORM_VERSION. One call, so a new consumer
    // (`dist-apple`, `dist-apk`) reads the same answer the compiler flag and
    // the fingerprint slot already resolved, rather than restating it.
    if (auto tt = mcpp::toolchain::triple::parse(tc->targetTriple))
        e.minPlatformVersion = min_platform_version(m, *tt, tc->binaryPath);
}

// THE PROJECT'S MINIMUM PLATFORM VERSION FOR THIS TARGET, in one place.
//
// Two platforms fuse it into the effective triple and each names it in its own
// words: macOS's deployment target lives in `[build]` because it applies to
// every Apple artefact a project produces, and Android's API level lives in
// `[target.<triple>]` because it applies to one row. `llvm_triple` takes one
// parameter for both, so the choice between them is made here rather than at
// each of its call sites -- there are two, and a decision made twice is the
// shape this codebase records most often.
std::string min_platform_version(const mcpp::manifest::Manifest& m,
                                 const mcpp::toolchain::triple::Triple& t,
                                 const std::filesystem::path& compilerPath) {
    if (t.is_android()) {
        if (auto it = m.targetOverrides.find(t.str()); it != m.targetOverrides.end())
            if (it->second.minApiLevel > 0)
                return std::to_string(it->second.minApiLevel);
        // AND THERE IS NO SUCH THING AS LEAVING IT OUT. This returned an empty
        // string with the comment "the NDK's own default, which clang
        // supplies", which was never verified and is false. Measured:
        //
        //     --target=aarch64-unknown-linux-android   (no level)
        //     sys/cdefs.h:365:2: error: Unversioned target triples are not
        //       supported!
        //
        // bionic refuses it, so the level is mandatory and a project that
        // never heard of API levels still needs one. The NDK declares the
        // floor it supports in `meta/platforms.json` and that is the honest
        // default -- the payload's own answer, which moves when the payload
        // does. macOS is the same shape and already works this way: its
        // default comes from the platform module, not from the manifest.
        // THE PAYLOAD'S OWN ANSWER FIRST, AND THE ENGINE'S DERIVATION AS
        // THE FALLBACK. `platform_floor` in `.mcpp-toolchain.json` is the
        // same number by a channel that does not require this engine to know
        // that an NDK keeps it in `meta/platforms.json`, nor that file's
        // schema. A payload shipping no descriptor still resolves, which is
        // what makes the descriptor additive.
        //
        // A MALFORMED descriptor is read as absence HERE ONLY, because this
        // function has no error channel and does not need one: a
        // payload-provided compiler reaches this point through
        // `payload_frontend`, which refuses a malformed descriptor by name
        // before any of these decisions are made.
        if (auto desc =
                mcpp::toolchain::payload_descriptor_for_compiler(compilerPath);
            desc && *desc && !(*desc)->platformFloor.empty())
            return (*desc)->platformFloor;
        if (auto level = mcpp::toolchain::ndk_min_api_level(compilerPath);
            level > 0)
            return std::to_string(level);
        return {};   // the caller refuses; see android_api_level_refusal
    }
    // APPLE'S TWO PLATFORMS ANSWER FROM TWO KEYS, ONE SLOT.
    //
    // "14.0" is a macOS version and means nothing to an iOS SDK, so the
    // project states them separately -- and only one of them can apply to any
    // one target, which is why they still share this function's single return
    // and the single fingerprint slot behind it.
    //
    // Empty is a legal answer here and not a refusal, unlike Android's, and
    // for the iOS rows prepare fills it with the located SDK's version before
    // this is read: an unversioned `arm64-apple-ios` made clang refuse
    // thread-local storage (measured, Xcode 16.4), so the driver's own
    // default is not the SDK's. Bionic rejects the unversioned triple
    // outright, which is the other half of the asymmetry.
    if (t.is_ios()) return m.buildConfig.iosDeploymentTarget;
    // AND ONLY FOR A macOS TARGET. `deployment_target` itself no longer
    // consults the host (#685); the discriminator is `t.os`, which is this
    // function's own target parameter and is available regardless of what
    // machine mcpp runs on. A non-Apple target (Linux, Windows, wasm,
    // freestanding) answers empty here, same as it always has.
    if (t.os == "macos")
        return mcpp::platform::macos::deployment_target(
            /*targetIsMacos=*/true, m.buildConfig.macosDeploymentTarget);
    return {};
}

std::optional<std::string>
host_tool_declared_toolchain(const mcpp::manifest::Manifest& tool,
                             const std::filesystem::path& toolRoot,
                             std::string_view platform) {
    auto effective = tool;
    if (const auto wsRoot = mcpp::project::find_workspace_root(toolRoot); !wsRoot.empty())
        if (auto ws = mcpp::manifest::load(wsRoot / "mcpp.toml");
            ws && mcpp::project::is_workspace_member(*ws, wsRoot, toolRoot))
            mcpp::project::inherit_workspace_root_position(effective, *ws, wsRoot);
    if (auto* row = find_target_entry(effective, mcpp::toolchain::triple::host_triple());
        row && !row->toolchain.empty())
        return row->toolchain;
    return effective.toolchain.for_platform(platform);
}

} // namespace mcpp::build
