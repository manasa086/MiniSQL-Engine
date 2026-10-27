#include "index/bplus_tree.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace {

constexpr size_t NODE_HEADER = 8;       // [leaf:u8][pad][count:u16][next:u32]
constexpr size_t ENTRY_SIZE = 16;       // key:i64 + rid:u64
constexpr size_t CHILD_OFFSET = NODE_HEADER + ENTRY_SIZE * BPlusTree::MAX_INTERNAL;

void encode(const BPlusTree::Node& n, char* pg) {
    std::memset(pg, 0, PAGE_SIZE);
    pg[0] = n.leaf ? 1 : 0;
    page::wr16(pg + 2, static_cast<uint16_t>(n.keys.size()));
    std::memcpy(pg + 4, &n.next, 4);
    for (size_t i = 0; i < n.keys.size(); ++i) {
        std::memcpy(pg + NODE_HEADER + ENTRY_SIZE * i, &n.keys[i].key, 8);
        std::memcpy(pg + NODE_HEADER + ENTRY_SIZE * i + 8, &n.keys[i].rid, 8);
    }
    for (size_t i = 0; i < n.children.size(); ++i) std::memcpy(pg + CHILD_OFFSET + 4 * i, &n.children[i], 4);
}

BPlusTree::Node decode(const char* pg) {
    BPlusTree::Node n;
    n.leaf = pg[0] != 0;
    size_t count = page::rd16(pg + 2);
    std::memcpy(&n.next, pg + 4, 4);
    n.keys.resize(count);
    for (size_t i = 0; i < count; ++i) {
        std::memcpy(&n.keys[i].key, pg + NODE_HEADER + ENTRY_SIZE * i, 8);
        std::memcpy(&n.keys[i].rid, pg + NODE_HEADER + ENTRY_SIZE * i + 8, 8);
    }
    if (!n.leaf) {
        n.children.resize(count + 1);
        for (size_t i = 0; i <= count; ++i) std::memcpy(&n.children[i], pg + CHILD_OFFSET + 4 * i, 4);
    }
    return n;
}

}  // namespace

BPlusTree::BPlusTree(BufferPool& pool, DiskManager& disk, size_t maxLeaf, size_t maxInternal)
    : pool_(pool), fileId_(pool.registerFile(&disk)), maxLeaf_(maxLeaf), maxInternal_(maxInternal) {
    if (maxLeaf < 2 || maxLeaf > MAX_LEAF || maxInternal < 2 || maxInternal > MAX_INTERNAL)
        throw std::invalid_argument("invalid B+ tree fanout");
    if (disk.numPages() == 0) {
        uint32_t meta;
        pool_.newPage(fileId_, meta);  // page 0 is reserved for the meta page
        pool_.unpin(fileId_, meta, true);
        root_ = alloc(Node{});  // empty root leaf
        setRoot(root_);
    } else {
        char* pg = pool_.fetch(fileId_, 0);
        std::memcpy(&root_, pg, 4);
        pool_.unpin(fileId_, 0, false);
    }
}

BPlusTree::Node BPlusTree::load(uint32_t pid) const {
    char* pg = pool_.fetch(fileId_, pid);
    Node n = decode(pg);
    pool_.unpin(fileId_, pid, false);
    return n;
}

void BPlusTree::save(uint32_t pid, const Node& n) {
    char* pg = pool_.fetch(fileId_, pid);
    encode(n, pg);
    pool_.unpin(fileId_, pid, true);
}

uint32_t BPlusTree::alloc(const Node& n) {
    uint32_t pid;
    char* pg = pool_.newPage(fileId_, pid);
    encode(n, pg);
    pool_.unpin(fileId_, pid, true);
    return pid;
}

void BPlusTree::setRoot(uint32_t pid) {
    root_ = pid;
    char* pg = pool_.fetch(fileId_, 0);
    std::memcpy(pg, &pid, 4);
    pool_.unpin(fileId_, 0, true);
}

BPlusTree::Split BPlusTree::insertRec(uint32_t pid, const Entry& e) {
    Node n = load(pid);
    Split out;
    if (n.leaf) {
        n.keys.insert(std::lower_bound(n.keys.begin(), n.keys.end(), e), e);
        if (n.keys.size() <= maxLeaf_) {
            save(pid, n);
            return out;
        }
        size_t mid = n.keys.size() / 2;
        Node right;
        right.keys.assign(n.keys.begin() + mid, n.keys.end());
        right.next = n.next;
        n.keys.resize(mid);
        out.right = alloc(right);
        n.next = out.right;
        out.sep = right.keys.front();
        out.happened = true;
        save(pid, n);
        return out;
    }

    // Child i holds entries >= keys[i-1] and < keys[i].
    size_t i = std::upper_bound(n.keys.begin(), n.keys.end(), e) - n.keys.begin();
    Split s = insertRec(n.children[i], e);
    if (!s.happened) return out;

    n.keys.insert(n.keys.begin() + i, s.sep);
    n.children.insert(n.children.begin() + i + 1, s.right);
    if (n.keys.size() <= maxInternal_) {
        save(pid, n);
        return out;
    }
    size_t mid = n.keys.size() / 2;
    Node right;
    right.leaf = false;
    right.keys.assign(n.keys.begin() + mid + 1, n.keys.end());
    right.children.assign(n.children.begin() + mid + 1, n.children.end());
    out.sep = n.keys[mid];  // moves up, not copied
    n.keys.resize(mid);
    n.children.resize(mid + 1);
    out.right = alloc(right);
    out.happened = true;
    save(pid, n);
    return out;
}

void BPlusTree::insert(int64_t key, uint64_t rid) {
    Split s = insertRec(root_, Entry{key, rid});
    if (!s.happened) return;
    Node root;
    root.leaf = false;
    root.keys = {s.sep};
    root.children = {root_, s.right};
    setRoot(alloc(root));
}

size_t BPlusTree::height() const {
    size_t h = 1;
    Node n = load(root_);
    while (!n.leaf) {
        n = load(n.children.front());
        ++h;
    }
    return h;
}

BPlusTree::Iterator BPlusTree::range(int64_t lo, int64_t hi) const {
    const Entry probe{lo, 0};
    Node n = load(root_);
    while (!n.leaf) {
        size_t i = std::upper_bound(n.keys.begin(), n.keys.end(), probe) - n.keys.begin();
        n = load(n.children[i]);
    }
    Iterator it;
    it.tree_ = this;
    it.hi_ = hi;
    it.pos_ = std::lower_bound(n.keys.begin(), n.keys.end(), probe) - n.keys.begin();
    it.node_ = std::move(n);
    return it;
}

bool BPlusTree::Iterator::next(int64_t& key, uint64_t& rid) {
    for (;;) {
        if (pos_ >= node_.keys.size()) {
            if (node_.next == 0) return false;
            node_ = tree_->load(node_.next);
            pos_ = 0;
            continue;
        }
        const Entry& e = node_.keys[pos_];
        if (e.key > hi_) return false;
        key = e.key;
        rid = e.rid;
        ++pos_;
        return true;
    }
}
