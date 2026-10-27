#pragma once
#include <cstdint>
#include <optional>

#include "parser/ast.h"
#include "storage/table.h"

struct IndexRange {
    const TableIndex* index;
    int64_t lo, hi;  // inclusive; lo > hi means the range is empty
};

// Looks at the AND-conjuncts of `pred` for comparisons of an indexed INT column
// against an integer literal (=, <, <=, >, >=) and folds them into one key range.
// Returns nothing if no index applies (e.g. the predicate is an OR, or uses !=).
// The caller must still apply the full predicate; the range only narrows the scan.
std::optional<IndexRange> chooseIndexRange(const Expr& pred, const Table& table);
