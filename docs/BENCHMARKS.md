# Benchmarks

Reproduce: `make bench` (full, ~2 min) or `make bench ARGS=--quick`. Raw numbers are in
[`bench/results/`](../bench/results). `make bench ARGS=--calibrate` re-measures the cost-model constants.
Single machine (Apple-silicon laptop, `-O2`), one thread, 2 MB buffer pool (512 frames) unless noted.

Every query is timed end to end (parse, plan, optimize, execute) as the median of 3-25 runs after a warm-up run.
Test table `facts(id, k, grp, payload)`, ~66 bytes/row: `id` is in insertion order (clustered index),
`k` is a random permutation (unclustered index), `grp` has 100 distinct values.

> **Caveat:** the "disk" is the OS page cache, so a buffer-pool miss costs a memory copy, not a real disk seek.
> Absolute scan-vs-index ratios would be larger on cold storage; the *shapes* (O(N) vs O(log N), crossover points) hold.

## 1. Index scan vs. full scan: point lookup by table size

`SELECT COUNT(*) FROM facts WHERE k = X`

| rows | heap pages | full scan | index scan | speedup |
|---:|---:|---:|---:|---:|
| 10,000 | 173 | 1.17 ms | 0.004 ms | 300x |
| 100,000 | 1,725 | 12.4 ms | 0.003 ms | ~4,000x |
| 1,000,000 | 17,242 | 129 ms | 0.003 ms | ~40,000x |

The scan grows linearly; the index lookup stays flat (B+ tree height 3, 4 buffer-pool misses).

## 2. When is an index *not* worth it? (range scans, 200k rows)

Index range scan vs. sequential scan across selectivities, and whether the optimizer picked the faster one.

* **Unclustered index (`k`)**: the index wins up to **16.2%** of the table, then loses (a random heap fetch per row).
  The cost model predicts **16.3%**. The optimizer picked the faster plan at 13/13 points; worst case 1.03x slower than the best.
* **Clustered index (`id`)**: rows are fetched in heap order, so the index stays ahead until **81.8%**.
  The model predicts 69.9% (conservative); picked the faster plan at 12/13 points, worst case 1.17x.

## 3. Joins

Equal-sized inputs, `SELECT COUNT(*) FROM jl JOIN jr ON jl.id = jr.id`:

| rows each | nested loop | hash | speedup |
|---:|---:|---:|---:|
| 500 | 0.33 ms | 0.09 ms | 4x |
| 2,000 | 4.35 ms | 0.35 ms | 12x |
| 8,000 | 65.9 ms | 1.39 ms | **47x** |

Nested loop is quadratic, hash join linear. For `facts (200k) JOIN dim(R)`, hash join takes 22-23 ms for R = 100 to 10,000
(nested loop: 43 ms at R=100, 226 ms at R=1,000); the optimizer always chose hash and built on the smaller side.

## 4. Optimizer rules

`SELECT dim.label, COUNT(*) FROM facts JOIN dim ON facts.grp = dim.id WHERE facts.id < 2000 GROUP BY dim.label`

| variant | latency | speedup |
|---|---:|---:|
| no rules (filter after join) | 41.2 ms | 1.0x |
| predicate pushdown | 27.4 ms | 1.5x |
| pushdown + column pruning | 27.0 ms | 1.5x |
| all rules + index scan (enabled by pushdown) | 0.48 ms | **86x** |

Pushdown matters most because it lets the filter reach the table scan, where an index can serve it.
Column pruning alone (wide rows through a join, no filter) gave 1.2x.

## 5. Buffer pool (100k rows, 4,000 random index lookups)

Hit ratio grows with pool size (45% at 0.1 MB, 78% at 4 MB, 100% once the working set fits at 16 MB);
a hot-set workload (90% of lookups on 5% of keys) reaches 88% at 4 MB, showing LRU keeping the hot pages resident.
Latency improves ~2x end to end because misses here are OS-cache copies.

## 6. Throughput

Sequential scan ~10 M rows/s (`COUNT(*)`), filtered scan ~7.7 M rows/s, hash aggregation 5.8-7.6 M rows/s.

## Cost-model calibration

The planner's cost constants (microseconds, `CostParams` in `src/optimizer/cost.h`) were measured by `--calibrate`:

| parameter | value | how measured |
|---|---:|---|
| `seqPerRow` | 0.18 | seq scan with a two-term range predicate |
| `indexFetchPerRow` | 1.1 | slope of index-scan time vs. matched rows (unclustered) |
| `indexFetchClusteredPerRow` | 0.24 | same, clustered index |
| `indexNodeVisit` | 0.33 | random B+ tree probes |
| `hashBuildPerRow` / `hashProbePerRow` | 0.09 / 0.02 | least-squares fit of hash-join time minus input scans (probe floored; the fit was noise-limited) |
| `nlBufferPerRow` / `nlComparePerPair` | 0.001 / 0.0011 | least-squares fit of nested-loop time |

What calibration changed:

1. Initial guesses were in page-access units and ignored CPU cost; replaced with measured microseconds.
2. The first sweep showed the model was **3x off on clustered indexes** (it assumed every fetch is a random page).
   Fix: `ANALYZE` now records an index *clustering* statistic (fraction of adjacent keys sharing a heap page) and the
   model interpolates between the random and clustered fetch costs.
3. The first scan-cost calibration used a one-term predicate and under-estimated scan cost; calibrating with the same
   predicate shape as the sweep moved the predicted unclustered crossover from 19% to 16%, matching the measured 16%.

Known gaps: the clustered crossover is still predicted ~12 points early; the model assumes uniform key distribution;
constants are machine-specific (the ratios matter, not the absolute values); no join reordering.
