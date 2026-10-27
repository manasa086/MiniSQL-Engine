#pragma once
#include <memory>
#include <string>
#include <vector>

#include "parser/ast.h"
#include "storage/table.h"

struct AggSpec {
    std::string fn;      // COUNT, SUM, MIN, MAX
    std::string column;  // "*" for COUNT(*)
    std::string name() const { return fn + "(" + column + ")"; }
};

struct LogicalPlan {
    enum Kind { Scan, Filter, Project, Join, Aggregate } kind;
    std::string table;                 // Scan
    std::unique_ptr<Expr> predicate;   // Filter
    std::vector<std::string> columns;  // Project
    std::string leftKey, rightKey;     // Join: equi-join leftKey = rightKey
    std::vector<std::string> groupBy;  // Aggregate
    std::vector<AggSpec> aggs;         // Aggregate
    std::unique_ptr<LogicalPlan> child;
    std::unique_ptr<LogicalPlan> right;  // Join only: right input (child is the left)

    // Indented tree, root first, e.g. "Project(name)\n  Filter(age > 30)\n    Scan(users)".
    std::string toString(int indent = 0) const;
};

std::string exprToString(const Expr& e);

// Columns produced by a plan node (table tags are kept so names stay resolvable).
Schema planSchema(const LogicalPlan& plan, const Catalog& catalog);

// Validates names against the catalog and builds, bottom to top:
//   Scan [Join Scan ...] -> Filter (WHERE) -> Aggregate (GROUP BY / aggregates) -> Project
// Filter and Aggregate are omitted when not needed.
std::unique_ptr<LogicalPlan> buildLogicalPlan(SelectStmt stmt, const Catalog& catalog);
