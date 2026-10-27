#include "storage/disk_manager.h"

#include <filesystem>
#include <stdexcept>
#include <vector>

DiskManager::DiskManager(const std::string& path, bool truncate) : path_(path) {
    namespace fs = std::filesystem;
    if (truncate || !fs::exists(path)) std::ofstream(path, std::ios::binary | std::ios::trunc);
    file_.open(path, std::ios::in | std::ios::out | std::ios::binary);
    if (!file_) throw std::runtime_error("cannot open " + path);
    numPages_ = static_cast<uint32_t>(fs::file_size(path) / PAGE_SIZE);
}

void DiskManager::readPage(uint32_t pageId, char* buf) {
    if (pageId >= numPages_) throw std::runtime_error("read past end of " + path_);
    file_.seekg(static_cast<std::streamoff>(pageId) * PAGE_SIZE);
    file_.read(buf, PAGE_SIZE);
    if (!file_) throw std::runtime_error("read failed on " + path_);
}

void DiskManager::writePage(uint32_t pageId, const char* buf) {
    if (pageId >= numPages_) throw std::runtime_error("write past end of " + path_);
    file_.seekp(static_cast<std::streamoff>(pageId) * PAGE_SIZE);
    file_.write(buf, PAGE_SIZE);
    file_.flush();
    if (!file_) throw std::runtime_error("write failed on " + path_);
}

uint32_t DiskManager::allocatePage() {
    std::vector<char> zero(PAGE_SIZE, 0);
    uint32_t id = numPages_++;
    file_.seekp(static_cast<std::streamoff>(id) * PAGE_SIZE);
    file_.write(zero.data(), PAGE_SIZE);
    file_.flush();
    if (!file_) throw std::runtime_error("allocate failed on " + path_);
    return id;
}
