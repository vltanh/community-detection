// Louvain L4 self-RNG bit-equal-per-step tracer.
//
// Single-source compile-time-toggle build (per byte-equal-tracer skill
// §"Compile-time tracer-mode toggle"). One source produces TWO binaries:
//
//   -DLV_TRACER_MODE   /tmp/louvain_l4_tracer_swapped
//                      JS-bridged RNG (MT19937, mirroring
//                      comdet/js/louvain/louvain.js MT19937 byte-for-byte)
//                      + FP type = double (so JS Float64 reaches bit
//                      equality). The verification cross-check artifact;
//                      paired with kernel_check_l4.mjs.
//
//   -DLV_CANONICAL_MODE /tmp/louvain_l4_tracer_canonical
//                      Original RNG = libc rand() (matches gen-louvain's
//                      louvain.cpp:225 + the LD_PRELOAD seed shim's
//                      srand interception) + FP type = long double
//                      (matches externals/louvain Makefile's natural
//                      promotion). For the build-pair equivalence test:
//                      tracer_canonical's PARTITION (decoded from
//                      fineMembership) must byte-equal externals/louvain
//                      run_louvain.py's com.csv output under matching
//                      LOUVAIN_SEED + LD_PRELOAD shim.
//
// The algorithm body is canonical-faithful in BOTH modes (canonical
// modularity admin, canonical neigh_comm trail layout, canonical
// remove + gain + insert sequence, canonical partition2graph_binary
// renumber by original-id ASC, std::map-keyed aggregation in collapse).
// Only the RNG family + FP type swap between modes.
//
// Build: instrumented/build_l4.sh -> /tmp/louvain_l4_tracer_{canonical,swapped}
// Run:   /tmp/louvain_l4_tracer_swapped <edge.csv> <seed>
//        LOUVAIN_SEED=<seed> LD_PRELOAD=<shim>.so /tmp/louvain_l4_tracer_canonical <edge.csv>
//        (canonical mode reads seed only via LD_PRELOAD shim's srand
//        interception — argv seed is ignored when LV_CANONICAL_MODE.)

// ── Mode resolution. Default to LV_TRACER_MODE for back-compat with
//    callers that built without explicit -D macro. ─────────────────
#if !defined(LV_CANONICAL_MODE) && !defined(LV_TRACER_MODE)
  #define LV_TRACER_MODE
#endif
#if defined(LV_CANONICAL_MODE) && defined(LV_TRACER_MODE)
  #error "Define exactly one of LV_CANONICAL_MODE or LV_TRACER_MODE."
#endif

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#if defined(LV_CANONICAL_MODE)
  #include <unistd.h>  // getpid for canonical-mode fallback seed
#endif

using std::int32_t;
using std::int64_t;
using std::size_t;
using std::uint32_t;
using std::uint64_t;

// ── FP type swap (audit row D). ──────────────────────────────────
// Canonical externals/louvain uses `long double` everywhere (modularity.cpp,
// graph_binary.h, louvain.cpp). On x86-64 Linux gcc default that's 80-bit
// x87 extended; truncating to uint64 for trace bit-compare loses the
// extended bits, so trace bit-compare is meaningful only in TRACER_MODE.
// Under CANONICAL_MODE we still emit trace JSON for diagnostic purposes
// but the dGainBits / dSbits / Q_final_bits are double-precision casts.
#if defined(LV_CANONICAL_MODE)
  using fp_t = long double;
#else
  using fp_t = double;
#endif

static inline uint64_t bits_of_double(double x) {
    uint64_t b; std::memcpy(&b, &x, sizeof(b)); return b;
}
static inline uint64_t bits_of_fp(fp_t x) {
    return bits_of_double((double)x);
}

// ── RNG swap (audit rows A/B/C). ─────────────────────────────────
// Two RNG families behind a uniform `int_inclusive(lo, hi)` interface:
//   - LV_TRACER_MODE: MT19937 mirroring comdet/js/louvain/louvain.js
//     byte-for-byte (init recurrence + tempering + rejection-sampled
//     int(lo, hi) all identical). Init via argv seed.
//   - LV_CANONICAL_MODE: libc rand() / srand() (the same RNG canonical
//     uses at louvain.cpp:225). Shuffle becomes `rand() % range + lo`
//     (canonical's bias-tolerated form, NO rejection sampling). Seed
//     comes from the LD_PRELOAD shim's interception of srand(time+pid).
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
        y ^= (y << 7)  & 0x9d2c5680u;
        y ^= (y << 15) & 0xefc60000u;
        y ^= (y >> 18);
        return y;
    }
    int32_t int_inclusive(int32_t lo, int32_t hi) {
        int32_t range = hi - lo + 1;
        if (range <= 0) return lo;
        uint64_t limit = ((uint64_t)0x100000000ull / (uint64_t)range) * (uint64_t)range;
        uint32_t r;
        do { r = next(); } while ((uint64_t)r >= limit);
        return lo + (int32_t)(r % (uint32_t)range);
    }
};

#if defined(LV_CANONICAL_MODE)
struct LibcRand {
    explicit LibcRand(uint32_t seed) {
        // Honour the LD_PRELOAD shim if present: shim's srand intercepts
        // any srand() call and replaces the seed with $LOUVAIN_SEED. So
        // calling srand(seed) here lets the shim override; without the
        // shim, our seed argument is used directly.
        std::srand(seed);
    }
    int32_t int_inclusive(int32_t lo, int32_t hi) {
        // Canonical louvain.cpp:225: rand() % (qual->size - i) + i.
        // Translates to int_inclusive(i, size-1): range = size-i,
        // result = rand() % range + i. NO rejection sampling — canonical
        // tolerates the modulo bias. Same site under CANONICAL_MODE.
        int32_t range = hi - lo + 1;
        if (range <= 0) return lo;
        return lo + (std::rand() % range);
    }
};
using RNG_t = LibcRand;
#else
using RNG_t = MT19937;
#endif

// ───────────── Graph (mirrors canonical graph_binary.{h,cpp}) ────
// Same adjacency layout as canonical: each non-self edge appears in
// both endpoint lists; self-loops appear ONCE. weighted_degree(v) sums
// adj weights so a self-loop contributes once. total_weight = Σ
// weighted_degree(v).
struct Graph {
    int32_t n;
    int32_t m;
    std::vector<int32_t> eu, ev;
    std::vector<fp_t>    ew;
    std::vector<std::vector<int32_t>> adjN;
    std::vector<std::vector<fp_t>>    adjW;
    std::vector<int32_t> nodes_w;
    fp_t total_weight;

    Graph() : n(0), m(0), total_weight(fp_t(0)) {}

    void build(int32_t n_, const std::vector<std::tuple<int32_t,int32_t,fp_t>>& edges,
               const std::vector<int32_t>* nodes_w_ = nullptr) {
        n = n_;
        m = (int32_t)edges.size();
        eu.assign(m, 0); ev.assign(m, 0); ew.assign(m, fp_t(0));
        for (int32_t i = 0; i < m; i++) {
            eu[i] = std::get<0>(edges[i]);
            ev[i] = std::get<1>(edges[i]);
            ew[i] = std::get<2>(edges[i]);
        }
        adjN.assign(n, {});
        adjW.assign(n, {});
        for (int32_t e = 0; e < m; e++) {
            int32_t u = eu[e], v = ev[e];
            fp_t    w = ew[e];
            adjN[u].push_back(v); adjW[u].push_back(w);
            if (u != v) {
                adjN[v].push_back(u); adjW[v].push_back(w);
            }
        }
        // Audit row N — adj iteration order. Mirror canonical
        // Graph::clean() (graph.cpp:107-129) in BOTH modes: per-node adj
        // sorted by target-id ASC. Canonical convert pre-processes the
        // .bin via std::map<int,long double> aggregation = key-ASC
        // iteration; reading the .bin reproduces ASC adj. Tracer must
        // match in both modes; JS Louvain Graph must also sort. The
        // toggle does NOT swap algorithmic conventions — only RNG + FP.
        for (int32_t v = 0; v < n; v++) {
            std::vector<size_t> idx(adjN[v].size());
            for (size_t k = 0; k < idx.size(); k++) idx[k] = k;
            std::sort(idx.begin(), idx.end(), [&](size_t a, size_t b) {
                return adjN[v][a] < adjN[v][b];
            });
            std::vector<int32_t> sN(adjN[v].size());
            std::vector<fp_t>    sW(adjW[v].size());
            for (size_t k = 0; k < idx.size(); k++) {
                sN[k] = adjN[v][idx[k]];
                sW[k] = adjW[v][idx[k]];
            }
            adjN[v] = std::move(sN);
            adjW[v] = std::move(sW);
        }
        if (nodes_w_) nodes_w = *nodes_w_;
        else nodes_w.assign(n, 1);
        total_weight = fp_t(0);
        for (int32_t v = 0; v < n; v++) {
            fp_t s = fp_t(0);
            for (size_t i = 0; i < adjW[v].size(); i++) s += adjW[v][i];
            total_weight += s;
        }
    }

    int32_t nb_neighbors(int32_t v) const { return (int32_t)adjN[v].size(); }
    fp_t weighted_degree(int32_t v) const {
        fp_t s = fp_t(0);
        for (size_t i = 0; i < adjW[v].size(); i++) s += adjW[v][i];
        return s;
    }
    fp_t nb_selfloops(int32_t v) const {
        for (size_t i = 0; i < adjN[v].size(); i++) {
            if (adjN[v][i] == v) return adjW[v][i];
        }
        return fp_t(0);
    }
};

// ───────────── Modularity admin (mirrors canonical Modularity) ───
// in[c] += 2·dnc + nb_selfloops(node) on insert, equivalent subtract on
// remove. tot[c] tracks Σ weighted_degree of constituents. gain returns
// dnc - tot[c]·w_degree / m2 in unnormalized "gain units" (canonical
// modularity.h:80-88).
struct Modularity {
    Graph& g;
    int32_t size;
    std::vector<int32_t> n2c;
    std::vector<fp_t>    in_;
    std::vector<fp_t>    tot_;

    explicit Modularity(Graph& gr) : g(gr), size(gr.n) {
        n2c.assign(size, 0);
        in_.assign(size, fp_t(0));
        tot_.assign(size, fp_t(0));
        for (int32_t i = 0; i < size; i++) {
            n2c[i] = i;
            in_[i]  = g.nb_selfloops(i);
            tot_[i] = g.weighted_degree(i);
        }
    }

    inline void remove(int32_t node, int32_t comm, fp_t dnodecomm) {
        in_[comm]  -= fp_t(2) * dnodecomm + g.nb_selfloops(node);
        tot_[comm] -= g.weighted_degree(node);
        n2c[node] = -1;
    }
    inline void insert(int32_t node, int32_t comm, fp_t dnodecomm) {
        in_[comm]  += fp_t(2) * dnodecomm + g.nb_selfloops(node);
        tot_[comm] += g.weighted_degree(node);
        n2c[node] = comm;
    }
    inline fp_t gain(int32_t /*node*/, int32_t comm, fp_t dnc, fp_t w_degree) const {
        fp_t totc = tot_[comm];
        fp_t m2   = g.total_weight;
        return dnc - totc * w_degree / m2;
    }
    fp_t quality() const {
        fp_t q = fp_t(0);
        fp_t m2 = g.total_weight;
        for (int32_t i = 0; i < size; i++) {
            if (tot_[i] > fp_t(0)) q += in_[i] - (tot_[i] * tot_[i]) / m2;
        }
        return q / m2;
    }
};

// ───────────── Louvain driver (mirrors canonical louvain.{h,cpp}) ─
struct Louvain {
    Modularity* qual;
    int nb_pass;
    fp_t eps_impr;
    std::vector<fp_t>    neigh_weight;
    std::vector<int32_t> neigh_pos;
    int neigh_last;

    Louvain(int nbp, fp_t eps, Modularity* q)
        : qual(q), nb_pass(nbp), eps_impr(eps), neigh_last(0) {
        neigh_weight.assign(qual->size, fp_t(-1));
        neigh_pos.assign(qual->size, 0);
    }

    // Canonical louvain.cpp:78-105.
    void neigh_comm(int32_t node) {
        for (int i = 0; i < neigh_last; i++) neigh_weight[neigh_pos[i]] = fp_t(-1);
        neigh_last = 0;
        const Graph& g = qual->g;
        int32_t deg = g.nb_neighbors(node);
        neigh_pos[0] = qual->n2c[node];
        neigh_weight[neigh_pos[0]] = fp_t(0);
        neigh_last = 1;
        for (int32_t i = 0; i < deg; i++) {
            int32_t neigh = g.adjN[node][i];
            int32_t neigh_comm_id = qual->n2c[neigh];
            fp_t    neigh_w = g.adjW[node][i];
            if (neigh != node) {
                if (neigh_weight[neigh_comm_id] == fp_t(-1)) {
                    neigh_weight[neigh_comm_id] = fp_t(0);
                    neigh_pos[neigh_last++] = neigh_comm_id;
                }
                neigh_weight[neigh_comm_id] += neigh_w;
            }
        }
    }

    // Canonical louvain.cpp:147-211: renumber by original-id ASC, build
    // new Graph aggregating per-target std::map<int, fp_t>. For each
    // comm c, m's keys iterate target-id ASC; emit (c, target, w) per
    // entry. canonical's display_binary stores g2.links per-direction
    // ONCE (each pair (c0,c1) stored as c1 in adj[c0] and c0 in adj[c1],
    // single entry each). Self-loop in adj of c stored ONCE with weight
    // 2·intra_c (m's c-self entry captures both walks via per-endpoint
    // adj iteration). Used in BOTH modes — algorithmic convention is
    // canonical-faithful regardless of toggle.
    Graph partition2graph_binary() {
        const Graph& g = qual->g;
        std::vector<int32_t> renumber(qual->size, -1);
        for (int32_t node = 0; node < qual->size; node++) renumber[qual->n2c[node]] = 1;
        int32_t last = 0;
        for (int32_t i = 0; i < qual->size; i++) {
            if (renumber[i] != -1) renumber[i] = last++;
        }
        std::vector<std::vector<int32_t>> comm_nodes(last);
        std::vector<int32_t> comm_weight(last, 0);
        for (int32_t node = 0; node < qual->size; node++) {
            int32_t c = renumber[qual->n2c[node]];
            comm_nodes[c].push_back(node);
            comm_weight[c] += g.nodes_w[node];
        }
        Graph g2;
        g2.n = last;
        g2.adjN.assign(last, {});
        g2.adjW.assign(last, {});
        g2.nodes_w = comm_weight;
        g2.eu.clear(); g2.ev.clear(); g2.ew.clear();
        g2.m = 0;
        g2.total_weight = fp_t(0);
        for (int32_t c = 0; c < last; c++) {
            std::map<int32_t, fp_t> m;
            for (size_t k = 0; k < comm_nodes[c].size(); k++) {
                int32_t node = comm_nodes[c][k];
                int32_t deg = g.nb_neighbors(node);
                for (int32_t i = 0; i < deg; i++) {
                    int32_t neigh = g.adjN[node][i];
                    int32_t neigh_c = renumber[qual->n2c[neigh]];
                    fp_t    w = g.adjW[node][i];
                    auto it = m.find(neigh_c);
                    if (it == m.end()) m.insert(std::make_pair(neigh_c, w));
                    else it->second += w;
                }
            }
            // Emit per-direction ONCE in adj[c] (mirror canonical
            // g2.links/weights). m iteration is target-id ASC by std::map
            // key order — adj is naturally sorted target-id ASC.
            for (auto it = m.begin(); it != m.end(); ++it) {
                g2.adjN[c].push_back(it->first);
                g2.adjW[c].push_back(it->second);
                g2.total_weight += it->second;
                g2.eu.push_back(c);
                g2.ev.push_back(it->first);
                g2.ew.push_back(it->second);
                g2.m++;
            }
        }
        return g2;
    }
};

// ───────────── Trace records ─────────────────────────────────────
struct VisitRecord {
    int32_t pass;
    int32_t visit;
    int32_t node;
    int32_t fromComm;
    int32_t toComm;
    bool moved;
    uint64_t dGainBits;   // bits(best_increase) — canonical gain units
    uint64_t dQbits;      // bits(best_increase / m2) — Q units
    // Running-tracker probes — running tracker state immediately AFTER
    // insert(node, best_comm, ...). Captures inC/totC for both the
    // source comm (post-remove minus this insert) and the target comm
    // (post-insert). Used to detect sub-ulp drift in admin accumulators
    // that doesn't flip move decisions but corrupts Q_final.
    uint64_t inC_from_bits;
    uint64_t inC_to_bits;
    uint64_t totC_from_bits;
    uint64_t totC_to_bits;
};
struct LevelTrace {
    int32_t level;
    int32_t n_before;
    int32_t passes;
    std::vector<std::vector<int32_t>> visit_order_per_pass;
    std::vector<VisitRecord> visits;
    std::vector<int32_t> n2c_post;
    // Per-pass quality after each pass completes (bit-equal compare).
    std::vector<uint64_t> quality_per_pass_bits;
    // Per-level total_weight (input graph + post-collapse).
    uint64_t total_weight_bits_pre;
    uint64_t total_weight_bits_post;  // 0 if last level.
    // Post-collapse n.
    int32_t n_after_collapse;
};

static int32_t one_level_trace(Louvain& c, RNG_t& rng, int32_t /*levelIdx*/,
                               LevelTrace& lvl) {
    bool improvement = false;
    int nb_moves;
    int nb_pass_done = 0;
    fp_t new_qual = c.qual->quality();
    fp_t cur_qual = new_qual;

    // Canonical louvain.cpp:221-229. Shuffle direction matches canonical.
    // Under TRACER_MODE the int_inclusive(i, size-1) call routes through
    // MT19937 + rejection sampling; under CANONICAL_MODE through libc
    // rand() % (size-i) + i (no rejection).
    int32_t size = c.qual->size;
    std::vector<int32_t> random_order(size);
    for (int32_t i = 0; i < size; i++) random_order[i] = i;
    for (int32_t i = 0; i < size - 1; i++) {
        int32_t rand_pos = rng.int_inclusive(i, size - 1);
        int32_t tmp = random_order[i];
        random_order[i] = random_order[rand_pos];
        random_order[rand_pos] = tmp;
    }

    do {
        cur_qual = new_qual;
        nb_moves = 0;
        nb_pass_done++;
        std::vector<int32_t> visit_order = random_order;
        lvl.visit_order_per_pass.push_back(visit_order);
        for (int32_t node_tmp = 0; node_tmp < size; node_tmp++) {
            int32_t node = random_order[node_tmp];
            int32_t node_comm = c.qual->n2c[node];
            fp_t    w_degree = c.qual->g.weighted_degree(node);
            c.neigh_comm(node);
            c.qual->remove(node, node_comm, c.neigh_weight[node_comm]);
            int32_t best_comm = node_comm;
            fp_t    best_nblinks  = fp_t(0);
            fp_t    best_increase = fp_t(0);
            for (int32_t i = 0; i < c.neigh_last; i++) {
                fp_t increase = c.qual->gain(node, c.neigh_pos[i],
                                             c.neigh_weight[c.neigh_pos[i]], w_degree);
                if (increase > best_increase) {
                    best_comm = c.neigh_pos[i];
                    best_nblinks = c.neigh_weight[c.neigh_pos[i]];
                    best_increase = increase;
                }
            }
            c.qual->insert(node, best_comm, best_nblinks);
            VisitRecord vr;
            vr.pass = nb_pass_done - 1;
            vr.visit = node_tmp;
            vr.node = node;
            vr.fromComm = node_comm;
            vr.toComm = best_comm;
            vr.moved = best_comm != node_comm;
            vr.dGainBits = bits_of_fp(vr.moved ? best_increase : fp_t(0));
            fp_t m2 = c.qual->g.total_weight;
            vr.dQbits = bits_of_fp(vr.moved ? (best_increase / m2) : fp_t(0));
            // Running-tracker state post-insert. node_comm = source
            // (post-remove); best_comm = target (post-insert). Note: when
            // moved==false, node_comm == best_comm; both reflect the
            // self-restore (canonical insert(node, vComm, 0) on no-move).
            vr.inC_from_bits  = bits_of_fp(c.qual->in_[node_comm]);
            vr.inC_to_bits    = bits_of_fp(c.qual->in_[best_comm]);
            vr.totC_from_bits = bits_of_fp(c.qual->tot_[node_comm]);
            vr.totC_to_bits   = bits_of_fp(c.qual->tot_[best_comm]);
            lvl.visits.push_back(vr);
            if (best_comm != node_comm) nb_moves++;
        }
        new_qual = c.qual->quality();
        lvl.quality_per_pass_bits.push_back(bits_of_fp(new_qual));
        if (nb_moves > 0) improvement = true;
    } while (nb_moves > 0 && new_qual - cur_qual > c.eps_impr);

    lvl.passes = nb_pass_done;
    return improvement ? 1 : 0;
}

// ───────────── CSV parse + main ──────────────────────────────────
static void read_edge_csv(const std::string& path,
                          std::vector<std::tuple<int32_t,int32_t,fp_t>>& edges,
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
        // Header lines fail to parse two ints + are skipped.
        if (!(ss >> u >> v)) continue;
        double w_d = 1.0;
        ss >> w_d;
        edges.emplace_back(u, v, fp_t(w_d));
        if (u > maxNode) maxNode = u;
        if (v > maxNode) maxNode = v;
    }
    n_out = maxNode + 1;
}

int main(int argc, char** argv) {
    if (argc < 2) {
#if defined(LV_CANONICAL_MODE)
        std::fprintf(stderr, "usage: %s <edge.csv>  (seed via LD_PRELOAD shim + $LOUVAIN_SEED)\n", argv[0]);
#else
        std::fprintf(stderr, "usage: %s <edge.csv> <seed>\n", argv[0]);
#endif
        return 2;
    }
    std::string edgePath = argv[1];

    std::vector<std::tuple<int32_t,int32_t,fp_t>> edges;
    int32_t n = 0;
    read_edge_csv(edgePath, edges, n);

    Graph g0;
    g0.build(n, edges);
    int32_t n_orig = n;

#if defined(LV_CANONICAL_MODE)
    // Canonical mode: seed init mirrors gen-louvain main_louvain.cpp:245
    // (`srand(time(NULL)+getpid())`). The LD_PRELOAD seed shim then
    // intercepts and substitutes $LOUVAIN_SEED. We pass a placeholder
    // here; the shim overrides. If argv has a seed argument, pass it as
    // a fallback (for runs without the shim).
    uint32_t fallback_seed = 0;
    if (argc >= 3) fallback_seed = (uint32_t)std::strtoul(argv[2], nullptr, 10);
    else fallback_seed = (uint32_t)((std::time(nullptr) + (long)getpid()) & 0xffffffffu);
    RNG_t rng(fallback_seed);
    uint32_t seed_for_json = fallback_seed;
#else
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <edge.csv> <seed>\n", argv[0]);
        return 2;
    }
    uint32_t seed_for_json = (uint32_t)std::strtoul(argv[2], nullptr, 10);
    RNG_t rng(seed_for_json);
#endif

    fp_t precision = fp_t(1e-6);

    // Outer level loop (canonical main_louvain.cpp:273-310).
    Graph g = g0;
    Modularity* q = new Modularity(g);
    Louvain c(-1, precision, q);
    bool improvement = true;

    std::vector<LevelTrace> levels;
    std::vector<int32_t> fineMembership(n_orig);
    for (int32_t i = 0; i < n_orig; i++) fineMembership[i] = i;
    int32_t level = 0;
    while (improvement) {
        int32_t n_before = q->size;
        LevelTrace lvl;
        lvl.level = level;
        lvl.n_before = n_before;
        lvl.total_weight_bits_pre = bits_of_fp(q->g.total_weight);
        lvl.total_weight_bits_post = 0;
        lvl.n_after_collapse = 0;
        improvement = (one_level_trace(c, rng, level, lvl) != 0);
        // Snapshot pre-renumber n2c for trace.
        lvl.n2c_post.assign(q->size, 0);
        for (int32_t i = 0; i < q->size; i++) lvl.n2c_post[i] = q->n2c[i];
        // Compose fineMembership: for each original node v, follow its
        // current super-vertex through the renumber that
        // partition2graph_binary will apply.
        std::vector<int32_t> renumber(q->size, -1);
        for (int32_t v = 0; v < q->size; v++) renumber[q->n2c[v]] = 1;
        int32_t last = 0;
        for (int32_t i = 0; i < q->size; i++) {
            if (renumber[i] != -1) renumber[i] = last++;
        }
        // fineMembership[v] currently holds v's super-vertex at this
        // level; promote it via the renumber to the next level's id.
        std::vector<int32_t> new_fine(n_orig);
        for (int32_t v = 0; v < n_orig; v++) {
            int32_t curr = fineMembership[v];
            int32_t comm = q->n2c[curr];
            new_fine[v] = renumber[comm];
        }
        // Aggregate to the next level.
        Graph g_next = c.partition2graph_binary();
        lvl.total_weight_bits_post = bits_of_fp(g_next.total_weight);
        lvl.n_after_collapse = (int32_t)g_next.n;
        levels.push_back(std::move(lvl));
        if ((int32_t)g_next.n >= n_before || g_next.n <= 1) {
            fineMembership = new_fine;
            level++;
            break;
        }
        fineMembership = new_fine;
        delete q;
        g = std::move(g_next);
        q = new Modularity(g);
        c = Louvain(-1, precision, q);
        level++;
        if (level > 100) break;
    }

    // Final partition: map fineMembership through one final renumber so
    // surviving comms get contiguous 0..K-1 ids in original-id-ASC
    // order (matches canonical run_louvain.py output convention).
    std::vector<int32_t> finalRenum(n_orig, -1);
    for (int32_t v = 0; v < n_orig; v++) finalRenum[fineMembership[v]] = 1;
    int32_t kFinal = 0;
    for (int32_t i = 0; i < n_orig; i++) {
        if (finalRenum[i] != -1) finalRenum[i] = kFinal++;
    }
    std::vector<int32_t> finalMembership(n_orig);
    for (int32_t v = 0; v < n_orig; v++) finalMembership[v] = finalRenum[fineMembership[v]];
    delete q;
    // Compute Q on (original graph, composed fine_membership). JS does
    // the same: it constructs Partition(graph, fineMembership, q),
    // renumbers, and reports P.quality(). Computing Q on the LAST-LEVEL
    // (graph, partition) is equivalent in exact arithmetic but FP-
    // divergent because in_/tot_ accumulated through every move
    // suffer different rounding than rebuilt-from-membership.
    Modularity Qfinal_mod(g0);
    // rebuildAdmin equivalent: re-init in/tot from finalMembership.
    for (int32_t i = 0; i < g0.n; i++) {
        Qfinal_mod.n2c[i] = finalMembership[i];
        Qfinal_mod.in_[i]  = fp_t(0);
        Qfinal_mod.tot_[i] = fp_t(0);
    }
    for (int32_t v = 0; v < g0.n; v++) {
        Qfinal_mod.tot_[finalMembership[v]] += g0.weighted_degree(v);
    }
    for (int32_t e = 0; e < g0.m; e++) {
        int32_t u = g0.eu[e], v = g0.ev[e];
        fp_t    w = g0.ew[e];
        int32_t cu = finalMembership[u], cv = finalMembership[v];
        if (u == v) {
            if (cu == cv) Qfinal_mod.in_[cu] += w;
        } else if (cu == cv) {
            Qfinal_mod.in_[cu] += fp_t(2) * w;
        }
    }
    fp_t Q_final = Qfinal_mod.quality();

    // ─── Emit JSON ───
    const char* mode_tag =
#if defined(LV_CANONICAL_MODE)
        "canonical"
#else
        "swapped"
#endif
        ;
    std::cout << "{";
    std::cout << "\"mode\":\"" << mode_tag << "\",";
    std::cout << "\"seed\":" << seed_for_json << ",";
    std::cout << "\"n\":" << n_orig << ",";
    std::cout << "\"levels\":[";
    for (size_t li = 0; li < levels.size(); li++) {
        const LevelTrace& lv = levels[li];
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
            char hex_g[32], hex_q[32];
            char hex_if[32], hex_it[32], hex_tf[32], hex_tt[32];
            std::snprintf(hex_g, sizeof(hex_g), "0x%016llx", (unsigned long long)vr.dGainBits);
            std::snprintf(hex_q, sizeof(hex_q), "0x%016llx", (unsigned long long)vr.dQbits);
            std::snprintf(hex_if, sizeof(hex_if), "0x%016llx", (unsigned long long)vr.inC_from_bits);
            std::snprintf(hex_it, sizeof(hex_it), "0x%016llx", (unsigned long long)vr.inC_to_bits);
            std::snprintf(hex_tf, sizeof(hex_tf), "0x%016llx", (unsigned long long)vr.totC_from_bits);
            std::snprintf(hex_tt, sizeof(hex_tt), "0x%016llx", (unsigned long long)vr.totC_to_bits);
            std::cout << "{\"pass\":" << vr.pass
                      << ",\"visit\":" << vr.visit
                      << ",\"v\":" << vr.node
                      << ",\"fromComm\":" << vr.fromComm
                      << ",\"toComm\":" << vr.toComm
                      << ",\"moved\":" << (vr.moved ? "true" : "false")
                      << ",\"dGainBits\":\"" << hex_g << "\""
                      << ",\"dSbits\":\"" << hex_q << "\""
                      << ",\"inCfromBits\":\"" << hex_if << "\""
                      << ",\"inCtoBits\":\"" << hex_it << "\""
                      << ",\"totCfromBits\":\"" << hex_tf << "\""
                      << ",\"totCtoBits\":\"" << hex_tt << "\"}";
        }
        std::cout << "],";
        std::cout << "\"n2c_post\":[";
        for (size_t i = 0; i < lv.n2c_post.size(); i++) {
            if (i) std::cout << ",";
            std::cout << lv.n2c_post[i];
        }
        std::cout << "],";
        std::cout << "\"qualityPerPassBits\":[";
        for (size_t i = 0; i < lv.quality_per_pass_bits.size(); i++) {
            if (i) std::cout << ",";
            char hex_qp[32];
            std::snprintf(hex_qp, sizeof(hex_qp), "0x%016llx", (unsigned long long)lv.quality_per_pass_bits[i]);
            std::cout << "\"" << hex_qp << "\"";
        }
        std::cout << "],";
        char hex_twp[32], hex_tw2[32];
        std::snprintf(hex_twp, sizeof(hex_twp), "0x%016llx", (unsigned long long)lv.total_weight_bits_pre);
        std::snprintf(hex_tw2, sizeof(hex_tw2), "0x%016llx", (unsigned long long)lv.total_weight_bits_post);
        std::cout << "\"totalWeightBitsPre\":\"" << hex_twp << "\",";
        std::cout << "\"totalWeightBitsPost\":\"" << hex_tw2 << "\",";
        std::cout << "\"nAfterCollapse\":" << lv.n_after_collapse;
        std::cout << "}";
    }
    std::cout << "],";
    std::cout << "\"fine_membership_post_renumber\":[";
    for (size_t i = 0; i < finalMembership.size(); i++) {
        if (i) std::cout << ",";
        std::cout << finalMembership[i];
    }
    std::cout << "],";
    char qb[32]; std::snprintf(qb, sizeof(qb), "0x%016llx", (unsigned long long)bits_of_fp(Q_final));
    std::cout << "\"Q_final_bits\":\"" << qb << "\"";
    std::cout << "}\n";
    return 0;
}
