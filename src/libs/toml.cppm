// mcpp.libs.toml — Minimal TOML parser tailored for mcpp.toml / mcpp.lock
// Only the subset required by our schema:
//  - Tables: [a.b.c]
//  - Inline strings: key = "..."  / key = '...'
//  - Booleans: key = true / false
//  - Integers
//  - Arrays of strings: key = ["a", "b"]
//  - Inline tables (rudimentary): key = { version = "..." }
// Comments: # to end of line. No multiline strings, no datetime.

export module mcpp.libs.toml;

import std;

export namespace mcpp::libs::toml {

struct Position {
    std::size_t line   = 1;
    std::size_t column = 1;
};

struct ParseError {
    std::string message;
    Position    where;
};

class Value;

using Table = std::map<std::string, Value, std::less<>>;
using Array = std::vector<Value>;

class Value {
public:
    enum class Kind { Null, String, Int, Bool, Array, Table };

    Value() = default;
    Value(std::string s) : kind_(Kind::String), str_(std::move(s)) {}
    Value(std::int64_t i) : kind_(Kind::Int), int_(i) {}
    Value(bool b) : kind_(Kind::Bool), bool_(b) {}
    Value(Array a) : kind_(Kind::Array), arr_(std::make_shared<Array>(std::move(a))) {}
    Value(Table t) : kind_(Kind::Table), tab_(std::make_shared<Table>(std::move(t))) {}

    Kind kind() const { return kind_; }
    bool is_string() const { return kind_ == Kind::String; }
    bool is_int()    const { return kind_ == Kind::Int; }
    bool is_bool()   const { return kind_ == Kind::Bool; }
    bool is_array()  const { return kind_ == Kind::Array; }
    bool is_table()  const { return kind_ == Kind::Table; }
    bool is_null()   const { return kind_ == Kind::Null; }

    const std::string&  as_string() const { return str_; }
    std::int64_t        as_int()    const { return int_; }
    bool                as_bool()   const { return bool_; }
    const Array&        as_array()  const { return *arr_; }
    Array&              as_array()        { return *arr_; }
    const Table&        as_table()  const { return *tab_; }
    Table&              as_table()        { return *tab_; }

    Position position {};

private:
    Kind                            kind_ = Kind::Null;
    std::string                     str_;
    std::int64_t                    int_  = 0;
    bool                            bool_ = false;
    std::shared_ptr<Array>          arr_;
    std::shared_ptr<Table>          tab_;
};

class Document {
public:
    explicit Document(Table root) : root_(std::move(root)) {}

    const Table& root() const { return root_; }
    Table&       root()       { return root_; }

    // Helpers to navigate by dotted path: e.g. "package.name"
    const Value* get(std::string_view dotted_path) const;

    bool                                  contains(std::string_view dotted_path) const { return get(dotted_path) != nullptr; }
    std::optional<std::string>            get_string(std::string_view path) const;
    std::optional<std::int64_t>           get_int(std::string_view path) const;
    std::optional<bool>                   get_bool(std::string_view path) const;
    std::optional<std::vector<std::string>> get_string_array(std::string_view path) const;
    const Table*                          get_table(std::string_view path) const;

private:
    Table root_;
};

std::expected<Document, ParseError> parse(std::string_view src);
std::expected<Document, ParseError> parse_file(const std::filesystem::path& p);

// Serialization helpers (for emitting mcpp.lock and xpkg generation)
std::string escape_string(std::string_view raw);

} // namespace mcpp::libs::toml

// =====================================================================
// Implementation
// =====================================================================

namespace mcpp::libs::toml {

namespace detail {

struct Lexer {
    std::string_view src;
    std::size_t      pos    = 0;
    std::size_t      line   = 1;
    std::size_t      column = 1;

    bool eof() const { return pos >= src.size(); }
    char peek(std::size_t o = 0) const { return pos + o < src.size() ? src[pos + o] : '\0'; }
    Position position() const { return {line, column}; }

    void advance() {
        if (pos >= src.size()) return;
        if (src[pos] == '\n') { ++line; column = 1; } else { ++column; }
        ++pos;
    }

    void skip_whitespace_and_comments() {
        while (!eof()) {
            char c = peek();
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                advance();
            } else if (c == '#') {
                while (!eof() && peek() != '\n') advance();
            } else {
                break;
            }
        }
    }

    void skip_inline_whitespace() {
        while (!eof()) {
            char c = peek();
            if (c == ' ' || c == '\t') advance();
            else break;
        }
    }
};

inline bool is_bare_key_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
        || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

inline std::expected<std::string, ParseError> read_string(Lexer& L) {
    char quote = L.peek();
    if (quote != '"' && quote != '\'') {
        return std::unexpected(ParseError{"expected string literal", L.position()});
    }
    L.advance();
    std::string out;
    while (!L.eof() && L.peek() != quote) {
        char c = L.peek();
        if (c == '\\' && quote == '"') {
            L.advance();
            char esc = L.peek();
            switch (esc) {
                case 'n': out.push_back('\n'); break;
                case 't': out.push_back('\t'); break;
                case 'r': out.push_back('\r'); break;
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '0': out.push_back('\0'); break;
                default:
                    return std::unexpected(ParseError{
                        std::format("invalid escape '\\{}'", esc), L.position()});
            }
            L.advance();
        } else if (c == '\n') {
            return std::unexpected(ParseError{"unterminated string", L.position()});
        } else {
            out.push_back(c);
            L.advance();
        }
    }
    if (L.eof()) return std::unexpected(ParseError{"unterminated string", L.position()});
    L.advance(); // closing quote
    return out;
}

inline std::expected<std::string, ParseError> read_key(Lexer& L) {
    L.skip_inline_whitespace();
    if (L.peek() == '"' || L.peek() == '\'') return read_string(L);
    std::string out;
    while (!L.eof() && is_bare_key_char(L.peek())) {
        out.push_back(L.peek());
        L.advance();
    }
    if (out.empty()) {
        return std::unexpected(ParseError{"expected key", L.position()});
    }
    return out;
}

inline std::expected<std::vector<std::string>, ParseError> read_dotted_key(Lexer& L) {
    std::vector<std::string> parts;
    while (true) {
        auto p = read_key(L);
        if (!p) return std::unexpected(p.error());
        parts.push_back(std::move(*p));
        L.skip_inline_whitespace();
        if (L.peek() == '.') { L.advance(); L.skip_inline_whitespace(); }
        else break;
    }
    return parts;
}

std::expected<Value, ParseError> read_value(Lexer& L);

inline std::expected<Value, ParseError> read_array(Lexer& L) {
    if (L.peek() != '[') return std::unexpected(ParseError{"expected '['", L.position()});
    L.advance();
    Array out;
    L.skip_whitespace_and_comments();
    if (L.peek() == ']') { L.advance(); return Value{out}; }
    while (true) {
        L.skip_whitespace_and_comments();
        auto v = read_value(L);
        if (!v) return std::unexpected(v.error());
        out.push_back(std::move(*v));
        L.skip_whitespace_and_comments();
        if (L.peek() == ',') { L.advance(); continue; }
        if (L.peek() == ']') { L.advance(); break; }
        return std::unexpected(ParseError{"expected ',' or ']' in array", L.position()});
    }
    return Value{std::move(out)};
}

inline std::expected<Value, ParseError> read_inline_table(Lexer& L) {
    if (L.peek() != '{') return std::unexpected(ParseError{"expected '{'", L.position()});
    L.advance();
    Table tab;
    L.skip_inline_whitespace();
    if (L.peek() == '}') { L.advance(); return Value{tab}; }
    while (true) {
        L.skip_inline_whitespace();
        auto k = read_key(L);
        if (!k) return std::unexpected(k.error());
        L.skip_inline_whitespace();
        if (L.peek() != '=') {
            return std::unexpected(ParseError{"expected '=' in inline table", L.position()});
        }
        L.advance();
        L.skip_inline_whitespace();
        auto v = read_value(L);
        if (!v) return std::unexpected(v.error());
        tab[*k] = std::move(*v);
        L.skip_inline_whitespace();
        if (L.peek() == ',') { L.advance(); continue; }
        if (L.peek() == '}') { L.advance(); break; }
        return std::unexpected(ParseError{"expected ',' or '}' in inline table", L.position()});
    }
    return Value{std::move(tab)};
}

inline std::expected<Value, ParseError> read_value(Lexer& L) {
    L.skip_inline_whitespace();
    Position pos = L.position();
    char c = L.peek();
    Value v;
    if (c == '"' || c == '\'') {
        auto s = read_string(L);
        if (!s) return std::unexpected(s.error());
        v = Value{std::move(*s)};
    } else if (c == '[') {
        auto r = read_array(L);
        if (!r) return std::unexpected(r.error());
        v = std::move(*r);
    } else if (c == '{') {
        auto r = read_inline_table(L);
        if (!r) return std::unexpected(r.error());
        v = std::move(*r);
    } else if (c == 't' || c == 'f') {
        std::string buf;
        while (!L.eof() && (std::isalpha(static_cast<unsigned char>(L.peek())))) {
            buf.push_back(L.peek()); L.advance();
        }
        if (buf == "true")       v = Value{true};
        else if (buf == "false") v = Value{false};
        else return std::unexpected(ParseError{
            std::format("invalid bare value '{}'", buf), pos});
    } else if (c == '-' || (c >= '0' && c <= '9')) {
        std::string buf;
        if (c == '-') { buf.push_back(c); L.advance(); }
        while (!L.eof() && (std::isdigit(static_cast<unsigned char>(L.peek())) || L.peek() == '_')) {
            if (L.peek() != '_') buf.push_back(L.peek());
            L.advance();
        }
        try {
            v = Value{static_cast<std::int64_t>(std::stoll(buf))};
        } catch (...) {
            return std::unexpected(ParseError{
                std::format("invalid integer '{}'", buf), pos});
        }
    } else {
        return std::unexpected(ParseError{
            std::format("unexpected character '{}'", c), pos});
    }
    v.position = pos;
    return v;
}

// Navigate a table via dotted path; create missing nodes.
inline Table* dive(Table& root, const std::vector<std::string>& path, bool create_intermediate = true) {
    Table* cur = &root;
    for (auto const& seg : path) {
        auto it = cur->find(seg);
        if (it == cur->end()) {
            if (!create_intermediate) return nullptr;
            (*cur)[seg] = Value{Table{}};
            cur = &(*cur)[seg].as_table();
        } else if (!it->second.is_table()) {
            return nullptr; // collision: existing non-table
        } else {
            cur = &it->second.as_table();
        }
    }
    return cur;
}

} // namespace detail

const Value* Document::get(std::string_view dotted_path) const {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= dotted_path.size(); ++i) {
        if (i == dotted_path.size() || dotted_path[i] == '.') {
            parts.emplace_back(dotted_path.substr(start, i - start));
            start = i + 1;
        }
    }
    if (parts.empty()) return nullptr;
    const Table* cur = &root_;
    for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
        auto it = cur->find(parts[i]);
        if (it == cur->end() || !it->second.is_table()) return nullptr;
        cur = &it->second.as_table();
    }
    auto it = cur->find(parts.back());
    if (it == cur->end()) return nullptr;
    return &it->second;
}

std::optional<std::string> Document::get_string(std::string_view path) const {
    auto* v = get(path);
    if (!v || !v->is_string()) return std::nullopt;
    return v->as_string();
}

std::optional<std::int64_t> Document::get_int(std::string_view path) const {
    auto* v = get(path);
    if (!v || !v->is_int()) return std::nullopt;
    return v->as_int();
}

std::optional<bool> Document::get_bool(std::string_view path) const {
    auto* v = get(path);
    if (!v || !v->is_bool()) return std::nullopt;
    return v->as_bool();
}

std::optional<std::vector<std::string>> Document::get_string_array(std::string_view path) const {
    auto* v = get(path);
    if (!v || !v->is_array()) return std::nullopt;
    std::vector<std::string> out;
    for (auto& e : v->as_array()) {
        if (!e.is_string()) return std::nullopt;
        out.push_back(e.as_string());
    }
    return out;
}

const Table* Document::get_table(std::string_view path) const {
    auto* v = get(path);
    if (!v || !v->is_table()) return nullptr;
    return &v->as_table();
}

std::expected<Document, ParseError> parse(std::string_view src) {
    using namespace detail;
    Lexer L { src };
    Table root;
    Table* current_table = &root;

    while (true) {
        L.skip_whitespace_and_comments();
        if (L.eof()) break;

        if (L.peek() == '[') {
            L.advance();
            // Note: we don't currently distinguish [[array of tables]]; mcpp doesn't use them.
            if (L.peek() == '[') {
                return std::unexpected(ParseError{"array-of-tables not supported", L.position()});
            }
            auto path = read_dotted_key(L);
            if (!path) return std::unexpected(path.error());
            L.skip_inline_whitespace();
            if (L.peek() != ']') {
                return std::unexpected(ParseError{"expected ']'", L.position()});
            }
            L.advance();
            auto* t = dive(root, *path);
            if (!t) return std::unexpected(ParseError{
                std::format("table path '{}' conflicts with non-table value",
                            std::accumulate(path->begin(), path->end(), std::string{},
                                [](std::string a, const std::string& b){
                                    return a.empty() ? b : a + "." + b;
                                })),
                L.position()});
            current_table = t;
        } else {
            // key = value
            auto path = read_dotted_key(L);
            if (!path) return std::unexpected(path.error());
            L.skip_inline_whitespace();
            if (L.peek() != '=') {
                return std::unexpected(ParseError{"expected '='", L.position()});
            }
            L.advance();
            auto v = read_value(L);
            if (!v) return std::unexpected(v.error());

            Table* target = current_table;
            if (path->size() > 1) {
                std::vector<std::string> parents(path->begin(), path->end() - 1);
                auto* t = dive(*current_table, parents);
                if (!t) return std::unexpected(ParseError{"key path conflict", L.position()});
                target = t;
            }
            (*target)[path->back()] = std::move(*v);
        }
    }

    return Document{std::move(root)};
}

std::expected<Document, ParseError> parse_file(const std::filesystem::path& p) {
    std::ifstream is(p);
    if (!is) {
        return std::unexpected(ParseError{
            std::format("cannot open '{}'", p.string()), {0, 0}});
    }
    std::stringstream ss;
    ss << is.rdbuf();
    return parse(ss.str());
}

std::string escape_string(std::string_view raw) {
    std::string out = "\"";
    for (char c : raw) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\t': out += "\\t";  break;
            case '\r': out += "\\r";  break;
            default:   out.push_back(c);
        }
    }
    out += '"';
    return out;
}

} // namespace mcpp::libs::toml
