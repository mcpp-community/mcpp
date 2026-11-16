
import std;
import mcpplibs.mtest;
import mcpp.build.pe_exports;
import mcpp.build.coff_exports;
import mcpp.build.plan;
import mcpp.manifest;
import mcpp.modgraph.graph;
import mcpp.modgraph.scanner;
import mcpp.toolchain.model;

using namespace mcpplibs::mtest;

using mcpp::build::pe::ir_declares_exports;
using mcpp::build::pe::read_nm_exports;

UTest recognizes_both_bitcode_containers_by_contents{"PeExports/RecognizesBothBitcodeContainersByContents", [] {
    const std::array raw{std::byte{0x42}, std::byte{0x43}, std::byte{0xc0}, std::byte{0xde}};
    const std::array wrapped{std::byte{0xde}, std::byte{0xc0}, std::byte{0x17}, std::byte{0x0b}};
    const std::array coff{std::byte{0x64}, std::byte{0x86}, std::byte{1}, std::byte{0}};
    expect(mcpp::build::pe::is_bitcode(raw));
    expect(mcpp::build::pe::is_bitcode(wrapped));
    expect(!mcpp::build::pe::is_bitcode(coff));
    expect(!(mcpp::build::pe::is_bitcode(std::span(raw).first(3))));
}};

UTest dll_storage_intent_covers_functions_data_and_aliases{"PeExports/DllStorageIntentCoversFunctionsDataAndAliases", [] {
    expect(bool(ir_declares_exports("define dso_local dllexport i32 @api() {\nret i32 7\n}")));
    expect(bool(ir_declares_exports("@api = dllexport global i32 7")));
    expect(bool(ir_declares_exports("@api = dllexport alias i32, ptr @other")));
    expect(bool(ir_declares_exports("@\"name with spaces\" = dso_local dllexport constant i32 7")));
}};

UTest names_comments_attributes_and_data_do_not_declare_exports{"PeExports/NamesCommentsAttributesAndDataDoNotDeclareExports", [] {
    expect(!(ir_declares_exports(R"(
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
)")));
}};

UTest linker_metadata_intent_follows_only_its_named_root{"PeExports/LinkerMetadataIntentFollowsOnlyItsNamedRoot", [] {
    expect(bool(ir_declares_exports(R"(
!llvm.linker.options = !{!7, !8}
!7 = !{!"/DEFAULTLIB:libcmt"}
!8 = !{!"\2DEXPORT:chosen"}
)")));
    expect(bool(ir_declares_exports("!llvm.linker.options = !{!\"/ExPoRt:chosen,DATA\"}")));
    expect(bool(ir_declares_exports("module asm \".section .drectve\\0A.ascii \\22 /EXPORT:chosen\\22\"")));
    expect(!ir_declares_exports("!llvm.linker.options = !{!1}\n!1 = !{!\"/DEFAULTLIB:x\"}\n!2 = !{!\"/EXPORT:unused\"}"));
    expect(!ir_declares_exports("!llvm.linker.options = !{!1}\n!1 = !{!1}"));
}};

UTest nm_candidates_keep_data_and_comdat_but_exclude_non_definitions{"PeExports/NmCandidatesKeepDataAndComdatButExcludeNonDefinitions", [] {
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
    if (auto&& failed = expect(fatal(symbols.has_value())); !failed) failed << symbols.error();
    const std::vector<mcpp::build::coff::Export> expected{
        {"function", false}, {"data", true}, {"zero", true},
        {"constant", true}, {"comdat", false}, {"vtable", true}};
    expect(eq(*symbols, expected));
}};

UTest i386_keeps_stdcall_decoration_but_normalizes_cdecl{"PeExports/I386KeepsStdcallDecorationButNormalizesCdecl", [] {
    auto symbols = read_nm_exports("_cdecl T ---------------- 0\n_stdcall@4 T ---------------- 0\n_variable D ---------------- 0\n", true);
    expect(fatal(symbols.has_value()));
    const std::vector<mcpp::build::coff::Export> expected{
        {"cdecl", false}, {"_stdcall@4", false}, {"variable", true}};
    expect(eq(*symbols, expected));
    auto amd64 = read_nm_exports("_cdecl T ---------------- 0", false);
    expect(fatal(amd64.has_value()));
    expect(eq(amd64->front().name, "_cdecl"));
}};

UTest unexpected_nm_output_is_an_error_rather_than_an_empty_surface{"PeExports/UnexpectedNmOutputIsAnErrorRatherThanAnEmptySurface", [] {
    expect(!read_nm_exports("tool emitted an unexpected diagnostic", false));
    expect(!read_nm_exports("candidate T", false));
}};

UTest opt_out_affects_only_the_msvc_abi_shared_link_form{"PeExports/OptOutAffectsOnlyTheMsvcAbiSharedLinkForm", [] {
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
            if (auto&& failed = expect(fatal(plan.has_value())); !failed) failed << plan.error();
            expect(fatal(eq(plan->linkUnits.size(), 1u)));
            expect(eq(!plan->linkUnits.front().defFile.empty(), enabled && std::string_view(triple).ends_with("windows-msvc"))) << triple;
        }
    }
}};

UTest symbol_patterns_follow_version_script_globs{"PeExports/SymbolPatternsFollowVersionScriptGlobs", [] {
    using mcpp::build::pe::symbol_matches;
    expect(symbol_matches("vk_icd*", "vk_icdGetInstanceProcAddr"));
    expect(symbol_matches("vk_icd*", "vk_icd"));
    expect(!symbol_matches("vk_icd*", "vkGetInstanceProcAddr"));
    expect(symbol_matches("api_?", "api_x"));
    expect(!symbol_matches("api_?", "api_xy"));
    expect(symbol_matches("api_[a-c]*", "api_b_open"));
    expect(!symbol_matches("api_[!a-c]*", "api_b_open"));
    expect(symbol_matches("*Plugin*", "?createPlugin@@YAPEAXXZ"));
    expect(symbol_matches("exact", "exact"));
    expect(!symbol_matches("exact", "exactly"));
    expect(symbol_matches("*", ""));
    expect(symbol_matches("a[b", "a[b"));   // an unterminated class is a character
}};

UTest narrow_keeps_the_matching_candidates_and_their_data_flag{"PeExports/NarrowKeepsTheMatchingCandidatesAndTheirDataFlag", [] {
    std::vector<mcpp::build::coff::Export> all{
        {"plugin_open", false}, {"plugin_table", true}, {"helper", false}};
    const std::vector<std::string> patterns{"plugin_*"};
    auto kept = mcpp::build::pe::narrow(all, patterns);
    expect(fatal(eq(kept.size(), 2u)));
    expect(eq(kept[0].name, "plugin_open"));
    expect(!(kept[0].data));
    expect(eq(kept[1].name, "plugin_table"));
    expect(bool(kept[1].data));
}};

UTest exports_without_discovery_is_refused_only_on_msvc_abi_rows{"PeExports/ExportsWithoutDiscoveryIsRefusedOnlyOnMsvcAbiRows", [] {
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
            expect(fatal(!plan.has_value())) << triple;
            expect(ne(plan.error().find("windows_auto_export = false"), std::string::npos));
        } else {
            if (auto&& failed = expect(plan.has_value()); !failed) failed << triple << ": " << (plan ? "" : plan.error());
        }
    }
}};
