#include <gtest/gtest.h>

import std;
import mcpp.manifest;
import mcpp.toolchain.dialect;
import mcpp.toolchain.model;
import mcpp.toolchain.registry;

using namespace mcpp::toolchain;

namespace {
Toolchain make_tc(CompilerId id, std::string triple = {}) {
    Toolchain tc;
    tc.compiler = id;
    tc.targetTriple = std::move(triple);
    return tc;
}
} // namespace

TEST(CommandDialect, GnuSpellings) {
    auto tc = make_tc(CompilerId::GCC);
    const auto& d = dialect_for(tc);
    EXPECT_EQ(d.id, "gnu");
    EXPECT_EQ(d.includePrefix, "-I");
    EXPECT_EQ(d.stdPrefix, "-std=");
    EXPECT_EQ(d.outputObjPrefix, "-o ");
    EXPECT_EQ(d.objExt, ".o");
    EXPECT_EQ(d.archiveCmd, "$ar rcs $out $in");
    EXPECT_FALSE(d.rspfileLink);
    EXPECT_EQ(d.linkStyle, CommandDialect::LinkStyle::Driver);
    // Clang and MinGW share the gnu row.
    EXPECT_EQ(dialect_for(make_tc(CompilerId::Clang)).id, "gnu");
    EXPECT_EQ(dialect_for(make_tc(CompilerId::GCC, "x86_64-w64-mingw32")).id, "gnu");
}

TEST(CommandDialect, MsvcSpellings) {
    const auto& d = dialect_for(make_tc(CompilerId::MSVC));
    EXPECT_EQ(d.id, "msvc");
    EXPECT_EQ(d.includePrefix, "/I");
    EXPECT_EQ(d.stdPrefix, "/std:");
    EXPECT_EQ(d.outputObjPrefix, "/Fo:");
    EXPECT_EQ(d.objExt, ".obj");
    // Each object carries its debug information: no compile writes through
    // the shared PDB server another compile's unit holds.
    EXPECT_EQ(d.debugFlags, "/Z7");
    EXPECT_TRUE(d.rspfileLink);
    EXPECT_EQ(d.linkStyle, CommandDialect::LinkStyle::SeparateLinker);
}

TEST(BmiTraitsSpellings, ModuleFlagRowsAreConsistent) {
    auto gcc = bmi_traits(make_tc(CompilerId::GCC));
    EXPECT_EQ(gcc.compileModulesFlag, " -fmodules");
    EXPECT_TRUE(gcc.stdBmiUsePrefix.empty());
    EXPECT_TRUE(gcc.moduleOutputPrefix.empty());

    auto clang = bmi_traits(make_tc(CompilerId::Clang));
    EXPECT_TRUE(clang.compileModulesFlag.empty());
    EXPECT_EQ(clang.stdBmiUsePrefix, " -fmodule-file=std=");
    EXPECT_EQ(clang.moduleOutputPrefix, " -fmodule-output=");
    EXPECT_EQ(clang.bmiSearchPrefix, " -fprebuilt-module-path=");

    auto msvc = bmi_traits(make_tc(CompilerId::MSVC));
    EXPECT_EQ(msvc.stdBmiUsePrefix, " /reference std=");
    EXPECT_EQ(msvc.moduleOutputPrefix, " /ifcOutput ");
    EXPECT_EQ(msvc.bmiSearchPrefix, " /ifcSearchDir ");
}

TEST(MingwSpec, LegacyMingwSpellingIsGccTargetingWindowsGnu) {
    // `mingw` is no longer a family: it normalizes to gcc targeting
    // x86_64-windows-gnu. Which payload serves it is host-split at the
    // distribution layer (winlibs mingw-gcc on Windows, mingw-cross-gcc
    // elsewhere) — the user-facing identity is the same either way.
    auto spec = parse_toolchain_spec("mingw@16.1.0");
    ASSERT_TRUE(spec.has_value());
    EXPECT_FALSE(is_system_toolchain(*spec));
    EXPECT_EQ(spec->family, Family::Gcc);
    EXPECT_EQ(spec->target.str(), "x86_64-windows-gnu");
    EXPECT_EQ(spec->display(), "gcc@16.1.0 → x86_64-windows-gnu");

    auto pkg = to_xim_package(*spec);
    EXPECT_EQ(pkg.ximVersion, "16.1.0");
    ASSERT_FALSE(pkg.frontendCandidates.empty());
#if defined(_WIN32)
    EXPECT_EQ(pkg.ximName, "mingw-gcc");
    EXPECT_EQ(pkg.frontendCandidates.front(), "g++.exe");
#else
    EXPECT_EQ(pkg.ximName, "mingw-cross-gcc");
    EXPECT_EQ(pkg.frontendCandidates.front(), "x86_64-w64-mingw32-g++");
#endif
    EXPECT_FALSE(pkg.needsGccPostInstallFixup);
}

TEST(MingwSpec, PayloadIdentityAndDefaultMatching) {
    // Both host-split payload dirs identify as the SAME (gcc, windows-gnu).
    for (auto dir : {"mingw-gcc", "mingw-cross-gcc"}) {
        auto id = identify_xim_payload(dir);
        ASSERT_TRUE(id.has_value()) << dir;
        EXPECT_EQ(id->family, Family::Gcc) << dir;
        EXPECT_EQ(id->target.str(), "x86_64-windows-gnu") << dir;
    }
    auto id = *identify_xim_payload("mingw-cross-gcc");
    auto def = parse_toolchain_spec("mingw-cross@16.1.0");   // legacy default string
    ASSERT_TRUE(def.has_value());
    EXPECT_TRUE(spec_matches_payload(*def, id, "16.1.0"));
    EXPECT_FALSE(spec_matches_payload(*def, id, "15.1.0"));
    auto llvmDef = parse_toolchain_spec("llvm@16.1.0");
    ASSERT_TRUE(llvmDef.has_value());
    EXPECT_FALSE(spec_matches_payload(*llvmDef, id, "16.1.0"));
}

TEST(StdFlagFor, PerDialectSpelling) {
    const auto& gnu  = dialect_for(make_tc(CompilerId::GCC));
    const auto& msvc = dialect_for(make_tc(CompilerId::MSVC));
    EXPECT_EQ(std_flag_for(gnu, "c++26", 26), "-std=c++26");
    EXPECT_EQ(std_flag_for(gnu, "gnu++23", 23), "-std=gnu++23");
    EXPECT_EQ(std_flag_for(gnu, "c++20", 20), "-std=c++20");
    EXPECT_EQ(std_flag_for(gnu, "gnu++20", 20), "-std=gnu++20");
    // The `level <= 20` row: unreachable until c++20 entered the allow-list,
    // because 23 was the lowest level a manifest could produce.
    EXPECT_EQ(std_flag_for(msvc, "c++20", 20), "/std:c++20");
    EXPECT_EQ(std_flag_for(msvc, "gnu++20", 20), "/std:c++20");   // no MSVC gnu dialect
    EXPECT_EQ(std_flag_for(msvc, "c++23", 23), "/std:c++latest");
    EXPECT_EQ(std_flag_for(msvc, "c++26", 26), "/std:c++latest");
}

// ─── issue #210: dialect-class flag extraction ───────────────────────────

TEST(DialectFlags, KnownListAndEscapeHatch) {
    mcpp::manifest::BuildConfig bc;
    bc.cxxflags = {"-freflection", "-O3", "-Wall", "-D_GLIBCXX_USE_CXX11_ABI=0"};
    bc.dialectCxxflags = {"-fcustom-std-thing", "-freflection"};  // dup dedups
    auto flags = mcpp::manifest::dialect_flags(bc);
    ASSERT_EQ(flags.size(), 3u);
    EXPECT_EQ(flags[0], "-fcustom-std-thing");   // explicit first, order kept
    EXPECT_EQ(flags[1], "-freflection");         // deduped against cxxflags copy
    EXPECT_EQ(flags[2], "-D_GLIBCXX_USE_CXX11_ABI=0");  // auto-promoted
}

TEST(DialectFlags, ExtractionDetails) {
    using mcpp::manifest::is_dialect_flag;
    EXPECT_TRUE(is_dialect_flag("-freflection"));
    EXPECT_TRUE(is_dialect_flag("-fcontracts"));
    EXPECT_TRUE(is_dialect_flag("-fno-char8_t"));
    EXPECT_TRUE(is_dialect_flag("-D_GLIBCXX_USE_CXX11_ABI=1"));
    // Conservative first list: these stay per-unit.
    EXPECT_FALSE(is_dialect_flag("-fno-exceptions"));
    EXPECT_FALSE(is_dialect_flag("-fno-rtti"));
    EXPECT_FALSE(is_dialect_flag("-O2"));
    EXPECT_FALSE(is_dialect_flag("-Wall"));
    EXPECT_FALSE(is_dialect_flag("-fPIC"));
}

TEST(MingwModel, TargetPredicate) {
    EXPECT_TRUE(is_mingw_target(make_tc(CompilerId::GCC, "x86_64-w64-mingw32")));
    EXPECT_FALSE(is_mingw_target(make_tc(CompilerId::GCC, "x86_64-linux-gnu")));
    EXPECT_FALSE(is_mingw_target(make_tc(CompilerId::Clang, "x86_64-pc-windows-msvc")));
}

// ─── #718: the CRT model reaches every MSVC-ABI row, cl and clang++ alike ──

// The property the whole design rests on: one helper, `msvc_abi_crt_word`,
// spells the SAME two-state switch (static/dynamic CRT) for whichever driver
// `tc` is, and says nothing for a row that is not the MSVC ABI at all.
TEST(MsvcAbiCrtWord, SpelledForEachDriverAndEmptyOffTheAbi) {
    auto cl = make_tc(CompilerId::MSVC, "x86_64-pc-windows-msvc");
    EXPECT_EQ(msvc_abi_crt_word(cl, /*staticCrt=*/true),  "/MT");
    EXPECT_EQ(msvc_abi_crt_word(cl, /*staticCrt=*/false), "/MD");

    auto llvmRow = make_tc(CompilerId::Clang, "x86_64-pc-windows-msvc");
    EXPECT_EQ(msvc_abi_crt_word(llvmRow, /*staticCrt=*/true),
              "-fms-runtime-lib=static");
    EXPECT_EQ(msvc_abi_crt_word(llvmRow, /*staticCrt=*/false),
              "-fms-runtime-lib=dll");

    // Neither the MSVC ABI: MinGW links the MSVC CRT to no row at all, and a
    // plain Linux row has no CRT axis of this kind either.
    EXPECT_TRUE(msvc_abi_crt_word(
        make_tc(CompilerId::GCC, "x86_64-w64-mingw32"), true).empty());
    EXPECT_TRUE(msvc_abi_crt_word(
        make_tc(CompilerId::Clang, "x86_64-linux-gnu"), false).empty());
    EXPECT_TRUE(msvc_abi_crt_word(
        make_tc(CompilerId::GCC, "x86_64-linux-gnu"), true).empty());
}

// Every MSVC-ABI row x {undeclared, self-contained, toolchain-coupled,
// host-coupled, linkage=static} yields exactly one CRT word, spelled for its
// own driver — the property property test the design asks for at the level
// this pure helper can state it at (§7.3's criterion). "undeclared" and
// "toolchain-coupled" share a word (`/MD`/`-fms-runtime-lib=dll`) because the
// undeclared default and an explicit `toolchain-coupled` both compile
// against the dynamic CRT; they differ only in whether the redistributable
// is staged, which is a distribution.cppm/flags.cppm concern this helper does
// not carry.
TEST(MsvcAbiCrtWord, EveryContractResolvesToExactlyOneWordPerRow) {
    struct Row { Toolchain tc; std::string_view label; };
    Row rows[] = {
        {make_tc(CompilerId::MSVC, "x86_64-pc-windows-msvc"), "cl"},
        {make_tc(CompilerId::Clang, "x86_64-pc-windows-msvc"), "llvm"},
    };
    struct Case {
        std::string_view linkage, cxxRuntime;
        bool wantsStatic;
    };
    Case cases[] = {
        {"", "",                   false},  // undeclared
        {"", "self-contained",     true},
        {"", "toolchain-coupled",  false},
        {"", "host-coupled",       false},
        {"static", "",             true},
    };
    for (auto& row : rows) {
        for (auto& c : cases) {
            const bool staticCrt = msvc_wants_static_crt(c.linkage, c.cxxRuntime);
            EXPECT_EQ(staticCrt, c.wantsStatic)
                << row.label << " linkage='" << c.linkage
                << "' cxx_runtime='" << c.cxxRuntime << "'";
            auto word = msvc_abi_crt_word(row.tc, staticCrt);
            EXPECT_FALSE(word.empty()) << row.label;
            // Exactly one of the two spellings for this driver, and it is
            // the SAME word `flags.cppm`'s compile line and `scan.cpp`'s std
            // BMI mirror both call this helper for — one derivation, so a TU
            // and its std BMI cannot disagree by construction.
            if (row.tc.compiler == CompilerId::MSVC) {
                EXPECT_EQ(word, staticCrt ? "/MT" : "/MD") << row.label;
            } else {
                EXPECT_EQ(word, staticCrt ? "-fms-runtime-lib=static"
                                          : "-fms-runtime-lib=dll") << row.label;
            }
        }
    }
}

// A shared-PDB word is refused on cl.exe, either dash; other words pass.
TEST(CheckSharedPdbWord, ZiZIAndFSAreRefusedOtherWordsPass) {
    for (auto* word : {"/Zi", "-Zi", "/ZI", "-ZI", "/FS", "-FS"}) {
        auto refusal = check_shared_pdb_word(word, "[build] cxxflags");
        ASSERT_TRUE(refusal.has_value()) << word;
        EXPECT_NE(refusal->find(word), std::string::npos) << *refusal;
        EXPECT_NE(refusal->find("[build] cxxflags"), std::string::npos) << *refusal;
        EXPECT_NE(refusal->find("/Z7"), std::string::npos) << *refusal;
    }
    for (auto* word : {"/Z7", "-Z7", "/Zc:__cplusplus", "/Zp8", "/FSx", "/MT", "-g"})
        EXPECT_FALSE(check_shared_pdb_word(word, "[build] cxxflags").has_value()) << word;
}

// D3: a free-form CRT word is always a second statement once the model is
// resolved for every MSVC-ABI row.
TEST(CheckCrtWord, AgreeingWordIsRedundantContradictingWordIsRefused) {
    // Recognised spellings, both dash conventions and both drivers.
    for (auto* word : {"/MT", "-MT", "-fms-runtime-lib=static"}) {
        auto agree = check_crt_word(word, /*staticCrt=*/true, "[build] cxxflags");
        ASSERT_TRUE(agree.has_value()) << word;
        EXPECT_FALSE(agree->contradicts) << word;
        // The key it names states the same model.
        EXPECT_NE(agree->message.find("self-contained"), std::string::npos) << agree->message;
        auto disagree = check_crt_word(word, /*staticCrt=*/false, "[build] cxxflags");
        ASSERT_TRUE(disagree.has_value()) << word;
        EXPECT_TRUE(disagree->contradicts) << word;
        EXPECT_NE(disagree->message.find(word), std::string::npos) << disagree->message;
        EXPECT_NE(disagree->message.find("[build] cxxflags"), std::string::npos)
            << disagree->message;
    }
    for (auto* word : {"/MD", "-MD", "-fms-runtime-lib=dll"}) {
        auto agree = check_crt_word(word, /*staticCrt=*/false, "dialect_cxxflags");
        ASSERT_TRUE(agree.has_value()) << word;
        EXPECT_FALSE(agree->contradicts) << word;
        EXPECT_NE(agree->message.find("toolchain-coupled"), std::string::npos) << agree->message;
        EXPECT_EQ(agree->message.find("self-contained"), std::string::npos) << agree->message;
        auto disagree = check_crt_word(word, /*staticCrt=*/true, "dialect_cxxflags");
        ASSERT_TRUE(disagree.has_value()) << word;
        EXPECT_TRUE(disagree->contradicts) << word;
    }
    // A debug CRT word is refused under either model: the model has no debug
    // axis, and the std module and the link use the release CRT.
    for (auto* word : {"/MTd", "-MTd", "/MDd", "-MDd",
                       "-fms-runtime-lib=static_dbg", "-fms-runtime-lib=dll_dbg"}) {
        for (bool staticCrt : {true, false}) {
            auto v = check_crt_word(word, staticCrt, "[build] cxxflags");
            ASSERT_TRUE(v.has_value()) << word;
            EXPECT_TRUE(v->contradicts) << word;
            EXPECT_NE(v->message.find("debug CRT"), std::string::npos) << v->message;
        }
    }
    // A word this axis does not recognise says nothing about it.
    EXPECT_FALSE(check_crt_word("-O2", true, "[build] cxxflags").has_value());
    EXPECT_FALSE(check_crt_word("/EHsc", false, "[build] cxxflags").has_value());
}
