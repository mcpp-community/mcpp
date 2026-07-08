// mcpp.manifest:xpkg — parse the `mcpp = {}` segment of xpkg .lua
// descriptors (tiny Lua-subset reader; we never execute Lua).

export module mcpp.manifest.xpkg;

import mcpp.manifest.types;
import std;
import mcpp.pm.dep_spec;
import mcpp.pm.dependency_selector;
import mcpp.platform;

export namespace mcpp::manifest {

// M6.x: `mcpp` field in xpkg.lua may be either:
//   - a string (path to mcpp.toml inside the extracted tarball, glob-able)
//   - a table (inline Form B descriptor)
// extract_mcpp_field discriminates and returns the right kind.
struct McppField {
    enum Kind { Absent, StringPath, TableBody } kind = Absent;
    std::string                 value;   // glob path (StringPath) or table body (TableBody)
};
McppField extract_mcpp_field(std::string_view luaContent);
// Extract the list of available versions for `platform` (e.g. "linux", "macosx",
// "windows") from an xpkg .lua's xpm.<platform> = { ["X.Y.Z"] = {...}, ... }.
// Returns an empty vector if the platform table is missing or has no entries.
std::vector<std::string>
list_xpkg_versions(std::string_view luaContent, std::string_view platform);
// Extract the `namespace` field from an xpkg .lua's `package = { ... }` block.
// Returns empty string if the field is absent (legacy descriptors).
std::string extract_xpkg_namespace(std::string_view luaContent);
// Extract the `name` field from an xpkg .lua's `package = { ... }` block.
// Returns empty string if the field is absent.
std::string extract_xpkg_name(std::string_view luaContent);
// Canonical package identity — the unified (ns, name) model (design doc §4.2).
//
// A package's identity is a 2-tuple: `ns` is a hierarchical namespace path
// (sub-namespaces, dotted: `compat`, `a.b.c`), `name` is a single atomic
// segment. Surface spellings (dotted name, embedded prefix, missing namespace)
// all normalize to this tuple. Normalization:
//   1. If `declaredNs` is empty, inherit `indexDefaultNs` (owning-index ns).
//   2. Fully-qualified name: if `declaredName` already starts with `ns.`, it is
//      the FQN; otherwise FQN = `ns.declaredName` (or just `declaredName` when
//      there is no namespace at all).
//   3. Split the FQN on its LAST dot: prefix → `ns`, final segment → `name`.
struct XpkgIdentity {
    std::string ns;
    std::string name;
    bool operator==(const XpkgIdentity&) const = default;
};
XpkgIdentity canonical_xpkg_identity(std::string_view declaredNs,
                                     std::string_view declaredName,
                                     std::string_view indexDefaultNs = {});
// Convenience: the canonical identity of an xpkg .lua, read from its declared
// `package.{namespace,name}`. Empty `name` field → empty identity.
XpkgIdentity canonical_xpkg_identity_from_lua(std::string_view luaContent,
                                              std::string_view indexDefaultNs = {});
// Identity gate: does this xpkg .lua actually DECLARE the package the caller
// asked for? Compares the descriptor's declared `package.{name,namespace}`
// against the requested (ns, shortName) coordinate. This is the invariant that
// makes filename-based lookup safe — a file found by a candidate filename is
// only accepted when it *is* the requested package, so a bare `zlib.lua` from a
// foreign index never satisfies a request for `compat.zlib`.
//
// A descriptor that declares no `name` is accepted (cannot verify → lenient).
// `allowLegacyBareDefault` governs only the default-namespace case: whether a
// no-namespace descriptor whose bare name matches counts as the default-ns
// package (preserves legacy bare-named mcpplibs descriptors).
//
// `indexDefaultNs` is the namespace OWNED BY the index the descriptor was found
// in. When the read is scoped to a single known index (e.g. a `[indices]` local
// path index owns namespace `local-dev`), a descriptor that declares no
// namespace inherits the index's — so `tinycfg.lua` (name only) in the
// `local-dev` index matches a request for `(local-dev, tinycfg)`. Empty for
// multi-index scans where the owning index isn't known per-file (the builtin
// global scan); see the design doc §4.1.
bool xpkg_lua_identity_matches(std::string_view luaContent,
                               std::string_view ns,
                               std::string_view shortName,
                               bool allowLegacyBareDefault = true,
                               std::string_view indexDefaultNs = {});
// Resolve the lib-root path for a manifest:
//   1. `[lib].path` if explicitly set (cargo-style override),
//   2. otherwise the convention `src/<package-tail>.cppm`, where
//      `<package-tail>` is the last `.`-segment of [package].name
//      (e.g. `mcpplibs.tinyhttps` → `src/tinyhttps.cppm`).
// The returned path is relative to the package root unless the user
// passed an absolute path in `[lib].path`.
// Synthesize a Manifest from an xpkg .lua file's `mcpp = {}` segment.
// Used when a fetched dep has no source/mcpp.toml — the index entry's
// `mcpp = {}` workaround block carries the missing build info.
//
// The resulting Manifest is in-memory only; sourcePath is set to the
// supplied package name + version so error messages can refer to it.
std::expected<Manifest, ManifestError>
synthesize_from_xpkg_lua(std::string_view luaContent,
                         std::string_view packageName,
                         std::string_view packageVersion);
} // namespace mcpp::manifest

namespace mcpp::manifest {


// =====================================================================
//  synthesize_from_xpkg_lua — parse mcpp = {} segment from an xpkg .lua
// =====================================================================
//
//  Scope: tiny Lua-subset reader specialised for our `mcpp = { ... }`
//  workaround block. We don't run real Lua; we just locate the mcpp
//  table and read a short list of typed fields out of it.

namespace {

struct LuaCursor {
    std::string_view text;
    std::size_t      pos = 0;

    bool eof() const { return pos >= text.size(); }
    char peek() const { return pos < text.size() ? text[pos] : '\0'; }

    void skip_ws_and_comments() {
        while (!eof()) {
            char c = peek();
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ',' || c == ';') {
                ++pos;
            } else if (c == '-' && pos + 1 < text.size() && text[pos + 1] == '-') {
                // Block comment --[[…]] / --[=[…]=] spans lines; otherwise
                // line comment to end of line.
                if (long_bracket_level_at(pos + 2) >= 0) {
                    pos += 2;
                    (void)read_long_bracket();
                } else {
                    while (!eof() && peek() != '\n') ++pos;
                }
            } else {
                break;
            }
        }
    }

    bool consume(char c) {
        skip_ws_and_comments();
        if (peek() == c) { ++pos; return true; }
        return false;
    }

    // Lua long-bracket support ([[…]], [=[…]=], …). Returns the level
    // (number of '=' between the brackets) if `at` starts a long-bracket
    // opening, -1 otherwise.
    int long_bracket_level_at(std::size_t at) const {
        if (at >= text.size() || text[at] != '[') return -1;
        std::size_t i = at + 1;
        int level = 0;
        while (i < text.size() && text[i] == '=') { ++level; ++i; }
        return (i < text.size() && text[i] == '[') ? level : -1;
    }

    // True when the next value token is a string in either spelling
    // (quoted or long-bracket). Skips leading ws/comments like read_string.
    bool at_string_start() {
        skip_ws_and_comments();
        return peek() == '"' || peek() == '\'' ||
               long_bracket_level_at(pos) >= 0;
    }

    // Read a long-bracket string; caller ensured `pos` is at an opening.
    // Content is verbatim (no escape processing); per Lua semantics a
    // newline immediately after the opening bracket is skipped.
    std::string read_long_bracket() {
        int level = long_bracket_level_at(pos);
        if (level < 0) return {};
        pos += 2 + static_cast<std::size_t>(level);   // consume [=*[
        if (pos < text.size() && (text[pos] == '\n' || text[pos] == '\r')) {
            char first = text[pos++];
            if (pos < text.size() && (text[pos] == '\n' || text[pos] == '\r') &&
                text[pos] != first)
                ++pos;                                 // \r\n / \n\r pair
        }
        std::string close = "]" + std::string(static_cast<std::size_t>(level), '=') + "]";
        auto end = text.find(close, pos);
        std::string out;
        if (end == std::string_view::npos) {           // unterminated
            out.assign(text.substr(pos));
            pos = text.size();
        } else {
            out.assign(text.substr(pos, end - pos));
            pos = end + close.size();
        }
        return out;
    }

    std::string read_string() {
        skip_ws_and_comments();
        if (long_bracket_level_at(pos) >= 0) return read_long_bracket();
        if (peek() != '"' && peek() != '\'') return {};
        char q = text[pos++];
        std::string out;
        while (!eof() && peek() != q) {
            if (peek() == '\\' && pos + 1 < text.size()) {
                ++pos;
                char e = text[pos++];
                switch (e) {
                    case 'n':  out.push_back('\n'); break;
                    case 't':  out.push_back('\t'); break;
                    case 'r':  out.push_back('\r'); break;
                    case '"':  out.push_back('"');  break;
                    case '\'': out.push_back('\''); break;
                    case '\\': out.push_back('\\'); break;
                    default:   out.push_back(e);
                }
            } else {
                out.push_back(text[pos++]);
            }
        }
        if (!eof()) ++pos;     // closing quote
        return out;
    }

    std::string read_ident() {
        skip_ws_and_comments();
        std::string out;
        while (!eof() && (std::isalnum(static_cast<unsigned char>(peek())) ||
                          peek() == '_'))
        {
            out.push_back(text[pos++]);
        }
        return out;
    }

    // Read either a bare ident or `["string"]`.
    std::string read_key() {
        skip_ws_and_comments();
        if (peek() == '[') {
            ++pos;
            auto s = read_string();
            skip_ws_and_comments();
            if (peek() == ']') ++pos;
            return s;
        }
        return read_ident();
    }

    // Read a Lua barewordy value: number/true/false/nil up to delimiter.
    std::string read_bareword() {
        skip_ws_and_comments();
        std::string out;
        while (!eof() && !std::isspace(static_cast<unsigned char>(peek())) &&
               peek() != ',' && peek() != '}' && peek() != ';')
        {
            out.push_back(text[pos++]);
        }
        return out;
    }

    // Skip an entire balanced { ... } block, string-aware.
    void skip_table() {
        if (!consume('{')) return;
        int depth = 1;
        while (!eof() && depth > 0) {
            char c = peek();
            if (c == '"' || c == '\'') {
                read_string();
                continue;
            } else if (c == '[' && long_bracket_level_at(pos) >= 0) {
                (void)read_long_bracket();
                continue;
            } else if (c == '-' && pos + 1 < text.size() && text[pos+1] == '-') {
                if (long_bracket_level_at(pos + 2) >= 0) {
                    pos += 2;
                    (void)read_long_bracket();
                } else {
                    while (!eof() && peek() != '\n') ++pos;
                }
                continue;
            } else if (c == '{') { ++depth; ++pos; }
            else if (c == '}') { --depth; ++pos; }
            else { ++pos; }
        }
    }

    // Read and consume a balanced { ... } block, returning the inner text.
    std::string read_table_body() {
        if (!consume('{')) return {};
        auto start = pos;
        int depth = 1;
        while (!eof() && depth > 0) {
            char c = peek();
            if (c == '"' || c == '\'') {
                read_string();
                continue;
            }
            if (c == '[' && long_bracket_level_at(pos) >= 0) {
                (void)read_long_bracket();
                continue;
            }
            if (c == '-' && pos + 1 < text.size() && text[pos + 1] == '-') {
                if (long_bracket_level_at(pos + 2) >= 0) {
                    pos += 2;
                    (void)read_long_bracket();
                } else {
                    while (!eof() && peek() != '\n') ++pos;
                }
                continue;
            }
            if (c == '{') {
                ++depth;
                ++pos;
                continue;
            }
            if (c == '}') {
                --depth;
                if (depth == 0) {
                    auto end = pos;
                    ++pos;
                    return std::string(text.substr(start, end - start));
                }
                ++pos;
                continue;
            }
            ++pos;
        }
        return {};
    }
};

std::string top_level_table_body_for_key(std::string_view body, std::string_view wantedKey) {
    LuaCursor cur { body };
    cur.skip_ws_and_comments();
    while (!cur.eof()) {
        auto key = cur.read_key();
        if (key.empty()) {
            cur.skip_ws_and_comments();
            if (cur.eof()) break;
            ++cur.pos;
            continue;
        }
        cur.skip_ws_and_comments();
        if (!cur.consume('=')) {
            cur.skip_ws_and_comments();
            continue;
        }
        cur.skip_ws_and_comments();
        if (key == wantedKey) {
            return cur.read_table_body();
        }
        if (cur.peek() == '{') cur.skip_table();
        else if (cur.at_string_start()) (void)cur.read_string();
        else (void)cur.read_bareword();
        cur.skip_ws_and_comments();
    }
    return {};
}

std::string top_level_string_value_for_key(std::string_view body, std::string_view wantedKey) {
    LuaCursor cur { body };
    cur.skip_ws_and_comments();
    while (!cur.eof()) {
        auto key = cur.read_key();
        if (key.empty()) {
            cur.skip_ws_and_comments();
            if (cur.eof()) break;
            ++cur.pos;
            continue;
        }
        cur.skip_ws_and_comments();
        if (!cur.consume('=')) {
            cur.skip_ws_and_comments();
            continue;
        }
        cur.skip_ws_and_comments();
        if (key == wantedKey && cur.at_string_start()) {
            return cur.read_string();
        }
        if (cur.peek() == '{') cur.skip_table();
        else if (cur.at_string_start()) (void)cur.read_string();
        else (void)cur.read_bareword();
        cur.skip_ws_and_comments();
    }
    return {};
}

// Strip Lua line comments (`-- ...\n`) and string contents from text,
// replacing them with spaces of the same length so positions are
// preserved. This is a simple-but-correct way to make the scanner
// in extract_mcpp_segment_body() ignore comments and strings without
// re-implementing a full Lua tokenizer.
std::string strip_lua_comments_and_strings(std::string_view text) {
    std::string out(text.size(), ' ');
    std::size_t i = 0;
    // Long-bracket opening at `at`? Returns level (count of '='), else -1.
    auto lb_level = [&](std::size_t at) -> int {
        if (at >= text.size() || text[at] != '[') return -1;
        std::size_t k = at + 1;
        int level = 0;
        while (k < text.size() && text[k] == '=') { ++level; ++k; }
        return (k < text.size() && text[k] == '[') ? level : -1;
    };
    // Blank [begin,end) preserving newlines for line-number fidelity.
    auto blank = [&](std::size_t begin, std::size_t end) {
        for (std::size_t d = begin; d < end && d < text.size(); ++d)
            out[d] = (text[d] == '\n') ? '\n' : ' ';
    };
    while (i < text.size()) {
        char c = text[i];
        // Comment: block (--[[…]] / --[=[…]=]) spans lines; else line comment.
        if (c == '-' && i + 1 < text.size() && text[i+1] == '-') {
            if (int lvl = lb_level(i + 2); lvl >= 0) {
                std::string close = "]" + std::string(static_cast<std::size_t>(lvl), '=') + "]";
                auto end = text.find(close, i + 2 + 2 + static_cast<std::size_t>(lvl));
                std::size_t stop = (end == std::string_view::npos)
                                 ? text.size() : end + close.size();
                blank(i, stop);
                i = stop;
                continue;
            }
            while (i < text.size() && text[i] != '\n') {
                out[i] = ' ';
                ++i;
            }
            continue;
        }
        // Long-bracket string: keep delimiters, blank content (it may
        // contain braces that would corrupt the structural depth count).
        if (int lvl = lb_level(i); lvl >= 0) {
            std::size_t openLen = 2 + static_cast<std::size_t>(lvl);
            for (std::size_t d = 0; d < openLen; ++d) out[i + d] = text[i + d];
            std::string close = "]" + std::string(static_cast<std::size_t>(lvl), '=') + "]";
            auto end = text.find(close, i + openLen);
            if (end == std::string_view::npos) {
                blank(i + openLen, text.size());
                i = text.size();
            } else {
                blank(i + openLen, end);
                for (std::size_t d = 0; d < close.size(); ++d) out[end + d] = text[end + d];
                i = end + close.size();
            }
            continue;
        }
        // String literal
        if (c == '"' || c == '\'') {
            char q = c;
            out[i] = c;        // keep opening quote so structure-aware search still sees it
            ++i;
            while (i < text.size() && text[i] != q) {
                if (text[i] == '\\' && i + 1 < text.size()) {
                    out[i]   = ' ';
                    out[i+1] = ' ';
                    i += 2;
                    continue;
                }
                out[i] = (text[i] == '\n') ? '\n' : ' ';
                ++i;
            }
            if (i < text.size()) {
                out[i] = q;     // closing quote
                ++i;
            }
            continue;
        }
        out[i] = c;
        ++i;
    }
    return out;
}

// Locate the body of `mcpp = { ... }` and return the inner content (no
// surrounding braces). Returns empty string if not found.
// M6.x: locate the `mcpp = ...` field at top level of an xpkg.lua and
// classify it as either a table body or a string path. Operates on a
// comment-/string-stripped copy so literal "mcpp = ..." inside Lua
// comments doesn't false-match.
McppField extract_mcpp_field_impl(std::string_view raw_text) {
    auto sanitized = strip_lua_comments_and_strings(raw_text);
    std::string_view text { sanitized };

    std::size_t p = 0;
    while ((p = text.find("mcpp", p)) != std::string_view::npos) {
        bool word_start = (p == 0 || (!std::isalnum(static_cast<unsigned char>(text[p-1]))
                                       && text[p-1] != '_'));
        if (!word_start) { ++p; continue; }
        std::size_t q = p + 4;
        if (q < text.size() && (std::isalnum(static_cast<unsigned char>(text[q])) ||
                                text[q] == '_')) { ++p; continue; }
        while (q < text.size() && (text[q] == ' ' || text[q] == '\t')) ++q;
        if (q >= text.size() || text[q] != '=') { ++p; continue; }
        ++q;
        while (q < text.size() && (text[q] == ' ' || text[q] == '\t' ||
                                    text[q] == '\n' || text[q] == '\r')) ++q;
        if (q >= text.size()) { ++p; continue; }

        // Discriminate: { → table body, " → string path
        if (text[q] == '{') {
            ++q;
            std::size_t body_start = q;
            int depth = 1;
            while (q < text.size() && depth > 0) {
                char c = text[q];
                if (c == '{') ++depth;
                else if (c == '}') {
                    --depth;
                    if (depth == 0) {
                        return McppField{
                            McppField::TableBody,
                            std::string(raw_text.substr(body_start, q - body_start))};
                    }
                }
                ++q;
            }
            return {};
        }
        if (text[q] == '"') {
            // string literal — but the sanitizer blanks string contents, so
            // re-locate the same `"..."` in raw_text and take its body.
            // Find the opening `"` at offset q in raw_text (offsets align
            // because sanitizer keeps positions).
            std::size_t s = q;
            if (s >= raw_text.size() || raw_text[s] != '"') { ++p; continue; }
            ++s;
            std::string val;
            while (s < raw_text.size() && raw_text[s] != '"') {
                if (raw_text[s] == '\\' && s + 1 < raw_text.size()) {
                    char nc = raw_text[s + 1];
                    switch (nc) {
                        case 'n': val.push_back('\n'); break;
                        case 't': val.push_back('\t'); break;
                        case '"': val.push_back('"');  break;
                        case '\\': val.push_back('\\'); break;
                        default: val.push_back(nc);
                    }
                    s += 2;
                } else {
                    val.push_back(raw_text[s++]);
                }
            }
            return McppField{ McppField::StringPath, std::move(val) };
        }
        ++p;
    }
    return {};
}

// Backward-compat: old API; prefer extract_mcpp_field for new callers.
std::string extract_mcpp_segment_body(std::string_view raw_text) {
    auto f = extract_mcpp_field_impl(raw_text);
    return f.kind == McppField::TableBody ? std::move(f.value) : std::string{};
}

} // namespace

McppField extract_mcpp_field(std::string_view luaContent) {
    return extract_mcpp_field_impl(luaContent);
}

std::string extract_xpkg_namespace(std::string_view luaContent) {
    auto packageBody = top_level_table_body_for_key(luaContent, "package");
    if (packageBody.empty()) return {};
    return top_level_string_value_for_key(packageBody, "namespace");
}

std::string extract_xpkg_name(std::string_view luaContent) {
    auto packageBody = top_level_table_body_for_key(luaContent, "package");
    if (packageBody.empty()) return {};
    return top_level_string_value_for_key(packageBody, "name");
}

XpkgIdentity canonical_xpkg_identity(std::string_view declaredNs,
                                     std::string_view declaredName,
                                     std::string_view indexDefaultNs) {
    // Step 1 — owning-index namespace: a descriptor that declares no namespace
    // inherits the namespace of the index it lives in.
    std::string ns(declaredNs.empty() ? indexDefaultNs : declaredNs);
    std::string name(declaredName);

    // Step 2 — fully-qualified name. A declared name already prefixed with the
    // namespace IS the FQN; otherwise the namespace is prepended. With no
    // namespace at all, the declared name stands alone.
    std::string fqn;
    if (ns.empty()) {
        fqn = name;
    } else {
        std::string prefix = ns + ".";
        fqn = name.starts_with(prefix) ? name : ns + "." + name;
    }

    // Step 3 — split the FQN on its LAST dot: prefix → ns, final segment → name.
    auto pos = fqn.rfind('.');
    if (pos == std::string::npos) return XpkgIdentity{ /*ns=*/{}, /*name=*/fqn };
    return XpkgIdentity{ fqn.substr(0, pos), fqn.substr(pos + 1) };
}

XpkgIdentity canonical_xpkg_identity_from_lua(std::string_view luaContent,
                                              std::string_view indexDefaultNs) {
    auto name = extract_xpkg_name(luaContent);
    if (name.empty()) return {};
    return canonical_xpkg_identity(extract_xpkg_namespace(luaContent), name,
                                   indexDefaultNs);
}

bool xpkg_lua_identity_matches(std::string_view luaContent,
                               std::string_view ns,
                               std::string_view shortName,
                               bool allowLegacyBareDefault,
                               std::string_view indexDefaultNs) {
    auto luaName = extract_xpkg_name(luaContent);
    if (luaName.empty()) return true;   // no declared name → cannot verify, accept

    // Reduce the descriptor to its canonical (ns, name) tuple (design doc §4.2),
    // then match per the unified model.
    auto id = canonical_xpkg_identity(extract_xpkg_namespace(luaContent),
                                      luaName, indexDefaultNs);

    // The single atomic name must equal the requested short name in every mode.
    if (id.name != shortName) return false;

    // Discovery (empty request ns, e.g. `mcpp new --template X`): the caller
    // derives the namespace from the descriptor, so a name match is enough.
    if (ns.empty()) return true;

    // Unqualified / default-namespace request: resolve the name against the
    // default namespace search path — the default namespace itself, then the
    // `compat` wrapper namespace (`kCompatNamespace`, shared with the candidate
    // generator). A legacy no-namespace descriptor is admitted under the flag.
    if (ns == kDefaultNamespace) {
        return id.ns == kDefaultNamespace
            || id.ns == kCompatNamespace
            || (allowLegacyBareDefault && id.ns.empty());
    }

    // Qualified request (concrete namespace: compat, xim, a custom/nested ns):
    // exact namespace equality. Cross-namespace collisions are structurally
    // impossible — a foreign `(xim, zlib)` never equals `(compat, zlib)`.
    return id.ns == ns;
}

std::vector<std::string>
list_xpkg_versions(std::string_view luaContent, std::string_view platform) {
    // Locate `xpm = { ... <platform> = { ["X.Y.Z"] = {...}, ... } ... }`.
    // We work on a sanitized copy so quoted version keys remain locatable
    // by their offsets in the original text.
    auto sanitized = strip_lua_comments_and_strings(luaContent);
    std::string_view text { sanitized };
    std::vector<std::string> versions;

    auto find_word_at_lhs = [&](std::string_view name, std::size_t from)
        -> std::size_t
    {
        std::size_t p = from;
        while ((p = text.find(name, p)) != std::string_view::npos) {
            bool word_start = (p == 0 ||
                (!std::isalnum(static_cast<unsigned char>(text[p-1])) && text[p-1] != '_'));
            std::size_t after = p + name.size();
            bool word_end = (after >= text.size() ||
                (!std::isalnum(static_cast<unsigned char>(text[after])) && text[after] != '_'));
            if (!word_start || !word_end) { ++p; continue; }
            std::size_t q = after;
            while (q < text.size() && (text[q] == ' ' || text[q] == '\t' ||
                                       text[q] == '\n' || text[q] == '\r')) ++q;
            if (q < text.size() && text[q] == '=') return p;
            ++p;
        }
        return std::string_view::npos;
    };

    auto skip_to_open_brace = [&](std::size_t from) -> std::size_t {
        std::size_t q = from;
        while (q < text.size() && text[q] != '{') ++q;
        return q < text.size() ? q : std::string_view::npos;
    };

    // Match braces to find table extent.
    auto find_table_end = [&](std::size_t open) -> std::size_t {
        int depth = 1;
        std::size_t q = open + 1;
        while (q < text.size() && depth > 0) {
            char c = text[q];
            if (c == '{') ++depth;
            else if (c == '}') {
                --depth;
                if (depth == 0) return q;
            }
            ++q;
        }
        return std::string_view::npos;
    };

    auto xpm_pos = find_word_at_lhs("xpm", 0);
    if (xpm_pos == std::string_view::npos) return versions;
    auto xpm_open = skip_to_open_brace(xpm_pos);
    if (xpm_open == std::string_view::npos) return versions;
    auto xpm_end = find_table_end(xpm_open);
    if (xpm_end == std::string_view::npos) return versions;

    auto plat_pos = find_word_at_lhs(platform, xpm_open + 1);
    if (plat_pos == std::string_view::npos || plat_pos >= xpm_end) return versions;
    auto plat_open = skip_to_open_brace(plat_pos);
    if (plat_open == std::string_view::npos || plat_open >= xpm_end) return versions;
    auto plat_end = find_table_end(plat_open);
    if (plat_end == std::string_view::npos) return versions;

    // Inside platform table: scan for ["X.Y.Z"] = { ... }
    std::size_t q = plat_open + 1;
    while (q < plat_end) {
        if (text[q] == '[') {
            std::size_t r = q + 1;
            while (r < plat_end && (text[r] == ' ' || text[r] == '\t')) ++r;
            if (r < plat_end && (text[r] == '"' || text[r] == '\'')) {
                const char quote = text[r];
                ++r;
                std::size_t key_start = r;
                while (r < plat_end && text[r] != quote && text[r] != '\n') ++r;
                if (r < plat_end && text[r] == quote) {
                    versions.emplace_back(luaContent.substr(key_start, r - key_start));
                }
            }
        }
        ++q;
    }
    return versions;
}

std::expected<Manifest, ManifestError>
synthesize_from_xpkg_lua(std::string_view luaContent,
                         std::string_view packageName,
                         std::string_view packageVersion)
{
    auto body = extract_mcpp_segment_body(luaContent);
    if (body.empty()) {
        return std::unexpected(ManifestError{
            std::format(
                "package '{}' has no `mcpp = {{}}` segment in its index entry "
                "and the source has no mcpp.toml — cannot derive a manifest.",
                packageName),
            std::format("xpkg-lua of {}@{}", packageName, packageVersion),
            0, 0});
    }
    if (auto platformBody = top_level_table_body_for_key(body, mcpp::platform::xpkg_platform);
        !platformBody.empty()) {
        body += "\n";
        body += platformBody;
    }

    Manifest m;
    m.sourcePath  = std::format("xpkg-lua://{}@{}", packageName, packageVersion);
    m.package.name    = std::string(packageName);
    m.package.version = std::string(packageVersion);
    m.package.standard = "c++23";
    m.language.standard   = "c++23";
    m.language.modules    = true;
    m.language.importStd  = true;

    LuaCursor cur { body };
    cur.skip_ws_and_comments();

    while (!cur.eof()) {
        cur.skip_ws_and_comments();
        if (cur.eof()) break;
        auto key = cur.read_key();
        if (key.empty()) {
            cur.skip_ws_and_comments();
            if (cur.eof()) break;
            ++cur.pos;            // unknown char — advance and retry
            continue;
        }
        cur.skip_ws_and_comments();
        if (!cur.consume('=')) {
            return std::unexpected(ManifestError{
                std::format("malformed mcpp segment near key '{}'", key),
                m.sourcePath, 0, 0});
        }
        cur.skip_ws_and_comments();

        if      (key == "language") {
            auto v = cur.read_string();
            if (!v.empty()) {
                m.language.standard = v;
                m.package.standard = v;
            }
        }
        else if (key == "import_std") {
            auto v = cur.read_bareword();
            m.language.importStd = (v == "true");
        }
        else if (key == "modules") {
            // `{ "a", "b", ... }`
            if (!cur.consume('{')) {
                return std::unexpected(ManifestError{
                    "expected '{' after `modules =`", m.sourcePath, 0, 0});
            }
            cur.skip_ws_and_comments();
            while (!cur.eof() && cur.peek() != '}') {
                auto s = cur.read_string();
                if (!s.empty()) m.modules.exports_.push_back(std::move(s));
                cur.skip_ws_and_comments();
            }
            cur.consume('}');
        }
        else if (key == "sources") {
            if (!cur.consume('{')) {
                return std::unexpected(ManifestError{
                    "expected '{' after `sources =`", m.sourcePath, 0, 0});
            }
            cur.skip_ws_and_comments();
            while (!cur.eof() && cur.peek() != '}') {
                auto s = cur.read_string();
                if (!s.empty()) {
                    m.modules.sources.push_back(s);
                    m.buildConfig.sources.push_back(std::move(s));   // M5.0 mirror
                }
                cur.skip_ws_and_comments();
            }
            cur.consume('}');
        }
        else if (key == "include_dirs") {
            // M5.0: shipped headers exposed to dependents AND used by this
            // package's own compile (mcpp's symmetric include_dirs semantics).
            if (!cur.consume('{')) {
                return std::unexpected(ManifestError{
                    "expected '{' after `include_dirs =`", m.sourcePath, 0, 0});
            }
            cur.skip_ws_and_comments();
            while (!cur.eof() && cur.peek() != '}') {
                auto s = cur.read_string();
                if (!s.empty()) m.buildConfig.includeDirs.emplace_back(s);
                cur.skip_ws_and_comments();
            }
            cur.consume('}');
        }
        else if (key == "provides") {
            // Package-level capabilities (Feature System v2 S3): this package
            // satisfies the listed abstract capability names for any dependent
            // that `requires` them. `{ "blas", "lapack", ... }`.
            if (!cur.consume('{')) {
                return std::unexpected(ManifestError{
                    "expected '{' after `provides =`", m.sourcePath, 0, 0});
            }
            cur.skip_ws_and_comments();
            while (!cur.eof() && cur.peek() != '}') {
                auto s = cur.read_string();
                if (!s.empty()) m.provides.push_back(std::move(s));
                cur.skip_ws_and_comments();
            }
            cur.consume('}');
        }
        else if (key == "generated_files") {
            // `{ ["relative/path"] = "contents", ... }`
            if (!cur.consume('{')) {
                return std::unexpected(ManifestError{
                    "expected '{' after `generated_files =`", m.sourcePath, 0, 0});
            }
            cur.skip_ws_and_comments();
            while (!cur.eof() && cur.peek() != '}') {
                auto path = cur.read_key();
                if (path.empty()) break;
                cur.skip_ws_and_comments();
                if (!cur.consume('=')) {
                    return std::unexpected(ManifestError{
                        "expected '=' in `generated_files` entry", m.sourcePath, 0, 0});
                }
                auto content = cur.read_string();
                m.buildConfig.generatedFiles.emplace(path, std::move(content));
                cur.skip_ws_and_comments();
            }
            cur.consume('}');
        }
        else if (key == "targets") {
            // `{ ["name"] = { kind = "lib" }, ... }`
            if (!cur.consume('{')) {
                return std::unexpected(ManifestError{
                    "expected '{' after `targets =`", m.sourcePath, 0, 0});
            }
            cur.skip_ws_and_comments();
            while (!cur.eof() && cur.peek() != '}') {
                auto tname = cur.read_key();
                if (tname.empty()) { cur.skip_ws_and_comments(); break; }
                cur.skip_ws_and_comments();
                if (!cur.consume('=')) break;
                cur.skip_ws_and_comments();
                if (!cur.consume('{')) break;

                Target t;
                t.name = tname;
                t.kind = Target::Library;     // default
                cur.skip_ws_and_comments();
                while (!cur.eof() && cur.peek() != '}') {
                    auto sub = cur.read_key();
                    cur.skip_ws_and_comments();
                    if (!cur.consume('=')) break;
                    cur.skip_ws_and_comments();
                    if (sub == "kind") {
                        auto k = cur.read_string();
                        if      (k == "lib" || k == "library")     t.kind = Target::Library;
                        else if (k == "bin" || k == "binary")      t.kind = Target::Binary;
                        else if (k == "shared" || k == "dylib"
                              || k == "so" || k == "shlib")        t.kind = Target::SharedLibrary;
                    } else if (sub == "main") {
                        t.main = cur.read_string();
                    } else if (sub == "soname") {
                        t.soname = cur.read_string();
                    } else {
                        // unknown subfield — skip its value
                        cur.skip_ws_and_comments();
                        if (cur.peek() == '{') cur.skip_table();
                        else (void)cur.read_bareword();
                    }
                    cur.skip_ws_and_comments();
                }
                cur.consume('}');
                if (auto msg = validate_target_soname(t, std::format("targets.{}.", tname))) {
                    return std::unexpected(ManifestError{*msg, m.sourcePath, 0, 0});
                }
                m.targets.push_back(std::move(t));
                cur.skip_ws_and_comments();
            }
            cur.consume('}');
        }
        else if (key == "features") {
            // `{ ["main"] = { sources = { "*/gtest_main.cc" } }, ... }`
            // Registers the feature (so it's a known feature) and, when it
            // carries `sources`, records them as feature-gated source globs
            // (excluded by default; included only when the feature is active —
            // resolved in prepare_build). A feature with no `sources` is still
            // registered (empty implied set) so it can be requested/validated.
            if (!cur.consume('{')) {
                return std::unexpected(ManifestError{
                    "expected '{' after `features =`", m.sourcePath, 0, 0});
            }
            cur.skip_ws_and_comments();
            while (!cur.eof() && cur.peek() != '}') {
                auto fname = cur.read_key();
                if (fname.empty()) { cur.skip_ws_and_comments(); break; }
                cur.skip_ws_and_comments();
                if (!cur.consume('=')) break;
                cur.skip_ws_and_comments();
                if (!cur.consume('{')) break;
                // register the feature (no implied features for now)
                m.featuresMap.try_emplace(fname, std::vector<std::string>{});
                cur.skip_ws_and_comments();
                while (!cur.eof() && cur.peek() != '}') {
                    auto sub = cur.read_key();
                    cur.skip_ws_and_comments();
                    if (!cur.consume('=')) break;
                    cur.skip_ws_and_comments();
                    if (sub == "deps" && cur.peek() == '{') {
                        // Feature-activated optional deps (Stage 2a):
                        //   deps = { ["compat.openblas"] = "0.3.x", ... }
                        // Same flat/dotted form as the top-level `deps` table.
                        cur.consume('{');
                        cur.skip_ws_and_comments();
                        while (!cur.eof() && cur.peek() != '}') {
                            auto dname = cur.read_key();
                            if (dname.empty()) break;
                            cur.skip_ws_and_comments();
                            if (!cur.consume('=')) break;
                            cur.skip_ws_and_comments();
                            auto dver = cur.read_string();
                            DependencySpec spec;
                            spec.version = dver;
                            auto selector = mcpp::pm::resolve_dependency_selector(
                                dname,
                                mcpp::pm::DependencySelectorMode::OmittedMcpplibsPriority);
                            if (!selector.candidates.empty()) {
                                spec.namespace_ = selector.candidates.front().namespace_;
                                spec.shortName  = selector.candidates.front().shortName;
                                spec.candidates = std::move(selector.candidates);
                                m.featureDeps[fname][selector.stableMapKey] = std::move(spec);
                            }
                            cur.skip_ws_and_comments();
                        }
                        cur.consume('}');
                    } else {
                    // Feature subfields that carry a string array. `sources`
                    // gates source globs; `defines` carries package-owned macros
                    // (Stage 1); `requires`/`provides` declare capabilities
                    // (Stage 3). All share the `{ "...", ... }` shape.
                    std::vector<std::string>* arr =
                        sub == "implies"  ? &m.featuresMap[fname]
                      : sub == "sources"  ? &m.buildConfig.featureSources[fname]
                      : sub == "defines"  ? &m.buildConfig.featureDefines[fname]
                      : sub == "requires" ? &m.featureRequires[fname]
                      : sub == "provides" ? &m.featureProvides[fname]
                      : nullptr;
                    if (arr && cur.peek() == '{') {
                        cur.consume('{');
                        cur.skip_ws_and_comments();
                        while (!cur.eof() && cur.peek() != '}') {
                            auto s = cur.read_string();
                            if (!s.empty()) arr->push_back(std::move(s));
                            cur.skip_ws_and_comments();
                        }
                        cur.consume('}');
                    } else {
                        // unknown subfield — skip its value
                        if (cur.peek() == '{') cur.skip_table();
                        else (void)cur.read_bareword();
                    }
                    }
                    cur.skip_ws_and_comments();
                }
                cur.consume('}');
                cur.skip_ws_and_comments();
            }
            cur.consume('}');
        }
        else if (key == "deps") {
            // `{ ["name"] = "version", ["ns.name"] = "version", ... }`
            // The mcpp segment uses the flat / dotted form only — namespaced
            // subtables would require a richer Lua parser than we have here,
            // and the same expressivity is reachable by writing
            //     ["mcpplibs.cmdline"] = "0.0.2"
            // which the consumer side accepts identically.
            if (!cur.consume('{')) {
                return std::unexpected(ManifestError{
                    "expected '{' after `deps =`", m.sourcePath, 0, 0});
            }
            cur.skip_ws_and_comments();
            while (!cur.eof() && cur.peek() != '}') {
                auto dname = cur.read_key();
                if (dname.empty()) break;
                cur.skip_ws_and_comments();
                if (!cur.consume('=')) break;
                cur.skip_ws_and_comments();
                auto dver = cur.read_string();
                if (!dname.empty()) {
                    DependencySpec spec;
                    spec.version = dver;
                    auto selector = mcpp::pm::resolve_dependency_selector(
                        dname,
                        mcpp::pm::DependencySelectorMode::OmittedMcpplibsPriority);
                    if (!selector.candidates.empty()) {
                        spec.namespace_ = selector.candidates.front().namespace_;
                        spec.shortName = selector.candidates.front().shortName;
                        spec.candidates = std::move(selector.candidates);
                        m.dependencies[selector.stableMapKey] = std::move(spec);
                    }
                }
                cur.skip_ws_and_comments();
            }
            cur.consume('}');
        }
        else if (key == "cflags" || key == "cxxflags" || key == "ldflags") {
            // `{ "-Dfoo", "-Wall", ... }` — appended to the per-rule baseline
            // by ninja_backend. cflags goes to the C rule (.c files), cxxflags
            // to C++ rule (.cpp/.cc/.cxx/.cppm), ldflags to link commands.
            if (!cur.consume('{')) {
                return std::unexpected(ManifestError{
                    std::format("expected '{{' after `{} =`", key),
                    m.sourcePath, 0, 0});
            }
            cur.skip_ws_and_comments();
            auto& target = (key == "cflags")
                ? m.buildConfig.cflags
                : (key == "cxxflags" ? m.buildConfig.cxxflags : m.buildConfig.ldflags);
            while (!cur.eof() && cur.peek() != '}') {
                auto s = cur.read_string();
                if (key == "cxxflags" && starts_with_std_flag(s)) {
                    return std::unexpected(ManifestError{
                        std::format("cxxflags contains '{}'; use language/package standard to configure the C++ language standard",
                                    s),
                        m.sourcePath, 0, 0});
                }
                if (!s.empty()) target.push_back(std::move(s));
                cur.skip_ws_and_comments();
            }
            cur.consume('}');
        }
        else if (key == "c_standard") {
            auto v = cur.read_string();
            if (!v.empty()) m.buildConfig.cStandard = v;
        }
        else if (key == "runtime") {
            auto runtimeBody = cur.read_table_body();
            LuaCursor rc { runtimeBody };
            rc.skip_ws_and_comments();
            while (!rc.eof()) {
                auto sub = rc.read_key();
                if (sub.empty()) {
                    rc.skip_ws_and_comments();
                    if (rc.eof()) break;
                    ++rc.pos;
                    continue;
                }
                rc.skip_ws_and_comments();
                if (!rc.consume('=')) {
                    return std::unexpected(ManifestError{
                        std::format("malformed runtime segment near key '{}'", sub),
                        m.sourcePath, 0, 0});
                }
                rc.skip_ws_and_comments();
                auto read_string_list = [&](std::vector<std::string>& out)
                    -> std::expected<void, ManifestError>
                {
                    if (!rc.consume('{')) {
                        return std::unexpected(ManifestError{
                            std::format("expected '{{' after `runtime.{} =`", sub),
                            m.sourcePath, 0, 0});
                    }
                    rc.skip_ws_and_comments();
                    while (!rc.eof() && rc.peek() != '}') {
                        auto s = rc.read_string();
                        if (!s.empty()) out.push_back(std::move(s));
                        rc.skip_ws_and_comments();
                    }
                    rc.consume('}');
                    return {};
                };
                if (sub == "library_dirs") {
                    std::vector<std::string> dirs;
                    if (auto r = read_string_list(dirs); !r) return std::unexpected(r.error());
                    for (auto& d : dirs) m.runtimeConfig.libraryDirs.emplace_back(std::move(d));
                } else if (sub == "dlopen_libs") {
                    if (auto r = read_string_list(m.runtimeConfig.dlopenLibs); !r)
                        return std::unexpected(r.error());
                } else if (sub == "capabilities") {
                    if (auto r = read_string_list(m.runtimeConfig.capabilities); !r)
                        return std::unexpected(r.error());
                } else if (sub == "provides") {
                    if (auto r = read_string_list(m.runtimeConfig.provides); !r)
                        return std::unexpected(r.error());
                } else {
                    rc.skip_ws_and_comments();
                    if      (rc.at_string_start()) (void)rc.read_string();
                    else if (rc.peek() == '{') rc.skip_table();
                    else                        (void)rc.read_bareword();
                }
                rc.skip_ws_and_comments();
            }
        }
        else if (key == "scan_overrides") {
            // `{ ["glob"] = { provides = { "m" }, imports = { "std" } }, ... }`
            // Author-asserted scan results (manifest:types ScanOverride):
            // matched files bypass the M1 text scan; verified against the
            // compiler's P1689 output at build time.
            if (!cur.consume('{')) {
                return std::unexpected(ManifestError{
                    "expected '{' after `scan_overrides =`", m.sourcePath, 0, 0});
            }
            cur.skip_ws_and_comments();
            while (!cur.eof() && cur.peek() != '}') {
                auto glob = cur.read_key();
                if (glob.empty()) break;
                cur.skip_ws_and_comments();
                if (!cur.consume('=') || !cur.consume('{')) {
                    return std::unexpected(ManifestError{
                        std::format("malformed scan_overrides entry '{}'", glob),
                        m.sourcePath, 0, 0});
                }
                ScanOverride ov;
                cur.skip_ws_and_comments();
                while (!cur.eof() && cur.peek() != '}') {
                    auto sub = cur.read_key();
                    cur.skip_ws_and_comments();
                    if (sub.empty() || !cur.consume('=')) break;
                    cur.skip_ws_and_comments();
                    std::vector<std::string>* arr =
                        sub == "provides" ? &ov.provides
                      : sub == "imports"  ? &ov.imports
                      : nullptr;
                    if (arr && cur.peek() == '{') {
                        cur.consume('{');
                        cur.skip_ws_and_comments();
                        while (!cur.eof() && cur.peek() != '}') {
                            auto s = cur.read_string();
                            if (!s.empty()) arr->push_back(std::move(s));
                            cur.skip_ws_and_comments();
                        }
                        cur.consume('}');
                    } else {
                        return std::unexpected(ManifestError{
                            std::format("scan_overrides '{}': unknown subfield '{}' "
                                        "(expected provides / imports)", glob, sub),
                            m.sourcePath, 0, 0});
                    }
                    cur.skip_ws_and_comments();
                }
                cur.consume('}');
                if (ov.provides.empty() && ov.imports.empty()) {
                    return std::unexpected(ManifestError{
                        std::format("scan_overrides '{}' declares neither provides "
                                    "nor imports", glob),
                        m.sourcePath, 0, 0});
                }
                m.modules.scanOverrides.emplace(std::move(glob), std::move(ov));
                cur.skip_ws_and_comments();
            }
            cur.consume('}');
        }
        else if (key == "linux" || key == "macosx" || key == "windows") {
            // Per-platform sub-table. The CURRENT platform's body was already
            // appended to the segment before this loop (see above), so every
            // spelling here is a known key: skip the table itself.
            if (cur.peek() == '{') cur.skip_table();
        }
        else if (key == "schema") {
            // Descriptor schema tag (e.g. "0.1") — accepted, currently
            // informational only.
            (void)cur.read_string();
        }
        else {
            // Unknown key — skip the value (string / bareword / table), but
            // record the key so `mcpp xpkg parse` can warn (schema evolution
            // must be loud, not silently ignored).
            m.xpkgUnknownKeys.push_back(key);
            cur.skip_ws_and_comments();
            if      (cur.at_string_start()) (void)cur.read_string();
            else if (cur.peek() == '{') cur.skip_table();
            else                        (void)cur.read_bareword();
        }
    }

    // Validate minimum
    if (m.modules.sources.empty()) {
        return std::unexpected(ManifestError{
            "synthesised manifest missing sources (mcpp segment must declare `sources = { ... }`)",
            m.sourcePath, 0, 0});
    }
    if (m.targets.empty()) {
        // Default to a library target with the same name as the package.
        Target t;
        t.name = m.package.name;
        // For dotted names like mcpplibs.cmdline, take the last segment.
        auto dot = t.name.find_last_of('.');
        if (dot != std::string::npos) t.name = t.name.substr(dot + 1);
        t.kind = Target::Library;
        m.targets.push_back(std::move(t));
    }

    auto stdCfg = normalize_cpp_standard(m.package.standard);
    if (!stdCfg) {
        return std::unexpected(ManifestError{stdCfg.error(), m.sourcePath, 0, 0});
    }
    m.cppStandard = *stdCfg;
    m.package.standard = m.cppStandard.canonical;
    m.language.standard = m.cppStandard.canonical;

    return m;
}

} // namespace mcpp::manifest
