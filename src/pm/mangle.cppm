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
// Single-line comments and string literals are not parsed: the
// matcher requires the keyword (`module` / `import`) to be at the
// start of a logical line (whitespace-only prefix). That covers
// every real-world declaration site without needing a full lexer.
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

std::string rewrite_module_decls(
    std::string_view source,
    const std::map<std::string, std::string>& rename)
{
    if (rename.empty()) return std::string(source);

    std::string out;
    out.reserve(source.size());

    std::size_t i = 0;
    while (i < source.size()) {
        // We're at the start of a logical line. Capture leading whitespace.
        std::size_t lineStart = i;
        std::size_t afterWs   = skip_ws(source, i);

        // Try to recognize `(export )?(module|import) NAME[:PART][; or ws]`.
        // On any deviation, copy the line verbatim.
        std::size_t cur = afterWs;
        bool hasExport = false;
        if (auto p = consume_keyword(source, cur, "export"); p != std::string::npos) {
            hasExport = true;
            cur = skip_ws(source, p);
        }

        std::string_view kw;
        if (auto p = consume_keyword(source, cur, "module"); p != std::string::npos) {
            kw = "module"; cur = p;
        } else if (auto p = consume_keyword(source, cur, "import"); p != std::string::npos) {
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

        std::size_t afterKw = skip_ws(source, cur);

        // Bare partition import / global module fragment: `import :P;` /
        // `module ;` — no name to rename, copy verbatim.
        if (afterKw < source.size() && (source[afterKw] == ':' || source[afterKw] == ';')) {
            std::size_t eol = source.find('\n', lineStart);
            if (eol == std::string_view::npos) eol = source.size();
            else                                ++eol;
            out.append(source.substr(lineStart, eol - lineStart));
            i = eol;
            continue;
        }

        auto [nameEnd, name] = read_name(source, afterKw);
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
