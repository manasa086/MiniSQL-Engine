#include "execution/operator.h"

#include <stdexcept>

Value evalExpr(const Expr& e, const Schema& schema, const Row& row) {
    auto truthy = [](const Value& v) { return std::holds_alternative<int64_t>(v) && std::get<int64_t>(v) != 0; };
    switch (e.kind) {
        case Expr::Literal: return e.lit;
        case Expr::ColumnRef: return row[schema.indexOf(e.col)];
        case Expr::Not: return int64_t(!truthy(evalExpr(*e.left, schema, row)));
        case Expr::Binary: {
            if (e.op == "AND")
                return int64_t(truthy(evalExpr(*e.left, schema, row)) && truthy(evalExpr(*e.right, schema, row)));
            if (e.op == "OR")
                return int64_t(truthy(evalExpr(*e.left, schema, row)) || truthy(evalExpr(*e.right, schema, row)));
            int c = compare(evalExpr(*e.left, schema, row), evalExpr(*e.right, schema, row));
            if (e.op == "=") return int64_t(c == 0);
            if (e.op == "!=") return int64_t(c != 0);
            if (e.op == "<") return int64_t(c < 0);
            if (e.op == "<=") return int64_t(c <= 0);
            if (e.op == ">") return int64_t(c > 0);
            if (e.op == ">=") return int64_t(c >= 0);
            throw std::runtime_error("unknown operator: " + e.op);
        }
    }
    throw std::runtime_error("bad expression");
}

bool SeqScan::next(Row& row) {
    if (pos_ >= table_.rows.size()) return false;
    row = table_.rows[pos_++];
    return true;
}

bool Filter::next(Row& row) {
    while (child_->next(row)) {
        const Value v = evalExpr(pred_, child_->schema(), row);
        if (std::holds_alternative<int64_t>(v) && std::get<int64_t>(v) != 0) return true;
    }
    return false;
}

Projection::Projection(std::unique_ptr<Operator> child, const std::vector<std::string>& cols)
    : child_(std::move(child)) {
    for (const auto& c : cols) {
        int i = child_->schema().indexOf(c);
        if (i < 0) throw std::runtime_error("unknown column: " + c);
        idx_.push_back(i);
        schema_.cols.push_back(child_->schema().cols[i]);
    }
}

bool Projection::next(Row& row) {
    Row in;
    if (!child_->next(in)) return false;
    row.clear();
    for (int i : idx_) row.push_back(std::move(in[i]));
    return true;
}

std::unique_ptr<Operator> buildOperator(const LogicalPlan& plan, const Catalog& catalog) {
    switch (plan.kind) {
        case LogicalPlan::Scan: return std::make_unique<SeqScan>(catalog.get(plan.table));
        case LogicalPlan::Filter:
            return std::make_unique<Filter>(buildOperator(*plan.child, catalog), *plan.predicate);
        case LogicalPlan::Project:
            return std::make_unique<Projection>(buildOperator(*plan.child, catalog), plan.columns);
    }
    throw std::runtime_error("bad plan node");
}
