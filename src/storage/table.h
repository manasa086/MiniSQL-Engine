#pragma once
#include <algorithm>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <unordered_set>
#include <vector>
#include <vector>
#include <string>

#include "common/value.h"
#include "index/bplus_tree.h"
#include "storage/buffer_pool.h"
#include "storage/disk_manager.h"
#include "storage/table_heap.h"

// B+ tree over one INT column of a table.
struct TableIndex {
    std::string column;
    int col = -1;
    // How well key order matches heap order, in [0,1]: the fraction of adjacent index
    // entries (in key order) whose rows share a heap page. ~0 for a random column,
    // ~1 when rows were inserted in key order. Set by Table::analyze().
    double clustering = 0;
    std::unique_ptr<DiskManager> disk;
    std::unique_ptr<BPlusTree> tree;  // destroyed before disk
};

struct ColumnStats {
    bool hasRange = false;  // INT columns only
    int64_t min = 0, max = 0;
    uint64_t ndv = 0;  // distinct values
};

// Gathered by Table::analyze(); row and page counts are always read live.
struct TableStats {
    bool analyzed = false;
    std::vector<ColumnStats> cols;
};

struct Table {
    std::string name;
    Schema schema;
    std::unique_ptr<DiskManager> disk;
    std::unique_ptr<TableHeap> heap;
    std::vector<std::unique_ptr<TableIndex>> indexes;
    uint64_t rowCount = 0;
    TableStats stats;

    // Appends the row and keeps every index up to date.
    void insert(const Row& row) {
        RowId rid = heap->insert(row);
        ++rowCount;
        for (auto& idx : indexes) idx->tree->insert(std::get<int64_t>(row[idx->col]), rid.pack());
    }

    // Full scan computing per-column min/max/distinct counts for the optimizer.
    void analyze() {
        size_t n = schema.cols.size();
        std::vector<ColumnStats> cs(n);
        std::vector<std::unordered_set<Value, ValueHash>> distinct(n);
        auto sc = heap->scan();
        Row row;
        uint64_t count = 0;
        while (sc.next(row)) {
            ++count;
            for (size_t i = 0; i < n; ++i) {
                distinct[i].insert(row[i]);
                if (auto* v = std::get_if<int64_t>(&row[i])) {
                    if (!cs[i].hasRange) { cs[i].hasRange = true; cs[i].min = cs[i].max = *v; }
                    cs[i].min = std::min(cs[i].min, *v);
                    cs[i].max = std::max(cs[i].max, *v);
                }
            }
        }
        for (size_t i = 0; i < n; ++i) cs[i].ndv = distinct[i].size();
        for (auto& idx : indexes) {
            auto it = idx->tree->range(std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max());
            int64_t key;
            uint64_t rid, prevPage = 0, pairs = 0, same = 0;
            bool first = true;
            while (it.next(key, rid)) {
                uint64_t page = rid >> 16;
                if (!first) { ++pairs; same += page == prevPage; }
                prevPage = page;
                first = false;
            }
            idx->clustering = pairs ? static_cast<double>(same) / pairs : 1.0;
        }
        rowCount = count;
        stats.cols = std::move(cs);
        stats.analyzed = true;
    }
    const TableIndex* findIndex(const std::string& column) const {
        for (auto& idx : indexes)
            if (idx->column == column) return idx.get();
        return nullptr;
    }
};

// Owns the buffer pool and one heap file per table (<dir>/<name>.tbl).
class Catalog {
public:
    explicit Catalog(std::string dir, size_t poolFrames = 64) : dir_(std::move(dir)), pool_(poolFrames) {
        std::filesystem::create_directories(dir_);
    }
    ~Catalog() {
        try { pool_.flushAll(); } catch (...) {}
    }
    Catalog(const Catalog&) = delete;
    Catalog& operator=(const Catalog&) = delete;

    // Creates an empty table, discarding any existing file of the same name.
    Table& create(const std::string& name, Schema schema) {
        for (auto& c : schema.cols) c.table = name;
        auto t = std::make_unique<Table>();
        t->name = name;
        t->schema = schema;
        t->disk = std::make_unique<DiskManager>(dir_ + "/" + name + ".tbl", true);
        t->heap = std::make_unique<TableHeap>(pool_, *t->disk, std::move(schema));
        Table& ref = *t;
        tables_[name] = std::move(t);
        return ref;
    }
    // Builds a B+ tree on an INT column, backfilling from existing rows.
    void createIndex(const std::string& tableName, const std::string& column) {
        auto it = tables_.find(tableName);
        if (it == tables_.end()) throw std::runtime_error("unknown table: " + tableName);
        Table& t = *it->second;
        int col = t.schema.indexOf(column);
        if (col < 0) throw std::runtime_error("unknown column: " + column);
        if (t.schema.cols[col].type != Type::Int) throw std::runtime_error("can only index INT columns");
        if (t.findIndex(column)) throw std::runtime_error("index already exists on " + column);

        auto idx = std::make_unique<TableIndex>();
        idx->column = column;
        idx->col = col;
        idx->disk = std::make_unique<DiskManager>(dir_ + "/" + tableName + "." + column + ".idx", true);
        idx->tree = std::make_unique<BPlusTree>(pool_, *idx->disk);
        auto sc = t.heap->scan();
        Row row;
        RowId rid{};
        while (sc.next(row, &rid)) idx->tree->insert(std::get<int64_t>(row[col]), rid.pack());
        t.indexes.push_back(std::move(idx));
        t.analyze();  // the optimizer needs statistics to cost this index
    }
    void analyze(const std::string& tableName) { mutableTable(tableName).analyze(); }
    void analyzeAll() {
        for (auto& kv : tables_) kv.second->analyze();
    }
    const Table& get(const std::string& name) const {
        auto it = tables_.find(name);
        if (it == tables_.end()) throw std::runtime_error("unknown table: " + name);
        return *it->second;
    }
    BufferPool& pool() { return pool_; }

private:
    Table& mutableTable(const std::string& name) {
        auto it = tables_.find(name);
        if (it == tables_.end()) throw std::runtime_error("unknown table: " + name);
        return *it->second;
    }

    std::string dir_;
    BufferPool pool_;  // declared before tables_ so it outlives them
    std::map<std::string, std::unique_ptr<Table>> tables_;
};
