#pragma once
#include <optional>

#include "optimizer/access_path.h"
#include "optimizer/options.h"
#include "planner/logical_plan.h"

// Per-operation costs in microseconds on the machine they were calibrated on
// (`make bench` -> `--calibrate` measures and prints these). Only the ratios
// matter to plan choice; absolute values just keep the estimates interpretable.
struct CostParams {
    // Defaults come from `build/bench_run --calibrate` (200k-row table, 2 MB buffer pool, Apple M-series laptop).
    double seqPerRow = 0.18;                  // scan + decode + evaluate a range predicate on one heap row
    double indexNodeVisit = 0.33;             // load + decode one B+ tree node
    double indexFetchPerRow = 1.1;            // B+ tree entry -> heap fetch by RowId -> decode, per matched row (random heap order)
    double indexFetchClusteredPerRow = 0.24;  // same, when key order matches heap order (see TableIndex::clustering)
    double hashBuildPerRow = 0.09;            // insert one row into the hash table
    double hashProbePerRow = 0.02;            // look up one row (measured fit was ~0; floored at a plausible lookup cost)
    double nlBufferPerRow = 0.001;            // buffer one inner row
    double nlComparePerPair = 0.0011;         // compare one (outer, inner) pair
};
CostParams& costParams();  // mutable so the benchmark harness can override it

// Cost model. Row counts are live; selectivities use per-column statistics from
// Table::analyze() (min/max/distinct, uniform-distribution assumption) and fall
// back to fixed defaults for unanalyzed tables.

// Estimated output cardinality of a logical plan node.
double estimateRows(const LogicalPlan& plan, const Catalog& catalog);

// Fraction of rows of `schema` expected to satisfy `pred`, in [0, 1].
double selectivity(const Expr& pred, const Schema& schema, const Catalog& catalog);

struct ScanCosts {
    double seq = 0, index = 0;
};
// Estimated cost of scanning `table` vs. fetching `matchedRows` rows through `index`.
ScanCosts scanCosts(const Table& table, const TableIndex& index, double matchedRows);
// Fraction of the table at which the two plans cost the same (1.0 if the index never loses).
double modelCrossoverSelectivity(const Table& table, const TableIndex& index);

struct ScanDecision {
    std::optional<IndexRange> range;  // set when an index scan should be used
    double seqCost = 0, indexCost = 0;  // microseconds; 0 if no index applies
    double rangeRows = 0;               // estimated rows inside the index range
};
ScanDecision decideScan(const Expr& pred, const Table& table, const QueryOptions& opts);

struct JoinDecision {
    bool hash = true;
    bool buildLeft = false;  // hash join: build the table on the (smaller) left input
    double hashCost = 0, nestedLoopCost = 0;
};
JoinDecision decideJoin(double leftRows, double rightRows, const QueryOptions& opts);
