#pragma once
#include <cstdint>
#include <vector>

#include "storage/buffer_pool.h"

// Disk-based B+ tree mapping int64 keys to 64-bit row ids, stored in its own
// file through the buffer pool. Duplicate keys are allowed: entries are ordered
// by (key, rid), so equal keys form a contiguous run. No deletion yet.
//
// File layout: page 0 is a meta page holding the root page id; every other
// page is a leaf or internal node (leaves are chained via `next`).
class BPlusTree {
public:
    static constexpr size_t MAX_LEAF = 254;      // (4096 - 8) / 16
    static constexpr size_t MAX_INTERNAL = 200;  // separators; one more child than keys

    // Opens the tree in `disk`, creating an empty one if the file is empty.
    // Smaller fanouts exist so tests can force deep trees with few keys.
    BPlusTree(BufferPool& pool, DiskManager& disk, size_t maxLeaf = MAX_LEAF, size_t maxInternal = MAX_INTERNAL);

    void insert(int64_t key, uint64_t rid);
    size_t height() const { return height_; }  // 1 = root is a leaf

    struct Entry {
        int64_t key;
        uint64_t rid;
        bool operator<(const Entry& o) const { return key < o.key || (key == o.key && rid < o.rid); }
    };
    struct Node {
        bool leaf = true;
        std::vector<Entry> keys;          // leaf entries, or internal separators
        std::vector<uint32_t> children;   // internal only: keys.size() + 1 page ids
        uint32_t next = 0;                // leaf only: right sibling, 0 = none
    };

    // Iterates entries with lo <= key <= hi in key order.
    class Iterator {
    public:
        bool next(int64_t& key, uint64_t& rid);

    private:
        friend class BPlusTree;
        const BPlusTree* tree_ = nullptr;
        Node node_;
        size_t pos_ = 0;
        int64_t hi_ = 0;
    };
    Iterator range(int64_t lo, int64_t hi) const;

private:
    struct Split {
        bool happened = false;
        Entry sep{};
        uint32_t right = 0;
    };

    Node load(uint32_t pid) const;
    void save(uint32_t pid, const Node& n);
    uint32_t alloc(const Node& n);
    void setRoot(uint32_t pid);
    Split insertRec(uint32_t pid, const Entry& e);

    BufferPool& pool_;
    int fileId_;
    size_t maxLeaf_, maxInternal_;
    uint32_t root_ = 0;
    size_t height_ = 1;
};
