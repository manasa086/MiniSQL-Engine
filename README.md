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
- [ ] Hash join, aggregation
- [ ] Optimizer rules
- [ ] Benchmarks

## Build
    make && ./minisql     # REPL with a demo `users` table
    make test
