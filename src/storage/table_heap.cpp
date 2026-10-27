#include "storage/table_heap.h"

#include <stdexcept>

#include "storage/page.h"
#include "storage/row_codec.h"

TableHeap::TableHeap(BufferPool& pool, DiskManager& disk, Schema schema)
    : pool_(pool), disk_(disk), fileId_(pool.registerFile(&disk)), schema_(std::move(schema)) {}

RowId TableHeap::insert(const Row& row) {
    std::string rec = serializeRow(row, schema_);
    if (rec.size() > page::MAX_RECORD) throw std::runtime_error("row too large for a page");

    if (disk_.numPages() > 0) {
        uint32_t last = disk_.numPages() - 1;
        char* pg = pool_.fetch(fileId_, last);
        int slot = page::insert(pg, rec.data(), rec.size());
        pool_.unpin(fileId_, last, slot >= 0);
        if (slot >= 0) return {last, static_cast<uint16_t>(slot)};
    }
    uint32_t id;
    char* pg = pool_.newPage(fileId_, id);
    page::init(pg);
    int slot = page::insert(pg, rec.data(), rec.size());
    pool_.unpin(fileId_, id, true);
    return {id, static_cast<uint16_t>(slot)};
}

bool TableHeap::get(RowId rid, Row& row) const {
    if (rid.page >= disk_.numPages()) return false;
    char* pg = pool_.fetch(fileId_, rid.page);
    bool ok = rid.slot < page::numSlots(pg);
    if (ok) {
        const char* data;
        uint16_t len;
        page::get(pg, rid.slot, data, len);
        try {
            row = deserializeRow(data, len, schema_);
        } catch (...) {
            pool_.unpin(fileId_, rid.page, false);
            throw;
        }
    }
    pool_.unpin(fileId_, rid.page, false);
    return ok;
}

TableHeap::Scanner::~Scanner() {
    if (page_) pool_.unpin(fileId_, pageId_, false);
}

bool TableHeap::Scanner::next(Row& row, RowId* rid) {
    for (;;) {
        if (!page_) {
            if (pageId_ >= numPages_) return false;
            page_ = pool_.fetch(fileId_, pageId_);
            slot_ = 0;
        }
        if (slot_ < page::numSlots(page_)) {
            const char* data;
            uint16_t len;
            page::get(page_, slot_, data, len);
            row = deserializeRow(data, len, schema_);
            if (rid) *rid = {pageId_, static_cast<uint16_t>(slot_)};
            ++slot_;
            return true;
        }
        pool_.unpin(fileId_, pageId_, false);
        page_ = nullptr;
        ++pageId_;
    }
}
