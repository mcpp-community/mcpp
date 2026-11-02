#include <gtest/gtest.h>

import std;
import mcpp.build.command_line;

// The two readers of a command's words: a POSIX shell's, for the link flags as
// ninja runs them, and a response file's, as GCC, Clang and their linkers read
// one (`@<file>`).

using words = std::vector<std::string>;
using mcpp::build::command_line::response_file_words;
using mcpp::build::command_line::shell_words;

TEST(CommandWords, AShellReadsQuotesAndBackslashes) {
    EXPECT_EQ(shell_words("  -o 'a b'  \"c d\" e\\ f "), (words{"-o", "a b", "c d", "e f"}));
    EXPECT_EQ(shell_words("'a\\b'"), (words{"a\\b"}));
    EXPECT_EQ(shell_words("\"a\\b\\\"c\""), (words{"a\\b\"c"}));
    EXPECT_EQ(shell_words("'it'\\''s'"), (words{"it's"}));
    EXPECT_EQ(shell_words("a\\\nb"), (words{"ab"}));
    EXPECT_EQ(shell_words("''"), (words{""}));
    EXPECT_EQ(shell_words(" \t\n"), (words{}));
}

TEST(CommandWords, AResponseFileTakesTheCharacterAfterEveryBackslash) {
    EXPECT_EQ(response_file_words("/a/lib.a\n\"/b c/lib.a\"\r\n'x\\'y'"),
              (words{"/a/lib.a", "/b c/lib.a", "x'y"}));
    EXPECT_EQ(response_file_words("a\\ b \"c\\\\d\""), (words{"a b", "c\\d"}));
}

TEST(CommandWords, AWordQuotedForAShellReadsBackAsItself) {
    for (std::string w : {"plain", "a b", "it's", "a\\b", "$x", "\"q\"", ""})
        EXPECT_EQ(shell_words(mcpp::build::command_line::quote_posix(w)), words{w}) << w;
}
