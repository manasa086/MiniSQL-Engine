#include "engine.h"

#include "execution/operator.h"
#include "parser/parser.h"
#include "planner/logical_plan.h"

QueryResult runQuery(const std::string& sql, const Catalog& catalog, const QueryOptions& opts) {
    auto plan = buildLogicalPlan(parseSelect(sql), catalog);
    auto op = buildOperator(*plan, catalog, opts);
    QueryResult res;
    res.schema = op->schema();
    res.plan = plan->toString();
    res.physical = op->explain();
    Row row;
    while (op->next(row)) res.rows.push_back(row);
    return res;
}
