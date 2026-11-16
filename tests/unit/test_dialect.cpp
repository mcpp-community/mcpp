
import std;
import mcpplibs.mtest;
import mcpp.toolchain.dialect;
import mcpp.toolchain.model;

using namespace mcpplibs::mtest;

// The MSVC row is unreachable in a real build until the cl.exe backend lands,
// so these tests are the only thing keeping it honest.

UTest lib_flag_has_both_shapes{"Dialect/LibFlagHasBothShapes", [] {
    // GNU names a library by prefixing, MSVC by suffixing. A single
    // string_view prefix cannot express `z.lib`, which is why libFlag is a
    // format rather than a prefix.
    expect(eq(mcpp::toolchain::lib_flag_for(mcpp::toolchain::gnu_dialect(),  "z"), "-lz"));
    expect(eq(mcpp::toolchain::lib_flag_for(mcpp::toolchain::msvc_dialect(), "z"), "z.lib"));
}};

UTest lib_flag_handles_dotted_and_hyphenated_names{"Dialect/LibFlagHandlesDottedAndHyphenatedNames", [] {
    expect(eq(mcpp::toolchain::lib_flag_for(mcpp::toolchain::gnu_dialect(),
                                            "avcodec-60"), "-lavcodec-60"));
    expect(eq(mcpp::toolchain::lib_flag_for(mcpp::toolchain::msvc_dialect(),
                                            "avcodec-60"), "avcodec-60.lib"));
}};

// Every dialect field must be populated in both rows. An empty one silently
// emits nothing, which for staticRuntime or forceCxxLang means the compile
// changes meaning rather than failing.
UTest link_and_language_fields_populated_in_both_rows{"Dialect/LinkAndLanguageFieldsPopulatedInBothRows", [] {
    for (auto const* d : { &mcpp::toolchain::gnu_dialect(),
                           &mcpp::toolchain::msvc_dialect() }) {
        if (auto&& failed = expect(!(d->libFlag.empty())); !failed) failed << d->id;
        if (auto&& failed = expect(!(d->libSearchPrefix.empty())); !failed) failed << d->id;
        if (auto&& failed = expect(!(d->forceCxxLang.empty())); !failed) failed << d->id;
        if (auto&& failed = expect(!(d->staticRuntime.empty())); !failed) failed << d->id;
        if (auto&& failed = expect(!(d->outputExePrefix.empty())); !failed) failed << d->id;
        // A `{}` placeholder is what makes lib_flag_for work at all.
        expect(ne(d->libFlag.find("{}"), std::string_view::npos)) << d->id;
    }
}};

// dialect_for must keep routing clang-targeting-MSVC to the gnu spellings:
// that driver takes GNU flags even though its ABI and STL are Microsoft's.
UTest only_native_cl_exe_gets_the_msvc_row{"Dialect/OnlyNativeClExeGetsTheMsvcRow", [] {
    mcpp::toolchain::Toolchain clangMsvc;
    clangMsvc.compiler     = mcpp::toolchain::CompilerId::Clang;
    clangMsvc.targetTriple = "x86_64-pc-windows-msvc";
    expect(eq(mcpp::toolchain::dialect_for(clangMsvc).id, "gnu"));

    mcpp::toolchain::Toolchain cl;
    cl.compiler     = mcpp::toolchain::CompilerId::MSVC;
    cl.targetTriple = "x86_64-pc-windows-msvc";
    expect(eq(mcpp::toolchain::dialect_for(cl).id, "msvc"));
}};
