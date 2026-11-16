
import std;
import mcpplibs.mtest;
import mcpp.libs.toml;

using namespace mcpplibs::mtest;

namespace t = mcpp::libs::toml;

UTest empty_document_parses{"Toml/EmptyDocumentParses", [] {
    auto d = t::parse("");
    expect(fatal(d.has_value()));
    expect(d->root().empty());
}};

UTest simple_key_value{"Toml/SimpleKeyValue", [] {
    auto d = t::parse("key = \"value\"\nnum = 42\nflag = true");
    if (auto&& failed = expect(fatal(d.has_value())); !failed) failed << d.error().message;
    expect(eq(d->get_string("key").value_or(""), "value"));
    expect(eq(d->get_int("num").value_or(-1), 42));
    expect(eq(d->get_bool("flag").value_or(false), true));
}};

UTest nested_tables{"Toml/NestedTables", [] {
    auto d = t::parse(R"(
[a.b]
x = "deep"
[a]
y = "shallow"
)");
    if (auto&& failed = expect(fatal(d.has_value())); !failed) failed << d.error().message;
    expect(eq(d->get_string("a.b.x").value_or(""), "deep"));
    expect(eq(d->get_string("a.y").value_or(""), "shallow"));
}};

UTest array_of_strings{"Toml/ArrayOfStrings", [] {
    auto d = t::parse(R"(items = ["a", "b", "c"])");
    expect(fatal(d.has_value()));
    auto arr = d->get_string_array("items");
    expect(fatal(arr.has_value()));
    expect(eq(arr->size(), 3u));
    expect(eq((*arr)[0], "a"));
    expect(eq((*arr)[2], "c"));
}};

UTest array_allows_trailing_comma{"Toml/ArrayAllowsTrailingComma", [] {
    auto d = t::parse(R"(
items = [
  "a",
  "b",
]
)");
    if (auto&& failed = expect(fatal(d.has_value())); !failed) failed << d.error().message;
    auto arr = d->get_string_array("items");
    expect(fatal(arr.has_value()));
    expect(eq(*arr, std::vector<std::string>({"a", "b"})));
}};

UTest escaped_string{"Toml/EscapedString", [] {
    auto d = t::parse(R"(s = "a\tb\nc")");
    expect(fatal(d.has_value()));
    expect(eq(d->get_string("s").value_or(""), std::string("a\tb\nc")));
}};

// TOML 1.0's escapes: \b, \f and the Unicode forms were refused as "invalid
// escape", so a valid manifest written with them could not be read.
UTest every_toml_escape_is_read{"Toml/EveryTomlEscapeIsRead", [] {
    auto d = t::parse(R"(s = "a\bb\fc")");
    if (auto&& failed = expect(fatal(d.has_value())); !failed) failed << d.error().message;
    expect(eq(d->get_string("s").value_or(""), std::string("a\bb\fc")));

    // One, two, three and four UTF-8 bytes, upper- and lower-case hex.
    d = t::parse(R"(s = "\u0041 caf\u00e9 \u4E2D \U0001F600")");
    if (auto&& failed = expect(fatal(d.has_value())); !failed) failed << d.error().message;
    expect(eq(d->get_string("s").value_or(""), std::string("A caf\xC3\xA9 \xE4\xB8\xAD \xF0\x9F\x98\x80")));

    d = t::parse("s = \"\"\"\nJos\\u00e9\\U0000004A\"\"\"");
    if (auto&& failed = expect(fatal(d.has_value())); !failed) failed << d.error().message;
    expect(eq(d->get_string("s").value_or(""), std::string("Jos\xC3\xA9J")));

    // A quoted key is a basic string too.
    d = t::parse(R"("caf\u00e9" = 1)");
    if (auto&& failed = expect(fatal(d.has_value())); !failed) failed << d.error().message;
    expect(eq(d->root().count("caf\xC3\xA9"), 1u));

    // A literal string keeps its backslashes.
    d = t::parse(R"(s = 'C:\u0041')");
    if (auto&& failed = expect(fatal(d.has_value())); !failed) failed << d.error().message;
    expect(eq(d->get_string("s").value_or(""), R"(C:\u0041)"));
}};

UTest a_unicode_escape_names_a_scalar_value{"Toml/AUnicodeEscapeNamesAScalarValue", [] {
    expect(!(t::parse(R"(s = "\u12")").has_value()));          // too few digits
    expect(!(t::parse(R"(s = "\u12G4")").has_value()));        // not hex
    expect(!(t::parse(R"(s = "\U0001F60")").has_value()));     // \U takes eight
    expect(!(t::parse(R"(s = "\uD800")").has_value()));        // a surrogate
    expect(!(t::parse(R"(s = "\U00110000")").has_value()));    // past U+10FFFF
    expect(!(t::parse(R"(s = "\x41")").has_value()));          // not TOML 1.0

    auto bad = t::parse("s = \"ok\\uDFFF\"");
    expect(fatal(!bad.has_value()));
    if (auto&& failed = expect(ne(bad.error().message.find("\\uDFFF"), std::string::npos)); !failed) failed << bad.error().message;
}};

// TOML 1.1: an inline table may span lines, hold comments, and end with a
// comma. Each was refused with `expected key`, so the multi-line `runner = {...}`
// of docs/35 could not be read.
UTest an_inline_table_spans_lines{"Toml/AnInlineTableSpansLines", [] {
    auto d = t::parse(R"(
runner = {   # a comment after the brace
    cmd   = ["dirun", "--", "{}"],
    hosts = ["linux-x86_64",
             "macos-aarch64"],
    # a comment line between pairs
    env   = { A = "1", B = { C = 2, }, },
}
after = true
)");
    if (auto&& failed = expect(fatal(d.has_value())); !failed) failed << d.error().message;
    expect(eq(*d->get_string_array("runner.cmd"), std::vector<std::string>({"dirun", "--", "{}"})));
    expect(eq(*d->get_string_array("runner.hosts"), std::vector<std::string>({"linux-x86_64", "macos-aarch64"})));
    expect(eq(d->get_string("runner.env.A").value_or(""), "1"));
    expect(eq(d->get_int("runner.env.B.C").value_or(-1), 2));
    expect(eq(d->get_bool("after").value_or(false), true));

    d = t::parse("empty = {\n}\nx = { a = 1, }");
    if (auto&& failed = expect(fatal(d.has_value())); !failed) failed << d.error().message;
    expect(fatal(d->get_table("empty") != nullptr));
    expect(d->get_table("empty")->empty());
    expect(eq(d->get_int("x.a").value_or(-1), 1));
}};

// What TOML 1.1 still refuses in an inline table: a pair split across lines,
// a missing separator, an empty element, and a table that never closes.
UTest an_inline_table_keeps_each_pair_on_one_line{"Toml/AnInlineTableKeepsEachPairOnOneLine", [] {
    expect(!(t::parse("x = { a\n= 1 }").has_value()));
    expect(!(t::parse("x = { a =\n1 }").has_value()));
    expect(!(t::parse("x = { a = 1\nb = 2 }").has_value()));
    expect(!(t::parse("x = { a = 1,, }").has_value()));
    expect(!(t::parse("x = { , }").has_value()));

    auto open = t::parse("x = {\n  a = 1,\n");
    expect(fatal(!open.has_value()));
    if (auto&& failed = expect(eq(open.error().message, "unterminated inline table")); !failed) failed << open.error().message;
    expect(eq(open.error().where.line, 1u));
    expect(eq(open.error().where.column, 5u));
}};

UTest reject_unterminated_string{"Toml/RejectUnterminatedString", [] {
    auto d = t::parse(R"(x = "no end)");
    expect(!d.has_value());
}};

UTest comments_ignored{"Toml/CommentsIgnored", [] {
    auto d = t::parse(R"(
# top comment
x = 1  # trailing isn't supported but full-line is
y = 2
)");
    if (auto&& failed = expect(fatal(d.has_value())); !failed) failed << d.error().message;
    expect(eq(d->get_int("x").value_or(-1), 1));
    expect(eq(d->get_int("y").value_or(-1), 2));
}};

UTest escape_string_helper{"Toml/EscapeStringHelper", [] {
    expect(eq(t::escape_string("hello"), "\"hello\""));
    expect(eq(t::escape_string("a\"b"), "\"a\\\"b\""));
    expect(eq(t::escape_string("a\\b"), "\"a\\\\b\""));
}};

UTest multiline_basic_string{"Toml/MultilineBasicString", [] {
    auto d = t::parse("body = \"\"\"\nline1\nline2 \"quoted\"\n\"\"\"\nafter = 1");
    if (auto&& failed = expect(fatal(d.has_value())); !failed) failed << d.error().message;
    // First newline after the opening delimiter is trimmed (TOML 1.0).
    expect(eq(d->get_string("body").value_or(""), "line1\nline2 \"quoted\"\n"));
    expect(eq(d->get_int("after").value_or(-1), 1));
}};

UTest multiline_literal_string{"Toml/MultilineLiteralString", [] {
    auto d = t::parse("raw = '''\nno \\escapes here\n'''");
    if (auto&& failed = expect(fatal(d.has_value())); !failed) failed << d.error().message;
    expect(eq(d->get_string("raw").value_or(""), "no \\escapes here\n"));
}};

UTest multiline_line_ending_backslash{"Toml/MultilineLineEndingBackslash", [] {
    auto d = t::parse("s = \"\"\"a\\\n   b\"\"\"");
    if (auto&& failed = expect(fatal(d.has_value())); !failed) failed << d.error().message;
    expect(eq(d->get_string("s").value_or(""), "ab"));
}};

UTest multiline_unterminated_is_error{"Toml/MultilineUnterminatedIsError", [] {
    auto d = t::parse("s = \"\"\"never closed");
    expect(!d.has_value());
}};

UTest empty_string_still_parses{"Toml/EmptyStringStillParses", [] {
    auto d = t::parse("e = \"\"\nx = 2");
    if (auto&& failed = expect(fatal(d.has_value())); !failed) failed << d.error().message;
    expect(eq(d->get_string("e").value_or("?"), ""));
    expect(eq(d->get_int("x").value_or(-1), 2));
}};

// #227: `[[dotted.path]]` array-of-tables — each occurrence appends a fresh
// table to an array at that dotted path; keys after it fill THAT table until
// the next `[...]`/`[[...]]` header. Declaration order must be preserved
// (it's the "later entries win" override order for [build].flags).
UTest parses_array_of_tables{"Toml/ParsesArrayOfTables", [] {
    auto d = t::parse(R"(
[[build.flags]]
glob = "a/**"
cxxflags = ["-DX=1"]
[[build.flags]]
glob = "b/**"
)");
    if (auto&& failed = expect(fatal(d.has_value())); !failed) failed << d.error().message;
    auto* v = d->get("build.flags");
    expect(fatal(ne(v, nullptr)));
    expect(fatal(v->is_array()));
    auto& arr = v->as_array();
    expect(fatal(eq(arr.size(), 2u)));
    expect(fatal(arr[0].is_table()));
    expect(eq(arr[0].as_table().at("glob").as_string(), "a/**"));
    auto cx = arr[0].as_table().at("cxxflags");
    expect(fatal(cx.is_array()));
    expect(fatal(eq(cx.as_array().size(), 1u)));
    expect(eq(cx.as_array()[0].as_string(), "-DX=1"));
    expect(fatal(arr[1].is_table()));
    expect(eq(arr[1].as_table().at("glob").as_string(), "b/**"));
    expect(eq(arr[1].as_table().count("cxxflags"), 0u));
}};

// Regular `[table]` and inline-array-of-tables parsing must be unaffected by
// AOT support — array-of-tables is an ADDITIONAL grammar form, not a
// replacement.
UTest regular_tables_still_work_alongside_array_of_tables{"Toml/RegularTablesStillWorkAlongsideArrayOfTables", [] {
    auto d = t::parse(R"(
[package]
name = "x"
[[build.flags]]
glob = "a/**"
[dependencies]
foo = "1.0"
)");
    if (auto&& failed = expect(fatal(d.has_value())); !failed) failed << d.error().message;
    expect(eq(d->get_string("package.name").value_or(""), "x"));
    expect(eq(d->get_string("dependencies.foo").value_or(""), "1.0"));
    auto* v = d->get("build.flags");
    expect(fatal(ne(v, nullptr)));
    expect(fatal(v->is_array()));
    expect(eq(v->as_array().size(), 1u));
}};
