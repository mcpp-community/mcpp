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
// purview; it does not see what is local to the interface's translation unit.
// Read from the text, with no semantic analysis, and refused when in doubt:
//
//   - a preprocessor directive in the purview before the fragment (a macro the
//     fragment may use; the global module fragment is copied, and is not this)
//   - an unnamed namespace, or `static` at namespace scope, before the fragment
//   - a using-directive at namespace scope before the fragment
//   - a module that is a partition, or no module declaration at all
//
// A fragment that passes these and still does not build as a unit names its
// own lines in the compiler's error; `private_fragment = "as-written"` builds
// the source as it is.

export module mcpp.buildopt.source;

import std;
import mcpp.invocation;
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
std::optional<std::expected<Units, std::string>>
private_fragment(std::string_view text, std::string_view spelling);

// Whether this build builds private module fragments as units: the entry's
// switch (`MCPP_PRIVATE_FRAGMENT`, then `[build] private_fragment`, then the
// default, "unit"), under the statement that turns every entry off. A value
// that is neither "unit" nor "as-written" is handed to `onInvalid` and the
// default stands.
bool private_fragment_as_unit(const mcpp::manifest::Manifest& m,
                              const std::function<void(std::string_view)>& onInvalid = {});

} // namespace mcpp::buildopt::source

namespace mcpp::buildopt::source {

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

bool word_at(std::string_view s, std::size_t at, std::string_view word) {
    auto ident = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
    if (s.substr(at, word.size()) != word) return false;
    if (at > 0 && ident(s[at - 1])) return false;
    return at + word.size() >= s.size() || !ident(s[at + word.size()]);
}

bool has_word(std::string_view s, std::string_view word) {
    for (auto at = s.find(word); at != std::string_view::npos; at = s.find(word, at + 1))
        if (word_at(s, at, word)) return true;
    return false;
}

// One line's code: comments removed, string and character literals blanked.
// `inBlock` carries a block comment over lines.
std::string code_of(std::string_view line, bool& inBlock) {
    std::string out;
    for (std::size_t i = 0; i < line.size(); ++i) {
        if (inBlock) {
            if (line.substr(i, 2) == "*/") { inBlock = false; ++i; }
            continue;
        }
        if (line.substr(i, 2) == "//") break;
        if (line.substr(i, 2) == "/*") { inBlock = true; ++i; continue; }
        if (line[i] == '"' || line[i] == '\'') {
            const char quote = line[i];
            out += quote;
            for (++i; i < line.size() && line[i] != quote; ++i)
                if (line[i] == '\\') ++i;
            out += quote;
            continue;
        }
        out += line[i];
    }
    return out;
}

std::string line_directive(std::size_t line, std::string_view spelling) {
    std::string quoted;
    for (char c : spelling) {
        if (c == '\\' || c == '"') quoted += '\\';
        quoted += c;
    }
    return std::format("#line {} \"{}\"\n", line, quoted);
}

} // namespace

std::optional<std::expected<Units, std::string>>
private_fragment(std::string_view text, std::string_view spelling) {
    std::vector<std::string_view> lines;
    for (std::size_t at = 0; at <= text.size();) {
        const auto end = text.find('\n', at);
        lines.push_back(text.substr(at, end == std::string_view::npos ? std::string_view::npos : end - at));
        if (end == std::string_view::npos) break;
        at = end + 1;
    }
    std::vector<std::string> code(lines.size());
    {
        bool inBlock = false;
        for (std::size_t i = 0; i < lines.size(); ++i) code[i] = code_of(lines[i], inBlock);
    }
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

    // The purview before the fragment: what is local to the interface's
    // translation unit, and so not seen by an implementation unit.
    std::vector<bool> scopes;   // one a brace still open; true: a namespace's
    std::string statement;      // the code since the last `;`, `{` or `}`
    for (std::size_t i = *declared + 1; i < *fragment; ++i) {
        const auto c = trim(code[i]);
        if (c.starts_with("#"))
            return Result{std::unexpected(std::format(
                "line {}: a preprocessor directive in the module's purview before the fragment", i + 1))};
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
                scopes.push_back(ns);
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
        // A function defined at namespace scope: `static int f() {` reaches
        // the brace before any `;`.
        const bool atNamespace = std::ranges::all_of(scopes, [](bool ns) { return ns; });
        if (const auto st = trim(statement); atNamespace && st.starts_with("static") && word_at(st, 0, "static"))
            return Result{std::unexpected(std::format(
                "line {}: `static` at namespace scope before the fragment", i + 1))};
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

bool private_fragment_as_unit(const mcpp::manifest::Manifest& m,
                              const std::function<void(std::string_view)>& onInvalid) {
    if (registry::standard_only()) return false;
    std::string v;
    if (auto e = mcpp::invocation::get("MCPP_PRIVATE_FRAGMENT")) v = *e;
    else v = m.buildConfig.privateFragment;
    if (v.empty() || v == "unit") return true;
    if (v == "as-written") return false;
    if (onInvalid) onInvalid(v);
    return true;
}

} // namespace mcpp::buildopt::source
