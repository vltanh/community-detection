// Louvain L4 self-RNG bit-equal-per-step tracer.
//
// Single-file C++ build. The algorithm body is a faithful port of
// externals/louvain (Blondel-Guillaume-Lambiotte-Lefebvre 2008,
// gen-louvain v0.3 — files referenced inline by canonical line below).
// Two substitutions vs upstream:
//   1. `long double` -> `double` everywhere (so JS Float64 can reach
//      bit-equality; upstream's 80-bit x87 extended precision is the
//      primary obstacle to byte-identity vs JS).
//   2. `libc rand()` -> inline MT19937 (matches the JS port's RNG so
//      both sides produce the same shuffle sequence under matching
//      seed; libc rand is glibc-specific and not reachable from JS).
// Everything else — Modularity::in/tot admin, neigh_comm trail layout,
// remove + gain + insert sequence, partition2graph_binary renumber by
// original-id ASC, std::map-keyed aggregation in collapse — is verbatim
// canonical.
//
// Output: stdout JSON of the trace described below.
// Build: instrumented/build_l4.sh -> /tmp/louvain_l4_tracer
// Run:   /tmp/louvain_l4_tracer <edge.csv> <seed>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using std::int32_t;
using std::int64_t;
using std::size_t;
using std::uint32_t;
using std::uint64_t;

static inline uint64_t bits_of(double x) {
    uint64_t b; std::memcpy(&b, &x, sizeof(b)); return b;
}

// ───────────── MT19937 (drop-in for libc rand) ───────────────────
// Mirrors comdet/js/louvain/louvain.js MT19937 byte-for-byte. int(lo,hi)
// uses rejection sampling identical to the JS side. The canonical
// upstream uses libc rand() at louvain.cpp:225; this build replaces
// rand()%(qual->size-i)+i with MT19937::int_inclusive(i, qual->size-1).
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

// ───────────── Graph (mirrors canonical graph_binary.{h,cpp}) ────
// Same adjacency layout as canonical: each non-self edge appears in
// both endpoint lists; self-loops appear ONCE. weighted_degree(v) sums
// adj weights so a self-loop contributes once. total_weight = Σ
// weighted_degree(v).
struct Graph {
    int32_t n;
    int32_t m;
    std::vector<int32_t> eu, ev;
    std::vector<double>  ew;
    std::vector<std::vector<int32_t>> adjN;
    std::vector<std::vector<double>>  adjW;
    std::vector<int32_t> nodes_w;
    double total_weight;

    Graph() : n(0), m(0), total_weight(0.0) {}

    void build(int32_t n_, const std::vector<std::tuple<int32_t,int32_t,double>>& edges,
               const std::vector<int32_t>* nodes_w_ = nullptr) {
        n = n_;
        m = (int32_t)edges.size();
        eu.assign(m, 0); ev.assign(m, 0); ew.assign(m, 0.0);
        for (int32_t i = 0; i < m; i++) {
            eu[i] = std::get<0>(edges[i]);
            ev[i] = std::get<1>(edges[i]);
            ew[i] = std::get<2>(edges[i]);
        }
        adjN.assign(n, {});
        adjW.assign(n, {});
        for (int32_t e = 0; e < m; e++) {
            int32_t u = eu[e], v = ev[e];
            double  w = ew[e];
            adjN[u].push_back(v); adjW[u].push_back(w);
            if (u != v) {
                adjN[v].push_back(u); adjW[v].push_back(w);
            }
        }
        if (nodes_w_) nodes_w = *nodes_w_;
        else nodes_w.assign(n, 1);
        total_weight = 0.0;
        for (int32_t v = 0; v < n; v++) {
            double s = 0.0;
            for (size_t i = 0; i < adjW[v].size(); i++) s += adjW[v][i];
            total_weight += s;
        }
    }

    int32_t nb_neighbors(int32_t v) const { return (int32_t)adjN[v].size(); }
    double weighted_degree(int32_t v) const {
        double s = 0.0;
        for (size_t i = 0; i < adjW[v].size(); i++) s += adjW[v][i];
        return s;
    }
    double nb_selfloops(int32_t v) const {
        for (size_t i = 0; i < adjN[v].size(); i++) {
            if (adjN[v][i] == v) return adjW[v][i];
        }
        return 0.0;
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
    std::vector<double>  in_;
    std::vector<double>  tot_;

    explicit Modularity(Graph& gr) : g(gr), size(gr.n) {
        n2c.assign(size, 0);
        in_.assign(size, 0.0);
        tot_.assign(size, 0.0);
        for (int32_t i = 0; i < size; i++) {
            n2c[i] = i;
            in_[i]  = g.nb_selfloops(i);
            tot_[i] = g.weighted_degree(i);
        }
    }

    inline void remove(int32_t node, int32_t comm, double dnodecomm) {
        in_[comm]  -= 2.0 * dnodecomm + g.nb_selfloops(node);
        tot_[comm] -= g.weighted_degree(node);
        n2c[node] = -1;
    }
    inline void insert(int32_t node, int32_t comm, double dnodecomm) {
        in_[comm]  += 2.0 * dnodecomm + g.nb_selfloops(node);
        tot_[comm] += g.weighted_degree(node);
        n2c[node] = comm;
    }
    inline double gain(int32_t /*node*/, int32_t comm, double dnc, double w_degree) const {
        double totc = tot_[comm];
        double m2   = g.total_weight;
        return dnc - totc * w_degree / m2;
    }
    double quality() const {
        double q = 0.0;
        double m2 = g.total_weight;
        for (int32_t i = 0; i < size; i++) {
            if (tot_[i] > 0.0) q += in_[i] - (tot_[i] * tot_[i]) / m2;
        }
        return q / m2;
    }
};

// ───────────── Louvain driver (mirrors canonical louvain.{h,cpp}) ─
struct Louvain {
    Modularity* qual;
    int nb_pass;
    double eps_impr;
    std::vector<double>  neigh_weight;
    std::vector<int32_t> neigh_pos;
    int neigh_last;

    Louvain(int nbp, double eps, Modularity* q)
        : qual(q), nb_pass(nbp), eps_impr(eps), neigh_last(0) {
        neigh_weight.assign(qual->size, -1.0);
        neigh_pos.assign(qual->size, 0);
    }

    // Canonical louvain.cpp:78-105.
    void neigh_comm(int32_t node) {
        for (int i = 0; i < neigh_last; i++) neigh_weight[neigh_pos[i]] = -1.0;
        neigh_last = 0;
        const Graph& g = qual->g;
        int32_t deg = g.nb_neighbors(node);
        neigh_pos[0] = qual->n2c[node];
        neigh_weight[neigh_pos[0]] = 0.0;
        neigh_last = 1;
        for (int32_t i = 0; i < deg; i++) {
            int32_t neigh = g.adjN[node][i];
            int32_t neigh_comm_id = qual->n2c[neigh];
            double  neigh_w = g.adjW[node][i];
            if (neigh != node) {
                if (neigh_weight[neigh_comm_id] == -1.0) {
                    neigh_weight[neigh_comm_id] = 0.0;
                    neigh_pos[neigh_last++] = neigh_comm_id;
                }
                neigh_weight[neigh_comm_id] += neigh_w;
            }
        }
    }

    // Canonical louvain.cpp:147-211: renumber by original-id ASC, build
    // new Graph aggregating per-target std::map<int, double>. Each
    // intra edge contributes from BOTH endpoints' walks → super-self-
    // loop weight = 2·intra_c. Each inter pair (a,b) appears in both
    // a's and b's m[], producing both directions in the new adj list.
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
        std::vector<std::tuple<int32_t,int32_t,double>> newEdges;
        for (int32_t c = 0; c < last; c++) {
            std::map<int32_t, double> m;
            for (size_t k = 0; k < comm_nodes[c].size(); k++) {
                int32_t node = comm_nodes[c][k];
                int32_t deg = g.nb_neighbors(node);
                for (int32_t i = 0; i < deg; i++) {
                    int32_t neigh = g.adjN[node][i];
                    int32_t neigh_c = renumber[qual->n2c[neigh]];
                    double  w = g.adjW[node][i];
                    auto it = m.find(neigh_c);
                    if (it == m.end()) m.insert(std::make_pair(neigh_c, w));
                    else it->second += w;
                }
            }
            for (auto it = m.begin(); it != m.end(); ++it) {
                newEdges.emplace_back(c, it->first, it->second);
            }
        }
        Graph g2;
        g2.build(last, newEdges, &comm_weight);
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
};
struct LevelTrace {
    int32_t level;
    int32_t n_before;
    int32_t passes;
    std::vector<std::vector<int32_t>> visit_order_per_pass;
    std::vector<VisitRecord> visits;
    std::vector<int32_t> n2c_post;
};

static int32_t one_level_trace(Louvain& c, MT19937& rng, int32_t levelIdx,
                               LevelTrace& lvl) {
    bool improvement = false;
    int nb_moves;
    int nb_pass_done = 0;
    double new_qual = c.qual->quality();
    double cur_qual = new_qual;

    // Canonical louvain.cpp:221-229. random_order shuffle. Drop libc
    // rand for MT19937; loop direction matches canonical exactly.
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
            double  w_degree = c.qual->g.weighted_degree(node);
            c.neigh_comm(node);
            c.qual->remove(node, node_comm, c.neigh_weight[node_comm]);
            int32_t best_comm = node_comm;
            double  best_nblinks  = 0.0;
            double  best_increase = 0.0;
            for (int32_t i = 0; i < c.neigh_last; i++) {
                double increase = c.qual->gain(node, c.neigh_pos[i],
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
            vr.dGainBits = bits_of(vr.moved ? best_increase : 0.0);
            double m2 = c.qual->g.total_weight;
            vr.dQbits = bits_of(vr.moved ? (best_increase / m2) : 0.0);
            lvl.visits.push_back(vr);
            if (best_comm != node_comm) nb_moves++;
        }
        new_qual = c.qual->quality();
        if (nb_moves > 0) improvement = true;
    } while (nb_moves > 0 && new_qual - cur_qual > c.eps_impr);

    lvl.passes = nb_pass_done;
    return improvement ? 1 : 0;
}

// ───────────── CSV parse + main ──────────────────────────────────
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
        // Header lines fail to parse two ints + are skipped.
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

    Graph g0;
    g0.build(n, edges);
    int32_t n_orig = n;

    MT19937 rng(seed);
    double precision = 1e-6;

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
        levels.push_back(std::move(lvl));
        // Aggregate to the next level.
        Graph g_next = c.partition2graph_binary();
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
        Qfinal_mod.in_[i]  = 0.0;
        Qfinal_mod.tot_[i] = 0.0;
    }
    for (int32_t v = 0; v < g0.n; v++) {
        Qfinal_mod.tot_[finalMembership[v]] += g0.weighted_degree(v);
    }
    for (int32_t e = 0; e < g0.m; e++) {
        int32_t u = g0.eu[e], v = g0.ev[e];
        double  w = g0.ew[e];
        int32_t cu = finalMembership[u], cv = finalMembership[v];
        if (u == v) {
            if (cu == cv) Qfinal_mod.in_[cu] += w;
        } else if (cu == cv) {
            Qfinal_mod.in_[cu] += 2.0 * w;
        }
    }
    double Q_final = Qfinal_mod.quality();

    // ─── Emit JSON ───
    std::cout << "{";
    std::cout << "\"seed\":" << seed << ",";
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
            std::snprintf(hex_g, sizeof(hex_g), "0x%016llx", (unsigned long long)vr.dGainBits);
            std::snprintf(hex_q, sizeof(hex_q), "0x%016llx", (unsigned long long)vr.dQbits);
            std::cout << "{\"pass\":" << vr.pass
                      << ",\"visit\":" << vr.visit
                      << ",\"v\":" << vr.node
                      << ",\"fromComm\":" << vr.fromComm
                      << ",\"toComm\":" << vr.toComm
                      << ",\"moved\":" << (vr.moved ? "true" : "false")
                      << ",\"dGainBits\":\"" << hex_g << "\""
                      << ",\"dSbits\":\"" << hex_q << "\"}";
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
    for (size_t i = 0; i < finalMembership.size(); i++) {
        if (i) std::cout << ",";
        std::cout << finalMembership[i];
    }
    std::cout << "],";
    char qb[32]; std::snprintf(qb, sizeof(qb), "0x%016llx", (unsigned long long)bits_of(Q_final));
    std::cout << "\"Q_final_bits\":\"" << qb << "\"";
    std::cout << "}\n";
    return 0;
}
