#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <numeric>
#include <random>

#include "engine.h"
#include "index/bplus_tree.h"
#include "test_util.h"

static std::vector<std::pair<int64_t, uint64_t>> collect(const BPlusTree& t, int64_t lo, int64_t hi) {
    std::vector<std::pair<int64_t, uint64_t>> out;
    auto it = t.range(lo, hi);
    int64_t k;
    uint64_t r;
    while (it.next(k, r)) out.emplace_back(k, r);
    return out;
}

static void testSmallFanoutTree() {
    TempDir dir("bpt");
    DiskManager dm(dir.path + "/i.idx", true);
    BufferPool pool(8);
    BPlusTree tree(pool, dm, 4, 4);  // tiny nodes force many splits and several levels

    std::vector<int64_t> keys(2000);
    std::iota(keys.begin(), keys.end(), -1000);
    std::shuffle(keys.begin(), keys.end(), std::mt19937(42));
    for (int64_t k : keys) tree.insert(k, static_cast<uint64_t>(k + 5000));
    CHECK(tree.height() >= 4);

    auto all = collect(tree, INT64_MIN, INT64_MAX);
    CHECK(all.size() == 2000);
    bool ordered = true;
    for (size_t i = 0; i < all.size(); ++i)
        ordered = ordered && all[i].first == static_cast<int64_t>(i) - 1000 &&
                  all[i].second == static_cast<uint64_t>(all[i].first + 5000);
    CHECK(ordered);

    CHECK(collect(tree, 100, 200).size() == 101);
    CHECK(collect(tree, 7, 7).size() == 1);
    CHECK(collect(tree, 5000, 6000).empty());
    CHECK(collect(tree, 10, 9).empty());
    CHECK(collect(tree, -2000, -1000).size() == 1);
}

static void testDuplicates() {
    TempDir dir("dup");
    DiskManager dm(dir.path + "/i.idx", true);
    BufferPool pool(8);
    BPlusTree tree(pool, dm, 4, 4);
    for (uint64_t r = 0; r < 100; ++r) {
        tree.insert(r % 3, r);  // keys 0,1,2 each appear ~33 times, spanning many leaves
    }
    CHECK(collect(tree, 1, 1).size() == 33);
    CHECK(collect(tree, 0, 0).size() == 34);
    CHECK(collect(tree, 0, 2).size() == 100);
}

static void testLargeAndPersistent() {
    TempDir dir("big");
    const std::string path = dir.path + "/i.idx";
    const int N = 100000;
    {
        DiskManager dm(path, true);
        BufferPool pool(16);
        BPlusTree tree(pool, dm);
        for (int i = 0; i < N; ++i) tree.insert((int64_t(i) * 7919) % N, i);  // permutation of 0..N-1
        CHECK(tree.height() == 3);
        CHECK(collect(tree, 0, N).size() == static_cast<size_t>(N));
        pool.flushAll();
    }
    {
        DiskManager dm(path, false);
        BufferPool pool(16);
        BPlusTree tree(pool, dm);
        CHECK(collect(tree, 500, 599).size() == 100);
        tree.insert(N, 1);
        CHECK(collect(tree, N, N).size() == 1);
    }
}

static uint64_t accesses(const BufferPool& p) { return p.stats().hits + p.stats().misses; }

static void testIndexScanQueries() {
    TempDir dir("q");
    Catalog c(dir.path, 32);
    Table& t = c.create("t", {{{"id", Type::Int}, {"grp", Type::Int}, {"name", Type::Text}}});
    for (int i = 0; i < 20000; ++i) t.insert({int64_t(i), int64_t(i % 10), "n" + std::to_string(i)});
    c.createIndex("t", "id");  // backfilled from existing rows
    for (int i = 20000; i < 20100; ++i) t.insert({int64_t(i), int64_t(i % 10), "n" + std::to_string(i)});  // maintained

    QueryOptions off;
    off.useIndexes = false;

    auto same = [&](const std::string& sql, size_t expected) {
        auto a = runQuery(sql, c);
        auto b = runQuery(sql, c, off);
        CHECK(a.rows.size() == expected);
        CHECK(a.rows == b.rows);
        return a;
    };

    auto point = same("SELECT name FROM t WHERE id = 12345", 1);
    CHECK(toString(point.rows[0][0]) == "n12345");
    CHECK(point.physical.find("IndexScan(t.id in [12345, 12345])") != std::string::npos);

    same("SELECT id FROM t WHERE id >= 100 AND id < 110", 10);
    same("SELECT id FROM t WHERE id > 100 AND id <= 110", 10);
    same("SELECT id FROM t WHERE 20095 < id", 4);           // literal on the left
    same("SELECT id FROM t WHERE id = 5 AND id = 6", 0);    // contradictory range
    same("SELECT id FROM t WHERE id > 9223372036854775807", 0);
    same("SELECT id FROM t WHERE id >= 50 AND id < 100 AND grp = 3", 5);  // residual filter
    same("SELECT id FROM t WHERE id >= 20090", 10);
    CHECK(runQuery("SELECT id FROM t WHERE t.id = 9", c).physical.find("IndexScan") != std::string::npos);  // qualified name          // rows inserted after index build

    auto noidx = same("SELECT id FROM t WHERE id < 5 OR id > 20097", 5 + 2);
    CHECK(noidx.physical.find("IndexScan") == std::string::npos);
    CHECK(runQuery("SELECT id FROM t WHERE id != 3", c).physical.find("IndexScan") == std::string::npos);
    CHECK(runQuery("SELECT id FROM t WHERE grp = 3", c).physical.find("SeqScan") != std::string::npos);

    // The point of an index: far fewer page accesses than a full scan.
    c.pool().resetStats();
    runQuery("SELECT id FROM t WHERE id = 777", c);
    uint64_t withIndex = accesses(c.pool());
    c.pool().resetStats();
    runQuery("SELECT id FROM t WHERE id = 777", c, off);
    uint64_t withScan = accesses(c.pool());
    CHECK(withIndex * 20 < withScan);

    bool threw = false;
    try { c.createIndex("t", "name"); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);
}

int main() {
    testSmallFanoutTree();
    testDuplicates();
    testLargeAndPersistent();
    testIndexScanQueries();
    if (failures == 0) std::cout << "index tests passed\n";
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
