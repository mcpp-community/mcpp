#include <gtest/gtest.h>

import std;
import mcpp.build.compile_commands;
import mcpp.build.flags;
import mcpp.build.ninja;
import mcpp.build.plan;
import mcpp.manifest;
import mcpp.toolchain.model;
import mcpp.platform;

using namespace mcpp::build;

namespace {

std::size_t count_occurrences(std::string_view haystack, std::string_view needle) {
    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string_view::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

std::string escaped_include_flag(const std::filesystem::path& path) {
    auto s = path.string();
    std::string escaped;
    escaped.reserve(s.size());
    for (char c : s) {
        if (c == ' ' || c == '$' || c == ':')
            escaped.push_back('$');
        escaped.push_back(c);
    }
    return "-I" + escaped;
}

BuildPlan minimal_plan() {
    BuildPlan plan;
    plan.projectRoot = std::filesystem::temp_directory_path() / "mcpp-ninja-test";
    plan.outputDir = plan.projectRoot / "target" / "test";
    plan.manifest.package.name = "objc_rule_test";
    plan.manifest.buildConfig.cStandard = "c11";
    plan.toolchain.compiler = mcpp::toolchain::CompilerId::GCC;
    plan.toolchain.version = "test";
    plan.toolchain.binaryPath = "/usr/bin/g++";
    plan.toolchain.targetTriple = "x86_64-linux-gnu";
    return plan;
}

}  // namespace

TEST(NinjaBackend, ObjectiveCSourceUsesCObjectRuleAndCFlags) {
    auto plan = minimal_plan();
    plan.compileUnits.push_back({
        .source = "src/cocoa.m",
        .object = "obj/cocoa.o",
        .packageName = "objc_rule_test",
        .packageCflags = {"-DOBJ_C_BUILD=1"},
        .packageCxxflags = {"-DWRONG_CXX_FLAG=1"},
    });

    auto ninja = emit_ninja_string(plan);

    EXPECT_NE(ninja.find("build obj/cocoa.o : c_object src/cocoa.m"), std::string::npos)
        << ninja;
    EXPECT_EQ(ninja.find("build obj/cocoa.o : cxx_object src/cocoa.m"), std::string::npos)
        << ninja;
    EXPECT_NE(ninja.find("unit_cflags = -DOBJ_C_BUILD=1"), std::string::npos)
        << ninja;
    EXPECT_EQ(ninja.find("unit_cxxflags = -DWRONG_CXX_FLAG=1"), std::string::npos)
        << ninja;
}

// mcpp#235: cxx_module/cxx_object must track header/purview/GMF includes via
// a GNU-style depfile on non-MSVC toolchains (this test's plan uses GCC on a
// non-Windows host, so posixDepfile is true). Before the fix, neither rule
// had ANY depfile outside the msvcDeps branch, so editing a file #include'd
// inside a module's purview (or a header pulled in by a .cpp) never
// invalidated the compile edge. The depfile is routed through a scratch
// `$out.d.raw` + `awk` filter (not written directly to `$out.d`) because
// GCC's `-fmodules` bolts non-standard "reversed" module rules onto the raw
// -MMD output that ninja's depfile loader rejects — see the long comment at
// the definition site for the empirically-confirmed failure mode.
TEST(NinjaBackend, CxxModuleAndCxxObjectRulesTrackHeaderDepsViaGccDepfile) {
    // The filtered gcc depfile (#235) is POSIX-only: `posixDepfile =
    // !msvcDeps && !is_windows` (awk isn't available on native Windows, and
    // MSVC uses `deps = msvc` instead). This asserts the POSIX emission.
    if constexpr (mcpp::platform::is_windows)
        GTEST_SKIP() << "gcc depfile filter is POSIX-only (Windows uses deps=msvc)";

    auto plan = minimal_plan();

    auto ninja = emit_ninja_string(plan);

    auto module_rule_start = ninja.find("rule cxx_module");
    auto object_rule_start = ninja.find("rule cxx_object");
    ASSERT_NE(module_rule_start, std::string::npos) << ninja;
    ASSERT_NE(object_rule_start, std::string::npos) << ninja;
    ASSERT_LT(module_rule_start, object_rule_start) << ninja;

    auto module_rule = ninja.substr(module_rule_start, object_rule_start - module_rule_start);
    auto object_rule = ninja.substr(object_rule_start);

    for (auto const& rule : {module_rule, object_rule}) {
        EXPECT_NE(rule.find("-MMD -MF $out.d.raw"), std::string::npos) << ninja;
        EXPECT_NE(rule.find("deps = gcc"), std::string::npos) << ninja;
        EXPECT_NE(rule.find("depfile = $out.d\n"), std::string::npos) << ninja;
        // The raw compiler depfile (with GCC's module-specific reversed
        // rules) must never be bound directly as ninja's depfile.
        EXPECT_EQ(rule.find("depfile = $out.d.raw"), std::string::npos) << ninja;
    }
}

TEST(NinjaBackend, UsesPackageCppStandardForCxxFlags) {
    auto plan = minimal_plan();
    plan.manifest.package.standard = "c++26";
    plan.manifest.language.standard = "c++26";
    plan.cppStandard = "c++26";
    plan.cppStandardFlag = "-std=c++26";
    plan.compileUnits.push_back({
        .source = "src/main.cpp",
        .object = "obj/main.o",
        .packageName = "cpp26_test",
    });

    auto ninja = emit_ninja_string(plan);

    EXPECT_NE(ninja.find("cxxflags  = -std=c++26"), std::string::npos)
        << ninja;
    EXPECT_EQ(ninja.find("-std=c++23"), std::string::npos)
        << ninja;
}

TEST(NinjaBackend, CompileCommandsUsesSameCppStandard) {
    auto plan = minimal_plan();
    plan.manifest.package.standard = "c++26";
    plan.manifest.language.standard = "c++26";
    plan.cppStandard = "c++26";
    plan.cppStandardFlag = "-std=c++26";
    plan.compileUnits.push_back({
        .source = "src/main.cpp",
        .object = "obj/main.o",
        .packageName = "cpp26_test",
    });

    auto flags = compute_flags(plan);
    auto cdb = emit_compile_commands(plan, flags);

    EXPECT_NE(cdb.find("\"-std=c++26\""), std::string::npos)
        << cdb;
    EXPECT_EQ(cdb.find("\"-std=c++23\""), std::string::npos)
        << cdb;
}

TEST(NinjaBackend, CxxFlagsIncludeBuildIncludeDirs) {
    auto plan = minimal_plan();
    plan.manifest.buildConfig.includeDirs = {"include", "third_party/imgui"};

    auto flags = compute_flags(plan);

    EXPECT_NE(flags.cxx.find(escaped_include_flag(plan.projectRoot / "include")),
              std::string::npos)
        << flags.cxx;
    EXPECT_NE(flags.cxx.find(escaped_include_flag(
                  plan.projectRoot / std::filesystem::path{"third_party/imgui"})),
              std::string::npos)
        << flags.cxx;
}

// Cluster A review fix (#226/#234 follow-up): `[build] include_dirs` is a
// TYPED PATH channel — bare paths from the manifest, dialect prefix applied
// at emission (-I under GNU, /I under MSVC) — not the FLAG-STRING channel
// that normalize_include_flags serves (cflags/cxxflags, where the prefix is
// already embedded in the string by the scanner). Routing dialect-prefixed
// include tokens through normalize_include_flags (whose prefix table only
// knows GNU spellings: -I/-iquote/-isystem/-idirafter/-iprefix/-L) silently
// no-ops under MSVC: "/Iinclude" matches no table entry and is never
// rewritten against plan.projectRoot, so it survives as a *relative* path —
// but ninja runs with cwd = the output dir, so the include stops resolving.
// The fix absolutizes the path directly (dialect-agnostic) before
// prepending the dialect prefix. This test would FAIL before the fix
// (emitting the literal, unrewritten "/Iinclude") and passes after.
TEST(NinjaBackend, MsvcIncludeDirsAreAbsolutizedNotGnuNormalized) {
    // The MSVC-dialect logic under test is host-independent; run it on POSIX
    // where the test's temp projectRoot has no drive letter. On Windows the
    // runner's `C:\...` temp path gets its `:` ninja-escaped (`C$:`), which
    // would need escape-aware matching unrelated to what this test verifies.
    if constexpr (mcpp::platform::is_windows)
        GTEST_SKIP() << "MSVC-dialect path check runs on POSIX (avoids Windows drive-colon ninja escaping)";

    auto plan = minimal_plan();
    plan.toolchain.compiler = mcpp::toolchain::CompilerId::MSVC;
    plan.toolchain.binaryPath = "cl.exe";
    plan.toolchain.targetTriple = "x86_64-pc-windows-msvc";
    plan.manifest.buildConfig.includeDirs = {"include"};

    auto flags = compute_flags(plan);

    auto expected = "/I" + (plan.projectRoot / "include").string();
    EXPECT_NE(flags.cxx.find(expected), std::string::npos) << flags.cxx;
    // The un-rewritten, still-relative token must never appear.
    EXPECT_EQ(flags.cxx.find("/Iinclude"), std::string::npos) << flags.cxx;
}

// ── assembly sources (.S/.s → asm_object via $cc, .asm → nasm_object) ────────

TEST(NinjaBackend, GasSourceUsesAsmObjectRule) {
    auto plan = minimal_plan();
    plan.compileUnits.push_back({
        .source = "src/copy.S",
        .object = "obj/copy.S.o",
        .packageName = "asm_rule_test",
        // Only the -D/-U/-I subset may reach the assembler: -std/-O/-w on an
        // asm command line are driver noise (or errors).
        .packageCflags = {"-DHAVE_ASM=1", "-std=c99", "-O2", "-w"},
        .packageCxxflags = {"-DWRONG_CXX_FLAG=1"},
    });

    auto ninja = emit_ninja_string(plan);

    EXPECT_NE(ninja.find("rule asm_object"), std::string::npos) << ninja;
    EXPECT_NE(ninja.find("build obj/copy.S.o : asm_object src/copy.S"),
              std::string::npos) << ninja;
    EXPECT_NE(ninja.find("cc        = "), std::string::npos) << ninja;
    EXPECT_NE(ninja.find("unit_asmflags = -DHAVE_ASM=1\n"), std::string::npos)
        << ninja;
    EXPECT_EQ(ninja.find("unit_asmflags = -DHAVE_ASM=1 -std=c99"), std::string::npos)
        << ninja;
    // The global asm flag string must not carry a C standard or opt level.
    auto asmline_pos = ninja.find("asmflags  =");
    ASSERT_NE(asmline_pos, std::string::npos) << ninja;
    auto asmline = ninja.substr(asmline_pos, ninja.find('\n', asmline_pos) - asmline_pos);
    EXPECT_EQ(asmline.find("-std="), std::string::npos) << asmline;
    EXPECT_EQ(asmline.find("-O"), std::string::npos) << asmline;
}

TEST(NinjaBackend, NasmSourceUsesNasmRuleWithDerivedFormat) {
    auto plan = minimal_plan();
    plan.nasmPath = "/opt/bin/nasm";
    plan.nasmFormat = "elf64";
    plan.compileUnits.push_back({
        .source = "src/simd.asm",
        .object = "obj/simd.asm.o",
        .packageName = "nasm_rule_test",
        .packageCflags = {"-DHAVE_AVX2=1", "-O2"},
    });

    auto ninja = emit_ninja_string(plan);

    EXPECT_NE(ninja.find("rule nasm_object"), std::string::npos) << ninja;
    EXPECT_NE(ninja.find("nasm      = /opt/bin/nasm"), std::string::npos) << ninja;
    EXPECT_NE(ninja.find("nasmfmt   = elf64"), std::string::npos) << ninja;
    EXPECT_NE(ninja.find("build obj/simd.asm.o : nasm_object src/simd.asm"),
              std::string::npos) << ninja;
    EXPECT_NE(ninja.find("unit_asmflags = -DHAVE_AVX2=1\n"), std::string::npos)
        << ninja;
}

TEST(NinjaBackend, NoAsmRulesWithoutAsmSources) {
    auto plan = minimal_plan();
    plan.compileUnits.push_back({
        .source = "src/main.cpp",
        .object = "obj/main.o",
        .packageName = "plain_test",
    });

    auto ninja = emit_ninja_string(plan);

    EXPECT_EQ(ninja.find("rule asm_object"), std::string::npos) << ninja;
    EXPECT_EQ(ninja.find("rule nasm_object"), std::string::npos) << ninja;
}

TEST(NinjaBackend, CompileCommandsSkipNasmAndCoverGas) {
    auto plan = minimal_plan();
    plan.nasmPath = "/opt/bin/nasm";
    plan.nasmFormat = "elf64";
    plan.compileUnits.push_back({
        .source = "src/simd.asm",
        .object = "obj/simd.asm.o",
        .packageName = "cdb_test",
    });
    plan.compileUnits.push_back({
        .source = "src/copy.S",
        .object = "obj/copy.S.o",
        .packageName = "cdb_test",
    });

    auto flags = compute_flags(plan);
    auto cdb = emit_compile_commands(plan, flags);

    // NASM command lines are meaningless to CDB consumers (clangd) — excluded.
    EXPECT_EQ(cdb.find("simd.asm"), std::string::npos) << cdb;
    // GAS units ride the C driver and stay in the CDB.
    EXPECT_NE(cdb.find("copy.S"), std::string::npos) << cdb;
    EXPECT_EQ(cdb.find("\"-std=c11\""), std::string::npos) << cdb;   // asm-safe flags, no C std
}

// mcpp#234: each packageCflags/packageCxxflags element is already one argv
// token — apply_glob_flags pushes a define like `T=long long` as the single
// element `-DT=long long`. join_flags previously joined tokens with a bare
// space and zero quoting, so once ninja resolved the command line and handed
// it to the shell, the embedded space split `-DT=long long` into TWO words
// (`-DT=long` and a bare `long`). The emitted unit_cflags line must carry the
// define as a single shell-quoted token.
TEST(NinjaBackend, QuotesFlagValueWithSpace) {
    auto plan = minimal_plan();
    plan.compileUnits.push_back({
        .source = "src/main.c",
        .object = "obj/main.o",
        .packageName = "quote_test",
        .packageCflags = {"-DT=long long"},
    });

    auto ninja = emit_ninja_string(plan);

    // shell_quote_arg wraps in single quotes on POSIX, double quotes on
    // Windows — assert the platform-appropriate spelling.
    const std::string quoted = mcpp::platform::is_windows
        ? "unit_cflags = \"-DT=long long\""
        : "unit_cflags = '-DT=long long'";
    EXPECT_NE(ninja.find(quoted), std::string::npos) << ninja;
    // Must NOT appear as two bare, unquoted words split on the space.
    EXPECT_EQ(ninja.find("unit_cflags = -DT=long long"), std::string::npos)
        << ninja;
}

// Plain framework-shaped flags with nothing shell-significant must pass
// through byte-for-byte unquoted (no over-quoting regression).
TEST(NinjaBackend, PlainFlagsPassThroughUnquoted) {
    auto plan = minimal_plan();
    plan.compileUnits.push_back({
        .source = "src/main.cpp",
        .object = "obj/main.o",
        .packageName = "plain_flag_test",
        .packageCxxflags = {"-DFOO=1", "-O2"},
    });

    auto ninja = emit_ninja_string(plan);

    EXPECT_NE(ninja.find("unit_cxxflags = -DFOO=1 -O2"), std::string::npos)
        << ninja;
}

// Regression: mcpp-GENERATED per-unit LINK flags are already correctly
// shell-quoted + ninja-escaped at construction — e.g. the shared-dep rpath
// token `-Wl,-rpath,'$$ORIGIN'` (single quotes stop shell $-expansion, `$$`
// is ninja's literal `$`). join_flags for link flags must NOT re-run
// shell_quote_arg over them: doing so double-quotes the token, baking a
// literal `'$ORIGIN'` (quotes included) into the binary's RUNPATH so the
// dynamic linker can't find dependency .so's next to the exe (e2e 55-57/64).
TEST(NinjaBackend, LinkFlagsAreNotReQuoted) {
    auto plan = minimal_plan();
    plan.compileUnits.push_back({
        .source = "src/main.cpp",
        .object = "obj/main.o",
        .packageName = "rpath_test",
    });
    plan.linkUnits.push_back({
        .targetName = "app",
        .kind = mcpp::build::LinkUnit::Binary,
        .objects = {"obj/main.o"},
        .linkFlags = {"-Wl,-rpath,'$$ORIGIN'"},
        .output = "bin/app",
        .entryMain = "src/main.cpp",
    });

    auto ninja = emit_ninja_string(plan);

    // The rpath token passes through verbatim (ninja `$$` = literal `$`).
    EXPECT_NE(ninja.find("-Wl,-rpath,'$$ORIGIN'"), std::string::npos) << ninja;
    // Must NOT be double-quoted: shell_quote_arg escaping an embedded `'`
    // produces the `'\''` sequence, which only appears if it re-quoted.
    EXPECT_EQ(ninja.find("'\\''"), std::string::npos) << ninja;
}

// Regression (#234 follow-up): a raw descriptor flag that packs two argv
// tokens into one string — e.g. compat.lua's `-include <header>` — must pass
// through VERBATIM so the shell splits it back into `-include` + the header.
// Blanket shell-quoting wrapped it into one malformed arg, so gcc looked for a
// file literally named "<space>header" → "No such file" → aarch64/macos/windows
// cross-builds failed. Only `-D`/`/D` define tokens with an intra-value space
// get quoted; `-include foo.h` does not.
TEST(NinjaBackend, RawMultiTokenFlagIsNotQuoted) {
    auto plan = minimal_plan();
    plan.compileUnits.push_back({
        .source = "src/main.c",
        .object = "obj/main.o",
        .packageName = "include_flag_test",
        .packageCflags = {"-include mcpp_lua_platform_config.h"},
    });

    auto ninja = emit_ninja_string(plan);

    // Verbatim, so the shell re-splits into two args.
    EXPECT_NE(ninja.find("unit_cflags = -include mcpp_lua_platform_config.h"),
              std::string::npos) << ninja;
    // Must NOT be wrapped in quotes (which would make it one malformed arg).
    EXPECT_EQ(ninja.find("'-include mcpp_lua_platform_config.h'"),
              std::string::npos) << ninja;
}

// mcpp#247: driver-style (gnu dialect) link/archive/shared rules must route
// the object list through a response file on Windows — every command spawns
// via CreateProcess (32 KiB command-line ceiling), and ffmpeg/opencv-class
// source packages link thousands of objects, so an inlined $in overflows it.
// POSIX keeps the inline form byte-identical (ARG_MAX is ample).
TEST(NinjaBackend, DriverStyleLinkRulesUseRspfileOnWindowsOnly) {
    auto plan = minimal_plan();  // GCC → gnu dialect → driver-style branch

    auto ninja = emit_ninja_string(plan);

    for (std::string_view rule : {"rule cxx_link\n", "rule cxx_archive\n",
                                  "rule cxx_shared\n"}) {
        auto start = ninja.find(rule);
        ASSERT_NE(start, std::string::npos) << ninja;
        auto end = ninja.find("\n\n", start);
        ASSERT_NE(end, std::string::npos) << ninja;
        auto body = ninja.substr(start, end - start);
        if constexpr (mcpp::platform::is_windows) {
            EXPECT_NE(body.find("@$out.rsp"), std::string::npos) << body;
            EXPECT_NE(body.find("rspfile = $out.rsp"), std::string::npos) << body;
            EXPECT_NE(body.find("rspfile_content = $in"), std::string::npos) << body;
        } else {
            EXPECT_EQ(body.find("rspfile"), std::string::npos) << body;
            EXPECT_NE(body.find("$in"), std::string::npos) << body;
        }
    }
}

TEST(NinjaBackend, RootPackageCxxflagsAreEmittedOncePerUnit) {
    auto plan = minimal_plan();
    plan.manifest.buildConfig.cxxflags = {"-DROOT_FLAG=1"};
    plan.compileUnits.push_back({
        .source = "src/main.cpp",
        .object = "obj/main.o",
        .packageName = "root_flag_test",
        .packageCxxflags = {"-DROOT_FLAG=1"},
    });

    auto ninja = emit_ninja_string(plan);

    EXPECT_EQ(count_occurrences(ninja, "unit_cxxflags = -DROOT_FLAG=1"), 2u)
        << ninja;
    EXPECT_EQ(ninja.find("cxxflags  = -std=c++23 -O2 -DROOT_FLAG=1"), std::string::npos)
        << ninja;
}
