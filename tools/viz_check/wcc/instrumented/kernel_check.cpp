// WCC kernel cross-check, instrumented C++ leg.
//
// Verbatim copy of the WCC pipeline from constrained-clustering:
//   src/mincut_only.cpp:4-103         (MincutOnly::main, Logarithmic branch)
//   includes/mincut_only.h:39-132     (MinCutWorker single-threaded path)
//   includes/mincut_only.h:13-37      (GetConnectedComponentsOnPartition) - shared
//   includes/constrained.h:425-471    (IsWellConnected)
//   includes/constrained.h:393-419    (GetConnectedComponents) - shared
//   includes/constrained.h:122-151    (RemoveInterClusterEdges) - shared
//   src/constrained.cpp:32-104        (Get/LoadEdges, GetOriginalToNewIdMap) - shared
//   src/constrained.cpp:135-152       (WriteClusterQueue<vector<int>>)
//   src/mincut_custom.cpp:3-107       (MinCutCustom::ComputeMinCut + In/Out partitions)
//
// Skill-conformant compile-time toggle (per byte-equal-tracer playbook §1):
//   -DCANONICAL_MODE -> /tmp/wcc_kernel_check_canonical
//                       std::log + canonical VieCut chain (std::unordered_*).
//                       For build-pair test (a) vs unmodified canonical_clustering.
//   -DTRACER_MODE    -> /tmp/wcc_kernel_check_swapped
//                       jsLog (V8-equivalent fdlibm port) + TRACER_MODE-swapped
//                       VieCut chain (std::set/map row-H closure). The cross-
//                       check artifact for L4 self-RNG vs JS production walker.
//
// The VieCut chain is picked up via the instrumented sibling include path
// at tools/viz_check/viecut/instrumented/include/, which precedes upstream
// VieCut/lib in the include search order. MinCutCustom is inlined here as
// `MinCutInline` so the toggle propagates into VieCut headers (rather than
// linking against precompiled libinternal_libs.a which can't be retrofitted).
//
// Build: ./build.sh -> /tmp/wcc_kernel_check_{canonical,swapped}
// Run:   /tmp/wcc_kernel_check_canonical <edge.csv> <com.csv> <out.csv> [criterion]
//        (criterion defaults to "1log_10(n)")
//
// stdout: JSON trace {pops:[{n, cut, in[], out[], wc, pushed}], survivors}.
// stderr: [TRACE-WCC ...] structured log.
#if !defined(CANONICAL_MODE) && !defined(TRACER_MODE)
#error "must define -DCANONICAL_MODE or -DTRACER_MODE"
#endif
#if defined(CANONICAL_MODE) && defined(TRACER_MODE)
#error "exactly one of CANONICAL_MODE / TRACER_MODE must be defined"
#endif

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "tracer_io.h"

#ifdef TRACER_MODE
  #include "js_math_port.h"
  inline double JSLOG(double x) { return viz_check::jsmath::jsLog(x); }
#else
  inline double JSLOG(double x) { return std::log(x); }
#endif

// VieCut chain — picked up via -I tools/viz_check/viecut/instrumented/include
// (instrumented headers take precedence; container_swap.h's TRACER_MODE
// vs CANONICAL_MODE alias decides row-H closure for the 8 unordered_* sites
// AND provides `k_build_mode` constexpr).
//
// Under CANONICAL_MODE: TracerSet=std::unordered_set; instrumented headers
// are wire-compatible with libinternal_libs.a's canonical MinCutCustom
// symbol (signatures align). We link against libinternal_libs.a so the
// random_functions::m_mt static lives in the same TU as the unmodified
// constrained_clustering binary's, preserving build-pair test (a) parity.
//
// Under TRACER_MODE: TracerSet=std::set; signatures DIFFER from canonical
// libinternal_libs.a. We inline MinCutInline locally + don't link
// libinternal_libs.a. random_functions::m_mt static lives in this TU.
#include "tools/container_swap.h"
#include "algorithms/global_mincut/cactus/cactus_mincut.h"
#include "algorithms/global_mincut/minimum_cut.h"
#include "algorithms/global_mincut/noi_minimum_cut.h"
#include "common/configuration.h"
#include "common/definitions.h"
#include "data_structure/mutable_graph.h"
#include "tools/random_functions.h"

#ifdef CANONICAL_MODE
  // Pulls in the canonical MinCutCustom symbol from libinternal_libs.a.
  // [UPSTREAM constrained-clustering/includes/mincut_custom.h]
  #include "mincut_custom.h"
  using MincutOp = MinCutCustom;
#endif

using namespace viz_check;

#ifdef TRACER_MODE
// [UPSTREAM mincut_custom.cpp:3-107] inlined; MinCutCustom -> MinCutInline.
// Only used under TRACER_MODE; under CANONICAL_MODE we link upstream's
// libinternal_libs.a MinCutCustom symbol so the random_functions::m_mt
// static lives in the same TU as the unmodified canonical pipeline.
class MinCutInline {
  public:
    MinCutInline(const igraph_t* g, const std::string& mt = "cactus")
      : graph(g), mincut_type(mt) {}

    int ComputeMinCut() {
        auto cfg = configuration::getConfig();
        cfg->find_most_balanced_cut = true;
        cfg->threads = 1;
        cfg->save_cut = true;
        cfg->set_node_in_cut = true;
        // mincut_custom.cpp:37 leaves random_functions::setSeed(0) commented out.
        // We mirror that — RNG state continues from previous pop's residual.

        std::shared_ptr<mutable_graph> G = std::make_shared<mutable_graph>();
        int num_nodes = igraph_vcount(this->graph);
        G->start_construction(num_nodes);
        for (int i = 0; i < num_nodes; i++) {
            NodeID cn = G->new_node();
            G->setPartitionIndex(cn, 0);
        }
        igraph_eit_t eit;
        igraph_eit_create(this->graph, igraph_ess_all(IGRAPH_EDGEORDER_ID), &eit);
        for (; !IGRAPH_EIT_END(eit); IGRAPH_EIT_NEXT(eit)) {
            igraph_integer_t e = IGRAPH_EIT_GET(eit);
            int from = IGRAPH_FROM(this->graph, e);
            int to   = IGRAPH_TO(this->graph, e);
            // [mincut_custom.cpp:57-65] the (from < target_node) check uses
            // target_node before assignment (== -1), so the else branch always
            // fires. We mirror the resulting net behavior: source = to,
            // target = from. Bug-for-bug.
            int source_node = to;
            int target_node = from;
            G->new_edge(source_node, target_node, 1);
        }
        igraph_eit_destroy(&eit);
        G->finish_construction();
        G->computeDegrees();

        std::unique_ptr<minimum_cut> mc;
        if (this->mincut_type == "cactus")
            mc = std::make_unique<cactus_mincut<std::shared_ptr<mutable_graph>>>();
        else if (this->mincut_type == "noi")
            mc = std::make_unique<noi_minimum_cut<std::shared_ptr<mutable_graph>>>();
        else
            throw std::runtime_error("Unknown mincut_type: " + this->mincut_type);

        int edge_cut_size = mc->perform_minimum_cut(G);
        for (int n = 0; n < num_nodes; n++) {
            if (G->getNodeInCut(n)) in_partition.push_back(n);
            else                    out_partition.push_back(n);
        }
        return edge_cut_size;
    }

    const std::vector<int>& GetInPartition()  const { return in_partition; }
    const std::vector<int>& GetOutPartition() const { return out_partition; }

  private:
    const igraph_t* graph;
    std::string mincut_type;
    std::vector<int> in_partition;
    std::vector<int> out_partition;
};
using MincutOp = MinCutInline;
#endif  // TRACER_MODE

// [UPSTREAM constrained.h:425-471] IsWellConnected (Logarithmic branch only).
//
// [TRACE-WCC-IWC] P0 closure (gaps 1+5+6+7): emit cluster_size source
// (in_size + out_size = canonical's expression at constrained.h:430), the
// is_close flag (gap 5), and the abs delta bits (gap 6). The cast-site
// (gap 7) is the implicit `int → double` conversion on `cut` inside the
// `thr - cut` subtraction — both operands are emitted as bits below.
static bool IsWellConnectedLog(double pre_computed_log,
                               int in_size, int out_size, int cut,
                               int pop_idx) {
    int n_iwc = in_size + out_size;
    double log_n_iwc = JSLOG((double)n_iwc);
    double thr = pre_computed_log * log_n_iwc;
    double diff = thr - (double)cut;
    double absdiff = std::abs(diff);
    bool is_close = absdiff <= 1e-9;
    bool wc = !is_close && thr < cut;
    uint64_t thr_b, cut_dbl_b, diff_b, abs_b, lognb;
    double cut_dbl = (double)cut;
    std::memcpy(&thr_b, &thr, 8);
    std::memcpy(&cut_dbl_b, &cut_dbl, 8);
    std::memcpy(&diff_b, &diff, 8);
    std::memcpy(&abs_b, &absdiff, 8);
    std::memcpy(&lognb, &log_n_iwc, 8);
    fprintf(stderr,
            "[TRACE-WCC-IWC] pop=%d in_size=%d out_size=%d n_iwc=%d "
            "log_n_iwc_bits=0x%016llx "
            "cut=%d cut_dbl_bits=0x%016llx thr_bits=0x%016llx "
            "diff_bits=0x%016llx abs_bits=0x%016llx is_close=%d wc=%d\n",
            pop_idx, in_size, out_size, n_iwc,
            (unsigned long long)lognb,
            cut, (unsigned long long)cut_dbl_b,
            (unsigned long long)thr_b,
            (unsigned long long)diff_b,
            (unsigned long long)abs_b,
            is_close ? 1 : 0, wc ? 1 : 0);
    return wc;
}

// [TRACE-WCC-HASH] Order-sensitive hash for integer vectors (FNV-1a-ish).
// Used to fingerprint idmap, in_local, out_local, induced-edge-list,
// mutable_graph adj, etc., without dumping the full vector at every pop.
static uint64_t hash_int_vec(const std::vector<int>& v) {
    uint64_t h = 1469598103934665603ULL;  // FNV offset
    for (int x : v) {
        uint64_t u = (uint64_t)(uint32_t)x;
        h ^= u;
        h *= 1099511628211ULL;
    }
    h ^= (uint64_t)v.size();
    h *= 1099511628211ULL;
    return h;
}
static uint64_t hash_edge_list(const std::vector<std::pair<int,int>>& es) {
    uint64_t h = 1469598103934665603ULL;
    for (auto& p : es) {
        h ^= (uint64_t)(uint32_t)p.first;
        h *= 1099511628211ULL;
        h ^= (uint64_t)(uint32_t)p.second;
        h *= 1099511628211ULL;
    }
    h ^= (uint64_t)es.size() * 17;
    h *= 1099511628211ULL;
    return h;
}

// Snapshot the std::mt19937 state for fingerprinting (gap 8 — RNG state
// at ComputeMinCut entry/exit). MT19937 internal state is 624 uint32 +
// position; we collapse it to a hash + the position counter (m_mt's
// internal index isn't directly exposed but we can call getRand() which
// returns the mt19937 by value and use operator<< to serialize).
static uint64_t mt_state_hash() {
    // Serialize via operator<< into a stringstream, then FNV-hash it.
    std::stringstream ss;
    auto rng = random_functions::getRand();
    ss << rng;
    std::string s = ss.str();
    uint64_t h = 1469598103934665603ULL;
    for (char c : s) {
        h ^= (uint64_t)(uint8_t)c;
        h *= 1099511628211ULL;
    }
    return h;
}

// [UPSTREAM mincut_only.h:39-132 + mincut_only.cpp:51-95]
// Single-threaded MinCutWorker (num_proc=1) inlined into the round loop.
struct PopRecord {
    int n;
    int cut;
    // Row E (FP composition order) sub-term probes. canonical evaluates
    // threshold = pre_log * log(n) in TWO operands (pre_log cached once at
    // startup as c / log(x); see constrained.cpp:235). The final product
    // alone bit-matches under matching pre_log + log(n), but only when the
    // intermediate sub-terms are themselves bit-equal — which row E is
    // about. We probe all three (pre_log, log_n, threshold final) to lock
    // operand-order in the diff harness; pre_log is constant across pops
    // so it's also surfaced once at top-level (pre_computed_log_bits).
    double pre_log;     // = pre_computed_log (constant across pops; per-pop probe)
    double log_n;       // = JSLOG((double)cur.size())
    double threshold;   // = pre_log * log_n
    bool wc;
    std::vector<int> cluster_nodes;
    std::vector<int> in_partition;   // global ids
    std::vector<int> out_partition;
    std::vector<std::vector<int>> pushed;
};

static void RunWCC(igraph_t* graph,
                   double pre_computed_log,
                   const std::string& mincut_type,
                   std::queue<std::vector<int>>& to_be_mincut,
                   std::queue<std::vector<int>>& done,
                   std::vector<PopRecord>& trace) {
    // [TRACE-WCC-ROUND] gap 19/20 — canonical structures the carve as outer
    // round loop (mincut_only.cpp:51-95) where each round drains a queue
    // snapshot. Single-thread inline mirror collapses to one while-loop, but
    // we emit a single ROUND boundary marker so the diff harness can detect
    // outer-loop iteration count divergence (gap 20). gap 19 (pre_log
    // constancy) is asserted per-pop below: the per-pop pre_log_bits equals
    // the pipeline-level pre_computed_log_bits by construction.
    // Single-thread invariant (gap 22): we drain inline; no thread spawn.
    fprintf(stderr, "[TRACE-WCC-ROUND] enter init_queue_size=%zu threads=1\n",
            to_be_mincut.size());
    int pop_idx = 0;
    uint64_t pre_log_const_bits;
    std::memcpy(&pre_log_const_bits, &pre_computed_log, 8);
    while (!to_be_mincut.empty()) {
        // [TRACE-WCC-POP-IN] gap 8 — per-pop ComputeMinCut entry RNG state
        // fingerprint + per-pop queue head/tail snapshot. m_mt persists
        // across pops since mincut_custom.cpp:37 leaves setSeed commented.
        uint64_t rng_pre = mt_state_hash();
        size_t qsz_pre = to_be_mincut.size();
        std::vector<int> cur = to_be_mincut.front();
        to_be_mincut.pop();
        // [TRACE-WCC-POP-CUR] gap 1 — cluster size source. cur.size() is the
        // queue-stored size (used for log_n in rec.log_n). IsWellConnected
        // uses in_size+out_size below. Both emitted; match on single-
        // connected induced subgraph.
        uint64_t cluster_hash = hash_int_vec(cur);
        fprintf(stderr,
                "[TRACE-WCC-POP-CUR] pop=%d rng_pre=0x%016llx queue_pre=%zu "
                "cur_size=%zu cluster_hash=0x%016llx first=%d last=%d\n",
                pop_idx, (unsigned long long)rng_pre, qsz_pre, cur.size(),
                (unsigned long long)cluster_hash,
                cur.empty() ? -1 : cur.front(),
                cur.empty() ? -1 : cur.back());

        igraph_vector_int_t keep, idmap;
        igraph_vector_int_init(&idmap, cur.size());
        igraph_vector_int_init(&keep, cur.size());
        for (size_t i = 0; i < cur.size(); i++) VECTOR(keep)[i] = cur[i];
        igraph_t sub;
        igraph_induced_subgraph_map(graph, &sub, igraph_vss_vector(&keep),
                                    IGRAPH_SUBGRAPH_CREATE_FROM_SCRATCH, NULL, &idmap);
        // [TRACE-WCC-POP-SUB] gap 2 — induced subgraph metadata. vcount,
        // ecount, edge-list hash (in igraph EID order = the order in which
        // igraph_induced_subgraph_map emitted new edges). idmap hash (gap 3)
        // emitted alongside; idmap is the OUTPUT of
        // igraph_induced_subgraph_map (cur[i] → new_id i; idmap[new_id] =
        // orig_id). gap 23 — self-loop/multi-edge assertion: count and emit.
        long long sub_n = (long long)igraph_vcount(&sub);
        long long sub_m = (long long)igraph_ecount(&sub);
        std::vector<std::pair<int,int>> sub_edges;
        igraph_eit_t seit;
        igraph_eit_create(&sub, igraph_ess_all(IGRAPH_EDGEORDER_ID), &seit);
        int n_selfloops = 0, n_multi = 0;
        std::map<std::pair<int,int>, int> edge_count;
        for (; !IGRAPH_EIT_END(seit); IGRAPH_EIT_NEXT(seit)) {
            igraph_integer_t e = IGRAPH_EIT_GET(seit);
            int f = IGRAPH_FROM(&sub, e), t = IGRAPH_TO(&sub, e);
            sub_edges.emplace_back(f, t);
            if (f == t) n_selfloops++;
            int lo = std::min(f, t), hi = std::max(f, t);
            int &c = edge_count[{lo, hi}];
            c++;
            if (c == 2) n_multi++;
        }
        igraph_eit_destroy(&seit);
        uint64_t sub_edge_hash = hash_edge_list(sub_edges);
        std::vector<int> idmap_vec(cur.size());
        for (size_t i = 0; i < cur.size(); i++) idmap_vec[i] = VECTOR(idmap)[i];
        uint64_t idmap_hash = hash_int_vec(idmap_vec);
        fprintf(stderr,
                "[TRACE-WCC-POP-SUB] pop=%d sub_n=%lld sub_m=%lld "
                "sub_edge_hash=0x%016llx idmap_hash=0x%016llx "
                "n_selfloops=%d n_multi=%d\n",
                pop_idx, sub_n, sub_m,
                (unsigned long long)sub_edge_hash,
                (unsigned long long)idmap_hash,
                n_selfloops, n_multi);

        MincutOp mcc(&sub, mincut_type);
        int cut = mcc.ComputeMinCut();
        std::vector<int> in_local = mcc.GetInPartition();
        std::vector<int> out_local = mcc.GetOutPartition();
        // [TRACE-WCC-POP-LOCAL] gap 4 + gap 12 — in_local / out_local BEFORE
        // idmap translation; per-node getNodeInCut output collapsed into
        // bipartition. We emit per-node cut flag implicitly via the
        // ordering: in_local[i] = local_id with cut=true (in-side); the
        // entries are appended in `for n = 0..num_nodes-1` (see MinCutInline
        // body above + mincut_custom.cpp:95-104), so the pair (in_local,
        // out_local) determines the per-node flag bit-equally.
        uint64_t in_local_hash = hash_int_vec(in_local);
        uint64_t out_local_hash = hash_int_vec(out_local);
        fprintf(stderr,
                "[TRACE-WCC-POP-LOCAL] pop=%d cut=%d in_local_size=%zu "
                "in_local_hash=0x%016llx out_local_size=%zu "
                "out_local_hash=0x%016llx\n",
                pop_idx, cut, in_local.size(),
                (unsigned long long)in_local_hash, out_local.size(),
                (unsigned long long)out_local_hash);

        // [TRACE-WCC-POP-OUT] gap 8 — RNG state fingerprint at exit.
        uint64_t rng_post = mt_state_hash();
        fprintf(stderr, "[TRACE-WCC-POP-OUT] pop=%d rng_post=0x%016llx "
                        "rng_delta=%s\n",
                pop_idx, (unsigned long long)rng_post,
                rng_post == rng_pre ? "0" : "nonzero");

        bool wc = IsWellConnectedLog(pre_computed_log,
                                     (int)in_local.size(),
                                     (int)out_local.size(), cut, pop_idx);

        PopRecord rec;
        rec.n = (int)cur.size();
        rec.cut = cut;
        // Row E (FP composition order) probe: split the product into its two
        // operands so the diff harness can localize sub-ulp drift to either
        // pre_log (constant; cached at startup) or log_n (per-pop), rather
        // than masking divergence under a final-product compare. Mirrors
        // canonical: constrained.h:430 does `pre_computed_log * std::log(n)`
        // as ONE mul of TWO operands, NOT `c * log(n) / log(x)` (3 ops).
        rec.pre_log = pre_computed_log;
        rec.log_n = JSLOG((double)cur.size());
        rec.threshold = rec.pre_log * rec.log_n;
        rec.wc = wc;
        rec.cluster_nodes = cur;
        for (int i : in_local)  rec.in_partition.push_back(VECTOR(idmap)[i]);
        for (int i : out_local) rec.out_partition.push_back(VECTOR(idmap)[i]);

        // Bit-reinterpret all three row-E sub-terms for the trace log so any
        // divergence (in pre_log, log_n, or their product) is visible at the
        // exact site it surfaces. Probes are append-only per playbook
        // discipline ("Tracer prints stay"); thr_bits stays as the legacy
        // probe, pre_log_bits + log_n_bits join it.
        uint64_t pre_log_bits, log_n_bits, thr_bits;
        std::memcpy(&pre_log_bits, &rec.pre_log, 8);
        std::memcpy(&log_n_bits,  &rec.log_n,  8);
        std::memcpy(&thr_bits,    &rec.threshold, 8);
        // gap 19 — assert pre_log constancy across pops.
        bool pre_log_const = (pre_log_bits == pre_log_const_bits);
        fprintf(stderr,
                "[TRACE-WCC] POP idx=%d n=%d cut=%d pre_log=%.17g pre_log_bits=0x%016llx "
                "log_n=%.17g log_n_bits=0x%016llx thr=%.17g thr_bits=0x%016llx wc=%s "
                "in=%zu out=%zu pre_log_const=%d\n",
                pop_idx, rec.n, rec.cut,
                rec.pre_log, (unsigned long long)pre_log_bits,
                rec.log_n,   (unsigned long long)log_n_bits,
                rec.threshold, (unsigned long long)thr_bits,
                wc ? "true" : "false",
                rec.in_partition.size(), rec.out_partition.size(),
                pre_log_const ? 1 : 0);

        if (wc) {
            done.push(cur);
        } else {
            // GetConnectedComponentsOnPartition for each side, push back.
            // Order: in-side then out-side (mirrors mincut_only.h:97-122).
            for (int side_idx = 0; side_idx < 2; side_idx++) {
                auto& side_local = side_idx == 0 ? in_local : out_local;
                const char* side_tag = side_idx == 0 ? "in" : "out";
                if (side_local.size() <= 1) continue;
                // [TRACE-WCC-SIDE-IN] gap 16 — per-side induced subgraph
                // hash + idmap. GetConnectedComponentsOnPartition builds an
                // induced subgraph over side_local (mincut_only.h:13-37);
                // we recompute it here for the probe (mirroring same call)
                // and emit metadata. The shared helper at tracer_io.h
                // also runs internally; we don't perturb its results.
                {
                    igraph_vector_int_t side_keep, side_idmap;
                    igraph_vector_int_init(&side_idmap, side_local.size());
                    igraph_vector_int_init(&side_keep, side_local.size());
                    for (size_t i = 0; i < side_local.size(); i++)
                        VECTOR(side_keep)[i] = side_local[i];
                    igraph_t side_sub;
                    igraph_induced_subgraph_map(&sub, &side_sub,
                        igraph_vss_vector(&side_keep),
                        IGRAPH_SUBGRAPH_CREATE_FROM_SCRATCH, NULL, &side_idmap);
                    long long side_n = (long long)igraph_vcount(&side_sub);
                    long long side_m = (long long)igraph_ecount(&side_sub);
                    std::vector<std::pair<int,int>> side_edges;
                    igraph_eit_t eit2;
                    igraph_eit_create(&side_sub, igraph_ess_all(IGRAPH_EDGEORDER_ID), &eit2);
                    for (; !IGRAPH_EIT_END(eit2); IGRAPH_EIT_NEXT(eit2)) {
                        igraph_integer_t e2 = IGRAPH_EIT_GET(eit2);
                        side_edges.emplace_back(IGRAPH_FROM(&side_sub, e2),
                                                IGRAPH_TO(&side_sub, e2));
                    }
                    igraph_eit_destroy(&eit2);
                    uint64_t side_edge_hash = hash_edge_list(side_edges);
                    std::vector<int> side_idmap_vec(side_local.size());
                    for (size_t i = 0; i < side_local.size(); i++)
                        side_idmap_vec[i] = VECTOR(side_idmap)[i];
                    uint64_t side_idmap_hash = hash_int_vec(side_idmap_vec);
                    fprintf(stderr,
                            "[TRACE-WCC-SIDE-IN] pop=%d side=%s side_local_size=%zu "
                            "side_n=%lld side_m=%lld side_edge_hash=0x%016llx "
                            "side_idmap_hash=0x%016llx\n",
                            pop_idx, side_tag, side_local.size(), side_n, side_m,
                            (unsigned long long)side_edge_hash,
                            (unsigned long long)side_idmap_hash);
                    igraph_vector_int_destroy(&side_keep);
                    igraph_vector_int_destroy(&side_idmap);
                    igraph_destroy(&side_sub);
                }
                auto comps = GetConnectedComponentsOnPartition(&sub, side_local);
                // [TRACE-WCC-SIDE-COMPS] gap 17 — full comps[] BEFORE the
                // size>1 filter. GetConnectedComponentsOnPartition already
                // drops singletons (via constrained.h:409), so the comps
                // list returned is already size>1. We emit raw count +
                // per-comp size to expose pre-filter shape for the harness.
                fprintf(stderr,
                        "[TRACE-WCC-SIDE-COMPS] pop=%d side=%s n_comps=%zu",
                        pop_idx, side_tag, comps.size());
                for (auto& comp : comps)
                    fprintf(stderr, " sz=%zu", comp.size());
                fprintf(stderr, "\n");
                for (auto& comp : comps) {
                    std::vector<int> tr;
                    for (int i : comp) tr.push_back(VECTOR(idmap)[i]);
                    if (tr.size() > 1) {
                        to_be_mincut.push(tr);
                        rec.pushed.push_back(tr);
                        uint64_t tr_hash = hash_int_vec(tr);
                        fprintf(stderr,
                                "[TRACE-WCC]   PUSH %s size=%zu hash=0x%016llx\n",
                                side_tag, tr.size(),
                                (unsigned long long)tr_hash);
                    } else {
                        fprintf(stderr, "[TRACE-WCC]   DROP %s size=%zu\n",
                                side_tag, tr.size());
                    }
                }
            }
            // [TRACE-WCC-QUEUE-TAIL] gap 18 — queue tail snapshot after this
            // pop's push-back. Single-shared-queue invariant means new
            // entries sit at the tail; emitting tail size + hash of last
            // entry localizes a divergent push-back order at the very next
            // pop, rather than one-but-the-next pop downstream.
            size_t qsz_post = to_be_mincut.size();
            uint64_t tail_hash = 0;
            int tail_first = -1, tail_last = -1;
            if (!to_be_mincut.empty()) {
                const auto& tail = to_be_mincut.back();
                tail_hash = hash_int_vec(tail);
                if (!tail.empty()) { tail_first = tail.front(); tail_last = tail.back(); }
            }
            fprintf(stderr,
                    "[TRACE-WCC-QUEUE-TAIL] pop=%d queue_post=%zu "
                    "tail_hash=0x%016llx tail_first=%d tail_last=%d\n",
                    pop_idx, qsz_post, (unsigned long long)tail_hash,
                    tail_first, tail_last);
        }
        trace.push_back(std::move(rec));
        igraph_vector_int_destroy(&keep);
        igraph_vector_int_destroy(&idmap);
        igraph_destroy(&sub);
        pop_idx++;
    }
    fprintf(stderr, "[TRACE-WCC-ROUND] exit total_pops=%d\n", pop_idx);
}

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr,
                     "usage: %s <edge.csv> <com.csv> <out.csv> [criterion] [mincut_type] [seed]\n",
                     argv[0]);
        return 2;
    }
    std::string edge_csv = argv[1];
    std::string com_csv  = argv[2];
    std::string out_csv  = argv[3];
    std::string criterion   = (argc >= 5) ? argv[4] : "1log_10(n)";
    std::string mincut_type = (argc >= 6) ? argv[5] : "cactus";
    size_t seed             = (argc >= 7) ? (size_t)std::atoi(argv[6]) : 0;

    fprintf(stderr,
            "[TRACE-WCC] PIPELINE_START edge=%s com=%s build=%s mincut=%s seed=%zu\n",
            edge_csv.c_str(), com_csv.c_str(), k_build_mode, mincut_type.c_str(), seed);

    // [TRACE-WCC-FP-PRIM] gap 24 — pipeline-start probe for the
    // Exponential branch's std::pow + Custom/piecewise branch's std::ceil
    // + std::sqrt. The tracer currently rejects non-Logarithmic criterion
    // strings (see argv parse below), but the JS port (wcc.js parseCriterion)
    // supports all four branches. We emit a fixed sweep of FP primitive
    // outputs over n ∈ {10, 50, 100, 250, 500, 1000, 5000, 10000, 50000} so
    // any cross-build drift in std::pow / std::ceil / std::sqrt vs Math.pow /
    // Math.ceil / Math.sqrt becomes visible BEFORE downstream coverage
    // expands to those branches. Closes the audit row D pending pow/ceil
    // verification + gap-24 (Exponential + piecewise primitives uncovered).
    {
        const int probe_ns[] = {10, 50, 100, 250, 500, 1000, 5000, 10000, 50000};
        for (int n : probe_ns) {
            double pow_05 = std::pow((double)n, 0.5);
            double pow_n2 = std::pow((double)n, 2.0);
            double sqrt_n = std::sqrt((double)n);
            double ceil_v = std::ceil(0.1 * pow_05);
            uint64_t b_pow05, b_pow2, b_sqrt, b_ceil;
            std::memcpy(&b_pow05, &pow_05, 8);
            std::memcpy(&b_pow2,  &pow_n2, 8);
            std::memcpy(&b_sqrt,  &sqrt_n, 8);
            std::memcpy(&b_ceil,  &ceil_v, 8);
            fprintf(stderr,
                    "[TRACE-WCC-FP-PRIM] n=%d pow_05_bits=0x%016llx "
                    "pow_2_bits=0x%016llx sqrt_bits=0x%016llx "
                    "ceil_01pow05_bits=0x%016llx\n",
                    n, (unsigned long long)b_pow05,
                    (unsigned long long)b_pow2,
                    (unsigned long long)b_sqrt,
                    (unsigned long long)b_ceil);
        }
    }

    // VieCut RNG seed.
    auto cfg = configuration::getConfig();
    cfg->seed = seed;
    random_functions::setSeed(seed);

    // [UPSTREAM constrained.cpp:201-249] Parse Clog_x(n).
    double C_param = 1.0, x_param = 10.0;
    {
        size_t p1 = criterion.find("log_");
        size_t p2 = criterion.find("(");
        if (p1 == std::string::npos || p2 == std::string::npos) {
            fprintf(stderr, "[TRACE-WCC] only Clog_x(n) supported here, got '%s'\n", criterion.c_str());
            return 2;
        }
        C_param = std::stod(criterion.substr(0, p1));
        x_param = std::stod(criterion.substr(p1 + 4, p2 - (p1 + 4)));
    }
    // [UPSTREAM constrained.cpp:235] pre_computed_log = c / log(x), once.
    double pre_computed_log = C_param / JSLOG(x_param);
    uint64_t pl_bits;
    std::memcpy(&pl_bits, &pre_computed_log, 8);
    fprintf(stderr,
            "[TRACE-WCC] criterion='%s' C=%.17g x=%.17g pre_log=%.17g pre_log_bits=0x%016llx\n",
            criterion.c_str(), C_param, x_param, pre_computed_log,
            (unsigned long long)pl_bits);

    auto orig_to_new = GetOriginalToNewIdMap(edge_csv);
    auto new_to_orig = InvertMap(orig_to_new);
    igraph_t graph;
    igraph_empty(&graph, 0, IGRAPH_UNDIRECTED);
    LoadEdgesFromFile(&graph, edge_csv, orig_to_new);
    fprintf(stderr, "[TRACE-WCC] loaded n=%lld m=%lld\n",
            (long long)igraph_vcount(&graph), (long long)igraph_ecount(&graph));

    // [TRACE-WCC-IDMAP] gap 15 — orig_to_new iteration order (std::map<
    // string,int> is string-ASC). JS uses insertion-order Map (= input CSV
    // order). Emit full mapping in canonical iteration order so harness can
    // check the JS port follows the same ASC order downstream.
    {
        fprintf(stderr, "[TRACE-WCC-IDMAP] size=%zu order=string_asc",
                orig_to_new.size());
        size_t cnt = 0;
        for (auto& [orig, nid] : orig_to_new) {
            fprintf(stderr, " '%s':%d", orig.c_str(), nid);
            if (++cnt >= 64) { fprintf(stderr, " ..."); break; }
        }
        fprintf(stderr, "\n");
    }

    auto partition = ReadCommunities(orig_to_new, com_csv);
    RemoveInterClusterEdges(&graph, partition);
    fprintf(stderr, "[TRACE-WCC] after_remove m=%lld\n",
            (long long)igraph_ecount(&graph));

    // [TRACE-WCC-CC-EDGES] gap 13 — post-RemoveInterClusterEdges adjacency
    // hash (in igraph EID order). Closes the silent STAGE_CC residual graph
    // probe: cpp vs JS edge filter divergence becomes visible HERE, before
    // GetConnectedComponents.
    {
        std::vector<std::pair<int,int>> resid_edges;
        igraph_eit_t eit;
        igraph_eit_create(&graph, igraph_ess_all(IGRAPH_EDGEORDER_ID), &eit);
        for (; !IGRAPH_EIT_END(eit); IGRAPH_EIT_NEXT(eit)) {
            igraph_integer_t e = IGRAPH_EIT_GET(eit);
            resid_edges.emplace_back(IGRAPH_FROM(&graph, e),
                                     IGRAPH_TO(&graph, e));
        }
        igraph_eit_destroy(&eit);
        uint64_t resid_hash = hash_edge_list(resid_edges);
        fprintf(stderr,
                "[TRACE-WCC-CC-EDGES] resid_m=%zu resid_hash=0x%016llx\n",
                resid_edges.size(), (unsigned long long)resid_hash);
    }

    // [TRACE-WCC-CC-MEMBER] gap 14 — full GetConnectedComponents membership
    // vector + per-component size BEFORE the csize>1 filter is applied at
    // constrained.h:409. We re-run igraph_connected_components inline here
    // to expose the raw membership (the shared helper already drops
    // singletons internally), so the harness can verify decomposition
    // parity even when one side picks up an extra isolated vertex.
    {
        igraph_vector_int_t cid, sz;
        igraph_vector_int_init(&cid, 0);
        igraph_vector_int_init(&sz, 0);
        igraph_integer_t nc;
        igraph_connected_components(&graph, &cid, &sz, &nc, IGRAPH_WEAK);
        std::vector<int> cid_vec(igraph_vcount(&graph));
        for (int n = 0; n < igraph_vcount(&graph); n++)
            cid_vec[n] = VECTOR(cid)[n];
        std::vector<int> sz_vec((size_t)nc);
        for (igraph_integer_t i = 0; i < nc; i++) sz_vec[i] = VECTOR(sz)[i];
        uint64_t cid_hash = hash_int_vec(cid_vec);
        uint64_t sz_hash = hash_int_vec(sz_vec);
        fprintf(stderr,
                "[TRACE-WCC-CC-MEMBER] n_components=%lld cid_hash=0x%016llx "
                "sz_hash=0x%016llx\n",
                (long long)nc, (unsigned long long)cid_hash,
                (unsigned long long)sz_hash);
        igraph_vector_int_destroy(&cid);
        igraph_vector_int_destroy(&sz);
    }

    auto components = GetConnectedComponents(&graph);
    fprintf(stderr, "[TRACE-WCC] initial_components=%zu\n", components.size());
    // [TRACE-WCC-CC-OUT] gap 14 (continued) — post-filter components: per
    // CC size + first-node so harness can lockstep-compare to JS's
    // allComps filter.
    for (size_t i = 0; i < components.size(); i++) {
        uint64_t ch = hash_int_vec(components[i]);
        fprintf(stderr,
                "[TRACE-WCC-CC-OUT] cc=%zu size=%zu first=%d hash=0x%016llx\n",
                i, components[i].size(),
                components[i].empty() ? -1 : components[i].front(),
                (unsigned long long)ch);
    }

    std::queue<std::vector<int>> to_be_mincut, done;
    for (auto& c : components) to_be_mincut.push(c);

    std::vector<PopRecord> trace;
    RunWCC(&graph, pre_computed_log, mincut_type, to_be_mincut, done, trace);

    fprintf(stderr, "[TRACE-WCC] survivors=%zu total_pops=%zu\n",
            done.size(), trace.size());

    // [UPSTREAM constrained.cpp:135-152] WriteClusterQueue body.
    {
        std::ofstream out(out_csv);
        out << "node_id,cluster_id\n";
        int cid = 0;
        std::queue<std::vector<int>> q = done;
        while (!q.empty()) {
            auto cur = q.front(); q.pop();
            for (int nid : cur) out << new_to_orig.at(nid) << "," << cid << "\n";
            cid++;
        }
    }

    // Emit JSON trace.
    std::cout << "{\n  \"build_mode\": \"" << k_build_mode << "\",\n";
    std::cout << "  \"seed\": " << seed << ",\n";
    std::cout << "  \"criterion\": \"" << criterion << "\",\n";
    std::cout << "  \"mincut_type\": \"" << mincut_type << "\",\n";
    std::cout << std::setprecision(17);
    std::cout << "  \"pre_computed_log\": " << pre_computed_log << ",\n";
    std::cout << "  \"pre_computed_log_bits\": \"0x"
              << std::hex << std::setw(16) << std::setfill('0') << pl_bits
              << std::dec << std::setfill(' ') << "\",\n";
    std::cout << "  \"node_map\": [";
    {
        bool first = true;
        for (auto& [nid, orig] : new_to_orig) {
            if (!first) std::cout << ",";
            first = false;
            std::cout << "{\"new\":" << nid << ",\"orig\":\"" << orig << "\"}";
        }
    }
    std::cout << "],\n  \"initial_components\": [";
    for (size_t i = 0; i < components.size(); i++) {
        if (i) std::cout << ",";
        emit_int_array(std::cout, components[i]);
    }
    std::cout << "],\n  \"pops\": [\n";
    for (size_t i = 0; i < trace.size(); i++) {
        if (i) std::cout << ",\n";
        const auto& r = trace[i];
        // Row E sub-term probes: pre_log, log_n, threshold (final) bit-
        // reinterpret. The harness compares all three lockstep so a sub-ulp
        // drift in either operand is caught BEFORE it can mask under final-
        // product compare. pre_log is constant across pops (= pre_computed_log
        // top-level field) but emitted per-pop too so the diff harness needs
        // no special-case for the constant.
        uint64_t pre_log_bits, log_n_bits, thr_bits;
        std::memcpy(&pre_log_bits, &r.pre_log,   8);
        std::memcpy(&log_n_bits,   &r.log_n,     8);
        std::memcpy(&thr_bits,     &r.threshold, 8);
        std::cout << "    {\"n\":" << r.n << ",\"cut\":" << r.cut
                  << ",\"pre_log\":" << r.pre_log
                  << ",\"pre_log_bits\":\"0x"
                  << std::hex << std::setw(16) << std::setfill('0') << pre_log_bits
                  << std::dec << std::setfill(' ') << "\""
                  << ",\"log_n\":" << r.log_n
                  << ",\"log_n_bits\":\"0x"
                  << std::hex << std::setw(16) << std::setfill('0') << log_n_bits
                  << std::dec << std::setfill(' ') << "\""
                  << ",\"thr\":" << r.threshold
                  << ",\"thr_bits\":\"0x"
                  << std::hex << std::setw(16) << std::setfill('0') << thr_bits
                  << std::dec << std::setfill(' ') << "\""
                  << ",\"wc\":" << (r.wc ? "true" : "false")
                  << ",\"cluster\":";
        emit_int_array(std::cout, r.cluster_nodes);
        std::cout << ",\"in\":";
        emit_int_array(std::cout, r.in_partition);
        std::cout << ",\"out\":";
        emit_int_array(std::cout, r.out_partition);
        std::cout << ",\"pushed\":[";
        for (size_t k = 0; k < r.pushed.size(); k++) {
            if (k) std::cout << ",";
            emit_int_array(std::cout, r.pushed[k]);
        }
        std::cout << "]}";
    }
    std::cout << "\n  ],\n  \"survivors\": [";
    {
        std::queue<std::vector<int>> q = done;
        bool first = true;
        while (!q.empty()) {
            if (!first) std::cout << ",";
            first = false;
            emit_int_array(std::cout, q.front());
            q.pop();
        }
    }
    std::cout << "]\n}\n";

    igraph_destroy(&graph);
    fprintf(stderr, "[TRACE-WCC] PIPELINE_END\n");
    return 0;
}
