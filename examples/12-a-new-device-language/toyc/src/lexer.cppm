// The lexer for `.toy`.
//
// The interface exports one function and two aggregates. The parser's own
// types -- the variant-shaped AST -- stay inside `example.toyc.compile`,
// because a module interface that exports nested standard containers is the
// shape that has truncated BMIs on this repository's own toolchain before.
export module example.toyc.lexer;

import std;

export namespace toyc {

enum class tok {
    end, number, ident,
    kw_kernel, kw_let, kw_return, kw_if, kw_else, kw_while,
    lparen, rparen, lbrace, rbrace, comma, semi,
    assign, plus, minus, star, slash, percent,
    lt, gt, le, ge, eq, ne,
};

struct token {
    tok          kind  = tok::end;
    std::string  text;             // for ident, and for diagnostics
    long long    value = 0;        // for number
    int          line  = 1;
    int          col   = 1;
};

// A diagnostic carries a position because a compiler that cannot say WHERE is
// a compiler its users debug by bisecting the input.
struct diag {
    std::string message;
    int         line = 0;
    int         col  = 0;
};

std::string spelling(tok k) {
    switch (k) {
        case tok::end:       return "end of file";
        case tok::number:    return "a number";
        case tok::ident:     return "an identifier";
        case tok::kw_kernel: return "`kernel`";
        case tok::kw_let:    return "`let`";
        case tok::kw_return: return "`return`";
        case tok::kw_if:     return "`if`";
        case tok::kw_else:   return "`else`";
        case tok::kw_while:  return "`while`";
        case tok::lparen:    return "`(`";
        case tok::rparen:    return "`)`";
        case tok::lbrace:    return "`{`";
        case tok::rbrace:    return "`}`";
        case tok::comma:     return "`,`";
        case tok::semi:      return "`;`";
        case tok::assign:    return "`=`";
        default:             return "an operator";
    }
}

std::expected<std::vector<token>, diag> scan(std::string_view src) {
    std::vector<token> out;
    int line = 1, col = 1;
    std::size_t i = 0;

    auto advance = [&](std::size_t n) {
        for (std::size_t k = 0; k < n; ++k) {
            if (src[i + k] == '\n') { ++line; col = 1; } else { ++col; }
        }
        i += n;
    };
    auto push = [&](tok k, std::size_t n, std::string text = {}) {
        out.push_back(token{k, std::move(text), 0, line, col});
        advance(n);
    };

    while (i < src.size()) {
        const char c = src[i];

        if (c == '#') {                                  // a comment runs to the newline
            std::size_t n = 0;
            while (i + n < src.size() && src[i + n] != '\n') ++n;
            advance(n);
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(c))) { advance(1); continue; }

        if (std::isdigit(static_cast<unsigned char>(c))) {
            std::size_t n = 0;
            long long v = 0;
            while (i + n < src.size() && std::isdigit(static_cast<unsigned char>(src[i + n]))) {
                v = v * 10 + (src[i + n] - '0');
                ++n;
            }
            token t{tok::number, {}, v, line, col};
            out.push_back(t);
            advance(n);
            continue;
        }

        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            std::size_t n = 0;
            while (i + n < src.size() &&
                   (std::isalnum(static_cast<unsigned char>(src[i + n])) || src[i + n] == '_')) ++n;
            const std::string word(src.substr(i, n));
            const tok k = word == "kernel" ? tok::kw_kernel
                        : word == "let"    ? tok::kw_let
                        : word == "return" ? tok::kw_return
                        : word == "if"     ? tok::kw_if
                        : word == "else"   ? tok::kw_else
                        : word == "while"  ? tok::kw_while
                                           : tok::ident;
            push(k, n, word);
            continue;
        }

        // Two-character operators are tested first: `<` is a prefix of `<=`,
        // and testing the one-character form first would lex `<=` as `<` `=`
        // and report the error at the `=`, one column past the cause.
        const std::string_view two = src.substr(i, 2);
        if (two == "<=") { push(tok::le, 2); continue; }
        if (two == ">=") { push(tok::ge, 2); continue; }
        if (two == "==") { push(tok::eq, 2); continue; }
        if (two == "!=") { push(tok::ne, 2); continue; }

        switch (c) {
            case '(': push(tok::lparen,  1); continue;
            case ')': push(tok::rparen,  1); continue;
            case '{': push(tok::lbrace,  1); continue;
            case '}': push(tok::rbrace,  1); continue;
            case ',': push(tok::comma,   1); continue;
            case ';': push(tok::semi,    1); continue;
            case '=': push(tok::assign,  1); continue;
            case '+': push(tok::plus,    1); continue;
            case '-': push(tok::minus,   1); continue;
            case '*': push(tok::star,    1); continue;
            case '/': push(tok::slash,   1); continue;
            case '%': push(tok::percent, 1); continue;
            case '<': push(tok::lt,      1); continue;
            case '>': push(tok::gt,      1); continue;
            default: break;
        }
        return std::unexpected(diag{std::format("unexpected character `{}`", c), line, col});
    }

    out.push_back(token{tok::end, {}, 0, line, col});
    return out;
}

} // namespace toyc
