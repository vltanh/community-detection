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

// [UPSTREAM cm.h:12-39] RunClusterOnPartition (leiden-cpm only)
// + [UPSTREAM constrained.h:301-324] RunLeidenAndUpdatePartition.
// Leiden side stays canonical: per Leiden audit row A-M, JS Leiden L4 closed
// bit-equal under canonical libleidenalg + igraph_rng. No TRACER_MODE swap
// needed on Leiden side.
static std::vector<std::vector<int>>
RunClusterOnPartition(const igraph_t* graph, double resolution, int seed,
                      std::vector<int>& partition,
                      std::vector<int>* dbg_first_membership = nullptr) {
    std::vector<std::vector<int>> cluster_vectors;
    igraph_vector_int_t keep, idmap;
    igraph_vector_int_init(&idmap, partition.size());
    igraph_vector_int_init(&keep, partition.size());
    for (size_t i = 0; i < partition.size(); i++) VECTOR(keep)[i] = partition[i];
    igraph_t sub;
    igraph_induced_subgraph_map(graph, &sub, igraph_vss_vector(&keep),
                                IGRAPH_SUBGRAPH_CREATE_FROM_SCRATCH, NULL, &idmap);
    Graph leiden_graph(&sub);
    CPMVertexPartition lpart(&leiden_graph, resolution);
    std::map<int,int> partition_map;
    Optimiser o;
    o.set_rng_seed(seed);
    for (int i = 0; i < 2; i++) o.optimise_partition(&lpart);
    igraph_eit_t eit; igraph_eit_create(&sub, igraph_ess_all(IGRAPH_EDGEORDER_ID), &eit);
    std::set<int> visited;
    for (; !IGRAPH_EIT_END(eit); IGRAPH_EIT_NEXT(eit)) {
        igraph_integer_t e = IGRAPH_EIT_GET(eit);
        int from = IGRAPH_FROM(&sub, e), to = IGRAPH_TO(&sub, e);
        if (!visited.contains(from)) { visited.insert(from); partition_map[from] = lpart.membership(from); }
        if (!visited.contains(to)) { visited.insert(to); partition_map[to] = lpart.membership(to); }
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
    std::vector<int> in_leiden_membership;
    std::vector<int> out_leiden_membership;
    std::vector<std::pair<std::vector<int>, int>> children;
};

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: %s <edge.csv> <com.csv> <out.csv> "
                             "[criterion] [resolution] [seed]\n", argv[0]);
        return 2;
    }
    std::string edge_csv = argv[1];
    std::string com_csv = argv[2];
    std::string out_csv = argv[3];
    std::string criterion = (argc >= 5) ? argv[4] : "1log_10(n)";
    double resolution = (argc >= 6) ? std::stod(argv[5]) : 0.0001;
    int seed = (argc >= 7) ? std::atoi(argv[6]) : 0;

    fprintf(stderr, "[TRACE-CM] PIPELINE_START build=%s edge=%s com=%s\n",
            k_build_mode, edge_csv.c_str(), com_csv.c_str());

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
    for (auto& comp : initial_components) {
        int parent_cluster_id = -1;
        int current_cluster_id = -1;
        int first = comp[0];
        int orig_cid = partition.at(first);
        int orig_size = cluster_id_to_node_map[orig_cid].size();
        int sub_size = comp.size();
        if (orig_size == sub_size) {
            current_cluster_id = orig_cid;
        } else {
            bool found = false;
            for (int c : parent_to_child[-1]) if (c == orig_cid) { found = true; break; }
            if (!found) parent_to_child[-1].push_back(orig_cid);
            parent_cluster_id = orig_cid;
            current_cluster_id = next_cluster_id++;
        }
        parent_to_child[parent_cluster_id].push_back(current_cluster_id);
        to_be_mincut.push({comp, current_cluster_id});
    }

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

            // No --prune: skip the prune loop. Run mincut once via inlined
            // VieCut (instrumented headers picked up via -I order).
            InlinedMinCut mcc(&sub, "cactus");
            int cut = mcc.compute();
            std::vector<int> in_local = mcc.in_partition;
            std::vector<int> out_local = mcc.out_partition;
            bool wc = IsWellConnectedLog(pre_log, in_local.size(), out_local.size(), cut);

            PopRecord rec;
            rec.round = round;
            rec.pop_idx = pop_idx++;
            rec.cluster_id = current_cluster_id;
            rec.n = cur.size();
            rec.cut = cut;
            rec.threshold = pre_log * LOG_FN((double)cur.size());
            rec.wc = wc;
            rec.cluster_nodes = cur;
            for (int i : in_local)  rec.in_partition.push_back(VECTOR(idmap)[i]);
            for (int i : out_local) rec.out_partition.push_back(VECTOR(idmap)[i]);

            // Reinterpret threshold as uint64 for byte-equal diff harness.
            uint64_t thr_bits;
            std::memcpy(&thr_bits, &rec.threshold, 8);
            fprintf(stderr, "[TRACE-CM]   POP r=%d idx=%d cid=%d n=%d cut=%d "
                            "thr=%.17g thr_bits=0x%016lx wc=%s in=%zu out=%zu\n",
                    rec.round, rec.pop_idx, rec.cluster_id, rec.n, rec.cut,
                    rec.threshold, (unsigned long)thr_bits, wc ? "true" : "false",
                    rec.in_partition.size(), rec.out_partition.size());

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
                    auto subclusters = RunClusterOnPartition(&sub, resolution, seed, side, &dbg_mem);
                    if (side_idx == 0) rec.in_leiden_membership = dbg_mem;
                    else rec.out_leiden_membership = dbg_mem;
                    fprintf(stderr, "[TRACE-CM]     RECLUSTER side=%s size=%zu -> %zu sub\n",
                            side_idx == 0 ? "in" : "out", side.size(), subclusters.size());
                    for (auto& sc : subclusters) {
                        std::vector<int> tr;
                        for (int local : sc) tr.push_back(new_to_old[local]);
                        // [UPSTREAM cm.h:138-148] cpp pushes EVERY translated
                        // cluster regardless of size (no size>1 filter at this
                        // site). Tracer probe captures per-push size for the
                        // singleton-asymmetry resolution check (audit row I
                        // open question).
                        fprintf(stderr, "[TRACE-CM]       PUSH side=%s size=%zu\n",
                                side_idx == 0 ? "in" : "out", tr.size());
                        to_be_clustered.push({tr, current_cluster_id});
                        rec.children.push_back({tr, current_cluster_id});
                    }
                }
            }

            trace.push_back(std::move(rec));
            igraph_vector_int_destroy(&keep);
            igraph_vector_int_destroy(&idmap);
            igraph_destroy(&sub);
        }
        // [UPSTREAM cm.cpp:78-95] move to_be_clustered -> to_be_mincut with fresh ids.
        if (to_be_clustered.empty()) {
            fprintf(stderr, "[TRACE-CM] ROUND_END round=%d done_total=%zu (TERMINATE)\n",
                    round, done_being_clustered.size());
            break;
        }
        while (!to_be_clustered.empty()) {
            auto cur_pair = to_be_clustered.front(); to_be_clustered.pop();
            parent_to_child[cur_pair.second].push_back(next_cluster_id);
            to_be_mincut.push({cur_pair.first, next_cluster_id});
            next_cluster_id++;
        }
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
                  << ",\"wc\":" << (r.wc ? "true" : "false")
                  << ",\"cluster\":";
        emit_int_array(std::cout, r.cluster_nodes);
        std::cout << ",\"in\":"; emit_int_array(std::cout, r.in_partition);
        std::cout << ",\"out\":"; emit_int_array(std::cout, r.out_partition);
        std::cout << ",\"in_leiden\":"; emit_int_array(std::cout, r.in_leiden_membership);
        std::cout << ",\"out_leiden\":"; emit_int_array(std::cout, r.out_leiden_membership);
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
    std::cout << "]\n}\n";

    igraph_destroy(&graph);
    fprintf(stderr, "[TRACE-CM] PIPELINE_END pops=%zu\n", trace.size());
    return 0;
}
