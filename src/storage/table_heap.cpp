#include "storage/table_heap.h"

#include <stdexcept>

#include "storage/page.h"
#include "storage/row_codec.h"

TableHeap::TableHeap(BufferPool& pool, DiskManager& disk, Schema schema)
    : pool_(pool), disk_(disk), fileId_(pool.registerFile(&disk)), schema_(std::move(schema)) {}

void TableHeap::insert(const Row& row) {
    std::string rec = serializeRow(row, schema_);
    if (rec.size() > page::MAX_RECORD) throw std::runtime_error("row too large for a page");

    if (disk_.numPages() > 0) {
        uint32_t last = disk_.numPages() - 1;
        char* pg = pool_.fetch(fileId_, last);
        bool ok = page::insert(pg, rec.data(), rec.size()) >= 0;
        pool_.unpin(fileId_, last, ok);
        if (ok) return;
    }
    uint32_t id;
    char* pg = pool_.newPage(fileId_, id);
    page::init(pg);
    page::insert(pg, rec.data(), rec.size());
    pool_.unpin(fileId_, id, true);
}

TableHeap::Scanner::~Scanner() {
    if (page_) pool_.unpin(fileId_, pageId_, false);
}

bool TableHeap::Scanner::next(Row& row) {
    for (;;) {
        if (!page_) {
            if (pageId_ >= numPages_) return false;
            page_ = pool_.fetch(fileId_, pageId_);
            slot_ = 0;
        }
        if (slot_ < page::numSlots(page_)) {
            const char* data;
            uint16_t len;
            page::get(page_, slot_++, data, len);
            row = deserializeRow(data, len, schema_);
            return true;
        }
        pool_.unpin(fileId_, pageId_, false);
        page_ = nullptr;
        ++pageId_;
    }
}
