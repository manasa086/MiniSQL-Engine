#pragma once
#include <memory>
#include <string>
#include <vector>

#include "common/value.h"

struct Expr {
    enum Kind { Literal, ColumnRef, Binary, Not } kind;
    Value lit;            // Literal
    std::string col;      // ColumnRef
    std::string op;       // Binary: = != < <= > >= AND OR
    std::unique_ptr<Expr> left, right;  // Binary (both), Not (left)
};

// A select-list entry: a plain column, or an aggregate fn(column) where
// column may be "*" for COUNT(*).
struct SelectItem {
    std::string fn;  // empty, or COUNT / SUM / MIN / MAX
    std::string column;
    std::string name() const { return fn.empty() ? column : fn + "(" + column + ")"; }
};

struct JoinClause {
    std::string table;
    std::string leftCol, rightCol;  // ON leftCol = rightCol
};

struct SelectStmt {
    bool star = false;
    std::vector<SelectItem> items;
    std::string table;
    std::vector<JoinClause> joins;
    std::unique_ptr<Expr> where;
    std::vector<std::string> groupBy;
};
