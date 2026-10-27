#include <gtest/gtest.h>

import std;
import mcpplibs.os.platform;
import mcpplibs.os.fs;
import mcpp.invocation;
import mcpp.home.files;

namespace {

struct TempDir {
    std::filesystem::path path = std::filesystem::temp_directory_path()
        / std::format("mcpp-platform-fs-{}",
                      std::chrono::steady_clock::now().time_since_epoch().count());

    TempDir() { std::filesystem::create_directories(path); }

    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

void write_file(const std::filesystem::path& path, std::string_view text) {
    std::ofstream out(path);
    out << text;
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path);
    return {std::istreambuf_iterator<char>(in), {}};
}

TEST(PlatformFs, ReplaceFileAtomicallyReplacesExistingFile) {
    TempDir temp;
    auto source = temp.path / "new.tmp";
    auto destination = temp.path / "compile_commands.json";
    write_file(source, "new");
    write_file(destination, "old");

    std::error_code ec;
    EXPECT_TRUE(mcpp::home::replace_file(source, destination, ec))
        << ec.message();
    EXPECT_FALSE(std::filesystem::exists(source));
    EXPECT_EQ(read_file(destination), "new");
}

TEST(PlatformFs, ReplaceFailureKeepsExistingDestination) {
    TempDir temp;
    auto destination = temp.path / "compile_commands.json";
    write_file(destination, "last-known-good");

    std::error_code ec;
    EXPECT_FALSE(mcpp::home::replace_file(
        temp.path / "missing.tmp", destination, ec));
    EXPECT_TRUE(ec);
    EXPECT_EQ(read_file(destination), "last-known-good");
}

} // namespace

// ── extended_length (mcpp#641, item 3) ────────────────────────────────────

// Outside Windows there is no path limit of this kind, so the helper is the
// identity: a relative path stays relative and nothing is spelled differently.
TEST(PlatformFs, ExtendedLengthIsTheIdentityOutsideWindows) {
    if constexpr (mcpplibs::os::is_windows) {
        GTEST_SKIP() << "the Windows branch is covered by the spelling tests";
    } else {
        const std::filesystem::path rel{"obj/pkg/__pkg/dep/a.cpp.ddi"};
        EXPECT_EQ(mcpplibs::os::fs::extended(rel), rel);
        const std::filesystem::path abs{"/tmp/x/../y"};
        EXPECT_EQ(mcpplibs::os::fs::extended(abs), abs);
        EXPECT_TRUE(mcpplibs::os::fs::extended({}).empty());
    }
}

// The Windows rule, as a pure function, so every host tests it.
TEST(PlatformFs, WindowsExtendedLengthSpellsADrivePath) {
    using mcpplibs::os::fs::windows_extended;
    EXPECT_EQ(mcpplibs::os::fs::windows_extended("C:/Users/runner/.mcpp/obj/a.ddi"),
              "\\\\?\\C:\\Users\\runner\\.mcpp\\obj\\a.ddi");
    // `.` and `..` are resolved lexically, because the prefix turns their
    // processing off; `..` does not climb above the drive.
    EXPECT_EQ(mcpplibs::os::fs::windows_extended("C:\\a\\.\\b\\..\\c\\"),
              "\\\\?\\C:\\a\\c");
    EXPECT_EQ(mcpplibs::os::fs::windows_extended("C:/../../x"), "\\\\?\\C:\\x");
    EXPECT_EQ(mcpplibs::os::fs::windows_extended("D:/"), "\\\\?\\D:\\");
}

TEST(PlatformFs, WindowsExtendedLengthSpellsAUncPath) {
    using mcpplibs::os::fs::windows_extended;
    EXPECT_EQ(mcpplibs::os::fs::windows_extended("//server/share/dir/../f.o"),
              "\\\\?\\UNC\\server\\share\\f.o");
    // `..` does not climb above the share.
    EXPECT_EQ(mcpplibs::os::fs::windows_extended("\\\\server\\share\\..\\..\\f"),
              "\\\\?\\UNC\\server\\share\\f");
}

TEST(PlatformFs, WindowsExtendedLengthLeavesPrefixedAndRelativePathsAlone) {
    using mcpplibs::os::fs::windows_extended;
    // Idempotent: a path handed on to a child process is converted again there.
    EXPECT_EQ(mcpplibs::os::fs::windows_extended("\\\\?\\C:\\a\\b"), "\\\\?\\C:\\a\\b");
    EXPECT_EQ(mcpplibs::os::fs::windows_extended("\\\\.\\pipe\\x"), "\\\\.\\pipe\\x");
    EXPECT_EQ(mcpplibs::os::fs::windows_extended("obj/a.ddi"), "obj/a.ddi");
    EXPECT_EQ(mcpplibs::os::fs::windows_extended(""), "");
}

// A NAME THE SHELL ANSWERS FOR ITSELF still resolves to the program on PATH.
// `command -v true` prints `true`, because a shell runs its own builtin, and
// the path is then found by walking PATH (mcpp#755: a bare-name payload
// override of such a name was refused as "not found on PATH" on a machine
// carrying /usr/bin/true).
TEST(PlatformFs, WhichResolvesANameThatIsAlsoAShellBuiltin) {
#if defined(_WIN32)
    GTEST_SKIP() << "`where` reports programs only; the shell answers nothing";
#else
    std::error_code ec;
    if (!std::filesystem::is_regular_file("/usr/bin/true", ec)
        && !std::filesystem::is_regular_file("/bin/true", ec))
        GTEST_SKIP() << "this host has no `true` program to find";
    const auto found = mcpp::invocation::which("true");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->filename(), "true");
    EXPECT_TRUE(std::filesystem::is_regular_file(*found, ec)) << found->string();
#endif
}

TEST(PlatformFs, WhichStillAnswersNothingForANameNoHostHas) {
    EXPECT_FALSE(mcpp::invocation::which("mcpp-no-such-program-7c25d9f6").has_value());
}
