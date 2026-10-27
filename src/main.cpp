#include <algorithm>
#include <iostream>

#include "engine.h"

static Catalog demoCatalog() {
    Catalog c;
    Table& u = c.create("users", {{{"id", Type::Int}, {"name", Type::Text}, {"age", Type::Int}, {"city", Type::Text}}});
    u.rows = {{int64_t(1), "Alice", int64_t(34), "SF"},  {int64_t(2), "Bob", int64_t(25), "NYC"},
              {int64_t(3), "Carol", int64_t(41), "SF"},  {int64_t(4), "Dave", int64_t(30), "LA"},
              {int64_t(5), "Eve", int64_t(52), "NYC"}};
    return c;
}

static void print(const QueryResult& r) {
    for (size_t i = 0; i < r.schema.cols.size(); ++i) std::cout << (i ? " | " : "") << r.schema.cols[i].name;
    std::cout << "\n";
    for (const auto& row : r.rows) {
        for (size_t i = 0; i < row.size(); ++i) std::cout << (i ? " | " : "") << toString(row[i]);
        std::cout << "\n";
    }
    std::cout << "(" << r.rows.size() << " rows)\n";
}

int main() {
    Catalog catalog = demoCatalog();
    std::cout << "MiniSQL Engine. Tables: users(id, name, age, city). Prefix with EXPLAIN to see the plan.\n";
    std::string line;
    while (std::cout << "minisql> " && std::getline(std::cin, line)) {
        if (line.empty()) continue;
        if (line == "exit" || line == "quit") break;
        try {
            bool explain = line.size() > 8 && std::equal(line.begin(), line.begin() + 7, "EXPLAIN",
                                                         [](char a, char b) { return std::toupper(a) == b; });
            QueryResult r = runQuery(explain ? line.substr(8) : line, catalog);
            if (explain) std::cout << r.plan << "\n";
            else print(r);
        } catch (const std::exception& e) {
            std::cout << "error: " << e.what() << "\n";
        }
    }
    return 0;
}
