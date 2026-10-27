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

struct SelectStmt {
    bool star = false;
    std::vector<std::string> columns;
    std::string table;
    std::unique_ptr<Expr> where;
};
