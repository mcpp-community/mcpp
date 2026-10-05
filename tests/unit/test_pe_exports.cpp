#include <gtest/gtest.h>

import std;
import mcpp.build.pe_exports;
import mcpp.build.coff_exports;
import mcpp.build.plan;
import mcpp.manifest;
import mcpp.modgraph.graph;
import mcpp.modgraph.scanner;
import mcpp.toolchain.model;

using mcpp::build::pe::ir_declares_exports;
using mcpp::build::pe::read_nm_exports;

TEST(PeExports, RecognizesBothBitcodeContainersByContents) {
    const std::array raw{std::byte{0x42}, std::byte{0x43}, std::byte{0xc0}, std::byte{0xde}};
    const std::array wrapped{std::byte{0xde}, std::byte{0xc0}, std::byte{0x17}, std::byte{0x0b}};
    const std::array coff{std::byte{0x64}, std::byte{0x86}, std::byte{1}, std::byte{0}};
    EXPECT_TRUE(mcpp::build::pe::is_bitcode(raw));
    EXPECT_TRUE(mcpp::build::pe::is_bitcode(wrapped));
    EXPECT_FALSE(mcpp::build::pe::is_bitcode(coff));
    EXPECT_FALSE(mcpp::build::pe::is_bitcode(std::span(raw).first(3)));
}

TEST(PeExports, DllStorageIntentCoversFunctionsDataAndAliases) {
    EXPECT_TRUE(ir_declares_exports("define dso_local dllexport i32 @api() {\nret i32 7\n}"));
    EXPECT_TRUE(ir_declares_exports("@api = dllexport global i32 7"));
    EXPECT_TRUE(ir_declares_exports("@api = dllexport alias i32, ptr @other"));
    EXPECT_TRUE(ir_declares_exports("@\"name with spaces\" = dso_local dllexport constant i32 7"));
}

TEST(PeExports, NamesCommentsAttributesAndDataDoNotDeclareExports) {
    EXPECT_FALSE(ir_declares_exports(R"(
; define dllexport i32 @comment()
@dllexport = global i32 1
@"dllexport" = global i32 2
@message = constant [30 x i8] c"dllexport /EXPORT:pretend\00"
define i32 @dllexport() {
  %dllexport = add i32 1, 2
  ret i32 %dllexport
}
attributes #0 = { "dllexport"="/EXPORT:pretend" }
!llvm.ident = !{!1}
!1 = !{!"/EXPORT:not_a_linker_option"}
)"));
}

TEST(PeExports, LinkerMetadataIntentFollowsOnlyItsNamedRoot) {
    EXPECT_TRUE(ir_declares_exports(R"(
!llvm.linker.options = !{!7, !8}
!7 = !{!"/DEFAULTLIB:libcmt"}
!8 = !{!"\2DEXPORT:chosen"}
)"));
    EXPECT_TRUE(ir_declares_exports("!llvm.linker.options = !{!\"/ExPoRt:chosen,DATA\"}"));
    EXPECT_TRUE(ir_declares_exports("module asm \".section .drectve\\0A.ascii \\22 /EXPORT:chosen\\22\""));
    EXPECT_FALSE(ir_declares_exports("!llvm.linker.options = !{!1}\n!1 = !{!\"/DEFAULTLIB:x\"}\n!2 = !{!\"/EXPORT:unused\"}"));
    EXPECT_FALSE(ir_declares_exports("!llvm.linker.options = !{!1}\n!1 = !{!1}"));
}

TEST(PeExports, NmCandidatesKeepDataAndComdatButExcludeNonDefinitions) {
    auto symbols = read_nm_exports(R"(
function T ---------------- 0
data D ---------------- 0
zero B ---------------- 0
constant R ---------------- 0
comdat W ---------------- 0
vtable V ---------------- 0
external U ---------------- 0
local t ---------------- 0
absolute A ---------------- 0
??_Gwidget@@ T ---------------- 0
managed.name T ---------------- 0
)", false);
    ASSERT_TRUE(symbols.has_value()) << symbols.error();
    const std::vector<mcpp::build::coff::Export> expected{
        {"function", false}, {"data", true}, {"zero", true},
        {"constant", true}, {"comdat", false}, {"vtable", true}};
    EXPECT_EQ(*symbols, expected);
}

TEST(PeExports, I386KeepsStdcallDecorationButNormalizesCdecl) {
    auto symbols = read_nm_exports("_cdecl T ---------------- 0\n_stdcall@4 T ---------------- 0\n_variable D ---------------- 0\n", true);
    ASSERT_TRUE(symbols.has_value());
    const std::vector<mcpp::build::coff::Export> expected{
        {"cdecl", false}, {"_stdcall@4", false}, {"variable", true}};
    EXPECT_EQ(*symbols, expected);
    auto amd64 = read_nm_exports("_cdecl T ---------------- 0", false);
    ASSERT_TRUE(amd64.has_value());
    EXPECT_EQ(amd64->front().name, "_cdecl");
}

TEST(PeExports, UnexpectedNmOutputIsAnErrorRatherThanAnEmptySurface) {
    EXPECT_FALSE(read_nm_exports("tool emitted an unexpected diagnostic", false));
    EXPECT_FALSE(read_nm_exports("candidate T", false));
}

TEST(PeExports, OptOutAffectsOnlyTheMsvcAbiSharedLinkForm) {
    for (auto triple : {"x86_64-pc-windows-msvc", "i686-pc-windows-msvc",
                       "x86_64-w64-mingw32", "x86_64-linux-gnu", "aarch64-apple-darwin"}) {
        for (bool enabled : {false, true}) {
            mcpp::manifest::Manifest manifest;
            manifest.package.name = "probe";
            manifest.package.version = "0.1.0";
            manifest.package.standard = "c++23";
            manifest.targets.push_back({.name = "probe",
                .kind = mcpp::manifest::Target::SharedLibrary, .windowsAutoExport = enabled});
            mcpp::toolchain::Toolchain tc;
            tc.compiler = mcpp::toolchain::CompilerId::Clang;
            tc.targetTriple = triple;
            mcpp::modgraph::PackageRoot root;
            root.root = std::filesystem::temp_directory_path() / "mcpp-pe-plan";
            root.manifest = manifest;
            const auto plan = mcpp::build::make_plan(manifest, tc, {}, {}, {}, {root},
                root.root, root.root / "target", {}, {});
            ASSERT_TRUE(plan.has_value()) << plan.error();
            ASSERT_EQ(plan->linkUnits.size(), 1u);
            EXPECT_EQ(!plan->linkUnits.front().defFile.empty(),
                enabled && std::string_view(triple).ends_with("windows-msvc")) << triple;
        }
    }
}

TEST(PeExports, SymbolPatternsFollowVersionScriptGlobs) {
    using mcpp::build::pe::symbol_matches;
    EXPECT_TRUE(symbol_matches("vk_icd*", "vk_icdGetInstanceProcAddr"));
    EXPECT_TRUE(symbol_matches("vk_icd*", "vk_icd"));
    EXPECT_FALSE(symbol_matches("vk_icd*", "vkGetInstanceProcAddr"));
    EXPECT_TRUE(symbol_matches("api_?", "api_x"));
    EXPECT_FALSE(symbol_matches("api_?", "api_xy"));
    EXPECT_TRUE(symbol_matches("api_[a-c]*", "api_b_open"));
    EXPECT_FALSE(symbol_matches("api_[!a-c]*", "api_b_open"));
    EXPECT_TRUE(symbol_matches("*Plugin*", "?createPlugin@@YAPEAXXZ"));
    EXPECT_TRUE(symbol_matches("exact", "exact"));
    EXPECT_FALSE(symbol_matches("exact", "exactly"));
    EXPECT_TRUE(symbol_matches("*", ""));
    EXPECT_TRUE(symbol_matches("a[b", "a[b"));   // an unterminated class is a character
}

TEST(PeExports, NarrowKeepsTheMatchingCandidatesAndTheirDataFlag) {
    std::vector<mcpp::build::coff::Export> all{
        {"plugin_open", false}, {"plugin_table", true}, {"helper", false}};
    const std::vector<std::string> patterns{"plugin_*"};
    auto kept = mcpp::build::pe::narrow(all, patterns);
    ASSERT_EQ(kept.size(), 2u);
    EXPECT_EQ(kept[0].name, "plugin_open");
    EXPECT_FALSE(kept[0].data);
    EXPECT_EQ(kept[1].name, "plugin_table");
    EXPECT_TRUE(kept[1].data);
}

TEST(PeExports, ExportsWithoutDiscoveryIsRefusedOnlyOnMsvcAbiRows) {
    for (auto triple : {"x86_64-pc-windows-msvc", "x86_64-w64-mingw32", "x86_64-linux-gnu"}) {
        mcpp::manifest::Manifest manifest;
        manifest.package.name = "probe";
        manifest.package.version = "0.1.0";
        manifest.package.standard = "c++23";
        manifest.targets.push_back({.name = "probe",
            .kind = mcpp::manifest::Target::SharedLibrary,
            .windowsAutoExport = false, .exportPatterns = {"probe_*"}});
        mcpp::toolchain::Toolchain tc;
        tc.compiler = mcpp::toolchain::CompilerId::Clang;
        tc.targetTriple = triple;
        mcpp::modgraph::PackageRoot root;
        root.root = std::filesystem::temp_directory_path() / "mcpp-pe-plan";
        root.manifest = manifest;
        const auto plan = mcpp::build::make_plan(manifest, tc, {}, {}, {}, {root},
            root.root, root.root / "target", {}, {});
        if (std::string_view(triple).ends_with("windows-msvc")) {
            ASSERT_FALSE(plan.has_value()) << triple;
            EXPECT_NE(plan.error().find("windows_auto_export = false"), std::string::npos);
        } else {
            EXPECT_TRUE(plan.has_value()) << triple << ": " << (plan ? "" : plan.error());
        }
    }
}
