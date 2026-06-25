#include <gtest/gtest.h>

import std;
import mcpp.build.plan;

using namespace mcpp::build;

namespace {

// Write `content` to a unique temp .cpp and return its path.
std::filesystem::path write_tmp(std::string_view content, std::string_view tag) {
    auto dir = std::filesystem::temp_directory_path();
    auto p = dir / std::format("mcpp_maindetect_{}.cpp", tag);
    std::ofstream os(p);
    os << content;
    os.close();
    return p;
}

bool defines_main(std::string_view content, std::string_view tag) {
    auto p = write_tmp(content, tag);
    bool r = source_defines_main(p);
    std::error_code ec;
    std::filesystem::remove(p, ec);
    return r;
}

}  // namespace

TEST(MainDetection, RealMainIsDetected) {
    EXPECT_TRUE(defines_main("import std;\nint main() { return 0; }\n", "real"));
}

TEST(MainDetection, RealMainWithArgsIsDetected) {
    EXPECT_TRUE(defines_main(
        "int main(int argc, char** argv) { (void)argc; (void)argv; return 0; }\n", "args"));
}

TEST(MainDetection, AutoMainIsDetected) {
    EXPECT_TRUE(defines_main("auto main() -> int { return 0; }\n", "automain"));
}

// The regression: test fixtures embed `"int main() {...}"` as a STRING — that
// must NOT count as the test binary defining main (it doesn't). A false positive
// chose archive linking → gtest_main.o not pulled by MSVC lld-link → LNK1561.
TEST(MainDetection, MainInsideStringLiteralIsIgnored) {
    EXPECT_FALSE(defines_main(
        "#include <gtest/gtest.h>\n"
        "TEST(M, x) {\n"
        "  auto src = \"int main() { return 0; }\\n\";\n"
        "  EXPECT_FALSE(src.empty());\n"
        "}\n", "strlit"));
}

TEST(MainDetection, MainInsideRawStringIsIgnored) {
    EXPECT_FALSE(defines_main(
        "#include <gtest/gtest.h>\n"
        "TEST(M, x) {\n"
        "  auto src = R\"(\nint main() { return 0; }\n)\";\n"
        "  EXPECT_FALSE(src.empty());\n"
        "}\n", "rawstr"));
}

TEST(MainDetection, MainInsideCommentIsIgnored) {
    EXPECT_FALSE(defines_main(
        "// int main() { return 0; }\n"
        "#include <gtest/gtest.h>\n"
        "TEST(M, x) { EXPECT_TRUE(true); }\n", "comment"));
}

TEST(MainDetection, NoMainGtestStyleIsFalse) {
    EXPECT_FALSE(defines_main(
        "#include <gtest/gtest.h>\n"
        "TEST(M, x) { EXPECT_EQ(1, 1); }\n", "nomain"));
}

TEST(MainDetection, SimilarIdentifierIsNotMain) {
    EXPECT_FALSE(defines_main("int mainHelper() { return 0; }\n", "helper"));
}
