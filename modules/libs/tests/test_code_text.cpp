import std;
import mcpplibs.mtest;
import mcpp.libs.code_text;

using namespace mcpplibs::mtest;

// SUBSYSTEM-LEVEL: what of a C++ source is code (design 2026-11-27 §4.3). One reader gives it to
// the module scanner, the source seam of the build optimizations and the manifest's module-name
// functions; that each of them reads its result is stated beside them.

namespace {

std::string code(std::string_view text) { return mcpp::libs::code_text::read(text).text; }

} // namespace

UTest the_result_has_the_offsets_of_the_text{"CodeText/TheResultHasTheOffsetsOfTheText", [] {
    const std::string_view text = "int a; // tail\n/* block\n over */ int b;\n";
    const auto c = code(text);
    expect(fatal(eq(c.size(), text.size())));
    expect(eq(c, std::string("int a;        \n        \n         int b;\n")));
}};

UTest a_raw_string_holds_no_code{"CodeText/ARawStringHoldsNoCode", [] {
    const auto c = code("const char* s = R\"x(\nexport module hidden;\n)\" /* ) */ )x\";\nimport shown;\n");
    expect(!c.contains("hidden"));
    expect(c.contains("import shown;"));
}};

UTest a_commented_raw_opener_opens_nothing{"CodeText/ACommentedRawOpenerOpensNothing", [] {
    expect(code("// R\"(\nimport x;\n").contains("import x;"));
}};

UTest a_literal_holds_no_comment{"CodeText/ALiteralHoldsNoComment", [] {
    expect(code("const char* s = \"a /* b\";\nimport x;\n").contains("import x;"));
    expect(code("char c = '\"'; import y;\n").contains("import y;"));
}};

UTest a_digit_separator_is_part_of_a_number{"CodeText/ADigitSeparatorIsPartOfANumber", [] {
    expect(code("int n = 1'000; import x; int m = 0x1'f;\n").contains("import x;"));
}};

UTest a_continued_comment_goes_on{"CodeText/AContinuedCommentGoesOn", [] {
    const auto r = mcpp::libs::code_text::read("// a comment \\\nimport hidden;\nimport shown;\n");
    expect(!r.text.contains("hidden"));
    expect(r.text.contains("import shown;"));
    expect(eq(r.joins.size(), 1u));
}};

UTest a_header_name_is_not_tokens{"CodeText/AHeaderNameIsNotTokens", [] {
    const auto c = code("#include <a'b>\n#error don't\nimport x;\n");
    expect(c.contains("#include"));
    expect(!c.contains("a'b"));
    expect(c.contains("import x;"));
}};
