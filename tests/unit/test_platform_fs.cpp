#include <gtest/gtest.h>

import std;
import mcpp.platform.fs;
import mcpp.platform.common;

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
    EXPECT_TRUE(mcpp::platform::fs::replace_file(source, destination, ec))
        << ec.message();
    EXPECT_FALSE(std::filesystem::exists(source));
    EXPECT_EQ(read_file(destination), "new");
}

TEST(PlatformFs, ReplaceFailureKeepsExistingDestination) {
    TempDir temp;
    auto destination = temp.path / "compile_commands.json";
    write_file(destination, "last-known-good");

    std::error_code ec;
    EXPECT_FALSE(mcpp::platform::fs::replace_file(
        temp.path / "missing.tmp", destination, ec));
    EXPECT_TRUE(ec);
    EXPECT_EQ(read_file(destination), "last-known-good");
}

} // namespace

// ── extended_length (mcpp#641, item 3) ────────────────────────────────────

// Outside Windows there is no path limit of this kind, so the helper is the
// identity: a relative path stays relative and nothing is spelled differently.
TEST(PlatformFs, ExtendedLengthIsTheIdentityOutsideWindows) {
    if constexpr (mcpp::platform::is_windows) {
        GTEST_SKIP() << "the Windows branch is covered by the spelling tests";
    } else {
        const std::filesystem::path rel{"obj/pkg/__pkg/dep/a.cpp.ddi"};
        EXPECT_EQ(mcpp::platform::fs::extended_length(rel), rel);
        const std::filesystem::path abs{"/tmp/x/../y"};
        EXPECT_EQ(mcpp::platform::fs::extended_length(abs), abs);
        EXPECT_TRUE(mcpp::platform::fs::extended_length({}).empty());
    }
}

// The Windows rule, as a pure function, so every host tests it.
TEST(PlatformFs, WindowsExtendedLengthSpellsADrivePath) {
    using mcpp::platform::fs::windows_extended_length_spelling;
    EXPECT_EQ(windows_extended_length_spelling("C:/Users/runner/.mcpp/obj/a.ddi"),
              "\\\\?\\C:\\Users\\runner\\.mcpp\\obj\\a.ddi");
    // `.` and `..` are resolved lexically, because the prefix turns their
    // processing off; `..` does not climb above the drive.
    EXPECT_EQ(windows_extended_length_spelling("C:\\a\\.\\b\\..\\c\\"),
              "\\\\?\\C:\\a\\c");
    EXPECT_EQ(windows_extended_length_spelling("C:/../../x"), "\\\\?\\C:\\x");
    EXPECT_EQ(windows_extended_length_spelling("D:/"), "\\\\?\\D:\\");
}

TEST(PlatformFs, WindowsExtendedLengthSpellsAUncPath) {
    using mcpp::platform::fs::windows_extended_length_spelling;
    EXPECT_EQ(windows_extended_length_spelling("//server/share/dir/../f.o"),
              "\\\\?\\UNC\\server\\share\\f.o");
    // `..` does not climb above the share.
    EXPECT_EQ(windows_extended_length_spelling("\\\\server\\share\\..\\..\\f"),
              "\\\\?\\UNC\\server\\share\\f");
}

TEST(PlatformFs, WindowsExtendedLengthLeavesPrefixedAndRelativePathsAlone) {
    using mcpp::platform::fs::windows_extended_length_spelling;
    // Idempotent: a path handed on to a child process is converted again there.
    EXPECT_EQ(windows_extended_length_spelling("\\\\?\\C:\\a\\b"), "\\\\?\\C:\\a\\b");
    EXPECT_EQ(windows_extended_length_spelling("\\\\.\\pipe\\x"), "\\\\.\\pipe\\x");
    EXPECT_EQ(windows_extended_length_spelling("obj/a.ddi"), "obj/a.ddi");
    EXPECT_EQ(windows_extended_length_spelling(""), "");
}
