#pragma once
#include <string>
#include <vector>

#include "execution/operator.h"
#include "storage/table.h"

struct QueryResult {
    Schema schema;
    std::vector<Row> rows;
    std::string plan;      // logical plan
    std::string physical;  // physical plan (scan choice), for EXPLAIN
};

// Parse -> logical plan -> execute. Throws std::runtime_error on bad SQL.
QueryResult runQuery(const std::string& sql, const Catalog& catalog, const QueryOptions& opts = {});
