#include <gtest/gtest.h>

import std;
import mcpp.manifest;
import mcpp.build.resources;

namespace res = mcpp::build::resources;
namespace fs  = std::filesystem;

namespace {

mcpp::manifest::Package sample_package() {
    mcpp::manifest::Package p;
    p.name        = "myapp";
    p.version     = "0.2.0";
    p.description = "My application";
    p.license     = "MIT";
    p.authors     = {"Acme"};
    return p;
}

// A scratch .rc on disk; scan_rc reads files, so the tests write them.
struct TempDir {
    fs::path path;
    TempDir() {
        auto base = fs::temp_directory_path() /
            std::format("mcpp-rc-test-{}", reinterpret_cast<std::uintptr_t>(this));
        fs::create_directories(base);
        path = base;
    }
    ~TempDir() { std::error_code ec; fs::remove_all(path, ec); }
    fs::path write(std::string_view name, std::string_view body) const {
        auto p = path / name;
        std::ofstream os(p, std::ios::binary);
        os << body;
        return p;
    }
};

} // namespace

// ─── Synthesis: the mcpp#365 headline ─────────────────────────────────────
//
// The reported symptom was a VERSIONINFO that llvm-readobj shows and Windows
// cannot read. Root cause: `VS_VERSION_INFO` is a <windows.h> macro (= 1), and
// without it the resource is filed under the STRING name "VS_VERSION_INFO"
// while GetFileVersionInfo looks up ordinal 1. What mcpp generates must
// therefore never spell the macro.

TEST(BuildResources, SynthesizedScriptNamesTheVersionResourceByOrdinal) {
    mcpp::manifest::Resources r;
    auto rc = res::synthesize_rc(sample_package(), r, "myapp.exe", {});
    ASSERT_TRUE(rc) << rc.error();
    EXPECT_NE(rc->find("1 VERSIONINFO"), std::string::npos);
    EXPECT_EQ(rc->find("VS_VERSION_INFO"), std::string::npos)
        << "the macro is only defined when <windows.h> was included; naming it "
           "here is the bug this feature exists to avoid";
}

TEST(BuildResources, FileVersionComesFromThePackageVersion) {
    mcpp::manifest::Resources r;
    auto rc = res::synthesize_rc(sample_package(), r, "myapp.exe", {});
    ASSERT_TRUE(rc);
    EXPECT_NE(rc->find("FILEVERSION    0,2,0,0"), std::string::npos);
    EXPECT_NE(rc->find("PRODUCTVERSION 0,2,0,0"), std::string::npos);
    // The STRING fields keep the version verbatim, so a form the numeric
    // fields cannot hold (a pre-release) is still visible in the properties
    // dialog.
    EXPECT_NE(rc->find("\"FileVersion\", \"0.2.0\""), std::string::npos);
}

TEST(BuildResources, FourSegmentDateVersionsFitTheNumericFields) {
    auto pkg = sample_package();
    pkg.version = "2026.8.7.1";
    mcpp::manifest::Resources r;
    auto rc = res::synthesize_rc(pkg, r, "mcpp.exe", {});
    ASSERT_TRUE(rc) << rc.error();
    EXPECT_NE(rc->find("FILEVERSION    2026,8,7,1"), std::string::npos);
}

TEST(BuildResources, AVersionFieldThatCannotFitIsAnErrorNotAClamp) {
    auto pkg = sample_package();
    pkg.version = "70000.0.0";     // > 65535: FILEVERSION fields are 16-bit
    mcpp::manifest::Resources r;
    auto rc = res::synthesize_rc(pkg, r, "myapp.exe", {});
    ASSERT_FALSE(rc);
    EXPECT_NE(rc.error().find("65535"), std::string::npos);
    // Clamping would put a version in the binary that is not the version that
    // was built — the failure mode is a wrong answer, so it must not be silent.
    EXPECT_NE(rc.error().find("70000"), std::string::npos);
}

TEST(BuildResources, MetadataDefaultsFromPackageAndIsOverridable) {
    mcpp::manifest::Resources r;
    auto rc = res::synthesize_rc(sample_package(), r, "myapp.exe", {});
    ASSERT_TRUE(rc);
    EXPECT_NE(rc->find("\"CompanyName\", \"Acme\""), std::string::npos);
    EXPECT_NE(rc->find("\"ProductName\", \"myapp\""), std::string::npos);
    EXPECT_NE(rc->find("\"FileDescription\", \"My application\""), std::string::npos);
    EXPECT_NE(rc->find("\"OriginalFilename\", \"myapp.exe\""), std::string::npos);

    r.info.company = "Other Co";
    r.info.product = "Renamed";
    auto rc2 = res::synthesize_rc(sample_package(), r, "myapp.exe", {});
    ASSERT_TRUE(rc2);
    EXPECT_NE(rc2->find("\"CompanyName\", \"Other Co\""), std::string::npos);
    EXPECT_NE(rc2->find("\"ProductName\", \"Renamed\""), std::string::npos);
}

TEST(BuildResources, GeneratedTextIsAsciiSoItDoesNotDependOnTheCodepageFlag) {
    // A UTF-8 codepage is passed to the rc tool for the USER's metadata, but
    // text mcpp writes itself must not need it: an em dash in the default
    // copyright line failed llvm-rc outright ("Non-ASCII 8-bit codepoint").
    mcpp::manifest::Resources r;
    auto rc = res::synthesize_rc(sample_package(), r, "myapp.exe", {});
    ASSERT_TRUE(rc);
    auto generated = rc->substr(0, rc->find("VALUE \"CompanyName\""));
    for (unsigned char c : generated)
        EXPECT_LT(c, 0x80u) << "generated scaffolding must stay ASCII";
    EXPECT_NE(rc->find("\"LegalCopyright\", \"(C) Acme - MIT\""), std::string::npos);
}

TEST(BuildResources, IconIsEmittedAtOrdinalOne) {
    mcpp::manifest::Resources r;
    r.icon = "assets/app.ico";
    auto rc = res::synthesize_rc(sample_package(), r, "myapp.exe",
                                 "/proj/assets/app.ico");
    ASSERT_TRUE(rc);
    // Explorer shows the lowest-numbered icon group.
    EXPECT_NE(rc->find("1 ICON \"/proj/assets/app.ico\""), std::string::npos);
}

// The 3-row rule: author-supplied scripts own the resource ID space, so mcpp
// does not add a second VERSIONINFO behind their back — unless asked.
TEST(BuildResources, VersionInfoSynthesisFollowsTheThreeRowRule) {
    mcpp::manifest::Resources r;
    EXPECT_TRUE(r.synthesize_version_info());          // nothing declared

    r.files = {"res/app.rc"};
    EXPECT_FALSE(r.synthesize_version_info());         // author took over

    r.versionInfo = true;
    EXPECT_TRUE(r.synthesize_version_info());          // explicit opt-in wins

    r.versionInfo = false;
    EXPECT_FALSE(r.synthesize_version_info());
    r.files.clear();
    EXPECT_FALSE(r.synthesize_version_info());         // explicit opt-out wins
}

// ─── Scanning an author-written .rc ───────────────────────────────────────

TEST(BuildResources, ScanCollectsQuotedIncludesAndDataFiles) {
    TempDir d;
    auto rc = d.write("app.rc", R"(#include "ids.h"
#include <windows.h>
1 ICON "assets/app.ico"
2 RCDATA "blob.bin"
IDR_MANIFEST 24 "app.manifest"
STRINGTABLE
BEGIN
  1 "hello"
END
)");
    auto s = res::scan_rc(rc);
    auto has = [&](std::string_view leaf) {
        return std::any_of(s.inputs.begin(), s.inputs.end(),
            [&](const fs::path& p){ return p.filename() == leaf; });
    };
    EXPECT_TRUE(has("ids.h"));
    EXPECT_TRUE(has("app.ico"));
    EXPECT_TRUE(has("blob.bin"));
    // Angled includes belong to the toolchain: immutable for the life of a
    // build directory and already folded into the fingerprint.
    EXPECT_FALSE(has("windows.h"));
    // STRINGTABLE carries its data inline — nothing to track.
    EXPECT_EQ(s.inputs.size(), 3u);
}

TEST(BuildResources, ScanNamesWhatItCouldNotResolve) {
    TempDir d;
    auto rc = d.write("app.rc", "1 ICON APP_ICON\n");
    auto s = res::scan_rc(rc);
    // A macro hides the file name. Reporting the gap is the whole point: a
    // silently untracked input leaves a stale resource in a shipped binary.
    ASSERT_EQ(s.gaps.size(), 1u);
    EXPECT_NE(s.gaps[0].find("APP_ICON"), std::string::npos);
    EXPECT_TRUE(s.inputs.empty());
}

TEST(BuildResources, ScanFlagsAVersionResourceWindowsWillNotFind) {
    TempDir d;
    auto rc = d.write("bad.rc", R"(VS_VERSION_INFO VERSIONINFO
 FILEVERSION 0,2,0,0
BEGIN
END
)");
    auto s = res::scan_rc(rc);
    EXPECT_TRUE(s.versionInfoNamedByString);
    EXPECT_EQ(s.versionInfoName, "VS_VERSION_INFO");
}

// ─── Splitting a Windows environment list ─────────────────────────────────

TEST(BuildResources, EnvListSplitsOnSemicolonsOnly) {
    // The rc-tool search walks the toolchain's PATH override, which the MSVC
    // backend builds with `;` from real Windows paths. Splitting on ":" as well
    // cuts at the DRIVE COLON: `C:\...` becomes `C` plus a current-drive-relative
    // tail, which resolves by accident on the same drive and finds nothing
    // otherwise — so rc.exe (which lives in the SDK bin, never next to cl.exe)
    // became unfindable.
    auto v = res::split_env_list(
        R"(C:\Program Files (x86)\Windows Kits\10\bin\10.0.22621.0\x64;C:\VC\bin\Hostx64\x64)");
    ASSERT_EQ(v.size(), 2u);
    EXPECT_EQ(v[0], R"(C:\Program Files (x86)\Windows Kits\10\bin\10.0.22621.0\x64)");
    EXPECT_EQ(v[1], R"(C:\VC\bin\Hostx64\x64)");

    // Empty entries (a trailing or doubled separator) are dropped rather than
    // becoming a probe of the current directory.
    auto e = res::split_env_list(R"(C:\a;;C:\b;)");
    ASSERT_EQ(e.size(), 2u);
    EXPECT_EQ(e[0], R"(C:\a)");
    EXPECT_EQ(e[1], R"(C:\b)");

    EXPECT_TRUE(res::split_env_list("").empty());
    EXPECT_EQ(res::split_env_list(R"(C:\only)").size(), 1u);
}

TEST(BuildResources, ScanStaysQuietWhenTheMacroIsActuallyDefined) {
    TempDir d;
    // Either of these makes VS_VERSION_INFO real, so there is nothing to warn
    // about — the lint must not cry wolf at correct scripts.
    auto viaInclude = d.write("ok1.rc", "#include <windows.h>\n"
                                        "VS_VERSION_INFO VERSIONINFO\nBEGIN\nEND\n");
    EXPECT_FALSE(res::scan_rc(viaInclude).versionInfoNamedByString);

    auto viaDefine = d.write("ok2.rc", "#define VS_VERSION_INFO 1\n"
                                       "VS_VERSION_INFO VERSIONINFO\nBEGIN\nEND\n");
    EXPECT_FALSE(res::scan_rc(viaDefine).versionInfoNamedByString);

    auto literal = d.write("ok3.rc", "1 VERSIONINFO\nBEGIN\nEND\n");
    EXPECT_FALSE(res::scan_rc(literal).versionInfoNamedByString);
}
