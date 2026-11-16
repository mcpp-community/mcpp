// The rules docs/specs/build-database.md (SPEC-005) states that can be pinned
// without planning a project: the role of each declaration form, the standard
// library names, the toolchain id, and the recovery of the std units' argument
// vectors from the commands mcpp renders. The recovery tests drive the REAL
// command builders, so a builder that changes shape fails here rather than in
// a document an editor reads.


import std;
import mcpplibs.mtest;
import mcpp.build.build_database;
import mcpp.modgraph.graph;
import mcpplibs.os.platform;
import mcpp.toolchain.clang;
import mcpp.toolchain.gcc;
import mcpp.toolchain.model;
import mcpp.toolchain.msvc;

using namespace mcpplibs::mtest;

namespace db = mcpp::build::database;
using mcpp::modgraph::ModuleDeclaration;
using namespace mcpp::toolchain;

namespace {

const bool kWindows = mcpplibs::os::is_windows;

std::filesystem::path host_path(std::string_view posix, std::string_view windows) {
    return std::filesystem::path{std::string(kWindows ? windows : posix)};
}

bool contains(const std::vector<std::string>& words, const std::string& w) {
    return std::ranges::find(words, w) != words.end();
}

} // namespace

UTest role_of_each_declaration_form{"BuildDatabase/RoleOfEachDeclarationForm", [] {
    expect(eq(db::role_name(ModuleDeclaration::None), "non-module"));
    expect(eq(db::role_name(ModuleDeclaration::Interface), "module-interface"));
    expect(eq(db::role_name(ModuleDeclaration::InterfacePartition), "module-partition-interface"));
    expect(eq(db::role_name(ModuleDeclaration::ImplementationPartition), "module-partition-implementation"));
    expect(eq(db::role_name(ModuleDeclaration::Implementation), "module-implementation"));
    expect(eq(db::role_name(ModuleDeclaration::Unknown), "unknown"));
}};

UTest standard_library_names{"BuildDatabase/StandardLibraryNames", [] {
    expect(eq(db::stdlib_name("libstdc++"), "libstdc++"));
    expect(eq(db::stdlib_name("libc++"), "libc++"));
    expect(eq(db::stdlib_name("msvc-stl"), "msvc-stl"));
    expect(eq(db::stdlib_name("newlib"), "other"));
}};

UTest toolchain_id_names_family_version_and_the_compilers_triple{"BuildDatabase/ToolchainIdNamesFamilyVersionAndTheCompilersTriple", [] {
    Toolchain tc;
    tc.compiler = CompilerId::Clang;
    tc.version = "22.1.8";
    tc.targetTriple = "x86_64-windows-msvc";
    // mcpp's family name in the opaque id (`llvm`, as in `llvm@22.1.8`), and the
    // triple as the compiler spells it; the S1 `family` field says `clang`.
    expect(eq(db::toolchain_id(tc, "x86_64-pc-windows-msvc"), "llvm-22.1.8-x86_64-pc-windows-msvc"));
    expect(eq(tc.compiler_name(), "clang"));
}};

UTest posix_words_undo_shell_quoting{"BuildDatabase/PosixWordsUndoShellQuoting", [] {
    auto w = db::split_command_words(
        "cd '/c d' && env LD_LIBRARY_PATH='/r t' 'it'\\''s' \"a\\\"b\" x\\ y 2>&1",
        /*windows=*/false);
    std::vector<std::string> want{"cd", "/c d", "&&", "env", "LD_LIBRARY_PATH=/r t",
                                  "it's", "a\"b", "x y", "2>&1"};
    expect(eq(w, want));
}};

UTest windows_words_follow_the_runtime_rules{"BuildDatabase/WindowsWordsFollowTheRuntimeRules", [] {
    auto w = db::split_command_words(
        "cd /d \"C:\\a b\" && \"C:\\t\\clang++.exe\" \"x\\\\\\\"y\" C:\\s\\std.ixx 2>&1",
        /*windows=*/true);
    std::vector<std::string> want{"cd", "/d", "C:\\a b", "&&", "C:\\t\\clang++.exe",
                                  "x\\\"y", "C:\\s\\std.ixx", "2>&1"};
    expect(eq(w, want));
}};

UTest recovers_the_gcc_std_unit_from_its_builder{"BuildDatabase/RecoversTheGccStdUnitFromItsBuilder", [] {
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
    expect(fatal(!commands.empty()));
    auto inv = db::recover_invocation(commands, tc.stdModuleSource, tc.binaryPath,
                                      "/unused", kWindows);
    expect(fatal(inv.has_value())) << commands.front();
    expect(eq(inv->workDirectory, cache)) << commands.front();
    expect(fatal(!(inv->arguments.empty())));
    expect(eq(inv->arguments.front(), tc.binaryPath.string())) << commands.front();
    expect(contains(inv->arguments, tc.stdModuleSource.string())) << commands.front();
    expect(contains(inv->arguments, "-std=c++23")) << commands.front();
    expect(!contains(inv->arguments, "2>&1")) << commands.front();
    expect(!contains(inv->arguments, "env")) << commands.front();
    for (auto const& a : inv->arguments)
        expect(!a.starts_with("LD_LIBRARY_PATH=")) << a;
}};

UTest recovers_the_clang_std_units_from_their_builders{"BuildDatabase/RecoversTheClangStdUnitsFromTheirBuilders", [] {
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
    expect(fatal(inv.has_value())) << stdCommands.front();
    expect(eq(inv->arguments.front(), tc.binaryPath.string()));
    expect(contains(inv->arguments, "--precompile")) << stdCommands.front();
    expect(contains(inv->arguments, tc.stdModuleSource.string()));
    expect(eq(inv->workDirectory, cache));

    auto compatCommands = clang::std_compat_build_commands(
        tc, cache, clang::std_compat_bmi_path(cache), bmi, "", "-std=c++23");
    auto compat = db::recover_invocation(compatCommands, tc.stdCompatSource,
                                         tc.binaryPath, cache, kWindows);
    expect(fatal(compat.has_value())) << compatCommands.front();
    expect(contains(compat->arguments, tc.stdCompatSource.string()));
}};

UTest recovers_the_msvc_std_units_from_their_builders{"BuildDatabase/RecoversTheMsvcStdUnitsFromTheirBuilders", [] {
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
    expect(fatal(inv.has_value())) << stdCommands.front();
    expect(eq(inv->workDirectory, cache)) << stdCommands.front();
    expect(eq(inv->arguments.front(), tc.binaryPath.string())) << stdCommands.front();
    expect(contains(inv->arguments, "/std:c++latest")) << stdCommands.front();
    expect(contains(inv->arguments, tc.stdModuleSource.string())) << stdCommands.front();
    expect(!contains(inv->arguments, "2>&1")) << stdCommands.front();

    auto compatCommands = msvc::std_compat_build_commands(tc, cache, "/std:c++latest", "/MD");
    auto compat = db::recover_invocation(compatCommands, tc.stdCompatSource,
                                         tc.binaryPath, "/unused", kWindows);
    expect(fatal(compat.has_value())) << compatCommands.front();
    expect(contains(compat->arguments, "/reference")) << compatCommands.front();
}};

UTest no_command_naming_the_source_recovers_nothing{"BuildDatabase/NoCommandNamingTheSourceRecoversNothing", [] {
    auto inv = db::recover_invocation({"cd /c && /bin/g++ -c other.cc -o o.o"},
                                      "/src/std.cc", "/bin/g++", "/c", false);
    expect(!inv.has_value());
}};

UTest an_unquoted_driver_with_a_space_is_rejoined{"BuildDatabase/AnUnquotedDriverWithASpaceIsRejoined", [] {
    auto inv = db::recover_invocation(
        {"C:\\Program Files\\LLVM\\bin\\clang++.exe -std=c++23 --precompile "
         "\"C:\\s\\std.cppm\" -o \"C:\\c\\std.pcm\""},
        "C:\\s\\std.cppm", "C:\\Program Files\\LLVM\\bin\\clang++.exe", "C:\\c",
        /*windows=*/true);
    expect(fatal(inv.has_value()));
    expect(eq(inv->arguments.front(), "C:\\Program Files\\LLVM\\bin\\clang++.exe"));
    expect(eq(inv->arguments[1], "-std=c++23"));
}};
