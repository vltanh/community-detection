// Standalone instrumented SBM mcmc_sweep mirror of comdet/js/sbm
// (release-2.98 graph-tool semantics, restricted to the JS port's path:
// uniform proposal at c->infty, single-vertex Metropolis-Hastings,
// exact microcanonical entropy per Peixoto Eq 22 (NDC) / Eq 43 (DC) /
// Zhang+Peixoto 2020 (PP)). RNG: std::mt19937 (NOT graph-tool's
// pcg64_k1024). Per-visit JSON trace emitted to stdout so the JS
// replay leg (tools/viz_check/sbm/<variant>/kernel_check.mjs) can
// drive its own mcmc_sweep through opts.proposalOracle and verify
// byte-equal trace + final partition against this leg.
//
// The JS Σ at any fixed partition is byte-equal to graph-tool's
// state.entropy(exact=True, deg_dl_kind="uniform") (deterministic
// part); see tools/viz_check/sbm/<variant>/entropy_check.py. The MCMC
// trace itself is byte-equal between THIS binary and the JS replay,
// not vs graph-tool's pipeline (which uses pcg64 + multilevel agglom
// merges absent from the JS port).
//
// Build:  g++ -std=c++20 -O2 -o /tmp/sbm_flat_kernel_check flat_traced.cpp
// Run:
//   sbm_flat_kernel_check --mode=dc|ndc|pp --edge=path --init=path
//                         --seed=N --sweeps=K [--beta=1.0]
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace {

// ── JS-compatible RNG wrapper ───────────────────────────────────────
// std::mt19937(seed) seeds the state via the same Knuth-style recurrence
// that comdet/js/louvain/louvain.js's MT19937 init uses, so the 624-word
// state is byte-equal to JS's after construction. raw() returns the
// next 32-bit output, intRange uses the same rejection algorithm as
// JS's rng.int(lo, hi).
struct JSRng {
    std::mt19937 mt;
    explicit JSRng(uint32_t s) : mt(s) {}
    uint32_t raw() { return mt(); }
    int32_t intRange(int32_t lo, int32_t hi) {
        int64_t range = (int64_t)hi - (int64_t)lo + 1;
        if (range <= 0) return lo;
        uint64_t limit = (0x100000000ULL / (uint64_t)range) * (uint64_t)range;
        uint32_t r;
        do { r = mt(); } while ((uint64_t)r >= limit);
        return lo + (int32_t)(r % (uint64_t)range);
    }
    double acceptUniform() { return (double)mt() / 4294967296.0; }
};

// JS Fisher-Yates: idx = n-1 .. 1, j = int(0, idx), swap(arr[idx], arr[j]).
template <class T>
void jsShuffle(std::vector<T>& a, JSRng& rng) {
    for (int32_t idx = (int32_t)a.size() - 1; idx >= 1; --idx) {
        int32_t j = rng.intRange(0, idx);
        std::swap(a[idx], a[j]);
    }
}

// ── Edge / partition I/O ────────────────────────────────────────────
struct Graph {
    int N;
    std::vector<std::vector<int>> nb;       // neighbours
    std::vector<std::vector<double>> nbW;   // weights aligned with nb
    std::vector<double> selfW;              // per-node accumulated self-loop weight
    std::vector<double> strength;           // sum_e w with self-loops counted twice
    std::vector<std::string> idToOrig;      // renumber[i] = orig string id
    double E = 0.0;
};

// Renumber nodes by first-seen order in edge.csv (same as
// constrained-clustering's GetOriginalToNewIdMap, but iteration order
// is preserved from the file rather than std::map's lexicographic
// sort). The JS replay loads edges via the renum_to_orig map echoed
// in the trace.
Graph loadEdges(const std::string& path) {
    Graph g;
    std::map<std::string, int> idx;
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot open edge: " + path);
    std::string line;
    bool header = true;
    std::vector<std::pair<int,int>> es;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        if (header) { header = false; continue; }
        std::string a, b;
        size_t c = line.find(',');
        if (c == std::string::npos) { c = line.find('\t'); }
        if (c == std::string::npos) throw std::runtime_error("bad edge line: " + line);
        a = line.substr(0, c);
        b = line.substr(c + 1);
        // Strip trailing CR/LF/spaces.
        while (!b.empty() && (b.back() == '\r' || b.back() == '\n' || b.back() == ' ')) b.pop_back();
        while (!a.empty() && (a.back() == ' ')) a.pop_back();
        auto reg = [&](const std::string& s) {
            auto it = idx.find(s);
            if (it != idx.end()) return it->second;
            int n = (int)idx.size();
            idx[s] = n;
            g.idToOrig.push_back(s);
            return n;
        };
        int u = reg(a), v = reg(b);
        es.emplace_back(u, v);
    }
    g.N = (int)idx.size();
    g.nb.assign(g.N, {});
    g.nbW.assign(g.N, {});
    g.selfW.assign(g.N, 0.0);
    g.strength.assign(g.N, 0.0);
    for (auto& [u, v] : es) {
        if (u == v) {
            g.nb[u].push_back(v);
            g.nbW[u].push_back(1.0);
            g.selfW[u] += 1.0;
            g.strength[u] += 2.0;
        } else {
            g.nb[u].push_back(v);
            g.nbW[u].push_back(1.0);
            g.nb[v].push_back(u);
            g.nbW[v].push_back(1.0);
            g.strength[u] += 1.0;
            g.strength[v] += 1.0;
        }
        g.E += 1.0;
    }
    return g;
}

// Read com.csv keyed on the same orig_id strings used by edge.csv.
// Returns membership[N], renumbered to 0..K-1 in first-occurrence
// order (matches the JS port's rebuildFromMembership and graph-tool's
// BlockState init from a vertex-property `b`). Negative cluster_ids
// in the file are treated as real labels (so multiple nodes with
// cluster_id=-1 end up in the same block); only nodes not present
// in the file are synthesized into fresh singleton blocks.
std::vector<int> loadInitMembership(const std::string& path,
                                    const std::vector<std::string>& idToOrig) {
    std::map<std::string, int> origToNew;
    for (int i = 0; i < (int)idToOrig.size(); ++i) origToNew[idToOrig[i]] = i;
    constexpr int64_t MISSING = (int64_t)1 << 33;
    std::vector<int64_t> raw(idToOrig.size(), MISSING);
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot open init: " + path);
    std::string line; bool header = true;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        if (header) { header = false; continue; }
        size_t c = line.find(',');
        if (c == std::string::npos) c = line.find('\t');
        if (c == std::string::npos) continue;
        std::string id = line.substr(0, c);
        std::string cl = line.substr(c + 1);
        while (!cl.empty() && (cl.back() == '\r' || cl.back() == '\n' || cl.back() == ' ')) cl.pop_back();
        auto it = origToNew.find(id);
        if (it == origToNew.end()) continue;
        raw[it->second] = std::atoll(cl.c_str());
    }
    // Synthesize fresh singleton labels (one per missing node) using a
    // disjoint sentinel range that cannot collide with any in-file value.
    constexpr int64_t SINGLETON_BASE = -((int64_t)1 << 40);
    int64_t nextSingleton = 0;
    for (int v = 0; v < (int)raw.size(); ++v) {
        if (raw[v] == MISSING) raw[v] = SINGLETON_BASE - (nextSingleton++);
    }
    std::map<int64_t, int> remap;
    std::vector<int> mem(raw.size());
    int K = 0;
    for (int v = 0; v < (int)raw.size(); ++v) {
        auto it = remap.find(raw[v]);
        if (it == remap.end()) { remap[raw[v]] = K; mem[v] = K; ++K; }
        else mem[v] = it->second;
    }
    return mem;
}

// ── BlockState ──────────────────────────────────────────────────────
// Mirrors comdet/js/sbm/block_state.js. Capacity B = N (max possible
// block count); ers stored row-major B*B with diagonal doubled to match
// graph-tool's e_rr "each internal edge contributes twice" convention.
enum class Mode { DC, NDC, PP };

struct BlockState {
    const Graph& g;
    Mode mode;
    bool useEdgesDl;
    bool usePartitionDl;
    bool useDegreeDl;
    int N, B;
    double E;
    std::vector<int> b;           // membership
    std::vector<int> nr;          // block size
    std::vector<double> er;       // sum_s ers[r,s]
    std::vector<double> ers;      // B*B
    std::vector<int> neList;      // active block ids
    std::vector<int> neIdx;       // r -> position in neList (-1 if empty)
    int Bne = 0;
    double dcDegreeConst = 0.0;
    static constexpr double LOG2 = 0.6931471805599453;

    BlockState(const Graph& g_, Mode mode_, const std::vector<int>& init)
        : g(g_), mode(mode_), N(g_.N), B(g_.N), E(g_.E),
          b(init), nr(B, 0), er(B, 0.0), ers((size_t)B * B, 0.0),
          neList(B, -1), neIdx(B, -1) {
        useEdgesDl = (mode != Mode::PP);
        usePartitionDl = true;
        useDegreeDl = (mode == Mode::DC);
        for (int v = 0; v < N; ++v) dcDegreeConst += std::lgamma(g.strength[v] + 1.0);
        rebuildFromMembership();
    }

    inline int blockOf(int v) const { return b[v]; }
    inline int blockSize(int r) const { return nr[r]; }
    void nonEmptyBlocks(std::vector<int>& out) const {
        out.assign(neList.begin(), neList.begin() + Bne);
    }

    void rebuildFromMembership() {
        // Renumber in first-occurrence order matching JS's seen-Map walk.
        std::map<int, int> seen;
        int next = 0;
        for (int v = 0; v < N; ++v) {
            int r = b[v];
            auto it = seen.find(r);
            if (it == seen.end()) { seen[r] = next; b[v] = next; ++next; }
            else b[v] = it->second;
        }
        std::fill(nr.begin(), nr.end(), 0);
        std::fill(er.begin(), er.end(), 0.0);
        std::fill(ers.begin(), ers.end(), 0.0);
        for (int v = 0; v < N; ++v) nr[b[v]] += 1;
        for (int v = 0; v < N; ++v) {
            int r = b[v];
            const auto& nb = g.nb[v];
            const auto& nbW = g.nbW[v];
            for (size_t i = 0; i < nb.size(); ++i) {
                int u = nb[i];
                if (u <= v) continue;
                double w = nbW[i];
                int s = b[u];
                ers[(size_t)r * B + s] += w;
                if (r != s) ers[(size_t)s * B + r] += w;
            }
            ers[(size_t)r * B + r] += g.selfW[v];
        }
        for (int r = 0; r < B; ++r) ers[(size_t)r * B + r] *= 2.0;
        Bne = 0;
        for (int r = 0; r < B; ++r) neIdx[r] = -1;
        for (int r = 0; r < B; ++r) {
            if (nr[r] > 0) {
                neIdx[r] = Bne;
                neList[Bne] = r;
                ++Bne;
            }
            double row = 0.0;
            for (int s = 0; s < B; ++s) row += ers[(size_t)r * B + s];
            er[r] = row;
        }
    }

    void moveVertex(int v, int s) {
        int r = b[v];
        if (r == s) return;
        const auto& nb = g.nb[v];
        const auto& nbW = g.nbW[v];
        for (size_t i = 0; i < nb.size(); ++i) {
            int u = nb[i];
            if (u == v) continue;
            double w = nbW[i];
            int t = b[u];
            if (t == r) {
                ers[(size_t)r * B + r] -= 2.0 * w;
                er[r] -= 2.0 * w;
            } else {
                ers[(size_t)r * B + t] -= w;
                ers[(size_t)t * B + r] -= w;
                er[r] -= w;
                er[t] -= w;
            }
            if (t == s) {
                ers[(size_t)s * B + s] += 2.0 * w;
                er[s] += 2.0 * w;
            } else {
                ers[(size_t)s * B + t] += w;
                ers[(size_t)t * B + s] += w;
                er[s] += w;
                er[t] += w;
            }
        }
        double sw = g.selfW[v];
        if (sw > 0.0) {
            ers[(size_t)r * B + r] -= 2.0 * sw;
            er[r] -= 2.0 * sw;
            ers[(size_t)s * B + s] += 2.0 * sw;
            er[s] += 2.0 * sw;
        }
        bool wasROccupied = nr[r] > 0;
        bool wasSOccupied = nr[s] > 0;
        nr[r] -= 1;
        nr[s] += 1;
        if (wasROccupied && nr[r] == 0) {
            int pos = neIdx[r];
            --Bne;
            int tail = neList[Bne];
            neList[pos] = tail;
            neIdx[tail] = pos;
            neIdx[r] = -1;
        }
        if (!wasSOccupied && nr[s] > 0) {
            neIdx[s] = Bne;
            neList[Bne] = s;
            ++Bne;
        }
        b[v] = s;
    }

    inline double safelog(double x) const { return x > 0.0 ? std::log(x) : 0.0; }
    inline double xlogx(double x) const { return x > 0.0 ? x * std::log(x) : 0.0; }

    // log binom(n, k) = lgamma(n+1) - lgamma(k+1) - lgamma(n-k+1).
    double lbinom(double n, double k) const {
        if (n < 0.0 || k < 0.0 || k > n) return 0.0;
        return std::lgamma(n + 1.0) - std::lgamma(k + 1.0) - std::lgamma(n - k + 1.0);
    }
    // logChooseRep(n,k) = log C(n+k-1, k), stars-and-bars.
    double logChooseRep(double n, double k) const {
        return lbinom(n + k - 1.0, k);
    }

    double exactEntropy() const {
        double S = 0.0;
        for (int r = 0; r < B; ++r) {
            if (nr[r] == 0) continue;
            S += (mode == Mode::DC) ? std::lgamma(er[r] + 1.0) : er[r] * safelog((double)nr[r]);
            double e_rr_half = ers[(size_t)r * B + r] / 2.0;
            S -= e_rr_half * LOG2 + std::lgamma(e_rr_half + 1.0);
            for (int s = r + 1; s < B; ++s) S -= std::lgamma(ers[(size_t)r * B + s] + 1.0);
        }
        return S;
    }
    double edgesDl() const {
        double NB = (double)Bne * (Bne + 1) / 2.0;
        return logChooseRep(NB, E);
    }
    double partitionDl() const {
        double S = std::lgamma((double)N + 1.0);
        for (int r = 0; r < B; ++r) if (nr[r] > 0) S -= std::lgamma((double)nr[r] + 1.0);
        S += lbinom((double)N - 1.0, (double)Bne - 1.0) + std::log((double)N);
        return S;
    }
    double degreeDlUniform() const {
        double S = 0.0;
        for (int r = 0; r < B; ++r) {
            if (nr[r] == 0) continue;
            S += logChooseRep((double)nr[r], std::round(er[r]));
        }
        return S;
    }
    double ppLikelihood() const {
        double Ein2 = 0.0, Min = 0.0;
        int nTot = 0;
        for (int r = 0; r < B; ++r) {
            if (nr[r] == 0) continue;
            Ein2 += ers[(size_t)r * B + r];
            Min += (double)nr[r] * (nr[r] - 1) / 2.0;
            nTot += nr[r];
        }
        double Ein = Ein2 / 2.0;
        double Mtot = (double)nTot * (nTot - 1) / 2.0;
        double Mout = Mtot - Min;
        double Eout = E - Ein;
        double S = 0.0;
        if (Min > 0.0 && Ein > 0.0 && Ein <= Min) S += lbinom(Min, Ein);
        if (Mout > 0.0 && Eout > 0.0 && Eout <= Mout) S += lbinom(Mout, Eout);
        return S;
    }
    double ppEdgesDl() const {
        double Min = 0.0;
        for (int r = 0; r < B; ++r) if (nr[r] > 0) Min += (double)nr[r] * (nr[r] - 1) / 2.0;
        return std::log(std::min(E, Min) + 1.0);
    }

    double entropy() const {
        double S;
        if (mode == Mode::PP) {
            S = ppLikelihood() + ppEdgesDl();
        } else {
            S = exactEntropy();
            if (useEdgesDl) S += edgesDl();
            if (mode == Mode::DC) S -= dcDegreeConst;
        }
        if (usePartitionDl) S += partitionDl();
        if (useDegreeDl)    S += degreeDlUniform();
        return S;
    }

    // subset entropy: sum of vterm/eterm entries that involve r or s.
    double subsetEntropy(int r, int s) const {
        double S = 0.0;
        if (nr[r] > 0) S += (mode == Mode::DC) ? std::lgamma(er[r] + 1.0) : er[r] * safelog((double)nr[r]);
        if (nr[s] > 0) S += (mode == Mode::DC) ? std::lgamma(er[s] + 1.0) : er[s] * safelog((double)nr[s]);
        if (nr[r] > 0) {
            double e_rr_half = ers[(size_t)r * B + r] / 2.0;
            S -= e_rr_half * LOG2 + std::lgamma(e_rr_half + 1.0);
        }
        if (nr[s] > 0 && s != r) {
            double e_ss_half = ers[(size_t)s * B + s] / 2.0;
            S -= e_ss_half * LOG2 + std::lgamma(e_ss_half + 1.0);
        }
        if (r != s) S -= std::lgamma(ers[(size_t)r * B + s] + 1.0);
        for (int t = 0; t < B; ++t) {
            if (t == r || t == s) continue;
            if (nr[t] == 0) continue;
            S -= std::lgamma(ers[(size_t)r * B + t] + 1.0);
            S -= std::lgamma(ers[(size_t)s * B + t] + 1.0);
        }
        return S;
    }
    double partitionDlSubset(int rA, int sA) const {
        double S = lbinom((double)N - 1.0, (double)Bne - 1.0);
        if (nr[rA] > 0) S -= std::lgamma((double)nr[rA] + 1.0);
        if (nr[sA] > 0 && sA != rA) S -= std::lgamma((double)nr[sA] + 1.0);
        return S;
    }
    double edgesDlSubset() const {
        double NB = (double)Bne * (Bne + 1) / 2.0;
        return logChooseRep(NB, E);
    }
    double degreeDlSubset(int rA, int sA) const {
        double S = 0.0;
        if (nr[rA] > 0) S += logChooseRep((double)nr[rA], std::round(er[rA]));
        if (nr[sA] > 0 && sA != rA) S += logChooseRep((double)nr[sA], std::round(er[sA]));
        return S;
    }
    double ppSubset() const {
        double S = ppLikelihood() + ppEdgesDl();
        if (usePartitionDl) S += partitionDl();
        return S;
    }
    double subsetSE(int rA, int sA) const {
        if (mode == Mode::PP) return ppSubset();
        double S = subsetEntropy(rA, sA);
        if (useEdgesDl) S += edgesDlSubset();
        if (usePartitionDl) S += partitionDlSubset(rA, sA);
        if (useDegreeDl)    S += degreeDlSubset(rA, sA);
        return S;
    }

    double virtualMove(int v, int s) {
        if (s == b[v]) return 0.0;
        int fromR = b[v];
        double before = subsetSE(fromR, s);
        moveVertex(v, s);
        double after = subsetSE(fromR, s);
        moveVertex(v, fromR);
        return after - before;
    }
};

// ── candidatePool ───────────────────────────────────────────────────
void candidatePool(const BlockState& st, int v, std::vector<int>& cands) {
    cands.clear();
    cands.insert(cands.end(), st.neList.begin(), st.neList.begin() + st.Bne);
    if (st.blockSize(st.blockOf(v)) > 1) {
        int maxId = -1;
        for (int i = 0; i < st.Bne; ++i) if (st.neList[i] > maxId) maxId = st.neList[i];
        cands.push_back(maxId + 1);
    }
}

// ── arg parsing ─────────────────────────────────────────────────────
struct Args {
    std::string mode;
    std::string edge;
    std::string init;
    int seed = 1;
    int sweeps = 1;
    double beta = 1.0;
};

Args parseArgs(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        auto starts = [&](const std::string& p) { return s.rfind(p, 0) == 0; };
        if      (starts("--mode="))   a.mode   = s.substr(7);
        else if (starts("--edge="))   a.edge   = s.substr(7);
        else if (starts("--init="))   a.init   = s.substr(7);
        else if (starts("--seed="))   a.seed   = std::atoi(s.c_str() + 7);
        else if (starts("--sweeps=")) a.sweeps = std::atoi(s.c_str() + 9);
        else if (starts("--beta="))   a.beta   = std::atof(s.c_str() + 7);
        else throw std::runtime_error("unknown arg: " + s);
    }
    if (a.mode.empty() || a.edge.empty() || a.init.empty())
        throw std::runtime_error("--mode, --edge, --init required");
    return a;
}

Mode parseMode(const std::string& s) {
    if (s == "dc") return Mode::DC;
    if (s == "ndc") return Mode::NDC;
    if (s == "pp") return Mode::PP;
    throw std::runtime_error("unknown mode: " + s);
}

// ── JSON emit ───────────────────────────────────────────────────────
struct JEmit {
    std::ostringstream os;
    JEmit() { os.setf(std::ios::fmtflags(0), std::ios::floatfield); }
    void writeDouble(double x) {
        // 17 sig digits is round-trip exact for IEEE-754 double.
        os << std::setprecision(17) << x;
    }
};

}  // namespace

int main(int argc, char** argv) try {
    Args args = parseArgs(argc, argv);
    Mode mode = parseMode(args.mode);

    Graph g = loadEdges(args.edge);
    std::vector<int> init = loadInitMembership(args.init, g.idToOrig);

    BlockState st(g, mode, init);
    JSRng rng((uint32_t)args.seed);

    JEmit em;
    auto& o = em.os;
    o << "{\"mode\":\"" << args.mode
      << "\",\"seed\":" << args.seed
      << ",\"sweeps_planned\":" << args.sweeps
      << ",\"beta\":";
    em.writeDouble(args.beta);
    o << ",\"n\":" << st.N << ",\"E\":";
    em.writeDouble(g.E);
    o << ",\"renum_to_orig\":[";
    for (int i = 0; i < st.N; ++i) {
        if (i) o << ",";
        o << "\"" << g.idToOrig[i] << "\"";
    }
    o << "],\"init_membership\":[";
    for (int i = 0; i < st.N; ++i) { if (i) o << ","; o << st.b[i]; }
    o << "],\"S_init\":";
    em.writeDouble(st.entropy());
    o << ",\"Bne_init\":" << st.Bne;
    o << ",\"sweeps\":[";

    std::vector<int> order(st.N);
    std::vector<int> cands; cands.reserve(st.N + 1);
    int totalAccepted = 0;
    for (int sw = 0; sw < args.sweeps; ++sw) {
        for (int i = 0; i < st.N; ++i) order[i] = i;
        jsShuffle(order, rng);
        if (sw) o << ",";
        o << "{\"sweep\":" << sw << ",\"visit_order\":[";
        for (int i = 0; i < st.N; ++i) { if (i) o << ","; o << order[i]; }
        o << "],\"S_pre\":";
        em.writeDouble(st.entropy());
        o << ",\"visits\":[";
        int sweepAccepted = 0;
        for (int i = 0; i < (int)order.size(); ++i) {
            int v = order[i];
            int fromR = st.blockOf(v);
            candidatePool(st, v, cands);
            int pickIdx = rng.intRange(0, (int)cands.size() - 1);
            int toS = cands[pickIdx];
            double dS = (toS == fromR) ? 0.0 : st.virtualMove(v, toS);
            double acceptU = rng.acceptUniform();
            bool accept = (dS <= 0.0) || (acceptU < std::exp(-args.beta * dS));
            bool committed = accept && (toS != fromR);
            if (committed) { st.moveVertex(v, toS); ++sweepAccepted; }
            if (i) o << ",";
            o << "{\"v\":" << v
              << ",\"fromR\":" << fromR
              << ",\"toS\":" << toS
              << ",\"pickIdx\":" << pickIdx
              << ",\"cands\":[";
            for (size_t j = 0; j < cands.size(); ++j) { if (j) o << ","; o << cands[j]; }
            o << "],\"dS\":";
            em.writeDouble(dS);
            o << ",\"acceptU\":";
            em.writeDouble(acceptU);
            o << ",\"accept\":" << (accept ? "true" : "false")
              << ",\"moved\":" << (committed ? "true" : "false")
              << "}";
        }
        totalAccepted += sweepAccepted;
        o << "],\"S_post\":";
        em.writeDouble(st.entropy());
        o << ",\"accepted\":" << sweepAccepted
          << ",\"Bne_post\":" << st.Bne
          << "}";
    }
    o << "],\"S_final\":";
    em.writeDouble(st.entropy());
    o << ",\"Bne_final\":" << st.Bne
      << ",\"accepted_total\":" << totalAccepted
      << ",\"final_membership\":[";
    for (int i = 0; i < st.N; ++i) { if (i) o << ","; o << st.b[i]; }
    o << "]}\n";

    std::cout << o.str();
    return 0;
} catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 2;
}
