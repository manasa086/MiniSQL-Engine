#pragma once
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

using Value = std::variant<int64_t, std::string>;
using Row = std::vector<Value>;

enum class Type { Int, Text };

struct Column {
    std::string name;
    Type type;
};

struct Schema {
    std::vector<Column> cols;
    // Returns -1 if the column does not exist.
    int indexOf(const std::string& name) const;
};

std::string toString(const Value& v);
// Three-way comparison; throws std::runtime_error on mismatched types.
int compare(const Value& a, const Value& b);
