#if defined(_WIN32)
#include <windows.h>
#endif

#include <gtest/gtest.h>

import std;
import mcpp.platform.fs;

namespace {

std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}

} // namespace

TEST(PlatformFs, ReplaceFileOverwritesDestination) {
    const auto root = std::filesystem::temp_directory_path()
                    / std::format("mcpp_replace_file_{}",
                        std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(root);
    const auto source = root / "source.tmp";
    const auto destination = root / "current.json";
    std::ofstream(source) << "new";
    std::ofstream(destination) << "old";

    std::error_code ec;
    mcpp::platform::fs::replace_file(source, destination, ec);

    EXPECT_FALSE(ec) << ec.message();
    EXPECT_EQ(read_text(destination), "new");
    EXPECT_FALSE(std::filesystem::exists(source));
    std::filesystem::remove_all(root, ec);
}

#if defined(_WIN32)
TEST(PlatformFs, FailedReplacePreservesDestination) {
    const auto root = std::filesystem::temp_directory_path()
                    / std::format("mcpp_replace_locked_{}",
                        std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(root);
    const auto source = root / "source.tmp";
    const auto destination = root / "current.json";
    std::ofstream(source) << "new";
    std::ofstream(destination) << "old";

    // 禁止 FILE_SHARE_DELETE，稳定制造 Windows 原子替换失败。
    HANDLE locked = CreateFileW(destination.wstring().c_str(), GENERIC_READ,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(locked, INVALID_HANDLE_VALUE);

    std::error_code ec;
    mcpp::platform::fs::replace_file(source, destination, ec);
    CloseHandle(locked);

    EXPECT_TRUE(ec);
    EXPECT_EQ(read_text(destination), "old");
    EXPECT_EQ(read_text(source), "new");
    std::filesystem::remove_all(root, ec);
}
#endif
