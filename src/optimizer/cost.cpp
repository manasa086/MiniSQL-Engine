#include "optimizer/cost.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

constexpr double LEAF_FILL = 127;  // average entries per B+ tree leaf (half-full after splits)

constexpr double DEFAULT_EQ_SEL = 0.01;
constexpr double DEFAULT_RANGE_SEL = 0.33;

double clamp01(double x) { return std::min(1.0, std::max(0.0, x)); }

const ColumnStats* lookupStats(const std::string& name, const Schema& schema, const Catalog& catalog) {
    int i;
    try {
        i = schema.indexOf(name);
    } catch (const std::exception&) {
        return nullptr;
    }
    if (i < 0 || schema.cols[i].table.empty()) return nullptr;
    const Table& t = catalog.get(schema.cols[i].table);
    if (!t.stats.analyzed) return nullptr;
    int j = t.schema.indexOf(schema.cols[i].name);
    return j < 0 ? nullptr : &t.stats.cols[j];
}

// Selectivity of lo <= col <= hi.
double rangeSelectivity(const ColumnStats* s, int64_t lo, int64_t hi) {
    if (lo > hi) return 0;
    if (!s || !s->hasRange) return lo == hi ? DEFAULT_EQ_SEL : DEFAULT_RANGE_SEL;
    double a = std::max<double>(lo, s->min), b = std::min<double>(hi, s->max);
    if (b < a) return 0;
    if (lo == hi) return s->ndv ? 1.0 / s->ndv : DEFAULT_EQ_SEL;
    return clamp01((b - a + 1) / (static_cast<double>(s->max) - s->min + 1));
}

double ndvOfKey(const std::string& key, const LogicalPlan& sub, const Catalog& catalog) {
    double rows = std::max(1.0, estimateRows(sub, catalog));
    const ColumnStats* s = lookupStats(key, planSchema(sub, catalog), catalog);
    return s && s->ndv ? std::min<double>(s->ndv, rows) : rows;
}

std::string flipOp(const std::string& op) {
    if (op == "<") return ">";
    if (op == ">") return "<";
    if (op == "<=") return ">=";
    if (op == ">=") return "<=";
    return op;
}

}  // namespace

double selectivity(const Expr& e, const Schema& schema, const Catalog& catalog) {
    switch (e.kind) {
        case Expr::Not: return clamp01(1 - selectivity(*e.left, schema, catalog));
        case Expr::Literal: return 1;
        case Expr::ColumnRef: return DEFAULT_RANGE_SEL;
        case Expr::Binary: break;
    }
    if (e.op == "AND") return selectivity(*e.left, schema, catalog) * selectivity(*e.right, schema, catalog);
    if (e.op == "OR") {
        double a = selectivity(*e.left, schema, catalog), b = selectivity(*e.right, schema, catalog);
        return clamp01(a + b - a * b);
    }

    const Expr *col = nullptr, *lit = nullptr;
    std::string op = e.op;
    if (e.left->kind == Expr::ColumnRef && e.right->kind == Expr::Literal) {
        col = e.left.get();
        lit = e.right.get();
    } else if (e.left->kind == Expr::Literal && e.right->kind == Expr::ColumnRef) {
        col = e.right.get();
        lit = e.left.get();
        op = flipOp(op);
    } else if (e.left->kind == Expr::ColumnRef && e.right->kind == Expr::ColumnRef) {
        return op == "=" ? 0.1 : DEFAULT_RANGE_SEL;
    } else {
        return DEFAULT_RANGE_SEL;
    }

    const ColumnStats* s = lookupStats(col->col, schema, catalog);
    const int64_t* v = std::get_if<int64_t>(&lit->lit);
    constexpr int64_t MIN = std::numeric_limits<int64_t>::min(), MAX = std::numeric_limits<int64_t>::max();
    if (op == "=" || op == "!=") {
        double eq = s && s->ndv ? 1.0 / s->ndv : DEFAULT_EQ_SEL;
        if (s && s->hasRange && v && (*v < s->min || *v > s->max)) eq = 0;
        return op == "=" ? eq : 1 - eq;
    }
    if (!v) return DEFAULT_RANGE_SEL;
    if (op == "<") return *v == MIN ? 0 : rangeSelectivity(s, MIN, *v - 1);
    if (op == "<=") return rangeSelectivity(s, MIN, *v);
    if (op == ">") return *v == MAX ? 0 : rangeSelectivity(s, *v + 1, MAX);
    if (op == ">=") return rangeSelectivity(s, *v, MAX);
    return DEFAULT_RANGE_SEL;
}

double estimateRows(const LogicalPlan& p, const Catalog& catalog) {
    switch (p.kind) {
        case LogicalPlan::Scan: return static_cast<double>(catalog.get(p.table).rowCount);
        case LogicalPlan::Project: return estimateRows(*p.child, catalog);
        case LogicalPlan::Filter:
            return estimateRows(*p.child, catalog) * selectivity(*p.predicate, planSchema(*p.child, catalog), catalog);
        case LogicalPlan::Join: {
            double l = estimateRows(*p.child, catalog), r = estimateRows(*p.right, catalog);
            double ndv = std::max(ndvOfKey(p.leftKey, *p.child, catalog), ndvOfKey(p.rightKey, *p.right, catalog));
            return l * r / std::max(1.0, ndv);
        }
        case LogicalPlan::Aggregate: {
            double rows = estimateRows(*p.child, catalog);
            if (p.groupBy.empty()) return 1;
            double groups = 1;
            for (const auto& g : p.groupBy) groups *= ndvOfKey(g, *p.child, catalog);
            return std::min(rows, groups);
        }
    }
    return 0;
}

CostParams& costParams() {
    static CostParams params;
    return params;
}

ScanCosts scanCosts(const Table& table, const TableIndex& index, double m) {
    const CostParams& p = costParams();
    const double leafPages = std::max(1.0, std::ceil(m / LEAF_FILL));
    ScanCosts c;
    c.seq = p.seqPerRow * static_cast<double>(table.rowCount);
    const double fetch = (1 - index.clustering) * p.indexFetchPerRow + index.clustering * p.indexFetchClusteredPerRow;
    c.index = p.indexNodeVisit * (index.tree->height() + leafPages) + fetch * m;
    return c;
}

double modelCrossoverSelectivity(const Table& table, const TableIndex& index) {
    const double rows = static_cast<double>(table.rowCount);
    if (rows <= 0) return 1;
    double lo = 0, hi = rows;
    ScanCosts all = scanCosts(table, index, rows);
    if (all.index <= all.seq) return 1;
    for (int i = 0; i < 60; ++i) {
        double mid = (lo + hi) / 2;
        ScanCosts c = scanCosts(table, index, mid);
        (c.index < c.seq ? lo : hi) = mid;
    }
    return lo / rows;
}

ScanDecision decideScan(const Expr& pred, const Table& table, const QueryOptions& opts) {
    ScanDecision d;
    auto range = chooseIndexRange(pred, table);
    if (!range) return d;

    const ColumnStats* s = nullptr;
    if (table.stats.analyzed) s = &table.stats.cols[range->index->col];

    d.rangeRows = static_cast<double>(table.rowCount) * rangeSelectivity(s, range->lo, range->hi);
    ScanCosts c = scanCosts(table, *range->index, d.rangeRows);
    d.seqCost = c.seq;
    d.indexCost = c.index;

    if (opts.forceIndex || d.indexCost < d.seqCost) d.range = range;
    return d;
}

JoinDecision decideJoin(double l, double r, const QueryOptions& opts) {
    const CostParams& p = costParams();
    JoinDecision d;
    const double build = std::min(l, r), probe = std::max(l, r);
    d.hashCost = p.hashBuildPerRow * build + p.hashProbePerRow * probe;
    d.nestedLoopCost = p.nlBufferPerRow * r + p.nlComparePerPair * l * r;
    d.buildLeft = l < r;
    switch (opts.join) {
        case JoinAlgorithm::Hash: d.hash = true; break;
        case JoinAlgorithm::NestedLoop: d.hash = false; break;
        case JoinAlgorithm::Auto: d.hash = d.hashCost <= d.nestedLoopCost; break;
    }
    return d;
}
