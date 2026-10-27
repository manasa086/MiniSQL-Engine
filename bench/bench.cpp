// Benchmark harness for MiniSQL Engine.
//   build/bench_run [--quick] [--pool FRAMES] [--csv DIR] [--calibrate] [scenario ...]
// Scenarios: point, sweep, join, rules, scan, pool   (default: all)
// Every query is timed end to end (parse, plan, optimize, execute) as the median of
// several runs after one warm-up run.
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <numeric>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "engine.h"
#include "optimizer/cost.h"
#include "parser/parser.h"

using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;

struct Args {
    bool quick = false, calibrate = false;
    size_t pool = 512;  // buffer pool frames (4 KB each)
    std::string csvDir;
    std::set<std::string> scenarios;
    bool wants(const std::string& s) const { return scenarios.empty() || scenarios.count(s); }
};
static Args args;
static std::string benchRoot;

// ---------- timing and output helpers ----------

template <class F>
static double timeMs(F&& f) {
    auto t0 = Clock::now();
    f();
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

template <class F>
static double medianMs(F&& f, int reps) {
    f();  // warm-up
    std::vector<double> v;
    for (int i = 0; i < reps; ++i) v.push_back(timeMs(f));
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

static std::string fmt(const char* f, double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, f, v);
    return buf;
}
static std::string fmtMs(double ms) { return fmt(ms < 1 ? "%.3f" : ms < 100 ? "%.2f" : "%.0f", ms); }

struct Report {
    std::string name, title;
    std::vector<std::string> header;
    std::vector<std::vector<std::string>> rows;

    void print() const {
        std::vector<size_t> w(header.size());
        for (size_t i = 0; i < header.size(); ++i) w[i] = header[i].size();
        for (const auto& r : rows)
            for (size_t i = 0; i < r.size(); ++i) w[i] = std::max(w[i], r[i].size());
        std::printf("\n== %s\n", title.c_str());
        auto line = [&](const std::vector<std::string>& r) {
            for (size_t i = 0; i < r.size(); ++i) std::printf("%s%*s", i ? "  " : "", static_cast<int>(w[i]), r[i].c_str());
            std::printf("\n");
        };
        line(header);
        for (const auto& r : rows) line(r);
        std::fflush(stdout);
        if (!args.csvDir.empty()) {
            fs::create_directories(args.csvDir);
            std::ofstream f(args.csvDir + "/" + name + ".csv");
            auto csv = [&](const std::vector<std::string>& r) {
                for (size_t i = 0; i < r.size(); ++i) f << (i ? "," : "") << r[i];
                f << "\n";
            };
            csv(header);
            for (const auto& r : rows) csv(r);
        }
    }
};

// ---------- data generation ----------

// facts(id, k, grp, payload): id is the insertion order (clustered), k a random
// permutation of 0..n-1 (unclustered), grp has 100 distinct values. ~66 bytes/row.
static void buildFacts(Catalog& c, int64_t n) {
    Table& t = c.create("facts", {{{"id", Type::Int}, {"k", Type::Int}, {"grp", Type::Int}, {"payload", Type::Text}}});
    std::vector<int64_t> perm(n);
    std::iota(perm.begin(), perm.end(), 0);
    std::shuffle(perm.begin(), perm.end(), std::mt19937_64(1));
    std::vector<std::string> pad;
    for (int i = 0; i < 26; ++i) pad.emplace_back(40, static_cast<char>('a' + i));
    for (int64_t i = 0; i < n; ++i) t.insert({i, perm[i], i % 100, pad[i % 26]});
    c.createIndex("facts", "id");
    c.createIndex("facts", "k");
}

static void buildDim(Catalog& c, const std::string& name, int64_t n) {
    Table& t = c.create(name, {{{"id", Type::Int}, {"label", Type::Text}}});
    for (int64_t i = 0; i < n; ++i) t.insert({i, "label-" + std::to_string(i)});
    t.analyze();
}

static std::string workDir(const std::string& tag) {
    std::string d = benchRoot + "/" + tag;
    fs::remove_all(d);
    return d;
}

static QueryOptions seqOnly() { QueryOptions o; o.useIndexes = false; return o; }
static QueryOptions forceIdx() { QueryOptions o; o.forceIndex = true; return o; }

static uint64_t poolAccesses(Catalog& c, const std::string& sql, const QueryOptions& o, uint64_t* misses = nullptr) {
    c.pool().resetStats();
    runQuery(sql, c, o);
    if (misses) *misses = c.pool().stats().misses;
    return c.pool().stats().hits + c.pool().stats().misses;
}

static bool usesIndex(Catalog& c, const std::string& sql, const QueryOptions& o = {}) {
    return runQuery(sql, c, o).physical.find("IndexScan") != std::string::npos;
}

// ---------- scenario: point lookups across dataset sizes ----------

static void scenarioPoint() {
    Report r{"point", "Point lookup SELECT COUNT(*) WHERE k = X (unclustered index) vs full scan, by table size",
             {"rows", "pages", "build_s", "seq_ms", "index_ms", "speedup", "seq_misses", "idx_misses"}, {}};
    std::vector<int64_t> sizes = args.quick ? std::vector<int64_t>{10000, 50000} : std::vector<int64_t>{10000, 100000, 1000000};
    for (int64_t n : sizes) {
        Catalog c(workDir("point"), args.pool);
        double build = timeMs([&] { buildFacts(c, n); }) / 1000;
        const std::string q = "SELECT COUNT(*) FROM facts WHERE k = " + std::to_string(n / 3);
        double seq = medianMs([&] { runQuery(q, c, seqOnly()); }, n >= 1000000 ? 3 : 5);
        double idx = medianMs([&] { runQuery(q, c); }, 25);
        uint64_t ms = 0, mi = 0;
        poolAccesses(c, q, seqOnly(), &ms);
        poolAccesses(c, q, {}, &mi);
        r.rows.push_back({std::to_string(n), std::to_string(c.get("facts").heap->numPages()), fmt("%.1f", build),
                          fmtMs(seq), fmtMs(idx), fmt("%.0fx", seq / idx), std::to_string(ms), std::to_string(mi)});
        if (!usesIndex(c, q)) r.rows.back().push_back("(optimizer chose scan!)");
    }
    r.print();
}

// ---------- scenario: selectivity sweep and calibration of the scan cost model ----------

struct SweepPoint {
    double sel, seqMs, idxMs, autoMs;
    bool autoUsedIndex;
};

static std::vector<double> sweepFractions() {
    return args.quick ? std::vector<double>{0.0002, 0.002, 0.01, 0.03, 0.1, 0.3, 1.0}
                      : std::vector<double>{0.00005, 0.0002, 0.001, 0.003, 0.01, 0.02, 0.04, 0.07, 0.1, 0.2, 0.4, 0.7, 1.0};
}

static std::vector<SweepPoint> sweep(Catalog& c, const std::string& col, int64_t n) {
    std::vector<SweepPoint> pts;
    for (double s : sweepFractions()) {
        int64_t m = std::max<int64_t>(1, std::llround(s * n));
        const std::string q = "SELECT COUNT(*) FROM facts WHERE " + col + " >= 0 AND " + col + " < " + std::to_string(m);
        int reps = m * 4 < n ? 7 : 3;
        SweepPoint p;
        p.sel = static_cast<double>(m) / n;
        p.seqMs = medianMs([&] { runQuery(q, c, seqOnly()); }, reps);
        p.idxMs = medianMs([&] { runQuery(q, c, forceIdx()); }, reps);
        p.autoMs = medianMs([&] { runQuery(q, c); }, reps);
        p.autoUsedIndex = usesIndex(c, q);
        pts.push_back(p);
    }
    return pts;
}

// Selectivity where index time first exceeds scan time (log-linear interpolation); 1.0+ if never.
static double measuredCrossover(const std::vector<SweepPoint>& pts) {
    for (size_t i = 1; i < pts.size(); ++i) {
        double a = std::log(pts[i - 1].idxMs / pts[i - 1].seqMs), b = std::log(pts[i].idxMs / pts[i].seqMs);
        if (a < 0 && b >= 0) {
            double t = -a / (b - a);
            return std::exp(std::log(pts[i - 1].sel) + t * (std::log(pts[i].sel) - std::log(pts[i - 1].sel)));
        }
    }
    return pts.back().idxMs < pts.back().seqMs ? 2.0 : 0.0;
}

static void reportSweep(Catalog& c, const std::string& col, int64_t n, const char* tag, const char* what) {
    auto pts = sweep(c, col, n);
    const Table& t = c.get("facts");
    Report r{std::string("sweep_") + tag, std::string("Range scan, ") + what + " (" + std::to_string(n) + " rows): seq vs index vs optimizer's choice",
             {"selectivity", "seq_ms", "index_ms", "faster", "auto_ms", "auto_picked", "regret"}, {}};
    int correct = 0;
    double worstRegret = 1;
    for (const auto& p : pts) {
        bool idxFaster = p.idxMs < p.seqMs;
        double best = std::min(p.seqMs, p.idxMs);
        double regret = p.autoMs / best;
        bool ok = p.autoUsedIndex == idxFaster || regret < 1.15;  // near-ties don't count against the model
        correct += ok;
        worstRegret = std::max(worstRegret, regret);
        r.rows.push_back({fmt("%.4f%%", p.sel * 100).substr(0), fmtMs(p.seqMs), fmtMs(p.idxMs), idxFaster ? "index" : "seq",
                          fmtMs(p.autoMs), p.autoUsedIndex ? "index" : "seq", fmt("%.2fx", regret)});
    }
    r.print();
    const TableIndex* idx = t.findIndex(col);
    double mc = measuredCrossover(pts), model = modelCrossoverSelectivity(t, *idx);
    std::printf("  measured crossover: %s of the table; cost model predicts: %.2f%%; optimizer picked the faster plan (within 15%%) at %d/%zu points\n",
                mc >= 2 ? ">100%" : fmt("%.2f%%", mc * 100).c_str(), model * 100, correct, pts.size());
    std::printf("  worst-case regret of the optimizer's choice: %.2fx slower than the best plan\n", worstRegret);
}

static void scenarioSweep(Catalog& c, int64_t n) {
    reportSweep(c, "k", n, "unclustered", "unclustered index on k (random heap order)");
    reportSweep(c, "id", n, "clustered", "clustered index on id (heap order = key order)");
}

// ---------- scenario: joins ----------

static void scenarioJoin(Catalog& main, int64_t n) {
    {
        Report r{"join_algos", "Equi-join COUNT(*), equal-sized inputs: nested loop vs hash vs optimizer's choice",
                 {"rows_each", "nested_ms", "hash_ms", "hash_speedup", "auto_ms", "auto_picked"}, {}};
        std::vector<int64_t> sizes = args.quick ? std::vector<int64_t>{250, 500, 1000, 2000} : std::vector<int64_t>{500, 1000, 2000, 4000, 8000};
        for (int64_t m : sizes) {
            Catalog c(workDir("join"), args.pool);
            buildDim(c, "jl", m);
            buildDim(c, "jr", m);
            const std::string q = "SELECT COUNT(*) FROM jl JOIN jr ON jl.id = jr.id";
            QueryOptions nl, hash;
            nl.join = JoinAlgorithm::NestedLoop;
            hash.join = JoinAlgorithm::Hash;
            double tn = medianMs([&] { runQuery(q, c, nl); }, 3), th = medianMs([&] { runQuery(q, c, hash); }, 5);
            double ta = medianMs([&] { runQuery(q, c); }, 5);
            bool hashPicked = runQuery(q, c).physical.find("HashJoin") != std::string::npos;
            r.rows.push_back({std::to_string(m), fmtMs(tn), fmtMs(th), fmt("%.0fx", tn / th), fmtMs(ta), hashPicked ? "hash" : "nested"});
        }
        r.print();
    }
    {
        Report r{"join_dims", "facts (" + std::to_string(n) + " rows) JOIN dim(R rows) ON facts.k = dim.id, COUNT(*): by dimension size",
                 {"dim_rows", "nested_ms", "hash_ms", "auto_ms", "auto_picked", "build_side"}, {}};
        for (int64_t m : {int64_t(100), int64_t(1000), int64_t(10000)}) {
            buildDim(main, "dim", m);
            const std::string q = "SELECT COUNT(*) FROM facts JOIN dim ON facts.k = dim.id";
            QueryOptions nl, hash;
            nl.join = JoinAlgorithm::NestedLoop;
            hash.join = JoinAlgorithm::Hash;
            std::string tn = "-";
            if (n * m <= 300000000LL) tn = fmtMs(medianMs([&] { runQuery(q, main, nl); }, 1));
            double th = medianMs([&] { runQuery(q, main, hash); }, 3);
            double ta = medianMs([&] { runQuery(q, main); }, 3);
            auto plan = runQuery(q, main).physical;
            bool hashPicked = plan.find("HashJoin") != std::string::npos;
            r.rows.push_back({std::to_string(m), tn, fmtMs(th), fmtMs(ta), hashPicked ? "hash" : "nested",
                              plan.find("build=left") != std::string::npos ? "left" : "right"});
        }
        r.print();
    }
}

// ---------- scenario: rewrite rules ----------

static void scenarioRules(Catalog& c, int64_t n) {
    buildDim(c, "dim", 100);
    auto run = [&](const std::string& title, const std::string& name, const std::string& q,
                   std::vector<std::pair<std::string, QueryOptions>> variants) {
        Report r{name, title, {"variant", "ms", "vs_baseline", "result_rows"}, {}};
        double base = 0;
        for (auto& v : variants) {
            const std::string& label = v.first;
            const QueryOptions& o = v.second;
            size_t rows = 0;
            double ms = medianMs([&] { rows = runQuery(q, c, o).rows.size(); }, 3);
            if (base == 0) base = ms;
            r.rows.push_back({label, fmtMs(ms), fmt("%.1fx faster", base / ms), std::to_string(rows)});
        }
        r.print();
    };
    QueryOptions none, pushOnly, pruneOnly, noIdx;
    none.pushdown = none.pruneColumns = false;
    pushOnly.pruneColumns = false;
    pruneOnly.pushdown = false;
    noIdx.useIndexes = false;
    const std::string sel = std::to_string(n / 100);
    run("Selective filter on a join: SELECT dim.label, COUNT(*) ... WHERE facts.id < " + sel + " GROUP BY dim.label",
        "rules_pushdown",
        "SELECT dim.label, COUNT(*) FROM facts JOIN dim ON facts.grp = dim.id WHERE facts.id < " + sel + " GROUP BY dim.label",
        {{"no rules (filter after join)", none},
         {"pushdown only, no index", [&] { auto o = pushOnly; o.useIndexes = false; return o; }()},
         {"pushdown + pruning, no index", noIdx},
         {"all rules + index scan", QueryOptions{}}});
    run("Wide rows through a join (no filter): column pruning only", "rules_pruning",
        "SELECT COUNT(*) FROM facts JOIN dim ON facts.grp = dim.id",
        {{"pruning off", [&] { auto o = QueryOptions{}; o.pruneColumns = false; return o; }()}, {"pruning on", QueryOptions{}}});
}

// ---------- scenario: scan / aggregation throughput ----------

static void scenarioScan(Catalog& c, int64_t n) {
    Report r{"scan", "Scan and aggregation throughput on facts (" + std::to_string(n) + " rows)", {"query", "ms", "Mrows_per_s"}, {}};
    for (const char* q : {"SELECT COUNT(*) FROM facts", "SELECT COUNT(*) FROM facts WHERE grp = 7",
                          "SELECT grp, COUNT(*), SUM(k), MAX(id) FROM facts GROUP BY grp", "SELECT payload, COUNT(*) FROM facts GROUP BY payload"}) {
        double ms = medianMs([&] { runQuery(q, c, seqOnly()); }, 3);
        r.rows.push_back({q, fmtMs(ms), fmt("%.2f", n / ms / 1000)});
    }
    r.print();
}

// ---------- scenario: buffer pool size ----------

static void scenarioPool() {
    int64_t n = args.quick ? 20000 : 100000;
    Report r{"pool", "Buffer pool size vs. index point lookups (" + std::to_string(n) + " rows, 4000 lookups)",
             {"frames", "pool_MB", "workload", "ms", "hit_ratio", "disk_reads", "evictions"}, {}};
    std::mt19937_64 rng(7);
    for (size_t frames : {size_t(16), size_t(64), size_t(256), size_t(1024), size_t(4096)}) {
        Catalog c(workDir("pool"), frames);
        buildFacts(c, n);
        for (const char* workload : {"uniform", "hot 5% / 90%"}) {
            std::vector<int64_t> keys;
            for (int i = 0; i < 4000; ++i) {
                bool hot = std::string(workload) != "uniform" && rng() % 100 < 90;
                keys.push_back(static_cast<int64_t>(rng() % (hot ? n / 20 : n)));
            }
            for (int pass = 0; pass < 2; ++pass) {  // first pass warms the cache and is not reported
                c.pool().resetStats();
                double ms = timeMs([&] {
                    for (int64_t k : keys) runQuery("SELECT payload FROM facts WHERE k = " + std::to_string(k), c);
                });
                if (pass == 0) continue;
                const auto& s = c.pool().stats();
                double total = static_cast<double>(s.hits + s.misses);
                r.rows.push_back({std::to_string(frames), fmt("%.1f", frames * 4096.0 / (1 << 20)), workload, fmtMs(ms),
                                  fmt("%.1f%%", 100.0 * s.hits / total), std::to_string(s.misses), std::to_string(s.evictions)});
            }
        }
    }
    r.print();
}

// ---------- calibration ----------

// Least squares for y = a*x1 + b*x2 (both clamped to be non-negative).
static std::pair<double, double> fit2(const std::vector<std::array<double, 3>>& d) {
    double s11 = 0, s12 = 0, s22 = 0, s1y = 0, s2y = 0;
    for (auto& [x1, x2, y] : d) {
        s11 += x1 * x1; s12 += x1 * x2; s22 += x2 * x2; s1y += x1 * y; s2y += x2 * y;
    }
    double det = s11 * s22 - s12 * s12;
    double a = (s1y * s22 - s2y * s12) / det, b = (s2y * s11 - s1y * s12) / det;
    return {std::max(a, 1e-4), std::max(b, 1e-4)};
}

static void calibrate(Catalog& c, int64_t n) {
    CostParams p;  // fresh measurements, not the currently compiled defaults
    const Table& facts = c.get("facts");

    // seqPerRow: scan + decode + evaluate a two-term range predicate on every row (almost none match)
    double seqMs = medianMs([&] { runQuery("SELECT COUNT(*) FROM facts WHERE k >= 0 AND k < 1", c, seqOnly()); }, 5);
    p.seqPerRow = seqMs * 1000 / n;

    // index fetch cost per matched row = slope of forced index range-scan time vs matched rows.
    // Unclustered index: measure at 1% and 10% of the table; clustered: at 30% and 100%.
    auto slope = [&](const std::string& col, double f1, double f2) {
        auto t = [&](int64_t m) {
            return medianMs([&] { runQuery("SELECT COUNT(*) FROM facts WHERE " + col + " >= 0 AND " + col + " < " + std::to_string(m), c, forceIdx()); }, 5);
        };
        int64_t m1 = static_cast<int64_t>(f1 * n), m2 = static_cast<int64_t>(f2 * n);
        return std::max(0.01, (t(m2) - t(m1)) * 1000 / (m2 - m1));
    };
    p.indexFetchPerRow = slope("k", 0.01, 0.10);
    p.indexFetchClusteredPerRow = slope("id", 0.30, 1.00);

    // indexNodeVisit: random probes straight on the tree
    const TableIndex* idx = facts.findIndex("k");
    std::mt19937_64 rng(3);
    const int probes = 20000;
    double probeMs = timeMs([&] {
        for (int i = 0; i < probes; ++i) {
            int64_t k = static_cast<int64_t>(rng() % n), key;
            uint64_t rid;
            auto it = idx->tree->range(k, k);
            it.next(key, rid);
        }
    });
    p.indexNodeVisit = probeMs * 1000 / probes / idx->tree->height();

    // join costs: subtract the (separately measured) input scans, then fit per-row terms
    auto joinTime = [&](int64_t l, int64_t r, JoinAlgorithm a) {
        Catalog jc(workDir("calib"), args.pool);
        buildDim(jc, "jl", l);
        buildDim(jc, "jr", r);
        QueryOptions o;
        o.join = a;
        double joinMs = medianMs([&] { runQuery("SELECT COUNT(*) FROM jl JOIN jr ON jl.id = jr.id", jc, o); }, 5);
        double scanMs = medianMs([&] { runQuery("SELECT COUNT(*) FROM jl", jc); }, 5) +
                        medianMs([&] { runQuery("SELECT COUNT(*) FROM jr", jc); }, 5);
        return std::max(0.0, (joinMs - scanMs) * 1000);  // time attributable to the join itself
    };
    std::vector<std::array<double, 3>> hash, nl;
    for (auto [l, r] : std::vector<std::pair<int64_t, int64_t>>{{20000, 2000}, {20000, 20000}, {5000, 50000}, {50000, 5000}, {100000, 10000}, {10000, 1000}}) {
        hash.push_back({double(std::min(l, r)), double(std::max(l, r)), joinTime(l, r, JoinAlgorithm::Hash)});
    }
    for (auto [l, r] : std::vector<std::pair<int64_t, int64_t>>{{1000, 1000}, {2000, 1000}, {1000, 3000}, {3000, 3000}, {2000, 4000}, {5000, 2000}}) {
        nl.push_back({double(r), double(l) * r, joinTime(l, r, JoinAlgorithm::NestedLoop)});
    }
    std::tie(p.hashBuildPerRow, p.hashProbePerRow) = fit2(hash);
    p.hashProbePerRow = std::max(p.hashProbePerRow, 0.02);  // a lookup cannot be free; the fit is noise-limited here
    std::tie(p.nlBufferPerRow, p.nlComparePerPair) = fit2(nl);

    std::printf("\n== Calibrated cost parameters (microseconds) on this machine\n");
    std::printf("    double seqPerRow = %.3f;\n    double indexNodeVisit = %.3f;\n    double indexFetchPerRow = %.3f;\n"
                "    double indexFetchClusteredPerRow = %.3f;\n    double hashBuildPerRow = %.3f;\n    double hashProbePerRow = %.3f;\n"
                "    double nlBufferPerRow = %.4f;\n    double nlComparePerPair = %.5f;\n",
                p.seqPerRow, p.indexNodeVisit, p.indexFetchPerRow, p.indexFetchClusteredPerRow, p.hashBuildPerRow,
                p.hashProbePerRow, p.nlBufferPerRow, p.nlComparePerPair);
    std::printf("  implied index/scan crossover: %.1f%% (unclustered), %.1f%% (clustered)\n", 100 * p.seqPerRow / p.indexFetchPerRow,
                100 * std::min(1.0, p.seqPerRow / p.indexFetchClusteredPerRow));
    std::printf("  index clustering statistic: k = %.3f, id = %.3f\n", facts.findIndex("k")->clustering, facts.findIndex("id")->clustering);
    costParams() = p;  // use the fresh values for the rest of this run
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--quick") args.quick = true;
        else if (a == "--calibrate") args.calibrate = true;
        else if (a == "--pool" && i + 1 < argc) args.pool = std::stoul(argv[++i]);
        else if (a == "--csv" && i + 1 < argc) args.csvDir = argv[++i];
        else args.scenarios.insert(a);
    }
    if (args.calibrate && args.scenarios.empty()) args.scenarios.insert("(calibration only)");
    benchRoot = (fs::temp_directory_path() / "minisql_bench").string();
    fs::remove_all(benchRoot);
    fs::create_directories(benchRoot);

    std::printf("MiniSQL benchmarks (%s mode, buffer pool %zu frames = %.1f MB)\n", args.quick ? "quick" : "full", args.pool,
                args.pool * 4096.0 / (1 << 20));
    try {
        if (args.wants("point")) scenarioPoint();

        const int64_t n = args.quick ? 50000 : 200000;
        bool needMain = args.calibrate || args.wants("sweep") || args.wants("join") || args.wants("rules") || args.wants("scan");
        if (needMain) {
            Catalog c(workDir("main"), args.pool);
            double build = timeMs([&] { buildFacts(c, n); }) / 1000;
            std::printf("\nbuilt facts: %lld rows, %u heap pages, 2 indexes (height %zu; clustering k=%.3f id=%.3f) in %.1fs\n",
                        static_cast<long long>(n), c.get("facts").heap->numPages(), c.get("facts").findIndex("k")->tree->height(),
                        c.get("facts").findIndex("k")->clustering, c.get("facts").findIndex("id")->clustering, build);
            if (args.calibrate) calibrate(c, n);
            if (args.wants("sweep")) scenarioSweep(c, n);
            if (args.wants("join")) scenarioJoin(c, n);
            if (args.wants("rules")) scenarioRules(c, n);
            if (args.wants("scan")) scenarioScan(c, n);
        }
        if (args.wants("pool")) scenarioPool();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "benchmark failed: %s\n", e.what());
        fs::remove_all(benchRoot);
        return 1;
    }
    fs::remove_all(benchRoot);
    return 0;
}
