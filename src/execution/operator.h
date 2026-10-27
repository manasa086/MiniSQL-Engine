#pragma once
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/value.h"
#include "optimizer/options.h"
#include "planner/logical_plan.h"
#include "storage/table.h"

// Volcano-style iterator: next() produces one row at a time.
class Operator {
public:
    virtual ~Operator() = default;
    virtual bool next(Row& row) = 0;
    virtual const Schema& schema() const = 0;
    // Physical plan as an indented tree, root first.
    virtual std::string explain(int indent = 0) const = 0;

    double estRows = -1;  // optimizer's row-count estimate; negative = none

protected:
    std::string est() const {
        return estRows < 0 ? "" : " [est=" + std::to_string(static_cast<long long>(estRows + 0.5)) + "]";
    }
};

// Evaluates an expression to a Value; booleans are int64 0/1.
Value evalExpr(const Expr& e, const Schema& schema, const Row& row);

class SeqScan : public Operator {
public:
    explicit SeqScan(const Table& t) : table_(t), scanner_(t.heap->scan()) {}
    bool next(Row& row) override { return scanner_.next(row); }
    const Schema& schema() const override { return table_.schema; }
    std::string explain(int indent) const override {
        return std::string(indent, ' ') + "SeqScan(" + table_.name + ")" + est();
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
        return std::string(indent, ' ') + "Filter(" + exprToString(pred_) + ")" + est() + "\n" +
               child_->explain(indent + 2);
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

// Equi-join. Builds an in-memory hash table on one input (right by default, or
// left when buildLeft), then streams the other input and probes it. Output rows
// are always left columns followed by right columns.
class HashJoin : public Operator {
public:
    HashJoin(std::unique_ptr<Operator> left, std::unique_ptr<Operator> right, const std::string& leftKey,
             const std::string& rightKey, bool buildLeft = false);
    bool next(Row& row) override;
    const Schema& schema() const override { return schema_; }
    std::string explain(int indent) const override;

private:
    void build();

    std::unique_ptr<Operator> left_, right_;
    std::string leftKeyName_, rightKeyName_;
    int lk_, rk_;
    bool buildLeft_;
    Schema schema_;
    std::unordered_map<Value, std::vector<Row>, ValueHash> table_;
    bool built_ = false;
    Row probeRow_;
    const std::vector<Row>* matches_ = nullptr;
    size_t matchPos_ = 0;
};

// Equi-join by comparing every left row with every right row. The right input is
// buffered in memory once (operators cannot be rewound); cost is O(|L| * |R|)
// comparisons, which is what the join benchmarks contrast with HashJoin.
class NestedLoopJoin : public Operator {
public:
    NestedLoopJoin(std::unique_ptr<Operator> left, std::unique_ptr<Operator> right, const std::string& leftKey,
                   const std::string& rightKey);
    bool next(Row& row) override;
    const Schema& schema() const override { return schema_; }
    std::string explain(int indent) const override;

private:
    std::unique_ptr<Operator> left_, right_;
    std::string leftKeyName_, rightKeyName_;
    int lk_, rk_;
    Schema schema_;
    std::vector<Row> rightRows_;
    bool built_ = false, haveLeft_ = false;
    Row leftRow_;
    size_t rightPos_ = 0;
};

// Blocking hash aggregation. Output: group columns, then one column per aggregate,
// groups in first-seen order. With no GROUP BY and empty input it yields one row of
// zeros when every aggregate is COUNT/SUM, and no rows if a MIN/MAX is present
// (there is no NULL to report).
class HashAggregate : public Operator {
public:
    HashAggregate(std::unique_ptr<Operator> child, const std::vector<std::string>& groupBy,
                  const std::vector<AggSpec>& aggs);
    bool next(Row& row) override;
    const Schema& schema() const override { return schema_; }
    std::string explain(int indent) const override;

private:
    void compute();

    std::unique_ptr<Operator> child_;
    std::vector<std::string> groupNames_;
    std::vector<AggSpec> aggs_;
    std::vector<int> groupIdx_, aggIdx_;  // aggIdx_ is -1 for COUNT(*)
    Schema schema_;
    std::vector<Row> results_;
    size_t pos_ = 0;
    bool done_ = false;
};

// Builds an operator tree. The plan must outlive the returned operator.
std::unique_ptr<Operator> buildOperator(const LogicalPlan& plan, const Catalog& catalog, const QueryOptions& opts = {});
