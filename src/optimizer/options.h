#pragma once

enum class JoinAlgorithm { Auto, Hash, NestedLoop };

// Knobs for the optimizer and physical planner; the defaults are what a normal
// query uses, the rest exist so benchmarks can compare alternative plans.
struct QueryOptions {
    bool pushdown = true;       // push WHERE conjuncts below joins
    bool pruneColumns = true;   // project away columns not needed above a join
    bool useIndexes = true;     // allow index scans (chosen by cost)
    bool forceIndex = false;    // use an applicable index even if a scan looks cheaper
    JoinAlgorithm join = JoinAlgorithm::Auto;  // Auto = cost-based
};
