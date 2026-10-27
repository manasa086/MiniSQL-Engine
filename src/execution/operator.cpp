#include "execution/operator.h"

#include <stdexcept>

#include "optimizer/access_path.h"
#include "optimizer/cost.h"

Value evalExpr(const Expr& e, const Schema& schema, const Row& row) {
    auto truthy = [](const Value& v) { return std::holds_alternative<int64_t>(v) && std::get<int64_t>(v) != 0; };
    switch (e.kind) {
        case Expr::Literal: return e.lit;
        case Expr::ColumnRef: return row[schema.indexOf(e.col)];
        case Expr::Not: return int64_t(!truthy(evalExpr(*e.left, schema, row)));
        case Expr::Binary: {
            if (e.op == "AND")
                return int64_t(truthy(evalExpr(*e.left, schema, row)) && truthy(evalExpr(*e.right, schema, row)));
            if (e.op == "OR")
                return int64_t(truthy(evalExpr(*e.left, schema, row)) || truthy(evalExpr(*e.right, schema, row)));
            int c = compare(evalExpr(*e.left, schema, row), evalExpr(*e.right, schema, row));
            if (e.op == "=") return int64_t(c == 0);
            if (e.op == "!=") return int64_t(c != 0);
            if (e.op == "<") return int64_t(c < 0);
            if (e.op == "<=") return int64_t(c <= 0);
            if (e.op == ">") return int64_t(c > 0);
            if (e.op == ">=") return int64_t(c >= 0);
            throw std::runtime_error("unknown operator: " + e.op);
        }
    }
    throw std::runtime_error("bad expression");
}

bool Filter::next(Row& row) {
    while (child_->next(row)) {
        const Value v = evalExpr(pred_, child_->schema(), row);
        if (std::holds_alternative<int64_t>(v) && std::get<int64_t>(v) != 0) return true;
    }
    return false;
}

Projection::Projection(std::unique_ptr<Operator> child, const std::vector<std::string>& cols)
    : child_(std::move(child)) {
    for (const auto& c : cols) {
        int i = child_->schema().indexOf(c);
        if (i < 0) throw std::runtime_error("unknown column: " + c);
        idx_.push_back(i);
        schema_.cols.push_back(child_->schema().cols[i]);
    }
}

bool Projection::next(Row& row) {
    Row in;
    if (!child_->next(in)) return false;
    row.clear();
    for (int i : idx_) row.push_back(std::move(in[i]));
    return true;
}

bool IndexScan::next(Row& row) {
    int64_t key;
    uint64_t rid;
    while (it_.next(key, rid))
        if (table_.heap->get(RowId::unpack(rid), row)) return true;
    return false;
}

std::string IndexScan::explain(int indent) const {
    auto bound = [](int64_t v) { return std::to_string(v); };
    return std::string(indent, ' ') + "IndexScan(" + table_.name + "." + idx_.column + " in [" + bound(lo_) + ", " +
           bound(hi_) + "])" + est();
}

std::string Projection::explain(int indent) const {
    std::string s = std::string(indent, ' ') + "Projection(";
    for (size_t i = 0; i < schema_.cols.size(); ++i) s += (i ? ", " : "") + schema_.cols[i].name;
    return s + ")" + est() + "\n" + child_->explain(indent + 2);
}

namespace {

Schema concat(const Schema& a, const Schema& b) {
    Schema s = a;
    s.cols.insert(s.cols.end(), b.cols.begin(), b.cols.end());
    return s;
}

int resolveKey(const Schema& schema, const std::string& name) {
    int i = schema.indexOf(name);
    if (i < 0) throw std::runtime_error("unknown join column: " + name);
    return i;
}

Row concatRows(const Row& a, const Row& b) {
    Row r;
    r.reserve(a.size() + b.size());
    r.insert(r.end(), a.begin(), a.end());
    r.insert(r.end(), b.begin(), b.end());
    return r;
}

std::string joinExplain(const std::string& head, int indent, const Operator& left, const Operator& right) {
    return std::string(indent, ' ') + head + "\n" + left.explain(indent + 2) + "\n" + right.explain(indent + 2);
}

}  // namespace

HashJoin::HashJoin(std::unique_ptr<Operator> left, std::unique_ptr<Operator> right, const std::string& leftKey,
                   const std::string& rightKey, bool buildLeft)
    : left_(std::move(left)), right_(std::move(right)), leftKeyName_(leftKey), rightKeyName_(rightKey),
      lk_(resolveKey(left_->schema(), leftKey)), rk_(resolveKey(right_->schema(), rightKey)), buildLeft_(buildLeft),
      schema_(concat(left_->schema(), right_->schema())) {}

void HashJoin::build() {
    Operator& in = buildLeft_ ? *left_ : *right_;
    const int key = buildLeft_ ? lk_ : rk_;
    Row r;
    while (in.next(r)) table_[r[key]].push_back(r);
    built_ = true;
}

bool HashJoin::next(Row& row) {
    if (!built_) build();
    Operator& probe = buildLeft_ ? *right_ : *left_;
    const int probeKey = buildLeft_ ? rk_ : lk_;
    for (;;) {
        if (matches_ && matchPos_ < matches_->size()) {
            const Row& m = (*matches_)[matchPos_++];
            row = buildLeft_ ? concatRows(m, probeRow_) : concatRows(probeRow_, m);
            return true;
        }
        if (!probe.next(probeRow_)) return false;
        auto it = table_.find(probeRow_[probeKey]);
        matches_ = it == table_.end() ? nullptr : &it->second;
        matchPos_ = 0;
    }
}

std::string HashJoin::explain(int indent) const {
    return joinExplain("HashJoin(" + leftKeyName_ + " = " + rightKeyName_ + ") build=" +
                           (buildLeft_ ? "left" : "right") + est(),
                       indent, *left_, *right_);
}

NestedLoopJoin::NestedLoopJoin(std::unique_ptr<Operator> left, std::unique_ptr<Operator> right,
                               const std::string& leftKey, const std::string& rightKey)
    : left_(std::move(left)), right_(std::move(right)), leftKeyName_(leftKey), rightKeyName_(rightKey),
      lk_(resolveKey(left_->schema(), leftKey)), rk_(resolveKey(right_->schema(), rightKey)),
      schema_(concat(left_->schema(), right_->schema())) {}

bool NestedLoopJoin::next(Row& row) {
    if (!built_) {
        Row r;
        while (right_->next(r)) rightRows_.push_back(r);
        built_ = true;
    }
    for (;;) {
        if (!haveLeft_) {
            if (!left_->next(leftRow_)) return false;
            haveLeft_ = true;
            rightPos_ = 0;
        }
        while (rightPos_ < rightRows_.size()) {
            const Row& rr = rightRows_[rightPos_++];
            if (compare(leftRow_[lk_], rr[rk_]) == 0) {
                row = concatRows(leftRow_, rr);
                return true;
            }
        }
        haveLeft_ = false;
    }
}

std::string NestedLoopJoin::explain(int indent) const {
    return joinExplain("NestedLoopJoin(" + leftKeyName_ + " = " + rightKeyName_ + ")" + est(), indent, *left_, *right_);
}

HashAggregate::HashAggregate(std::unique_ptr<Operator> child, const std::vector<std::string>& groupBy,
                             const std::vector<AggSpec>& aggs)
    : child_(std::move(child)), groupNames_(groupBy), aggs_(aggs) {
    const Schema& in = child_->schema();
    for (const auto& g : groupBy) {
        int i = in.indexOf(g);
        if (i < 0) throw std::runtime_error("unknown column: " + g);
        groupIdx_.push_back(i);
        schema_.cols.push_back(in.cols[i]);
    }
    for (const auto& a : aggs) {
        int i = -1;
        Type t = Type::Int;
        if (a.column != "*") {
            i = in.indexOf(a.column);
            if (i < 0) throw std::runtime_error("unknown column: " + a.column);
            if (a.fn == "SUM" && in.cols[i].type != Type::Int) throw std::runtime_error("SUM needs an INT column");
            if (a.fn == "MIN" || a.fn == "MAX") t = in.cols[i].type;
        }
        aggIdx_.push_back(i);
        schema_.cols.push_back({a.name(), t, ""});
    }
}

void HashAggregate::compute() {
    struct AggState {
        int64_t count = 0, sum = 0;
        Value best;
        bool has = false;
    };
    struct Group {
        Row key;
        std::vector<AggState> states;
    };
    std::vector<Group> groups;
    std::unordered_map<std::string, size_t> index;

    Row row;
    while (child_->next(row)) {
        std::string k;
        for (int i : groupIdx_) {
            if (auto* n = std::get_if<int64_t>(&row[i])) k += "i" + std::to_string(*n) + ";";
            else {
                const auto& str = std::get<std::string>(row[i]);
                k += "s" + std::to_string(str.size()) + ":" + str;
            }
        }
        auto [it, inserted] = index.emplace(std::move(k), groups.size());
        if (inserted) {
            Group g;
            for (int i : groupIdx_) g.key.push_back(row[i]);
            g.states.resize(aggs_.size());
            groups.push_back(std::move(g));
        }
        Group& g = groups[it->second];
        for (size_t a = 0; a < aggs_.size(); ++a) {
            AggState& st = g.states[a];
            const std::string& fn = aggs_[a].fn;
            if (fn == "COUNT") {
                ++st.count;
            } else if (fn == "SUM") {
                st.sum += std::get<int64_t>(row[aggIdx_[a]]);
            } else {
                const Value& v = row[aggIdx_[a]];
                int c = st.has ? compare(v, st.best) : 0;
                if (!st.has || (fn == "MIN" ? c < 0 : c > 0)) st.best = v;
                st.has = true;
            }
        }
    }

    if (groups.empty() && groupIdx_.empty()) {
        bool allZeroable = true;
        for (const auto& a : aggs_) allZeroable = allZeroable && (a.fn == "COUNT" || a.fn == "SUM");
        if (allZeroable) {
            Group g;
            g.states.resize(aggs_.size());
            groups.push_back(std::move(g));
        }
    }

    for (auto& g : groups) {
        Row out = g.key;
        for (size_t a = 0; a < aggs_.size(); ++a) {
            const std::string& fn = aggs_[a].fn;
            if (fn == "COUNT") out.emplace_back(g.states[a].count);
            else if (fn == "SUM") out.emplace_back(g.states[a].sum);
            else out.push_back(g.states[a].best);
        }
        results_.push_back(std::move(out));
    }
    done_ = true;
}

bool HashAggregate::next(Row& row) {
    if (!done_) compute();
    if (pos_ >= results_.size()) return false;
    row = results_[pos_++];
    return true;
}

std::string HashAggregate::explain(int indent) const {
    std::string s = std::string(indent, ' ') + "HashAggregate(group by [";
    for (size_t i = 0; i < groupNames_.size(); ++i) s += (i ? ", " : "") + groupNames_[i];
    s += "]; ";
    for (size_t i = 0; i < aggs_.size(); ++i) s += (i ? ", " : "") + aggs_[i].name();
    return s + ")" + est() + "\n" + child_->explain(indent + 2);
}

namespace {

std::unique_ptr<Operator> build(const LogicalPlan& plan, const Catalog& catalog, const QueryOptions& opts) {
    switch (plan.kind) {
        case LogicalPlan::Scan: return std::make_unique<SeqScan>(catalog.get(plan.table));
        case LogicalPlan::Filter: {
            // Access-path selection: for a predicate directly over a table scan,
            // compare the estimated cost of a sequential scan with an index range
            // scan. The full predicate stays as a residual filter either way.
            if (opts.useIndexes && plan.child->kind == LogicalPlan::Scan) {
                const Table& t = catalog.get(plan.child->table);
                ScanDecision d = decideScan(*plan.predicate, t, opts);
                if (d.range) {
                    auto scan = std::make_unique<IndexScan>(t, *d.range->index, d.range->lo, d.range->hi);
                    scan->estRows = d.rangeRows;
                    return std::make_unique<Filter>(std::move(scan), *plan.predicate);
                }
            }
            return std::make_unique<Filter>(buildOperator(*plan.child, catalog, opts), *plan.predicate);
        }
        case LogicalPlan::Project:
            return std::make_unique<Projection>(buildOperator(*plan.child, catalog, opts), plan.columns);
        case LogicalPlan::Aggregate:
            return std::make_unique<HashAggregate>(buildOperator(*plan.child, catalog, opts), plan.groupBy, plan.aggs);
        case LogicalPlan::Join: {
            JoinDecision d = decideJoin(estimateRows(*plan.child, catalog), estimateRows(*plan.right, catalog), opts);
            auto l = buildOperator(*plan.child, catalog, opts);
            auto r = buildOperator(*plan.right, catalog, opts);
            if (!d.hash)
                return std::make_unique<NestedLoopJoin>(std::move(l), std::move(r), plan.leftKey, plan.rightKey);
            return std::make_unique<HashJoin>(std::move(l), std::move(r), plan.leftKey, plan.rightKey, d.buildLeft);
        }
    }
    throw std::runtime_error("bad plan node");
}

}  // namespace

std::unique_ptr<Operator> buildOperator(const LogicalPlan& plan, const Catalog& catalog, const QueryOptions& opts) {
    auto op = build(plan, catalog, opts);
    op->estRows = estimateRows(plan, catalog);
    return op;
}
