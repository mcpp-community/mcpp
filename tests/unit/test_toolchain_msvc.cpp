#include <gtest/gtest.h>
#include <cstdlib>       // setenv / _putenv_s — not part of `import std;`

import std;
import mcpp.toolchain.model;
import mcpp.toolchain.msvc;
import mcpp.toolchain.registry;
import mcpp.toolchain.dialect;

using namespace mcpp::toolchain;

// ─── parse_cl_banner ─────────────────────────────────────────────────────

TEST(MsvcBanner, ParsesEnglishBanner) {
    auto r = msvc::parse_cl_banner(
        "Microsoft (R) C/C++ Optimizing Compiler Version 19.44.35211 for x64\n"
        "Copyright (C) Microsoft Corporation.  All rights reserved.\n"
        "\n"
        "usage: cl [ option... ] filename... [ /link linkoption... ]\n");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->first, "19.44.35211");
    EXPECT_EQ(r->second, "x64");
}

TEST(MsvcBanner, ParsesLocalizedBannerByTokens) {
    // Chinese VS reorders the sentence; only the tokens are stable.
    auto r = msvc::parse_cl_banner(
        "用于 x64 的 Microsoft (R) C/C++ 优化编译器 19.44.35211 版\n"
        "版权所有(C) Microsoft Corporation。保留所有权利。\n");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->first, "19.44.35211");
    EXPECT_EQ(r->second, "x64");
}

TEST(MsvcBanner, ParsesArm64AndFourComponentVersions) {
    auto r = msvc::parse_cl_banner(
        "Microsoft (R) C/C++ Optimizing Compiler Version 19.29.30133.0 for ARM64\n");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->first, "19.29.30133.0");
    EXPECT_EQ(r->second, "arm64");
}

TEST(MsvcBanner, RejectsGarbage) {
    EXPECT_FALSE(msvc::parse_cl_banner("").has_value());
    EXPECT_FALSE(msvc::parse_cl_banner("bash: cl: command not found").has_value());
    // A bare two-component number is not a cl version.
    EXPECT_FALSE(msvc::parse_cl_banner("something 1.2 else").has_value());
}

TEST(MsvcBanner, TripleForArch) {
    EXPECT_EQ(msvc::triple_for_arch("x64"),   "x86_64-pc-windows-msvc");
    EXPECT_EQ(msvc::triple_for_arch("x86"),   "i686-pc-windows-msvc");
    EXPECT_EQ(msvc::triple_for_arch("arm64"), "aarch64-pc-windows-msvc");
    // Unknown/empty arch falls back to the x64 triple.
    EXPECT_EQ(msvc::triple_for_arch(""),      "x86_64-pc-windows-msvc");
}

// ─── install guidance ────────────────────────────────────────────────────

TEST(MsvcGuidance, OffersBothOrigins) {
    auto g = msvc::install_guidance();
    ASSERT_FALSE(g.empty());
    // The managed origin has to be reachable from the message a user sees
    // when nothing is installed — otherwise "mcpp can install a toolset" is
    // true and undiscoverable at the same time.
    EXPECT_NE(g.find("mcpp toolchain install msvc"), std::string::npos) << g;
    EXPECT_NE(g.find("[toolchain]"), std::string::npos) << g;
    // …and the system origin stays offered, with its own route.
    EXPECT_NE(g.find("winget"), std::string::npos) << g;
    EXPECT_NE(g.find("msvc@system"), std::string::npos) << g;
    EXPECT_NE(g.find("mcpp toolchain default msvc"), std::string::npos) << g;
}

// ─── spec layer: the VERSION axis decides the origin ─────────────────────

TEST(MsvcSpec, SystemOriginIsTheUnversionedSpec) {
    for (auto s : {"msvc", "msvc@system"}) {
        auto spec = parse_toolchain_spec(s);
        ASSERT_TRUE(spec.has_value()) << s;
        EXPECT_TRUE(is_system_toolchain(*spec)) << s;
    }
    auto gcc = parse_toolchain_spec("gcc@16.1.0");
    ASSERT_TRUE(gcc.has_value());
    EXPECT_FALSE(is_system_toolchain(*gcc));
}

TEST(MsvcSpec, ToolsetVersionIsAManagedPayloadNotASystemSpec) {
    // The defect this closes: EVERY msvc spec used to be a system spec, so a
    // manifest could name a toolset and silently get whatever the machine
    // had. A version means a payload, exactly like gcc@16.1.0.
    for (auto s : {"msvc@14.44.35207", "msvc@14.52.36629"}) {
        auto spec = parse_toolchain_spec(s);
        ASSERT_TRUE(spec.has_value()) << s;
        EXPECT_FALSE(is_system_toolchain(*spec)) << s;
        auto pkg = to_xim_package(*spec);
        EXPECT_EQ(pkg.ximName, "msvc") << s;
        EXPECT_EQ(pkg.ximVersion, std::string(s).substr(5)) << s;
    }
}

TEST(MsvcSpec, StableDefaultMatchesAnyDetectedVersion) {
    // The persisted default is always the stable "msvc@system" — it matches
    // whatever concrete version detection reports (family-level match).
    auto def = parse_toolchain_spec("msvc@system");
    ASSERT_TRUE(def.has_value());
    PayloadIdentity msvcId{ Family::Msvc, {} };
    EXPECT_TRUE(spec_matches_payload(*def, msvcId, "19.44.35211"));
    EXPECT_TRUE(spec_matches_payload(*def, msvcId, "19.29.30133"));
    PayloadIdentity gccId{ Family::Gcc, {} };
    EXPECT_FALSE(spec_matches_payload(*def, gccId, "16.1.0"));
    auto gccDef = parse_toolchain_spec("gcc@16.1.0");
    ASSERT_TRUE(gccDef.has_value());
    EXPECT_FALSE(spec_matches_payload(*gccDef, msvcId, "19.44.35211"));
}

TEST(MsvcSpec, PinnedToolsetMatchesOnlyItsOwnPayload) {
    // The other half of the family-level match above: once a version is
    // named, it has to be compared. A pin that matched any payload would be
    // the same silent divergence wearing a version number.
    auto pinned = parse_toolchain_spec("msvc@14.52.36629");
    ASSERT_TRUE(pinned.has_value());
    PayloadIdentity msvcId{ Family::Msvc, {} };
    EXPECT_TRUE(spec_matches_payload(*pinned, msvcId, "14.52.36629"));
    EXPECT_FALSE(spec_matches_payload(*pinned, msvcId, "14.44.35207"));
}

TEST(MsvcSpec, PayloadDirectoryIsIdentifiedAsMsvc) {
    // Installed payloads are listed by walking `xim-x-<name>` directories.
    // Without this row an install would succeed and then be invisible.
    auto id = identify_xim_payload("msvc");
    ASSERT_TRUE(id.has_value());
    EXPECT_EQ(id->family, Family::Msvc);
    EXPECT_TRUE(id->target.empty());   // host-target, like gcc and llvm
}

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

void put_env(const char* name, const std::optional<std::string>& value) {
#if defined(_WIN32)
    // An empty value REMOVES the variable on Windows, which is what nullopt
    // has to mean here.
    ::_putenv_s(name, value ? value->c_str() : "");
#else
    if (value) ::setenv(name, value->c_str(), 1);
    else       ::unsetenv(name);
#endif
}

// RAII for an environment variable. nullopt = unset it.
//
// Unsetting matters as much as setting: these tests run on a Windows CI
// runner that may have vcvars exported, and an ambient WindowsSdkDir would
// otherwise answer before the fixture ever got a turn — a green test proving
// nothing about the code under it.
struct ScopedEnv {
    const char* name;
    std::optional<std::string> old;
    ScopedEnv(const char* n, std::optional<std::string> value) : name(n) {
        if (auto* v = std::getenv(name)) old = v;
        put_env(name, value);
    }
    ~ScopedEnv() { put_env(name, old); }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;
};

// Every SDK test starts from "nothing declared", then declares what it means
// to test.
struct NoSdkEnv {
    ScopedEnv dir{"WindowsSdkDir", std::nullopt};
    ScopedEnv ver{"WindowsSdkVersion", std::nullopt};
};

} // namespace

TEST(MsvcManaged, ResolvesTheDeclaredToolsetAndNotItsNeighbour) {
    FakeToolset t{"pick"};
    t.add_toolset("14.44.35207");
    t.add_toolset("14.52.36629");

    // Both present, and the OLDER one is asked for. A "latest" scan would
    // answer 14.52 — the exact substitution the managed origin exists to
    // prevent.
    auto older = msvc::installation_at(t.root, "14.44.35207");
    ASSERT_TRUE(older.has_value());
    EXPECT_EQ(older->toolsVersion, "14.44.35207");
    EXPECT_TRUE(older->hasStdModules);
    EXPECT_EQ(older->clPath.filename(), "cl.exe");
    EXPECT_NE(older->clPath.string().find("14.44.35207"), std::string::npos);

    auto newer = msvc::installation_at(t.root, "14.52.36629");
    ASSERT_TRUE(newer.has_value());
    EXPECT_EQ(newer->toolsVersion, "14.52.36629");
}

TEST(MsvcManaged, TheVersionDirIsTheRootNotItsLoneSubdirectory) {
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
    ASSERT_TRUE(ok.has_value()) << "the version directory must be the root";

    EXPECT_FALSE(msvc::installation_at(t.root / "VC", "14.44.35207").has_value())
        << "a caller handing in the VC subdir is passing the wrong thing, and "
           "must be told so rather than quietly rescued";
}

TEST(MsvcManaged, AbsentToolsetIsNulloptNotASubstitute) {
    FakeToolset t{"absent"};
    t.add_toolset("14.44.35207");
    // Nothing is "close enough": a pin that silently fell back would report
    // success while building with a compiler nobody asked for.
    EXPECT_FALSE(msvc::installation_at(t.root, "14.52.36629").has_value());
    EXPECT_FALSE(msvc::installation_at(t.root, "14.44").has_value());
    EXPECT_FALSE(msvc::installation_at(t.root, "").has_value());
}

TEST(MsvcManaged, VersionFallsBackToTheDeclaredOneWhenTheBannerCannotBeRead) {
    // A fixture cl.exe produces no banner (and on Windows a real one would).
    // display_version() must still name the toolset rather than an empty
    // string — the declared version is a fact even when the probe fails.
    FakeToolset t{"banner"};
    t.add_toolset("14.52.36629");
    auto inst = msvc::installation_at(t.root, "14.52.36629");
    ASSERT_TRUE(inst.has_value());
    EXPECT_EQ(inst->display_version(), "14.52.36629");
}

TEST(MsvcManaged, PayloadFrontendFindsClWhereMsvcActuallyKeepsIt) {
    // The defect this pins: `toolchain list` asked `toolchain_frontend(root /
    // "bin", …)`, got nothing, and skipped the row -- so an msvc toolset
    // installed correctly and then did not appear anywhere. Three places need
    // to know that cl.exe is four levels deeper than `bin/`; two knew.
    FakeToolset t{"frontend"};
    t.add_toolset("14.44.35207");

    auto spec = parse_toolchain_spec("msvc@14.44.35207");
    ASSERT_TRUE(spec.has_value());
    auto pkg = to_xim_package(*spec);

    auto found = payload_frontend(t.root, pkg, Family::Msvc);
    ASSERT_FALSE(found.empty()) << "payload_frontend found no cl.exe under " << t.root;
    EXPECT_EQ(found.filename(), "cl.exe");

    // The `bin/`-shaped question is the one that used to be asked, and it
    // still answers nothing here — which is exactly why it was the wrong
    // question rather than a broken implementation.
    EXPECT_TRUE(toolchain_frontend(t.root / "bin", pkg).empty());

    // A root with no toolset at that version stays empty rather than
    // returning a path that does not exist.
    EXPECT_TRUE(payload_frontend(t.root,
                    to_xim_package(*parse_toolchain_spec("msvc@14.52.36629")),
                    Family::Msvc).empty());
}

// ─── Windows SDK discovery ───────────────────────────────────────────────

TEST(MsvcSdk, WindowsSdkDirBeatsTheHardcodedPaths) {
    NoSdkEnv clean;
    FakeToolset t{"sdkenv"};
    t.add_sdk("10.0.26100.0");
    ScopedEnv dir{"WindowsSdkDir", t.root.string()};
    auto sdk = msvc::find_windows_sdk();
    ASSERT_TRUE(sdk.has_value());
    EXPECT_EQ(sdk->root, t.root);
    EXPECT_EQ(sdk->version, "10.0.26100.0");
}

TEST(MsvcSdk, WindowsSdkVersionSelectsAmongInstalledOnes) {
    NoSdkEnv clean;
    FakeToolset t{"sdkver"};
    t.add_sdk("10.0.22621.0");
    t.add_sdk("10.0.26100.0");
    ScopedEnv dir{"WindowsSdkDir", t.root.string()};
    {   // vcvars exports it with a trailing backslash; that is not part of
        // the directory name, and comparing it raw finds nothing.
        ScopedEnv ver{"WindowsSdkVersion", "10.0.22621.0\\"};
        auto sdk = msvc::find_windows_sdk();
        ASSERT_TRUE(sdk.has_value());
        EXPECT_EQ(sdk->version, "10.0.22621.0");
    }
    // Unset again: highest usable wins.
    auto sdk = msvc::find_windows_sdk();
    ASSERT_TRUE(sdk.has_value());
    EXPECT_EQ(sdk->version, "10.0.26100.0");
}

TEST(MsvcSdk, ExtraRootsCoverTheManagedPayload) {
    NoSdkEnv clean;
    FakeToolset t{"sdkextra"};
    t.add_sdk("10.0.26100.0");
    // Nothing declared — the payload root is the only way the SDK can be
    // found, which is exactly the managed toolset's situation.
    std::array roots{t.root};
    auto sdk = msvc::find_windows_sdk(roots);
    ASSERT_TRUE(sdk.has_value());
    EXPECT_EQ(sdk->root, t.root);
}

TEST(MsvcSdk, IncompleteSdkRootIsNotAnAnswer) {
    // Include/<ver>/ exists but carries no ucrt/corecrt.h: a half-installed
    // SDK must read as absent, not as a usable one that fails later inside
    // the compiler. Checked through extraRoots so the assertion cannot be
    // satisfied by a real SDK on the machine.
    NoSdkEnv clean;
    FakeToolset t{"sdkpartial"};
    std::filesystem::create_directories(t.root / "Include" / "10.0.26100.0" / "um");
    std::array roots{t.root};
    auto sdk = msvc::find_windows_sdk(roots);
    if (sdk) EXPECT_NE(sdk->root, t.root) << "an SDK-less root was accepted";
}

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

TEST(MsvcRedist, FoundFromTheCompilerPathAlone) {
    // Nothing configured, no version derived: the compiler's own location
    // says which toolset this is, and the toolset carries its runtime.
    FakeRedist t{"14.44.35207", "14.44.35112"};
    auto d = msvc::vc_redist_dir(t.clPath, "x64");
    ASSERT_FALSE(d.empty()) << "the toolset's redistributable CRT was not found";
    EXPECT_TRUE(std::filesystem::exists(d / "vcruntime140.dll"));
}

TEST(MsvcRedist, TheRedistVersionIsNotTheToolsVersion) {
    // 14.44.35207 (tools) vs 14.44.35112 (redist) is what MSVC actually
    // ships. Deriving one from the other finds nothing — this is why the
    // directory is searched rather than composed.
    FakeRedist t{"14.44.35207", "14.44.35112"};
    auto d = msvc::vc_redist_dir(t.clPath, "x64");
    ASSERT_FALSE(d.empty());
    EXPECT_NE(d.string().find("14.44.35112"), std::string::npos)
        << "did not land in the redist version directory: " << d.string();
}

TEST(MsvcRedist, NewestRedistWins) {
    FakeRedist t{"14.44.35207", "14.40.00000"};
    t.add("14.44.35112", "x64", "Microsoft.VC143.CRT", "vcruntime140.dll");
    auto d = msvc::vc_redist_dir(t.clPath, "x64");
    ASSERT_FALSE(d.empty());
    EXPECT_NE(d.string().find("14.44.35112"), std::string::npos)
        << "older redist won: " << d.string();
}

TEST(MsvcRedist, TheDebugCrtIsNeverReturned) {
    // debug_nonredist may NOT be redistributed. Returning it would put
    // vcruntime140d.dll on PATH and, later, into a shipped artifact.
    FakeRedist t{"14.44.35207", "14.44.35112"};
    t.add_debug("14.99.99999");          // newer, and must still lose
    auto d = msvc::vc_redist_dir(t.clPath, "x64");
    ASSERT_FALSE(d.empty());
    EXPECT_EQ(d.string().find("debug_nonredist"), std::string::npos)
        << "returned the non-redistributable debug CRT: " << d.string();
}

TEST(MsvcRedist, AToolsetWithoutARedistIsNotAnError) {
    // msvc@system on a machine whose VS install omits the redist component,
    // for instance. Empty means "nothing to add to PATH", not a failure.
    FakeRedist t{"14.44.35207", "14.44.35112"};
    std::error_code ec;
    std::filesystem::remove_all(t.root / "VC" / "Redist", ec);
    EXPECT_TRUE(msvc::vc_redist_dir(t.clPath, "x64").empty());
}

TEST(MsvcSdk, HeadersWithoutImportLibsIsNotAnAnswer) {
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
    if (sdk) EXPECT_NE(sdk->root, t.root)
        << "a headers-only SDK was accepted; the link would fail on kernel32.lib";
}

TEST(MsvcSdk, ACompleteRootIsStillAccepted) {
    // The other direction, so the check above cannot be satisfied by
    // rejecting everything.
    NoSdkEnv clean;
    FakeToolset t{"sdkcomplete"};
    t.add_sdk("10.0.26100.0");
    std::array roots{t.root};
    auto sdk = msvc::find_windows_sdk(roots);
    ASSERT_TRUE(sdk) << "a complete SDK root was rejected";
    EXPECT_EQ(sdk->root, t.root);
    EXPECT_EQ(sdk->version, "10.0.26100.0");
}

TEST(MsvcSdk, APartialRootYieldsToACompleteOne) {
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
    ASSERT_TRUE(sdk);
    EXPECT_EQ(sdk->root, complete.root)
        << "the partial root won on version; it cannot link";
}

TEST(MsvcSdk, DeclaredRootOutranksTheManagedOne) {
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
    ASSERT_TRUE(sdk.has_value());
    EXPECT_EQ(sdk->root, declared.root);
    EXPECT_EQ(sdk->version, "10.0.22621.0");
}

TEST(MsvcSdk, SiblingSdkRootsAreEmptyForACompilerOutsideAnyStore) {
    // A system cl.exe is not in an xlings store, so there is nothing to
    // offer — and offering the wrong thing would be worse than nothing.
    EXPECT_TRUE(msvc::sibling_sdk_roots(
        "C:/Program Files/Microsoft Visual Studio/18/Enterprise/VC/Tools/"
        "MSVC/14.51.36231/bin/Hostx64/x64/cl.exe").empty());
}

// ─── model traits ────────────────────────────────────────────────────────

TEST(MsvcModel, BmiTraitsUseIfc) {
    Toolchain tc;
    tc.compiler = CompilerId::MSVC;
    auto t = bmi_traits(tc);
    EXPECT_EQ(t.bmiDir, "ifc.cache");
    EXPECT_EQ(t.bmiExt, ".ifc");
    EXPECT_TRUE(t.needsExplicitModuleOutput);
    EXPECT_FALSE(t.scanNeedsFModules);
}

// ─── std module minimum standard level ───────────────────────────────────
//
// microsoft/STL#3945 ("Supporting `import std;` in C++20") was fixed by
// STL#3977, first shipping in VS 2022 17.8 = cl 19.38. Older STLs still block
// C++20 and would fail inside std.ixx; they answer 23 so the build layer can
// refuse with an actionable message instead.
// See .agents/docs/2026-07-31-cpp20-standard-support-design.md §2.3.

TEST(MsvcStdModule, MinLevelFollowsStlUnblockVersion) {
    auto tc_of = [](std::string ver) {
        Toolchain tc;
        tc.compiler = CompilerId::MSVC;
        tc.version = std::move(ver);
        return tc;
    };
    // VS 2022 17.8 and newer: C++20 is allowed.
    EXPECT_EQ(msvc::std_module_min_level(tc_of("19.38.33130")), 20);
    EXPECT_EQ(msvc::std_module_min_level(tc_of("19.44.35211")), 20);
    EXPECT_EQ(msvc::std_module_min_level(tc_of("20.0")), 20);
    // Older toolsets keep the C++23 floor.
    EXPECT_EQ(msvc::std_module_min_level(tc_of("19.37.32825")), 23);
    EXPECT_EQ(msvc::std_module_min_level(tc_of("19.29.30153")), 23);
    // Unparseable banner version: stay strict rather than guess.
    EXPECT_EQ(msvc::std_module_min_level(tc_of("")), 23);
    EXPECT_EQ(msvc::std_module_min_level(tc_of("unknown")), 23);
}

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
TEST(ToolchainMsvc, StdModuleCarriesTheProjectCrtModel) {
    Toolchain tc;
    tc.compiler   = CompilerId::MSVC;
    tc.binaryPath = "C:/vc/bin/cl.exe";
    tc.stdModuleSource  = "C:/vc/modules/std.ixx";
    tc.stdCompatSource  = "C:/vc/modules/std.compat.ixx";

    const std::filesystem::path cache = "C:/cache/std/key";

    for (std::string_view crt : {std::string_view("/MT"), std::string_view("/MD")}) {
        auto cmds = msvc::std_module_build_commands(tc, cache, "/std:c++23", crt);
        ASSERT_EQ(cmds.size(), 1u);
        EXPECT_NE(cmds[0].find(crt), std::string::npos)
            << crt << " missing from the std build command: " << cmds[0];

        auto compat = msvc::std_compat_build_commands(tc, cache, "/std:c++23", crt);
        ASSERT_EQ(compat.size(), 1u);
        EXPECT_NE(compat[0].find(crt), std::string::npos)
            << crt << " missing from the std.compat build command: " << compat[0];
    }

    // The two models must produce DIFFERENT command strings. Equal strings would
    // mean equal cache keys, i.e. the exact silent divergence this fixes.
    EXPECT_NE(msvc::std_module_build_commands(tc, cache, "/std:c++23", "/MT")[0],
              msvc::std_module_build_commands(tc, cache, "/std:c++23", "/MD")[0]);

    // Empty keeps cl's own default, so non-MSVC callers are unaffected.
    auto bare = msvc::std_module_build_commands(tc, cache, "/std:c++23");
    EXPECT_EQ(bare[0].find("/MT"), std::string::npos);
    EXPECT_EQ(bare[0].find("/MD"), std::string::npos);
}

// The mapping linkage -> CRT model lives in ONE place. It was derived twice —
// flags.cppm for the project's TUs, and cl's default for the std module — which
// is how they disagreed.
TEST(ToolchainMsvc, CrtFlagHasASingleDerivation) {
    Toolchain cl;
    cl.compiler = CompilerId::MSVC;
    const auto& msvcDialect = dialect_for(cl);
    EXPECT_EQ(msvc_crt_flag(msvcDialect, /*staticCrt=*/true),  "/MT");
    EXPECT_EQ(msvc_crt_flag(msvcDialect, /*staticCrt=*/false), "/MD");

    // GNU has no counterpart; the helper must yield nothing rather than invent
    // a flag that would be passed to gcc.
    Toolchain gcc;
    gcc.compiler = CompilerId::GCC;
    const auto& gnu = dialect_for(gcc);
    EXPECT_TRUE(msvc_crt_flag(gnu, false).empty());
}

// Both manifest keys reach the SAME physical switch, because on the MSVC ABI
// they are the same switch. `cxx_runtime = "self-contained"` used to report
// "not implemented" while `linkage = "static"` quietly did the very thing it
// said was unimplemented.
TEST(ToolchainMsvc, BothKeysSelectTheStaticCrt) {
    EXPECT_TRUE(msvc_wants_static_crt("static", ""));
    EXPECT_TRUE(msvc_wants_static_crt("", "self-contained"));
    EXPECT_TRUE(msvc_wants_static_crt("static", "self-contained"));

    // Default: neither written down. This has to stay /MD — most roles
    // DEFAULT to the self-contained contract, so keying off the resolved
    // contract instead of the written key would flip every Windows build to
    // /MT and split the CRT across a project's dependencies.
    EXPECT_FALSE(msvc_wants_static_crt("", ""));
    EXPECT_FALSE(msvc_wants_static_crt("dynamic", ""));
    EXPECT_FALSE(msvc_wants_static_crt("", "host-coupled"));
    EXPECT_FALSE(msvc_wants_static_crt("", "toolchain-coupled"));
}

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

TEST(MsvcSdkOrigin, ACompilerInAStoreIsManagedAndOneOutsideIsNot) {
    FakeStore store;
    auto cl = store.add_toolset("14.44.35207");
    EXPECT_EQ(msvc::origin_of(cl), Origin::Managed);
    EXPECT_EQ(msvc::origin_of(
        "C:/Program Files/Microsoft Visual Studio/18/Enterprise/VC/Tools/"
        "MSVC/14.51.36231/bin/Hostx64/x64/cl.exe"), Origin::SystemMsvc);
}

TEST(MsvcSdkOrigin, AManagedToolsetTakesTheSdkFromItsOwnStore) {
    NoSdkEnv clean;
    FakeStore store;
    auto cl = store.add_toolset("14.44.35207");
    store.add_sdk("10.0.26100.0");

    auto choice = msvc::resolve_sdk_for(cl);
    ASSERT_TRUE(choice.sdk.has_value())
        << "the toolset's own SDK payload was not found";
    EXPECT_EQ(choice.origin, Origin::Managed);
    EXPECT_EQ(choice.sdk->version, "10.0.26100.0");
    EXPECT_TRUE(choice.note.empty()) << choice.note;
}

// THE CRITERION THIS AXIS EXISTS FOR. An environment that can overwrite the
// choice means the manifest did not pin anything — it only expressed a
// preference that the machine gets to overrule, silently.
//
// This is the design doc's §6 acceptance test, as a unit test: point
// WindowsSdkDir somewhere else entirely and the build must still use the
// payload's SDK, and must SAY that the variable was ignored.
TEST(MsvcSdkOrigin, WindowsSdkDirCannotOverrideAPinnedToolsetsSdk) {
    NoSdkEnv clean;
    FakeStore store;
    auto cl = store.add_toolset("14.44.35207");
    store.add_sdk("10.0.26100.0");

    FakeToolset elsewhere{"sdk-elsewhere"};
    elsewhere.add_sdk("10.0.22621.0");
    ScopedEnv dir{"WindowsSdkDir", elsewhere.root.string()};

    auto choice = msvc::resolve_sdk_for(cl);
    ASSERT_TRUE(choice.sdk.has_value());
    EXPECT_EQ(choice.sdk->version, "10.0.26100.0")
        << "the environment overrode a pinned toolset's SDK; the pin is not one";
    EXPECT_NE(choice.sdk->root, elsewhere.root);
    EXPECT_NE(choice.note.find("WindowsSdkDir"), std::string::npos)
        << "an ignored override that says nothing is indistinguishable from "
           "one that was never set: " << choice.note;
}

TEST(MsvcSdkOrigin, WindowsSdkVersionDoesNotPickAmongPayloadsEither) {
    // Same override wearing a smaller hat: naming a version is still the
    // environment choosing which headers a pinned build compiles against.
    NoSdkEnv clean;
    FakeStore store;
    auto cl = store.add_toolset("14.44.35207");
    store.add_sdk("10.0.22621.0");
    store.add_sdk("10.0.26100.0");
    ScopedEnv ver{"WindowsSdkVersion", "10.0.22621.0\\"};

    auto choice = msvc::resolve_sdk_for(cl);
    ASSERT_TRUE(choice.sdk.has_value());
    EXPECT_EQ(choice.sdk->version, "10.0.26100.0")
        << "WindowsSdkVersion selected among the store's payloads";
    EXPECT_FALSE(choice.note.empty());
}

TEST(MsvcSdkOrigin, AManagedToolsetWithNoSdkPayloadFallsBackAndSaysSo) {
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
    ASSERT_TRUE(choice.sdk.has_value());
    EXPECT_EQ(choice.sdk->root, machine.root);
    EXPECT_NE(choice.note.find("machine"), std::string::npos) << choice.note;
}

TEST(MsvcSdkOrigin, ASystemToolsetKeepsTheDeclaredSearchChain) {
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
    EXPECT_EQ(choice.origin, Origin::SystemMsvc);
    ASSERT_TRUE(choice.sdk.has_value());
    EXPECT_EQ(choice.sdk->root, declared.root);
    EXPECT_TRUE(choice.note.empty())
        << "nothing was ignored, so there is nothing to report: " << choice.note;
}
