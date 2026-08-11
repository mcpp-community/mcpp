#include <gtest/gtest.h>

import std;
import mcpp.source_kind;
import mcpp.toolchain.model;
import mcpp.toolchain.detect;

using mcpp::SourceKind;

// ─── T-1. classify() ───────────────────────────────────────────────────────

TEST(SourceKind, BuiltInTableIsCppmOnly) {
    // Load-bearing, not conservatism. Widening the built-in set widens the
    // DEFAULT source glob with it, so a published package with a vendored
    // MSVC-only `.ixx` under src/ would start compiling it on the next mcpp
    // upgrade — a break its author cannot fix, because that version's tarball
    // has already shipped.
    auto t = mcpp::builtin_extension_table();
    EXPECT_EQ(t.moduleInterface, (std::vector<std::string>{".cppm"}));
    EXPECT_EQ(mcpp::classify("src/a.ixx", t),  SourceKind::Other);
    EXPECT_EQ(mcpp::classify("src/a.ccm", t),  SourceKind::Other);
    EXPECT_EQ(mcpp::classify("src/a.cxxm", t), SourceKind::Other);
}

TEST(SourceKind, ClassifiesEveryBuiltInRole) {
    auto t = mcpp::builtin_extension_table();
    struct Row { const char* path; SourceKind want; };
    const Row rows[] = {
        {"src/a.cppm", SourceKind::ModuleInterface},
        {"src/a.cpp",  SourceKind::Cxx},
        {"src/a.cc",   SourceKind::Cxx},
        {"src/a.cxx",  SourceKind::Cxx},
        {"src/a.mm",   SourceKind::Cxx},
        {"src/a.c",    SourceKind::C},
        {"src/a.m",    SourceKind::C},
        {"src/a.S",    SourceKind::GasAsm},
        {"src/a.s",    SourceKind::GasAsm},
        {"src/a.asm",  SourceKind::NasmAsm},
        {"src/a.h",    SourceKind::Header},
        {"src/a.hpp",  SourceKind::Header},
        {"src/a.hh",   SourceKind::Header},
        {"src/a.hxx",  SourceKind::Header},
        {"src/a.txt",  SourceKind::Other},
        {"src/README", SourceKind::Other},
    };
    for (auto const& r : rows)
        EXPECT_EQ(mcpp::classify(r.path, t), r.want) << r.path;
}

TEST(SourceKind, CaseIsNeverFolded) {
    // `.S` and `.s` are DIFFERENT LANGUAGES here — `.S` goes through the C
    // preprocessor, `.s` does not. Any normalization that lowercases would
    // silently merge them, so extensions are compared literally.
    auto t = mcpp::builtin_extension_table();
    EXPECT_EQ(mcpp::classify("a.S", t), SourceKind::GasAsm);
    EXPECT_EQ(mcpp::classify("a.s", t), SourceKind::GasAsm);
    EXPECT_EQ(mcpp::normalize_extension(".IXX"), ".IXX");
    EXPECT_EQ(mcpp::classify("a.CPPM", t), SourceKind::Other);
}

TEST(SourceKind, ConfiguredExtensionsAreAdditiveAndNormalized) {
    // `ixx` without a dot, a duplicate, and one already built in.
    auto t = mcpp::extension_table_for(
        std::vector<std::string>{"ixx", ".ccm", ".ccm", ".cppm", " .cxxm "});
    EXPECT_EQ(t.moduleInterface,
              (std::vector<std::string>{".cppm", ".ixx", ".ccm", ".cxxm"}));
    EXPECT_EQ(mcpp::classify("src/a.ixx", t),  SourceKind::ModuleInterface);
    EXPECT_EQ(mcpp::classify("src/a.ccm", t),  SourceKind::ModuleInterface);
    EXPECT_EQ(mcpp::classify("src/a.cxxm", t), SourceKind::ModuleInterface);
    // Built-in roles are untouched by the addition.
    EXPECT_EQ(mcpp::classify("src/a.cpp", t), SourceKind::Cxx);
    EXPECT_EQ(mcpp::classify("src/a.c", t),   SourceKind::C);
}

TEST(SourceKind, ReservedExtensionsAreRejectedNotSilentlyAccepted) {
    // Declaring `.c` a module interface has no legitimate use and would route
    // C files to the C++ module rule, failing somewhere that names neither the
    // file nor the key. A hard error, not a warning.
    for (auto const* bad : {".cpp", ".cc", ".cxx", ".c", ".m", ".mm",
                            ".h", ".hpp", ".hh", ".hxx", ".S", ".s", ".asm"}) {
        auto err = mcpp::validate_module_extensions(std::vector<std::string>{bad});
        ASSERT_TRUE(err.has_value()) << bad;
        EXPECT_NE(err->find(bad), std::string::npos) << *err;
    }
    // An unusual-but-free extension is fine: mcpp tells the compiler what the
    // unit is rather than relying on the driver to recognize the suffix.
    EXPECT_FALSE(mcpp::validate_module_extensions(
        std::vector<std::string>{".mpp", ".cppmi"}).has_value());
}

TEST(SourceKind, ValidationRejectsNonExtensionShapes) {
    for (auto const* bad : {"", "  ", ".", "src/*.ixx", "a/b.ixx",
                            "foo.bar.ixx"}) {
        EXPECT_TRUE(mcpp::validate_module_extensions(
            std::vector<std::string>{bad}).has_value()) << "accepted: " << bad;
    }
}

TEST(SourceKind, PredicatesAgreeWithTheKind) {
    EXPECT_TRUE(mcpp::produces_bmi(SourceKind::ModuleInterface));
    EXPECT_TRUE(mcpp::links_unconditionally(SourceKind::ModuleInterface));
    for (auto k : {SourceKind::Cxx, SourceKind::C, SourceKind::GasAsm,
                   SourceKind::NasmAsm, SourceKind::Header, SourceKind::Other}) {
        EXPECT_FALSE(mcpp::produces_bmi(k));
        EXPECT_FALSE(mcpp::links_unconditionally(k));
    }

    // Scan-exempt: cannot contain import/module, so no P1689 scan.
    EXPECT_TRUE(mcpp::is_scan_exempt(SourceKind::C));
    EXPECT_TRUE(mcpp::is_scan_exempt(SourceKind::GasAsm));
    EXPECT_TRUE(mcpp::is_scan_exempt(SourceKind::NasmAsm));
    EXPECT_FALSE(mcpp::is_scan_exempt(SourceKind::ModuleInterface));
    EXPECT_FALSE(mcpp::is_scan_exempt(SourceKind::Cxx));

    // The fast path's question: could editing this change the graph's SHAPE?
    // Assembly is absent on purpose — it has no import and no scanned include
    // graph, so editing one changes its object (ninja tracks that) and nothing
    // else. A NEW assembly file is a different question, answered by
    // glob_inputs_stale.
    EXPECT_TRUE(mcpp::affects_graph_shape(SourceKind::ModuleInterface));
    EXPECT_TRUE(mcpp::affects_graph_shape(SourceKind::Cxx));
    EXPECT_TRUE(mcpp::affects_graph_shape(SourceKind::C));
    EXPECT_TRUE(mcpp::affects_graph_shape(SourceKind::Header));
    EXPECT_FALSE(mcpp::affects_graph_shape(SourceKind::GasAsm));
    EXPECT_FALSE(mcpp::affects_graph_shape(SourceKind::Other));
}

TEST(SourceKind, DefaultGlobsAreDerivedFromTheTable) {
    // Two hand-maintained copies of this list used to exist and had already
    // drifted apart (the staging fallback was missing all three assembly
    // extensions). Deriving them is what keeps a declared extension from being
    // classified but never FOUND.
    auto builtin = mcpp::default_source_globs(mcpp::builtin_extension_table());
    EXPECT_EQ(builtin, (std::vector<std::string>{
        "src/**/*.cppm", "src/**/*.cpp", "src/**/*.cc", "src/**/*.c",
        "src/**/*.S", "src/**/*.s", "src/**/*.asm"}));

    auto wide = mcpp::default_source_globs(
        mcpp::extension_table_for(std::vector<std::string>{".ixx"}));
    EXPECT_EQ(wide.front(), "src/**/*.cppm");
    EXPECT_NE(std::ranges::find(wide, "src/**/*.ixx"), wide.end());

    EXPECT_EQ(mcpp::default_source_globs_note(mcpp::builtin_extension_table()),
              "sources [src/**/*.{cppm,cpp,cc,c,S,s,asm}]");
}

// ─── T-2. Object naming ────────────────────────────────────────────────────

TEST(SourceKind, ObjectNamingIsMonotoneAndCollisionFree) {
    // Historical names are FROZEN: an object's name is part of the internal
    // layout of a global cache entry, so renaming one without changing the
    // cache key produces a HIT on an entry that lacks the object the link then
    // asks for — a missing `.o` at link time, not a cache miss.
    EXPECT_EQ(mcpp::object_naming_for("foo.cpp"),  mcpp::ObjectNaming::Stem);
    EXPECT_EQ(mcpp::object_naming_for("foo.cc"),   mcpp::ObjectNaming::Stem);
    EXPECT_EQ(mcpp::object_naming_for("foo.c"),    mcpp::ObjectNaming::Stem);
    EXPECT_EQ(mcpp::object_naming_for("foo.cppm"), mcpp::ObjectNaming::StemDotM);
    EXPECT_EQ(mcpp::object_naming_for("foo.S"),    mcpp::ObjectNaming::FullFilename);
    EXPECT_EQ(mcpp::object_naming_for("foo.asm"),  mcpp::ObjectNaming::FullFilename);

    // Everything a project can ADD gets the collision-proof form, so a new
    // extension can never change an existing object's name.
    for (auto const* ext : {"foo.ixx", "foo.ccm", "foo.cxxm", "foo.mpp"})
        EXPECT_EQ(mcpp::object_naming_for(ext), mcpp::ObjectNaming::FullFilename)
            << ext;
}

TEST(SourceKind, SameStemAcrossModuleExtensionsNeverCollides) {
    // mcpp#272 proposed giving all four module extensions the `.m` prefix,
    // which makes `foo.cppm` and `foo.ccm` BOTH `foo.m.o`. The per-package
    // collision prefix cannot help: it mirrors the source DIRECTORY, and these
    // two are in the same one.
    auto name = [](std::string_view f) {
        switch (mcpp::object_naming_for(f)) {
            case mcpp::ObjectNaming::Stem:
                return std::filesystem::path(f).stem().string() + ".o";
            case mcpp::ObjectNaming::StemDotM:
                return std::filesystem::path(f).stem().string() + ".m.o";
            case mcpp::ObjectNaming::FullFilename:
                return std::filesystem::path(f).filename().string() + ".o";
        }
        return std::string{};
    };
    std::set<std::string> seen;
    for (auto const* f : {"foo.cppm", "foo.ccm", "foo.cxxm", "foo.ixx",
                          "foo.cpp", "foo.S", "foo.s", "foo.asm"}) {
        auto n = name(f);
        EXPECT_TRUE(seen.insert(n).second) << "collision: " << f << " -> " << n;
    }

    // ⚠️ KNOWN GAP, deliberately pinned rather than asserted away: `foo.c` and
    // `foo.cpp` in one directory have always shared `foo.o`. Fixing it renames
    // every C object, which is exactly the cache-layout change described
    // above and needs a cache-key revision to be safe. Tracked separately —
    // this assertion documents the gap so nobody "fixes" the test instead.
    EXPECT_EQ(name("foo.c"), name("foo.cpp"))
        << "known gap closed? update this test and bump the cache key";
}

// ─── T-8. The module-interface language flag ───────────────────────────────

TEST(SourceKind, ModuleInterfaceLangFlagIsPerCompilerAndNotInterchangeable) {
    // Measured 2026-08-11: `-x c++-module` makes GCC exit with "language
    // c++-module not recognized", and `-x c++` makes Clang emit a 174-byte
    // stub instead of a module BMI. The spelling is a property of the compiler
    // FAMILY, not of the command dialect — gcc and clang share the gnu dialect.
    auto traits_for = [](mcpp::toolchain::CompilerId id) {
        mcpp::toolchain::Toolchain tc;
        tc.compiler = id;
        return mcpp::toolchain::bmi_traits(tc);
    };
    EXPECT_EQ(traits_for(mcpp::toolchain::CompilerId::GCC).moduleInterfaceLangFlag,
              " -x c++");
    EXPECT_EQ(traits_for(mcpp::toolchain::CompilerId::Clang).moduleInterfaceLangFlag,
              " -x c++-module");
    EXPECT_EQ(traits_for(mcpp::toolchain::CompilerId::MSVC).moduleInterfaceLangFlag,
              " /interface /TP");

    // Never empty: mcpp tells the driver EVERY time rather than tracking which
    // suffix each driver version happens to know. That table would expire with
    // every compiler release, and getting it wrong is silent — Clang hands an
    // unrecognized suffix to the linker, warns, and exits 0 with no BMI.
    for (auto id : {mcpp::toolchain::CompilerId::GCC,
                    mcpp::toolchain::CompilerId::Clang,
                    mcpp::toolchain::CompilerId::MSVC})
        EXPECT_FALSE(traits_for(id).moduleInterfaceLangFlag.empty());
}
