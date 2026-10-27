#pragma once
#include <string>

#include "parser/ast.h"

// Grammar:
//   SELECT (* | col {, col}) FROM table [WHERE expr] [;]
//   expr := or ; or := and {OR and} ; and := not {AND not}
//   not  := NOT not | cmp ; cmp := primary [(=|!=|<|<=|>|>=) primary]
SelectStmt parseSelect(const std::string& sql);
