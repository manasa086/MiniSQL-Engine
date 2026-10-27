#pragma once
#include <map>
#include <stdexcept>
#include <string>

#include "common/value.h"

// In-memory table; replaced by page-based storage in a later milestone.
struct Table {
    std::string name;
    Schema schema;
    std::vector<Row> rows;
};

class Catalog {
public:
    Table& create(const std::string& name, Schema schema) {
        Table& t = tables_[name];
        t.name = name;
        t.schema = std::move(schema);
        return t;
    }
    const Table& get(const std::string& name) const {
        auto it = tables_.find(name);
        if (it == tables_.end()) throw std::runtime_error("unknown table: " + name);
        return it->second;
    }
    const std::map<std::string, Table>& all() const { return tables_; }

private:
    std::map<std::string, Table> tables_;
};
