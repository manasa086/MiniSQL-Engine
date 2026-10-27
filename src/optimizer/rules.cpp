#include "optimizer/rules.h"

#include <set>
#include <stdexcept>

namespace {

using ExprList = std::vector<std::unique_ptr<Expr>>;

void split(std::unique_ptr<Expr> e, ExprList& out) {
    if (e->kind == Expr::Binary && e->op == "AND") {
        auto l = std::move(e->left), r = std::move(e->right);
        split(std::move(l), out);
        split(std::move(r), out);
    } else {
        out.push_back(std::move(e));
    }
}

std::unique_ptr<Expr> andAll(ExprList& list) {
    std::unique_ptr<Expr> acc;
    for (auto& e : list) {
        if (!acc) { acc = std::move(e); continue; }
        auto n = std::make_unique<Expr>();
        n->kind = Expr::Binary;
        n->op = "AND";
        n->left = std::move(acc);
        n->right = std::move(e);
        acc = std::move(n);
    }
    return acc;
}

void collectColumns(const Expr& e, std::vector<std::string>& out) {
    if (e.kind == Expr::ColumnRef) out.push_back(e.col);
    if (e.left) collectColumns(*e.left, out);
    if (e.right) collectColumns(*e.right, out);
}

// True if every column the expression mentions resolves inside `schema`.
bool resolvesIn(const Expr& e, const Schema& schema) {
    std::vector<std::string> cols;
    collectColumns(e, cols);
    try {
        for (const auto& c : cols)
            if (schema.indexOf(c) < 0) return false;
    } catch (const std::exception&) {
        return false;
    }
    return true;
}

std::unique_ptr<LogicalPlan> withFilter(std::unique_ptr<LogicalPlan> plan, ExprList& preds) {
    if (preds.empty()) return plan;
    auto f = std::make_unique<LogicalPlan>();
    f->kind = LogicalPlan::Filter;
    f->predicate = andAll(preds);
    f->child = std::move(plan);
    return f;
}

// Pushes `pending` (predicates that must hold on this subtree's output) as deep as possible.
std::unique_ptr<LogicalPlan> pushDown(std::unique_ptr<LogicalPlan> n, ExprList pending, const Catalog& catalog) {
    switch (n->kind) {
        case LogicalPlan::Filter:
            split(std::move(n->predicate), pending);
            return pushDown(std::move(n->child), std::move(pending), catalog);
        case LogicalPlan::Scan: return withFilter(std::move(n), pending);
        case LogicalPlan::Join: {
            Schema ls = planSchema(*n->child, catalog), rs = planSchema(*n->right, catalog);
            ExprList toLeft, toRight, keep;
            for (auto& e : pending) {
                if (resolvesIn(*e, ls)) toLeft.push_back(std::move(e));
                else if (resolvesIn(*e, rs)) toRight.push_back(std::move(e));
                else keep.push_back(std::move(e));
            }
            n->child = pushDown(std::move(n->child), std::move(toLeft), catalog);
            n->right = pushDown(std::move(n->right), std::move(toRight), catalog);
            return withFilter(std::move(n), keep);
        }
        case LogicalPlan::Project:
        case LogicalPlan::Aggregate:
            n->child = pushDown(std::move(n->child), {}, catalog);
            return withFilter(std::move(n), pending);
    }
    throw std::runtime_error("bad plan node");
}

// A column's identity independent of how the query spelled it: "table.col" for base columns.
std::string columnId(const Schema& s, const std::string& name) {
    int i = s.indexOf(name);
    if (i < 0) throw std::runtime_error("unknown column: " + name);
    return s.cols[i].table.empty() ? s.cols[i].name : s.cols[i].table + "." + s.cols[i].name;
}

bool isLeafUnit(const LogicalPlan& n) {
    return n.kind == LogicalPlan::Scan || (n.kind == LogicalPlan::Filter && n.child->kind == LogicalPlan::Scan);
}

// `required` holds ids of columns the ancestors need from this subtree.
std::unique_ptr<LogicalPlan> prune(std::unique_ptr<LogicalPlan> n, std::set<std::string> required,
                                   const Catalog& catalog, bool belowJoin) {
    if (isLeafUnit(*n)) {
        if (!belowJoin) return n;
        const LogicalPlan& scan = n->kind == LogicalPlan::Scan ? *n : *n->child;
        const Schema& s = catalog.get(scan.table).schema;
        std::vector<std::string> kept;
        for (const auto& c : s.cols)
            if (required.count(c.table + "." + c.name)) kept.push_back(c.table + "." + c.name);
        if (kept.size() == s.cols.size()) return n;
        auto p = std::make_unique<LogicalPlan>();
        p->kind = LogicalPlan::Project;
        p->columns = std::move(kept);
        p->child = std::move(n);  // Project sits above the Filter so index scans stay applicable
        return p;
    }
    switch (n->kind) {
        case LogicalPlan::Project: {
            Schema in = planSchema(*n->child, catalog);
            std::set<std::string> need;
            for (const auto& c : n->columns) need.insert(columnId(in, c));
            n->child = prune(std::move(n->child), std::move(need), catalog, false);
            break;
        }
        case LogicalPlan::Aggregate: {
            Schema in = planSchema(*n->child, catalog);
            std::set<std::string> need;
            for (const auto& g : n->groupBy) need.insert(columnId(in, g));
            for (const auto& a : n->aggs)
                if (a.column != "*") need.insert(columnId(in, a.column));
            n->child = prune(std::move(n->child), std::move(need), catalog, false);
            break;
        }
        case LogicalPlan::Filter: {  // a filter that stayed above a join
            Schema in = planSchema(*n->child, catalog);
            std::vector<std::string> cols;
            collectColumns(*n->predicate, cols);
            for (const auto& c : cols) required.insert(columnId(in, c));
            n->child = prune(std::move(n->child), std::move(required), catalog, belowJoin);
            break;
        }
        case LogicalPlan::Join: {
            required.insert(columnId(planSchema(*n->child, catalog), n->leftKey));
            required.insert(columnId(planSchema(*n->right, catalog), n->rightKey));
            n->child = prune(std::move(n->child), required, catalog, true);
            n->right = prune(std::move(n->right), required, catalog, true);
            break;
        }
        case LogicalPlan::Scan: break;
    }
    return n;
}

}  // namespace

std::unique_ptr<LogicalPlan> optimize(std::unique_ptr<LogicalPlan> plan, const Catalog& catalog,
                                      const QueryOptions& opts) {
    if (opts.pushdown) plan = pushDown(std::move(plan), {}, catalog);
    if (opts.pruneColumns) plan = prune(std::move(plan), {}, catalog, false);
    return plan;
}
