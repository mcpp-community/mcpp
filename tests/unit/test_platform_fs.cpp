#include <gtest/gtest.h>

import std;
import mcpp.platform.fs;

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
