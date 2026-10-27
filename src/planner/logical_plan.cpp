#include "planner/logical_plan.h"

#include <stdexcept>

std::string exprToString(const Expr& e) {
    switch (e.kind) {
        case Expr::Literal:
            if (std::holds_alternative<std::string>(e.lit)) return "'" + std::get<std::string>(e.lit) + "'";
            return toString(e.lit);
        case Expr::ColumnRef: return e.col;
        case Expr::Not: return "NOT " + exprToString(*e.left);
        case Expr::Binary:
            return "(" + exprToString(*e.left) + " " + e.op + " " + exprToString(*e.right) + ")";
    }
    return "";
}

std::string LogicalPlan::toString(int indent) const {
    std::string s(indent, ' ');
    switch (kind) {
        case Scan: s += "Scan(" + table + ")"; break;
        case Filter: s += "Filter(" + exprToString(*predicate) + ")"; break;
        case Project: {
            s += "Project(";
            for (size_t i = 0; i < columns.size(); ++i) s += (i ? ", " : "") + columns[i];
            s += ")";
            break;
        }
    }
    if (child) s += "\n" + child->toString(indent + 2);
    return s;
}

namespace {
void checkColumns(const Expr& e, const Schema& schema) {
    if (e.kind == Expr::ColumnRef && schema.indexOf(e.col) < 0)
        throw std::runtime_error("unknown column: " + e.col);
    if (e.left) checkColumns(*e.left, schema);
    if (e.right) checkColumns(*e.right, schema);
}
}  // namespace

std::unique_ptr<LogicalPlan> buildLogicalPlan(SelectStmt stmt, const Catalog& catalog) {
    const Schema& schema = catalog.get(stmt.table).schema;

    if (stmt.star) {
        stmt.columns.clear();
        for (const auto& c : schema.cols) stmt.columns.push_back(c.name);
    }
    for (const auto& c : stmt.columns)
        if (schema.indexOf(c) < 0) throw std::runtime_error("unknown column: " + c);

    auto plan = std::make_unique<LogicalPlan>();
    plan->kind = LogicalPlan::Scan;
    plan->table = stmt.table;

    if (stmt.where) {
        checkColumns(*stmt.where, schema);
        auto f = std::make_unique<LogicalPlan>();
        f->kind = LogicalPlan::Filter;
        f->predicate = std::move(stmt.where);
        f->child = std::move(plan);
        plan = std::move(f);
    }

    auto p = std::make_unique<LogicalPlan>();
    p->kind = LogicalPlan::Project;
    p->columns = std::move(stmt.columns);
    p->child = std::move(plan);
    return p;
}
