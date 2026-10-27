#include "storage/buffer_pool.h"

#include <stdexcept>

BufferPool::BufferPool(size_t numFrames)
    : frames_(numFrames), lruPos_(numFrames), inLru_(numFrames, false) {
    for (size_t i = numFrames; i-- > 0;) free_.push_back(i);
}

int BufferPool::registerFile(DiskManager* dm) {
    files_.push_back(dm);
    return static_cast<int>(files_.size()) - 1;
}

size_t BufferPool::acquireFrame() {
    if (!free_.empty()) {
        size_t f = free_.back();
        free_.pop_back();
        return f;
    }
    if (lru_.empty()) throw std::runtime_error("buffer pool exhausted: all pages are pinned");
    size_t f = lru_.front();
    lru_.pop_front();
    inLru_[f] = false;
    Frame& fr = frames_[f];
    if (fr.dirty) {
        files_[fr.fileId]->writePage(fr.pageId, fr.data.data());
        ++stats_.diskWrites;
    }
    pageTable_.erase(key(fr.fileId, fr.pageId));
    ++stats_.evictions;
    fr.fileId = -1;
    fr.dirty = false;
    return f;
}

char* BufferPool::fetch(int fileId, uint32_t pageId) {
    auto it = pageTable_.find(key(fileId, pageId));
    if (it != pageTable_.end()) {
        ++stats_.hits;
        size_t f = it->second;
        if (inLru_[f]) {
            lru_.erase(lruPos_[f]);
            inLru_[f] = false;
        }
        ++frames_[f].pins;
        return frames_[f].data.data();
    }
    ++stats_.misses;
    size_t f = acquireFrame();
    Frame& fr = frames_[f];
    try {
        files_[fileId]->readPage(pageId, fr.data.data());
    } catch (...) {
        free_.push_back(f);
        throw;
    }
    fr.fileId = fileId;
    fr.pageId = pageId;
    fr.pins = 1;
    fr.dirty = false;
    pageTable_[key(fileId, pageId)] = f;
    return fr.data.data();
}

char* BufferPool::newPage(int fileId, uint32_t& pageId) {
    size_t f = acquireFrame();
    Frame& fr = frames_[f];
    try {
        pageId = files_[fileId]->allocatePage();
    } catch (...) {
        free_.push_back(f);
        throw;
    }
    fr.data.fill(0);
    fr.fileId = fileId;
    fr.pageId = pageId;
    fr.pins = 1;
    fr.dirty = true;
    pageTable_[key(fileId, pageId)] = f;
    return fr.data.data();
}

void BufferPool::unpin(int fileId, uint32_t pageId, bool dirty) {
    auto it = pageTable_.find(key(fileId, pageId));
    if (it == pageTable_.end()) throw std::runtime_error("unpin of page not in pool");
    size_t f = it->second;
    Frame& fr = frames_[f];
    if (fr.pins <= 0) throw std::runtime_error("unpin of unpinned page");
    fr.dirty = fr.dirty || dirty;
    if (--fr.pins == 0) {
        lru_.push_back(f);
        lruPos_[f] = std::prev(lru_.end());
        inLru_[f] = true;
    }
}

void BufferPool::flushAll() {
    for (Frame& fr : frames_) {
        if (fr.fileId >= 0 && fr.dirty) {
            files_[fr.fileId]->writePage(fr.pageId, fr.data.data());
            ++stats_.diskWrites;
            fr.dirty = false;
        }
    }
}
