#pragma once
#include <array>
#include <cstdint>
#include <list>
#include <unordered_map>
#include <vector>

#include "storage/disk_manager.h"

// Fixed-size page cache shared by all table files, with LRU replacement.
// Pages returned by fetch()/newPage() are pinned until unpin() is called.
class BufferPool {
public:
    struct Stats {
        uint64_t hits = 0, misses = 0, evictions = 0, diskWrites = 0;
    };

    explicit BufferPool(size_t numFrames);

    // The DiskManager must outlive its use by the pool. Returns a file id.
    int registerFile(DiskManager* dm);

    char* fetch(int fileId, uint32_t pageId);
    // Allocates a new zeroed page on disk and pins it.
    char* newPage(int fileId, uint32_t& pageId);
    void unpin(int fileId, uint32_t pageId, bool dirty);
    void flushAll();

    const Stats& stats() const { return stats_; }
    void resetStats() { stats_ = Stats(); }

private:
    struct Frame {
        std::array<char, PAGE_SIZE> data{};
        int fileId = -1;
        uint32_t pageId = 0;
        int pins = 0;
        bool dirty = false;
    };

    static uint64_t key(int fileId, uint32_t pageId) { return (uint64_t(fileId) << 32) | pageId; }
    size_t acquireFrame();

    std::vector<Frame> frames_;
    std::unordered_map<uint64_t, size_t> pageTable_;
    std::list<size_t> lru_;  // unpinned frames; front = least recently used
    std::vector<std::list<size_t>::iterator> lruPos_;
    std::vector<bool> inLru_;
    std::vector<size_t> free_;
    std::vector<DiskManager*> files_;
    Stats stats_;
};
