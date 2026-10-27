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

    Table& o = c.create("orders", {{{"oid", Type::Int}, {"user_id", Type::Int}, {"amount", Type::Int}}});
    o.insert({int64_t(100), int64_t(1), int64_t(50)});
    o.insert({int64_t(101), int64_t(1), int64_t(20)});
    o.insert({int64_t(102), int64_t(3), int64_t(75)});
    o.insert({int64_t(103), int64_t(5), int64_t(10)});
    o.insert({int64_t(104), int64_t(9), int64_t(99)});  // no matching user
}

static void print(const QueryResult& r) {
    for (size_t i = 0; i < r.schema.cols.size(); ++i) {
        const Column& c = r.schema.cols[i];
        bool dup = false;  // qualify names that appear in more than one table (joins)
        for (size_t j = 0; j < r.schema.cols.size(); ++j) dup = dup || (j != i && r.schema.cols[j].name == c.name);
        std::cout << (i ? " | " : "") << (dup && !c.table.empty() ? c.table + "." : "") << c.name;
    }
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
    catalog.analyzeAll();
    std::cout << "MiniSQL Engine. Tables: users(id, name, age, city), orders(oid, user_id, amount). Prefix with EXPLAIN to see the plan; .stats shows buffer pool counters;\n.index <table> <int column> builds a B+ tree; .indexscan on|off|force, .join auto|hash|nl, .rules on|off, .analyze tune the optimizer.\n";
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
            std::string v = line.substr(11);
            opts.useIndexes = v != "off";
            opts.forceIndex = v == "force";
            std::cout << "index scans: " << (opts.forceIndex ? "forced" : opts.useIndexes ? "cost-based" : "off") << "\n";
            continue;
        }
        if (line.rfind(".rules ", 0) == 0) {
            opts.pushdown = opts.pruneColumns = line.substr(7) != "off";
            std::cout << "pushdown/pruning rules " << (opts.pushdown ? "on" : "off") << "\n";
            continue;
        }
        if (line.rfind(".analyze", 0) == 0) {
            catalog.analyzeAll();
            std::cout << "statistics updated\n";
            continue;
        }
        if (line.rfind(".join ", 0) == 0) {
            std::string v = line.substr(6);
            opts.join = v == "nl" ? JoinAlgorithm::NestedLoop : v == "hash" ? JoinAlgorithm::Hash : JoinAlgorithm::Auto;
            std::cout << "join algorithm: " << (v == "nl" ? "nested loop" : v == "hash" ? "hash" : "cost-based") << "\n";
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
            if (explain)
                std::cout << "Logical:\n" << r.plan << "\nOptimized:\n" << r.optimized << "\nPhysical (est = estimated rows):\n"
                          << r.physical << "\n";
            else print(r);
        } catch (const std::exception& e) {
            std::cout << "error: " << e.what() << "\n";
        }
    }
    return 0;
}
