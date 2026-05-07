#include <gtest/gtest.h>

import std;
import mcpp.libs.toml;

namespace t = mcpp::libs::toml;

TEST(Toml, EmptyDocumentParses) {
    auto d = t::parse("");
    ASSERT_TRUE(d.has_value());
    EXPECT_TRUE(d->root().empty());
}

TEST(Toml, SimpleKeyValue) {
    auto d = t::parse("key = \"value\"\nnum = 42\nflag = true");
    ASSERT_TRUE(d.has_value()) << d.error().message;
    EXPECT_EQ(d->get_string("key").value_or(""), "value");
    EXPECT_EQ(d->get_int("num").value_or(-1), 42);
    EXPECT_EQ(d->get_bool("flag").value_or(false), true);
}

TEST(Toml, NestedTables) {
    auto d = t::parse(R"(
[a.b]
x = "deep"
[a]
y = "shallow"
)");
    ASSERT_TRUE(d.has_value()) << d.error().message;
    EXPECT_EQ(d->get_string("a.b.x").value_or(""), "deep");
    EXPECT_EQ(d->get_string("a.y").value_or(""), "shallow");
}

TEST(Toml, ArrayOfStrings) {
    auto d = t::parse(R"(items = ["a", "b", "c"])");
    ASSERT_TRUE(d.has_value());
    auto arr = d->get_string_array("items");
    ASSERT_TRUE(arr.has_value());
    EXPECT_EQ(arr->size(), 3u);
    EXPECT_EQ((*arr)[0], "a");
    EXPECT_EQ((*arr)[2], "c");
}

TEST(Toml, EscapedString) {
    auto d = t::parse(R"(s = "a\tb\nc")");
    ASSERT_TRUE(d.has_value());
    EXPECT_EQ(d->get_string("s").value_or(""), std::string("a\tb\nc"));
}

TEST(Toml, RejectUnterminatedString) {
    auto d = t::parse(R"(x = "no end)");
    EXPECT_FALSE(d.has_value());
}

TEST(Toml, CommentsIgnored) {
    auto d = t::parse(R"(
# top comment
x = 1  # trailing isn't supported but full-line is
y = 2
)");
    ASSERT_TRUE(d.has_value()) << d.error().message;
    EXPECT_EQ(d->get_int("x").value_or(-1), 1);
    EXPECT_EQ(d->get_int("y").value_or(-1), 2);
}

TEST(Toml, EscapeStringHelper) {
    EXPECT_EQ(t::escape_string("hello"), "\"hello\"");
    EXPECT_EQ(t::escape_string("a\"b"),  "\"a\\\"b\"");
    EXPECT_EQ(t::escape_string("a\\b"),  "\"a\\\\b\"");
}
