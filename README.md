# MiniSQL Engine

A small relational query engine in C++17.

Pipeline: SQL -> Parser -> Logical Plan -> Optimizer -> Physical Plan -> Executor -> Storage.

## Planned scope
- SQL subset: SELECT, WHERE, JOIN, GROUP BY
- Rule-based optimizer: predicate/projection pushdown, seq scan vs index scan, join choice
- Operators: scan, filter, projection, hash join, aggregation
- Page-based storage with an LRU buffer pool and a B+ tree index
- Benchmarks of query latency across plans and dataset sizes

## Status
- [x] Milestone 1: in-memory tables, lexer/parser (SELECT/FROM/WHERE with AND/OR/NOT), logical plan, SeqScan/Filter/Projection operators, REPL with `EXPLAIN`
- [x] Milestone 2: 4 KB slotted pages, disk manager, shared LRU buffer pool (pin/unpin, dirty write-back, hit/miss/eviction stats), heap-file tables, `.stats` in the REPL
- [x] Milestone 3: disk-based B+ tree (duplicate keys, range scans, persistent), `IndexScan` operator, access-path selection for `=`/`<`/`<=`/`>`/`>=` on indexed INT columns; `.index <table> <col>` and `.indexscan on|off` in the REPL. Index vs scan is currently a heuristic (use the index whenever a bound exists); cost-based choice comes with the optimizer.
- [x] Milestone 4: `JOIN ... ON a = b` (INNER, multi-table), `HashJoin` and `NestedLoopJoin`, `GROUP BY` with `COUNT/SUM/MIN/MAX` via `HashAggregate`; qualified column names; `.join hash|nl` in the REPL. No aliases or `AVG` yet.
- [x] Milestone 5: rule-based optimizer (predicate pushdown below joins, projection pushdown/column pruning) and a cost model with per-column statistics (`ANALYZE`: min/max/distinct) driving index-vs-sequential scan choice, hash-vs-nested-loop join choice, and hash build-side selection; `EXPLAIN` shows logical, optimized, and physical plans with estimated row counts; `.rules`, `.analyze`, `.indexscan on|off|force`, `.join auto|hash|nl`. Join *reordering* is not implemented.
- [x] Milestone 6: benchmark harness (`make bench`) comparing scan/index/join/optimizer plans across dataset sizes, and a calibrated cost model; results and methodology in [docs/BENCHMARKS.md](docs/BENCHMARKS.md). Headlines: index point lookup ~40,000x faster than a scan at 1M rows, hash join 47x faster than nested loop at 8k x 8k, optimizer's scan choice within 1.2x of the best plan.

## Build
    make && ./minisql     # REPL with a demo `users` table
    make test
    make bench            # benchmarks (~2 min); ARGS=--quick for a fast pass, ARGS=--calibrate to refit the cost model
