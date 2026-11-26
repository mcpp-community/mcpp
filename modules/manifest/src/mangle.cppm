// mcpp.pm.mangle — multi-version coexistence (Level 1 of the dep
// resolver's three-step strategy).
//
// When the SemVer merger (`try_merge_semver`) fails because two
// transitive consumers of the same package can't share a version,
// the resolver picks one as "primary" (keeps its module name as
// authored) and rewrites the secondary copy under a mangled name so
// both BMIs can coexist in the build graph without ODR clashes.
//
// This module owns the **pure** half of that work:
//   * `mangle_name` decides the mangled name format
//   * `rewrite_module_decls` does a regex pass over a single .cppm
//     file's text, rewriting module / import declarations whose name
//     appears in a caller-supplied rename table.
//
// The orchestration half (deciding which package gets mangled,
// staging files into a per-build directory, splicing the staged
// PackageRoot into the resolver output) lives in `cli.cppm`.

export module mcpp.pm.mangle;

import std;
import mcpp.libs.code_text;

export namespace mcpp::pm {

// Mangled module name format: `<base>__v<major>_<minor>_<patch>__mcpp`.
// The double underscores keep the suffix outside the user namespace
// (C++ ABI reserves `__` for the implementation, so users can't
// collide), and the trailing `__mcpp` makes link-error backtraces
// obviously mcpp-generated rather than mistaken for hand-rolled.
//
// Dots in `version` become underscores so the result is a valid
// C++ module identifier (modules allow `.` but using it here would
// confuse partition-style readings later).
std::string mangle_name(std::string_view base, std::string_view version);

// Primary module roots declared by source text, in first-seen order. Partition
// declarations return their owning root; imports, global module fragments and
// bare partition declarations are ignored. The text is read as code
// (mcpp.libs.code_text): `export module x;` inside a raw string or a comment
// declares nothing, as it declares nothing to the compiler (mcpp#963).
std::vector<std::string> declared_module_roots(std::string_view source);

// The module names a source IMPORTS, in order of appearance, deduplicated.
// `import N;` and `export import N;` yield `N`; `import N:P;` yields `N`;
// header units (`import <vector>;`, `import "x.h";`) and bare partition
// imports (`import :P;`) yield nothing, because none of them names a module
// another package could provide.
//
// The same matcher as `declared_module_roots`, over the same code text: the
// keyword opens a line of code, so an `import` inside a comment or a literal is
// not one.
std::vector<std::string> imported_module_names(std::string_view source);

// The headers a source includes, as written with their delimiters (`<cstdio>`, `"emit.hpp"`), in
// order of appearance, deduplicated. The same matcher: `#include` opens a line of code, so one in
// a comment or a raw string is not read; `#include MACRO` names nothing and yields nothing.
std::vector<std::string> included_headers(std::string_view source);

// Rewrite a single .cppm file's module / import declarations:
//   * `(export )?module N;`     → `(export )?module rename[N];`
//   * `(export )?module N:P;`   → `(export )?module rename[N]:P;`
//   * `(export )?import N;`     → `(export )?import rename[N];`
//   * `(export )?import N:P;`   → `(export )?import rename[N]:P;`
//
// Names not present in the `rename` table are left intact. Bare
// partition imports (`import :P;`) and the global module fragment
// (`module ;`) are also left intact — they don't name the enclosing
// module so they need no rewriting.
//
// The keyword (`module` / `import`) opens a line of the code text
// (mcpp.libs.code_text), so a declaration spelled inside a comment or
// a literal is copied as it stands. What is written is the source: a
// comment before the keyword on its line is kept.
std::string rewrite_module_decls(
    std::string_view source,
    const std::map<std::string, std::string>& rename);

} // namespace mcpp::pm

namespace mcpp::pm {

std::string mangle_name(std::string_view base, std::string_view version) {
    std::string vmangled;
    vmangled.reserve(version.size());
    for (char c : version) vmangled += (c == '.' ? '_' : c);
    return std::format("{}__v{}__mcpp", base, vmangled);
}

namespace {

// Module names are dotted identifiers: [A-Za-z_][A-Za-z0-9_.]*
bool is_name_start(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}
bool is_name_cont(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.';
}

// Skip whitespace (spaces, tabs) in `s` from `i`. Returns new index.
std::size_t skip_ws(std::string_view s, std::size_t i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return i;
}

// Try to consume the keyword `kw` at position `i`, requiring word boundary.
// Returns the index past `kw`, or std::string::npos on miss.
std::size_t consume_keyword(std::string_view s, std::size_t i,
                            std::string_view kw)
{
    if (i + kw.size() > s.size()) return std::string::npos;
    if (s.substr(i, kw.size()) != kw) return std::string::npos;
    std::size_t after = i + kw.size();
    if (after < s.size() && is_name_cont(s[after])) return std::string::npos;
    return after;
}

// Parse a dotted identifier starting at `i`. Returns end index (first
// position past the name) and the name slice. Returns npos if no name.
std::pair<std::size_t, std::string_view>
read_name(std::string_view s, std::size_t i)
{
    if (i >= s.size() || !is_name_start(s[i])) return {std::string::npos, {}};
    std::size_t start = i;
    ++i;
    while (i < s.size() && is_name_cont(s[i])) ++i;
    return {i, s.substr(start, i - start)};
}

} // namespace

std::vector<std::string> declared_module_roots(std::string_view text) {
    const std::string code = mcpp::libs::code_text::read(text).text;
    const std::string_view source = code;
    std::vector<std::string> roots;
    std::size_t lineStart = 0;
    while (lineStart < source.size()) {
        auto eol = source.find('\n', lineStart);
        if (eol == std::string_view::npos) eol = source.size();
        auto line = source.substr(lineStart, eol - lineStart);

        auto cur = skip_ws(line, 0);
        if (auto p = consume_keyword(line, cur, "export");
            p != std::string::npos) {
            cur = skip_ws(line, p);
        }
        auto afterModule = consume_keyword(line, cur, "module");
        if (afterModule != std::string::npos) {
            cur = skip_ws(line, afterModule);
            if (cur < line.size() && line[cur] != ';' && line[cur] != ':') {
                auto [nameEnd, name] = read_name(line, cur);
                if (nameEnd != std::string::npos
                    && std::ranges::find(roots, name) == roots.end()) {
                    roots.emplace_back(name);
                }
            }
        }
        if (eol == source.size()) break;
        lineStart = eol + 1;
    }
    return roots;
}

std::vector<std::string> imported_module_names(std::string_view text) {
    const std::string code = mcpp::libs::code_text::read(text).text;
    const std::string_view source = code;
    std::vector<std::string> names;
    std::size_t lineStart = 0;
    while (lineStart < source.size()) {
        auto eol = source.find('\n', lineStart);
        if (eol == std::string_view::npos) eol = source.size();
        auto line = source.substr(lineStart, eol - lineStart);

        auto cur = skip_ws(line, 0);
        if (auto p = consume_keyword(line, cur, "export");
            p != std::string::npos) {
            cur = skip_ws(line, p);
        }
        if (auto afterImport = consume_keyword(line, cur, "import");
            afterImport != std::string::npos) {
            cur = skip_ws(line, afterImport);
            auto [nameEnd, name] = read_name(line, cur);
            if (nameEnd != std::string::npos
                && std::ranges::find(names, name) == names.end()) {
                names.emplace_back(name);
            }
        }
        if (eol == source.size()) break;
        lineStart = eol + 1;
    }
    return names;
}

std::vector<std::string> included_headers(std::string_view source) {
    // The directive is matched in the code text, whose rest of an `#include`
    // line is blanked (a header name is not tokens); the name is read from the
    // source at the same offsets.
    const std::string code = mcpp::libs::code_text::read(source).text;
    std::vector<std::string> out;
    std::size_t lineStart = 0;
    while (lineStart < source.size()) {
        auto eol = source.find('\n', lineStart);
        if (eol == std::string_view::npos) eol = source.size();
        auto line = source.substr(lineStart, eol - lineStart);
        const auto codeLine = std::string_view(code).substr(lineStart, eol - lineStart);
        auto cur = skip_ws(codeLine, 0);
        if (cur < codeLine.size() && codeLine[cur] == '#') {
            cur = skip_ws(codeLine, cur + 1);
            if (auto p = consume_keyword(codeLine, cur, "include"); p != std::string::npos) {
                cur = skip_ws(line, p);
                const char close = cur < line.size() ? (line[cur] == '<' ? '>' : line[cur] == '"' ? '"' : 0) : 0;
                if (const auto end = close ? line.find(close, cur + 1) : std::string_view::npos;
                    end != std::string_view::npos && end > cur + 1) {
                    std::string h(line.substr(cur, end - cur + 1));
                    if (std::ranges::find(out, h) == out.end()) out.push_back(std::move(h));
                }
            }
        }
        if (eol == source.size()) break;
        lineStart = eol + 1;
    }
    return out;
}

std::string rewrite_module_decls(
    std::string_view source,
    const std::map<std::string, std::string>& rename)
{
    if (rename.empty()) return std::string(source);

    // Matched in the code text, written from the source: the two have the same
    // offsets.
    const std::string codeText = mcpp::libs::code_text::read(source).text;
    const std::string_view code = codeText;

    std::string out;
    out.reserve(source.size());

    std::size_t i = 0;
    while (i < source.size()) {
        // We're at the start of a logical line. Capture leading whitespace.
        std::size_t lineStart = i;
        std::size_t afterWs   = skip_ws(code, i);

        // Try to recognize `(export )?(module|import) NAME[:PART][; or ws]`.
        // On any deviation, copy the line verbatim.
        std::size_t cur = afterWs;
        bool hasExport = false;
        if (auto p = consume_keyword(code, cur, "export"); p != std::string::npos) {
            hasExport = true;
            cur = skip_ws(code, p);
        }

        std::string_view kw;
        if (auto p = consume_keyword(code, cur, "module"); p != std::string::npos) {
            kw = "module"; cur = p;
        } else if (auto p = consume_keyword(code, cur, "import"); p != std::string::npos) {
            kw = "import"; cur = p;
        } else {
            // Not a module/import line — emit the rest of the physical line.
            std::size_t eol = source.find('\n', lineStart);
            if (eol == std::string_view::npos) eol = source.size();
            else                                ++eol;
            out.append(source.substr(lineStart, eol - lineStart));
            i = eol;
            continue;
        }

        std::size_t afterKw = skip_ws(code, cur);

        // Bare partition import / global module fragment: `import :P;` /
        // `module ;` — no name to rename, copy verbatim.
        if (afterKw < code.size() && (code[afterKw] == ':' || code[afterKw] == ';')) {
            std::size_t eol = source.find('\n', lineStart);
            if (eol == std::string_view::npos) eol = source.size();
            else                                ++eol;
            out.append(source.substr(lineStart, eol - lineStart));
            i = eol;
            continue;
        }

        auto [nameEnd, name] = read_name(code, afterKw);
        if (nameEnd == std::string::npos) {
            // Not a recognized declaration. Verbatim.
            std::size_t eol = source.find('\n', lineStart);
            if (eol == std::string_view::npos) eol = source.size();
            else                                ++eol;
            out.append(source.substr(lineStart, eol - lineStart));
            i = eol;
            continue;
        }

        // Look up the name in the rename table.
        auto it = rename.find(std::string(name));
        if (it == rename.end()) {
            // No rewrite needed for this declaration; copy line verbatim.
            std::size_t eol = source.find('\n', lineStart);
            if (eol == std::string_view::npos) eol = source.size();
            else                                ++eol;
            out.append(source.substr(lineStart, eol - lineStart));
            i = eol;
            continue;
        }

        // Emit the rewritten prefix:
        //   <leading-ws>(export )?(module|import) <renamed>
        // Keep whatever follows the name (`:P;` / `;` / extras) verbatim.
        out.append(source.substr(lineStart, afterWs - lineStart));    // ws
        if (hasExport) out.append("export ");
        out.append(kw); out.append(" ");
        out.append(it->second);

        // Append the trailing portion (from nameEnd) up to and including
        // the newline.
        std::size_t eol = source.find('\n', nameEnd);
        if (eol == std::string_view::npos) eol = source.size();
        else                                ++eol;
        out.append(source.substr(nameEnd, eol - nameEnd));
        i = eol;
    }

    return out;
}

} // namespace mcpp::pm
