#pragma once
#include <string>

#include "parser/ast.h"

// Grammar:
//   SELECT (* | item {, item}) FROM table {[INNER] JOIN table ON col = col}
//          [WHERE expr] [GROUP BY col {, col}] [;]
//   item := col | (COUNT|SUM|MIN|MAX) '(' (col | *) ')'   -- * only for COUNT
//   expr := or ; or := and {OR and} ; and := not {AND not}
//   not  := NOT not | cmp ; cmp := primary [(=|!=|<|<=|>|>=) primary]
SelectStmt parseSelect(const std::string& sql);
