#pragma once
#include <filesystem>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>

#include "common/value.h"
#include "storage/buffer_pool.h"
#include "storage/disk_manager.h"
#include "storage/table_heap.h"

struct Table {
    std::string name;
    Schema schema;
    std::unique_ptr<DiskManager> disk;
    std::unique_ptr<TableHeap> heap;

    void insert(const Row& row) { heap->insert(row); }
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
        auto t = std::make_unique<Table>();
        t->name = name;
        t->schema = schema;
        t->disk = std::make_unique<DiskManager>(dir_ + "/" + name + ".tbl", true);
        t->heap = std::make_unique<TableHeap>(pool_, *t->disk, std::move(schema));
        Table& ref = *t;
        tables_[name] = std::move(t);
        return ref;
    }
    const Table& get(const std::string& name) const {
        auto it = tables_.find(name);
        if (it == tables_.end()) throw std::runtime_error("unknown table: " + name);
        return *it->second;
    }
    BufferPool& pool() { return pool_; }

private:
    std::string dir_;
    BufferPool pool_;  // declared before tables_ so it outlives them
    std::map<std::string, std::unique_ptr<Table>> tables_;
};
