#pragma once
#include <memory>
#include <string>
#include <vector>

#include "parser/ast.h"
#include "storage/table.h"

struct LogicalPlan {
    enum Kind { Scan, Filter, Project } kind;
    std::string table;                 // Scan
    std::unique_ptr<Expr> predicate;   // Filter
    std::vector<std::string> columns;  // Project
    std::unique_ptr<LogicalPlan> child;

    // Indented tree, root first, e.g. "Project(name)\n  Filter(age > 30)\n    Scan(users)".
    std::string toString(int indent = 0) const;
};

std::string exprToString(const Expr& e);

// Validates table/column names against the catalog and builds
// Project(Filter(Scan)). The Filter node is omitted when there is no WHERE.
std::unique_ptr<LogicalPlan> buildLogicalPlan(SelectStmt stmt, const Catalog& catalog);
