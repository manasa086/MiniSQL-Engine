#include <cstdlib>
#include <iostream>

#include "engine.h"
#include "storage/buffer_pool.h"
#include "storage/page.h"
#include "storage/table_heap.h"
#include "test_util.h"

static void testSlottedPage() {
    char pg[PAGE_SIZE];
    page::init(pg);
    int n = 0;
    std::string rec(100, 'x');
    while (page::insert(pg, rec.data(), rec.size()) >= 0) ++n;
    CHECK(n == 39);  // (4096 - 4) / (100 + 4)
    CHECK(page::numSlots(pg) == n);
    const char* d;
    uint16_t len;
    page::get(pg, 7, d, len);
    CHECK(len == 100 && std::string(d, len) == rec);
}

static void testBufferPool() {
    TempDir dir("pool");
    DiskManager dm(dir.path + "/f.tbl", true);
    BufferPool pool(4);
    int f = pool.registerFile(&dm);

    for (int i = 0; i < 20; ++i) {
        uint32_t id;
        char* pg = pool.newPage(f, id);
        CHECK(id == static_cast<uint32_t>(i));
        pg[0] = static_cast<char>(i + 1);
        pool.unpin(f, id, true);
    }
    CHECK(pool.stats().evictions >= 16);
    for (int i = 0; i < 20; ++i) {  // data survives eviction
        char* pg = pool.fetch(f, i);
        CHECK(pg[0] == i + 1);
        pool.unpin(f, i, false);
    }

    pool.resetStats();
    for (int i = 0; i < 5; ++i) {
        pool.fetch(f, 19);
        pool.unpin(f, 19, false);
    }
    CHECK(pool.stats().hits == 5 && pool.stats().misses == 0);

    // LRU: touch 16, 17, 18, 19 then re-touch 16; fetching a new page must evict 17.
    for (int i : {16, 17, 18, 19, 16}) { pool.fetch(f, i); pool.unpin(f, i, false); }
    pool.resetStats();
    pool.fetch(f, 0); pool.unpin(f, 0, false);
    pool.fetch(f, 16); pool.unpin(f, 16, false);
    CHECK(pool.stats().misses == 1 && pool.stats().hits == 1);
    pool.fetch(f, 17); pool.unpin(f, 17, false);
    CHECK(pool.stats().misses == 2);  // 17 was the victim

    // Pinning every frame makes further fetches fail.
    for (int i = 0; i < 4; ++i) pool.fetch(f, i);
    bool threw = false;
    try { pool.fetch(f, 10); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);
    for (int i = 0; i < 4; ++i) pool.unpin(f, i, false);
}

static const Schema kSchema{{{"id", Type::Int}, {"name", Type::Text}}};

static void testHeapAndPersistence() {
    TempDir dir("heap");
    const std::string path = dir.path + "/t.tbl";
    const int N = 5000;
    {
        DiskManager dm(path, true);
        BufferPool pool(8);
        TableHeap heap(pool, dm, kSchema);
        for (int i = 0; i < N; ++i) heap.insert({int64_t(i), "row-" + std::to_string(i)});
        CHECK(heap.numPages() > 8);  // forces eviction with an 8-frame pool

        auto sc = heap.scan();
        Row r;
        int count = 0;
        while (sc.next(r)) {
            CHECK(std::get<int64_t>(r[0]) == count);
            CHECK(std::get<std::string>(r[1]) == "row-" + std::to_string(count));
            ++count;
        }
        CHECK(count == N);
        pool.flushAll();
    }
    {  // reopen without truncating
        DiskManager dm(path, false);
        BufferPool pool(8);
        TableHeap heap(pool, dm, kSchema);
        auto sc = heap.scan();
        Row r;
        int count = 0;
        while (sc.next(r)) ++count;
        CHECK(count == N);
        // appending after reopen continues on the last page
        heap.insert({int64_t(N), "tail"});
    }
}

static void testErrors() {
    TempDir dir("err");
    DiskManager dm(dir.path + "/t.tbl", true);
    BufferPool pool(4);
    TableHeap heap(pool, dm, kSchema);
    auto throws = [&](const Row& r) {
        try { heap.insert(r); } catch (const std::runtime_error&) { return true; }
        return false;
    };
    CHECK(throws({int64_t(1)}));                              // wrong arity
    CHECK(throws({std::string("a"), std::string("b")}));      // wrong type
    CHECK(throws({int64_t(1), std::string(5000, 'x')}));      // larger than a page
}

static void testQueryOverDisk() {
    TempDir dir("query");
    Catalog c(dir.path, 4);  // tiny pool: scans must evict
    Table& t = c.create("nums", {{{"n", Type::Int}, {"label", Type::Text}}});
    for (int i = 0; i < 3000; ++i) t.insert({int64_t(i), std::string(40, 'a' + i % 26)});
    auto r = runQuery("SELECT n FROM nums WHERE n >= 2990", c);
    CHECK(r.rows.size() == 10);
    CHECK(c.pool().stats().evictions > 0);
}

int main() {
    testSlottedPage();
    testBufferPool();
    testHeapAndPersistence();
    testErrors();
    testQueryOverDisk();
    if (failures == 0) std::cout << "storage tests passed\n";
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
