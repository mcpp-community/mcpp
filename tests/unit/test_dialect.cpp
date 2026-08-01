#include <gtest/gtest.h>

import std;
import mcpp.toolchain.dialect;
import mcpp.toolchain.model;

// The MSVC row is unreachable in a real build until the cl.exe backend lands,
// so these tests are the only thing keeping it honest.

TEST(Dialect, LibFlagHasBothShapes) {
    // GNU names a library by prefixing, MSVC by suffixing. A single
    // string_view prefix cannot express `z.lib`, which is why libFlag is a
    // format rather than a prefix.
    EXPECT_EQ(mcpp::toolchain::lib_flag_for(mcpp::toolchain::gnu_dialect(),  "z"),
              "-lz");
    EXPECT_EQ(mcpp::toolchain::lib_flag_for(mcpp::toolchain::msvc_dialect(), "z"),
              "z.lib");
}

TEST(Dialect, LibFlagHandlesDottedAndHyphenatedNames) {
    EXPECT_EQ(mcpp::toolchain::lib_flag_for(mcpp::toolchain::gnu_dialect(),
                                            "avcodec-60"), "-lavcodec-60");
    EXPECT_EQ(mcpp::toolchain::lib_flag_for(mcpp::toolchain::msvc_dialect(),
                                            "avcodec-60"), "avcodec-60.lib");
}

// Every dialect field must be populated in both rows. An empty one silently
// emits nothing, which for staticRuntime or forceCxxLang means the compile
// changes meaning rather than failing.
TEST(Dialect, LinkAndLanguageFieldsPopulatedInBothRows) {
    for (auto const* d : { &mcpp::toolchain::gnu_dialect(),
                           &mcpp::toolchain::msvc_dialect() }) {
        EXPECT_FALSE(d->libFlag.empty())         << d->id;
        EXPECT_FALSE(d->libSearchPrefix.empty()) << d->id;
        EXPECT_FALSE(d->forceCxxLang.empty())    << d->id;
        EXPECT_FALSE(d->staticRuntime.empty())   << d->id;
        EXPECT_FALSE(d->outputExePrefix.empty()) << d->id;
        // A `{}` placeholder is what makes lib_flag_for work at all.
        EXPECT_NE(d->libFlag.find("{}"), std::string_view::npos) << d->id;
    }
}

// dialect_for must keep routing clang-targeting-MSVC to the gnu spellings:
// that driver takes GNU flags even though its ABI and STL are Microsoft's.
TEST(Dialect, OnlyNativeClExeGetsTheMsvcRow) {
    mcpp::toolchain::Toolchain clangMsvc;
    clangMsvc.compiler     = mcpp::toolchain::CompilerId::Clang;
    clangMsvc.targetTriple = "x86_64-pc-windows-msvc";
    EXPECT_EQ(mcpp::toolchain::dialect_for(clangMsvc).id, "gnu");

    mcpp::toolchain::Toolchain cl;
    cl.compiler     = mcpp::toolchain::CompilerId::MSVC;
    cl.targetTriple = "x86_64-pc-windows-msvc";
    EXPECT_EQ(mcpp::toolchain::dialect_for(cl).id, "msvc");
}
