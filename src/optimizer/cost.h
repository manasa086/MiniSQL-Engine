#pragma once
#include <optional>

#include "optimizer/access_path.h"
#include "optimizer/options.h"
#include "planner/logical_plan.h"

// Cost model. Row counts are live; selectivities use per-column statistics from
// Table::analyze() (min/max/distinct, uniform-distribution assumption) and fall
// back to fixed defaults for unanalyzed tables.

// Estimated output cardinality of a logical plan node.
double estimateRows(const LogicalPlan& plan, const Catalog& catalog);

// Fraction of rows of `schema` expected to satisfy `pred`, in [0, 1].
double selectivity(const Expr& pred, const Schema& schema, const Catalog& catalog);

struct ScanDecision {
    std::optional<IndexRange> range;  // set when an index scan should be used
    double seqCost = 0, indexCost = 0;  // in page-access equivalents; 0 if no index applies
    double rangeRows = 0;               // estimated rows inside the index range
};
ScanDecision decideScan(const Expr& pred, const Table& table, const QueryOptions& opts);

struct JoinDecision {
    bool hash = true;
    bool buildLeft = false;  // hash join: build the table on the (smaller) left input
    double hashCost = 0, nestedLoopCost = 0;
};
JoinDecision decideJoin(double leftRows, double rightRows, const QueryOptions& opts);
