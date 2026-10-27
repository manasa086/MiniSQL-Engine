#include <algorithm>
#include <cstdlib>
#include <iostream>

#include "engine.h"
#include "optimizer/cost.h"
#include "test_util.h"

static std::vector<Row> sorted(std::vector<Row> rows) {
    std::sort(rows.begin(), rows.end());
    return rows;
}
static size_t pos(const std::string& hay, const std::string& needle) { return hay.find(needle); }
static bool has(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

static void fill(Catalog& c) {
    Table& u = c.create("users", {{{"id", Type::Int}, {"name", Type::Text}, {"city", Type::Text}}});
    for (int i = 0; i < 200; ++i) u.insert({int64_t(i), "user" + std::to_string(i), i % 4 == 0 ? "SF" : "NYC"});
    Table& o = c.create("orders", {{{"oid", Type::Int}, {"user_id", Type::Int}, {"product_id", Type::Int}, {"amount", Type::Int}}});
    for (int i = 0; i < 1000; ++i) o.insert({int64_t(i), int64_t(i % 250), int64_t(i % 20), int64_t(i % 13 + 1)});
    Table& p = c.create("products", {{{"id", Type::Int}, {"title", Type::Text}}});
    for (int i = 0; i < 20; ++i) p.insert({int64_t(i), "item" + std::to_string(i)});

    Table& b = c.create("big", {{{"id", Type::Int}, {"grp", Type::Int}, {"name", Type::Text}}});
    for (int i = 0; i < 20000; ++i) b.insert({int64_t(i), int64_t(i % 10), "name-" + std::to_string(i)});
    Table& d = c.create("dim", {{{"id", Type::Int}, {"label", Type::Text}}});
    for (int i = 0; i < 10; ++i) d.insert({int64_t(i), "g" + std::to_string(i)});
    Table& ta = c.create("tiny_a", {{{"k", Type::Int}}});
    Table& tb = c.create("tiny_b", {{{"k", Type::Int}, {"v", Type::Int}}});
    for (int i = 0; i < 3; ++i) { ta.insert({int64_t(i)}); tb.insert({int64_t(i), int64_t(i * 10)}); }

    c.createIndex("big", "id");
    c.createIndex("big", "grp");
    c.analyzeAll();
}

static QueryOptions noRules() {
    QueryOptions o;
    o.pushdown = o.pruneColumns = false;
    return o;
}

static void testPushdown(Catalog& c) {
    const std::string q = "SELECT users.name, orders.amount FROM users JOIN orders ON users.id = orders.user_id "
                          "WHERE users.city = 'SF' AND orders.amount > 10";
    auto on = runQuery(q, c);
    auto off = runQuery(q, c, noRules());
    CHECK(!on.rows.empty());
    CHECK(sorted(on.rows) == sorted(off.rows));
    CHECK(pos(off.optimized, "Filter") < pos(off.optimized, "Join"));  // unoptimized: filter above join
    CHECK(pos(on.optimized, "Filter") > pos(on.optimized, "Join"));    // pushed below
    CHECK(has(on.optimized, "Filter((users.city = 'SF'))") && has(on.optimized, "Filter((orders.amount > 10))"));
    CHECK(on.plan == off.plan);  // `plan` is the pre-optimization tree

    // a conjunct that spans both sides must stay above the join
    auto cross = runQuery("SELECT users.name FROM users JOIN orders ON users.id = orders.user_id "
                          "WHERE users.id < orders.amount AND users.city = 'SF'", c);
    auto crossOff = runQuery("SELECT users.name FROM users JOIN orders ON users.id = orders.user_id "
                             "WHERE users.id < orders.amount AND users.city = 'SF'", c, noRules());
    CHECK(pos(cross.optimized, "Filter((users.id < orders.amount))") < pos(cross.optimized, "Join"));
    CHECK(pos(cross.optimized, "Filter((users.city = 'SF'))") > pos(cross.optimized, "Join"));
    CHECK(sorted(cross.rows) == sorted(crossOff.rows));

    // three-way join: each conjunct lands on its own table
    const std::string q3 = "SELECT users.name FROM users JOIN orders ON users.id = orders.user_id "
                           "JOIN products ON orders.product_id = products.id "
                           "WHERE products.title = 'item3' AND users.city = 'NYC' AND orders.amount >= 5";
    auto t3 = runQuery(q3, c);
    CHECK(sorted(t3.rows) == sorted(runQuery(q3, c, noRules()).rows));
    CHECK(pos(t3.optimized, "Filter((products.title = 'item3'))") > pos(t3.optimized, "Join"));

    // pushdown exposes an index scan that a filter above the join cannot use
    const std::string qi = "SELECT dim.label FROM big JOIN dim ON big.grp = dim.id WHERE big.id = 7";
    auto idx = runQuery(qi, c);
    auto noPush = runQuery(qi, c, noRules());
    CHECK(has(idx.physical, "IndexScan(big.id in [7, 7])"));
    CHECK(!has(noPush.physical, "IndexScan(big.id"));
    CHECK(idx.rows == noPush.rows && idx.rows.size() == 1);
}

static void testPruning(Catalog& c) {
    const std::string q = "SELECT users.name, orders.amount FROM users JOIN orders ON users.id = orders.user_id "
                          "WHERE users.city = 'SF' AND orders.amount > 10";
    auto on = runQuery(q, c);
    CHECK(has(on.optimized, "Project(users.id, users.name)"));         // city only needed below the Project
    CHECK(has(on.optimized, "Project(orders.user_id, orders.amount)"));
    QueryOptions noPrune;
    noPrune.pruneColumns = false;
    auto off = runQuery(q, c, noPrune);
    CHECK(!has(off.optimized, "Project(users.id"));
    CHECK(sorted(on.rows) == sorted(off.rows));

    const std::string qa = "SELECT users.city, SUM(orders.amount) FROM users JOIN orders ON users.id = orders.user_id "
                           "GROUP BY users.city";
    auto agg = runQuery(qa, c);
    CHECK(has(agg.optimized, "Project(users.id, users.city)"));
    CHECK(has(agg.optimized, "Project(orders.user_id, orders.amount)"));
    CHECK(sorted(agg.rows) == sorted(runQuery(qa, c, noRules()).rows));

    // COUNT(*) needs no columns beyond the join keys
    const std::string qc = "SELECT COUNT(*) FROM users JOIN orders ON users.id = orders.user_id";
    auto cnt = runQuery(qc, c);
    CHECK(has(cnt.optimized, "Project(users.id)") && has(cnt.optimized, "Project(orders.user_id)"));
    CHECK(cnt.rows == runQuery(qc, c, noRules()).rows && std::get<int64_t>(cnt.rows[0][0]) == 800);

    // SELECT * needs every column: nothing to prune
    auto star = runQuery("SELECT * FROM users JOIN orders ON users.id = orders.user_id", c);
    CHECK(star.rows.size() == 800 && star.schema.cols.size() == 7);
}

static void testScanChoice(Catalog& c) {
    auto phys = [&](const std::string& where, const QueryOptions& o = {}) {
        return runQuery("SELECT id FROM big WHERE " + where, c, o);
    };
    CHECK(has(phys("id = 5").physical, "IndexScan(big.id in [5, 5])"));
    CHECK(has(phys("id < 20").physical, "IndexScan"));
    CHECK(has(phys("id >= 0").physical, "SeqScan(big)"));       // 100% selectivity: scan wins
    CHECK(has(phys("id < 19000").physical, "SeqScan(big)"));
    CHECK(has(phys("id != 5").physical, "SeqScan(big)"));

    QueryOptions force;
    force.forceIndex = true;
    auto forced = phys("id >= 0", force);
    CHECK(has(forced.physical, "IndexScan") && forced.rows.size() == 20000);
    QueryOptions none;
    none.useIndexes = false;
    CHECK(has(phys("id = 5", none).physical, "SeqScan(big)"));
    CHECK(phys("id < 19000").rows == phys("id < 19000", force).rows);

    // The measured payoff: the chosen plan touches fewer pages than the rejected one.
    auto accesses = [&](const std::string& where, const QueryOptions& o) {
        c.pool().resetStats();
        runQuery("SELECT id FROM big WHERE " + where, c, o);
        return c.pool().stats().hits + c.pool().stats().misses;
    };
    CHECK(accesses("id = 5", {}) * 10 < accesses("id = 5", none));

    // Cardinality estimates track reality on uniform data
    auto within = [](double est, double actual) { return est >= actual / 2 && est <= actual * 2; };
    auto e1 = phys("grp = 3");
    CHECK(e1.rows.size() == 2000 && within(e1.estRows, 2000));
    auto e2 = phys("id < 5000");
    CHECK(e2.rows.size() == 5000 && within(e2.estRows, 5000));
    auto e3 = phys("grp = 3 AND id < 10000");
    CHECK(within(e3.estRows, static_cast<double>(e3.rows.size())));
}

static void testClustering() {
    TempDir dir("cluster");
    Catalog c(dir.path, 64);
    Table& t = c.create("t", {{{"seq", Type::Int}, {"scattered", Type::Int}}});
    for (int i = 0; i < 20000; ++i) t.insert({int64_t(i), int64_t((i * 7919LL) % 20000)});  // 7919 is coprime with 20000
    c.createIndex("t", "seq");
    c.createIndex("t", "scattered");
    const TableIndex* a = t.findIndex("seq");
    const TableIndex* b = t.findIndex("scattered");
    CHECK(a->clustering > 0.95 && b->clustering < 0.1);
    // a clustered index stays profitable to a much larger fraction of the table
    CHECK(modelCrossoverSelectivity(t, *a) > 3 * modelCrossoverSelectivity(t, *b));
}

static void testJoinChoice(Catalog& c) {
    const std::string onUO = "SELECT * FROM users JOIN orders ON users.id = orders.user_id";
    const std::string onOU = "SELECT * FROM orders JOIN users ON orders.user_id = users.id";
    auto uo = runQuery(onUO, c);
    auto ou = runQuery(onOU, c);
    CHECK(has(uo.physical, "HashJoin(users.id = orders.user_id) build=left"));   // build the smaller side
    CHECK(has(ou.physical, "HashJoin(orders.user_id = users.id) build=right"));
    QueryOptions nl;
    nl.join = JoinAlgorithm::NestedLoop;
    CHECK(sorted(uo.rows) == sorted(runQuery(onUO, c, nl).rows));  // build=left keeps column order
    CHECK(sorted(ou.rows) == sorted(runQuery(onOU, c, nl).rows));
    CHECK(uo.rows.size() == 800 && uo.estRows > 640 && uo.estRows < 960);

    auto tiny = runQuery("SELECT tiny_a.k, tiny_b.v FROM tiny_a JOIN tiny_b ON tiny_a.k = tiny_b.k", c);
    CHECK(has(tiny.physical, "NestedLoopJoin"));  // 3x3 is cheaper than building a hash table
    QueryOptions hash;
    hash.join = JoinAlgorithm::Hash;
    auto forced = runQuery("SELECT tiny_a.k, tiny_b.v FROM tiny_a JOIN tiny_b ON tiny_a.k = tiny_b.k", c, hash);
    CHECK(has(forced.physical, "HashJoin"));
    CHECK(sorted(tiny.rows) == sorted(forced.rows) && tiny.rows.size() == 3);

    // Without statistics the planner still works (falls back to default selectivities)
    TempDir dir("nostats");
    Catalog fresh(dir.path, 16);
    Table& t = fresh.create("t", {{{"a", Type::Int}, {"b", Type::Int}}});
    for (int i = 0; i < 500; ++i) t.insert({int64_t(i), int64_t(i % 7)});
    auto r = runQuery("SELECT a FROM t WHERE a < 100 AND b = 3", fresh);
    CHECK(r.rows.size() == 14 && r.estRows > 0);
}

int main() {
    TempDir dir("optimizer");
    Catalog c(dir.path, 64);
    fill(c);
    testPushdown(c);
    testPruning(c);
    testScanChoice(c);
    testClustering();
    testJoinChoice(c);
    if (failures == 0) std::cout << "optimizer tests passed\n";
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
