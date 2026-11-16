
import std;
import mcpplibs.mtest;
import mcpp.build.plan;

using namespace mcpplibs::mtest;

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

UTest real_main_is_detected{"MainDetection/RealMainIsDetected", [] {
    expect(bool(defines_main("import std;\nint main() { return 0; }\n", "real")));
}};

UTest real_main_with_args_is_detected{"MainDetection/RealMainWithArgsIsDetected", [] {
    expect(bool(defines_main(
        "int main(int argc, char** argv) { (void)argc; (void)argv; return 0; }\n", "args")));
}};

UTest auto_main_is_detected{"MainDetection/AutoMainIsDetected", [] {
    expect(bool(defines_main("auto main() -> int { return 0; }\n", "automain")));
}};

// The regression: test fixtures embed `"int main() {...}"` as a STRING — that
// must NOT count as the test binary defining main (it doesn't). A false positive
// chose archive linking → gtest_main.o not pulled by MSVC lld-link → LNK1561.
UTest main_inside_string_literal_is_ignored{"MainDetection/MainInsideStringLiteralIsIgnored", [] {
    expect(!(defines_main(
        "#include <gtest/gtest.h>\n"
        "TEST(M, x) {\n"
        "  auto src = \"int main() { return 0; }\\n\";\n"
        "  EXPECT_FALSE(src.empty());\n"
        "}\n", "strlit")));
}};

UTest main_inside_raw_string_is_ignored{"MainDetection/MainInsideRawStringIsIgnored", [] {
    expect(!(defines_main(
        "#include <gtest/gtest.h>\n"
        "TEST(M, x) {\n"
        "  auto src = R\"(\nint main() { return 0; }\n)\";\n"
        "  EXPECT_FALSE(src.empty());\n"
        "}\n", "rawstr")));
}};

UTest main_inside_comment_is_ignored{"MainDetection/MainInsideCommentIsIgnored", [] {
    expect(!(defines_main(
        "// int main() { return 0; }\n"
        "#include <gtest/gtest.h>\n"
        "TEST(M, x) { EXPECT_TRUE(true); }\n", "comment")));
}};

UTest no_main_gtest_style_is_false{"MainDetection/NoMainGtestStyleIsFalse", [] {
    expect(!(defines_main(
        "#include <gtest/gtest.h>\n"
        "TEST(M, x) { EXPECT_EQ(1, 1); }\n", "nomain")));
}};

UTest similar_identifier_is_not_main{"MainDetection/SimilarIdentifierIsNotMain", [] {
    expect(!(defines_main("int mainHelper() { return 0; }\n", "helper")));
}};
