#pragma once
#include "common/value.h"
#include "storage/buffer_pool.h"

// Physical location of a row: page id + slot within the page.
struct RowId {
    uint32_t page;
    uint16_t slot;
    uint64_t pack() const { return (uint64_t(page) << 16) | slot; }
    static RowId unpack(uint64_t v) { return {static_cast<uint32_t>(v >> 16), static_cast<uint16_t>(v & 0xFFFF)}; }
};

// Unordered collection of rows stored in slotted pages, accessed via the buffer pool.
class TableHeap {
public:
    TableHeap(BufferPool& pool, DiskManager& disk, Schema schema);

    RowId insert(const Row& row);
    // Returns false if the id does not refer to a stored row.
    bool get(RowId rid, Row& row) const;
    uint32_t numPages() const { return disk_.numPages(); }

    // Sequential scan; keeps the current page pinned until it moves on or is destroyed.
    class Scanner {
    public:
        Scanner(BufferPool& pool, int fileId, const Schema& schema, uint32_t numPages)
            : pool_(pool), fileId_(fileId), schema_(schema), numPages_(numPages) {}
        Scanner(const Scanner&) = delete;
        Scanner& operator=(const Scanner&) = delete;
        ~Scanner();
        // If rid is non-null it receives the location of the returned row.
        bool next(Row& row, RowId* rid = nullptr);

    private:
        BufferPool& pool_;
        int fileId_;
        const Schema& schema_;
        uint32_t numPages_;
        uint32_t pageId_ = 0;
        int slot_ = 0;
        char* page_ = nullptr;
    };

    Scanner scan() const { return Scanner(pool_, fileId_, schema_, disk_.numPages()); }

private:
    BufferPool& pool_;
    DiskManager& disk_;
    int fileId_;
    Schema schema_;
};
