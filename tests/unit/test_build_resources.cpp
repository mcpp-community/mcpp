#include <gtest/gtest.h>

import std;
import mcpp.manifest;
import mcpp.build.resources;
import mcpp.platform;
import mcpp.toolchain.detect;

namespace res = mcpp::build::resources;
namespace fs  = std::filesystem;

TEST(BuildResources, CoffTargetFollowsTheTargetInsteadOfTheHost) {
    const res::RcTool llvm{"/selected/bin/llvm-windres.exe", "gnu", true};
    EXPECT_EQ(res::coff_target_flag(llvm, "i686-windows-msvc"),
              "--target=i686-pc-windows-msvc");
    EXPECT_EQ(res::coff_target_flag(llvm, "x86_64-windows-msvc"),
              "--target=x86_64-pc-windows-msvc");
    EXPECT_EQ(res::coff_target_flag(llvm, "aarch64-windows-msvc"),
              "--target=aarch64-pc-windows-msvc");
    const res::RcTool gnu{"/selected/bin/windres", "gnu"};
    for (const auto arch : {"x86", "i386", "i486", "i586", "i686"}) {
        EXPECT_EQ(res::coff_target_flag(gnu, std::string(arch) + "-windows-gnu"),
                  "--target=pe-i386") << arch;
    }
    EXPECT_EQ(res::coff_target_flag(llvm, "i386-windows-msvc"),
              "--target=i386-pc-windows-msvc");
    // MSVC's spelling reaches LLVM as the triple LLVM accepts (#776 review).
    EXPECT_EQ(res::coff_target_flag(llvm, "x86-windows-msvc"),
              "--target=i686-pc-windows-msvc");
    EXPECT_EQ(res::coff_target_flag(gnu, "x86_64-windows-gnu"), "--target=pe-x86-64");
    EXPECT_TRUE(res::coff_target_flag({"rc.exe", "msvc"}, "i686-windows-msvc").empty());
    EXPECT_TRUE(res::coff_target_flag({"llvm-rc.exe", "msvc"}, "x86_64-windows-msvc").empty());
}

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

// ─── Which windres: decided where the tool is found ───────────────────────

TEST(BuildResources, LlvmWindresIsRecognisedThroughTheSymlinkThatNamesIt) {
    if constexpr (mcpp::platform::is_windows)
        GTEST_SKIP() << "creating a symlink needs a privilege a Windows runner may lack";
    TempDir d;
    const auto real = d.write("llvm-rc", "");
    std::error_code ec;
    fs::create_symlink(real, d.path / "llvm-windres", ec);
    ASSERT_FALSE(ec) << ec.message();
    fs::create_symlink(d.path / "llvm-windres", d.path / "i686-w64-mingw32-windres", ec);
    ASSERT_FALSE(ec) << ec.message();
    const auto gnu = d.write("windres", "");
    EXPECT_TRUE(res::is_llvm_windres(d.path / "llvm-windres"));
    EXPECT_TRUE(res::is_llvm_windres(d.path / "i686-w64-mingw32-windres"));
    EXPECT_FALSE(res::is_llvm_windres(gnu));
    EXPECT_FALSE(res::is_llvm_windres(d.path / "absent-windres"));

    // And `find_rc_tool` records it: the llvm-mingw spelling gets the triple.
    mcpp::toolchain::Toolchain tc;
    tc.binaryPath   = d.path / "clang";
    tc.targetTriple = "i686-windows-gnu";
    auto tool = res::find_rc_tool(tc, "gnu");
    ASSERT_TRUE(tool.has_value());
    EXPECT_EQ(tool->name(), "i686-w64-mingw32-windres");
    EXPECT_TRUE(tool->llvm);
    EXPECT_EQ(res::coff_target_flag(*tool, tc.targetTriple), "--target=i686-w64-windows-gnu");
}

// ─── The build program's manifest object: the command is part of it ───────

TEST(BuildResources, TheManifestObjectIsRemadeWhenItsCommandChanges) {
    if constexpr (mcpp::platform::is_windows)
        GTEST_SKIP() << "the stand-in resource compiler is a POSIX shell script";
    TempDir d;
    // A windres that writes the words it was given into its output.
    const auto tool = d.write("windres",
        "#!/bin/sh\nall=\"$*\"\n"
        "while [ $# -gt 0 ]; do [ \"$1\" = -o ] && out=\"$2\"; shift; done\n"
        "printf '%s\\n' \"$all\" > \"$out\"\n");
    fs::permissions(tool, fs::perms::owner_all);
    mcpp::toolchain::Toolchain tc;
    tc.binaryPath   = d.path / "gcc";
    tc.targetTriple = "x86_64-windows-gnu";
    const auto read = [](const fs::path& p) {
        std::ifstream in(p, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), {});
    };

    auto first = res::compile_utf8_manifest(tc, "gnu", d.path / "out", "m");
    ASSERT_TRUE(first.has_value()) << first.error();
    EXPECT_NE(read(*first).find("--target=pe-x86-64"), std::string::npos);

    // The same command reuses the object: a marker written into it survives.
    { std::ofstream(*first, std::ios::binary) << "reused"; }
    auto again = res::compile_utf8_manifest(tc, "gnu", d.path / "out", "m");
    ASSERT_TRUE(again.has_value()) << again.error();
    EXPECT_EQ(read(*again), "reused");

    // Another target is another command, so the object is made again. Before
    // 2026.10.5.3 only the manifest and the script were compared, and this
    // returned the x86-64 object.
    tc.targetTriple = "i686-windows-gnu";
    auto other = res::compile_utf8_manifest(tc, "gnu", d.path / "out", "m");
    ASSERT_TRUE(other.has_value()) << other.error();
    EXPECT_NE(read(*other).find("--target=pe-i386"), std::string::npos);
}

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
    // An application manifest is a file like the icon (#693): type 24 at the
    // TYPE position, spelt numerically, which no keyword names.
    EXPECT_TRUE(has("app.manifest"));
    EXPECT_TRUE(s.declaresManifest);
    // Angled includes belong to the toolchain: immutable for the life of a
    // build directory and already folded into the fingerprint.
    EXPECT_FALSE(has("windows.h"));
    // STRINGTABLE carries its data inline — nothing to track.
    EXPECT_EQ(s.inputs.size(), 4u);
}

// A quoted include and a resource file are found where the resource compiler
// finds them: beside the script, then in the include directories it is given.
// A name found nowhere stays beside the script, where a file a build action
// produces is ordered before the compile.
TEST(BuildResources, InputsAreResolvedThroughTheIncludeDirectories) {
    TempDir d;
    fs::create_directories(d.path / "include");
    d.write("include/ids.h", "#define APP_ICON 101\n");
    d.write("include/app.ico", "icon");
    d.write("local.ico", "icon");
    auto rc = d.write("app.rc", R"(#include "ids.h"
1 ICON "app.ico"
2 ICON "local.ico"
3 ICON "generated.ico"
)");
    const std::vector<fs::path> includes = {d.path / "include"};
    auto s = res::scan_rc(rc, includes);
    auto at = [&](std::string_view leaf) {
        for (auto const& p : s.inputs) if (p.filename() == leaf) return p;
        return fs::path{};
    };
    EXPECT_EQ(at("ids.h"), d.path / "include" / "ids.h");
    EXPECT_EQ(at("app.ico"), d.path / "include" / "app.ico");
    EXPECT_EQ(at("local.ico"), d.path / "local.ico");
    EXPECT_EQ(at("generated.ico"), d.path / "generated.ico");
}

// The numbers of a VERSIONINFO block contain 24 as well; only the TYPE
// position of a statement declares a manifest.
TEST(BuildResources, ATwentyFourOutsideTheTypePositionIsNotAManifest) {
    TempDir d;
    auto rc = d.write("app.rc", R"(1 VERSIONINFO
 FILEVERSION 24,1,0,0
 PRODUCTVERSION 1,24,0,0
BEGIN
END
)");
    auto s = res::scan_rc(rc);
    EXPECT_FALSE(s.declaresManifest);
    EXPECT_TRUE(s.inputs.empty());
}

// The manifest `windows_code_page = "utf-8"` embeds sits at ordinal 1 of type
// 24, and nothing else is added to a script synthesised for it alone.
TEST(BuildResources, AManifestOnlyScriptCarriesTheManifestAndNothingElse) {
    mcpp::manifest::Resources r;
    r.versionInfo = false;
    auto rc = res::synthesize_rc(sample_package(), r, "tool.exe", {},
                                 fs::path("/b/res/tool.mcpp.manifest"));
    ASSERT_TRUE(rc) << rc.error();
    EXPECT_NE(rc->find("1 24 \"/b/res/tool.mcpp.manifest\""), std::string::npos) << *rc;
    EXPECT_EQ(rc->find("VERSIONINFO"), std::string::npos) << *rc;
    EXPECT_EQ(rc->find("ICON"), std::string::npos) << *rc;
    EXPECT_NE(res::utf8_code_page_manifest().find(
                  "<activeCodePage xmlns=\"http://schemas.microsoft.com/SMI/2019/WindowsSettings\">UTF-8</activeCodePage>"),
              std::string::npos);
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
