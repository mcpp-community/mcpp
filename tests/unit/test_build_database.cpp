// The rules docs/specs/build-database.md (SPEC-005) states that can be pinned
// without planning a project: the role of each declaration form, the standard
// library names, the toolchain id, and the recovery of the std units' argument
// vectors from the commands mcpp renders. The recovery tests drive the REAL
// command builders, so a builder that changes shape fails here rather than in
// a document an editor reads.

#include <gtest/gtest.h>

import std;
import mcpp.build.build_database;
import mcpp.modgraph.graph;
import mcpp.platform;
import mcpp.toolchain.clang;
import mcpp.toolchain.gcc;
import mcpp.toolchain.model;
import mcpp.toolchain.msvc;

namespace db = mcpp::build::database;
using mcpp::modgraph::ModuleDeclaration;
using namespace mcpp::toolchain;

namespace {

const bool kWindows = mcpp::platform::is_windows;

std::filesystem::path host_path(std::string_view posix, std::string_view windows) {
    return std::filesystem::path{std::string(kWindows ? windows : posix)};
}

bool contains(const std::vector<std::string>& words, const std::string& w) {
    return std::ranges::find(words, w) != words.end();
}

} // namespace

TEST(BuildDatabase, RoleOfEachDeclarationForm) {
    EXPECT_EQ(db::role_name(ModuleDeclaration::None), "non-module");
    EXPECT_EQ(db::role_name(ModuleDeclaration::Interface), "module-interface");
    EXPECT_EQ(db::role_name(ModuleDeclaration::InterfacePartition),
              "module-partition-interface");
    EXPECT_EQ(db::role_name(ModuleDeclaration::ImplementationPartition),
              "module-partition-implementation");
    EXPECT_EQ(db::role_name(ModuleDeclaration::Implementation), "module-implementation");
    EXPECT_EQ(db::role_name(ModuleDeclaration::Unknown), "unknown");
}

TEST(BuildDatabase, StandardLibraryNames) {
    EXPECT_EQ(db::stdlib_name("libstdc++"), "libstdc++");
    EXPECT_EQ(db::stdlib_name("libc++"), "libc++");
    EXPECT_EQ(db::stdlib_name("msvc-stl"), "msvc-stl");
    EXPECT_EQ(db::stdlib_name("newlib"), "other");
}

TEST(BuildDatabase, ToolchainIdNamesFamilyVersionAndTheCompilersTriple) {
    Toolchain tc;
    tc.compiler = CompilerId::Clang;
    tc.version = "22.1.8";
    tc.targetTriple = "x86_64-windows-msvc";
    // mcpp's family name in the opaque id (`llvm`, as in `llvm@22.1.8`), and the
    // triple as the compiler spells it; the S1 `family` field says `clang`.
    EXPECT_EQ(db::toolchain_id(tc, "x86_64-pc-windows-msvc"),
              "llvm-22.1.8-x86_64-pc-windows-msvc");
    EXPECT_EQ(tc.compiler_name(), "clang");
}

TEST(BuildDatabase, PosixWordsUndoShellQuoting) {
    auto w = db::split_command_words(
        "cd '/c d' && env LD_LIBRARY_PATH='/r t' 'it'\\''s' \"a\\\"b\" x\\ y 2>&1",
        /*windows=*/false);
    std::vector<std::string> want{"cd", "/c d", "&&", "env", "LD_LIBRARY_PATH=/r t",
                                  "it's", "a\"b", "x y", "2>&1"};
    EXPECT_EQ(w, want);
}

TEST(BuildDatabase, WindowsWordsFollowTheRuntimeRules) {
    auto w = db::split_command_words(
        "cd /d \"C:\\a b\" && \"C:\\t\\clang++.exe\" \"x\\\\\\\"y\" C:\\s\\std.ixx 2>&1",
        /*windows=*/true);
    std::vector<std::string> want{"cd", "/d", "C:\\a b", "&&", "C:\\t\\clang++.exe",
                                  "x\\\"y", "C:\\s\\std.ixx", "2>&1"};
    EXPECT_EQ(w, want);
}

TEST(BuildDatabase, RecoversTheGccStdUnitFromItsBuilder) {
    Toolchain tc;
    tc.compiler = CompilerId::GCC;
    tc.version = "16.1.0";
    tc.binaryPath = host_path("/opt/my tools/bin/g++", "C:\\my tools\\bin\\g++.exe");
    tc.targetTriple = "x86_64-linux-gnu";
    tc.stdModuleSource = host_path("/opt/my tools/include/c++/16.1.0/bits/std.cc",
                                   "C:\\my tools\\include\\c++\\16.1.0\\bits\\std.cc");
    tc.compilerRuntimeDirs = { host_path("/opt/rt lib", "C:\\rt") };
    const auto cache = host_path("/home/u/.mcpp/cache/std/k1", "C:\\u\\.mcpp\\std\\k1");

    auto commands = gcc::std_module_build_commands(tc, cache, "", "-std=c++23");
    ASSERT_FALSE(commands.empty());
    auto inv = db::recover_invocation(commands, tc.stdModuleSource, tc.binaryPath,
                                      "/unused", kWindows);
    ASSERT_TRUE(inv.has_value()) << commands.front();
    EXPECT_EQ(inv->workDirectory, cache) << commands.front();
    ASSERT_FALSE(inv->arguments.empty());
    EXPECT_EQ(inv->arguments.front(), tc.binaryPath.string()) << commands.front();
    EXPECT_TRUE(contains(inv->arguments, tc.stdModuleSource.string())) << commands.front();
    EXPECT_TRUE(contains(inv->arguments, "-std=c++23")) << commands.front();
    EXPECT_FALSE(contains(inv->arguments, "2>&1")) << commands.front();
    EXPECT_FALSE(contains(inv->arguments, "env")) << commands.front();
    for (auto const& a : inv->arguments)
        EXPECT_FALSE(a.starts_with("LD_LIBRARY_PATH=")) << a;
}

TEST(BuildDatabase, RecoversTheClangStdUnitsFromTheirBuilders) {
    Toolchain tc;
    tc.compiler = CompilerId::Clang;
    tc.version = "22.1.8";
    tc.binaryPath = host_path("/opt/llvm/bin/clang++", "C:\\llvm\\bin\\clang++.exe");
    tc.targetTriple = "x86_64-linux-gnu";
    tc.stdModuleSource = host_path("/opt/llvm/share/libc++/v1/std.cppm",
                                   "C:\\llvm\\share\\libc++\\v1\\std.cppm");
    tc.stdCompatSource = host_path("/opt/llvm/share/libc++/v1/std.compat.cppm",
                                   "C:\\llvm\\share\\libc++\\v1\\std.compat.cppm");
    const auto cache = host_path("/home/u/.mcpp/cache/std/k2", "C:\\u\\.mcpp\\std\\k2");
    const auto bmi   = cache / "pcm.cache" / "std.pcm";

    auto stdCommands = clang::std_module_build_commands(tc, cache, bmi, "", "-std=c++23");
    auto inv = db::recover_invocation(stdCommands, tc.stdModuleSource, tc.binaryPath,
                                      cache, kWindows);
    ASSERT_TRUE(inv.has_value()) << stdCommands.front();
    EXPECT_EQ(inv->arguments.front(), tc.binaryPath.string());
    EXPECT_TRUE(contains(inv->arguments, "--precompile")) << stdCommands.front();
    EXPECT_TRUE(contains(inv->arguments, tc.stdModuleSource.string()));
    EXPECT_EQ(inv->workDirectory, cache);

    auto compatCommands = clang::std_compat_build_commands(
        tc, cache, clang::std_compat_bmi_path(cache), bmi, "", "-std=c++23");
    auto compat = db::recover_invocation(compatCommands, tc.stdCompatSource,
                                         tc.binaryPath, cache, kWindows);
    ASSERT_TRUE(compat.has_value()) << compatCommands.front();
    EXPECT_TRUE(contains(compat->arguments, tc.stdCompatSource.string()));
}

TEST(BuildDatabase, RecoversTheMsvcStdUnitsFromTheirBuilders) {
    Toolchain tc;
    tc.compiler = CompilerId::MSVC;
    tc.version = "19.44.35207";
    tc.binaryPath = host_path(
        "/vs/VC/Tools/MSVC/14.44.35207/bin/Hostx64/x64/cl.exe",
        "C:\\Program Files\\Microsoft Visual Studio\\VC\\Tools\\MSVC\\14.44.35207\\bin\\Hostx64\\x64\\cl.exe");
    tc.targetTriple = "x86_64-windows-msvc";
    tc.stdModuleSource = host_path("/vs/VC/Tools/MSVC/14.44.35207/modules/std.ixx",
        "C:\\Program Files\\Microsoft Visual Studio\\VC\\Tools\\MSVC\\14.44.35207\\modules\\std.ixx");
    tc.stdCompatSource = host_path("/vs/VC/Tools/MSVC/14.44.35207/modules/std.compat.ixx",
        "C:\\Program Files\\Microsoft Visual Studio\\VC\\Tools\\MSVC\\14.44.35207\\modules\\std.compat.ixx");
    const auto cache = host_path("/home/u/.mcpp/cache/std/k3", "C:\\u\\.mcpp\\std\\k3");

    auto stdCommands = msvc::std_module_build_commands(tc, cache, "/std:c++latest", "/MD");
    auto inv = db::recover_invocation(stdCommands, tc.stdModuleSource, tc.binaryPath,
                                      "/unused", kWindows);
    ASSERT_TRUE(inv.has_value()) << stdCommands.front();
    EXPECT_EQ(inv->workDirectory, cache) << stdCommands.front();
    EXPECT_EQ(inv->arguments.front(), tc.binaryPath.string()) << stdCommands.front();
    EXPECT_TRUE(contains(inv->arguments, "/std:c++latest")) << stdCommands.front();
    EXPECT_TRUE(contains(inv->arguments, tc.stdModuleSource.string())) << stdCommands.front();
    EXPECT_FALSE(contains(inv->arguments, "2>&1")) << stdCommands.front();

    auto compatCommands = msvc::std_compat_build_commands(tc, cache, "/std:c++latest", "/MD");
    auto compat = db::recover_invocation(compatCommands, tc.stdCompatSource,
                                         tc.binaryPath, "/unused", kWindows);
    ASSERT_TRUE(compat.has_value()) << compatCommands.front();
    EXPECT_TRUE(contains(compat->arguments, "/reference")) << compatCommands.front();
}

TEST(BuildDatabase, NoCommandNamingTheSourceRecoversNothing) {
    auto inv = db::recover_invocation({"cd /c && /bin/g++ -c other.cc -o o.o"},
                                      "/src/std.cc", "/bin/g++", "/c", false);
    EXPECT_FALSE(inv.has_value());
}

TEST(BuildDatabase, AnUnquotedDriverWithASpaceIsRejoined) {
    auto inv = db::recover_invocation(
        {"C:\\Program Files\\LLVM\\bin\\clang++.exe -std=c++23 --precompile "
         "\"C:\\s\\std.cppm\" -o \"C:\\c\\std.pcm\""},
        "C:\\s\\std.cppm", "C:\\Program Files\\LLVM\\bin\\clang++.exe", "C:\\c",
        /*windows=*/true);
    ASSERT_TRUE(inv.has_value());
    EXPECT_EQ(inv->arguments.front(), "C:\\Program Files\\LLVM\\bin\\clang++.exe");
    EXPECT_EQ(inv->arguments[1], "-std=c++23");
}
