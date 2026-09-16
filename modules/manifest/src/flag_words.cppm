// mcpp.manifest.flag_words — what an element of a compile-flag list means.
//
// A manifest states compile flags as lists of strings: `cflags`, `cxxflags`
// and `asmflags`, in `[build]`, in a target, in a glob or feature entry, in a
// `cfg` section, in an xpkg descriptor, and through a build program's
// `mcpp:cflag=` directive. Until 2026.9.17.1 the engine gave an element no
// meaning of its own. It pasted the element into a ninja rule, so the element
// meant whatever the host's command-line reader made of it: POSIX `sh` on
// Linux and macOS, the MSVCRT argument rules on Windows. The compile
// databases then re-read the same text with a third, hand-written reading
// (#655), and a `defines` value lost its quotes on the way to the shell.
//
// This module gives the element one meaning on every host. `flag_words` reads
// an element into the words the compiler receives, and every consumer of a
// flag list reads through it: the ninja writer quotes each word for its host,
// the compile databases list the words, and the assembler filter and the
// include-path normalisation look at words rather than at text. A value the
// engine itself inserts into such a list (a `defines` entry, a feature macro)
// is spelled with `flag_element`, so that it reads back as exactly one word.
//
// THE SYNTAX. It is the POSIX shell's word syntax without expansions, with one
// narrowing that keeps Windows paths intact:
//
//   - unquoted spaces and tabs separate words;
//   - `'...'` is literal up to the next `'`;
//   - `"..."` is literal except that `\"` and `\\` stand for `"` and `\`;
//   - outside quotes, a backslash before a space, a tab, `"`, `'` or `\`
//     stands for that character, and any other backslash is literal;
//   - quoted and unquoted pieces that touch form one word, so `""` is an
//     empty word and `-I"my dir"` is the single word `-Imy dir`.
//
// `$`, `*`, `;` and the other shell operators have no meaning.
//
// ONE EXCEPTION, KEPT FOR COMPATIBILITY. An element that begins with `-D` or
// `/D` and contains a space is one word, taken verbatim. Every release since
// mcpp#234 quoted such an element whole before the host read it, so
// `-DT=long long` has always been the single argument `-DT=long long` on every
// host; reading it as two words would change working manifests.
//
// The one spelling the published index relies on, libarchive's
// `-DPLATFORM_CONFIG_H=\"mcpp_libarchive_config.h\"`, reads as the word
// `-DPLATFORM_CONFIG_H="mcpp_libarchive_config.h"`, which is what both hosts
// passed before. pkg-config's `\ ` for a space in a path reads as a space.
// `C:\Users\x\include` keeps its backslashes, which POSIX `sh` would drop.
//
// `host_command_words` is the other direction's reader: the words a host
// makes of a command line the engine rendered for it. The build database
// needs it for command lines that are rendered text rather than lists (the
// global flag strings, the standard library module's command), and the
// upgrade diagnostic needs it to state what a previous release passed.

export module mcpp.manifest.flag_words;

import std;

export namespace mcpp::manifest {

// The words one element of a compile-flag list stands for. An element of
// spaces only stands for no word. An unterminated quote extends to the end of
// the element.
std::vector<std::string> flag_words(std::string_view element);

// The words of a whole list, in order.
std::vector<std::string> flag_words(const std::vector<std::string>& elements);

// The spelling of one word as a list element: `flag_words(flag_element(w))`
// is `{w}` for every string `w`, the empty string included. A word that needs
// no quoting is returned unchanged, so a plain flag keeps its spelling.
std::string flag_element(std::string_view word);

// The words a host makes of a command line: POSIX `sh` word splitting (quote
// removal and backslash escapes, no expansions) when `windows` is false, the
// MSVCRT argument rules when it is true.
std::vector<std::string> host_command_words(std::string_view command, bool windows);

}  // namespace mcpp::manifest

namespace mcpp::manifest {

namespace {

bool is_blank(char c) { return c == ' ' || c == '\t'; }

bool escapable_outside_quotes(char c) {
    return c == ' ' || c == '\t' || c == '"' || c == '\'' || c == '\\';
}

}  // namespace

std::vector<std::string> flag_words(std::string_view s) {
    if ((s.starts_with("-D") || s.starts_with("/D")) && s.find(' ') != std::string_view::npos)
        return {std::string(s)};
    std::vector<std::string> out;
    std::string word;
    bool started = false;
    auto flush = [&] {
        if (started) out.push_back(std::move(word));
        word.clear();
        started = false;
    };
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (is_blank(c)) { flush(); continue; }
        started = true;
        if (c == '\'') {
            for (++i; i < s.size() && s[i] != '\''; ++i) word.push_back(s[i]);
            continue;
        }
        if (c == '"') {
            for (++i; i < s.size() && s[i] != '"'; ++i) {
                if (s[i] == '\\' && i + 1 < s.size()
                    && (s[i + 1] == '"' || s[i + 1] == '\\')) {
                    ++i;
                }
                word.push_back(s[i]);
            }
            continue;
        }
        if (c == '\\' && i + 1 < s.size() && escapable_outside_quotes(s[i + 1])) {
            word.push_back(s[++i]);
            continue;
        }
        word.push_back(c);
    }
    flush();
    return out;
}

std::vector<std::string> flag_words(const std::vector<std::string>& elements) {
    std::vector<std::string> out;
    for (auto const& e : elements)
        for (auto& w : flag_words(e)) out.push_back(std::move(w));
    return out;
}

std::string flag_element(std::string_view word) {
    // A word with a space that begins with -D or /D is its own element (the
    // exception above); quoting it as well would be equally correct, and
    // leaving it keeps the spelling every release wrote.
    if ((word.starts_with("-D") || word.starts_with("/D")) && word.find(' ') != std::string_view::npos)
        return std::string(word);
    // Plain means the syntax reads the word back unchanged: no blank, no
    // quote, and no backslash in front of a character it would escape. A
    // Windows path (`C:\sdk\include`) is plain and keeps its spelling.
    bool plain = !word.empty() && word.find_first_of(" \t\"'") == std::string_view::npos;
    for (std::size_t i = 0; plain && i + 1 < word.size(); ++i)
        if (word[i] == '\\' && escapable_outside_quotes(word[i + 1])) plain = false;
    if (plain) return std::string(word);
    // Single quotes hold everything except `'`, which closes the region, is
    // written as an escaped quote outside it, and reopens it.
    std::string out = "'";
    for (char c : word) {
        if (c == '\'') out += "'\\''";
        else out.push_back(c);
    }
    out += "'";
    return out;
}

std::vector<std::string> host_command_words(std::string_view s, bool windows) {
    std::vector<std::string> out;
    std::string cur;
    bool started = false;
    auto flush = [&] {
        if (started) out.push_back(std::move(cur));
        cur.clear();
        started = false;
    };
    if (windows) {
        bool quoted = false;
        for (std::size_t i = 0; i < s.size(); ++i) {
            const char c = s[i];
            if (c == '\\') {
                std::size_t j = i;
                while (j < s.size() && s[j] == '\\') ++j;
                const std::size_t count = j - i;
                started = true;
                if (j < s.size() && s[j] == '"') {
                    cur.append(count / 2, '\\');
                    if (count % 2 == 1) { cur.push_back('"'); i = j; }
                    else                { i = j - 1; }
                    continue;
                }
                cur.append(count, '\\');
                i = j - 1;
                continue;
            }
            if (c == '"') { quoted = !quoted; started = true; continue; }
            if (!quoted && is_blank(c)) { flush(); continue; }
            cur.push_back(c);
            started = true;
        }
        flush();
        return out;
    }
    char quote = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (quote == '\'') {
            if (c == '\'') quote = 0; else cur.push_back(c);
            continue;
        }
        if (quote == '"') {
            if (c == '"') { quote = 0; continue; }
            if (c == '\\' && i + 1 < s.size()
                && (s[i + 1] == '"' || s[i + 1] == '\\' || s[i + 1] == '$'
                    || s[i + 1] == '`' || s[i + 1] == '\n')) {
                if (s[i + 1] != '\n') cur.push_back(s[i + 1]);
                ++i;
                continue;
            }
            cur.push_back(c);
            continue;
        }
        if (c == '\'' || c == '"') { quote = c; started = true; continue; }
        if (c == '\\' && i + 1 < s.size()) {
            if (s[i + 1] != '\n') { cur.push_back(s[i + 1]); started = true; }
            ++i;
            continue;
        }
        if (is_blank(c) || c == '\n') { flush(); continue; }
        cur.push_back(c);
        started = true;
    }
    flush();
    return out;
}

}  // namespace mcpp::manifest
