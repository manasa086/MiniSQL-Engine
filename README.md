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
- [ ] Page-based storage and buffer pool
- [ ] Hash index / B+ tree
- [ ] Hash join, aggregation
- [ ] Optimizer rules
- [ ] Benchmarks

## Build
    make && ./minisql     # REPL with a demo `users` table
    make test
