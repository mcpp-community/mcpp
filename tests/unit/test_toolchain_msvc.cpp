
import std;
import mcpplibs.mtest;
import mcpp.invocation;
import mcpp.toolchain.model;
import mcpp.toolchain.msvc;
import mcpp.toolchain.registry;
import mcpp.toolchain.dialect;

using namespace mcpplibs::mtest;

using namespace mcpp::toolchain;

// ─── parse_cl_banner ─────────────────────────────────────────────────────

UTest parses_english_banner{"MsvcBanner/ParsesEnglishBanner", [] {
    auto r = msvc::parse_cl_banner(
        "Microsoft (R) C/C++ Optimizing Compiler Version 19.44.35211 for x64\n"
        "Copyright (C) Microsoft Corporation.  All rights reserved.\n"
        "\n"
        "usage: cl [ option... ] filename... [ /link linkoption... ]\n");
    expect(fatal(r.has_value()));
    expect(eq(r->first, "19.44.35211"));
    expect(eq(r->second, "x64"));
}};

UTest parses_localized_banner_by_tokens{"MsvcBanner/ParsesLocalizedBannerByTokens", [] {
    // Chinese VS reorders the sentence; only the tokens are stable.
    auto r = msvc::parse_cl_banner(
        "用于 x64 的 Microsoft (R) C/C++ 优化编译器 19.44.35211 版\n"
        "版权所有(C) Microsoft Corporation。保留所有权利。\n");
    expect(fatal(r.has_value()));
    expect(eq(r->first, "19.44.35211"));
    expect(eq(r->second, "x64"));
}};

UTest parses_arm64_and_four_component_versions{"MsvcBanner/ParsesArm64AndFourComponentVersions", [] {
    auto r = msvc::parse_cl_banner(
        "Microsoft (R) C/C++ Optimizing Compiler Version 19.29.30133.0 for ARM64\n");
    expect(fatal(r.has_value()));
    expect(eq(r->first, "19.29.30133.0"));
    expect(eq(r->second, "arm64"));
}};

UTest rejects_garbage{"MsvcBanner/RejectsGarbage", [] {
    expect(!(msvc::parse_cl_banner("").has_value()));
    expect(!(msvc::parse_cl_banner("bash: cl: command not found").has_value()));
    // A bare two-component number is not a cl version.
    expect(!(msvc::parse_cl_banner("something 1.2 else").has_value()));
}};

UTest msvc_banner_triple_for_arch{"MsvcBanner/TripleForArch", [] {
    expect(eq(msvc::triple_for_arch("x64"), "x86_64-pc-windows-msvc"));
    expect(eq(msvc::triple_for_arch("x86"), "i686-pc-windows-msvc"));
    expect(eq(msvc::triple_for_arch("arm64"), "aarch64-pc-windows-msvc"));
    // Unknown/empty arch falls back to the x64 triple.
    expect(eq(msvc::triple_for_arch(""), "x86_64-pc-windows-msvc"));
}};

// ─── install guidance ────────────────────────────────────────────────────

// mcpp#746: the compiler version is read from cl.exe's VS_FIXEDFILEINFO, as
// clang reads it, and said on the command line.
namespace {
std::string fixed_file_info(std::uint32_t ms, std::uint32_t ls,
                            std::uint32_t strucVersion = 0x00010000u) {
    std::string out;
    auto put = [&](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
    };
    put(0xFEEF04BDu);
    put(strucVersion);
    put(ms);
    put(ls);
    put(0); put(0);      // product version
    return out;
}
} // namespace

UTest reads_the_fixed_file_info_as_clang_does{"MsvcCompilerVersion/ReadsTheFixedFileInfoAsClangDoes", [] {
    // 19.51.36257.0: MS = 19 << 16 | 51, LS = 36257 << 16 | 0.
    std::string image(300, 'x');
    image += fixed_file_info((19u << 16) | 51u, (36257u << 16) | 0u);
    image += std::string(40, 'y');
    auto v = mcpp::toolchain::msvc::pe_file_version(image);
    expect(fatal(v.has_value()));
    expect(eq(*v, "19.51.36257"));
}};

UTest skips_a_signature_that_is_not_the_structure{"MsvcCompilerVersion/SkipsASignatureThatIsNotTheStructure", [] {
    // The four signature bytes can occur in code; only the structure, whose
    // next word is 0x00010000, is read.
    std::string image = fixed_file_info((1u << 16) | 2u, (3u << 16), 0xDEADBEEFu);
    image += fixed_file_info((19u << 16) | 44u, (35211u << 16) | 7u);
    expect(eq(mcpp::toolchain::msvc::pe_file_version(image).value_or(""), "19.44.35211"));
    expect(!(mcpp::toolchain::msvc::pe_file_version("no version here").has_value()));
}};

UTest is_read_from_the_compiler_clang_reads_for_the_target{"MsvcCompilerVersion/IsReadFromTheCompilerClangReadsForTheTarget", [] {
    // clang reads bin/Host{x64|x86}/<target>/cl.exe; each target's compiler
    // here carries its own build, under both host directories, so the answer
    // depends on the target and not on which host runs the test.
    namespace fs = std::filesystem;
    const auto tools = fs::temp_directory_path()
        / std::format("mcpp-746-{}", std::chrono::steady_clock::now().time_since_epoch().count())
        / "VC" / "Tools" / "MSVC" / "14.51.36231";
    const std::pair<std::string_view, std::uint32_t> builds[] = {
        {"x64", 36260u}, {"arm64", 36261u}, {"x86", 36262u}};
    for (auto host : {"Hostx64", "Hostx86"})
        for (auto [target, build] : builds) {
            fs::create_directories(tools / "bin" / host / target);
            std::ofstream os(tools / "bin" / host / target / "cl.exe", std::ios::binary);
            os << std::string(64, 'M') << fixed_file_info((19u << 16) | 51u, (build << 16));
        }
    namespace msvc = mcpp::toolchain::msvc;
    expect(eq(msvc::compiler_version_in_tools_dir(tools, "x86_64"), "19.51.36260"));
    expect(eq(msvc::compiler_version_in_tools_dir(tools, "aarch64"), "19.51.36261"));
    expect(eq(msvc::compiler_version_in_tools_dir(tools, "i686"), "19.51.36262"));
    // Every 32-bit x86 spelling is the x86 directory (2026.10.5.3; i386-i586
    // used to read x64's compiler).
    expect(eq(msvc::compiler_version_in_tools_dir(tools, "i386"), "19.51.36262"));
    expect(eq(msvc::compiler_version_in_tools_dir(tools, "i586"), "19.51.36262"));
    // No compiler where clang looks: no version, and clang falls back itself.
    expect(eq(msvc::compiler_version_in_tools_dir(tools / "absent", "x86_64"), ""));
    std::error_code ec;
    fs::remove_all(tools.parent_path().parent_path().parent_path().parent_path(), ec);
}};

UTest offers_both_origins{"MsvcGuidance/OffersBothOrigins", [] {
    auto g = msvc::install_guidance();
    expect(fatal(!g.empty()));
    // The managed origin has to be reachable from the message a user sees
    // when nothing is installed — otherwise "mcpp can install a toolset" is
    // true and undiscoverable at the same time.
    expect(ne(g.find("mcpp toolchain install msvc"), std::string::npos)) << g;
    expect(ne(g.find("[toolchain]"), std::string::npos)) << g;
    // …and the system origin stays offered, with its own route.
    expect(ne(g.find("winget"), std::string::npos)) << g;
    expect(ne(g.find("msvc@system"), std::string::npos)) << g;
    expect(ne(g.find("mcpp toolchain default msvc"), std::string::npos)) << g;
}};

// ─── spec layer: the VERSION axis decides the origin ─────────────────────

UTest system_origin_is_the_unversioned_spec{"MsvcSpec/SystemOriginIsTheUnversionedSpec", [] {
    for (auto s : {"msvc", "msvc@system"}) {
        auto spec = parse_toolchain_spec(s);
        expect(fatal(spec.has_value())) << s;
        expect(is_system_toolchain(*spec)) << s;
    }
    auto gcc = parse_toolchain_spec("gcc@16.1.0");
    expect(fatal(gcc.has_value()));
    expect(!is_system_toolchain(*gcc));
}};

UTest toolset_version_is_a_managed_payload_not_a_system_spec{"MsvcSpec/ToolsetVersionIsAManagedPayloadNotASystemSpec", [] {
    // The defect this closes: EVERY msvc spec used to be a system spec, so a
    // manifest could name a toolset and silently get whatever the machine
    // had. A version means a payload, exactly like gcc@16.1.0.
    for (auto s : {"msvc@14.44.35207", "msvc@14.52.36629"}) {
        auto spec = parse_toolchain_spec(s);
        expect(fatal(spec.has_value())) << s;
        expect(!is_system_toolchain(*spec)) << s;
        auto pkg = to_xim_package(*spec);
        expect(eq(pkg.ximName, "msvc")) << s;
        expect(eq(pkg.ximVersion, std::string(s).substr(5))) << s;
    }
}};

UTest stable_default_matches_any_detected_version{"MsvcSpec/StableDefaultMatchesAnyDetectedVersion", [] {
    // The persisted default is always the stable "msvc@system" — it matches
    // whatever concrete version detection reports (family-level match).
    auto def = parse_toolchain_spec("msvc@system");
    expect(fatal(def.has_value()));
    PayloadIdentity msvcId{ Family::Msvc, {} };
    expect(bool(spec_matches_payload(*def, msvcId, "19.44.35211")));
    expect(bool(spec_matches_payload(*def, msvcId, "19.29.30133")));
    PayloadIdentity gccId{ Family::Gcc, {} };
    expect(!spec_matches_payload(*def, gccId, "16.1.0"));
    auto gccDef = parse_toolchain_spec("gcc@16.1.0");
    expect(fatal(gccDef.has_value()));
    expect(!spec_matches_payload(*gccDef, msvcId, "19.44.35211"));
}};

UTest pinned_toolset_matches_only_its_own_payload{"MsvcSpec/PinnedToolsetMatchesOnlyItsOwnPayload", [] {
    // The other half of the family-level match above: once a version is
    // named, it has to be compared. A pin that matched any payload would be
    // the same silent divergence wearing a version number.
    auto pinned = parse_toolchain_spec("msvc@14.52.36629");
    expect(fatal(pinned.has_value()));
    PayloadIdentity msvcId{ Family::Msvc, {} };
    expect(bool(spec_matches_payload(*pinned, msvcId, "14.52.36629")));
    expect(!spec_matches_payload(*pinned, msvcId, "14.44.35207"));
}};

UTest payload_directory_is_identified_as_msvc{"MsvcSpec/PayloadDirectoryIsIdentifiedAsMsvc", [] {
    // Installed payloads are listed by walking `xim-x-<name>` directories.
    // Without this row an install would succeed and then be invisible.
    auto id = identify_xim_payload("msvc");
    expect(fatal(id.has_value()));
    expect(eq(id->family, Family::Msvc));
    expect(id->target.empty());   // host-target, like gcc and llvm
}};

// ─── managed origin: resolution from a payload ───────────────────────────
//
// These run on every platform, and that is the point. The managed path used
// to be untestable off Windows because every entry point started by probing
// the machine; `installation_at` takes the directory instead, so a fixture
// tree exercises the same code a real payload does.

namespace {

// A payload-shaped tree: <root>/VC/Tools/MSVC/<ver>/{bin/Hostx64/x64/cl.exe,
// modules/std.ixx}. Exactly what xim:msvc unpacks, minus the 80 MB.
struct FakeToolset {
    std::filesystem::path root;

    explicit FakeToolset(std::string_view tag) {
        root = std::filesystem::temp_directory_path()
             / std::format("mcpp-msvc-{}-{}", tag,
                           std::chrono::steady_clock::now().time_since_epoch().count());
        std::filesystem::create_directories(root);
    }
    ~FakeToolset() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
    FakeToolset(const FakeToolset&) = delete;
    FakeToolset& operator=(const FakeToolset&) = delete;

    void add_toolset(std::string_view version, bool withStdIxx = true) {
        auto tools = root / "VC" / "Tools" / "MSVC" / std::string(version);
        std::filesystem::create_directories(tools / "bin" / "Hostx64" / "x64");
        std::ofstream{tools / "bin" / "Hostx64" / "x64" / "cl.exe"} << "not a compiler";
        if (withStdIxx) {
            std::filesystem::create_directories(tools / "modules");
            std::ofstream{tools / "modules" / "std.ixx"} << "export module std;";
        }
    }
    // A COMPLETE SDK: headers and import libraries. Both, because
    // find_windows_sdk() requires both — a root with only headers is the
    // half-installed state `add_sdk_headers_only()` below exists to build.
    // Libs for both architectures so the fixture does not care which host it
    // is running on.
    void add_sdk(std::string_view version) {
        add_sdk_headers_only(version);
        for (auto arch : {"x64", "arm64"}) {
            auto lib = root / "Lib" / std::string(version) / "um" / arch;
            std::filesystem::create_directories(lib);
            std::ofstream{lib / "kernel32.lib"} << "not a library";
        }
    }
    void add_sdk_headers_only(std::string_view version) {
        auto inc = root / "Include" / std::string(version) / "ucrt";
        std::filesystem::create_directories(inc);
        std::ofstream{inc / "corecrt.h"} << "#pragma once";
    }
};

// RAII for an environment variable. nullopt = unset it.
//
// Unsetting matters as much as setting: these tests run on a Windows CI
// runner that may have vcvars exported, and an ambient WindowsSdkDir would
// otherwise answer before the fixture ever got a turn — a green test proving
// nothing about the code under it.
// Stated for this invocation, as mcpp states its own (mcpp.invocation).
using ScopedEnv = mcpp::invocation::ScopedEnv;

// Every SDK test starts from "nothing declared", then declares what it means
// to test.
struct NoSdkEnv {
    ScopedEnv dir{"WindowsSdkDir", std::nullopt};
    ScopedEnv ver{"WindowsSdkVersion", std::nullopt};
};

} // namespace

UTest resolves_the_declared_toolset_and_not_its_neighbour{"MsvcManaged/ResolvesTheDeclaredToolsetAndNotItsNeighbour", [] {
    FakeToolset t{"pick"};
    t.add_toolset("14.44.35207");
    t.add_toolset("14.52.36629");

    // Both present, and the OLDER one is asked for. A "latest" scan would
    // answer 14.52 — the exact substitution the managed origin exists to
    // prevent.
    auto older = msvc::installation_at(t.root, "14.44.35207");
    expect(fatal(older.has_value()));
    expect(eq(older->toolsVersion, "14.44.35207"));
    expect(bool(older->hasStdModules));
    expect(eq(older->clPath.filename(), "cl.exe"));
    expect(ne(older->clPath.string().find("14.44.35207"), std::string::npos));

    auto newer = msvc::installation_at(t.root, "14.52.36629");
    expect(fatal(newer.has_value()));
    expect(eq(newer->toolsVersion, "14.52.36629"));
}};

UTest the_version_dir_is_the_root_not_its_lone_subdirectory{"MsvcManaged/TheVersionDirIsTheRootNotItsLoneSubdirectory", [] {
    // A real installed payload has exactly ONE entry: `VC/`. The fetcher's
    // `XpkgPayload::root` treats the version directory as the root only when
    // it directly contains bin/ include/ lib/, and otherwise descends into a
    // lone subdirectory -- so for msvc it hands back `<ver>/VC`, and the
    // toolset then looks missing on a payload that installed perfectly.
    //
    // Pinning both sides here: the version directory resolves, and the `VC`
    // subdirectory does NOT. The second half is what makes this a test rather
    // than a restatement -- an implementation that searched upward from
    // whatever it was given would pass the first and fail this.
    FakeToolset t{"verdir"};
    t.add_toolset("14.44.35207");

    auto ok = msvc::installation_at(t.root, "14.44.35207");
    expect(fatal(ok.has_value())) << "the version directory must be the root";

    expect(!(msvc::installation_at(t.root / "VC", "14.44.35207").has_value()))
        << "a caller handing in the VC subdir is passing the wrong thing, and "
           "must be told so rather than quietly rescued";
}};

UTest absent_toolset_is_nullopt_not_a_substitute{"MsvcManaged/AbsentToolsetIsNulloptNotASubstitute", [] {
    FakeToolset t{"absent"};
    t.add_toolset("14.44.35207");
    // Nothing is "close enough": a pin that silently fell back would report
    // success while building with a compiler nobody asked for.
    expect(!(msvc::installation_at(t.root, "14.52.36629").has_value()));
    expect(!(msvc::installation_at(t.root, "14.44").has_value()));
    expect(!(msvc::installation_at(t.root, "").has_value()));
}};

UTest version_falls_back_to_the_declared_one_when_the_banner_cannot_be_read{"MsvcManaged/VersionFallsBackToTheDeclaredOneWhenTheBannerCannotBeRead", [] {
    // A fixture cl.exe produces no banner (and on Windows a real one would).
    // display_version() must still name the toolset rather than an empty
    // string — the declared version is a fact even when the probe fails.
    FakeToolset t{"banner"};
    t.add_toolset("14.52.36629");
    auto inst = msvc::installation_at(t.root, "14.52.36629");
    expect(fatal(inst.has_value()));
    expect(eq(inst->display_version(), "14.52.36629"));
}};

UTest payload_frontend_finds_cl_where_msvc_actually_keeps_it{"MsvcManaged/PayloadFrontendFindsClWhereMsvcActuallyKeepsIt", [] {
    // The defect this pins: `toolchain list` asked `toolchain_frontend(root /
    // "bin", …)`, got nothing, and skipped the row -- so an msvc toolset
    // installed correctly and then did not appear anywhere. Three places need
    // to know that cl.exe is four levels deeper than `bin/`; two knew.
    FakeToolset t{"frontend"};
    t.add_toolset("14.44.35207");

    auto spec = parse_toolchain_spec("msvc@14.44.35207");
    expect(fatal(spec.has_value()));
    auto pkg = to_xim_package(*spec);

    // The family comes from the PACKAGE now, not from a second argument every
    // caller had to source from a differently-named local -- five of them
    // composed `payload->binDir` themselves instead and so could not see
    // `frontendSubdir` at all.
    expect(eq(pkg.family, Family::Msvc));
    auto found = payload_frontend(t.root, pkg);
    if (auto&& failed = expect(fatal(found.has_value())); !failed) failed << found.error();
    expect(fatal(!found->empty()))
        << "payload_frontend found no cl.exe under " << t.root;
    expect(eq(found->filename(), "cl.exe"));

    // The `bin/`-shaped question is the one that used to be asked, and it
    // still answers nothing here — which is exactly why it was the wrong
    // question rather than a broken implementation.
    expect(toolchain_frontend(t.root / "bin", pkg).empty());

    // A root with no toolset at that version stays empty rather than
    // returning a path that does not exist.
    expect(payload_frontend(t.root,
                    to_xim_package(*parse_toolchain_spec("msvc@14.52.36629")))
                 .value().empty());

    // AND THE DIRECTORY THE SEARCH USED IS AVAILABLE FOR THE MESSAGE. MSVC is
    // the case that proves it is a lookup rather than a constant: the path
    // carries the toolset version, so a refusal naming `bin` would name a
    // directory nothing looked in.
    auto dir = payload_frontend_dir(t.root, pkg);
    expect(!dir.empty());
    expect(ne(dir.string().find("14.44.35207"), std::string::npos)) << dir;
}};

// ─── Windows SDK discovery ───────────────────────────────────────────────

UTest windows_sdk_dir_beats_the_hardcoded_paths{"MsvcSdk/WindowsSdkDirBeatsTheHardcodedPaths", [] {
    NoSdkEnv clean;
    FakeToolset t{"sdkenv"};
    t.add_sdk("10.0.26100.0");
    ScopedEnv dir{"WindowsSdkDir", t.root.string()};
    auto sdk = msvc::find_windows_sdk();
    expect(fatal(sdk.has_value()));
    expect(eq(sdk->root, t.root));
    expect(eq(sdk->version, "10.0.26100.0"));
}};

UTest windows_sdk_version_selects_among_installed_ones{"MsvcSdk/WindowsSdkVersionSelectsAmongInstalledOnes", [] {
    NoSdkEnv clean;
    FakeToolset t{"sdkver"};
    t.add_sdk("10.0.22621.0");
    t.add_sdk("10.0.26100.0");
    ScopedEnv dir{"WindowsSdkDir", t.root.string()};
    {   // vcvars exports it with a trailing backslash; that is not part of
        // the directory name, and comparing it raw finds nothing.
        ScopedEnv ver{"WindowsSdkVersion", "10.0.22621.0\\"};
        auto sdk = msvc::find_windows_sdk();
        expect(fatal(sdk.has_value()));
        expect(eq(sdk->version, "10.0.22621.0"));
    }
    // Unset again: highest usable wins.
    auto sdk = msvc::find_windows_sdk();
    expect(fatal(sdk.has_value()));
    expect(eq(sdk->version, "10.0.26100.0"));
}};

UTest extra_roots_cover_the_managed_payload{"MsvcSdk/ExtraRootsCoverTheManagedPayload", [] {
    NoSdkEnv clean;
    FakeToolset t{"sdkextra"};
    t.add_sdk("10.0.26100.0");
    // Nothing declared — the payload root is the only way the SDK can be
    // found, which is exactly the managed toolset's situation.
    std::array roots{t.root};
    auto sdk = msvc::find_windows_sdk(roots);
    expect(fatal(sdk.has_value()));
    expect(eq(sdk->root, t.root));
}};

UTest incomplete_sdk_root_is_not_an_answer{"MsvcSdk/IncompleteSdkRootIsNotAnAnswer", [] {
    // Include/<ver>/ exists but carries no ucrt/corecrt.h: a half-installed
    // SDK must read as absent, not as a usable one that fails later inside
    // the compiler. Checked through extraRoots so the assertion cannot be
    // satisfied by a real SDK on the machine.
    NoSdkEnv clean;
    FakeToolset t{"sdkpartial"};
    std::filesystem::create_directories(t.root / "Include" / "10.0.26100.0" / "um");
    std::array roots{t.root};
    auto sdk = msvc::find_windows_sdk(roots);
    if (sdk) expect(ne(sdk->root, t.root)) << "an SDK-less root was accepted";
}};

// ─── the toolset's own redistributable CRT ───────────────────────────────

namespace {

// A toolset laid out the way MSVC actually lays one out, including the part
// that trips a derivation: the Redist version is NOT the tools version.
struct FakeRedist {
    std::filesystem::path root, clPath;

    explicit FakeRedist(std::string_view toolsVer, std::string_view redistVer) {
        root = std::filesystem::temp_directory_path()
             / std::format("mcpp-redist-{}", std::chrono::steady_clock::now()
                                                 .time_since_epoch().count());
        auto tools = root / "VC" / "Tools" / "MSVC" / std::string(toolsVer);
        auto bin = tools / "bin" / "Hostx64" / "x64";
        std::filesystem::create_directories(bin);
        clPath = bin / "cl.exe";
        std::ofstream{clPath} << "not a compiler";
        add(redistVer, "x64", "Microsoft.VC143.CRT", "vcruntime140.dll");
    }
    void add(std::string_view ver, std::string_view arch,
             std::string_view comp, std::string_view file) {
        auto d = root / "VC" / "Redist" / "MSVC" / std::string(ver)
               / std::string(arch) / std::string(comp);
        std::filesystem::create_directories(d);
        std::ofstream{d / std::string(file)} << "dll";
    }
    void add_debug(std::string_view ver) {
        auto d = root / "VC" / "Redist" / "MSVC" / std::string(ver)
               / "debug_nonredist" / "x64" / "Microsoft.VC143.DebugCRT";
        std::filesystem::create_directories(d);
        std::ofstream{d / "vcruntime140d.dll"} << "dll";
    }
    ~FakeRedist() { std::error_code ec; std::filesystem::remove_all(root, ec); }
    FakeRedist(const FakeRedist&) = delete;
    FakeRedist& operator=(const FakeRedist&) = delete;
};

} // namespace

UTest found_from_the_compiler_path_alone{"MsvcRedist/FoundFromTheCompilerPathAlone", [] {
    // Nothing configured, no version derived: the compiler's own location
    // says which toolset this is, and the toolset carries its runtime.
    FakeRedist t{"14.44.35207", "14.44.35112"};
    auto d = msvc::vc_redist_dir(t.clPath, "x64");
    expect(fatal(!d.empty())) << "the toolset's redistributable CRT was not found";
    expect(std::filesystem::exists(d / "vcruntime140.dll"));
}};

UTest the_redist_version_is_not_the_tools_version{"MsvcRedist/TheRedistVersionIsNotTheToolsVersion", [] {
    // 14.44.35207 (tools) vs 14.44.35112 (redist) is what MSVC actually
    // ships. Deriving one from the other finds nothing — this is why the
    // directory is searched rather than composed.
    FakeRedist t{"14.44.35207", "14.44.35112"};
    auto d = msvc::vc_redist_dir(t.clPath, "x64");
    expect(fatal(!d.empty()));
    expect(ne(d.string().find("14.44.35112"), std::string::npos))
        << "did not land in the redist version directory: " << d.string();
}};

UTest newest_redist_wins{"MsvcRedist/NewestRedistWins", [] {
    FakeRedist t{"14.44.35207", "14.40.00000"};
    t.add("14.44.35112", "x64", "Microsoft.VC143.CRT", "vcruntime140.dll");
    auto d = msvc::vc_redist_dir(t.clPath, "x64");
    expect(fatal(!d.empty()));
    expect(ne(d.string().find("14.44.35112"), std::string::npos))
        << "older redist won: " << d.string();
}};

UTest the_debug_crt_is_never_returned{"MsvcRedist/TheDebugCrtIsNeverReturned", [] {
    // debug_nonredist may NOT be redistributed. Returning it would put
    // vcruntime140d.dll on PATH and, later, into a shipped artifact.
    FakeRedist t{"14.44.35207", "14.44.35112"};
    t.add_debug("14.99.99999");          // newer, and must still lose
    auto d = msvc::vc_redist_dir(t.clPath, "x64");
    expect(fatal(!d.empty()));
    expect(eq(d.string().find("debug_nonredist"), std::string::npos))
        << "returned the non-redistributable debug CRT: " << d.string();
}};

UTest a_toolset_without_a_redist_is_not_an_error{"MsvcRedist/AToolsetWithoutARedistIsNotAnError", [] {
    // msvc@system on a machine whose VS install omits the redist component,
    // for instance. Empty means "nothing to add to PATH", not a failure.
    FakeRedist t{"14.44.35207", "14.44.35112"};
    std::error_code ec;
    std::filesystem::remove_all(t.root / "VC" / "Redist", ec);
    expect(msvc::vc_redist_dir(t.clPath, "x64").empty());
}};

// #718: the LLVM row reaches the SAME redistributable from its sysroot's
// tools directory, since it runs no cl.exe of its own to derive one from.
UTest the_same_directory_is_reachable_from_the_tools_dir_alone{"MsvcRedist/TheSameDirectoryIsReachableFromTheToolsDirAlone", [] {
    FakeRedist t{"14.44.35207", "14.44.35112"};
    // <VC>/Tools/MSVC/<ver>/bin/Hostx64/x64/cl.exe -> up 4 reaches the tools
    // dir `Toolchain::msvcToolsDir` carries (<VC>/Tools/MSVC/<ver>).
    auto toolsDir = t.clPath.parent_path().parent_path().parent_path().parent_path();
    auto fromCl    = msvc::vc_redist_dir(t.clPath, "x64");
    auto fromTools = msvc::vc_redist_dir_for_tools_dir(toolsDir, "x86_64");
    expect(fatal(!fromCl.empty()));
    expect(eq(fromCl, fromTools));
}};

UTest tools_dir_arch_mapping{"MsvcRedist/ToolsDirArchMapping", [] {
    // The GNU-spelled triple architecture (what a Toolchain carries) maps to
    // the msvc spelling `vc_redist_dir` takes.
    FakeRedist t{"14.44.35207", "14.44.35112"};
    t.add("14.44.35112", "arm64", "Microsoft.VC143.CRT", "vcruntime140.dll");
    t.add("14.44.35112", "x86", "Microsoft.VC143.CRT", "vcruntime140.dll");
    auto toolsDir = t.clPath.parent_path().parent_path().parent_path().parent_path();
    expect(!(msvc::vc_redist_dir_for_tools_dir(toolsDir, "aarch64").empty()));
    expect(!(msvc::vc_redist_dir_for_tools_dir(toolsDir, "i686").empty()));
    expect(eq(msvc::vc_redist_dir_for_tools_dir(toolsDir, "i386"), msvc::vc_redist_dir_for_tools_dir(toolsDir, "i686")));
    expect(!(msvc::vc_redist_dir_for_tools_dir(toolsDir, "x86_64").empty()));
    // An architecture this toolset was never given a redist for finds none —
    // it does not fall back to a different one.
    std::error_code ec;
    auto vc = toolsDir.parent_path().parent_path().parent_path();   // <VC>
    std::filesystem::remove_all(vc / "Redist" / "MSVC" / "14.44.35112" / "arm64", ec);
    expect(msvc::vc_redist_dir_for_tools_dir(toolsDir, "aarch64").empty());
}};

UTest headers_without_import_libs_is_not_an_answer{"MsvcSdk/HeadersWithoutImportLibsIsNotAnAnswer", [] {
    // The half that used to pass. `Include/<v>/ucrt/corecrt.h` is there and
    // `Lib/` is not, which is exactly what a managed windows-sdk payload
    // looked like when its um-libs MSI had not unpacked: every TU compiled
    // and the link died with
    //     LINK : fatal error LNK1104: cannot open file 'kernel32.lib'
    // An SDK is headers AND libraries; reporting "found" for half of one puts
    // this root ahead of the machine's complete SDK and breaks a build that
    // would otherwise have worked.
    NoSdkEnv clean;
    FakeToolset t{"sdkheadersonly"};
    t.add_sdk_headers_only("10.0.26100.0");
    std::array roots{t.root};
    auto sdk = msvc::find_windows_sdk(roots);
    if (sdk) expect(ne(sdk->root, t.root))
        << "a headers-only SDK was accepted; the link would fail on kernel32.lib";
}};

UTest a_complete_root_is_still_accepted{"MsvcSdk/ACompleteRootIsStillAccepted", [] {
    // The other direction, so the check above cannot be satisfied by
    // rejecting everything.
    NoSdkEnv clean;
    FakeToolset t{"sdkcomplete"};
    t.add_sdk("10.0.26100.0");
    std::array roots{t.root};
    auto sdk = msvc::find_windows_sdk(roots);
    expect(fatal(bool(sdk))) << "a complete SDK root was rejected";
    expect(eq(sdk->root, t.root));
    expect(eq(sdk->version, "10.0.26100.0"));
}};

UTest a_partial_root_yields_to_a_complete_one{"MsvcSdk/APartialRootYieldsToACompleteOne", [] {
    // Ordering, not just acceptance: given both, the usable one must win even
    // though the partial one carries the HIGHER version — which is how the
    // managed payload outranked the system SDK in the first place.
    NoSdkEnv clean;
    FakeToolset partial{"sdkpartialhigh"};
    partial.add_sdk_headers_only("10.0.99999.0");
    FakeToolset complete{"sdkcompletelow"};
    complete.add_sdk("10.0.26100.0");
    std::array roots{partial.root, complete.root};
    auto sdk = msvc::find_windows_sdk(roots);
    expect(fatal(bool(sdk)));
    expect(eq(sdk->root, complete.root))
        << "the partial root won on version; it cannot link";
}};

UTest declared_root_outranks_the_managed_one{"MsvcSdk/DeclaredRootOutranksTheManagedOne", [] {
    // Both available. WindowsSdkDir is someone saying which one to use, and
    // it has to win — the same precedence VSINSTALLDIR has over vswhere.
    NoSdkEnv clean;
    FakeToolset declared{"sdkdeclared"};
    declared.add_sdk("10.0.22621.0");
    FakeToolset managed{"sdkmanaged"};
    managed.add_sdk("10.0.26100.0");   // newer, and still must not win
    ScopedEnv dir{"WindowsSdkDir", declared.root.string()};
    std::array roots{managed.root};
    auto sdk = msvc::find_windows_sdk(roots);
    expect(fatal(sdk.has_value()));
    expect(eq(sdk->root, declared.root));
    expect(eq(sdk->version, "10.0.22621.0"));
}};

UTest sibling_sdk_roots_are_empty_for_a_compiler_outside_any_store{"MsvcSdk/SiblingSdkRootsAreEmptyForACompilerOutsideAnyStore", [] {
    // A system cl.exe is not in an xlings store, so there is nothing to
    // offer — and offering the wrong thing would be worse than nothing.
    expect(msvc::sibling_sdk_roots(
        "C:/Program Files/Microsoft Visual Studio/18/Enterprise/VC/Tools/"
        "MSVC/14.51.36231/bin/Hostx64/x64/cl.exe").empty());
}};

// ─── model traits ────────────────────────────────────────────────────────

UTest bmi_traits_use_ifc{"MsvcModel/BmiTraitsUseIfc", [] {
    Toolchain tc;
    tc.compiler = CompilerId::MSVC;
    auto t = bmi_traits(tc);
    expect(eq(t.bmiDir, "ifc.cache"));
    expect(eq(t.bmiExt, ".ifc"));
    expect(bool(t.needsExplicitModuleOutput));
    expect(!t.scanNeedsFModules);
}};

// ─── std module minimum standard level ───────────────────────────────────
//
// microsoft/STL#3945 ("Supporting `import std;` in C++20") was fixed by
// STL#3977, first shipping in VS 2022 17.8 = cl 19.38. Older STLs still block
// C++20 and would fail inside std.ixx; they answer 23 so the build layer can
// refuse with an actionable message instead.
// See .agents/docs/2026-07-31-cpp20-standard-support-design.md §2.3.

UTest min_level_follows_stl_unblock_version{"MsvcStdModule/MinLevelFollowsStlUnblockVersion", [] {
    auto tc_of = [](std::string ver) {
        Toolchain tc;
        tc.compiler = CompilerId::MSVC;
        tc.version = std::move(ver);
        return tc;
    };
    // VS 2022 17.8 and newer: C++20 is allowed.
    expect(eq(msvc::std_module_min_level(tc_of("19.38.33130")), 20));
    expect(eq(msvc::std_module_min_level(tc_of("19.44.35211")), 20));
    expect(eq(msvc::std_module_min_level(tc_of("20.0")), 20));
    // Older toolsets keep the C++23 floor.
    expect(eq(msvc::std_module_min_level(tc_of("19.37.32825")), 23));
    expect(eq(msvc::std_module_min_level(tc_of("19.29.30153")), 23));
    // Unparseable banner version: stay strict rather than guess.
    expect(eq(msvc::std_module_min_level(tc_of("")), 23));
    expect(eq(msvc::std_module_min_level(tc_of("unknown")), 23));
}};

// #603 — THE QUESTION IS ASKED OF THE STL, NOT OF WHATEVER COMPILER REACHES IT.
//
// Two compilers reach the same `std.ixx`: cl.exe under `msvc@system`, and clang
// targeting `*-windows-msvc` with no libc++ std module present. The clang path
// used to hardcode 23, with a comment that named the reason correctly --
// `tc.version` is clang's there -- and drew the wrong conclusion from it.
//
// Calling the banner form from that path would be worse than the hardcode,
// because it would compare a CLANG version number against an MSVC threshold and
// be right by accident: clang 20.x passes `>= 19.38` and clang 19.x fails it.
// The toolset version is in the path of the module source that was selected.
UTest min_level_for_stl_reads_the_toolset_out_of_the_module_source_path{"MsvcStdModule/MinLevelForStlReadsTheToolsetOutOfTheModuleSourcePath", [] {
    auto lvl = [](std::string p) {
        return msvc::std_module_min_level_for_stl(std::filesystem::path(p));
    };
    // VS 2022 17.8 (toolset 14.38) is where microsoft/STL#3977 first ships.
    expect(eq(lvl("C:/VS/VC/Tools/MSVC/14.38.33130/modules/std.ixx"), 20));
    expect(eq(lvl("C:/VS/VC/Tools/MSVC/14.44.35207/modules/std.ixx"), 20));
    // Older toolsets keep the C++23 floor and get an actionable diagnostic.
    expect(eq(lvl("C:/VS/VC/Tools/MSVC/14.37.32822/modules/std.ixx"), 23));
    expect(eq(lvl("C:/VS/VC/Tools/MSVC/14.29.30133/modules/std.ixx"), 23));
    // A layout this mapping is not defined for answers 23 rather than guessing.
    // The safety the hardcode was after is kept for exactly these cases.
    expect(eq(lvl(""), 23));
    expect(eq(lvl("C:/VS/VC/Tools/MSVC/modules/std.ixx"), 23));
    expect(eq(lvl("/opt/llvm/share/libc++/v1/std.cppm"), 23));
    expect(eq(lvl("C:/VS/VC/Tools/MSVC/15.0.0/modules/std.ixx"), 23));
}};

// The two forms must agree for a well-formed installation, because that is the
// one path the banner form was verified on. If they could disagree there, this
// change would be a silent behaviour change on the toolchain that worked.
UTest the_stl_and_banner_forms_agree_for_a_well_formed_installation{"MsvcStdModule/TheStlAndBannerFormsAgreeForAWellFormedInstallation", [] {
    auto tc_of = [](std::string ver) {
        Toolchain tc;
        tc.compiler = CompilerId::MSVC;
        tc.version = std::move(ver);
        return tc;
    };
    // cl banner `19.<N>` pairs with toolset `14.<N>` -- that pairing is the
    // whole reason the `>= 38` predicate transfers unchanged.
    for (auto [banner, toolset] : { std::pair{"19.38.33130", "14.38.33130"},
                                    std::pair{"19.44.35211", "14.44.35207"},
                                    std::pair{"19.37.32825", "14.37.32822"},
                                    std::pair{"19.29.30153", "14.29.30133"} }) {
        expect(eq(msvc::std_module_min_level(tc_of(banner)), msvc::std_module_min_level_for_stl(std::filesystem::path(
                      std::string("C:/VS/VC/Tools/MSVC/") + toolset
                      + "/modules/std.ixx"))))
            << "banner " << banner << " vs toolset " << toolset;
    }
}};

// #422 — the std module must be built with the SAME CRT model as the TUs that
// import it.
//
// cl bakes `_MSVC_MT` / `_MSVC_MD` into every module it produces. The std build
// passed no `/M` flag at all, so it took cl's default (`/MT`), while a project
// on the default (dynamic) linkage compiles `/MD`. cl accepts the mismatch with
// a C5050 warning and then fails for real inside the ucrt headers:
//
//     corecrt_malloc.h(89): error C2375: 'free': redefinition; different linkage
//
// Two properties are pinned here, and the second is the one that makes the
// first stay true: the flag must be IN the command (so the module is right),
// and it must be in the command STRING (so it enters `std_build_commands`,
// which is part of the std cache identity — two CRT models then cannot share a
// cache directory and silently serve each other's module).
UTest std_module_carries_the_project_crt_model{"ToolchainMsvc/StdModuleCarriesTheProjectCrtModel", [] {
    Toolchain tc;
    tc.compiler   = CompilerId::MSVC;
    tc.binaryPath = "C:/vc/bin/cl.exe";
    tc.stdModuleSource  = "C:/vc/modules/std.ixx";
    tc.stdCompatSource  = "C:/vc/modules/std.compat.ixx";

    const std::filesystem::path cache = "C:/cache/std/key";

    for (std::string_view crt : {std::string_view("/MT"), std::string_view("/MD")}) {
        auto cmds = msvc::std_module_build_commands(tc, cache, "/std:c++23", crt);
        expect(fatal(eq(cmds.size(), 1u)));
        if (auto&& failed = expect(ne(cmds[0].find(crt), std::string::npos)); !failed) failed << crt << " missing from the std build command: " << cmds[0];

        auto compat = msvc::std_compat_build_commands(tc, cache, "/std:c++23", crt);
        expect(fatal(eq(compat.size(), 1u)));
        if (auto&& failed = expect(ne(compat[0].find(crt), std::string::npos)); !failed) failed << crt << " missing from the std.compat build command: " << compat[0];
    }

    // The two models must produce DIFFERENT command strings. Equal strings would
    // mean equal cache keys, i.e. the exact silent divergence this fixes.
    expect(ne(msvc::std_module_build_commands(tc, cache, "/std:c++23", "/MT")[0], msvc::std_module_build_commands(tc, cache, "/std:c++23", "/MD")[0]));

    // Empty keeps cl's own default, so non-MSVC callers are unaffected.
    auto bare = msvc::std_module_build_commands(tc, cache, "/std:c++23");
    expect(eq(bare[0].find("/MT"), std::string::npos));
    expect(eq(bare[0].find("/MD"), std::string::npos));
}};

// The mapping linkage -> CRT model lives in ONE place. It was derived twice —
// flags.cppm for the project's TUs, and cl's default for the std module — which
// is how they disagreed.
UTest crt_flag_has_a_single_derivation{"ToolchainMsvc/CrtFlagHasASingleDerivation", [] {
    Toolchain cl;
    cl.compiler = CompilerId::MSVC;
    const auto& msvcDialect = dialect_for(cl);
    expect(eq(msvc_crt_flag(msvcDialect, /*staticCrt=*/true), "/MT"));
    expect(eq(msvc_crt_flag(msvcDialect, /*staticCrt=*/false), "/MD"));

    // GNU has no counterpart; the helper must yield nothing rather than invent
    // a flag that would be passed to gcc.
    Toolchain gcc;
    gcc.compiler = CompilerId::GCC;
    const auto& gnu = dialect_for(gcc);
    expect(msvc_crt_flag(gnu, false).empty());
}};

// Both manifest keys reach the SAME physical switch, because on the MSVC ABI
// they are the same switch. `cxx_runtime = "self-contained"` used to report
// "not implemented" while `linkage = "static"` quietly did the very thing it
// said was unimplemented.
UTest both_keys_select_the_static_crt{"ToolchainMsvc/BothKeysSelectTheStaticCrt", [] {
    expect(bool(msvc_wants_static_crt("static", "")));
    expect(bool(msvc_wants_static_crt("", "self-contained")));
    expect(bool(msvc_wants_static_crt("static", "self-contained")));

    // Default: neither written down. This has to stay /MD — most roles
    // DEFAULT to the self-contained contract, so keying off the resolved
    // contract instead of the written key would flip every Windows build to
    // /MT and split the CRT across a project's dependencies.
    expect(!msvc_wants_static_crt("", ""));
    expect(!msvc_wants_static_crt("dynamic", ""));
    expect(!msvc_wants_static_crt("", "host-coupled"));
    expect(!msvc_wants_static_crt("", "toolchain-coupled"));
}};

// ─── the SDK axis: bound for a managed toolset, searched for a system one ──
//
// Until now the Windows SDK had no identity: `find_windows_sdk()` scanned,
// and whatever the scan reached first won, for BOTH origins. That is the
// defect .agents/docs/2026-08-16-windows-toolchain-three-axes-design.md §2
// is about — the compiler got a version axis and the headers it compiles
// against did not, so two machines could build one manifest against two SDKs
// with nothing in the log naming either.

namespace {

// An xlings store, laid out the way the real one is: `xpkgs_from_compiler`
// finds the store by walking up for a directory literally named `xpkgs`, so
// that name is load-bearing and not decoration.
struct FakeStore {
    std::filesystem::path root;      // …/xpkgs

    FakeStore() {
        root = std::filesystem::temp_directory_path()
             / std::format("mcpp-store-{}", std::chrono::steady_clock::now()
                                                .time_since_epoch().count())
             / "data" / "xpkgs";
        std::filesystem::create_directories(root);
    }
    ~FakeStore() {
        std::error_code ec;
        std::filesystem::remove_all(root.parent_path().parent_path(), ec);
    }
    FakeStore(const FakeStore&) = delete;
    FakeStore& operator=(const FakeStore&) = delete;

    std::filesystem::path add_toolset(std::string_view version) {
        auto tools = root / "xim-x-msvc" / std::string(version)
                   / "VC" / "Tools" / "MSVC" / std::string(version);
        auto bin = tools / "bin" / "Hostx64" / "x64";
        std::filesystem::create_directories(bin);
        std::ofstream{bin / "cl.exe"} << "not a compiler";
        std::filesystem::create_directories(tools / "modules");
        std::ofstream{tools / "modules" / "std.ixx"} << "export module std;";
        return bin / "cl.exe";
    }
    // A complete SDK payload — headers AND import libraries, for both
    // architectures so the fixture does not care which host runs it.
    std::filesystem::path add_sdk(std::string_view version) {
        auto sdk = root / "xim-x-windows-sdk" / std::string(version);
        auto inc = sdk / "Include" / std::string(version) / "ucrt";
        std::filesystem::create_directories(inc);
        std::ofstream{inc / "corecrt.h"} << "#pragma once";
        for (auto arch : {"x64", "arm64"}) {
            auto lib = sdk / "Lib" / std::string(version) / "um" / arch;
            std::filesystem::create_directories(lib);
            std::ofstream{lib / "kernel32.lib"} << "not a library";
        }
        return sdk;
    }
};

} // namespace

UTest a_compiler_in_a_store_is_managed_and_one_outside_is_not{"MsvcSdkOrigin/ACompilerInAStoreIsManagedAndOneOutsideIsNot", [] {
    FakeStore store;
    auto cl = store.add_toolset("14.44.35207");
    expect(eq(msvc::origin_of(cl), Origin::Managed));
    expect(eq(msvc::origin_of(
        "C:/Program Files/Microsoft Visual Studio/18/Enterprise/VC/Tools/"
        "MSVC/14.51.36231/bin/Hostx64/x64/cl.exe"), Origin::SystemMsvc));
}};

UTest a_managed_toolset_takes_the_sdk_from_its_own_store{"MsvcSdkOrigin/AManagedToolsetTakesTheSdkFromItsOwnStore", [] {
    NoSdkEnv clean;
    FakeStore store;
    auto cl = store.add_toolset("14.44.35207");
    store.add_sdk("10.0.26100.0");

    auto choice = msvc::resolve_sdk_for(cl);
    expect(fatal(choice.sdk.has_value()))
        << "the toolset's own SDK payload was not found";
    expect(eq(choice.origin, Origin::Managed));
    expect(eq(choice.sdk->version, "10.0.26100.0"));
    expect(choice.note.empty()) << choice.note;
}};

// THE CRITERION THIS AXIS EXISTS FOR. An environment that can overwrite the
// choice means the manifest did not pin anything — it only expressed a
// preference that the machine gets to overrule, silently.
//
// This is the design doc's §6 acceptance test, as a unit test: point
// WindowsSdkDir somewhere else entirely and the build must still use the
// payload's SDK, and must SAY that the variable was ignored.
UTest windows_sdk_dir_cannot_override_a_pinned_toolsets_sdk{"MsvcSdkOrigin/WindowsSdkDirCannotOverrideAPinnedToolsetsSdk", [] {
    NoSdkEnv clean;
    FakeStore store;
    auto cl = store.add_toolset("14.44.35207");
    store.add_sdk("10.0.26100.0");

    FakeToolset elsewhere{"sdk-elsewhere"};
    elsewhere.add_sdk("10.0.22621.0");
    ScopedEnv dir{"WindowsSdkDir", elsewhere.root.string()};

    auto choice = msvc::resolve_sdk_for(cl);
    expect(fatal(choice.sdk.has_value()));
    expect(eq(choice.sdk->version, "10.0.26100.0"))
        << "the environment overrode a pinned toolset's SDK; the pin is not one";
    expect(ne(choice.sdk->root, elsewhere.root));
    expect(ne(choice.note.find("WindowsSdkDir"), std::string::npos))
        << "an ignored override that says nothing is indistinguishable from "
           "one that was never set: " << choice.note;
}};

UTest windows_sdk_version_does_not_pick_among_payloads_either{"MsvcSdkOrigin/WindowsSdkVersionDoesNotPickAmongPayloadsEither", [] {
    // Same override wearing a smaller hat: naming a version is still the
    // environment choosing which headers a pinned build compiles against.
    NoSdkEnv clean;
    FakeStore store;
    auto cl = store.add_toolset("14.44.35207");
    store.add_sdk("10.0.22621.0");
    store.add_sdk("10.0.26100.0");
    ScopedEnv ver{"WindowsSdkVersion", "10.0.22621.0\\"};

    auto choice = msvc::resolve_sdk_for(cl);
    expect(fatal(choice.sdk.has_value()));
    expect(eq(choice.sdk->version, "10.0.26100.0"))
        << "WindowsSdkVersion selected among the store's payloads";
    expect(!choice.note.empty());
}};

UTest a_managed_toolset_with_no_sdk_payload_falls_back_and_says_so{"MsvcSdkOrigin/AManagedToolsetWithNoSdkPayloadFallsBackAndSaysSo", [] {
    // Working beats failing here — the machine may well have a complete SDK,
    // and refusing to build would be a regression for anyone whose toolset
    // predates the SDK dependency. What must not happen is the fallback being
    // invisible: the build is no longer reproducible, and only this line says
    // so.
    NoSdkEnv clean;
    FakeStore store;
    auto cl = store.add_toolset("14.44.35207");   // no add_sdk

    FakeToolset machine{"sdk-machine"};
    machine.add_sdk("10.0.22621.0");
    ScopedEnv dir{"WindowsSdkDir", machine.root.string()};

    auto choice = msvc::resolve_sdk_for(cl);
    expect(fatal(choice.sdk.has_value()));
    expect(eq(choice.sdk->root, machine.root));
    expect(ne(choice.note.find("machine"), std::string::npos)) << choice.note;
}};

UTest a_system_toolset_keeps_the_declared_search_chain{"MsvcSdkOrigin/ASystemToolsetKeepsTheDeclaredSearchChain", [] {
    // The other origin is unchanged, and must be: a machine's things can only
    // be found by looking, and WindowsSdkDir is the most specific answer
    // available there — the same precedence VSINSTALLDIR has over vswhere.
    NoSdkEnv clean;
    FakeToolset vs{"sdk-system-vs"};
    vs.add_toolset("14.51.36231");
    FakeToolset declared{"sdk-system-declared"};
    declared.add_sdk("10.0.22621.0");
    ScopedEnv dir{"WindowsSdkDir", declared.root.string()};

    auto cl = vs.root / "VC" / "Tools" / "MSVC" / "14.51.36231"
            / "bin" / "Hostx64" / "x64" / "cl.exe";
    auto choice = msvc::resolve_sdk_for(cl);
    expect(eq(choice.origin, Origin::SystemMsvc));
    expect(fatal(choice.sdk.has_value()));
    expect(eq(choice.sdk->root, declared.root));
    expect(choice.note.empty())
        << "nothing was ignored, so there is nothing to report: " << choice.note;
}};

// ─── Toolset selection: one rule for the cl.exe row and the clang row ─────
//
// `select_system_toolset` reads only the directories it is handed, so these
// run on any host against a synthetic machine: two Visual Studio instances,
// side-by-side toolsets, `Microsoft.VCToolsVersion.default.txt`, and the
// environment a developer prompt exports.

namespace {

// One Visual Studio instance on disk: <root>/VC/Tools/MSVC/<v>/{include,
// lib/x64, bin/Hostx64/x64/cl.exe} per toolset, and the default-toolset file.
struct FakeInstance {
    FakeToolset dir;
    msvc::VsInstance inst;

    FakeInstance(std::string_view tag, std::string_view product,
                 std::string_view installVersion) : dir(tag) {
        inst.root = dir.root;
        inst.product = std::string(product);
        inst.installVersion = std::string(installVersion);
    }
    // complete=false leaves out lib/x64, the state of a half-installed toolset.
    void toolset(std::string_view v, bool complete = true, bool withCl = true) {
        auto tools = dir.root / "VC" / "Tools" / "MSVC" / std::string(v);
        std::filesystem::create_directories(tools / "include");
        if (complete) std::filesystem::create_directories(tools / "lib" / "x64");
        if (withCl) {
            std::filesystem::create_directories(tools / "bin" / "Hostx64" / "x64");
            std::ofstream{tools / "bin" / "Hostx64" / "x64" / "cl.exe"} << "x";
        }
    }
    void default_is(std::string_view v) {
        auto b = dir.root / "VC" / "Auxiliary" / "Build";
        std::filesystem::create_directories(b);
        std::ofstream{b / "Microsoft.VCToolsVersion.default.txt"} << v << "\r\n";
    }
    std::filesystem::path tools(std::string_view v) const {
        return dir.root / "VC" / "Tools" / "MSVC" / std::string(v);
    }
};

// VS 2022 (older installation) with a side-by-side 14.38, and an Insiders VS
// 18 (newer installation). The shape of a machine that has used Visual Studio
// for a few years.
struct TwoInstances {
    FakeInstance vs2022{"sel-2022", "Visual Studio Community 2022", "17.14.36301.6"};
    FakeInstance vs18{"sel-18", "Visual Studio Community 18 Insiders", "18.0.11010.1"};
    TwoInstances() {
        vs2022.toolset("14.44.35207");
        vs2022.toolset("14.38.33130");
        vs2022.default_is("14.44.35207");
        vs18.toolset("14.51.36014");
        vs18.default_is("14.51.36014");
    }
    std::vector<msvc::VsInstance> all() const { return {vs2022.inst, vs18.inst}; }
};

} // namespace

UTest compared_as_numbers_not_strings{"MsvcToolsetVersion/ComparedAsNumbersNotStrings", [] {
    expect(lt(msvc::compare_toolset_versions("14.9", "14.10"), 0));
    expect(gt(msvc::compare_toolset_versions("14.44.35207", "14.38.33130"), 0));
    expect(eq(msvc::compare_toolset_versions("14.44", "14.44.0"), 0));
    expect(eq(msvc::compare_toolset_versions("17.14.36301.6", "17.14.36301.6"), 0));
}};

UTest a_prefix_matches_whole_components{"MsvcToolsetVersion/APrefixMatchesWholeComponents", [] {
    expect(msvc::toolset_version_matches("14.44", "14.44.35207"));
    expect(msvc::toolset_version_matches("14.44.35207", "14.44.35207"));
    expect(msvc::toolset_version_matches("14", "14.51.36014"));
    // `14.4` is the 14.4 line, not a text prefix of 14.44.
    expect(!msvc::toolset_version_matches("14.4", "14.44.35207"));
    expect(!msvc::toolset_version_matches("14.44.35207.1", "14.44.35207"));
    expect(!msvc::toolset_version_matches("", "14.44.35207"));
}};

UTest a_pinned_version_is_found_in_whichever_instance_has_it{"MsvcToolsetSelect/APinnedVersionIsFoundInWhicheverInstanceHasIt", [] {
    TwoInstances m;
    msvc::ToolsetNeeds needs;
    auto a = msvc::select_system_toolset(m.all(), {}, "14.38", needs);
    expect(fatal(a.has_value()));
    expect(eq(a->version, "14.38.33130"));
    expect(eq(a->toolsDir, m.vs2022.tools("14.38.33130")));
    expect(eq(a->via, "pinned"));

    auto b = msvc::select_system_toolset(m.all(), {}, "14.51.36014", needs);
    expect(fatal(b.has_value()));
    expect(eq(b->vsRoot, m.vs18.dir.root));
    expect(eq(b->product, "Visual Studio Community 18 Insiders"));

    // The highest match across every instance, by number.
    auto c = msvc::select_system_toolset(m.all(), {}, "14", needs);
    expect(fatal(c.has_value()));
    expect(eq(c->version, "14.51.36014"));
}};

UTest a_pin_with_no_installed_match_is_nullopt_so_the_package_is_used{"MsvcToolsetSelect/APinWithNoInstalledMatchIsNulloptSoThePackageIsUsed", [] {
    TwoInstances m;
    expect(!(msvc::select_system_toolset(m.all(), {}, "14.29",
                                             msvc::ToolsetNeeds{}).has_value()));
    // `14.4` does not reach 14.44 by text.
    expect(!(msvc::select_system_toolset(m.all(), {}, "14.4",
                                             msvc::ToolsetNeeds{}).has_value()));
}};

UTest a_pin_ignores_the_environment_and_says_so{"MsvcToolsetSelect/APinIgnoresTheEnvironmentAndSaysSo", [] {
    // A developer prompt opened with `-vcvars_ver=14.38` exports this, and a
    // pinned manifest must not follow it.
    TwoInstances m;
    msvc::MsvcEnvSnapshot env;
    env.vcToolsInstallDir = m.vs2022.tools("14.38.33130").string() + "/";
    env.vsInstallDir = m.vs2022.dir.root;
    auto c = msvc::select_system_toolset(m.all(), env, "14.44.35207",
                                         msvc::ToolsetNeeds{});
    expect(fatal(c.has_value()));
    expect(eq(c->version, "14.44.35207"));
    bool noted = false;
    for (auto const& n : c->notes)
        noted = noted || (n.find("VCToolsInstallDir") != std::string::npos
                          && n.find("14.38.33130") != std::string::npos);
    expect(bool(noted)) << "an ignored declaration has to be reported";
}};

UTest an_incomplete_match_is_skipped_and_reported{"MsvcToolsetSelect/AnIncompleteMatchIsSkippedAndReported", [] {
    TwoInstances m;
    m.vs18.toolset("14.44.35210", /*complete=*/false);   // newer, but no lib/x64
    auto c = msvc::select_system_toolset(m.all(), {}, "14.44", msvc::ToolsetNeeds{});
    expect(fatal(c.has_value()));
    expect(eq(c->version, "14.44.35207"));
    expect(fatal(!(c->notes.empty())));
    expect(ne(c->notes.front().find("14.44.35210"), std::string::npos));
}};

UTest system_follows_the_declarations_in_order{"MsvcToolsetSelect/SystemFollowsTheDeclarationsInOrder", [] {
    TwoInstances m;
    msvc::ToolsetNeeds needs;

    msvc::MsvcEnvSnapshot byTools;
    byTools.vcToolsInstallDir = m.vs2022.tools("14.38.33130");
    auto a = msvc::select_system_toolset(m.all(), byTools, "system", needs);
    expect(fatal(a.has_value()));
    expect(eq(a->version, "14.38.33130"));
    expect(eq(a->via, "VCToolsInstallDir"));

    msvc::MsvcEnvSnapshot byInstance;
    byInstance.vsInstallDir = m.vs2022.dir.root;
    auto b = msvc::select_system_toolset(m.all(), byInstance, "system", needs);
    expect(fatal(b.has_value()));
    expect(eq(b->version, "14.44.35207")) << "the instance's default, not its highest";
    expect(eq(b->via, "VSINSTALLDIR"));

    msvc::MsvcEnvSnapshot byPath;
    byPath.clOnPath = m.vs2022.tools("14.38.33130") / "bin" / "Hostx64" / "x64" / "cl.exe";
    auto c = msvc::select_system_toolset(m.all(), byPath, "system", needs);
    expect(fatal(c.has_value()));
    expect(eq(c->version, "14.38.33130"));
    expect(eq(c->via, "PATH"));

    auto d = msvc::select_system_toolset(m.all(), {}, "system", needs);
    expect(fatal(d.has_value()));
    expect(eq(d->version, "14.51.36014")) << "the newest installation's default";
    expect(eq(d->via, "newest instance"));
}};

UTest the_default_file_outranks_the_highest_directory{"MsvcToolsetSelect/TheDefaultFileOutranksTheHighestDirectory", [] {
    // A side-by-side toolset newer than the instance's default is what VS's
    // own vcvars would NOT pick; neither does this.
    FakeInstance vs{"sel-default", "Visual Studio Community 2022", "17.14.1"};
    vs.toolset("14.44.35207");
    vs.toolset("14.50.35717");
    vs.default_is("14.44.35207");
    auto c = msvc::select_system_toolset({vs.inst}, {}, "system", msvc::ToolsetNeeds{});
    expect(fatal(c.has_value()));
    expect(eq(c->version, "14.44.35207"));
}};

UTest the_newest_instance_without_a_toolset_is_passed_over{"MsvcToolsetSelect/TheNewestInstanceWithoutAToolsetIsPassedOver", [] {
    // Visual Studio installed without the C++ workload: newest by installation
    // version, and nothing a build can use.
    TwoInstances m;
    FakeInstance bare{"sel-bare", "Visual Studio Community 2026", "19.0.1.1"};
    auto all = m.all();
    all.insert(all.begin(), bare.inst);
    auto c = msvc::select_system_toolset(all, {}, "system", msvc::ToolsetNeeds{});
    expect(fatal(c.has_value()));
    expect(eq(c->version, "14.51.36014"));
}};

UTest the_clang_row_does_not_need_cl_exe{"MsvcToolsetSelect/TheClangRowDoesNotNeedClExe", [] {
    FakeInstance vs{"sel-nocl", "Visual Studio Build Tools 2022", "17.14.1"};
    vs.toolset("14.44.35207", /*complete=*/true, /*withCl=*/false);
    msvc::ToolsetNeeds clRow;
    msvc::ToolsetNeeds clangRow;
    clangRow.cl = false;
    expect(!(msvc::select_system_toolset({vs.inst}, {}, "system", clRow).has_value()));
    auto c = msvc::select_system_toolset({vs.inst}, {}, "system", clangRow);
    expect(fatal(c.has_value()));
    expect(eq(c->version, "14.44.35207"));
}};

UTest the_refusal_lists_what_is_installed{"MsvcToolsetSelect/TheRefusalListsWhatIsInstalled", [] {
    TwoInstances m;
    auto lines = msvc::describe_system_toolsets(m.all(), msvc::ToolsetNeeds{});
    expect(fatal(eq(lines.size(), 3u)));
    bool defaultMarked = false;
    for (auto const& l : lines)
        defaultMarked = defaultMarked || (l.find("14.44.35207") != std::string::npos
                                          && l.find("(default)") != std::string::npos);
    expect(bool(defaultMarked));
}};

// ─── `xim:` names the ecosystem package ──────────────────────────────────

UTest xim_msvc_is_the_package_only{"ToolchainSpelling/XimMsvcIsThePackageOnly", [] {
    auto spec = parse_toolchain_spec("xim:msvc@14.44.35207");
    if (auto&& failed = expect(fatal(spec.has_value())); !failed) failed << spec.error();
    expect(eq(spec->family, Family::Msvc));
    expect(eq(spec->version, "14.44.35207"));
    expect(bool(spec->ecosystemOnly));
    expect(!is_system_toolchain(*spec));
    // The prefix changes the meaning for msvc, so it survives persistence.
    expect(eq(spec->spec_str(), "xim:msvc@14.44.35207"));
    expect(eq(to_xim_package(*spec).target(), "xim:msvc@14.44.35207"));

    auto bare = parse_toolchain_spec("msvc@14.44.35207");
    expect(fatal(bare.has_value()));
    expect(!bare->ecosystemOnly);
    expect(eq(bare->spec_str(), "msvc@14.44.35207"));
}};

UTest xim_is_a_synonym_where_there_is_no_system_origin{"ToolchainSpelling/XimIsASynonymWhereThereIsNoSystemOrigin", [] {
    for (auto [prefixed, bare] : {std::pair{"xim:gcc@16.1.0", "gcc@16.1.0"},
                                  std::pair{"xim:llvm@22.1.8", "llvm@22.1.8"}}) {
        auto a = parse_toolchain_spec(prefixed);
        auto b = parse_toolchain_spec(bare);
        expect(fatal(a.has_value())) << prefixed;
        expect(fatal(b.has_value())) << bare;
        expect(eq(a->spec_str(), b->spec_str())) << "one canonical spelling";
        expect(eq(to_xim_package(*a).target(), to_xim_package(*b).target()));
    }
}};

UTest xim_with_system_is_refused_naming_both_spellings{"ToolchainSpelling/XimWithSystemIsRefusedNamingBothSpellings", [] {
    auto spec = parse_toolchain_spec("xim:msvc@system");
    expect(fatal(!spec.has_value()));
    if (auto&& failed = expect(ne(spec.error().find("msvc@system"), std::string::npos)); !failed) failed << spec.error();
    if (auto&& failed = expect(ne(spec.error().find("xim:msvc@<version>"), std::string::npos)); !failed) failed << spec.error();
}};

UTest another_namespace_is_refused{"ToolchainSpelling/AnotherNamespaceIsRefused", [] {
    auto spec = parse_toolchain_spec("foo:gcc@16.1.0");
    expect(fatal(!spec.has_value()));
    if (auto&& failed = expect(ne(spec.error().find("xim:"), std::string::npos)); !failed) failed << spec.error();
}};

// vswhere `-format text`: one instance per `instanceId:` line, CRLF endings,
// `displayName` and not `catalog_productDisplayVersion`, and an instance with
// no installation path dropped.
UTest one_instance_per_instance_id{"MsvcVswhereText/OneInstancePerInstanceId", [] {
    const std::string text =
        "instanceId: 1a2b\r\n"
        "installationName: VisualStudio/17.14.8+36301.6\r\n"
        "installationPath: C:\\Program Files\\Microsoft Visual Studio\\2022\\Enterprise\r\n"
        "installationVersion: 17.14.36301.6\r\n"
        "displayName: Visual Studio Enterprise 2022\r\n"
        "catalog_productDisplayVersion: 17.14.8\r\n"
        "properties_nickname: \r\n"
        "\r\n"
        "instanceId: 3c4d\r\n"
        "installationPath: D:\\VS\\Preview\r\n"
        "installationVersion: 18.0.11010.1\r\n"
        "displayName: Visual Studio Community 2026 Insiders\r\n"
        "instanceId: 5e6f\r\n"
        "displayName: incomplete\r\n";
    auto v = mcpp::toolchain::msvc::parse_vswhere_text(text);
    expect(fatal(eq(v.size(), 2u)));
    expect(eq(v[0].root.string(), "C:\\Program Files\\Microsoft Visual Studio\\2022\\Enterprise"));
    expect(eq(v[0].installVersion, "17.14.36301.6"));
    expect(eq(v[0].product, "Visual Studio Enterprise 2022"));
    expect(eq(v[1].root.string(), "D:\\VS\\Preview"));
    expect(eq(v[1].product, "Visual Studio Community 2026 Insiders"));
    expect(mcpp::toolchain::msvc::parse_vswhere_text("").empty());
}};

// D16: the refusal and the first-run notice say which half of the MSVC ABI is
// missing and where the SDK was looked for. The sentence they replaced said
// "no Visual Studio found" on a machine whose Visual Studio was found and
// whose SDK sat on another drive (design 2026-10-10 §12.1, run 37974353683).
UTest the_description_names_the_missing_half_and_where_it_looked{"MsvcProbe/TheDescriptionNamesTheMissingHalfAndWhereItLooked", [] {
    namespace msvc = mcpp::toolchain::msvc;
    msvc::MsvcProbe vsOnly;
    vsOnly.visualStudio = {"D:\\VS\\2026\\Enterprise"};
    vsOnly.stl = true;
    vsOnly.sdkSearched = {"D:\\Kits\\10", "C:\\Program Files (x86)\\Windows Kits\\10"};
    const auto a = msvc::describe_msvc_probe(vsOnly);
    expect(ne(a.find("Visual Studio was found at D:\\VS\\2026\\Enterprise"), std::string::npos)) << a;
    expect(ne(a.find("no Windows SDK"), std::string::npos)) << a;
    expect(ne(a.find("D:\\Kits\\10"), std::string::npos)) << a;
    expect(eq(a.find("no Visual Studio"), std::string::npos)) << a;

    msvc::MsvcProbe sdkOnly;
    sdkOnly.sdk = msvc::WindowsSdk{"D:\\Kits\\10", "10.0.26100.0"};
    const auto b = msvc::describe_msvc_probe(sdkOnly);
    expect(ne(b.find("10.0.26100.0"), std::string::npos)) << b;
    expect(ne(b.find("no Visual Studio"), std::string::npos)) << b;

    msvc::MsvcProbe both = vsOnly;
    both.sdk = sdkOnly.sdk;
    expect(bool(both.usable()));
    expect(msvc::describe_msvc_probe(both).empty());
}};
