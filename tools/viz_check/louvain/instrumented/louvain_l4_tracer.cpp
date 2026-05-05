// Louvain L4 self-RNG bit-equal-per-step tracer.
//
// Single-file C++ mirror of comdet/js/louvain/louvain.js. Self-contained
// (does NOT link canonical externals/louvain/.o). Drops long double for
// double; drops glibc rand for MT19937; mirrors JS's diffMove formula,
// cands ordering, strict-positive cutoff, per-endpoint strength write
// convention, and renumber-by-csize-DESC level transition.
//
// Build: instrumented/build_l4.sh -> /tmp/louvain_l4_tracer
// Run:   /tmp/louvain_l4_tracer <edge.csv> <seed>
//
// Output: stdout JSON
//   {
//     "seed": <int>,
//     "n": <int>,
//     "levels": [
//       { "level": L, "n_before": int, "passes": int,
//         "visit_order_per_pass": [[int,...], ...],   // one array per pass
//         "visits": [
//           {"pass": p, "visit": i, "v": v, "fromComm": fc, "toComm": tc,
//            "moved": bool, "dSbits": "0xUUUUUUUUUUUUUUUU"},
//           ...
//         ],
//         "n2c_post": [int,...]   // post-level membership before renumber
//       },
//       ...
//     ],
//     "fine_membership_post_renumber": [int,...],     // composed across levels
//     "Q_final_bits": "0xUUUUUUUUUUUUUUUU"
//   }

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using std::int32_t;
using std::int64_t;
using std::size_t;
using std::uint32_t;
using std::uint64_t;

// ───────────────────────── bit reinterpret helpers ──────────────────
static inline uint64_t bits_of(double x) {
    uint64_t b; std::memcpy(&b, &x, sizeof(b)); return b;
}

// ───────────────────────── MT19937 (mirror louvain.js MT19937) ──────
// Matsumoto-Nishimura 32-bit. Tempering identical to JS.
struct MT19937 {
    static constexpr int N = 624;
    uint32_t mt[N];
    int mti;

    explicit MT19937(uint32_t seed) { init(seed); }

    void init(uint32_t s) {
        mt[0] = s;
        for (int i = 1; i < N; i++) {
            uint32_t t = (mt[i-1] ^ (mt[i-1] >> 30));
            uint32_t lo = (t & 0xffff), hi = (t >> 16);
            uint32_t m = (1812433253u*lo + (((1812433253u*hi) & 0xffff) << 16));
            mt[i] = m + (uint32_t)i;
        }
        mti = N;
    }
    uint32_t next() {
        if (mti >= N) {
            for (int i = 0; i < N; i++) {
                uint32_t y = (mt[i] & 0x80000000u) | (mt[(i+1) % N] & 0x7fffffffu);
                uint32_t v = mt[(i+397) % N] ^ (y >> 1);
                if (y & 1) v ^= 0x9908b0dfu;
                mt[i] = v;
            }
            mti = 0;
        }
        uint32_t y = mt[mti++];
        y ^= (y >> 11);
        y ^= (y << 7) & 0x9d2c5680u;
        y ^= (y << 15) & 0xefc60000u;
        y ^= (y >> 18);
        return y;
    }
    // Match JS's int(lo, hi) rejection sampling.
    int32_t int_inclusive(int32_t lo, int32_t hi) {
        int32_t range = hi - lo + 1;
        if (range <= 0) return lo;
        uint64_t limit = ((uint64_t)0x100000000ull / (uint64_t)range) * (uint64_t)range;
        uint32_t r;
        do { r = next(); } while ((uint64_t)r >= limit);
        return lo + (int32_t)(r % (uint32_t)range);
    }
};

// Mirror louvain.js shuffle (Fisher-Yates idx from n-1 down to 1).
static void js_shuffle(std::vector<int32_t>& arr, MT19937& rng) {
    for (int idx = (int)arr.size() - 1; idx >= 1; idx--) {
        int j = rng.int_inclusive(0, idx);
        int32_t t = arr[idx]; arr[idx] = arr[j]; arr[j] = t;
    }
}

// ───────────────────────── Graph (mirror louvain.js Graph) ──────────
struct Graph {
    int32_t n;
    int32_t m;
    std::vector<int32_t> eu, ev;
    std::vector<double>  ew;
    std::vector<std::vector<int32_t>> adjE;  // edge ids per node
    std::vector<std::vector<int32_t>> adjN;  // neighbour ids per node
    std::vector<double> nodeSelfWeights;
    std::vector<double> strength;
    std::vector<int32_t> nodeSizes;
    double totalWeight;
    bool directed;
    bool correctSelfLoops;

    Graph() : n(0), m(0), totalWeight(0.0), directed(false), correctSelfLoops(false) {}

    void build(int32_t n_, const std::vector<std::tuple<int32_t,int32_t,double>>& edges,
               bool directed_, bool correctSelfLoops_,
               const std::vector<int32_t>* nodeSizes_ = nullptr) {
        n = n_;
        m = (int32_t)edges.size();
        directed = directed_;
        correctSelfLoops = correctSelfLoops_;
        eu.resize(m); ev.resize(m); ew.resize(m);
        for (int32_t i = 0; i < m; i++) {
            eu[i] = std::get<0>(edges[i]);
            ev[i] = std::get<1>(edges[i]);
            ew[i] = std::get<2>(edges[i]);
        }
        adjE.assign(n, {});
        adjN.assign(n, {});
        for (int32_t e = 0; e < m; e++) {
            int32_t u = eu[e], v = ev[e];
            adjE[u].push_back(e); adjN[u].push_back(v);
            if (u != v) { adjE[v].push_back(e); adjN[v].push_back(u); }
        }
        nodeSelfWeights.assign(n, 0.0);
        for (int32_t e = 0; e < m; e++) if (eu[e] == ev[e]) nodeSelfWeights[eu[e]] += ew[e];
        strength.assign(n, 0.0);
        // Per-endpoint convention (mirror louvain.js Graph strength build).
        for (int32_t e = 0; e < m; e++) {
            strength[eu[e]] += ew[e];
            strength[ev[e]] += ew[e];
        }
        totalWeight = 0.0;
        for (int32_t e = 0; e < m; e++) totalWeight += ew[e];
        if (nodeSizes_) nodeSizes = *nodeSizes_;
        else nodeSizes.assign(n, 1);
    }

    int32_t vcount() const { return n; }
    int32_t ecount() const { return m; }
    bool isDirected() const { return directed; }

    double possibleEdges(double sz) const {
        double p = directed ? sz * sz : (sz * (sz - 1)) / 2.0;
        if (correctSelfLoops) p += sz;
        return p;
    }

    // collapse(membership, ncomm) — mirror louvain.js Graph.collapse.
    // Use std::unordered_map keyed on lo*ncomm+hi (single Map.set per edge).
    // Iteration order = JS Map iteration order = INSERTION order. For
    // bit-equal collapse output order across JS+cpp we mirror that:
    // walk old edges in original order; if key new, append; else add to
    // existing entry's accumulated weight.
    Graph collapse(const std::vector<int32_t>& membership, int32_t ncomm) const {
        std::unordered_map<int64_t, int32_t> keyToIdx;
        std::vector<int64_t>    keys;
        std::vector<double>     wsum;
        std::vector<int32_t>    los, his;
        keyToIdx.reserve((size_t)m * 2);
        for (int32_t e = 0; e < m; e++) {
            int32_t a = membership[eu[e]];
            int32_t b = membership[ev[e]];
            double  w = ew[e];
            int32_t lo = a, hi = b;
            if (!directed && lo > hi) { lo = b; hi = a; }
            int64_t key = (int64_t)lo * (int64_t)ncomm + (int64_t)hi;
            auto it = keyToIdx.find(key);
            if (it == keyToIdx.end()) {
                int32_t idx = (int32_t)keys.size();
                keyToIdx[key] = idx;
                keys.push_back(key);
                wsum.push_back(w);
                los.push_back(lo);
                his.push_back(hi);
            } else {
                wsum[it->second] += w;
            }
        }
        std::vector<std::tuple<int32_t,int32_t,double>> newEdges;
        newEdges.reserve(keys.size());
        for (size_t i = 0; i < keys.size(); i++) {
            newEdges.emplace_back(los[i], his[i], wsum[i]);
        }
        std::vector<int32_t> newSizes(ncomm, 0);
        for (int32_t v = 0; v < n; v++) newSizes[membership[v]] += nodeSizes[v];
        Graph g2;
        g2.build(ncomm, newEdges, directed, correctSelfLoops, &newSizes);
        return g2;
    }
};

// ───────────────────────── Modularity (mirror louvain.js Modularity) ─
// Pure structs of operations (no virtual dispatch). diffMove + quality
// take Partition& (defined below). Use forward decl trick.

struct Partition;  // fwd
struct Modularity {
    static double diffMove(Partition& P, int32_t v, int32_t newComm);
    static double quality(Partition& P);
};

// ───────────────────────── Partition (mirror louvain.js Partition) ──
struct Partition {
    Graph* graph;
    int32_t n;
    std::vector<int32_t> membership;
    int32_t ncomm;
    // Float64-typed admin.
    std::vector<double> csize;
    std::vector<int32_t> cnodes;
    std::vector<double> totalWeightInComm;
    std::vector<double> totalWeightToComm;
    // Under undirected, totalWeightFromComm aliases totalWeightToComm.
    // We model the alias by always reading via the helper.
    bool fromAliasesTo;
    std::vector<double> totalWeightFromCommStorage;  // used only when directed
    double totalWeightInAllComms;
    double totalPossibleEdgesInAllComms;
    std::unordered_set<int32_t> empties;

    // Per-node neighbour-comm cache.
    int32_t cacheV;
    std::vector<int32_t> cacheNeighComms;
    std::unordered_map<int32_t,double> cacheWeightsTo;
    std::unordered_map<int32_t,double> cacheWeightsFrom;

    // Diagnostic for visit emission.
    bool recordTrace;

    void init(Graph& g, const std::vector<int32_t>* initMem) {
        graph = &g;
        n = g.vcount();
        membership.assign(n, 0);
        if (initMem) for (int32_t i = 0; i < n; i++) membership[i] = (*initMem)[i];
        else for (int32_t i = 0; i < n; i++) membership[i] = i;
        cacheV = -1;
        rebuildAdmin();
    }

    double totalWeightFromComm(int32_t c) const {
        return fromAliasesTo ? totalWeightToComm[c] : totalWeightFromCommStorage[c];
    }

    void setTotalWeightFromComm(int32_t c, double v) {
        if (fromAliasesTo) totalWeightToComm[c] = v;
        else totalWeightFromCommStorage[c] = v;
    }

    void invalidateCache() {
        cacheV = -1;
        cacheNeighComms.clear();
        cacheWeightsTo.clear();
        cacheWeightsFrom.clear();
    }

    void rebuildAdmin() {
        ncomm = 0;
        for (int32_t i = 0; i < n; i++) if (membership[i] + 1 > ncomm) ncomm = membership[i] + 1;
        bool directed = graph->isDirected();
        csize.assign(ncomm, 0.0);
        cnodes.assign(ncomm, 0);
        totalWeightInComm.assign(ncomm, 0.0);
        totalWeightToComm.assign(ncomm, 0.0);
        if (directed) {
            totalWeightFromCommStorage.assign(ncomm, 0.0);
            fromAliasesTo = false;
        } else {
            totalWeightFromCommStorage.clear();
            fromAliasesTo = true;
        }
        for (int32_t v = 0; v < n; v++) {
            csize[membership[v]] += graph->nodeSizes[v];
            cnodes[membership[v]] += 1;
        }
        int32_t m = graph->ecount();
        for (int32_t e = 0; e < m; e++) {
            int32_t u = graph->eu[e];
            int32_t v = graph->ev[e];
            int32_t cu = membership[u], cv = membership[v];
            double w = graph->ew[e];
            if (cu == cv) totalWeightInComm[cu] += w;
            if (directed) {
                totalWeightFromCommStorage[cu] += w;
                totalWeightToComm[cv] += w;
            } else {
                totalWeightToComm[cu] += w;
                totalWeightToComm[cv] += w;
            }
        }
        totalPossibleEdgesInAllComms = 0.0;
        totalWeightInAllComms = 0.0;
        empties.clear();
        for (int32_t c = 0; c < ncomm; c++) {
            totalWeightInAllComms += totalWeightInComm[c];
            totalPossibleEdgesInAllComms += graph->possibleEdges(csize[c]);
            if (cnodes[c] == 0) empties.insert(c);
        }
        invalidateCache();
    }

    void cacheNeighbours(int32_t v) {
        if (cacheV == v) return;
        cacheV = v;
        cacheNeighComms.clear();
        cacheWeightsTo.clear();
        cacheWeightsFrom.clear();
        bool directed = graph->isDirected();
        const std::vector<int32_t>& adjE = graph->adjE[v];
        std::unordered_set<int32_t> seen;
        for (size_t i = 0; i < adjE.size(); i++) {
            int32_t e = adjE[i];
            int32_t uu = graph->eu[e];
            int32_t vv = graph->ev[e];
            int32_t other = (uu == v) ? vv : uu;
            if (other == v) continue;
            int32_t c = membership[other];
            if (seen.find(c) == seen.end()) {
                seen.insert(c);
                cacheNeighComms.push_back(c);
            }
            double w = graph->ew[e];
            if (directed) {
                if (vv == v) cacheWeightsTo[c] += w;
                else         cacheWeightsFrom[c] += w;
            } else {
                cacheWeightsTo[c] += w;
            }
        }
    }

    double weightToComm(int32_t v, int32_t comm) {
        cacheNeighbours(v);
        auto it = cacheWeightsTo.find(comm);
        return it == cacheWeightsTo.end() ? 0.0 : it->second;
    }

    double weightFromComm(int32_t v, int32_t comm) {
        if (!graph->isDirected()) return weightToComm(v, comm);
        cacheNeighbours(v);
        auto it = cacheWeightsFrom.find(comm);
        return it == cacheWeightsFrom.end() ? 0.0 : it->second;
    }

    const std::vector<int32_t>& getNeighComms(int32_t v) {
        cacheNeighbours(v);
        return cacheNeighComms;
    }

    void grow_admin(int32_t newN) {
        if (newN <= ncomm) return;
        bool directed = graph->isDirected();
        csize.resize(newN, 0.0);
        cnodes.resize(newN, 0);
        totalWeightInComm.resize(newN, 0.0);
        totalWeightToComm.resize(newN, 0.0);
        if (directed) totalWeightFromCommStorage.resize(newN, 0.0);
        // fromAliasesTo unchanged; totalWeightToComm grew, alias still holds.
        ncomm = newN;
    }

    void moveNode(int32_t v, int32_t target) {
        int32_t old = membership[v];
        if (old == target) return;
        bool directed = graph->isDirected();
        const std::vector<int32_t>& adjE = graph->adjE[v];
        double deltaInAll = 0.0;
        // Phase A: subtract v's contribution from `old`.
        for (size_t i = 0; i < adjE.size(); i++) {
            int32_t e = adjE[i];
            int32_t uu = graph->eu[e];
            int32_t vv = graph->ev[e];
            int32_t other = (uu == v) ? vv : uu;
            double w = graph->ew[e];
            if (other == v) {
                totalWeightInComm[old] -= w;
                if (fromAliasesTo) {
                    // JS undirected aliases from = to. The two JS writes
                    // (totalWeightFromComm[old] -= w; totalWeightToComm[old] -= w;)
                    // both land in the same storage → -2w net.
                    totalWeightToComm[old] -= w;
                    totalWeightToComm[old] -= w;
                } else {
                    totalWeightFromCommStorage[old] -= w;
                    totalWeightToComm[old] -= w;
                }
                deltaInAll -= w;
                continue;
            }
            int32_t cother = membership[other];
            if (directed) {
                if (uu == v) {
                    totalWeightFromCommStorage[old] -= w;
                    totalWeightToComm[cother] -= w;
                    if (cother == old) { totalWeightInComm[old] -= w; deltaInAll -= w; }
                } else {
                    totalWeightToComm[old] -= w;
                    totalWeightFromCommStorage[cother] -= w;
                    if (cother == old) { totalWeightInComm[old] -= w; deltaInAll -= w; }
                }
            } else {
                totalWeightToComm[old] -= w;
                totalWeightToComm[cother] -= w;
                if (cother == old) {
                    totalWeightInComm[old] -= w;
                    deltaInAll -= w;
                }
            }
        }
        // Phase B: relocate csize / cnodes; grow if target is fresh.
        csize[old] -= graph->nodeSizes[v];
        cnodes[old] -= 1;
        totalPossibleEdgesInAllComms -= graph->possibleEdges(csize[old] + graph->nodeSizes[v]);
        totalPossibleEdgesInAllComms += graph->possibleEdges(csize[old]);
        membership[v] = target;
        if (target >= ncomm) {
            int32_t newN = target + 1;
            int32_t oldN = ncomm;
            grow_admin(newN);
            totalPossibleEdgesInAllComms += graph->possibleEdges(0) * (double)(newN - oldN);
        }
        totalPossibleEdgesInAllComms -= graph->possibleEdges(csize[target]);
        csize[target] += graph->nodeSizes[v];
        cnodes[target] += 1;
        totalPossibleEdgesInAllComms += graph->possibleEdges(csize[target]);
        // Phase C: add v's contribution to `target`.
        for (size_t i = 0; i < adjE.size(); i++) {
            int32_t e = adjE[i];
            int32_t uu = graph->eu[e];
            int32_t vv = graph->ev[e];
            int32_t other = (uu == v) ? vv : uu;
            double w = graph->ew[e];
            if (other == v) {
                totalWeightInComm[target] += w;
                if (fromAliasesTo) {
                    // JS undirected: from = to alias → both writes net +2w.
                    totalWeightToComm[target] += w;
                    totalWeightToComm[target] += w;
                } else {
                    totalWeightFromCommStorage[target] += w;
                    totalWeightToComm[target] += w;
                }
                deltaInAll += w;
                continue;
            }
            int32_t cother = membership[other];
            if (directed) {
                if (uu == v) {
                    totalWeightFromCommStorage[target] += w;
                    totalWeightToComm[cother] += w;
                    if (cother == target) { totalWeightInComm[target] += w; deltaInAll += w; }
                } else {
                    totalWeightToComm[target] += w;
                    totalWeightFromCommStorage[cother] += w;
                    if (cother == target) { totalWeightInComm[target] += w; deltaInAll += w; }
                }
            } else {
                totalWeightToComm[target] += w;
                totalWeightToComm[cother] += w;
                if (cother == target) {
                    totalWeightInComm[target] += w;
                    deltaInAll += w;
                }
            }
        }
        if (cnodes[old] == 0) empties.insert(old);
        empties.erase(target);
        totalWeightInAllComms += deltaInAll;
        invalidateCache();
    }

    // Sort surviving comms by csize DESC (mirror louvain.js:458-493).
    // STABLE sort: ties broken by original comm-id ASC (since the `order`
    // vector is built by iterating c=0..ncomm and appending).
    void renumber() {
        std::vector<int32_t> order;
        for (int32_t c = 0; c < ncomm; c++) if (cnodes[c] > 0) order.push_back(c);
        std::stable_sort(order.begin(), order.end(),
            [this](int32_t a, int32_t b) { return csize[a] > csize[b]; });
        std::vector<int32_t> remap(ncomm, -1);
        for (size_t i = 0; i < order.size(); i++) remap[order[i]] = (int32_t)i;
        for (int32_t v = 0; v < n; v++) membership[v] = remap[membership[v]];
        int32_t newN = (int32_t)order.size();
        std::vector<double> newCsize(newN, 0.0), newIn(newN, 0.0), newTo(newN, 0.0);
        std::vector<int32_t> newCnodes(newN, 0);
        std::vector<double> newFrom; if (!fromAliasesTo) newFrom.assign(newN, 0.0);
        for (int32_t i = 0; i < newN; i++) {
            int32_t oldId = order[i];
            newCsize[i] = csize[oldId];
            newCnodes[i] = cnodes[oldId];
            newIn[i] = totalWeightInComm[oldId];
            newTo[i] = totalWeightToComm[oldId];
            if (!fromAliasesTo) newFrom[i] = totalWeightFromCommStorage[oldId];
        }
        csize = newCsize; cnodes = newCnodes;
        totalWeightInComm = newIn;
        totalWeightToComm = newTo;
        if (!fromAliasesTo) totalWeightFromCommStorage = newFrom;
        ncomm = newN;
        empties.clear();
        invalidateCache();
    }

};

// ───────────── Modularity diffMove + quality (mirror JS) ────────────
double Modularity::diffMove(Partition& P, int32_t v, int32_t newComm) {
    int32_t oldComm = P.membership[v];
    if (oldComm == newComm) return 0.0;
    Graph& G = *P.graph;
    double m = G.totalWeight;
    if (m <= 0.0) return 0.0;
    double W = G.isDirected() ? m : 2.0 * m;
    double kv = G.strength[v];
    double wToOld = P.weightToComm(v, oldComm);
    double wToNew = P.weightToComm(v, newComm);
    double Kold = P.totalWeightFromComm(oldComm);
    double Knew = P.totalWeightFromComm(newComm);
    double diffOld = wToOld - kv * (Kold - kv) / W;
    double diffNew = wToNew - kv * Knew / W;
    double diff = (diffNew - diffOld) * (G.isDirected() ? 1.0 : 2.0);
    return diff / W;
}

double Modularity::quality(Partition& P) {
    Graph& G = *P.graph;
    double m = G.totalWeight;
    if (m <= 0.0) return 0.0;
    double W = G.isDirected() ? m : 2.0 * m;
    double q = 0.0;
    for (int32_t c = 0; c < P.ncomm; c++) {
        if (P.cnodes[c] == 0) continue;
        double ec = P.totalWeightInComm[c];
        double Kc = P.totalWeightFromComm(c);
        q += (G.isDirected() ? ec : 2.0 * ec) - (Kc * Kc) / W;
    }
    return q / W;
}

// ───────────── sweep + phase1 + run (mirror louvain.js sweep/phase1/run) ─
struct VisitRecord {
    int32_t pass;
    int32_t visit;
    int32_t v;
    int32_t fromComm;
    int32_t toComm;
    bool moved;
    uint64_t dSbits;
};

struct LevelTrace {
    int32_t level;
    int32_t n_before;
    int32_t passes;
    std::vector<std::vector<int32_t>> visit_order_per_pass;
    std::vector<VisitRecord> visits;
    std::vector<int32_t> n2c_post;
};

struct SweepOut {
    double totalImprov;
    int32_t nbMoves;
};

static SweepOut louvain_sweep(Partition& P, MT19937& rng,
                              int32_t levelIdx, int32_t passIdx,
                              std::vector<VisitRecord>& outVisits,
                              std::vector<int32_t>& outOrder) {
    int32_t n = P.n;
    std::vector<int32_t> order(n);
    for (int32_t i = 0; i < n; i++) order[i] = i;
    js_shuffle(order, rng);
    outOrder = order;
    SweepOut so{0.0, 0};
    for (int32_t i = 0; i < n; i++) {
        int32_t v = order[i];
        int32_t vComm = P.membership[v];
        // JS sweep iterates getNeighComms(v) + vComm-if-absent. The
        // c==vComm pass produces delta=0 and never beats the strict-
        // positive `> maxImprov` cutoff with init 0, so the trailing
        // vComm visit can't change the decision. Iterate by const-ref
        // and skip vComm; saves a per-visit vector copy on the hot path.
        const std::vector<int32_t>& cands = P.getNeighComms(v);
        int32_t maxComm = vComm;
        double  maxImprov = 0.0;
        for (size_t j = 0; j < cands.size(); j++) {
            int32_t c = cands[j];
            if (c == vComm) continue;
            double d = Modularity::diffMove(P, v, c);
            if (d > maxImprov) {
                maxImprov = d;
                maxComm = c;
            }
        }
        bool moved = false;
        if (maxComm != vComm) {
            so.totalImprov += maxImprov;
            P.moveNode(v, maxComm);
            moved = true;
            so.nbMoves += 1;
        }
        VisitRecord vr;
        vr.pass = passIdx; vr.visit = i; vr.v = v;
        vr.fromComm = vComm; vr.toComm = maxComm;
        vr.moved = moved;
        // Mirror JS sweep recordTrace semantics: delta = moved ? maxImprov : 0.
        vr.dSbits = bits_of(moved ? maxImprov : 0.0);
        outVisits.push_back(vr);
    }
    return so;
}

struct Phase1Out {
    int32_t passes;
};

static Phase1Out louvain_phase1(Partition& P, MT19937& rng,
                                int32_t levelIdx, LevelTrace& lvl) {
    Phase1Out p1{0};
    while (true) {
        std::vector<int32_t> order;
        SweepOut so = louvain_sweep(P, rng, levelIdx, p1.passes, lvl.visits, order);
        lvl.visit_order_per_pass.push_back(std::move(order));
        p1.passes += 1;
        if (so.nbMoves == 0 || p1.passes > 50) break;
    }
    return p1;
}

struct RunOut {
    std::vector<LevelTrace> levels;
    std::vector<int32_t> fineMembership;
    double Q_final;
};

static RunOut louvain_run(Graph& G0, uint32_t seed) {
    MT19937 rng(seed);
    RunOut out;
    Graph collapsedG = G0;
    int32_t n0 = G0.vcount();
    std::vector<int32_t> aggregateMap(n0);
    for (int32_t i = 0; i < n0; i++) aggregateMap[i] = i;
    std::vector<int32_t> fineMembership(n0);
    for (int32_t i = 0; i < n0; i++) fineMembership[i] = i;
    int32_t level = 0;
    while (true) {
        int32_t prevVcount = collapsedG.vcount();
        Partition collP;
        collP.init(collapsedG, nullptr);
        LevelTrace lvl;
        lvl.level = level;
        lvl.n_before = prevVcount;
        Phase1Out p1 = louvain_phase1(collP, rng, level, lvl);
        lvl.passes = p1.passes;
        // Snapshot post-level n2c BEFORE renumber (matches existing tracer
        // convention; visualizer can show pre-renumber membership).
        lvl.n2c_post = collP.membership;
        // JS run does collP.renumber() (csize DESC) before collapse.
        collP.renumber();
        const std::vector<int32_t>& memColl = collP.membership;
        for (int32_t v = 0; v < n0; v++) {
            fineMembership[v] = memColl[aggregateMap[v]];
        }
        int32_t collapsedNcomm = collP.ncomm;
        Graph newCollapsed = collapsedG.collapse(memColl, collapsedNcomm);
        std::vector<int32_t> newAggregate(n0);
        for (int32_t v = 0; v < n0; v++) newAggregate[v] = memColl[aggregateMap[v]];
        aggregateMap = newAggregate;
        out.levels.push_back(std::move(lvl));
        if (newCollapsed.vcount() >= prevVcount || newCollapsed.vcount() <= 1) break;
        collapsedG = newCollapsed;
        level += 1;
        if (level > 30) break;
    }
    Partition Pfinal;
    Pfinal.init(G0, &fineMembership);
    Pfinal.renumber();
    out.fineMembership = Pfinal.membership;
    out.Q_final = Modularity::quality(Pfinal);
    return out;
}

// ───────────────── CSV parse + main entry ───────────────────────────
static void read_edge_csv(const std::string& path,
                          std::vector<std::tuple<int32_t,int32_t,double>>& edges,
                          int32_t& n_out) {
    std::ifstream f(path);
    if (!f.is_open()) {
        std::fprintf(stderr, "cannot open %s\n", path.c_str());
        std::exit(2);
    }
    std::string line;
    int32_t maxNode = -1;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        for (char& c : line) if (c == ',' || c == '\t') c = ' ';
        std::stringstream ss(line);
        int32_t u, v;
        // A non-numeric header line ("source,target", "node_id1,node_id2")
        // fails this parse and is silently skipped. Avoids a separate
        // header heuristic.
        if (!(ss >> u >> v)) continue;
        double w = 1.0;
        ss >> w;
        edges.emplace_back(u, v, w);
        if (u > maxNode) maxNode = u;
        if (v > maxNode) maxNode = v;
    }
    n_out = maxNode + 1;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <edge.csv> <seed>\n", argv[0]);
        return 2;
    }
    std::string edgePath = argv[1];
    uint32_t seed = (uint32_t)std::strtoul(argv[2], nullptr, 10);
    std::vector<std::tuple<int32_t,int32_t,double>> edges;
    int32_t n = 0;
    read_edge_csv(edgePath, edges, n);
    Graph G;
    G.build(n, edges, false, false);
    RunOut run = louvain_run(G, seed);

    // Emit JSON.
    std::cout << "{";
    std::cout << "\"seed\":" << seed << ",";
    std::cout << "\"n\":" << n << ",";
    std::cout << "\"levels\":[";
    for (size_t li = 0; li < run.levels.size(); li++) {
        const LevelTrace& lv = run.levels[li];
        if (li) std::cout << ",";
        std::cout << "{";
        std::cout << "\"level\":" << lv.level << ",";
        std::cout << "\"n_before\":" << lv.n_before << ",";
        std::cout << "\"passes\":" << lv.passes << ",";
        std::cout << "\"visit_order_per_pass\":[";
        for (size_t p = 0; p < lv.visit_order_per_pass.size(); p++) {
            if (p) std::cout << ",";
            std::cout << "[";
            const auto& ord = lv.visit_order_per_pass[p];
            for (size_t i = 0; i < ord.size(); i++) {
                if (i) std::cout << ",";
                std::cout << ord[i];
            }
            std::cout << "]";
        }
        std::cout << "],";
        std::cout << "\"visits\":[";
        for (size_t i = 0; i < lv.visits.size(); i++) {
            const VisitRecord& vr = lv.visits[i];
            if (i) std::cout << ",";
            char hexbuf[32];
            std::snprintf(hexbuf, sizeof(hexbuf), "0x%016llx", (unsigned long long)vr.dSbits);
            std::cout << "{\"pass\":" << vr.pass
                      << ",\"visit\":" << vr.visit
                      << ",\"v\":" << vr.v
                      << ",\"fromComm\":" << vr.fromComm
                      << ",\"toComm\":" << vr.toComm
                      << ",\"moved\":" << (vr.moved ? "true" : "false")
                      << ",\"dSbits\":\"" << hexbuf << "\"}";
        }
        std::cout << "],";
        std::cout << "\"n2c_post\":[";
        for (size_t i = 0; i < lv.n2c_post.size(); i++) {
            if (i) std::cout << ",";
            std::cout << lv.n2c_post[i];
        }
        std::cout << "]}";
    }
    std::cout << "],";
    std::cout << "\"fine_membership_post_renumber\":[";
    for (size_t i = 0; i < run.fineMembership.size(); i++) {
        if (i) std::cout << ",";
        std::cout << run.fineMembership[i];
    }
    std::cout << "],";
    char qb[32]; std::snprintf(qb, sizeof(qb), "0x%016llx", (unsigned long long)bits_of(run.Q_final));
    std::cout << "\"Q_final_bits\":\"" << qb << "\"";
    std::cout << "}\n";
    return 0;
}
