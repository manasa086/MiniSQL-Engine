#include "optimizer/access_path.h"

#include <algorithm>
#include <limits>
#include <string>
#include <vector>

namespace {

void conjuncts(const Expr& e, std::vector<const Expr*>& out) {
    if (e.kind == Expr::Binary && e.op == "AND") {
        conjuncts(*e.left, out);
        conjuncts(*e.right, out);
    } else {
        out.push_back(&e);
    }
}

std::string flip(const std::string& op) {
    if (op == "<") return ">";
    if (op == ">") return "<";
    if (op == "<=") return ">=";
    if (op == ">=") return "<=";
    return op;
}

}  // namespace

std::optional<IndexRange> chooseIndexRange(const Expr& pred, const Table& table) {
    constexpr int64_t MIN = std::numeric_limits<int64_t>::min();
    constexpr int64_t MAX = std::numeric_limits<int64_t>::max();
    std::vector<const Expr*> conj;
    conjuncts(pred, conj);

    for (const auto& idx : table.indexes) {
        int64_t lo = MIN, hi = MAX;
        bool used = false, empty = false;
        for (const Expr* c : conj) {
            if (c->kind != Expr::Binary) continue;
            const Expr *col, *lit;
            std::string op = c->op;
            if (c->left->kind == Expr::ColumnRef && c->right->kind == Expr::Literal) {
                col = c->left.get();
                lit = c->right.get();
            } else if (c->left->kind == Expr::Literal && c->right->kind == Expr::ColumnRef) {
                col = c->right.get();
                lit = c->left.get();
                op = flip(op);
            } else {
                continue;
            }
            const int64_t* v = std::get_if<int64_t>(&lit->lit);
            if (!v || (col->col != idx->column && col->col != table.name + "." + idx->column)) continue;

            if (op == "=") { lo = std::max(lo, *v); hi = std::min(hi, *v); }
            else if (op == ">=") lo = std::max(lo, *v);
            else if (op == "<=") hi = std::min(hi, *v);
            else if (op == ">") { if (*v == MAX) empty = true; else lo = std::max(lo, *v + 1); }
            else if (op == "<") { if (*v == MIN) empty = true; else hi = std::min(hi, *v - 1); }
            else continue;
            used = true;
        }
        if (used) {
            if (empty) { lo = 1; hi = 0; }
            return IndexRange{idx.get(), lo, hi};
        }
    }
    return std::nullopt;
}
