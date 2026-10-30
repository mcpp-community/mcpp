#include <gtest/gtest.h>

import std;
import mcpp.pack.polyglot;

// A polyglot file's layout (SPEC-014 §5.3). The PE half is asserted end to
// end by e2e 992, against the carriers a pack makes; what is asserted here is
// the arithmetic a wrong answer to would still run on the machine that wrote
// it: every program lies at the offset its line names, byte for byte, and the
// script's head names where the script is.

namespace pg = mcpp::pack::polyglot;

namespace {

pg::Program program(std::string system, std::string arch, std::string bytes) {
    return { std::move(system), std::move(arch), std::move(bytes), std::string(64, 'a') };
}

std::uint64_t number_after(std::string_view text, std::string_view key) {
    const auto at = text.find(key);
    if (at == std::string_view::npos) return ~std::uint64_t{0};
    std::uint64_t v = 0;
    for (auto i = at + key.size(); i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i)
        v = v * 10 + static_cast<std::uint64_t>(text[i] - '0');
    return v;
}

}  // namespace

TEST(PackPolyglot, WithoutWindowsTheFileIsAScriptAndEachProgramLiesWhereItsLineSays) {
    const std::string linux64(1000, 'L'), mac(777, 'M');
    auto file = pg::assemble("app", { program("linux", "x86_64", linux64), program("macos", "aarch64", mac) });
    ASSERT_TRUE(file.has_value()) << file.error();
    EXPECT_TRUE(file->starts_with("#!/bin/sh\no=$0;"));
    EXPECT_TRUE(pg::is_polyglot(*file));

    // The head names the script: `tail -c +<at+1>` and `head -c <length>`.
    const auto at = number_after(*file, "tail -c +") - 1;
    const auto length = number_after(*file, "|head -c ");
    ASSERT_EQ(at + length, file->size());
    const std::string_view script = std::string_view(*file).substr(at);
    EXPECT_TRUE(script.starts_with("# mcpp polyglot"));
    EXPECT_NE(script.find("n='app'"), std::string_view::npos);
    EXPECT_EQ(script.find("MINGW*"), std::string_view::npos) << "no Windows program, no MSYS line";

    for (auto [key, bytes] : { std::pair{std::string_view("k=linux-x86_64 a="), std::string_view(linux64)},
                               std::pair{std::string_view("k=macos-aarch64 a="), std::string_view(mac)} }) {
        const auto line = script.find(key);
        ASSERT_NE(line, std::string_view::npos) << key;
        const auto off = number_after(script.substr(line), key);
        const auto len = number_after(script.substr(line), " b=");
        EXPECT_EQ(len, bytes.size()) << key;
        EXPECT_EQ(std::string_view(*file).substr(off, len), bytes) << key;
    }
    // `uname` spells an architecture two ways on some systems.
    EXPECT_NE(script.find("Linux-x86_64|Linux-amd64)"), std::string_view::npos);
    EXPECT_NE(script.find("Darwin-aarch64|Darwin-arm64)"), std::string_view::npos);
}

TEST(PackPolyglot, RefusesWhatItCannotLayOut) {
    EXPECT_FALSE(pg::assemble("app", {}).has_value());
    EXPECT_FALSE(pg::assemble("a'b", { program("linux", "x86_64", "x") }).has_value())
        << "the name is quoted in the script";
    EXPECT_FALSE(pg::assemble("app", { program("freebsd", "x86_64", "x") }).has_value());
    auto notPe = pg::assemble("app", { program("windows", "x86_64", std::string(256, 'x')) });
    ASSERT_FALSE(notPe.has_value());
    EXPECT_NE(notPe.error().find("no PE"), std::string::npos) << notPe.error();
    auto twoWindows = pg::assemble("app", { program("windows", "x86_64", "MZ"), program("windows", "aarch64", "MZ") });
    ASSERT_FALSE(twoWindows.has_value());
    EXPECT_NE(twoWindows.error().find("one Windows program"), std::string::npos) << twoWindows.error();
}

TEST(PackPolyglot, RecognisesItsOwnHeadsOnly) {
    EXPECT_TRUE(pg::is_polyglot("MZqFpD='\n   "));
    EXPECT_FALSE(pg::is_polyglot("MZ\x90\x00"));
    EXPECT_FALSE(pg::is_polyglot("#!/bin/sh\necho hi\n"));
    EXPECT_FALSE(pg::is_polyglot(""));
}
