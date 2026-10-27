#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <map>

#include "engine.h"
#include "test_util.h"

static const int USERS = 200, ORDERS = 1000, PRODUCTS = 20;

// users(id, name, city) ; orders(oid, user_id, product_id, amount) ; products(id, title)
// user_id cycles over 0..249, so orders with user_id >= 200 have no matching user.
static void fill(Catalog& c) {
    Table& u = c.create("users", {{{"id", Type::Int}, {"name", Type::Text}, {"city", Type::Text}}});
    for (int i = 0; i < USERS; ++i) u.insert({int64_t(i), "user" + std::to_string(i), i % 4 == 0 ? "SF" : "NYC"});
    Table& o = c.create("orders", {{{"oid", Type::Int}, {"user_id", Type::Int}, {"product_id", Type::Int}, {"amount", Type::Int}}});
    for (int i = 0; i < ORDERS; ++i) o.insert({int64_t(i), int64_t(i % 250), int64_t(i % PRODUCTS), int64_t(i % 13 + 1)});
    Table& p = c.create("products", {{{"id", Type::Int}, {"title", Type::Text}}});
    for (int i = 0; i < PRODUCTS; ++i) p.insert({int64_t(i), "item" + std::to_string(i)});
}

static std::vector<Row> sorted(std::vector<Row> rows) {
    std::sort(rows.begin(), rows.end());
    return rows;
}

static bool fails(const std::string& sql, const Catalog& c, const std::string& msgPart = "") {
    try { runQuery(sql, c); } catch (const std::runtime_error& e) {
        return std::string(e.what()).find(msgPart) != std::string::npos;
    }
    return false;
}

static void testJoins(Catalog& c) {
    QueryOptions nl;
    nl.join = JoinAlgorithm::NestedLoop;

    size_t expected = 0;
    for (int i = 0; i < ORDERS; ++i) expected += (i % 250) < USERS;

    const std::string q = "SELECT users.name, orders.amount FROM users JOIN orders ON users.id = orders.user_id";
    auto h = runQuery(q, c);
    auto n = runQuery(q, c, nl);
    CHECK(h.rows.size() == expected);
    CHECK(sorted(h.rows) == sorted(n.rows));  // same result, different algorithm
    CHECK(h.physical.find("HashJoin(users.id = orders.user_id)") != std::string::npos);
    CHECK(n.physical.find("NestedLoopJoin(users.id = orders.user_id)") != std::string::npos);
    CHECK(h.plan.find("Join(users.id = orders.user_id)") != std::string::npos);

    // ON written in the opposite order, INNER keyword, WHERE across both tables
    auto r = runQuery("SELECT oid FROM orders INNER JOIN users ON users.id = orders.user_id "
                      "WHERE users.city = 'SF' AND orders.amount > 10", c);
    auto r2 = runQuery("SELECT oid FROM orders JOIN users ON orders.user_id = users.id "
                       "WHERE users.city = 'SF' AND orders.amount > 10", c, nl);
    size_t cnt = 0;
    for (int i = 0; i < ORDERS; ++i) cnt += (i % 250) < USERS && (i % 250) % 4 == 0 && (i % 13 + 1) > 10;
    CHECK(r.rows.size() == cnt && cnt > 0);
    CHECK(sorted(r.rows) == sorted(r2.rows));

    // SELECT * over a join returns both tables' columns, qualified
    auto star = runQuery("SELECT * FROM users JOIN orders ON users.id = orders.user_id", c);
    CHECK(star.schema.cols.size() == 7 && star.rows.size() == expected);

    // three-way join; the right-hand key may reference any earlier table
    auto three = runQuery("SELECT users.name, products.title FROM users JOIN orders ON users.id = orders.user_id "
                          "JOIN products ON orders.product_id = products.id", c);
    CHECK(three.rows.size() == expected);
    for (const auto& row : three.rows) {
        int uid = std::stoi(std::get<std::string>(row[0]).substr(4));
        CHECK(std::get<std::string>(row[1]).rfind("item", 0) == 0 && uid < USERS);
    }

    CHECK(fails("SELECT id FROM users JOIN products ON users.id = products.id", c, "ambiguous"));
    CHECK(fails("SELECT * FROM users JOIN orders ON users.id = orders.nope", c, "cannot resolve"));
    CHECK(fails("SELECT * FROM users JOIN orders ON users.name = orders.user_id", c, "types differ"));
    CHECK(fails("SELECT * FROM users JOIN users ON users.id = users.id", c, "joined twice"));
    CHECK(fails("SELECT * FROM users JOIN nope ON users.id = nope.id", c, "unknown table"));
}

static void testAggregates(Catalog& c) {
    auto one = [&](const std::string& sql) {
        auto r = runQuery(sql, c);
        CHECK(r.rows.size() == 1);
        return r.rows.at(0);
    };

    int64_t sum = 0, mx = 0, mn = 1000;
    for (int i = 0; i < ORDERS; ++i) { int a = i % 13 + 1; sum += a; mx = std::max<int64_t>(mx, a); mn = std::min<int64_t>(mn, a); }
    Row all = one("SELECT COUNT(*), SUM(amount), MIN(amount), MAX(amount) FROM orders");
    CHECK(all == (Row{int64_t(ORDERS), sum, mn, mx}));
    CHECK(toString(one("SELECT MIN(name) FROM users")[0]) == "user0");
    CHECK(toString(one("SELECT MAX(name) FROM users")[0]) == "user99");  // lexicographic max
    CHECK(one("select count(oid) from orders where amount > 100")[0] == Value(int64_t(0)));

    // GROUP BY with a WHERE and select-list order differing from group order
    auto g = runQuery("SELECT COUNT(*), product_id FROM orders WHERE amount >= 7 GROUP BY product_id", c);
    std::map<int64_t, int64_t> expect;
    for (int i = 0; i < ORDERS; ++i) if (i % 13 + 1 >= 7) ++expect[i % PRODUCTS];
    CHECK(g.rows.size() == expect.size());
    for (const auto& row : g.rows) CHECK(expect[std::get<int64_t>(row[1])] == std::get<int64_t>(row[0]));
    CHECK(g.physical.find("HashAggregate(group by [product_id]; COUNT(*))") != std::string::npos);

    // aggregate over a join, grouped by a column of the other table
    auto j = runQuery("SELECT users.city, COUNT(*), SUM(orders.amount) FROM users JOIN orders ON users.id = orders.user_id "
                      "GROUP BY users.city", c);
    int64_t cntSF = 0, sumSF = 0, cntNY = 0, sumNY = 0;
    for (int i = 0; i < ORDERS; ++i) {
        int uid = i % 250;
        if (uid >= USERS) continue;
        (uid % 4 == 0 ? cntSF : cntNY)++;
        (uid % 4 == 0 ? sumSF : sumNY) += i % 13 + 1;
    }
    CHECK(sorted(j.rows) == sorted({{std::string("NYC"), cntNY, sumNY}, {std::string("SF"), cntSF, sumSF}}));

    // GROUP BY with no aggregate behaves like DISTINCT; multi-column groups
    CHECK(runQuery("SELECT city FROM users GROUP BY city", c).rows.size() == 2);
    CHECK(runQuery("SELECT city, id FROM users GROUP BY city, id", c).rows.size() == USERS);

    // empty input
    CHECK(runQuery("SELECT COUNT(*) FROM orders WHERE amount > 100 GROUP BY product_id", c).rows.empty());
    Row zero = one("SELECT COUNT(*), SUM(amount) FROM orders WHERE amount > 100");
    CHECK(zero == (Row{int64_t(0), int64_t(0)}));
    CHECK(runQuery("SELECT MIN(amount) FROM orders WHERE amount > 100", c).rows.empty());

    CHECK(fails("SELECT name, COUNT(*) FROM users", c, "GROUP BY"));
    CHECK(fails("SELECT name FROM users GROUP BY city", c, "GROUP BY"));
    CHECK(fails("SELECT SUM(name) FROM users", c, "SUM needs"));
    CHECK(fails("SELECT * FROM users GROUP BY city", c, "GROUP BY"));
    CHECK(fails("SELECT SUM(*) FROM users", c, "COUNT"));
    CHECK(fails("SELECT AVG(amount) FROM orders", c, "unknown function"));
    CHECK(fails("SELECT COUNT(nope) FROM orders", c, "unknown column"));
    CHECK(fails("SELECT COUNT(* FROM orders", c, "expected )"));
}

int main() {
    TempDir dir("joinagg");
    Catalog c(dir.path, 16);
    fill(c);
    testJoins(c);
    testAggregates(c);
    if (failures == 0) std::cout << "join/aggregate tests passed\n";
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
