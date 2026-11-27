// mcpp.buildopt.source — which units a source is built as (SPEC-019 §5).
//
// THE SOURCE SEAM. A source file is one unit of the graph as it is written.
// An entry of this part gives the units it is built as instead, as text: the
// pipeline writes them, scans them as it scans any source, and knows nothing
// of why. An entry that cannot give units it is sure of gives a reason, and
// the source is built as written.
//
// ONE ENTRY: THE PRIVATE MODULE FRAGMENT AS A UNIT OF ITS OWN.
//
//   export module m;          ->   export module m;          (interface unit)
//   export int f();                export int f();
//   module :private;
//   int f() { return 1; }     ->   module m;                 (implementation unit)
//                                  int f() { return 1; }
//
// The language already says what the split says: nothing in the fragment is
// reachable from an importer ([module.private.frag]). The compilers do not
// build it that way. Measured on a module `c` imported by `b`, imported by
// `main` (2026-11-17), editing the body of a function defined in the fragment:
//
//   clang 23.1.3   c, b and main are compiled: the BMI of `c` changes with any
//                  byte of its source (reduced BMIs, the default, included:
//                  153 bytes differ)
//   gcc 16.1.0     the source does not build at all (`module :private;` is not
//                  implemented)
//
// and, built as the two units above, on both compilers: the implementation
// unit alone is compiled. Both units carry `#line` to the source as written,
// so a diagnostic, debug information and `__FILE__` name it.
//
// WHAT MAKES A FRAGMENT NOT MOVABLE, AND IS THEREFORE LEFT AS WRITTEN. The
// implementation unit sees what the interface declares in the module's
// purview; it does not see what is local to the interface's translation unit,
// nor what the preprocessor held there. Read from the text, with no semantic
// analysis, and refused when in doubt:
//
//   - a directive in the purview before the fragment that changes what the
//     fragment means: `#define`, `#undef`, `#include`, `#pragma`, `#line`, and
//     any not named below. The conditionals (`#if`, `#ifdef`, `#ifndef`,
//     `#elif`, `#else`, `#endif`) and `#error`, `#warning` do not: a condition
//     only decides which declarations exist, and the implementation unit
//     receives them from the BMI. A condition that is open at the module
//     declaration or at the fragment is refused, since the units would not
//     close it. (The global module fragment is copied, and is not this.)
//   - a use of `_Pragma` or `__pragma` there
//   - an unnamed namespace, or `static` at namespace scope (a linkage
//     specification `extern "C" { ... }` is namespace scope), before the
//     fragment
//   - a using-directive at namespace scope before the fragment
//   - a line continuation (`\` before a newline) from the module declaration
//     to the fragment, where a physical line is not a logical one
//   - a module that is a partition, or no module declaration at all
//
// The text is read once as C++ tokens: comments, string and character
// literals (raw strings included) are blanked, and a digit separator
// (`1'000`) is part of a number, not the start of a literal. With
// `private_fragment = "conservative"` any directive in that purview refuses,
// which is the rule before the narrowed one and the one a compiler that
// cannot build a fragment as written (GCC) is checked against.
//
// A fragment that passes these and still does not build as a unit names its
// own lines in the compiler's error; `private_fragment = "as-written"` builds
// the source as it is.

export module mcpp.buildopt.source;

import std;
import mcpp.invocation;
import mcpp.libs.code_text;
import mcpp.manifest;
import mcpp.buildopt.registry;

export namespace mcpp::buildopt::source {

struct Units {
    std::string moduleName;          // the module's name
    std::string interfaceUnit;   // the text of the interface unit
    std::string implementation;  // the text of the implementation unit
};

// The units a source with a private module fragment is built as. nullopt: the
// text has no private module fragment. An error: it has one that stays as
// written, and why. `spelling` is the file's path as `#line` names it.
// `conservative`: any directive in the purview before the fragment refuses,
// as it did before the conditionals were let through.
std::optional<std::expected<Units, std::string>>
private_fragment(std::string_view text, std::string_view spelling, bool conservative = false);

// How this build builds private module fragments: the entry's switch
// (`MCPP_PRIVATE_FRAGMENT`, then `[build] private_fragment`, then the
// default, "unit"), under the statement that turns every entry off. A value
// that is none of the three is handed to `onInvalid` and the default stands.
enum class Fragment { AsWritten, Unit, Conservative };
Fragment private_fragment_policy(const mcpp::manifest::Manifest& m,
                                 const std::function<void(std::string_view)>& onInvalid = {});

} // namespace mcpp::buildopt::source

namespace mcpp::buildopt::source {

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

bool ident_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

bool word_at(std::string_view s, std::size_t at, std::string_view word) {
    if (s.substr(at, word.size()) != word) return false;
    if (at > 0 && ident_char(s[at - 1])) return false;
    return at + word.size() >= s.size() || !ident_char(s[at + word.size()]);
}

bool has_word(std::string_view s, std::string_view word) {
    for (auto at = s.find(word); at != std::string_view::npos; at = s.find(word, at + 1))
        if (word_at(s, at, word)) return true;
    return false;
}

// The word after the `#` of a directive line (empty for a line that is `#`
// alone, and for one whose next token is not a word: `# 12 "file"`); nullopt
// when the line is not a directive.
std::optional<std::string_view> directive_of(std::string_view line) {
    line = trim(line);
    if (!line.starts_with('#')) return std::nullopt;
    line = trim(line.substr(1));
    std::size_t n = 0;
    while (n < line.size() && ident_char(line[n])) ++n;
    return n == 0 && !line.empty() ? std::string_view("?") : line.substr(0, n);
}

// What a directive does to the fragment's meaning. The conditionals only
// decide which declarations exist, and the implementation unit receives those
// from the interface; `#error` and `#warning` are said where they stand.
bool opens_condition(std::string_view w) { return w == "if" || w == "ifdef" || w == "ifndef"; }
bool leaves_meaning(std::string_view w) {
    return w.empty() || opens_condition(w) || w == "endif" || w == "else" || w == "elif" || w == "elifdef"
        || w == "elifndef" || w == "error" || w == "warning";
}

std::string line_directive(std::size_t line, std::string_view spelling) {
    std::string quoted;
    for (char c : spelling) {
        if (c == '\\' || c == '"') quoted += '\\';
        quoted += c;
    }
    return std::format("#line {} \"{}\"\n", line, quoted);
}

// The conditional directives open at line `to` (not counting it).
int open_conditions(const std::vector<std::string>& code, std::size_t to) {
    int depth = 0;
    for (std::size_t i = 0; i < to; ++i) {
        const auto w = directive_of(code[i]);
        if (!w) continue;
        if (opens_condition(*w)) ++depth;
        else if (*w == "endif") --depth;
    }
    return depth;
}

} // namespace

std::optional<std::expected<Units, std::string>>
private_fragment(std::string_view text, std::string_view spelling, bool conservative) {
    std::vector<std::string_view> lines;
    std::vector<std::size_t> starts;
    for (std::size_t at = 0; at <= text.size();) {
        const auto end = text.find('\n', at);
        starts.push_back(at);
        lines.push_back(text.substr(at, end == std::string_view::npos ? std::string_view::npos : end - at));
        if (end == std::string_view::npos) break;
        at = end + 1;
    }
    const auto scanned = mcpp::libs::code_text::read(text);
    std::vector<std::string> code;
    for (std::size_t i = 0; i < lines.size(); ++i)
        code.emplace_back(std::string_view(scanned.text).substr(starts[i], lines[i].size()));
    auto compact = [](std::string_view s) {
        std::string c;
        for (char ch : s)
            if (ch != ' ' && ch != '\t' && ch != '\r') c += ch;
        return c;
    };

    std::optional<std::size_t> declared, fragment;
    std::string moduleName;
    for (std::size_t i = 0; i < lines.size() && !fragment; ++i) {
        const auto c = trim(code[i]);
        if (compact(c) == "module:private;") { fragment = i; break; }
        if (!declared && c.starts_with("export") && word_at(c, 0, "export")) {
            const auto rest = trim(c.substr(6));
            if (rest.starts_with("module") && word_at(rest, 0, "module")) {
                auto name = trim(rest.substr(6));
                const auto end = name.find_first_of("; \t[");
                moduleName = std::string(name.substr(0, end));
                declared = i;
            }
        }
    }
    if (!fragment) return std::nullopt;
    using Result = std::expected<Units, std::string>;
    if (!declared || moduleName.empty())
        return Result{std::unexpected("no `export module` declaration is read before the fragment")};
    if (moduleName.find(':') != std::string::npos)
        return Result{std::unexpected("the unit is a partition")};

    // Where a physical line is not a logical one, the lines below are not the
    // text the compiler reads.
    for (auto at : scanned.joins) {
        const auto line = static_cast<std::size_t>(std::ranges::upper_bound(starts, at) - starts.begin()) - 1;
        if (line >= *declared && line <= *fragment)
            return Result{std::unexpected(std::format(
                "line {}: a line continuation between the module declaration and the fragment", line + 1))};
    }
    // A condition that holds the declaration or the fragment is not closed by
    // the unit that ends before the one and begins at the other.
    if (!conservative) {
        if (open_conditions(code, *declared) != 0)
            return Result{std::unexpected(std::format(
                "line {}: a conditional directive is open at the module declaration", *declared + 1))};
        if (open_conditions(code, *fragment) != 0)
            return Result{std::unexpected(std::format(
                "line {}: a conditional directive is open at the fragment", *fragment + 1))};
    }

    // The purview before the fragment: what is local to the interface's
    // translation unit, and so not seen by an implementation unit.
    std::vector<bool> scopes;   // one a brace still open; true: a namespace's
    std::string statement;      // the code since the last `;`, `{` or `}`
    for (std::size_t i = *declared + 1; i < *fragment; ++i) {
        const auto c = trim(code[i]);
        if (const auto word = directive_of(c)) {
            if (conservative || !leaves_meaning(*word))
                return Result{std::unexpected(std::format(
                    "line {}: a preprocessor directive in the module's purview before the fragment", i + 1))};
            continue;
        }
        if (has_word(c, "_Pragma") || has_word(c, "__pragma"))
            return Result{std::unexpected(std::format(
                "line {}: a pragma in the module's purview before the fragment", i + 1))};
        for (char ch : c) {
            if (ch == '{') {
                const bool ns = has_word(statement, "namespace") && statement.find('(') == std::string::npos;
                if (ns && has_word(statement, "namespace")) {
                    // `namespace {`, `inline namespace {`: no name after the keyword.
                    const auto at = statement.rfind("namespace");
                    if (trim(std::string_view(statement).substr(at + 9)).empty())
                        return Result{std::unexpected(std::format(
                            "line {}: an unnamed namespace before the fragment", i + 1))};
                }
                // A function defined at namespace scope: `static int f() {`.
                if (const auto st = trim(statement); !ns && st.starts_with("static") && word_at(st, 0, "static")
                    && std::ranges::all_of(scopes, [](bool s) { return s; }))
                    return Result{std::unexpected(std::format(
                        "line {}: `static` at namespace scope before the fragment", i + 1))};
                // `extern "C" {` holds declarations of the scope it stands in.
                const auto st = trim(statement);
                const bool linkage = st.starts_with("extern") && word_at(st, 0, "extern")
                    && trim(st.substr(6)).starts_with('"') && trim(st.substr(6)).ends_with('"');
                scopes.push_back(ns || linkage);
                statement.clear();
            } else if (ch == '}') {
                if (!scopes.empty()) scopes.pop_back();
                statement.clear();
            } else if (ch == ';') {
                const bool atNamespace = std::ranges::all_of(scopes, [](bool ns) { return ns; });
                const auto st = trim(statement);
                if (atNamespace && (st.starts_with("static") && word_at(st, 0, "static")))
                    return Result{std::unexpected(std::format(
                        "line {}: `static` at namespace scope before the fragment", i + 1))};
                if (atNamespace && st.starts_with("using") && has_word(st, "namespace"))
                    return Result{std::unexpected(std::format(
                        "line {}: a using-directive at namespace scope before the fragment", i + 1))};
                statement.clear();
            } else {
                statement += ch;
            }
        }
        statement += ' ';
    }

    Units units;
    units.moduleName = moduleName;
    units.interfaceUnit = line_directive(1, spelling);
    for (std::size_t i = 0; i < *fragment; ++i) {
        units.interfaceUnit += lines[i];
        units.interfaceUnit += '\n';
    }
    // The global module fragment as written (`module;` and what it includes),
    // then the module, then the fragment at the lines it has in the source.
    units.implementation = line_directive(1, spelling);
    for (std::size_t i = 0; i < *declared; ++i) {
        units.implementation += lines[i];
        units.implementation += '\n';
    }
    units.implementation += std::format("module {};\n", moduleName);
    units.implementation += line_directive(*fragment + 2, spelling);
    for (std::size_t i = *fragment + 1; i < lines.size(); ++i) {
        units.implementation += lines[i];
        if (i + 1 < lines.size()) units.implementation += '\n';
    }
    return Result{std::move(units)};
}

Fragment private_fragment_policy(const mcpp::manifest::Manifest& m,
                                 const std::function<void(std::string_view)>& onInvalid) {
    if (registry::standard_only()) return Fragment::AsWritten;
    std::string v;
    if (auto e = mcpp::invocation::get("MCPP_PRIVATE_FRAGMENT")) v = *e;
    else v = m.buildConfig.privateFragment;
    if (v.empty() || v == "unit") return Fragment::Unit;
    if (v == "conservative") return Fragment::Conservative;
    if (v == "as-written") return Fragment::AsWritten;
    if (onInvalid) onInvalid(v);
    return Fragment::Unit;
}

} // namespace mcpp::buildopt::source
