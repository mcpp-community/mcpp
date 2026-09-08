// The parser, the semantic checks and the C++ emitter for `.toy`.
//
// One module rather than three, and the AST is not exported: the emitter is
// the AST's only consumer, so exporting it would widen the interface without
// adding a reader.
//
// The grammar, which is the whole language:
//
//   program := { kernel }
//   kernel  := 'kernel' ident '(' [ ident { ',' ident } ] ')' block
//   block   := '{' { stmt } '}'
//   stmt    := 'let' ident '=' expr ';'
//            | ident '=' expr ';'
//            | 'return' expr ';'
//            | 'if' '(' expr ')' block [ 'else' block ]
//            | 'while' '(' expr ')' block
//   expr    := cmp
//   cmp     := sum { ('<' | '>' | '<=' | '>=' | '==' | '!=') sum }
//   sum     := term { ('+' | '-') term }
//   term    := unary { ('*' | '/' | '%') unary }
//   unary   := [ '-' ] primary
//   primary := number | ident | ident '(' [ expr { ',' expr } ] ')' | '(' expr ')'
export module example.toyc.compile;

import std;
import example.toyc.lexer;

namespace toyc {

// ── the AST ──────────────────────────────────────────────────────────────
struct expr;
using expr_ptr = std::unique_ptr<expr>;

struct e_number { long long value; };
struct e_name   { std::string id; int line, col; };
struct e_unary  { std::string op; expr_ptr operand; };
struct e_binary { std::string op; expr_ptr lhs, rhs; };
struct e_call   { std::string callee; std::vector<expr_ptr> args; int line, col; };

struct expr { std::variant<e_number, e_name, e_unary, e_binary, e_call> node; };

struct stmt;
using stmt_ptr = std::unique_ptr<stmt>;
struct block { std::vector<stmt_ptr> body; };

struct s_let    { std::string name; expr_ptr init;  int line, col; };
struct s_assign { std::string name; expr_ptr value; int line, col; };
struct s_return { expr_ptr value; };
struct s_if     { expr_ptr cond; block then_branch; std::optional<block> else_branch; };
struct s_while  { expr_ptr cond; block body; };

struct stmt { std::variant<s_let, s_assign, s_return, s_if, s_while> node; };

struct kernel {
    std::string              name;
    std::vector<std::string> params;
    block                    body;
    int                      line, col;
};

// ── the parser ───────────────────────────────────────────────────────────
class parser {
public:
    explicit parser(std::vector<token> toks) : toks_(std::move(toks)) {}

    std::expected<std::vector<kernel>, diag> parse_program() {
        std::vector<kernel> out;
        while (peek().kind != tok::end) {
            auto k = parse_kernel();
            if (!k) return std::unexpected(k.error());
            out.push_back(std::move(*k));
        }
        if (out.empty()) return std::unexpected(err("a source file declares at least one kernel"));
        return out;
    }

private:
    std::vector<token> toks_;
    std::size_t        i_ = 0;

    const token& peek(std::size_t ahead = 0) const {
        return toks_[std::min(i_ + ahead, toks_.size() - 1)];
    }
    const token& take() { return toks_[i_ < toks_.size() - 1 ? i_++ : i_]; }
    bool accept(tok k) { if (peek().kind == k) { ++i_; return true; } return false; }

    diag err(std::string m) const { return diag{std::move(m), peek().line, peek().col}; }
    std::unexpected<diag> expected(tok k) const {
        return std::unexpected(err(std::format("expected {}, found {}",
                                               spelling(k), spelling(peek().kind))));
    }

    std::expected<kernel, diag> parse_kernel() {
        const token head = peek();
        if (!accept(tok::kw_kernel)) return expected(tok::kw_kernel);
        if (peek().kind != tok::ident) return expected(tok::ident);
        kernel k{take().text, {}, {}, head.line, head.col};
        if (!accept(tok::lparen)) return expected(tok::lparen);
        if (!accept(tok::rparen)) {
            for (;;) {
                if (peek().kind != tok::ident) return expected(tok::ident);
                k.params.push_back(take().text);
                if (accept(tok::comma)) continue;
                if (accept(tok::rparen)) break;
                return expected(tok::rparen);
            }
        }
        auto b = parse_block();
        if (!b) return std::unexpected(b.error());
        k.body = std::move(*b);
        return k;
    }

    std::expected<block, diag> parse_block() {
        if (!accept(tok::lbrace)) return expected(tok::lbrace);
        block b;
        while (!accept(tok::rbrace)) {
            if (peek().kind == tok::end) return expected(tok::rbrace);
            auto s = parse_stmt();
            if (!s) return std::unexpected(s.error());
            b.body.push_back(std::move(*s));
        }
        return b;
    }

    std::expected<stmt_ptr, diag> parse_stmt() {
        const token head = peek();

        if (accept(tok::kw_let)) {
            if (peek().kind != tok::ident) return expected(tok::ident);
            std::string name = take().text;
            if (!accept(tok::assign)) return expected(tok::assign);
            auto e = parse_expr();
            if (!e) return std::unexpected(e.error());
            if (!accept(tok::semi)) return expected(tok::semi);
            return std::make_unique<stmt>(stmt{s_let{std::move(name), std::move(*e),
                                                     head.line, head.col}});
        }
        if (accept(tok::kw_return)) {
            auto e = parse_expr();
            if (!e) return std::unexpected(e.error());
            if (!accept(tok::semi)) return expected(tok::semi);
            return std::make_unique<stmt>(stmt{s_return{std::move(*e)}});
        }
        if (accept(tok::kw_if)) {
            if (!accept(tok::lparen)) return expected(tok::lparen);
            auto c = parse_expr();
            if (!c) return std::unexpected(c.error());
            if (!accept(tok::rparen)) return expected(tok::rparen);
            auto t = parse_block();
            if (!t) return std::unexpected(t.error());
            std::optional<block> e;
            if (accept(tok::kw_else)) {
                auto eb = parse_block();
                if (!eb) return std::unexpected(eb.error());
                e = std::move(*eb);
            }
            return std::make_unique<stmt>(stmt{s_if{std::move(*c), std::move(*t), std::move(e)}});
        }
        if (accept(tok::kw_while)) {
            if (!accept(tok::lparen)) return expected(tok::lparen);
            auto c = parse_expr();
            if (!c) return std::unexpected(c.error());
            if (!accept(tok::rparen)) return expected(tok::rparen);
            auto b = parse_block();
            if (!b) return std::unexpected(b.error());
            return std::make_unique<stmt>(stmt{s_while{std::move(*c), std::move(*b)}});
        }
        if (peek().kind == tok::ident && peek(1).kind == tok::assign) {
            std::string name = take().text;
            take();                                   // `=`
            auto e = parse_expr();
            if (!e) return std::unexpected(e.error());
            if (!accept(tok::semi)) return expected(tok::semi);
            return std::make_unique<stmt>(stmt{s_assign{std::move(name), std::move(*e),
                                                        head.line, head.col}});
        }
        return std::unexpected(err(std::format(
            "expected a statement, found {}", spelling(peek().kind))));
    }

    // Precedence is expressed by the call chain: cmp calls sum calls term.
    std::expected<expr_ptr, diag> parse_expr() { return parse_cmp(); }

    std::expected<expr_ptr, diag> parse_cmp() {
        auto lhs = parse_sum();
        if (!lhs) return lhs;
        for (;;) {
            std::string op;
            switch (peek().kind) {
                case tok::lt: op = "<";  break;
                case tok::gt: op = ">";  break;
                case tok::le: op = "<="; break;
                case tok::ge: op = ">="; break;
                case tok::eq: op = "=="; break;
                case tok::ne: op = "!="; break;
                default: return lhs;
            }
            take();
            auto rhs = parse_sum();
            if (!rhs) return rhs;
            lhs = std::make_unique<expr>(expr{e_binary{op, std::move(*lhs), std::move(*rhs)}});
        }
    }

    std::expected<expr_ptr, diag> parse_sum() {
        auto lhs = parse_term();
        if (!lhs) return lhs;
        for (;;) {
            std::string op;
            if      (peek().kind == tok::plus)  op = "+";
            else if (peek().kind == tok::minus) op = "-";
            else return lhs;
            take();
            auto rhs = parse_term();
            if (!rhs) return rhs;
            lhs = std::make_unique<expr>(expr{e_binary{op, std::move(*lhs), std::move(*rhs)}});
        }
    }

    std::expected<expr_ptr, diag> parse_term() {
        auto lhs = parse_unary();
        if (!lhs) return lhs;
        for (;;) {
            std::string op;
            if      (peek().kind == tok::star)    op = "*";
            else if (peek().kind == tok::slash)   op = "/";
            else if (peek().kind == tok::percent) op = "%";
            else return lhs;
            take();
            auto rhs = parse_unary();
            if (!rhs) return rhs;
            lhs = std::make_unique<expr>(expr{e_binary{op, std::move(*lhs), std::move(*rhs)}});
        }
    }

    std::expected<expr_ptr, diag> parse_unary() {
        if (accept(tok::minus)) {
            auto e = parse_unary();
            if (!e) return e;
            return std::make_unique<expr>(expr{e_unary{"-", std::move(*e)}});
        }
        return parse_primary();
    }

    std::expected<expr_ptr, diag> parse_primary() {
        const token head = peek();
        if (head.kind == tok::number) {
            take();
            return std::make_unique<expr>(expr{e_number{head.value}});
        }
        if (head.kind == tok::ident) {
            take();
            if (!accept(tok::lparen))
                return std::make_unique<expr>(expr{e_name{head.text, head.line, head.col}});
            e_call c{head.text, {}, head.line, head.col};
            if (!accept(tok::rparen)) {
                for (;;) {
                    auto a = parse_expr();
                    if (!a) return a;
                    c.args.push_back(std::move(*a));
                    if (accept(tok::comma)) continue;
                    if (accept(tok::rparen)) break;
                    return expected(tok::rparen);
                }
            }
            return std::make_unique<expr>(expr{std::move(c)});
        }
        if (accept(tok::lparen)) {
            auto e = parse_expr();
            if (!e) return e;
            if (!accept(tok::rparen)) return expected(tok::rparen);
            return e;
        }
        return std::unexpected(err(std::format(
            "expected an expression, found {}", spelling(head.kind))));
    }
};

// ── the emitter, with the semantic checks it needs to emit at all ────────
class emitter {
public:
    explicit emitter(const std::vector<kernel>& ks) {
        for (const auto& k : ks) arity_[k.name] = k.params.size();
    }

    std::expected<std::string, diag> emit(const std::vector<kernel>& ks,
                                          std::string_view source_name) {
        std::string out = std::format(
            "// Generated by toyc from {}. Do not edit.\n"
            "//\n"
            "// Every kernel is `extern \"C\"`: the C++ side and this file are\n"
            "// produced by different compilers and share no C++ ABI.\n\n",
            source_name);

        for (const auto& k : ks) out += declaration(k) + ";\n";
        out += "\n";

        for (const auto& k : ks) {
            scopes_.clear();
            scopes_.emplace_back();
            for (const auto& p : k.params) scopes_.back().insert(p);

            if (!always_returns(k.body))
                return std::unexpected(diag{
                    std::format("kernel `{}` can reach its end without a `return`", k.name),
                    k.line, k.col});

            out += declaration(k) + " {\n";
            auto body = emit_block(k.body, 1);
            if (!body) return std::unexpected(body.error());
            out += *body;
            out += "}\n\n";
        }
        return out;
    }

private:
    std::map<std::string, std::size_t>      arity_;
    std::vector<std::set<std::string>>      scopes_;

    // `v_` on every local, `toy_` on every kernel. The target language has
    // keywords of its own, and a kernel that names a variable `class` must not
    // become a C++ file that does not compile for a reason the toy source
    // cannot express.
    static std::string local(std::string_view n)  { return std::format("v_{}", n); }
    static std::string global(std::string_view n) { return std::format("toy_{}", n); }

    static std::string declaration(const kernel& k) {
        std::string params;
        for (std::size_t i = 0; i < k.params.size(); ++i)
            params += std::format("{}int {}", i ? ", " : "", local(k.params[i]));
        return std::format("extern \"C\" int {}({})", global(k.name),
                           params.empty() ? "void" : params);
    }

    // Every kernel returns an integer, so every path out of one is a `return`.
    // A block satisfies that if its last statement is a `return`, or an
    // `if`/`else` whose branches both do -- `while` never counts, because the
    // language cannot say that a loop runs at all.
    //
    // Emitting `return 0;` at the end instead would have compiled everything
    // and given a wrong answer for the kernel whose author forgot a branch.
    static bool always_returns(const block& b) {
        if (b.body.empty()) return false;
        const stmt& last = *b.body.back();
        if (std::holds_alternative<s_return>(last.node)) return true;
        if (const auto* i = std::get_if<s_if>(&last.node))
            return i->else_branch && always_returns(i->then_branch)
                                  && always_returns(*i->else_branch);
        return false;
    }

    bool visible(const std::string& n) const {
        return std::ranges::any_of(scopes_, [&](const auto& s) { return s.contains(n); });
    }

    std::expected<std::string, diag> emit_block(const block& b, int depth) {
        scopes_.emplace_back();
        std::string out;
        for (const auto& s : b.body) {
            auto one = emit_stmt(*s, depth);
            if (!one) return one;
            out += *one;
        }
        scopes_.pop_back();
        return out;
    }

    std::expected<std::string, diag> emit_stmt(const stmt& s, int depth) {
        const std::string pad(static_cast<std::size_t>(depth) * 4, ' ');
        return std::visit([&](const auto& n) -> std::expected<std::string, diag> {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, s_let>) {
                if (scopes_.back().contains(n.name))
                    return std::unexpected(diag{
                        std::format("`{}` is already declared in this block", n.name),
                        n.line, n.col});
                auto e = emit_expr(*n.init);
                if (!e) return std::unexpected(e.error());
                scopes_.back().insert(n.name);
                return std::format("{}int {} = {};\n", pad, local(n.name), *e);
            } else if constexpr (std::is_same_v<T, s_assign>) {
                if (!visible(n.name))
                    return std::unexpected(diag{
                        std::format("`{}` is assigned before it is declared", n.name),
                        n.line, n.col});
                auto e = emit_expr(*n.value);
                if (!e) return std::unexpected(e.error());
                return std::format("{}{} = {};\n", pad, local(n.name), *e);
            } else if constexpr (std::is_same_v<T, s_return>) {
                auto e = emit_expr(*n.value);
                if (!e) return std::unexpected(e.error());
                return std::format("{}return {};\n", pad, *e);
            } else if constexpr (std::is_same_v<T, s_if>) {
                auto c = emit_expr(*n.cond);
                if (!c) return std::unexpected(c.error());
                auto t = emit_block(n.then_branch, depth + 1);
                if (!t) return t;
                std::string out = std::format("{}if ({}) {{\n{}{}}}", pad, *c, *t, pad);
                if (n.else_branch) {
                    auto e = emit_block(*n.else_branch, depth + 1);
                    if (!e) return e;
                    out += std::format(" else {{\n{}{}}}", *e, pad);
                }
                return out + "\n";
            } else {
                auto c = emit_expr(*n.cond);
                if (!c) return std::unexpected(c.error());
                auto b = emit_block(n.body, depth + 1);
                if (!b) return b;
                return std::format("{}while ({}) {{\n{}{}}}\n", pad, *c, *b, pad);
            }
        }, s.node);
    }

    std::expected<std::string, diag> emit_expr(const expr& e) {
        return std::visit([&](const auto& n) -> std::expected<std::string, diag> {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, e_number>) {
                return std::format("{}", n.value);
            } else if constexpr (std::is_same_v<T, e_name>) {
                if (!visible(n.id))
                    return std::unexpected(diag{
                        std::format("`{}` is not declared", n.id), n.line, n.col});
                return local(n.id);
            } else if constexpr (std::is_same_v<T, e_unary>) {
                auto o = emit_expr(*n.operand);
                if (!o) return o;
                return std::format("(-{})", *o);
            } else if constexpr (std::is_same_v<T, e_binary>) {
                auto l = emit_expr(*n.lhs);
                if (!l) return l;
                auto r = emit_expr(*n.rhs);
                if (!r) return r;
                // Parenthesised because the emitter does not reproduce C++'s
                // precedence table: the AST already holds the grouping, and
                // parentheses are how it survives into the target language.
                return std::format("({} {} {})", *l, n.op, *r);
            } else {
                const auto it = arity_.find(n.callee);
                if (it == arity_.end())
                    return std::unexpected(diag{
                        std::format("`{}` is not a kernel in this file", n.callee),
                        n.line, n.col});
                if (it->second != n.args.size())
                    return std::unexpected(diag{
                        std::format("kernel `{}` takes {} argument(s), {} given",
                                    n.callee, it->second, n.args.size()),
                        n.line, n.col});
                std::string args;
                for (std::size_t i = 0; i < n.args.size(); ++i) {
                    auto a = emit_expr(*n.args[i]);
                    if (!a) return a;
                    args += std::format("{}{}", i ? ", " : "", *a);
                }
                return std::format("{}({})", global(n.callee), args);
            }
        }, e.node);
    }
};

} // namespace toyc

export namespace toyc {

// The whole compiler as one function: text in, C++ out, or one diagnostic
// with a position.
std::expected<std::string, diag> compile(std::string_view source,
                                         std::string_view source_name) {
    auto tokens = scan(source);
    if (!tokens) return std::unexpected(tokens.error());
    parser p(std::move(*tokens));
    auto kernels = p.parse_program();
    if (!kernels) return std::unexpected(kernels.error());

    std::set<std::string> seen;
    for (const auto& k : *kernels)
        if (!seen.insert(k.name).second)
            return std::unexpected(diag{std::format("kernel `{}` is declared twice", k.name),
                                        k.line, k.col});

    emitter e(*kernels);
    return e.emit(*kernels, source_name);
}

} // namespace toyc
