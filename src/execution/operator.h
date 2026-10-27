#pragma once
#include <memory>

#include "common/value.h"
#include "planner/logical_plan.h"
#include "storage/table.h"

// Volcano-style iterator: next() produces one row at a time.
class Operator {
public:
    virtual ~Operator() = default;
    virtual bool next(Row& row) = 0;
    virtual const Schema& schema() const = 0;
};

// Evaluates an expression to a Value; booleans are int64 0/1.
Value evalExpr(const Expr& e, const Schema& schema, const Row& row);

class SeqScan : public Operator {
public:
    explicit SeqScan(const Table& t) : table_(t) {}
    bool next(Row& row) override;
    const Schema& schema() const override { return table_.schema; }

private:
    const Table& table_;
    size_t pos_ = 0;
};

class Filter : public Operator {
public:
    Filter(std::unique_ptr<Operator> child, const Expr& pred) : child_(std::move(child)), pred_(pred) {}
    bool next(Row& row) override;
    const Schema& schema() const override { return child_->schema(); }

private:
    std::unique_ptr<Operator> child_;
    const Expr& pred_;
};

class Projection : public Operator {
public:
    Projection(std::unique_ptr<Operator> child, const std::vector<std::string>& cols);
    bool next(Row& row) override;
    const Schema& schema() const override { return schema_; }

private:
    std::unique_ptr<Operator> child_;
    std::vector<int> idx_;
    Schema schema_;
};

// Builds an operator tree. The plan must outlive the returned operator.
std::unique_ptr<Operator> buildOperator(const LogicalPlan& plan, const Catalog& catalog);
