#pragma once
#include <memory>

#include "optimizer/options.h"
#include "planner/logical_plan.h"

// Rule-based rewrites of the logical plan (each can be disabled via QueryOptions):
//  - predicate pushdown: WHERE conjuncts move below joins to the side they reference;
//    conjuncts spanning both sides stay above the join.
//  - projection pushdown: below each join input, columns not needed by ancestors
//    (join keys, remaining filters, aggregates, final select list) are dropped so
//    joins and hash tables carry narrower rows.
// Both preserve query results; they only change where work happens.
std::unique_ptr<LogicalPlan> optimize(std::unique_ptr<LogicalPlan> plan, const Catalog& catalog,
                                      const QueryOptions& opts);
