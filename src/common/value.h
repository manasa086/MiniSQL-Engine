#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <variant>
#include <vector>

using Value = std::variant<int64_t, std::string>;
using Row = std::vector<Value>;

enum class Type { Int, Text };

struct Column {
    std::string name;
    Type type;
    std::string table = "";  // owning table; empty for computed columns
};

struct Schema {
    std::vector<Column> cols;
    // Resolves "col" or "table.col". Returns -1 if absent; throws if an
    // unqualified name matches more than one column (e.g. after a join).
    int indexOf(const std::string& name) const;
};

struct ValueHash {
    size_t operator()(const Value& v) const {
        return std::visit([](const auto& x) { return std::hash<std::decay_t<decltype(x)>>{}(x); }, v);
    }
};

std::string toString(const Value& v);
// Three-way comparison; throws std::runtime_error on mismatched types.
int compare(const Value& a, const Value& b);
