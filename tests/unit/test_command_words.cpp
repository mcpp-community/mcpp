
import std;
import mcpplibs.mtest;
import mcpp.build.command_line;

using namespace mcpplibs::mtest;

// The two readers of a command's words: a POSIX shell's, for the link flags as
// ninja runs them, and a response file's, as GCC, Clang and their linkers read
// one (`@<file>`).

using words = std::vector<std::string>;
using mcpp::build::command_line::response_file_words;
using mcpp::build::command_line::windows_response_file_words;
using mcpp::build::command_line::shell_words;

UTest a_shell_reads_quotes_and_backslashes{"CommandWords/AShellReadsQuotesAndBackslashes", [] {
    expect(eq(shell_words("  -o 'a b'  \"c d\" e\\ f "), (words{"-o", "a b", "c d", "e f"})));
    expect(eq(shell_words("'a\\b'"), (words{"a\\b"})));
    expect(eq(shell_words("\"a\\b\\\"c\""), (words{"a\\b\"c"})));
    expect(eq(shell_words("'it'\\''s'"), (words{"it's"})));
    expect(eq(shell_words("a\\\nb"), (words{"ab"})));
    expect(eq(shell_words("''"), (words{""})));
    expect(eq(shell_words(" \t\n"), (words{})));
}};

UTest a_response_file_takes_the_character_after_every_backslash{"CommandWords/AResponseFileTakesTheCharacterAfterEveryBackslash", [] {
    expect(eq(response_file_words("/a/lib.a\n\"/b c/lib.a\"\r\n'x\\'y'"), (words{"/a/lib.a", "/b c/lib.a", "x'y"})));
    expect(eq(response_file_words("a\\ b \"c\\\\d\""), (words{"a b", "c\\d"})));
}};

// link.exe's rules (CommandLineToArgvW): a backslash is a character unless
// backslashes precede a double quote, and a native path is a word as written.
UTest a_windows_response_file_keeps_its_backslashes{"CommandWords/AWindowsResponseFileKeepsItsBackslashes", [] {
    expect(eq(windows_response_file_words("C:\\lib\\a.lib\r\n\"C:\\x y\\b.lib\"\r\n/OUT:c.exe"),
              (words{"C:\\lib\\a.lib", "C:\\x y\\b.lib", "/OUT:c.exe"})));
    // 2n backslashes before a quote are n and the quote opens or closes; 2n+1
    // are n and a quote; two quotes inside quotes are one.
    expect(eq(windows_response_file_words("a\\\\\"b c\" d\\\"e \"f\"\"g\""),
              (words{"a\\b c", "d\"e", "f\"g"})));
    expect(windows_response_file_words(" \t\r\n").empty());
    expect(eq(windows_response_file_words("\"\""), (words{""}))) << "an empty quoted word is a word";
}};

UTest a_word_quoted_for_a_shell_reads_back_as_itself{"CommandWords/AWordQuotedForAShellReadsBackAsItself", [] {
    for (std::string w : {"plain", "a b", "it's", "a\\b", "$x", "\"q\"", ""})
        expect(eq(shell_words(mcpp::build::command_line::quote_posix(w)), words{w})) << w;
}};
