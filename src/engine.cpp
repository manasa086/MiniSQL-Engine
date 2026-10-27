#include "engine.h"

#include "execution/operator.h"
#include "optimizer/rules.h"
#include "parser/parser.h"
#include "planner/logical_plan.h"

QueryResult runQuery(const std::string& sql, const Catalog& catalog, const QueryOptions& opts) {
    QueryResult res;
    auto plan = buildLogicalPlan(parseSelect(sql), catalog);
    res.plan = plan->toString();
    plan = optimize(std::move(plan), catalog, opts);
    res.optimized = plan->toString();
    auto op = buildOperator(*plan, catalog, opts);
    res.schema = op->schema();
    res.physical = op->explain();
    res.estRows = op->estRows;
    Row row;
    while (op->next(row)) res.rows.push_back(row);
    return res;
}
