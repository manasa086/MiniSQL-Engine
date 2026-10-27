#pragma once
#include <cstdint>
#include <fstream>
#include <string>

#include "storage/page.h"

// Reads and writes fixed-size pages of one file.
class DiskManager {
public:
    // truncate=true discards any existing contents.
    DiskManager(const std::string& path, bool truncate);

    void readPage(uint32_t pageId, char* buf);
    void writePage(uint32_t pageId, const char* buf);
    // Extends the file by one zeroed page and returns its id.
    uint32_t allocatePage();
    uint32_t numPages() const { return numPages_; }

private:
    std::fstream file_;
    std::string path_;
    uint32_t numPages_ = 0;
};
