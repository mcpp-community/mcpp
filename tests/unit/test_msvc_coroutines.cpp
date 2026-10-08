#include <gtest/gtest.h>

import std;
import mcpp.toolchain.msvc_coroutines;

// The note a failed compile gets when clang does not support coroutines on the
// MSVC ABI it targets (LLVM 23.1.3 Part 3, D1). The outputs below are copied
// from probe PR #788 (run 37792334189): clang 23.1.3, i686-pc-windows-msvc,
// MSVC 14.51.36231.

namespace {

namespace co = mcpp::toolchain::msvc_coroutines;

constexpr std::string_view kStdModuleFailure =
    "In file included from C:\\Program Files\\Microsoft Visual Studio\\18\\Enterprise\\VC\\Tools\\MSVC\\14.51.36231\\modules\\std.ixx:128:\n"
    "C:\\Program Files\\Microsoft Visual Studio\\18\\Enterprise\\VC\\Tools\\MSVC\\14.51.36231\\include\\generator:265:9: error: unknown type name 'suspend_always'\n"
    "  265 |         suspend_always initial_suspend() const noexcept {\n"
    "      |         ^\n"
    "20 errors generated.\n"
    "std module precompile failed (rc=1):\n"
    "\n"
    "command: C:\\Users\\runneradmin\\.mcpp\\registry\\data\\xpkgs\\xim-x-llvm\\23.1.3\\bin\\clang++.exe "
    "-std=c++23 -fms-runtime-lib=dll -x c++-module --target=i686-pc-windows-msvc "
    "-Xmicrosoft-visualc-tools-root \"C:\\Program Files\\Microsoft Visual Studio\\18\\Enterprise\\VC\\Tools\\MSVC\\14.51.36231\" "
    "--precompile std.ixx -o std.pcm\n";

constexpr std::string_view kCoroutineUseFailure =
    "[2/3] C:/Users/runneradmin/.mcpp/registry/data/xpkgs/xim-x-llvm/23.1.3/bin/clang++.exe @obj/main.o.rsp -std=c++20 --target=i686-pc-windows-msvc -c src/main.cpp\n"
    "FAILED: obj/main.o \n"
    "C:/Users/runneradmin/.mcpp/registry/data/xpkgs/xim-x-llvm/23.1.3/bin/clang++.exe @obj/main.o.rsp -std=c++20 -fms-runtime-lib=dll "
    "\"-fprebuilt-module-path=D:\\a\\_temp\\pcm.cache\" -O0 -g --target=i686-pc-windows-msvc -c D:/a/_temp/src/main.cpp -o obj/main.o\n"
    "D:/a/_temp/src/main.cpp:2:71: error: use of undeclared identifier 'std'\n"
    "D:/a/_temp/src/main.cpp:3:9: error: std::coroutine_traits type was not found; include <coroutine> before defining a coroutine\n"
    "3 errors generated.\n"
    "ninja: build stopped: subcommand failed.\n";

co::ProbeFn answering(bool predefines, int* calls = nullptr) {
    return [=](const co::FailedCommand&) -> std::optional<co::Probe> {
        if (calls) ++*calls;
        return co::Probe{predefines, "23.1.3"};
    };
}

// What the std module precompile's error actually carries (#781's Windows CI,
// run 37802975713): the command, not the compiler's diagnostics, which reach
// the terminal directly.
constexpr std::string_view kStdModuleMessage =
    "std module precompile failed (rc=1):\n"
    "\n"
    "command: C:/Users/runneradmin/.mcpp/registry\\data\\xpkgs\\xim-x-llvm\\23.1.3\\bin\\clang++.exe "
    "-std=c++23 -fms-runtime-lib=dll -x c++-module -Wno-include-angled-in-module-purview "
    "--target=i686-pc-windows-msvc "
    "-Xmicrosoft-visualc-tools-root \"C:\\Program Files\\Microsoft Visual Studio\\18\\Enterprise\\VC\\Tools\\MSVC\\14.51.36231\" "
    "--precompile \"C:\\Program Files\\Microsoft Visual Studio\\18\\Enterprise\\VC\\Tools\\MSVC\\14.51.36231\\modules\\std.ixx\" "
    "-o std.pcm\n";

std::string with_standard(std::string_view message, std::string_view from, std::string_view to) {
    std::string s(message);
    s.replace(s.find(from), from.size(), to);
    return s;
}

TEST(MsvcCoroutines, StdModuleMessageWithoutDiagnosticsGetsTheNote) {
    auto note = co::std_module_advice(kStdModuleMessage, answering(false));
    ASSERT_FALSE(note.empty());
    EXPECT_NE(note.find("<generator>"), std::string::npos);
    EXPECT_NE(note.find("standard = \"c++20\""), std::string::npos);
    EXPECT_NE(note.find("[target.i686-windows-msvc]"), std::string::npos);
}

TEST(MsvcCoroutines, StdModuleBelowCxx23IsNotThisFailure) {
    int calls = 0;
    auto cxx20 = with_standard(kStdModuleMessage, "-std=c++23", "-std=c++20");
    EXPECT_TRUE(co::std_module_advice(cxx20, answering(false, &calls)).empty());
    EXPECT_EQ(calls, 0);
    for (auto spelling : {"-std=c++2b", "-std=c++26", "-std=gnu++23"}) {
        auto later = with_standard(kStdModuleMessage, "-std=c++23", spelling);
        EXPECT_FALSE(co::std_module_advice(later, answering(false)).empty()) << spelling;
    }
}

TEST(MsvcCoroutines, StdModuleOfLibcxxIsNotThisFailure) {
    int calls = 0;
    auto libcxx = with_standard(kStdModuleMessage, "modules\\std.ixx", "share\\libc++\\v1\\std.cppm");
    EXPECT_TRUE(co::std_module_advice(libcxx, answering(false, &calls)).empty());
    EXPECT_EQ(calls, 0);
}

TEST(MsvcCoroutines, StdModuleOnACompilerThatPredefinesTheMacroGetsNoNote) {
    EXPECT_TRUE(co::std_module_advice(kStdModuleMessage, answering(true)).empty());
}

TEST(MsvcCoroutines, StdModuleFailureIsTheGeneratorSymptom) {
    EXPECT_EQ(co::symptom_in(kStdModuleFailure), co::Symptom::StdModuleGenerator);
    auto cmd = co::failed_command_in(kStdModuleFailure);
    ASSERT_TRUE(cmd.has_value());
    EXPECT_TRUE(cmd->compiler.ends_with("clang++.exe"));
    EXPECT_EQ(cmd->target, "i686-pc-windows-msvc");
    EXPECT_EQ(cmd->standard, "c++23");
}

TEST(MsvcCoroutines, StdModuleNoteOffersCxx20FirstAndLlvm22Optionally) {
    auto note = co::advice(kStdModuleFailure, answering(false));
    ASSERT_FALSE(note.empty());
    EXPECT_NE(note.find("clang 23.1.3 does not support C++20 coroutines"), std::string::npos);
    EXPECT_NE(note.find("<generator>"), std::string::npos);
    const auto cxx20 = note.find("standard = \"c++20\"");
    const auto llvm22 = note.find("toolchain = \"llvm@22.1.8\"");
    ASSERT_NE(cxx20, std::string::npos);
    ASSERT_NE(llvm22, std::string::npos);
    EXPECT_LT(cxx20, llvm22);
    // The section uses mcpp's spelling of the target.
    EXPECT_NE(note.find("[target.i686-windows-msvc]"), std::string::npos);
    EXPECT_NE(note.find("Defining __cpp_impl_coroutine yourself is not a fix"), std::string::npos);
}

TEST(MsvcCoroutines, CoroutineUseNoteDoesNotOfferCxx20) {
    EXPECT_EQ(co::symptom_in(kCoroutineUseFailure), co::Symptom::CoroutineUse);
    auto cmd = co::failed_command_in(kCoroutineUseFailure);
    ASSERT_TRUE(cmd.has_value());
    EXPECT_EQ(cmd->standard, "c++20");
    auto note = co::advice(kCoroutineUseFailure, answering(false));
    ASSERT_FALSE(note.empty());
    EXPECT_NE(note.find("Code that uses coroutines"), std::string::npos);
    EXPECT_EQ(note.find("c++20\""), std::string::npos);
    EXPECT_NE(note.find("llvm@22.1.8"), std::string::npos);
}

TEST(MsvcCoroutines, CompilerThatPredefinesTheMacroGetsNoNote) {
    EXPECT_TRUE(co::advice(kStdModuleFailure, answering(true)).empty());
}

TEST(MsvcCoroutines, ProbeFailureGetsNoNote) {
    co::ProbeFn none = [](const co::FailedCommand&) -> std::optional<co::Probe> {
        return std::nullopt;
    };
    EXPECT_TRUE(co::advice(kStdModuleFailure, none).empty());
}

TEST(MsvcCoroutines, UnrelatedFailureDoesNotRunTheProbe) {
    int calls = 0;
    std::string other =
        "FAILED: obj/main.o \n"
        "clang++.exe -std=c++23 --target=i686-pc-windows-msvc -c main.cpp\n"
        "main.cpp:1:10: fatal error: 'missing.h' file not found\n";
    EXPECT_TRUE(co::advice(other, answering(false, &calls)).empty());
    EXPECT_EQ(calls, 0);
}

TEST(MsvcCoroutines, NonMsvcTargetDoesNotRunTheProbe) {
    int calls = 0;
    std::string gnu =
        "FAILED: obj/main.o \n"
        "clang++ -std=c++20 --target=i686-pc-windows-gnu -c main.cpp\n"
        "main.cpp:3:9: error: std::coroutine_traits type was not found\n";
    EXPECT_TRUE(co::advice(gnu, answering(false, &calls)).empty());
    EXPECT_EQ(calls, 0);
}

TEST(MsvcCoroutines, OutputWithoutACommandGetsNoNote) {
    std::string bare =
        "main.cpp:3:9: error: std::coroutine_traits type was not found\n";
    EXPECT_TRUE(co::advice(bare, answering(false)).empty());
}

TEST(MsvcCoroutines, NoteWithoutVersionNamesTheCompilerGenerically) {
    co::ProbeFn unversioned = [](const co::FailedCommand&) -> std::optional<co::Probe> {
        return co::Probe{false, ""};
    };
    auto note = co::advice(kStdModuleFailure, unversioned);
    EXPECT_NE(note.find("this clang does not support"), std::string::npos);
}

} // namespace
