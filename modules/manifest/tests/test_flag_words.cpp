
import std;
import mcpplibs.mtest;
import mcpp.manifest.flag_words;

using namespace mcpplibs::mtest;

// SUBSYSTEM-LEVEL. The words an element of a compile-flag list stands for are
// part of what a manifest MEANS, so the syntax is stated here, with nothing
// else present. That the build and the compile databases both use it is
// stated in tests/unit (test_compile_commands.cpp).

using mcpp::manifest::flag_element;
using mcpp::manifest::flag_words;
using mcpp::manifest::host_command_words;
using Words = std::vector<std::string>;

// Each row is a spelling found in a manifest or a descriptor and the words it
// stands for on every host. The first rows are the ones #655 measured.
UTest the_syntax_table{"FlagWords/TheSyntaxTable", [] {
    const std::vector<std::pair<std::string, Words>> rows{
        // libarchive's descriptor, the one quoted spelling the index publishes.
        {R"(-DPLATFORM_CONFIG_H=\"mcpp_libarchive_config.h\")",
         {R"(-DPLATFORM_CONFIG_H="mcpp_libarchive_config.h")"}},
        // Quotes inside a word are quoting, as in `sh`.
        {R"(-DMID="mid")", {"-DMID=mid"}},
        {"-DSQ='sq'", {"-DSQ=sq"}},
        {"'-DV=a b'", {"-DV=a b"}},
        // The compatibility exception: a -D or /D element with a space is one
        // word, verbatim (mcpp#234), quotes and backslashes included.
        {"-DT=long long", {"-DT=long long"}},
        {R"(-DV="a b")", {R"(-DV="a b")"}},
        {"/DV=a b", {"/DV=a b"}},
        {"-DA=1 -DB=2", {"-DA=1 -DB=2"}},
        // compat.lua's descriptor: one element, two words.
        {"-include mcpp_lua_platform_config.h", {"-include", "mcpp_lua_platform_config.h"}},
        // pkg-config writes a space in a path as `\ `.
        {R"(-I/opt/my\ dir/include)", {"-I/opt/my dir/include"}},
        // A backslash before an ordinary character is literal: Windows paths.
        {R"(-IC:\Users\x\include)", {R"(-IC:\Users\x\include)"}},
        {R"("-IC:\Program Files\x")", {R"(-IC:\Program Files\x)"}},
        // Without a space the syntax applies to a -D element as to any other.
        {R"(-DV="mid")", {"-DV=mid"}},
        // Escapes a backslash can make outside and inside double quotes.
        {R"(-DV=a\\b)", {R"(-DV=a\b)"}},
        {R"("a\"b\\c\d")", {R"(a"b\c\d)"}},
        {R"(-DV=\'c\')", {"-DV='c'"}},
        // Single quotes hold backslashes and double quotes literally.
        {R"('a\"b')", {R"(a\"b)"}},
        // No expansions: `$` and the shell operators are ordinary characters.
        {"-DV=$HOME;x|y", {"-DV=$HOME;x|y"}},
        // Blanks separate words; tabs count; runs collapse.
        {" -O2\t -g  ", {"-O2", "-g"}},
        // Empty and blank elements.
        {"", {}},
        {"   ", {}},
        {R"("")", {""}},
        {"''", {""}},
        {R"(-DV="")", {"-DV="}},
        // An unterminated quote extends to the end of the element.
        {R"(-I"a b)", {"-Ia b"}},
        // A trailing backslash has nothing to escape.
        {R"(-DV=a\)", {R"(-DV=a\)"}},
    };
    for (auto const& [element, words] : rows)
        expect(eq(flag_words(element), words)) << "element: " << element;
}};

UTest a_list_is_the_concatenation_of_its_elements{"FlagWords/AListIsTheConcatenationOfItsElements", [] {
    const std::vector<std::string> list{"-include x.h", R"(-I"my dir")", "", "-O2"};
    expect(eq(flag_words(list), (Words{"-include", "x.h", "-Imy dir", "-O2"})));
}};

UTest a_plain_word_is_its_own_element{"FlagWords/APlainWordIsItsOwnElement", [] {
    for (std::string_view w : {"-O2", "-DV=1", "-IC:/x", "/std:c++latest", "-Wl,-z,defs", "$x",
                               R"(-IC:\sdk\include)", R"(C:\dir\)"})
        expect(eq(flag_element(w), w));
}};

// The property the engine relies on when it inserts a value into a flag list:
// the element it writes reads back as exactly that one word.
UTest an_element_reads_back_as_its_word{"FlagWords/AnElementReadsBackAsItsWord", [] {
    Words words{"", " ", "a b", "'", "\"", "\\", "a\\", "\\\\srv\\share", "it's",
                "-DV=\"def\"", "-DV='c'", "x\ty", "a\"b'c\\d e", "-DT=long long",
                "-DV=\"a b\"", "/DV=x y"};
    // A deterministic walk over the characters the syntax gives meaning to.
    const std::string alphabet = "ab \t\"'\\$-D";
    std::uint32_t state = 655;
    for (int n = 0; n < 2000; ++n) {
        std::string w;
        const int length = static_cast<int>(state % 9);
        for (int i = 0; i < length; ++i) {
            state = state * 1664525u + 1013904223u;
            w.push_back(alphabet[(state >> 16) % alphabet.size()]);
        }
        state = state * 1664525u + 1013904223u;
        words.push_back(std::move(w));
    }
    for (auto const& w : words)
        expect(eq(flag_words(flag_element(w)), Words{w})) << "word: [" << w << "]";
}};

UTest posix_undoes_shell_quoting{"HostCommandWords/PosixUndoesShellQuoting", [] {
    expect(eq(host_command_words(R"(a 'b c' "d\"e" f\ g "h\x" -DV="mid")", false), (Words{"a", "b c", "d\"e", "f g", "h\\x", "-DV=mid"})));
}};

UTest windows_follows_the_runtime_rules{"HostCommandWords/WindowsFollowsTheRuntimeRules", [] {
    expect(eq(host_command_words(R"(a "b c" "d\"e" C:\x\ "y\\" z\\\"w 'q')", true), (Words{"a", "b c", "d\"e", "C:\\x\\", "y\\", "z\\\"w", "'q'"})));
}};
