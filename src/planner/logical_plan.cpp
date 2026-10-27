#include "planner/logical_plan.h"

#include <set>
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

namespace {
std::string join(const std::vector<std::string>& v) {
    std::string s;
    for (size_t i = 0; i < v.size(); ++i) s += (i ? ", " : "") + v[i];
    return s;
}
}  // namespace

std::string LogicalPlan::toString(int indent) const {
    std::string s(indent, ' ');
    switch (kind) {
        case Scan: s += "Scan(" + table + ")"; break;
        case Filter: s += "Filter(" + exprToString(*predicate) + ")"; break;
        case Project: s += "Project(" + join(columns) + ")"; break;
        case Join: s += "Join(" + leftKey + " = " + rightKey + ")"; break;
        case Aggregate: {
            std::vector<std::string> names;
            for (const auto& a : aggs) names.push_back(a.name());
            s += "Aggregate(group by [" + join(groupBy) + "]; " + join(names) + ")";
            break;
        }
    }
    if (child) s += "\n" + child->toString(indent + 2);
    if (right) s += "\n" + right->toString(indent + 2);
    return s;
}

namespace {

void checkColumns(const Expr& e, const Schema& schema) {
    if (e.kind == Expr::ColumnRef && schema.indexOf(e.col) < 0)
        throw std::runtime_error("unknown column: " + e.col);
    if (e.left) checkColumns(*e.left, schema);
    if (e.right) checkColumns(*e.right, schema);
}

int requireColumn(const Schema& schema, const std::string& name) {
    int i = schema.indexOf(name);
    if (i < 0) throw std::runtime_error("unknown column: " + name);
    return i;
}

std::unique_ptr<LogicalPlan> scanNode(const std::string& table) {
    auto n = std::make_unique<LogicalPlan>();
    n->kind = LogicalPlan::Scan;
    n->table = table;
    return n;
}

std::unique_ptr<LogicalPlan> wrap(LogicalPlan::Kind kind, std::unique_ptr<LogicalPlan> child) {
    auto n = std::make_unique<LogicalPlan>();
    n->kind = kind;
    n->child = std::move(child);
    return n;
}

}  // namespace

std::unique_ptr<LogicalPlan> buildLogicalPlan(SelectStmt stmt, const Catalog& catalog) {
    Schema cur = catalog.get(stmt.table).schema;  // columns visible so far
    auto plan = scanNode(stmt.table);

    std::set<std::string> seen{stmt.table};
    for (const auto& j : stmt.joins) {
        if (!seen.insert(j.table).second) throw std::runtime_error("table joined twice (no aliases): " + j.table);
        const Schema& rs = catalog.get(j.table).schema;
        auto resolves = [&](const std::string& l, const std::string& r) {
            return cur.indexOf(l) >= 0 && rs.indexOf(r) >= 0;
        };
        std::string lk = j.leftCol, rk = j.rightCol;
        if (!resolves(lk, rk)) {
            std::swap(lk, rk);  // allow ON right.col = left.col
            if (!resolves(lk, rk)) throw std::runtime_error("cannot resolve join condition " + j.leftCol + " = " + j.rightCol);
        }
        if (cur.cols[cur.indexOf(lk)].type != rs.cols[rs.indexOf(rk)].type)
            throw std::runtime_error("join key types differ: " + lk + " vs " + rk);

        auto node = wrap(LogicalPlan::Join, std::move(plan));
        node->leftKey = lk;
        node->rightKey = rk;
        node->right = scanNode(j.table);
        plan = std::move(node);
        cur.cols.insert(cur.cols.end(), rs.cols.begin(), rs.cols.end());
    }

    if (stmt.where) {
        checkColumns(*stmt.where, cur);
        auto f = wrap(LogicalPlan::Filter, std::move(plan));
        f->predicate = std::move(stmt.where);
        plan = std::move(f);
    }

    if (stmt.star) {
        if (!stmt.groupBy.empty()) throw std::runtime_error("SELECT * cannot be used with GROUP BY");
        for (const auto& c : cur.cols)  // qualify names when joins make bare names ambiguous
            stmt.items.push_back({"", stmt.joins.empty() ? c.name : c.table + "." + c.name});
    }

    bool hasAgg = !stmt.groupBy.empty();
    for (const auto& it : stmt.items) hasAgg = hasAgg || !it.fn.empty();

    std::vector<std::string> outCols;
    if (hasAgg) {
        auto agg = wrap(LogicalPlan::Aggregate, std::move(plan));
        Schema out;  // what the Aggregate node produces: group columns, then aggregates
        for (const auto& g : stmt.groupBy) {
            agg->groupBy.push_back(g);
            out.cols.push_back(cur.cols[requireColumn(cur, g)]);
        }
        std::set<std::string> aggNames;
        for (const auto& it : stmt.items) {
            if (it.fn.empty() || !aggNames.insert(it.name()).second) continue;
            Type t = Type::Int;
            if (it.column != "*") {
                const Column& c = cur.cols[requireColumn(cur, it.column)];
                if (it.fn == "SUM" && c.type != Type::Int) throw std::runtime_error("SUM needs an INT column: " + it.column);
                if (it.fn == "MIN" || it.fn == "MAX") t = c.type;
            }
            agg->aggs.push_back({it.fn, it.column});
            out.cols.push_back({it.name(), t, ""});
        }
        for (const auto& it : stmt.items) {
            if (it.fn.empty() && out.indexOf(it.column) < 0)
                throw std::runtime_error("column " + it.column + " must appear in GROUP BY or inside an aggregate");
            outCols.push_back(it.name());
        }
        plan = std::move(agg);
    } else {
        for (const auto& it : stmt.items) {
            requireColumn(cur, it.column);
            outCols.push_back(it.column);
        }
    }

    auto p = wrap(LogicalPlan::Project, std::move(plan));
    p->columns = std::move(outCols);
    return p;
}
