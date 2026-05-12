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
#include <limits>
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
    // [TRACE-SBM-DRAWS] gap-audit item 4 (row B). Per-call raw()
    // draw counter. intRange consumes 1 + rejection-loop draws;
    // acceptUniform consumes 1; the explicit count is the L1 ladder
    // instrument that lets the harness localize "at visit k cpp drew
    // N raws, JS drew M" before the trajectory diverges. Subsumed by
    // L4 transitively (count drift surfaces as toS/pickIdx mismatch a
    // few visits downstream), but now directly emitted per visit.
    uint64_t drawCount = 0;
    explicit JSRng(uint32_t s) : mt(s) {}
    uint32_t raw() { ++drawCount; return mt(); }
    int32_t intRange(int32_t lo, int32_t hi) {
        int64_t range = (int64_t)hi - (int64_t)lo + 1;
        if (range <= 0) return lo;
        uint64_t limit = (0x100000000ULL / (uint64_t)range) * (uint64_t)range;
        uint32_t r;
        do { ++drawCount; r = mt(); } while ((uint64_t)r >= limit);
        return lo + (int32_t)(r % (uint64_t)range);
    }
    double acceptUniform() { ++drawCount; return (double)mt() / 4294967296.0; }
};

// JS Fisher-Yates: idx = n-1 .. 1, j = int(0, idx), swap(arr[idx], arr[j]).
template <class T>
void jsShuffle(std::vector<T>& a, JSRng& rng) {
    for (int32_t idx = (int32_t)a.size() - 1; idx >= 1; --idx) {
        int32_t j = rng.intRange(0, idx);
        std::swap(a[idx], a[j]);
    }
}

// Port of fdlibm's __ieee754_log (Sun Microsystems 1993, public domain
// in netlib's fdlibm release). V8's Math.log derives from the same
// source, so jsLog(x) is bit-IDENTICAL to JS Math.log(x); glibc's
// std::log differs at ~1 ulp on many inputs (e.g. log(46.5)). Routing
// every entropy/DL log through jsLog (and every lgamma through
// jsLgamma below) closes the V8 / glibc cross-impl gap, so cpp + JS
// produce bit-equal dS values under the proposalOracle replay.
//
// Verified bit-identical vs Math.log over ~94k sampled inputs across
// [0.5, 5000) step 0.0625 plus a geometric sweep on [1e-6, 1).
//
// Source: https://www.netlib.org/fdlibm/e_log.c
[[maybe_unused]] double jsLog(double x) {
    static constexpr double ln2_hi = 6.93147180369123816490e-01;
    static constexpr double ln2_lo = 1.90821492927058770002e-10;
    static constexpr double two54  = 1.80143985094819840000e+16;
    static constexpr double Lg1 = 6.666666666666735130e-01;
    static constexpr double Lg2 = 3.999999999940941908e-01;
    static constexpr double Lg3 = 2.857142874366239149e-01;
    static constexpr double Lg4 = 2.222219843214978396e-01;
    static constexpr double Lg5 = 1.818357216161805012e-01;
    static constexpr double Lg6 = 1.531383769920937332e-01;
    static constexpr double Lg7 = 1.479819860511658591e-01;
    static constexpr double zero = 0.0;
    auto hi_word = [](double v) {
        uint64_t b; std::memcpy(&b, &v, 8); return (uint32_t)(b >> 32);
    };
    auto lo_word = [](double v) {
        uint64_t b; std::memcpy(&b, &v, 8); return (uint32_t)b;
    };
    auto set_hi = [](double& v, uint32_t hi) {
        uint64_t b; std::memcpy(&b, &v, 8);
        b = (b & 0xFFFFFFFFULL) | ((uint64_t)hi << 32);
        std::memcpy(&v, &b, 8);
    };
    int32_t hx = (int32_t)hi_word(x);
    uint32_t lx = lo_word(x);
    int32_t k = 0;
    if (hx < 0x00100000) {
        if (((hx & 0x7fffffff) | (int32_t)lx) == 0) return -two54 / zero;
        if (hx < 0) return (x - x) / zero;
        k -= 54; x *= two54;
        hx = (int32_t)hi_word(x);
    }
    if (hx >= 0x7ff00000) return x + x;
    k += (hx >> 20) - 1023;
    hx &= 0x000fffff;
    int32_t i = (hx + 0x95f64) & 0x100000;
    set_hi(x, (uint32_t)(hx | (i ^ 0x3ff00000)));
    k += (i >> 20);
    double f = x - 1.0;
    double dk, hfsq, s, z, R, w, t1, t2;
    if ((0x000fffff & (2 + hx)) < 3) {
        if (f == zero) {
            if (k == 0) return zero;
            dk = (double)k; return dk * ln2_hi + dk * ln2_lo;
        }
        R = f * f * (0.5 - 0.33333333333333333 * f);
        if (k == 0) return f - R;
        dk = (double)k; return dk * ln2_hi - ((R - dk * ln2_lo) - f);
    }
    s = f / (2.0 + f);
    dk = (double)k;
    z = s * s;
    i = hx - 0x6147a;
    w = z * z;
    int32_t j = 0x6b851 - hx;
    t1 = w * (Lg2 + w * (Lg4 + w * Lg6));
    t2 = z * (Lg1 + w * (Lg3 + w * (Lg5 + w * Lg7)));
    i |= j;
    R = t2 + t1;
    if (i > 0) {
        hfsq = 0.5 * f * f;
        if (k == 0) return f - (hfsq - s * (hfsq + R));
        return dk * ln2_hi - ((hfsq - (s * (hfsq + R) + dk * ln2_lo)) - f);
    } else {
        if (k == 0) return f - s * (f - R);
        return dk * ln2_hi - ((s * (f - R) - dk * ln2_lo) - f);
    }
}

// Port of fdlibm's __ieee754_exp (Sun 1993, public domain via netlib).
// Verified bit-equal to V8 Math.exp on 200,000 sampled inputs across
// [-100, 100] step 0.001 except for the single special value exp(1):
// fdlibm's reduction loses 1 ulp there but V8 returns the correctly-
// rounded e. Special-cased below. Used by the MH accept comparison
// `acceptU < jsExp(-beta * dS)` where the argument is always negative
// (dS > 0 short-circuits otherwise), so the exp(1) special case is
// defensive only - keeps the function correct across the full domain
// in case a future caller passes positive args.
//
// Source: https://www.netlib.org/fdlibm/e_exp.c
[[maybe_unused]] double jsExp(double x) {
    static constexpr double one         = 1.0;
    static constexpr double halF[2]     = {0.5, -0.5};
    static constexpr double huge_       = 1.0e+300;
    static constexpr double twom1000    = 9.33263618503218878990e-302;
    static constexpr double o_threshold =  7.09782712893383973096e+02;
    static constexpr double u_threshold = -7.45133219101941108420e+02;
    static constexpr double ln2HI[2]    = { 6.93147180369123816490e-01, -6.93147180369123816490e-01};
    static constexpr double ln2LO[2]    = { 1.90821492927058770002e-10, -1.90821492927058770002e-10};
    static constexpr double invln2      =  1.44269504088896338700e+00;
    static constexpr double P1          =  1.66666666666666019037e-01;
    static constexpr double P2          = -2.77777777770155933842e-03;
    static constexpr double P3          =  6.61375632143793436117e-05;
    static constexpr double P4          = -1.65339022054652515390e-06;
    static constexpr double P5          =  4.13813679705723846039e-08;
    auto hi_word = [](double v) { uint64_t b; std::memcpy(&b, &v, 8); return (uint32_t)(b >> 32); };
    auto lo_word = [](double v) { uint64_t b; std::memcpy(&b, &v, 8); return (uint32_t)b; };
    auto set_hi = [](double& v, uint32_t hi) {
        uint64_t b; std::memcpy(&b, &v, 8);
        b = (b & 0xFFFFFFFFULL) | ((uint64_t)hi << 32);
        std::memcpy(&v, &b, 8);
    };
    // Special case: x == 1.0 returns the correctly-rounded e to match V8.
    if (x == 1.0) {
        double e; uint64_t b = 0x4005bf0a8b145769ULL;
        std::memcpy(&e, &b, 8);
        return e;
    }
    double y, hi = 0, lo = 0, c, t;
    int32_t k = 0, xsb;
    uint32_t hx;
    hx = hi_word(x);
    xsb = (hx >> 31) & 1;
    hx &= 0x7fffffff;
    if (hx >= 0x40862E42) {
        if (hx >= 0x7ff00000) {
            if (((hx & 0xfffff) | lo_word(x)) != 0) return x + x;
            return (xsb == 0) ? x : 0.0;
        }
        if (x > o_threshold) return huge_ * huge_;
        if (x < u_threshold) return twom1000 * twom1000;
    }
    if (hx > 0x3fd62e42) {
        if (hx < 0x3FF0A2B2) {
            hi = x - ln2HI[xsb]; lo = ln2LO[xsb]; k = 1 - xsb - xsb;
        } else {
            k  = (int32_t)(invln2 * x + halF[xsb]);
            t  = (double)k;
            hi = x - t * ln2HI[0];
            lo = t * ln2LO[0];
        }
        x = hi - lo;
    } else if (hx < 0x3e300000) {
        if (huge_ + x > one) return one + x;
    } else {
        k = 0;
    }
    t = x * x;
    c = x - t * (P1 + t * (P2 + t * (P3 + t * (P4 + t * P5))));
    if (k == 0) return one - ((x * c) / (c - 2.0) - x);
    y = one - ((lo - (x * c) / (2.0 - c)) - hi);
    if (k >= -1021) {
        uint32_t hy = hi_word(y);
        set_hi(y, hy + ((uint32_t)k << 20));
        return y;
    } else {
        uint32_t hy = hi_word(y);
        set_hi(y, hy + (((uint32_t)(k + 1000)) << 20));
        return y * twom1000;
    }
}

// Verbatim port of comdet/js/sbm/util.js lgamma. 7-coefficient Lanczos
// approximation; JS port disagrees with glibc's std::lgamma at ~1 ulp
// on most non-integer inputs. Routes through jsLog so cpp matches JS
// bit-for-bit. The reflection branch (x < 0.5) isn't hit by any
// entropy / DL call in this tracer (every arg comes from `+1` of a
// nonnegative integer-or-half-integer state value).
[[maybe_unused]] double jsLgamma(double x) {
    if (x < 0.5) return jsLog(M_PI / std::sin(M_PI * x)) - jsLgamma(1.0 - x);
    static const double c[9] = {
        0.99999999999980993,    676.5203681218851,  -1259.1392167224028,
        771.32342877765313,    -176.61502916214059,  12.507343278686905,
        -0.13857109526572012,   9.9843695780195716e-6, 1.5056327351493116e-7,
    };
    constexpr int g = 7;
    x -= 1.0;
    double a = c[0];
    const double t = x + (double)g + 0.5;
    for (int i = 1; i < g + 2; ++i) a += c[i] / (x + (double)i);
    return 0.5 * jsLog(2.0 * M_PI) + (x + 0.5) * jsLog(t) - t + jsLog(a);
}

// ── Compile-time tracer-mode toggle ──────────────────────────────────
// -DTRACER_MODE    : route entropy/DL math through V8-bit-equivalent
//                    jsLog / jsExp / jsLgamma ports. Bit-equal target vs
//                    JS production walker (comdet/js/sbm/).
// -DCANONICAL_MODE : route through libm std::log / std::exp / std::lgamma.
//                    Build-pair test reference; structural agreement only
//                    vs JS (libm differs from V8 by ~1 ulp on log/exp and
//                    by ~1 ulp on lgamma vs the JS Lanczos port).
// Per skill byte-equal-tracer / "Compile-time tracer-mode toggle":
// same source file produces two binaries via the macro selection.
#if !defined(CANONICAL_MODE) && !defined(TRACER_MODE)
#  error "Must define exactly one of -DCANONICAL_MODE or -DTRACER_MODE"
#endif
#if defined(CANONICAL_MODE) && defined(TRACER_MODE)
#  error "-DCANONICAL_MODE and -DTRACER_MODE are mutually exclusive"
#endif

#ifdef TRACER_MODE
  static inline double trace_log   (double x) { return jsLog(x); }
  static inline double trace_exp   (double x) { return jsExp(x); }
  static inline double trace_lgamma(double x) { return jsLgamma(x); }
  static constexpr const char* TRACE_MODE_NAME = "TRACER_MODE";
#else
  static inline double trace_log   (double x) { return std::log(x); }
  static inline double trace_exp   (double x) { return std::exp(x); }
  static inline double trace_lgamma(double x) { return std::lgamma(x); }
  static constexpr const char* TRACE_MODE_NAME = "CANONICAL_MODE";
#endif

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
// block count for a flat partition); ers stored row-major B*B with
// diagonal doubled to match graph-tool's e_rr "each internal edge
// contributes twice" convention.
//
// Latent limit: mcmcSweep can propose a fresh-block id `maxId+1` whose
// value drifts above N over many sweeps because empty slots aren't
// recycled (neList tracks live ids but does not renumber). When that
// happens reads/writes at id `s >= B` are out-of-bounds:
//   cpp: std::vector OOB = undefined behaviour (heap overflow / crash);
//   JS:  Int32Array OOB returns undefined → coerces to 0 silently +
//        OOB writes are dropped (still a bug in absolute Σ but does
//        not crash). Graph-tool (canonical) avoids the issue via
//        sparse maps. Surfaces as a `double free / heap overflow`
//        crash on `nested-ndc fixture32 sweeps>=12`. Documented in
//        community-detection/sbm/audit.md ("Known latent crash"); not
//        fixed here so both ports stay structurally faithful to each
//        other (same B=N design, both have the same root-cause issue).
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
        for (int v = 0; v < N; ++v) dcDegreeConst += trace_lgamma(g.strength[v] + 1.0);
        rebuildFromMembership();
    }

    // ── JS-typed-array OOB-tolerant accessors ─────────────────────────
    // Mirror comdet/js/sbm/block_state.js Int32Array / Float64Array
    // out-of-range semantics. The dense `B = N` capacity is shared by
    // cpp + JS, but candidatePool can propose a fresh-block id
    // maxId+1 == B when every slot is occupied (most aggressive in the
    // nested upper levels where N is small, e.g. N=2 at level 2 of a
    // 16-node fixture). JS Int32Array[s>=B] / Float64Array[s>=B] read
    // returns `undefined`, which coerces to 0 in `>` / `==` tests and to
    // NaN in arithmetic; OOB writes are silently dropped. cpp
    // std::vector OOB is undefined behaviour and surfaces as heap-
    // corruption on small T1 fixtures (n <= ~50 nested-dc/ndc). The
    // accessors below replicate JS semantics so cpp + JS stay bit-equal
    // and ASAN-clean. Pattern documented in audit row "Known latent
    // limit (B = N capacity)".
    inline int nr_at(int idx) const {
        return (idx >= 0 && idx < B) ? nr[idx] : 0;
    }
    inline double er_at(int idx) const {
        return (idx >= 0 && idx < B) ? er[idx] : 0.0;
    }
    // ers OOB read returns NaN to mirror JS `undefined + 1.0 = NaN`,
    // which then propagates through trace_lgamma/jsLog into a NaN dS.
    // Both legs short-circuit `dS <= 0` to false on NaN and consume one
    // acceptUniform draw, so RNG streams stay aligned.
    inline double ers_at(size_t idx) const {
        return (idx < ers.size()) ? ers[idx] : std::numeric_limits<double>::quiet_NaN();
    }
    // OOB writes are silently dropped (mirror JS typed-array semantics).
    inline void ers_set(size_t idx, double v) {
        if (idx < ers.size()) ers[idx] = v;
    }
    inline void ers_add(size_t idx, double v) {
        if (idx < ers.size()) ers[idx] += v;
    }
    inline void er_add(int idx, double v) {
        if (idx >= 0 && idx < B) er[idx] += v;
    }
    inline void nr_add(int idx, int v) {
        if (idx >= 0 && idx < B) nr[idx] += v;
    }

    inline int blockOf(int v) const { return b[v]; }
    inline int blockSize(int r) const { return nr_at(r); }
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
        // r is always in [0, B) since b[v] tracks live block ids; only
        // s (the move target) can be out of range when candidatePool
        // returns the fresh-block id maxId+1 == B. ers_add / er_add /
        // nr_add silently drop OOB writes to mirror JS Int32Array /
        // Float64Array semantics; the symmetric revert (moveVertex(v,
        // fromR)) also no-ops on those OOB slots so the BlockState
        // returns to its pre-virtualMove configuration.
        const auto& nb = g.nb[v];
        const auto& nbW = g.nbW[v];
        for (size_t i = 0; i < nb.size(); ++i) {
            int u = nb[i];
            if (u == v) continue;
            double w = nbW[i];
            int t = b[u];
            if (t == r) {
                ers_add((size_t)r * B + r, -2.0 * w);
                er_add(r, -2.0 * w);
            } else {
                ers_add((size_t)r * B + t, -w);
                ers_add((size_t)t * B + r, -w);
                er_add(r, -w);
                er_add(t, -w);
            }
            if (t == s) {
                ers_add((size_t)s * B + s, 2.0 * w);
                er_add(s, 2.0 * w);
            } else {
                ers_add((size_t)s * B + t, w);
                ers_add((size_t)t * B + s, w);
                er_add(s, w);
                er_add(t, w);
            }
        }
        double sw = g.selfW[v];
        if (sw > 0.0) {
            ers_add((size_t)r * B + r, -2.0 * sw);
            er_add(r, -2.0 * sw);
            ers_add((size_t)s * B + s, 2.0 * sw);
            er_add(s, 2.0 * sw);
        }
        bool wasROccupied = nr_at(r) > 0;
        bool wasSOccupied = nr_at(s) > 0;
        nr_add(r, -1);
        nr_add(s, 1);
        if (wasROccupied && nr_at(r) == 0) {
            int pos = neIdx[r];
            --Bne;
            int tail = neList[Bne];
            neList[pos] = tail;
            neIdx[tail] = pos;
            neIdx[r] = -1;
        }
        if (!wasSOccupied && nr_at(s) > 0) {
            // s in [0, B) here: nr_at returns nonzero only for valid
            // slots; OOB writes were dropped so nr_at(s>=B)==0 and
            // this branch is skipped (matches JS where the silently-
            // dropped Int32Array write leaves nr[s] as undefined and
            // `undefined > 0` is false).
            neIdx[s] = Bne;
            neList[Bne] = s;
            ++Bne;
        }
        b[v] = s;
    }

    inline double safelog(double x) const { return x > 0.0 ? trace_log(x) : 0.0; }

    // log binom(n, k) = lgamma(n+1) - lgamma(k+1) - lgamma(n-k+1).
    // Mirror comdet/js/sbm/util.js:lbinom early-return for k==0 || k==n;
    // the Lanczos approximation of lgamma(1) is not exactly 0, so
    // computing the formula on n=k yields -trace_lgamma(1) instead of 0,
    // breaking bit-equal vs JS at "open new block" virtualMove edges
    // where logChooseRep(1, 2) = lbinom(2, 2) sits in degreeDlSubset.
    double lbinom(double n, double k) const {
        if (n < 0.0 || k < 0.0 || k > n) return 0.0;
        if (k == 0.0 || k == n) return 0.0;
        return trace_lgamma(n + 1.0) - trace_lgamma(k + 1.0) - trace_lgamma(n - k + 1.0);
    }
    // logChooseRep(n,k) = log C(n+k-1, k), stars-and-bars.
    double logChooseRep(double n, double k) const {
        return lbinom(n + k - 1.0, k);
    }

    // Sorted neList prefix - every nonempty-block iteration below uses
    // this so the entropy/DL terms scan O(Bne) instead of O(B=N).
    std::vector<int> sortedNonEmpty() const {
        std::vector<int> a(neList.begin(), neList.begin() + Bne);
        std::sort(a.begin(), a.end());
        return a;
    }

    double exactEntropy() const {
        std::vector<int> ne = sortedNonEmpty();
        double S = 0.0;
        for (size_t i = 0; i < ne.size(); ++i) {
            int r = ne[i];
            S += (mode == Mode::DC) ? trace_lgamma(er[r] + 1.0) : er[r] * safelog((double)nr[r]);
            double e_rr_half = ers[(size_t)r * B + r] / 2.0;
            S -= e_rr_half * LOG2 + trace_lgamma(e_rr_half + 1.0);
            for (size_t j = i + 1; j < ne.size(); ++j) {
                int s = ne[j];
                S -= trace_lgamma(ers[(size_t)r * B + s] + 1.0);
            }
        }
        return S;
    }
    double edgesDl() const {
        double NB = (double)Bne * (Bne + 1) / 2.0;
        return logChooseRep(NB, E);
    }
    double partitionDl() const {
        std::vector<int> ne = sortedNonEmpty();
        double S = trace_lgamma((double)N + 1.0);
        for (int r : ne) S -= trace_lgamma((double)nr[r] + 1.0);
        S += lbinom((double)N - 1.0, (double)Bne - 1.0) + trace_log((double)N);
        return S;
    }
    double degreeDlUniform() const {
        std::vector<int> ne = sortedNonEmpty();
        double S = 0.0;
        for (int r : ne) S += logChooseRep((double)nr[r], std::round(er[r]));
        return S;
    }
    double ppLikelihood() const {
        std::vector<int> ne = sortedNonEmpty();
        double Ein2 = 0.0, Min = 0.0;
        int nTot = 0;
        for (int r : ne) {
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
        std::vector<int> ne = sortedNonEmpty();
        double Min = 0.0;
        for (int r : ne) Min += (double)nr[r] * (nr[r] - 1) / 2.0;
        return trace_log(std::min(E, Min) + 1.0);
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

    // Diagnostic: emit each entropy component for offline JS comparison.
    // Skill: byte-equal-tracer / per-step probe (audit row M).
    struct EntropyBreakdown {
        double exact, edges_dl, dc_deg_const, partition_dl, degree_dl, pp_lik, pp_edges, total;
    };
    EntropyBreakdown entropyBreakdown() const {
        EntropyBreakdown b{};
        if (mode == Mode::PP) {
            b.pp_lik = ppLikelihood();
            b.pp_edges = ppEdgesDl();
            b.partition_dl = usePartitionDl ? partitionDl() : 0.0;
            b.total = b.pp_lik + b.pp_edges + b.partition_dl;
        } else {
            b.exact = exactEntropy();
            b.edges_dl = useEdgesDl ? edgesDl() : 0.0;
            b.dc_deg_const = (mode == Mode::DC) ? dcDegreeConst : 0.0;
            b.partition_dl = usePartitionDl ? partitionDl() : 0.0;
            b.degree_dl = useDegreeDl ? degreeDlUniform() : 0.0;
            b.total = b.exact + b.edges_dl - b.dc_deg_const + b.partition_dl + b.degree_dl;
        }
        return b;
    }

    // Subset entropy: sum of vterm/eterm entries that involve r or s.
    // Iterates the sorted neList prefix for the cross-block eterms;
    // on dnc B=N=906 but Bne stays ~30, so virtualMove drops from
    // ~2B to ~2*Bne lgamma calls. Sorting (vs raw admin-order) keeps
    // the summation order id-ascending, which matches the pre-cache
    // version that walked t=0..B-1 - dS values stay byte-equal across
    // the refactor and the exploration sequence is preserved.
    double subsetEntropy(int r, int s) const {
        // r is always in [0, B) (current block of a vertex); s may equal
        // B when candidatePool returns the fresh-block id. Route every
        // s-indexed access through the OOB-tolerant accessors so cpp
        // matches JS Int32Array/Float64Array semantics: `nr_at(s>=B)==0`
        // skips the s-gated branches; `ers_at(s*B+t)` returns NaN which
        // propagates into a NaN dS (matches JS `undefined + 1.0`).
        double S = 0.0;
        if (nr_at(r) > 0) S += (mode == Mode::DC) ? trace_lgamma(er_at(r) + 1.0) : er_at(r) * safelog((double)nr_at(r));
        if (nr_at(s) > 0) S += (mode == Mode::DC) ? trace_lgamma(er_at(s) + 1.0) : er_at(s) * safelog((double)nr_at(s));
        if (nr_at(r) > 0) {
            double e_rr_half = ers_at((size_t)r * B + r) / 2.0;
            S -= e_rr_half * LOG2 + trace_lgamma(e_rr_half + 1.0);
        }
        if (nr_at(s) > 0 && s != r) {
            double e_ss_half = ers_at((size_t)s * B + s) / 2.0;
            S -= e_ss_half * LOG2 + trace_lgamma(e_ss_half + 1.0);
        }
        if (r != s) S -= trace_lgamma(ers_at((size_t)r * B + s) + 1.0);
        std::vector<int> ne = sortedNonEmpty();
        for (size_t i = 0; i < ne.size(); ++i) {
            int t = ne[i];
            if (t == r || t == s) continue;
            S -= trace_lgamma(ers_at((size_t)r * B + t) + 1.0);
            S -= trace_lgamma(ers_at((size_t)s * B + t) + 1.0);
        }
        return S;
    }
    double partitionDlSubset(int rA, int sA) const {
        // sA may equal B (fresh-block proposal); nr_at guards.
        double S = lbinom((double)N - 1.0, (double)Bne - 1.0);
        if (nr_at(rA) > 0) S -= trace_lgamma((double)nr_at(rA) + 1.0);
        if (nr_at(sA) > 0 && sA != rA) S -= trace_lgamma((double)nr_at(sA) + 1.0);
        return S;
    }
    double edgesDlSubset() const {
        double NB = (double)Bne * (Bne + 1) / 2.0;
        return logChooseRep(NB, E);
    }
    double degreeDlSubset(int rA, int sA) const {
        // sA may equal B (fresh-block proposal); nr_at + er_at guard.
        double S = 0.0;
        if (nr_at(rA) > 0) S += logChooseRep((double)nr_at(rA), std::round(er_at(rA)));
        if (nr_at(sA) > 0 && sA != rA) S += logChooseRep((double)nr_at(sA), std::round(er_at(sA)));
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

    // [TRACE-SBM-SUBSETSE] Probe-aware virtualMove for the byte-equal
    // tracer. Mirrors `virtualMove(v, s)` step-for-step but returns the
    // before/after subsetSE scalars in addition to dS. Sites: skill
    // byte-equal-tracer gap audit item 2 (rows D/E/M). When dS bits
    // diverge between cpp and JS, harness can bisect into `before`
    // vs `after` (and onwards into the four summands subsetEntropy +
    // edgesDlSubset + partitionDlSubset + degreeDlSubset/ppSubset).
    struct VirtualMoveTrace { double dS, before, after; };
    VirtualMoveTrace virtualMoveTraced(int v, int s) {
        if (s == b[v]) return {0.0, 0.0, 0.0};
        int fromR = b[v];
        double before = subsetSE(fromR, s);
        moveVertex(v, s);
        double after = subsetSE(fromR, s);
        moveVertex(v, fromR);
        return {after - before, before, after};
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

// ── Nested helpers ──────────────────────────────────────────────────
// Build a level-(l+1) (graph, init) pair from a level-l (graph, state).
// Mirrors comdet/js/sbm/nested_state.js:buildLevelGraph: edges carry
// the e_rs counts of the parent state (self-loops carry e_rr/2;
// off-diagonals carry e_rs as-is); vertices renumbered to 0..Bne_l-1
// via nonEmpty[i] -> i. The hierarchy entry b_{l+1} is keyed on
// parent block ids; relabel to align with the compacted ids.
struct LevelGraph {
    Graph g;
    std::vector<int> init;
    std::vector<int> nonEmpty;  // parent block id at position i
};

LevelGraph buildLevelGraph(const Graph& parentGraph, const BlockState& parent,
                           const std::vector<int>& b_next) {
    int N = parentGraph.N;
    std::map<long long, double> ers;
    auto key = [](int r, int s) -> long long {
        if (r > s) std::swap(r, s);
        return (long long)r * (1LL << 30) + (long long)s;
    };
    for (int v = 0; v < N; ++v) {
        int r = parent.b[v];
        const auto& nb = parentGraph.nb[v];
        const auto& nbW = parentGraph.nbW[v];
        for (size_t i = 0; i < nb.size(); ++i) {
            int u = nb[i];
            if (u <= v) continue;
            double w = nbW[i];
            int s = parent.b[u];
            ers[key(r, s)] += w;
        }
        if (parentGraph.selfW[v] > 0) ers[key(r, r)] += parentGraph.selfW[v];
    }
    std::vector<int> nonEmpty(parent.neList.begin(),
                              parent.neList.begin() + parent.Bne);
    std::vector<int> remap(parent.B + 1, -1);
    for (size_t i = 0; i < nonEmpty.size(); ++i) remap[nonEmpty[i]] = (int)i;
    LevelGraph out;
    int Bne = (int)nonEmpty.size();
    out.g.N = Bne;
    out.g.nb.assign(Bne, {});
    out.g.nbW.assign(Bne, {});
    out.g.selfW.assign(Bne, 0.0);
    out.g.strength.assign(Bne, 0.0);
    out.g.E = 0.0;
    out.g.idToOrig.resize(Bne);
    for (int i = 0; i < Bne; ++i) out.g.idToOrig[i] = std::to_string(nonEmpty[i]);
    for (auto& kv : ers) {
        long long k = kv.first;
        int r = (int)(k >> 30), s = (int)(k & ((1LL << 30) - 1));
        int rr = remap[r], ss = remap[s];
        if (rr < 0 || ss < 0) continue;
        double w = kv.second;
        if (rr == ss) {
            out.g.nb[rr].push_back(rr);
            out.g.nbW[rr].push_back(w);
            out.g.selfW[rr] += w;
            out.g.strength[rr] += 2.0 * w;
        } else {
            out.g.nb[rr].push_back(ss);
            out.g.nbW[rr].push_back(w);
            out.g.nb[ss].push_back(rr);
            out.g.nbW[ss].push_back(w);
            out.g.strength[rr] += w;
            out.g.strength[ss] += w;
        }
        out.g.E += w;
    }
    out.init.assign(Bne, 0);
    for (int i = 0; i < Bne; ++i) {
        // Hierarchy is sized to the parent block count at construction;
        // mid-mcmc "open new block" candidates fall outside. Default to
        // 0 to mirror the JS port's (b_next[idx] | 0) coercion in
        // nested_state.js:buildLevelGraph.
        int parentId = nonEmpty[i];
        out.init[i] = (parentId < (int)b_next.size()) ? b_next[parentId] : 0;
    }
    out.nonEmpty = std::move(nonEmpty);
    return out;
}

// Synthesize a deterministic hierarchy from a level-0 partition by
// halving the block count at each level until B==1. Matches the
// driver's auto-synthesis, used when --auto-levels is set.
std::vector<std::vector<int>> autoHierarchy(const std::vector<int>& b0, int maxLevels) {
    std::vector<std::vector<int>> hier{b0};
    int K = 0;
    for (int v : b0) K = std::max(K, v + 1);
    while ((int)hier.size() < maxLevels && K > 1) {
        const auto& prev = hier.back();
        std::vector<int> b(K);
        for (int r = 0; r < K; ++r) b[r] = r / 2;
        // The hierarchy entry must be sized like the level-(l-1) block
        // count; pad to prev's max block id.
        int prevMax = 0;
        for (int x : prev) prevMax = std::max(prevMax, x + 1);
        if ((int)b.size() < prevMax) b.resize(prevMax, 0);
        hier.push_back(b);
        // Update K to next level's block count.
        int nextK = 0;
        for (int x : b) nextK = std::max(nextK, x + 1);
        K = nextK;
    }
    return hier;
}

// ── arg parsing ─────────────────────────────────────────────────────
struct Args {
    std::string mode;
    std::string edge;
    std::string init;
    int seed = 1;
    int sweeps = 1;
    double beta = 1.0;
    bool nested = false;
    int autoLevels = 4;  // depth cap; nested terminates earlier when B==1
};

Args parseArgs(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        auto starts = [&](const std::string& p) { return s.rfind(p, 0) == 0; };
        if      (starts("--mode="))         a.mode   = s.substr(7);
        else if (starts("--edge="))         a.edge   = s.substr(7);
        else if (starts("--init="))         a.init   = s.substr(7);
        else if (starts("--seed="))         a.seed   = std::atoi(s.c_str() + 7);
        else if (starts("--sweeps="))       a.sweeps = std::atoi(s.c_str() + 9);
        else if (starts("--beta="))         a.beta   = std::atof(s.c_str() + 7);
        else if (s == "--nested")           a.nested = true;
        else if (starts("--auto-levels="))  a.autoLevels = std::atoi(s.c_str() + 14);
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
        // NaN / +/-Infinity have no native JSON token; emit them as the
        // string tokens "NaN" / "Infinity" / "-Infinity" so the JS-side
        // self-RNG harness can round-trip them back to the same IEEE
        // values via Number(). Default `operator<<` on NaN writes
        // literal `nan` which is NOT valid JSON. Surfaces in the nested
        // tracer when candidatePool picks a fresh-block target s == B
        // (capacity), routing the OOB-tolerant `ers_at` to NaN and
        // propagating through dS / subsetSE / expArg / expVal.
        if (std::isnan(x))      { os << "\"NaN\""; return; }
        if (std::isinf(x))      { os << (x < 0 ? "\"-Infinity\"" : "\"Infinity\""); return; }
        os << std::setprecision(17) << x;
    }
};

template <class T>
void emitIntArray(std::ostream& o, const T& v) {
    o << "[";
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) o << ",";
        o << v[i];
    }
    o << "]";
}

// Run one mcmc sweep on `st`, write the JSON object describing it
// to `o`, and return the sweep's accepted-move count. Shared between
// the flat top-level run and the per-level body of runNested so the
// per-visit emit stays in one place.
struct SweepHeader { const char* key = nullptr; int value = 0; };
int runSweep(BlockState& st, JSRng& rng, double beta,
             std::vector<int>& cands, std::vector<int>& order,
             JEmit& em, std::ostream& o,
             const SweepHeader& header = {}) {
    order.assign(st.N, 0);
    for (int i = 0; i < st.N; ++i) order[i] = i;
    jsShuffle(order, rng);
    o << "{";
    if (header.key) o << "\"" << header.key << "\":" << header.value << ",";
    o << "\"visit_order\":";
    emitIntArray(o, order);
    o << ",\"S_pre\":";
    em.writeDouble(st.entropy());
    o << ",\"visits\":[";
    int sweepAccepted = 0;
    for (int i = 0; i < (int)order.size(); ++i) {
        int v = order[i];
        int fromR = st.blockOf(v);
        candidatePool(st, v, cands);
        // [TRACE-SBM-DRAWS] gap-audit item 4 (row B). Snapshot draw
        // counter at visit entry so we can emit the per-visit raw()
        // delta. JS mirror compares its own per-visit delta on the
        // self-RNG leg.
        uint64_t drawsBefore = rng.drawCount;
        // [TRACE-SBM-PICKIDX] gap-audit item 5 (rows C, J). pickIdx is
        // the rng.intRange output that maps the uniform draw to a cand
        // slot; toS = cands[pickIdx] is then derived. Emitting pickIdx
        // separates "cands ordering shifted" (toS divergence with
        // matching pickIdx) from "RNG distribution mapping drifted"
        // (pickIdx divergence with matching cands).
        int pickIdx = rng.intRange(0, (int)cands.size() - 1);
        int toS = cands[pickIdx];
        // [TRACE-SBM-SUBSETSE] gap-audit item 2 (row M). Capture
        // subsetSE before/after via virtualMoveTraced so dS bisection
        // is possible without re-emitting the running BlockState.
        double dS = 0.0, subsetBefore = 0.0, subsetAfter = 0.0;
        if (toS != fromR) {
            auto vm = st.virtualMoveTraced(v, toS);
            dS = vm.dS; subsetBefore = vm.before; subsetAfter = vm.after;
        }
        // JS short-circuits accept when dS <= 0 and never draws the
        // uniform; cpp must mirror exactly so the MT19937 state stays
        // aligned for any downstream RNG consumer running standalone.
        // [TRACE-SBM-ACCEPT-FP] gap-audit item 3 (rows D, F). expArg
        // and expVal are the FP-primitive intermediates between dS and
        // accept; emitting them separates jsExp ulp drift from
        // acceptUniform draw drift when accept diverges on a dS>0 step.
        double acceptU = 0.0;
        double expArg = 0.0;
        double expVal = 1.0;
        bool accept;
        if (dS <= 0.0) {
            accept = true;
        } else {
            acceptU = rng.acceptUniform();
            expArg = -beta * dS;
            expVal = trace_exp(expArg);
            accept = (acceptU < expVal);
        }
        bool committed = accept && (toS != fromR);
        if (committed) { st.moveVertex(v, toS); ++sweepAccepted; }
        if (i) o << ",";
        o << "{\"v\":" << v
          << ",\"fromR\":" << fromR
          << ",\"toS\":" << toS
          << ",\"pickIdx\":" << pickIdx
          << ",\"cands\":";
        emitIntArray(o, cands);
        o << ",\"dS\":";
        em.writeDouble(dS);
        o << ",\"subsetSE_before\":";
        em.writeDouble(subsetBefore);
        o << ",\"subsetSE_after\":";
        em.writeDouble(subsetAfter);
        o << ",\"acceptU\":";
        em.writeDouble(acceptU);
        o << ",\"expArg\":";
        em.writeDouble(expArg);
        o << ",\"expVal\":";
        em.writeDouble(expVal);
        o << ",\"accept\":" << (accept ? "true" : "false")
          << ",\"moved\":" << (committed ? "true" : "false")
          << ",\"rng_draws\":" << (rng.drawCount - drawsBefore)
          << "}";
    }
    o << "],\"S_post\":";
    em.writeDouble(st.entropy());
    // [TRACE-SBM-BRK] gap-audit item 1 (rows D, E, M). After each
    // sweep emit the same entropyBreakdown that init dumps once. When
    // S_post bits diverge between cpp and JS, harness can bisect
    // straight into the divergent summand (exact / edges_dl / dc_deg_const
    // / partition_dl / degree_dl / pp_lik / pp_edges) without rerunning.
    {
        auto eb = st.entropyBreakdown();
        o << ",\"S_post_breakdown\":{\"exact\":";
        em.writeDouble(eb.exact);
        o << ",\"edges_dl\":";  em.writeDouble(eb.edges_dl);
        o << ",\"dc_deg_const\":"; em.writeDouble(eb.dc_deg_const);
        o << ",\"partition_dl\":"; em.writeDouble(eb.partition_dl);
        o << ",\"degree_dl\":";  em.writeDouble(eb.degree_dl);
        o << ",\"pp_lik\":";     em.writeDouble(eb.pp_lik);
        o << ",\"pp_edges\":";   em.writeDouble(eb.pp_edges);
        o << "}";
    }
    o << ",\"accepted\":" << sweepAccepted
      << ",\"Bne_post\":" << st.Bne
      << "}";
    return sweepAccepted;
}

}  // namespace

// ── Nested run ──────────────────────────────────────────────────────
// Round-robin multi-level mcmc with per-sweep propagation: after the
// level-l sweep ends, rebuild level-(l+1)..L graphs on the updated
// e_rs. Each level's BlockState owns its own membership + e_rs;
// inter-level coupling happens only at the rebuild boundary (matches
// the JS NestedBlockState's coarse-grained semantics).
int runNested(const Args& args, Mode mode, Graph& g0,
              const std::vector<int>& init0) {
    auto hierarchy = autoHierarchy(init0, args.autoLevels);
    std::vector<std::unique_ptr<Graph>> graphs;
    std::vector<std::unique_ptr<BlockState>> states;
    graphs.emplace_back(std::make_unique<Graph>(std::move(g0)));
    states.emplace_back(std::make_unique<BlockState>(*graphs[0], mode, hierarchy[0]));
    for (size_t l = 1; l < hierarchy.size(); ++l) {
        LevelGraph built = buildLevelGraph(*graphs[l - 1], *states[l - 1],
                                           hierarchy[l]);
        graphs.emplace_back(std::make_unique<Graph>(std::move(built.g)));
        states.emplace_back(std::make_unique<BlockState>(*graphs[l], Mode::NDC,
                                                        built.init));
    }
    int L = (int)states.size();

    auto totalEntropy = [&]() {
        double S = 0.0;
        for (int l = 0; l < L; ++l) S += states[l]->entropy();
        return S;
    };

    JSRng rng((uint32_t)args.seed);
    JEmit em;
    auto& o = em.os;
    o << "{\"trace_mode\":\"" << TRACE_MODE_NAME
      << "\",\"variant\":\"nested-" << args.mode
      << "\",\"seed\":" << args.seed
      << ",\"sweeps_planned\":" << args.sweeps
      << ",\"beta\":";
    em.writeDouble(args.beta);
    o << ",\"n\":" << graphs[0]->N
      << ",\"E\":";
    em.writeDouble(graphs[0]->E);
    o << ",\"renum_to_orig\":[";
    for (int i = 0; i < graphs[0]->N; ++i) {
        if (i) o << ",";
        o << "\"" << graphs[0]->idToOrig[i] << "\"";
    }
    o << "],\"hierarchy\":[";
    for (size_t l = 0; l < hierarchy.size(); ++l) {
        if (l) o << ",";
        o << "[";
        for (size_t i = 0; i < hierarchy[l].size(); ++i) {
            if (i) o << ",";
            o << hierarchy[l][i];
        }
        o << "]";
    }
    o << "],\"S_init\":";
    em.writeDouble(totalEntropy());
    o << ",\"level_S_init\":[";
    for (int l = 0; l < L; ++l) { if (l) o << ","; em.writeDouble(states[l]->entropy()); }
    o << "],\"level_N\":[";
    for (int l = 0; l < L; ++l) { if (l) o << ","; o << states[l]->N; }
    o << "],\"level_Bne_init\":[";
    for (int l = 0; l < L; ++l) { if (l) o << ","; o << states[l]->Bne; }
    o << "],\"sweeps\":[";

    int totalAccepted = 0;
    std::vector<int> cands;
    cands.reserve(states[0]->N + 1);
    std::vector<int> order;
    for (int sw = 0; sw < args.sweeps; ++sw) {
        if (sw) o << ",";
        o << "{\"sweep\":" << sw << ",\"levels\":[";
        for (int l = 0; l < L; ++l) {
            if (l) o << ",";
            // Wrap each level body in its own object so we can attach
            // the runSweep payload + a sibling [TRACE-SBM-REBUILD]
            // probe array describing every upper-level rebuilt graph.
            o << "{\"sweep_payload\":";
            totalAccepted += runSweep(*states[l], rng, args.beta, cands, order,
                                      em, o, {"level", l});
            // Propagate updated e_rs to upper levels. virtualMove's
            // apply-eval-revert can mutate neList ordering even when no
            // move is committed, which percolates into the cand pools
            // via candidatePool's nonEmptyBlocks slice; rebuild
            // unconditionally to keep cpp + JS replay byte-equal.
            o << ",\"rebuilds\":[";
            bool firstRebuild = true;
            for (int up = l + 1; up < L; ++up) {
                LevelGraph rebuilt = buildLevelGraph(*graphs[up - 1], *states[up - 1],
                                                    hierarchy[up]);
                // [TRACE-SBM-REBUILD] gap-audit items 6, 7, 10 (rows
                // H, I, K, L). Dump the rebuilt level graph + init
                // membership + the remap[] table before constructing
                // a fresh BlockState. nonEmpty[] = encounter-order of
                // parent's neList prefix (row L); remap[] = position
                // table indexed by parent block id (row L literal
                // site flat_traced.cpp:793-794); rebuilt N/E/edges
                // probe per-rebuild aggregation invariants.
                if (!firstRebuild) o << ",";
                firstRebuild = false;
                o << "{\"up\":" << up
                  << ",\"N\":" << rebuilt.g.N
                  << ",\"E\":"; em.writeDouble(rebuilt.g.E);
                o << ",\"nonEmpty\":";
                emitIntArray(o, rebuilt.nonEmpty);
                o << ",\"init\":";
                emitIntArray(o, rebuilt.init);
                // Strength array (per parent-block i, doubled-self-loop
                // strength). Bit-equal probe for row H (encounter-order)
                // + row I (self-loop convention) at every level.
                o << ",\"strength\":[";
                for (int i = 0; i < rebuilt.g.N; ++i) {
                    if (i) o << ",";
                    em.writeDouble(rebuilt.g.strength[i]);
                }
                o << "]";
                // Construct fresh BlockState; emit its post-rebuild
                // Bne + level entropy + breakdown so harness has the
                // bit-equal anchor *immediately* after the rebuild,
                // not just at end-of-sweep.
                graphs[up].reset(new Graph(std::move(rebuilt.g)));
                states[up].reset(new BlockState(*graphs[up], Mode::NDC, rebuilt.init));
                o << ",\"Bne_post_rebuild\":" << states[up]->Bne
                  << ",\"S_post_rebuild\":";
                em.writeDouble(states[up]->entropy());
                auto eb = states[up]->entropyBreakdown();
                o << ",\"S_post_rebuild_breakdown\":{\"exact\":";
                em.writeDouble(eb.exact);
                o << ",\"edges_dl\":";  em.writeDouble(eb.edges_dl);
                o << ",\"partition_dl\":"; em.writeDouble(eb.partition_dl);
                o << ",\"degree_dl\":";  em.writeDouble(eb.degree_dl);
                o << "}";
                o << "}";
            }
            o << "]}";
        }
        // [TRACE-SBM-LEVEL] gap-audit item 8 (rows K, M). Per-level
        // S_post snapshot at the close of each sweep. Lets harness
        // bisect "which level's entropy drifted" when total S_post
        // diverges, without rerunning per-level.
        o << "],\"level_S_post\":[";
        for (int l = 0; l < L; ++l) {
            if (l) o << ",";
            em.writeDouble(states[l]->entropy());
        }
        // [TRACE-SBM-LEVEL-MEMB] gap-audit item 9 (rows H, K, L). Per-
        // sweep snapshot of every level's membership vector (`b[]`).
        // Without this, divergence at sweep k>0 on level l>0 is only
        // visible via end-state `level_final_membership`. Emitting per
        // sweep lets the harness bit-compare the cumulative cluster
        // labels at any (sweep, level) tuple. Mirrors the per-sweep
        // S_post array above but at integer rather than double width.
        o << "],\"level_membership\":[";
        for (int l = 0; l < L; ++l) {
            if (l) o << ",";
            o << "[";
            for (int v = 0; v < states[l]->N; ++v) {
                if (v) o << ",";
                o << states[l]->b[v];
            }
            o << "]";
        }
        o << "],\"S_post\":";
        em.writeDouble(totalEntropy());
        o << "}";
    }
    o << "],\"S_final\":";
    em.writeDouble(totalEntropy());
    o << ",\"level_S_final\":[";
    for (int l = 0; l < L; ++l) { if (l) o << ","; em.writeDouble(states[l]->entropy()); }
    o << "],\"accepted_total\":" << totalAccepted
      << ",\"level_final_membership\":[";
    for (int l = 0; l < L; ++l) {
        if (l) o << ",";
        o << "[";
        for (int v = 0; v < states[l]->N; ++v) {
            if (v) o << ",";
            o << states[l]->b[v];
        }
        o << "]";
    }
    o << "]}\n";
    std::cout << o.str();
    return 0;
}

int main(int argc, char** argv) try {
    Args args = parseArgs(argc, argv);
    Mode mode = parseMode(args.mode);

    Graph g = loadEdges(args.edge);
    std::vector<int> init = loadInitMembership(args.init, g.idToOrig);

    if (args.nested) {
        return runNested(args, mode, g, init);
    }

    BlockState st(g, mode, init);
    JSRng rng((uint32_t)args.seed);

    JEmit em;
    auto& o = em.os;
    o << "{\"trace_mode\":\"" << TRACE_MODE_NAME
      << "\",\"mode\":\"" << args.mode
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
    {
        auto eb = st.entropyBreakdown();
        o << ",\"S_init_breakdown\":{\"exact\":";
        em.writeDouble(eb.exact);
        o << ",\"edges_dl\":";  em.writeDouble(eb.edges_dl);
        o << ",\"dc_deg_const\":"; em.writeDouble(eb.dc_deg_const);
        o << ",\"partition_dl\":"; em.writeDouble(eb.partition_dl);
        o << ",\"degree_dl\":";  em.writeDouble(eb.degree_dl);
        o << ",\"pp_lik\":";     em.writeDouble(eb.pp_lik);
        o << ",\"pp_edges\":";   em.writeDouble(eb.pp_edges);
        o << "}";
    }
    o << ",\"sweeps\":[";

    std::vector<int> order;
    std::vector<int> cands; cands.reserve(st.N + 1);
    int totalAccepted = 0;
    for (int sw = 0; sw < args.sweeps; ++sw) {
        if (sw) o << ",";
        totalAccepted += runSweep(st, rng, args.beta, cands, order,
                                  em, o, {"sweep", sw});
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
