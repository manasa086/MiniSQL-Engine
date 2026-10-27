#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

constexpr size_t PAGE_SIZE = 4096;

// Slotted page layout:
//   [numSlots:u16][freeEnd:u16][slot0 off:u16,len:u16][slot1 ...] ... free ... [records, growing down from freeEnd]
namespace page {

constexpr size_t HEADER = 4;
constexpr size_t SLOT = 4;
constexpr size_t MAX_RECORD = PAGE_SIZE - HEADER - SLOT;

inline uint16_t rd16(const char* p) { uint16_t v; std::memcpy(&v, p, 2); return v; }
inline void wr16(char* p, uint16_t v) { std::memcpy(p, &v, 2); }

inline void init(char* pg) {
    std::memset(pg, 0, PAGE_SIZE);
    wr16(pg + 2, static_cast<uint16_t>(PAGE_SIZE));
}

inline uint16_t numSlots(const char* pg) { return rd16(pg); }

// Returns the slot id, or -1 if the record does not fit.
inline int insert(char* pg, const char* data, size_t len) {
    uint16_t n = numSlots(pg), freeEnd = rd16(pg + 2);
    size_t slotsEnd = HEADER + SLOT * (n + 1);
    if (len > MAX_RECORD || freeEnd < slotsEnd || freeEnd - slotsEnd < len) return -1;
    uint16_t off = static_cast<uint16_t>(freeEnd - len);
    std::memcpy(pg + off, data, len);
    wr16(pg + HEADER + SLOT * n, off);
    wr16(pg + HEADER + SLOT * n + 2, static_cast<uint16_t>(len));
    wr16(pg, n + 1);
    wr16(pg + 2, off);
    return n;
}

inline void get(const char* pg, int slot, const char*& data, uint16_t& len) {
    data = pg + rd16(pg + HEADER + SLOT * slot);
    len = rd16(pg + HEADER + SLOT * slot + 2);
}

}  // namespace page
