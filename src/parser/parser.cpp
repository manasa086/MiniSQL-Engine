#include "parser/parser.h"

#include <algorithm>
#include <stdexcept>

#include "parser/lexer.h"

namespace {

std::string upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::toupper(c); });
    return s;
}

class Parser {
public:
    explicit Parser(std::vector<Token> t) : toks_(std::move(t)) {}

    SelectStmt select() {
        expectKeyword("SELECT");
        SelectStmt st;
        if (isSymbol("*")) {
            ++pos_;
            st.star = true;
        } else {
            st.columns.push_back(ident());
            while (isSymbol(",")) { ++pos_; st.columns.push_back(ident()); }
        }
        expectKeyword("FROM");
        st.table = ident();
        if (isKeyword("WHERE")) { ++pos_; st.where = orExpr(); }
        if (isSymbol(";")) ++pos_;
        if (cur().kind != Token::End) throw std::runtime_error("unexpected token: " + cur().text);
        return st;
    }

private:
    const Token& cur() const { return toks_[pos_]; }
    bool isSymbol(const char* s) const { return cur().kind == Token::Symbol && cur().text == s; }
    bool isKeyword(const char* k) const { return cur().kind == Token::Ident && upper(cur().text) == k; }
    void expectKeyword(const char* k) {
        if (!isKeyword(k)) throw std::runtime_error(std::string("expected ") + k);
        ++pos_;
    }
    std::string ident() {
        if (cur().kind != Token::Ident) throw std::runtime_error("expected identifier");
        return toks_[pos_++].text;
    }

    static std::unique_ptr<Expr> binary(std::string op, std::unique_ptr<Expr> l, std::unique_ptr<Expr> r) {
        auto e = std::make_unique<Expr>();
        e->kind = Expr::Binary;
        e->op = std::move(op);
        e->left = std::move(l);
        e->right = std::move(r);
        return e;
    }

    std::unique_ptr<Expr> orExpr() {
        auto l = andExpr();
        while (isKeyword("OR")) { ++pos_; l = binary("OR", std::move(l), andExpr()); }
        return l;
    }
    std::unique_ptr<Expr> andExpr() {
        auto l = notExpr();
        while (isKeyword("AND")) { ++pos_; l = binary("AND", std::move(l), notExpr()); }
        return l;
    }
    std::unique_ptr<Expr> notExpr() {
        if (isKeyword("NOT")) {
            ++pos_;
            auto e = std::make_unique<Expr>();
            e->kind = Expr::Not;
            e->left = notExpr();
            return e;
        }
        return cmpExpr();
    }
    std::unique_ptr<Expr> cmpExpr() {
        auto l = primary();
        for (const char* op : {"=", "!=", "<=", ">=", "<", ">"}) {
            if (isSymbol(op)) { ++pos_; return binary(op, std::move(l), primary()); }
        }
        return l;
    }
    std::unique_ptr<Expr> primary() {
        auto e = std::make_unique<Expr>();
        const Token& t = cur();
        if (t.kind == Token::Number) {
            e->kind = Expr::Literal;
            e->lit = static_cast<int64_t>(std::stoll(t.text));
            ++pos_;
        } else if (t.kind == Token::String) {
            e->kind = Expr::Literal;
            e->lit = t.text;
            ++pos_;
        } else if (t.kind == Token::Ident) {
            e->kind = Expr::ColumnRef;
            e->col = t.text;
            ++pos_;
        } else if (isSymbol("(")) {
            ++pos_;
            auto inner = orExpr();
            if (!isSymbol(")")) throw std::runtime_error("expected )");
            ++pos_;
            return inner;
        } else {
            throw std::runtime_error("unexpected token in expression: " + t.text);
        }
        return e;
    }

    std::vector<Token> toks_;
    size_t pos_ = 0;
};

}  // namespace

SelectStmt parseSelect(const std::string& sql) { return Parser(tokenize(sql)).select(); }
