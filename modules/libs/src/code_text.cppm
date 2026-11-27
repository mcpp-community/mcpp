// mcpp.libs.code_text — a C++ source as the text of its code.
//
// ONE READER. Three parts of mcpp ask what a source says before a compiler has
// read it: the module scanner (which module a unit declares and imports), the
// source seam of the build optimizations (where a private module fragment
// starts), and the manifest's module-name functions (which roots a package
// declares, which it imports, which headers it includes). Each had a reader of
// its own, and they differed in what they took for code: one knew raw strings
// and block comments and nothing of a digit separator, one knew all three, and
// one read lines as they stood, so `export module x;` inside a raw string was a
// declaration to it (mcpp#963). The compiler reads the text one way; so does
// mcpp, here.
//
// The result has the length of the input, offset for offset: what is not code
// is blanked with spaces and the newlines stay, so a line of the result is the
// same line of the source and a column means the same in both. A caller that
// needs what a blanked region held (the name of an included header) reads it
// from the source at the offsets the code gave.
//
// It imports nothing of mcpp.

export module mcpp.libs.code_text;

import std;

export namespace mcpp::libs::code_text {

// The text with comments and the contents of string and character literals
// blanked (raw strings included), and the rest of a directive whose operand is
// not tokens (`#include <a'b>`, `#error don't`). `joins` holds the offsets of a
// backslash that joins its line to the next.
struct Code {
    std::string              text;
    std::vector<std::size_t> joins;
};

Code read(std::string_view text);

} // namespace mcpp::libs::code_text

namespace mcpp::libs::code_text {

namespace {

bool ident_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

// THE TEXT AS C++ TOKENS, to the length it has. Comments and the contents of
// string and character literals are blanked (the newlines stay, so a line is a
// line), and `joins` holds the offsets of a backslash that joins its line to
// the next. Three things a reader by lines gets wrong are read here:
//
//   1'000        a digit separator is part of a number; taken for the start of
//                a character literal it hid the rest of the line, `;` and `{`
//   R"d(...)d"   a raw string runs over lines and holds anything, `"` and
//                `module :private;` included
//   \<newline>   a comment, a literal or a directive goes on to the next line

// The newline at `at` is joined to the next line by a backslash before it.
bool joined(std::string_view t, std::size_t at) {
    if (at > 0 && t[at - 1] == '\r') --at;
    return at > 0 && t[at - 1] == '\\';
}

// The end of the logical line that holds `from`: its newline, or the text's end.
std::size_t line_end(std::string_view t, std::size_t from, std::vector<std::size_t>& joins) {
    for (std::size_t j = from; j < t.size(); ++j) {
        if (t[j] != '\n') continue;
        if (!joined(t, j)) return j;
        joins.push_back(j);
    }
    return t.size();
}

// A raw string opens at the quote at `at` when an `R` (with its encoding
// prefix, if any) stands before it as a word of its own.
bool raw_string_at(std::string_view t, std::size_t at) {
    if (at == 0 || t[at - 1] != 'R') return false;
    std::size_t from = at - 1;
    while (from > 0 && ident_char(t[from - 1])) --from;
    const auto prefix = t.substr(from, at - from);
    return prefix == "R" || prefix == "u8R" || prefix == "uR" || prefix == "UR" || prefix == "LR";
}

// One past the end of the raw string opened at `at`; npos when it is not
// well formed (no `(`, a delimiter of more than 16 characters or with a
// character it may not have, no closing), and the compiler then says why.
std::size_t raw_string_end(std::string_view t, std::size_t at) {
    const auto open = t.find('(', at + 1);
    if (open == std::string_view::npos || open - at - 1 > 16) return std::string_view::npos;
    const auto delimiter = t.substr(at + 1, open - at - 1);
    if (delimiter.find_first_of(" ()\\\t\r\n") != std::string_view::npos) return std::string_view::npos;
    const auto closing = std::format("){}\"", delimiter);
    const auto end = t.find(closing, open + 1);
    return end == std::string_view::npos ? end : end + closing.size();
}

// One past the closing quote of the literal opened by the quote at `at`; where
// a newline comes first the literal is not closed, and ends before it.
std::size_t quoted_end(std::string_view t, std::size_t at, std::vector<std::size_t>& joins) {
    const char quote = t[at];
    for (std::size_t j = at + 1; j < t.size(); ++j) {
        if (t[j] == '\\') {
            std::size_t next = j + 1;
            if (next + 1 < t.size() && t[next] == '\r' && t[next + 1] == '\n') ++next;
            if (next < t.size() && t[next] == '\n') joins.push_back(j);
            j = next;
        } else if (t[j] == quote) {
            return j + 1;
        } else if (t[j] == '\n') {
            return j;
        }
    }
    return t.size();
}

// One past the end of the pp-number that starts at `at`: identifier characters
// and dots, a sign after an exponent, and a `'` before one more of them.
std::size_t number_end(std::string_view t, std::size_t at) {
    std::size_t j = at;
    for (; j < t.size(); ++j) {
        const char c = t[j];
        const bool sign = (c == '+' || c == '-') && j > at && std::string_view("eEpP").contains(t[j - 1]);
        const bool separator = c == '\'' && j + 1 < t.size() && ident_char(t[j + 1]);
        if (!(ident_char(c) || c == '.' || sign || separator)) break;
    }
    return j;
}

// The directives whose rest of the line is not tokens (`#include <a'b>`,
// `#error don't`).
bool text_directive(std::string_view word) {
    return word == "include" || word == "include_next" || word == "import" || word == "error" || word == "warning";
}

} // namespace

Code read(std::string_view t) {
    Code r{std::string(t), {}};
    auto blank = [&](std::size_t from, std::size_t to) {
        for (; from < to && from < r.text.size(); ++from)
            if (r.text[from] != '\n' && r.text[from] != '\r') r.text[from] = ' ';
    };
    bool lineStart = true;
    for (std::size_t i = 0; i < t.size();) {
        const char c = t[i];
        if (c == '\n') { lineStart = true; ++i; continue; }
        if (c == ' ' || c == '\t' || c == '\r') { ++i; continue; }
        if (c == '\\') {
            std::size_t next = i + 1;
            if (next + 1 < t.size() && t[next] == '\r' && t[next + 1] == '\n') ++next;
            if (next < t.size() && t[next] == '\n') { r.joins.push_back(i); i = next + 1; continue; }
        }
        const bool atLineStart = std::exchange(lineStart, false);
        if (c == '#' && atLineStart) {
            std::size_t w = i + 1;
            while (w < t.size() && (t[w] == ' ' || t[w] == '\t')) ++w;
            std::size_t e = w;
            while (e < t.size() && ident_char(t[e])) ++e;
            if (text_directive(t.substr(w, e - w))) {
                const auto end = line_end(t, e, r.joins);
                blank(e, end);
                i = end;
                continue;
            }
        } else if (t.substr(i, 2) == "//") {
            const auto end = line_end(t, i, r.joins);
            blank(i, end);
            i = end;
            continue;
        } else if (t.substr(i, 2) == "/*") {
            const auto close = t.find("*/", i + 2);
            const auto end = close == std::string_view::npos ? t.size() : close + 2;
            blank(i, end);
            i = end;
            continue;
        } else if (c == '"' && raw_string_at(t, i) && raw_string_end(t, i) != std::string_view::npos) {
            const auto end = raw_string_end(t, i);
            blank(i + 1, end - 1);
            i = end;
            continue;
        } else if (c == '"' || c == '\'') {
            const auto end = quoted_end(t, i, r.joins);
            blank(i + 1, end > i + 1 && t[end - 1] == c ? end - 1 : end);
            i = end;
            continue;
        } else if ((std::isdigit(static_cast<unsigned char>(c))
                    || (c == '.' && i + 1 < t.size() && std::isdigit(static_cast<unsigned char>(t[i + 1]))))
                   && (i == 0 || !ident_char(t[i - 1]))) {
            i = number_end(t, i);
            continue;
        }
        ++i;
    }
    return r;
}

} // namespace mcpp::libs::code_text
