#pragma once
#include <memory>

#include "common/value.h"
#include "planner/logical_plan.h"
#include "storage/table.h"

struct QueryOptions {
    bool useIndexes = true;  // allow the planner to pick B+ tree index scans
};

// Volcano-style iterator: next() produces one row at a time.
class Operator {
public:
    virtual ~Operator() = default;
    virtual bool next(Row& row) = 0;
    virtual const Schema& schema() const = 0;
    // Physical plan as an indented tree, root first.
    virtual std::string explain(int indent = 0) const = 0;
};

// Evaluates an expression to a Value; booleans are int64 0/1.
Value evalExpr(const Expr& e, const Schema& schema, const Row& row);

class SeqScan : public Operator {
public:
    explicit SeqScan(const Table& t) : table_(t), scanner_(t.heap->scan()) {}
    bool next(Row& row) override { return scanner_.next(row); }
    const Schema& schema() const override { return table_.schema; }
    std::string explain(int indent) const override {
        return std::string(indent, ' ') + "SeqScan(" + table_.name + ")";
    }

private:
    const Table& table_;
    TableHeap::Scanner scanner_;
};

// Fetches rows whose indexed column lies in [lo, hi] by walking the B+ tree
// and looking each row up in the heap by RowId.
class IndexScan : public Operator {
public:
    IndexScan(const Table& t, const TableIndex& idx, int64_t lo, int64_t hi)
        : table_(t), idx_(idx), lo_(lo), hi_(hi), it_(idx.tree->range(lo, hi)) {}
    bool next(Row& row) override;
    const Schema& schema() const override { return table_.schema; }
    std::string explain(int indent) const override;

private:
    const Table& table_;
    const TableIndex& idx_;
    int64_t lo_, hi_;
    BPlusTree::Iterator it_;
};

class Filter : public Operator {
public:
    Filter(std::unique_ptr<Operator> child, const Expr& pred) : child_(std::move(child)), pred_(pred) {}
    bool next(Row& row) override;
    const Schema& schema() const override { return child_->schema(); }
    std::string explain(int indent) const override {
        return std::string(indent, ' ') + "Filter(" + exprToString(pred_) + ")\n" + child_->explain(indent + 2);
    }

private:
    std::unique_ptr<Operator> child_;
    const Expr& pred_;
};

class Projection : public Operator {
public:
    Projection(std::unique_ptr<Operator> child, const std::vector<std::string>& cols);
    bool next(Row& row) override;
    const Schema& schema() const override { return schema_; }
    std::string explain(int indent) const override;

private:
    std::unique_ptr<Operator> child_;
    std::vector<int> idx_;
    Schema schema_;
};

// Builds an operator tree. The plan must outlive the returned operator.
std::unique_ptr<Operator> buildOperator(const LogicalPlan& plan, const Catalog& catalog, const QueryOptions& opts = {});
