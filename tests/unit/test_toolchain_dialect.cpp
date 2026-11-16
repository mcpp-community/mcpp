
import std;
import mcpplibs.mtest;
import mcpp.manifest;
import mcpp.toolchain.dialect;
import mcpp.toolchain.model;
import mcpp.toolchain.registry;

using namespace mcpplibs::mtest;

using namespace mcpp::toolchain;

namespace {
Toolchain make_tc(CompilerId id, std::string triple = {}) {
    Toolchain tc;
    tc.compiler = id;
    tc.targetTriple = std::move(triple);
    return tc;
}
} // namespace

UTest gnu_spellings{"CommandDialect/GnuSpellings", [] {
    auto tc = make_tc(CompilerId::GCC);
    const auto& d = dialect_for(tc);
    expect(eq(d.id, "gnu"));
    expect(eq(d.includePrefix, "-I"));
    expect(eq(d.stdPrefix, "-std="));
    expect(eq(d.outputObjPrefix, "-o "));
    expect(eq(d.objExt, ".o"));
    expect(eq(d.archiveCmd, "$ar rcs $out $in"));
    expect(!d.rspfileLink);
    expect(eq(d.linkStyle, CommandDialect::LinkStyle::Driver));
    // Clang and MinGW share the gnu row.
    expect(eq(dialect_for(make_tc(CompilerId::Clang)).id, "gnu"));
    expect(eq(dialect_for(make_tc(CompilerId::GCC, "x86_64-w64-mingw32")).id, "gnu"));
}};

UTest msvc_spellings{"CommandDialect/MsvcSpellings", [] {
    const auto& d = dialect_for(make_tc(CompilerId::MSVC));
    expect(eq(d.id, "msvc"));
    expect(eq(d.includePrefix, "/I"));
    expect(eq(d.stdPrefix, "/std:"));
    expect(eq(d.outputObjPrefix, "/Fo:"));
    expect(eq(d.objExt, ".obj"));
    // Each object carries its debug information: no compile writes through
    // the shared PDB server another compile's unit holds.
    expect(eq(d.debugFlags, "/Z7"));
    expect(bool(d.rspfileLink));
    expect(eq(d.linkStyle, CommandDialect::LinkStyle::SeparateLinker));
}};

UTest module_flag_rows_are_consistent{"BmiTraitsSpellings/ModuleFlagRowsAreConsistent", [] {
    auto gcc = bmi_traits(make_tc(CompilerId::GCC));
    expect(eq(gcc.compileModulesFlag, " -fmodules"));
    expect(gcc.stdBmiUsePrefix.empty());
    expect(gcc.moduleOutputPrefix.empty());

    auto clang = bmi_traits(make_tc(CompilerId::Clang));
    expect(clang.compileModulesFlag.empty());
    expect(eq(clang.stdBmiUsePrefix, " -fmodule-file=std="));
    expect(eq(clang.moduleOutputPrefix, " -fmodule-output="));
    expect(eq(clang.bmiSearchPrefix, " -fprebuilt-module-path="));

    auto msvc = bmi_traits(make_tc(CompilerId::MSVC));
    expect(eq(msvc.stdBmiUsePrefix, " /reference std="));
    expect(eq(msvc.moduleOutputPrefix, " /ifcOutput "));
    expect(eq(msvc.bmiSearchPrefix, " /ifcSearchDir "));
}};

UTest legacy_mingw_spelling_is_gcc_targeting_windows_gnu{"MingwSpec/LegacyMingwSpellingIsGccTargetingWindowsGnu", [] {
    // `mingw` is no longer a family: it normalizes to gcc targeting
    // x86_64-windows-gnu. Which payload serves it is host-split at the
    // distribution layer (winlibs mingw-gcc on Windows, mingw-cross-gcc
    // elsewhere) — the user-facing identity is the same either way.
    auto spec = parse_toolchain_spec("mingw@16.1.0");
    expect(fatal(spec.has_value()));
    expect(!is_system_toolchain(*spec));
    expect(eq(spec->family, Family::Gcc));
    expect(eq(spec->target.str(), "x86_64-windows-gnu"));
    expect(eq(spec->display(), "gcc@16.1.0 → x86_64-windows-gnu"));

    auto pkg = to_xim_package(*spec);
    expect(eq(pkg.ximVersion, "16.1.0"));
    expect(fatal(!pkg.frontendCandidates.empty()));
#if defined(_WIN32)
    expect(eq(pkg.ximName, "mingw-gcc"));
    expect(eq(pkg.frontendCandidates.front(), "g++.exe"));
#else
    expect(eq(pkg.ximName, "mingw-cross-gcc"));
    expect(eq(pkg.frontendCandidates.front(), "x86_64-w64-mingw32-g++"));
#endif
    expect(!pkg.needsGccPostInstallFixup);
}};

UTest payload_identity_and_default_matching{"MingwSpec/PayloadIdentityAndDefaultMatching", [] {
    // Both host-split payload dirs identify as the SAME (gcc, windows-gnu).
    for (auto dir : {"mingw-gcc", "mingw-cross-gcc"}) {
        auto id = identify_xim_payload(dir);
        expect(fatal(id.has_value())) << dir;
        expect(eq(id->family, Family::Gcc)) << dir;
        expect(eq(id->target.str(), "x86_64-windows-gnu")) << dir;
    }
    auto id = *identify_xim_payload("mingw-cross-gcc");
    auto def = parse_toolchain_spec("mingw-cross@16.1.0");   // legacy default string
    expect(fatal(def.has_value()));
    expect(bool(spec_matches_payload(*def, id, "16.1.0")));
    expect(!spec_matches_payload(*def, id, "15.1.0"));
    auto llvmDef = parse_toolchain_spec("llvm@16.1.0");
    expect(fatal(llvmDef.has_value()));
    expect(!spec_matches_payload(*llvmDef, id, "16.1.0"));
}};

UTest per_dialect_spelling{"StdFlagFor/PerDialectSpelling", [] {
    const auto& gnu  = dialect_for(make_tc(CompilerId::GCC));
    const auto& msvc = dialect_for(make_tc(CompilerId::MSVC));
    expect(eq(std_flag_for(gnu, "c++26", 26), "-std=c++26"));
    expect(eq(std_flag_for(gnu, "gnu++23", 23), "-std=gnu++23"));
    expect(eq(std_flag_for(gnu, "c++20", 20), "-std=c++20"));
    expect(eq(std_flag_for(gnu, "gnu++20", 20), "-std=gnu++20"));
    // The `level <= 20` row: unreachable until c++20 entered the allow-list,
    // because 23 was the lowest level a manifest could produce.
    expect(eq(std_flag_for(msvc, "c++20", 20), "/std:c++20"));
    expect(eq(std_flag_for(msvc, "gnu++20", 20), "/std:c++20"));   // no MSVC gnu dialect
    expect(eq(std_flag_for(msvc, "c++23", 23), "/std:c++latest"));
    expect(eq(std_flag_for(msvc, "c++26", 26), "/std:c++latest"));
}};

// ─── issue #210: dialect-class flag extraction ───────────────────────────

UTest known_list_and_escape_hatch{"DialectFlags/KnownListAndEscapeHatch", [] {
    mcpp::manifest::BuildConfig bc;
    bc.cxxflags = {"-freflection", "-O3", "-Wall", "-D_GLIBCXX_USE_CXX11_ABI=0"};
    bc.dialectCxxflags = {"-fcustom-std-thing", "-freflection"};  // dup dedups
    auto flags = mcpp::manifest::dialect_flags(bc);
    expect(fatal(eq(flags.size(), 3u)));
    expect(eq(flags[0], "-fcustom-std-thing"));   // explicit first, order kept
    expect(eq(flags[1], "-freflection"));         // deduped against cxxflags copy
    expect(eq(flags[2], "-D_GLIBCXX_USE_CXX11_ABI=0"));  // auto-promoted
}};

UTest extraction_details{"DialectFlags/ExtractionDetails", [] {
    using mcpp::manifest::is_dialect_flag;
    expect(is_dialect_flag("-freflection"));
    expect(is_dialect_flag("-fcontracts"));
    expect(is_dialect_flag("-fno-char8_t"));
    expect(is_dialect_flag("-D_GLIBCXX_USE_CXX11_ABI=1"));
    // Conservative first list: these stay per-unit.
    expect(!is_dialect_flag("-fno-exceptions"));
    expect(!is_dialect_flag("-fno-rtti"));
    expect(!is_dialect_flag("-O2"));
    expect(!is_dialect_flag("-Wall"));
    expect(!is_dialect_flag("-fPIC"));
}};

UTest target_predicate{"MingwModel/TargetPredicate", [] {
    expect(is_mingw_target(make_tc(CompilerId::GCC, "x86_64-w64-mingw32")));
    expect(!(is_mingw_target(make_tc(CompilerId::GCC, "x86_64-linux-gnu"))));
    expect(!(is_mingw_target(make_tc(CompilerId::Clang, "x86_64-pc-windows-msvc"))));
}};

// ─── #718: the CRT model reaches every MSVC-ABI row, cl and clang++ alike ──

// The property the whole design rests on: one helper, `msvc_abi_crt_word`,
// spells the SAME two-state switch (static/dynamic CRT) for whichever driver
// `tc` is, and says nothing for a row that is not the MSVC ABI at all.
UTest spelled_for_each_driver_and_empty_off_the_abi{"MsvcAbiCrtWord/SpelledForEachDriverAndEmptyOffTheAbi", [] {
    auto cl = make_tc(CompilerId::MSVC, "x86_64-pc-windows-msvc");
    expect(eq(msvc_abi_crt_word(cl, /*staticCrt=*/true), "/MT"));
    expect(eq(msvc_abi_crt_word(cl, /*staticCrt=*/false), "/MD"));

    auto llvmRow = make_tc(CompilerId::Clang, "x86_64-pc-windows-msvc");
    expect(eq(msvc_abi_crt_word(llvmRow, /*staticCrt=*/true), "-fms-runtime-lib=static"));
    expect(eq(msvc_abi_crt_word(llvmRow, /*staticCrt=*/false), "-fms-runtime-lib=dll"));

    // Neither the MSVC ABI: MinGW links the MSVC CRT to no row at all, and a
    // plain Linux row has no CRT axis of this kind either.
    expect(msvc_abi_crt_word(
        make_tc(CompilerId::GCC, "x86_64-w64-mingw32"), true).empty());
    expect(msvc_abi_crt_word(
        make_tc(CompilerId::Clang, "x86_64-linux-gnu"), false).empty());
    expect(msvc_abi_crt_word(
        make_tc(CompilerId::GCC, "x86_64-linux-gnu"), true).empty());
}};

// Every MSVC-ABI row x {undeclared, self-contained, toolchain-coupled,
// host-coupled, linkage=static} yields exactly one CRT word, spelled for its
// own driver — the property property test the design asks for at the level
// this pure helper can state it at (§7.3's criterion). "undeclared" and
// "toolchain-coupled" share a word (`/MD`/`-fms-runtime-lib=dll`) because the
// undeclared default and an explicit `toolchain-coupled` both compile
// against the dynamic CRT; they differ only in whether the redistributable
// is staged, which is a distribution.cppm/flags.cppm concern this helper does
// not carry.
UTest every_contract_resolves_to_exactly_one_word_per_row{"MsvcAbiCrtWord/EveryContractResolvesToExactlyOneWordPerRow", [] {
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
            expect(eq(staticCrt, c.wantsStatic))
                << row.label << " linkage='" << c.linkage
                << "' cxx_runtime='" << c.cxxRuntime << "'";
            auto word = msvc_abi_crt_word(row.tc, staticCrt);
            expect(!word.empty()) << row.label;
            // Exactly one of the two spellings for this driver, and it is
            // the SAME word `flags.cppm`'s compile line and `scan.cpp`'s std
            // BMI mirror both call this helper for — one derivation, so a TU
            // and its std BMI cannot disagree by construction.
            if (row.tc.compiler == CompilerId::MSVC) {
                expect(eq(word, staticCrt ? "/MT" : "/MD")) << row.label;
            } else {
                expect(eq(word, staticCrt ? "-fms-runtime-lib=static"
                                          : "-fms-runtime-lib=dll")) << row.label;
            }
        }
    }
}};

// A shared-PDB word is refused on cl.exe, either dash; other words pass.
UTest zi_zi_and_fs_are_refused_other_words_pass{"CheckSharedPdbWord/ZiZIAndFSAreRefusedOtherWordsPass", [] {
    for (auto* word : {"/Zi", "-Zi", "/ZI", "-ZI", "/FS", "-FS"}) {
        auto refusal = check_shared_pdb_word(word, "[build] cxxflags");
        expect(fatal(refusal.has_value())) << word;
        if (auto&& failed = expect(ne(refusal->find(word), std::string::npos)); !failed) failed << *refusal;
        if (auto&& failed = expect(ne(refusal->find("[build] cxxflags"), std::string::npos)); !failed) failed << *refusal;
        if (auto&& failed = expect(ne(refusal->find("/Z7"), std::string::npos)); !failed) failed << *refusal;
    }
    for (auto* word : {"/Z7", "-Z7", "/Zc:__cplusplus", "/Zp8", "/FSx", "/MT", "-g"})
        expect(!(check_shared_pdb_word(word, "[build] cxxflags").has_value())) << word;
}};

// D3: a free-form CRT word is always a second statement once the model is
// resolved for every MSVC-ABI row.
UTest agreeing_word_is_redundant_contradicting_word_is_refused{"CheckCrtWord/AgreeingWordIsRedundantContradictingWordIsRefused", [] {
    // Recognised spellings, both dash conventions and both drivers.
    for (auto* word : {"/MT", "-MT", "-fms-runtime-lib=static"}) {
        auto agree = check_crt_word(word, /*staticCrt=*/true, "[build] cxxflags");
        expect(fatal(agree.has_value())) << word;
        expect(!agree->contradicts) << word;
        // The key it names states the same model.
        expect(ne(agree->message.find("self-contained"), std::string::npos)) << agree->message;
        auto disagree = check_crt_word(word, /*staticCrt=*/false, "[build] cxxflags");
        expect(fatal(disagree.has_value())) << word;
        expect(bool(disagree->contradicts)) << word;
        if (auto&& failed = expect(ne(disagree->message.find(word), std::string::npos)); !failed) failed << disagree->message;
        if (auto&& failed = expect(ne(disagree->message.find("[build] cxxflags"), std::string::npos)); !failed) failed << disagree->message;
    }
    for (auto* word : {"/MD", "-MD", "-fms-runtime-lib=dll"}) {
        auto agree = check_crt_word(word, /*staticCrt=*/false, "dialect_cxxflags");
        expect(fatal(agree.has_value())) << word;
        expect(!agree->contradicts) << word;
        if (auto&& failed = expect(ne(agree->message.find("toolchain-coupled"), std::string::npos)); !failed) failed << agree->message;
        if (auto&& failed = expect(eq(agree->message.find("self-contained"), std::string::npos)); !failed) failed << agree->message;
        auto disagree = check_crt_word(word, /*staticCrt=*/true, "dialect_cxxflags");
        expect(fatal(disagree.has_value())) << word;
        expect(bool(disagree->contradicts)) << word;
    }
    // A debug CRT word is refused under either model: the model has no debug
    // axis, and the std module and the link use the release CRT.
    for (auto* word : {"/MTd", "-MTd", "/MDd", "-MDd",
                       "-fms-runtime-lib=static_dbg", "-fms-runtime-lib=dll_dbg"}) {
        for (bool staticCrt : {true, false}) {
            auto v = check_crt_word(word, staticCrt, "[build] cxxflags");
            expect(fatal(v.has_value())) << word;
            expect(bool(v->contradicts)) << word;
            if (auto&& failed = expect(ne(v->message.find("debug CRT"), std::string::npos)); !failed) failed << v->message;
        }
    }
    // A word this axis does not recognise says nothing about it.
    expect(!(check_crt_word("-O2", true, "[build] cxxflags").has_value()));
    expect(!(check_crt_word("/EHsc", false, "[build] cxxflags").has_value()));
}};
