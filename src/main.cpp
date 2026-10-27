#include <algorithm>
#include <iostream>
#include <sstream>

#include "engine.h"

static void loadDemo(Catalog& c) {
    Table& u = c.create("users", {{{"id", Type::Int}, {"name", Type::Text}, {"age", Type::Int}, {"city", Type::Text}}});
    u.insert({int64_t(1), "Alice", int64_t(34), "SF"});
    u.insert({int64_t(2), "Bob", int64_t(25), "NYC"});
    u.insert({int64_t(3), "Carol", int64_t(41), "SF"});
    u.insert({int64_t(4), "Dave", int64_t(30), "LA"});
    u.insert({int64_t(5), "Eve", int64_t(52), "NYC"});
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
    Catalog catalog("data");
    QueryOptions opts;
    loadDemo(catalog);
    std::cout << "MiniSQL Engine. Tables: users(id, name, age, city). Prefix with EXPLAIN to see the plan; .stats shows buffer pool counters;\n.index <table> <int column> builds a B+ tree; .indexscan on|off toggles index use.\n";
    std::string line;
    while (std::cout << "minisql> " && std::getline(std::cin, line)) {
        if (line.empty()) continue;
        if (line == "exit" || line == "quit") break;
        if (line.rfind(".index ", 0) == 0 && line.rfind(".indexscan", 0) != 0) {
            std::istringstream in(line.substr(7));
            std::string table, column;
            in >> table >> column;
            try {
                catalog.createIndex(table, column);
                std::cout << "index created on " << table << "(" << column << ")\n";
            } catch (const std::exception& e) {
                std::cout << "error: " << e.what() << "\n";
            }
            continue;
        }
        if (line.rfind(".indexscan ", 0) == 0) {
            opts.useIndexes = line.substr(11) == "on";
            std::cout << "index scans " << (opts.useIndexes ? "on" : "off") << "\n";
            continue;
        }
        if (line == ".stats") {
            const auto& st = catalog.pool().stats();
            std::cout << "hits=" << st.hits << " misses=" << st.misses << " evictions=" << st.evictions
                      << " disk_writes=" << st.diskWrites << "\n";
            continue;
        }
        try {
            bool explain = line.size() > 8 && std::equal(line.begin(), line.begin() + 7, "EXPLAIN",
                                                         [](char a, char b) { return std::toupper(a) == b; });
            QueryResult r = runQuery(explain ? line.substr(8) : line, catalog, opts);
            if (explain) std::cout << "Logical:\n" << r.plan << "\nPhysical:\n" << r.physical << "\n";
            else print(r);
        } catch (const std::exception& e) {
            std::cout << "error: " << e.what() << "\n";
        }
    }
    return 0;
}
