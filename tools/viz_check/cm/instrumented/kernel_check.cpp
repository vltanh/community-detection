// CM kernel cross-check, instrumented C++ leg with -DCANONICAL_MODE / -DTRACER_MODE toggle.
//
// Verbatim copy of the CM pipeline from constrained-clustering:
//   src/cm.cpp:3-105                  (CM::main, num_proc=1 path)
//   includes/cm.h:12-39               (RunClusterOnPartition)
//   includes/cm.h:41-155              (MinCutOrClusterWorker, --prune false)
//   includes/constrained.h:301-324    (RunLeidenAndUpdatePartition)
//   includes/constrained.h:425-471    (IsWellConnected, log branch)
//   src/mincut_custom.cpp:3-107       (MinCutCustom::ComputeMinCut, INLINED here
//                                      so that VieCut's instrumented sibling
//                                      headers participate in the build via
//                                      -I tools/viz_check/viecut/instrumented/include).
// Shared helpers (Get/Load/Read/Remove/GetCC) live in ../../_common/tracer_io.h.
//
// Compile-time mode toggle (per byte-equal-tracer skill playbook §1):
//   -DCANONICAL_MODE -> std::log + std::unordered_set/map (canonical VieCut
//                       containers via container_swap aliases). tracer_canonical
//                       output bit-equal to unmodified `constrained_clustering CM`.
//   -DTRACER_MODE    -> jsLog (V8-equivalent fdlibm port) + std::set/map
//                       (id-ASC iteration in VieCut's row-H sites). tracer_swapped
//                       output bit-equal to JS production walker.
//
// Single-thread (num_proc = 1), --prune false, --algorithm leiden-cpm,
// hardcoded 1log_10(n) criterion (configurable via argv).
//
// stdout = JSON trace: {init: [...], rounds: [{pops: [...], reclusters: [...]}]}
// stderr = [TRACE-CM ...] structured log.

#if !defined(CANONICAL_MODE) && !defined(TRACER_MODE)
#error "must define -DCANONICAL_MODE or -DTRACER_MODE"
#endif
#if defined(CANONICAL_MODE) && defined(TRACER_MODE)
#error "exactly one of CANONICAL_MODE / TRACER_MODE must be defined"
#endif

// VieCut's instrumented container_swap.h must come BEFORE any VieCut header
// so that container_swap's TracerSet/TracerMap aliases (= std::set/map under
// TRACER_MODE) propagate into mutable_graph + heavy_edges + contract_graph +
// recursive_cactus. Order matches viecut/instrumented/main_traced.cpp.
#include "tools/container_swap.h"

// VieCut chain for inlined MinCutCustom (instrumented sibling headers via
// -I tools/viz_check/viecut/instrumented/include in build.sh).
#include "algorithms/global_mincut/cactus/cactus_mincut.h"
#include "algorithms/global_mincut/noi_minimum_cut.h"
#include "algorithms/global_mincut/minimum_cut.h"
#include "common/configuration.h"
#include "common/definitions.h"
#include "data_structure/mutable_graph.h"
#include "tools/random_functions.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <queue>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
#include <igraph.h>
#include <libleidenalg/GraphHelper.h>
#include <libleidenalg/CPMVertexPartition.h>
#include <libleidenalg/ModularityVertexPartition.h>
#include <libleidenalg/Optimiser.h>
#include "tracer_io.h"

#ifdef TRACER_MODE
  #include "js_math_port.h"
  static inline double LOG_FN(double x) { return viz_check::jsmath::jsLog(x); }
#else
  static inline double LOG_FN(double x) { return std::log(x); }
#endif

using namespace viz_check;

// [UPSTREAM constrained.h:425-471] IsWellConnected (log branch). Routes log
// through LOG_FN so TRACER_MODE picks up jsLog (audit row D closure for WCC
// chain).
static bool IsWellConnectedLog(double pre_computed_log,
                               int in_size, int out_size, int cut) {
    double thr = pre_computed_log * LOG_FN((double)(in_size + out_size));
    bool is_close = std::abs(thr - cut) <= 1e-9;
    return !is_close && thr < cut;
}

// Helpers for emitting int-vector contents inline in stderr probes.
// NB: probes must serialise the FULL vector — truncation breaks
// byte-equal comparison vs the JS side, which emits the full
// children list verbatim. Optional limit kept for backwards-
// compatible call sites that may want to cap; default = no cap.
static std::string vec_to_str(const std::vector<int>& v,
                              size_t limit = (size_t)-1) {
    std::string s = "[";
    for (size_t i = 0; i < v.size(); i++) {
        if (i) s += ",";
        if (i == limit) { s += "..."; break; }
        s += std::to_string(v[i]);
    }
    s += "]";
    return s;
}

// uint64 reinterpret of a double, for byte-equal probe emission.
static inline uint64_t dbits(double x) {
    uint64_t u; std::memcpy(&u, &x, 8); return u;
}

// [UPSTREAM mincut_custom.cpp:3-107] MinCutCustom::ComputeMinCut + GetIn/Out.
// Inlined into the tracer so VieCut's instrumented headers are used (the
// libinternal_libs.a path would baked in canonical headers and defeat the
// TRACER_MODE container swap).
struct InlinedMinCut {
    const igraph_t* graph;
    std::string mincut_type;
    std::vector<int> in_partition;
    std::vector<int> out_partition;
    InlinedMinCut(const igraph_t* g, const std::string& mt = "cactus")
        : graph(g), mincut_type(mt) {}
    int compute() {
        auto cfg = configuration::getConfig();
        cfg->find_most_balanced_cut = true;
        cfg->threads = 1;
        cfg->save_cut = true;
        cfg->set_node_in_cut = true;
        // Per upstream: random_functions::setSeed is COMMENTED OUT. m_mt
        // stays in default-constructed state. JS port mirrors via libstdc++
        // default seed_seq convention (chain VieCut audit row A).
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
            int to = IGRAPH_TO(this->graph, e);
            // [UPSTREAM mincut_custom.cpp:57-65] swap-by-uninit-comparison.
            // The original `target_node = -1; if (from < target_node)` is a
            // no-op since target_node is uninit-then-compared; first branch
            // never taken. Source/target end up as (to, from). Mirrored
            // verbatim for build-pair byte-equality.
            int source_node = -1, target_node = -1;
            if (from < target_node) {
                source_node = from; target_node = to;
            } else {
                source_node = to; target_node = from;
            }
            G->new_edge(source_node, target_node, 1);
        }
        igraph_eit_destroy(&eit);
        G->finish_construction();
        G->computeDegrees();
        std::unique_ptr<minimum_cut> mc;
        if (mincut_type == "cactus")
            mc = std::make_unique<cactus_mincut<std::shared_ptr<mutable_graph>>>();
        else if (mincut_type == "noi")
            mc = std::make_unique<noi_minimum_cut<std::shared_ptr<mutable_graph>>>();
        else
            throw std::runtime_error("Unknown mincut_type: " + mincut_type);
        int edge_cut_size = mc->perform_minimum_cut(G);
        for (int nid = 0; nid < num_nodes; nid++) {
            if (G->getNodeInCut(nid)) in_partition.push_back(nid);
            else out_partition.push_back(nid);
        }
        return edge_cut_size;
    }
};

// [UPSTREAM cm.h:12-39] RunClusterOnPartition + [UPSTREAM constrained.h:301-324,
// 335-391] RunLeidenAndUpdatePartition / GetCommunities. Base algorithm
// dispatch mirrors `constrained.h:335-391`: "leiden-cpm" -> CPMVertexPartition,
// "leiden-mod" -> ModularityVertexPartition. Leiden side stays canonical: per
// Leiden audit row A-M, JS Leiden L4 closed bit-equal under canonical
// libleidenalg + igraph_rng. No TRACER_MODE swap needed on Leiden side.
static std::vector<std::vector<int>>
RunClusterOnPartition(const igraph_t* graph, const std::string& algorithm,
                      double resolution, int seed,
                      std::vector<int>& partition,
                      std::vector<int>* dbg_first_membership = nullptr,
                      // [P0-Gap1] iter1 membership snapshot for cross-iter
                      // bit-equality vs JS opts.initialMembership wiring.
                      std::vector<int>* dbg_iter1_membership = nullptr,
                      // [P0-Gap2 / P0-Gap9] context tag for boundary markers.
                      const std::string& boundary_tag = "") {
    std::vector<std::vector<int>> cluster_vectors;
    igraph_vector_int_t keep, idmap;
    igraph_vector_int_init(&idmap, partition.size());
    igraph_vector_int_init(&keep, partition.size());
    for (size_t i = 0; i < partition.size(); i++) VECTOR(keep)[i] = partition[i];
    igraph_t sub;
    igraph_induced_subgraph_map(graph, &sub, igraph_vss_vector(&keep),
                                IGRAPH_SUBGRAPH_CREATE_FROM_SCRATCH, NULL, &idmap);
    Graph leiden_graph(&sub);
    // [UPSTREAM constrained.h:335-391] GetCommunities branches on algorithm
    // string. JS port at vltanh.github.io/comdet/js/cm/cm.js:87-89 mirrors:
    //   "leiden-cpm" -> CPMVertexPartition(resolution)
    //   "leiden-mod" / "louvain" -> ModularityVertexPartition (resolution ignored)
    MutableVertexPartition* lpart = nullptr;
    if (algorithm == "leiden-cpm") {
        lpart = new CPMVertexPartition(&leiden_graph, resolution);
    } else if (algorithm == "leiden-mod") {
        lpart = new ModularityVertexPartition(&leiden_graph);
    } else {
        fprintf(stderr, "[TRACE-CM] unknown algorithm '%s'\n", algorithm.c_str());
        std::abort();
    }
    std::map<int,int> partition_map;
    Optimiser o;
    // [UPSTREAM constrained.h:359] canonical loops num_iter=2 on ONE
    // Optimiser instance (RNG continues across iter 2). JS production
    // walker (vltanh.github.io/comdet/js/cm/cm.js:runBaseAlgo) calls
    // optimisePartition TWICE with RNG RE-SEEDED on each call, threading
    // iter 1's partition into iter 2 via opts.initialMembership. To stay
    // byte-equal vs JS, the tracer re-seeds the Optimiser between iters.
    // SANCTIONED tracer divergence from unmodified canonical (per
    // layered-canonical flag — three claims #2 = FALSE under canonical
    // continue-RNG vs tracer re-seed-RNG). Trajectory same modulo this.
    //
    // [P0-Gap1 + P0-Gap2 + P0-Gap9] Emit boundary markers around each
    // optimise_partition call so:
    //   - Gap 1: per-iter membership becomes visible (LD_ITER_END dumps
    //     `lpart->membership(*)` after each iter).
    //   - Gap 2: a downstream filter can count [TRACE-LD] records emitted
    //     between LD_BEGIN/LD_END boundaries to get per-call RNG draws.
    //   - Gap 9: LD_RESEED records the explicit pre-iter set_rng_seed
    //     (sanctioned tracer divergence vs unmodified canonical's
    //     continue-RNG-across-iter behaviour; future regression of
    //     canonical-mode iter-2 RNG state would surface here).
    for (int i = 0; i < 2; i++) {
        fprintf(stderr, "[TRACE-CM] LD_RESEED %s iter=%d seed=%d "
                        "build=%s sanctioned=true\n",
                boundary_tag.c_str(), i + 1, seed, k_build_mode);
        o.set_rng_seed(seed);
        fprintf(stderr, "[TRACE-CM] LD_BEGIN %s iter=%d n=%zu\n",
                boundary_tag.c_str(), i + 1, partition.size());
        o.optimise_partition(lpart);
        // Snapshot membership after this iter (lpart->membership returns
        // local lpart id, which is the iter-i result).
        std::vector<int> mem_iter(partition.size());
        for (size_t k = 0; k < partition.size(); k++) {
            mem_iter[k] = lpart->membership(k);
        }
        fprintf(stderr, "[TRACE-CM] LD_ITER_END %s iter=%d mem=%s\n",
                boundary_tag.c_str(), i + 1, vec_to_str(mem_iter).c_str());
        // [P0-Gap1] capture iter-1 membership to caller, mirrored by JS
        // hook (__CM_HOOK_LD_ITER1) so cross-iter wiring is bit-equalled.
        if (i == 0 && dbg_iter1_membership) {
            *dbg_iter1_membership = mem_iter;
        }
        fprintf(stderr, "[TRACE-CM] LD_END %s iter=%d n_comm=%zu\n",
                boundary_tag.c_str(), i + 1, lpart->n_communities());
    }
    igraph_eit_t eit; igraph_eit_create(&sub, igraph_ess_all(IGRAPH_EDGEORDER_ID), &eit);
    std::set<int> visited;
    for (; !IGRAPH_EIT_END(eit); IGRAPH_EIT_NEXT(eit)) {
        igraph_integer_t e = IGRAPH_EIT_GET(eit);
        int from = IGRAPH_FROM(&sub, e), to = IGRAPH_TO(&sub, e);
        if (!visited.contains(from)) { visited.insert(from); partition_map[from] = lpart->membership(from); }
        if (!visited.contains(to)) { visited.insert(to); partition_map[to] = lpart->membership(to); }
    }
    igraph_eit_destroy(&eit);
    if (dbg_first_membership) {
        dbg_first_membership->resize(partition.size());
        for (size_t i = 0; i < partition.size(); i++) {
            auto it = partition_map.find((int)i);
            (*dbg_first_membership)[i] = (it == partition_map.end()) ? -1 : it->second;
        }
    }
    RemoveInterClusterEdges(&sub, partition_map);
    auto comps = GetConnectedComponents(&sub);
    for (auto& c : comps) {
        std::vector<int> tr;
        for (int newid : c) tr.push_back(VECTOR(idmap)[newid]);
        cluster_vectors.push_back(std::move(tr));
    }
    delete lpart;
    igraph_vector_int_destroy(&keep);
    igraph_vector_int_destroy(&idmap);
    igraph_destroy(&sub);
    return cluster_vectors;
}

struct PopRecord {
    int round;
    int pop_idx;
    int cluster_id;       // CM lineage id
    int n;
    int cut;
    double threshold;
    bool wc;
    std::vector<int> cluster_nodes;
    std::vector<int> in_partition;
    std::vector<int> out_partition;
    std::vector<int> in_leiden_membership;          // post-iter-2 (final)
    std::vector<int> out_leiden_membership;
    // [P0-Gap1] iter-1 membership snapshots (pre-iter-2). Bit-compared in
    // self_rng_check against the JS opts.initialMembership wiring.
    std::vector<int> in_leiden_membership_iter1;
    std::vector<int> out_leiden_membership_iter1;
    // [P0-Gap7] threshold decomposition. log_n is the per-call FP primitive
    // input; threshold = pre_log * log_n. Surfaced separately so future
    // -ffp-contract regressions are attributable to the multiply vs the
    // primitive itself.
    double log_n;
    std::vector<std::pair<std::vector<int>, int>> children;
};

// [P0-Gap6] Parent-to-child snapshot emitter. Serialises std::map<int,vec>
// in parent-id ASC; mirrors WriteClusterHistory ordering (constrained.cpp).
static void emit_ptc_snapshot(const std::map<int, std::vector<int>>& ptc,
                              const std::string& phase) {
    fprintf(stderr, "[TRACE-CM] PTC_SNAPSHOT phase=%s rows=%zu\n",
            phase.c_str(), ptc.size());
    for (auto const& [parent_id, children] : ptc) {
        fprintf(stderr, "[TRACE-CM]   PTC_ROW phase=%s parent=%d children=%s\n",
                phase.c_str(), parent_id, vec_to_str(children).c_str());
    }
}

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: %s <edge.csv> <com.csv> <out.csv> "
                             "[criterion] [resolution] [seed] [algorithm]\n",
                             argv[0]);
        return 2;
    }
    std::string edge_csv = argv[1];
    std::string com_csv = argv[2];
    std::string out_csv = argv[3];
    std::string criterion = (argc >= 5) ? argv[4] : "1log_10(n)";
    double resolution = (argc >= 6) ? std::stod(argv[5]) : 0.0001;
    int seed = (argc >= 7) ? std::atoi(argv[6]) : 0;
    // [UPSTREAM constrained.h:335-391] GetCommunities algorithm string.
    // Default "leiden-cpm" preserves the legacy 6-fixture stress matrix.
    // "leiden-mod" enables Leiden-Modularity base method (no resolution).
    std::string algorithm = (argc >= 8) ? argv[7] : "leiden-cpm";

    fprintf(stderr, "[TRACE-CM] PIPELINE_START build=%s edge=%s com=%s algorithm=%s\n",
            k_build_mode, edge_csv.c_str(), com_csv.c_str(), algorithm.c_str());

    // Parse Clog_x(n).
    double C = 1.0, x = 10.0;
    {
        size_t p1 = criterion.find("log_");
        size_t p2 = criterion.find("(");
        if (p1 == std::string::npos || p2 == std::string::npos) {
            fprintf(stderr, "[TRACE-CM] only Clog_x(n) supported, got '%s'\n", criterion.c_str());
            return 2;
        }
        C = std::stod(criterion.substr(0, p1));
        x = std::stod(criterion.substr(p1 + 4, p2 - (p1 + 4)));
    }
    // Pre-compute log per WCC audit row E: precompute c / log(x) ONCE; per-call
    // uses pre_log * log(in+out). LOG_FN routes via jsLog under TRACER_MODE.
    double pre_log = C / LOG_FN(x);
    fprintf(stderr, "[TRACE-CM] criterion='%s' pre_log=%.17g resolution=%.6f seed=%d\n",
            criterion.c_str(), pre_log, resolution, seed);

    auto orig_to_new = GetOriginalToNewIdMap(edge_csv);
    auto new_to_orig = InvertMap(orig_to_new);
    igraph_t graph;
    igraph_empty(&graph, 0, IGRAPH_UNDIRECTED);
    LoadEdgesFromFile(&graph, edge_csv, orig_to_new);
    auto partition = ReadCommunities(orig_to_new, com_csv);
    fprintf(stderr, "[TRACE-CM] loaded n=%lld m=%lld\n",
            (long long)igraph_vcount(&graph), (long long)igraph_ecount(&graph));

    RemoveInterClusterEdges(&graph, partition);
    fprintf(stderr, "[TRACE-CM] after_remove m=%lld\n", (long long)igraph_ecount(&graph));

    // [UPSTREAM cm.cpp:25-57] initial bucket -> first-component-keeps-id;
    // others get fresh ids via parent_to_child_map.
    auto cluster_id_to_node_map = std::map<int, std::vector<int>>{};
    for (auto const& [nid, cid] : partition) cluster_id_to_node_map[cid].push_back(nid);
    int next_cluster_id = -1;
    for (auto const& [cid, _] : cluster_id_to_node_map) {
        if (cid + 1 > next_cluster_id) next_cluster_id = cid + 1;
    }

    std::map<int, std::vector<int>> parent_to_child;
    auto initial_components = GetConnectedComponents(&graph);
    fprintf(stderr, "[TRACE-CM] initial_components=%zu\n", initial_components.size());

    std::queue<std::pair<std::vector<int>, int>> to_be_mincut;
    // [P0-Gap3] INIT_LINEAGE per-component 8-tuple probe. Captures the
    // (comp_idx, first_node, orig_cid, orig_size, sub_size, branch,
    //  parent_cid, current_cid) tuple plus the dedupe-tracking
    // parent_to_child[-1] state after each step. JS port mirrors via
    // __CM_HOOK_INIT_LINEAGE hook in cm.js.
    for (size_t i = 0; i < initial_components.size(); i++) {
        auto& comp = initial_components[i];
        int parent_cluster_id = -1;
        int current_cluster_id = -1;
        int first = comp[0];
        int orig_cid = partition.at(first);
        int orig_size = cluster_id_to_node_map[orig_cid].size();
        int sub_size = comp.size();
        const char* branch;
        if (orig_size == sub_size) {
            current_cluster_id = orig_cid;
            branch = "keep";
        } else {
            bool found = false;
            for (int c : parent_to_child[-1]) if (c == orig_cid) { found = true; break; }
            if (!found) parent_to_child[-1].push_back(orig_cid);
            parent_cluster_id = orig_cid;
            current_cluster_id = next_cluster_id++;
            branch = "fresh";
        }
        parent_to_child[parent_cluster_id].push_back(current_cluster_id);
        fprintf(stderr,
                "[TRACE-CM] INIT_LINEAGE comp_idx=%zu first_node=%d "
                "orig_cid=%d orig_size=%d sub_size=%d branch=%s "
                "parent_cid=%d current_cid=%d "
                "ptc_neg1_after=%s ptc_parent_after=%s\n",
                i, first, orig_cid, orig_size, sub_size, branch,
                parent_cluster_id, current_cluster_id,
                vec_to_str(parent_to_child[-1]).c_str(),
                vec_to_str(parent_to_child[parent_cluster_id]).c_str());
        to_be_mincut.push({comp, current_cluster_id});
    }
    // [P0-Gap6] post-init parent_to_child snapshot. Map content + ordering
    // bit-compared vs JS via __CM_HOOK_PTC_SNAPSHOT.
    emit_ptc_snapshot(parent_to_child, "after_init");

    std::queue<std::pair<std::vector<int>, int>> to_be_clustered;
    std::queue<std::pair<std::vector<int>, int>> done_being_clustered;
    std::vector<PopRecord> trace;
    int round = 0;

    while (true) {
        fprintf(stderr, "[TRACE-CM] ROUND_START round=%d to_be_mincut=%zu\n",
                round, to_be_mincut.size());
        // [UPSTREAM cm.h:41-155] inline single-thread.
        int pop_idx = 0;
        while (!to_be_mincut.empty()) {
            auto cur_pair = to_be_mincut.front();
            std::vector<int> cur = cur_pair.first;
            int current_cluster_id = cur_pair.second;
            to_be_mincut.pop();
            std::set<int> current_cluster_set(cur.begin(), cur.end());

            igraph_vector_int_t keep, idmap;
            igraph_vector_int_init(&idmap, cur.size());
            igraph_vector_int_init(&keep, cur.size());
            for (size_t i = 0; i < cur.size(); i++) VECTOR(keep)[i] = cur[i];
            igraph_t sub;
            igraph_induced_subgraph_map(&graph, &sub, igraph_vss_vector(&keep),
                                        IGRAPH_SUBGRAPH_CREATE_FROM_SCRATCH, NULL, &idmap);
            std::map<int,int> new_to_old;
            for (size_t i = 0; i < cur.size(); i++) new_to_old[i] = VECTOR(idmap)[i];

            // [P0-Gap2] VieCut boundary markers. Downstream filter can
            // count [TRACE-VC] records emitted between VC_BEGIN/VC_END to
            // get the per-pop k_mincut RNG draw count. Default-constructed
            // m_mt state means an off-by-one in any earlier pop silently
            // changes later pops; the boundary markers make per-pop draws
            // diff-attributable.
            fprintf(stderr, "[TRACE-CM]   VC_BEGIN r=%d idx=%d cid=%d n=%zu\n",
                    round, pop_idx, current_cluster_id, cur.size());
            InlinedMinCut mcc(&sub, "cactus");
            int cut = mcc.compute();
            std::vector<int> in_local = mcc.in_partition;
            std::vector<int> out_local = mcc.out_partition;
            fprintf(stderr, "[TRACE-CM]   VC_END r=%d idx=%d cut=%d in=%zu out=%zu\n",
                    round, pop_idx, cut, in_local.size(), out_local.size());
            bool wc = IsWellConnectedLog(pre_log, in_local.size(), out_local.size(), cut);

            PopRecord rec;
            rec.round = round;
            rec.pop_idx = pop_idx++;
            rec.cluster_id = current_cluster_id;
            rec.n = cur.size();
            rec.cut = cut;
            // [P0-Gap7] decompose threshold = pre_log * log(n). log_n is
            // the per-call FP primitive output; persist separately so
            // diff harness can localize a future -ffp-contract regression
            // to either the LOG_FN primitive or the multiply step.
            rec.log_n = LOG_FN((double)cur.size());
            rec.threshold = pre_log * rec.log_n;
            rec.wc = wc;
            rec.cluster_nodes = cur;
            for (int i : in_local)  rec.in_partition.push_back(VECTOR(idmap)[i]);
            for (int i : out_local) rec.out_partition.push_back(VECTOR(idmap)[i]);

            // Reinterpret threshold as uint64 for byte-equal diff harness.
            uint64_t thr_bits = dbits(rec.threshold);
            uint64_t log_n_bits = dbits(rec.log_n);
            uint64_t pre_log_bits = dbits(pre_log);
            fprintf(stderr, "[TRACE-CM]   POP r=%d idx=%d cid=%d n=%d cut=%d "
                            "thr=%.17g thr_bits=0x%016lx wc=%s in=%zu out=%zu\n",
                    rec.round, rec.pop_idx, rec.cluster_id, rec.n, rec.cut,
                    rec.threshold, (unsigned long)thr_bits, wc ? "true" : "false",
                    rec.in_partition.size(), rec.out_partition.size());
            // [P0-Gap7] threshold decomposition probe (log_n_bits +
            // pre_log_bits + product bits). FMA-vs-mul-then-add regression
            // would surface as thr_bits != pre_log_bits*log_n_bits while
            // log_n_bits itself is invariant.
            fprintf(stderr, "[TRACE-CM]   THR_DECOMP r=%d idx=%d n=%d "
                            "log_n=%.17g log_n_bits=0x%016lx "
                            "pre_log_bits=0x%016lx thr_bits=0x%016lx\n",
                    rec.round, rec.pop_idx, rec.n,
                    rec.log_n, (unsigned long)log_n_bits,
                    (unsigned long)pre_log_bits, (unsigned long)thr_bits);

            int round_singleton_count = 0;
            if (wc) {
                std::vector<int> done_nodes(current_cluster_set.begin(), current_cluster_set.end());
                done_being_clustered.push({done_nodes, current_cluster_id});
            } else {
                // [UPSTREAM cm.h:131-149] each side > 1 -> RunClusterOnPartition.
                std::vector<std::vector<int>> sides = {in_local, out_local};
                for (int side_idx = 0; side_idx < 2; side_idx++) {
                    auto& side = sides[side_idx];
                    if (side.size() <= 1) continue;
                    std::vector<int> dbg_mem;
                    std::vector<int> dbg_mem_iter1;
                    // [P0-Gap2] Leiden boundary markers (one per side).
                    // Downstream filter counts [TRACE-LD] records between
                    // LD_BEGIN/LD_END to get per-call k_leiden draws.
                    char tag[64];
                    snprintf(tag, sizeof(tag), "r=%d idx=%d cid=%d side=%s",
                             round, rec.pop_idx, current_cluster_id,
                             side_idx == 0 ? "in" : "out");
                    auto subclusters = RunClusterOnPartition(
                        &sub, algorithm, resolution, seed, side,
                        &dbg_mem, &dbg_mem_iter1, std::string(tag));
                    if (side_idx == 0) {
                        rec.in_leiden_membership = dbg_mem;
                        rec.in_leiden_membership_iter1 = dbg_mem_iter1;
                    } else {
                        rec.out_leiden_membership = dbg_mem;
                        rec.out_leiden_membership_iter1 = dbg_mem_iter1;
                    }
                    fprintf(stderr, "[TRACE-CM]     RECLUSTER side=%s size=%zu -> %zu sub\n",
                            side_idx == 0 ? "in" : "out", side.size(), subclusters.size());
                    for (auto& sc : subclusters) {
                        std::vector<int> tr;
                        for (int local : sc) tr.push_back(new_to_old[local]);
                        // [UPSTREAM cm.h:138-148] cpp pushes EVERY translated
                        // cluster regardless of size (no size>1 filter at this
                        // site). [P0-Gap5] singleton-incidence probe: emit
                        // is_singleton flag inline so self_rng_check can
                        // assert per-recluster singleton-push count parity
                        // independently of cell-level PASS.
                        bool is_singleton = (tr.size() == 1);
                        if (is_singleton) round_singleton_count++;
                        fprintf(stderr, "[TRACE-CM]       PUSH side=%s size=%zu is_singleton=%s\n",
                                side_idx == 0 ? "in" : "out", tr.size(),
                                is_singleton ? "true" : "false");
                        to_be_clustered.push({tr, current_cluster_id});
                        rec.children.push_back({tr, current_cluster_id});
                    }
                }
            }
            // [P0-Gap5] per-pop singleton count (subset of round count).
            fprintf(stderr, "[TRACE-CM]   POP_SINGLETON_COUNT r=%d idx=%d count=%d\n",
                    round, rec.pop_idx, round_singleton_count);

            trace.push_back(std::move(rec));
            igraph_vector_int_destroy(&keep);
            igraph_vector_int_destroy(&idmap);
            igraph_destroy(&sub);
        }
        // [UPSTREAM cm.cpp:78-95] move to_be_clustered -> to_be_mincut with fresh ids.
        if (to_be_clustered.empty()) {
            fprintf(stderr, "[TRACE-CM] ROUND_END round=%d done_total=%zu (TERMINATE)\n",
                    round, done_being_clustered.size());
            // [P0-Gap6] terminal parent_to_child snapshot.
            emit_ptc_snapshot(parent_to_child, "terminal");
            break;
        }
        // [P0-Gap4] END_ROUND_DRAIN per-assignment probe. Captures
        // (round, drain_idx, parent_cid, fresh_id, ptc_size_after) on
        // every fresh-id assignment, exposing iteration order of the
        // to_be_clustered queue.
        size_t drain_idx = 0;
        size_t drain_size_at_start = to_be_clustered.size();
        std::vector<int> fresh_ids_assigned;
        fprintf(stderr, "[TRACE-CM] END_ROUND_DRAIN_BEGIN round=%d drain_size=%zu\n",
                round, drain_size_at_start);
        while (!to_be_clustered.empty()) {
            auto cur_pair = to_be_clustered.front(); to_be_clustered.pop();
            int fresh_id = next_cluster_id;
            int parent_cid = cur_pair.second;
            parent_to_child[parent_cid].push_back(fresh_id);
            to_be_mincut.push({cur_pair.first, fresh_id});
            next_cluster_id++;
            fresh_ids_assigned.push_back(fresh_id);
            fprintf(stderr,
                    "[TRACE-CM]   END_ROUND_DRAIN round=%d drain_idx=%zu "
                    "parent_cid=%d fresh_id=%d ptc_size_after=%zu "
                    "ptc_parent_after=%s\n",
                    round, drain_idx, parent_cid, fresh_id,
                    parent_to_child.size(),
                    vec_to_str(parent_to_child[parent_cid]).c_str());
            drain_idx++;
        }
        fprintf(stderr,
                "[TRACE-CM] END_ROUND_DRAIN_END round=%d fresh_ids=%s\n",
                round, vec_to_str(fresh_ids_assigned).c_str());
        // [P0-Gap6] per-round parent_to_child snapshot (also captures
        // end-of-round drain effects).
        emit_ptc_snapshot(parent_to_child, std::string("after_round_") + std::to_string(round));
        round++;
    }

    // WriteClusterQueue<pair> with lineage ids.
    {
        std::ofstream out(out_csv);
        out << "node_id,cluster_id\n";
        std::queue<std::pair<std::vector<int>, int>> q = done_being_clustered;
        while (!q.empty()) {
            auto cur = q.front(); q.pop();
            for (int nid : cur.first) out << new_to_orig.at(nid) << "," << cur.second << "\n";
        }
    }

    // Emit JSON trace (stdout-clean; full precision so uint64 reinterpret
    // round-trips).
    std::cout << std::setprecision(17) << "{\n  \"build\": \"" << k_build_mode << "\",\n  \"node_map\": [";
    {
        bool first = true;
        for (auto& [nid, orig] : new_to_orig) {
            if (!first) std::cout << ",";
            first = false;
            std::cout << "{\"new\":" << nid << ",\"orig\":\"" << orig << "\"}";
        }
    }
    std::cout << "],\n  \"pre_log\": " << pre_log << ",\n";
    std::cout << "  \"pops\": [\n";
    for (size_t i = 0; i < trace.size(); i++) {
        if (i) std::cout << ",\n";
        const auto& r = trace[i];
        std::cout << "    {\"round\":" << r.round << ",\"pop_idx\":" << r.pop_idx
                  << ",\"cluster_id\":" << r.cluster_id
                  << ",\"n\":" << r.n << ",\"cut\":" << r.cut
                  << ",\"thr\":" << r.threshold
                  // [P0-Gap7] log_n surfaced in JSON so self_rng_check can
                  // bit-compare the FP primitive output separately from the
                  // multiply.
                  << ",\"log_n\":" << r.log_n
                  << ",\"wc\":" << (r.wc ? "true" : "false")
                  << ",\"cluster\":";
        emit_int_array(std::cout, r.cluster_nodes);
        std::cout << ",\"in\":"; emit_int_array(std::cout, r.in_partition);
        std::cout << ",\"out\":"; emit_int_array(std::cout, r.out_partition);
        std::cout << ",\"in_leiden\":"; emit_int_array(std::cout, r.in_leiden_membership);
        std::cout << ",\"out_leiden\":"; emit_int_array(std::cout, r.out_leiden_membership);
        // [P0-Gap1] iter-1 memberships in JSON for self_rng_check.
        std::cout << ",\"in_leiden_iter1\":"; emit_int_array(std::cout, r.in_leiden_membership_iter1);
        std::cout << ",\"out_leiden_iter1\":"; emit_int_array(std::cout, r.out_leiden_membership_iter1);
        std::cout << ",\"children\":[";
        for (size_t k = 0; k < r.children.size(); k++) {
            if (k) std::cout << ",";
            std::cout << "{\"parent\":" << r.children[k].second << ",\"nodes\":";
            emit_int_array(std::cout, r.children[k].first);
            std::cout << "}";
        }
        std::cout << "]}";
    }
    std::cout << "\n  ],\n  \"survivors\": [";
    {
        std::queue<std::pair<std::vector<int>, int>> q = done_being_clustered;
        bool first = true;
        while (!q.empty()) {
            if (!first) std::cout << ",";
            first = false;
            std::cout << "{\"id\":" << q.front().second << ",\"nodes\":";
            emit_int_array(std::cout, q.front().first);
            std::cout << "}";
            q.pop();
        }
    }
    // [P0-Gap6] final parent_to_child map in JSON, parent-id ASC iteration.
    // Bit-compared against JS parentToChild via self_rng_check.
    std::cout << "],\n  \"parent_to_child\": [";
    {
        bool first = true;
        for (auto const& [parent_id, children] : parent_to_child) {
            if (!first) std::cout << ",";
            first = false;
            std::cout << "{\"parent\":" << parent_id << ",\"children\":";
            emit_int_array(std::cout, children);
            std::cout << "}";
        }
    }
    std::cout << "]\n}\n";

    igraph_destroy(&graph);
    fprintf(stderr, "[TRACE-CM] PIPELINE_END pops=%zu\n", trace.size());
    return 0;
}
