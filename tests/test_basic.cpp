#include <cstdlib>
#include <iostream>

#include "engine.h"
#include "test_util.h"

static void fill(Catalog& c) {
    Table& u = c.create("users", {{{"id", Type::Int}, {"name", Type::Text}, {"age", Type::Int}, {"city", Type::Text}}});
    u.insert({int64_t(1), "Alice", int64_t(34), "SF"});
    u.insert({int64_t(2), "Bob", int64_t(25), "NYC"});
    u.insert({int64_t(3), "Carol", int64_t(41), "SF"});
    u.insert({int64_t(4), "Dave", int64_t(30), "LA"});
}

static bool throws(const std::string& sql, const Catalog& c) {
    try { runQuery(sql, c); } catch (const std::runtime_error&) { return true; }
    return false;
}

int main() {
    TempDir dir("basic");
    Catalog c(dir.path);
    fill(c);

    auto r = runQuery("SELECT name FROM users WHERE age > 30", c);
    CHECK(r.rows.size() == 2);
    CHECK(toString(r.rows[0][0]) == "Alice" && toString(r.rows[1][0]) == "Carol");
    CHECK(r.plan == "Project(name)\n  Filter((age > 30))\n    Scan(users)");

    CHECK(runQuery("SELECT * FROM users", c).rows.size() == 4);
    CHECK(runQuery("select id from users where city = 'SF' and age < 40;", c).rows.size() == 1);
    CHECK(runQuery("SELECT id FROM users WHERE city = 'LA' OR age = 25", c).rows.size() == 2);
    CHECK(runQuery("SELECT id FROM users WHERE NOT (city = 'SF')", c).rows.size() == 2);
    CHECK(runQuery("SELECT id FROM users WHERE age != 25 AND (city = 'SF' OR city = 'LA')", c).rows.size() == 3);

    CHECK(throws("SELECT x FROM users", c));
    CHECK(throws("SELECT id FROM nope", c));
    CHECK(throws("SELECT id FROM users WHERE age > 'a'", c));
    CHECK(throws("SELECT FROM users", c));
    CHECK(throws("SELECT id FROM users WHERE", c));

    if (failures == 0) std::cout << "all tests passed\n";
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
