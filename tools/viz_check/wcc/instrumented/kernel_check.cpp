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
static bool IsWellConnectedLog(double pre_computed_log,
                               int in_size, int out_size, int cut) {
    double thr = pre_computed_log * JSLOG((double)(in_size + out_size));
    bool is_close = std::abs(thr - cut) <= 1e-9;
    return !is_close && thr < cut;
}

// [UPSTREAM mincut_only.h:39-132 + mincut_only.cpp:51-95]
// Single-threaded MinCutWorker (num_proc=1) inlined into the round loop.
struct PopRecord {
    int n;
    int cut;
    double threshold;
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
    int pop_idx = 0;
    while (!to_be_mincut.empty()) {
        std::vector<int> cur = to_be_mincut.front();
        to_be_mincut.pop();
        igraph_vector_int_t keep, idmap;
        igraph_vector_int_init(&idmap, cur.size());
        igraph_vector_int_init(&keep, cur.size());
        for (size_t i = 0; i < cur.size(); i++) VECTOR(keep)[i] = cur[i];
        igraph_t sub;
        igraph_induced_subgraph_map(graph, &sub, igraph_vss_vector(&keep),
                                    IGRAPH_SUBGRAPH_CREATE_FROM_SCRATCH, NULL, &idmap);
        MincutOp mcc(&sub, mincut_type);
        int cut = mcc.ComputeMinCut();
        std::vector<int> in_local = mcc.GetInPartition();
        std::vector<int> out_local = mcc.GetOutPartition();
        bool wc = IsWellConnectedLog(pre_computed_log,
                                     (int)in_local.size(),
                                     (int)out_local.size(), cut);

        PopRecord rec;
        rec.n = (int)cur.size();
        rec.cut = cut;
        rec.threshold = pre_computed_log * JSLOG((double)cur.size());
        rec.wc = wc;
        rec.cluster_nodes = cur;
        for (int i : in_local)  rec.in_partition.push_back(VECTOR(idmap)[i]);
        for (int i : out_local) rec.out_partition.push_back(VECTOR(idmap)[i]);

        // Bit-reinterpret threshold for the trace log so divergence is visible.
        uint64_t thr_bits;
        std::memcpy(&thr_bits, &rec.threshold, 8);
        fprintf(stderr,
                "[TRACE-WCC] POP idx=%d n=%d cut=%d thr=%.17g thr_bits=0x%016llx wc=%s "
                "in=%zu out=%zu\n",
                pop_idx, rec.n, rec.cut, rec.threshold,
                (unsigned long long)thr_bits,
                wc ? "true" : "false",
                rec.in_partition.size(), rec.out_partition.size());

        if (wc) {
            done.push(cur);
        } else {
            // GetConnectedComponentsOnPartition for each side, push back.
            // Order: in-side then out-side (mirrors mincut_only.h:97-122).
            for (int side_idx = 0; side_idx < 2; side_idx++) {
                auto& side_local = side_idx == 0 ? in_local : out_local;
                if (side_local.size() <= 1) continue;
                auto comps = GetConnectedComponentsOnPartition(&sub, side_local);
                for (auto& comp : comps) {
                    std::vector<int> tr;
                    for (int i : comp) tr.push_back(VECTOR(idmap)[i]);
                    if (tr.size() > 1) {
                        to_be_mincut.push(tr);
                        rec.pushed.push_back(tr);
                        fprintf(stderr, "[TRACE-WCC]   PUSH %s size=%zu\n",
                                side_idx == 0 ? "in" : "out", tr.size());
                    } else {
                        fprintf(stderr, "[TRACE-WCC]   DROP %s size=%zu\n",
                                side_idx == 0 ? "in" : "out", tr.size());
                    }
                }
            }
        }
        trace.push_back(std::move(rec));
        igraph_vector_int_destroy(&keep);
        igraph_vector_int_destroy(&idmap);
        igraph_destroy(&sub);
        pop_idx++;
    }
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

    auto partition = ReadCommunities(orig_to_new, com_csv);
    RemoveInterClusterEdges(&graph, partition);
    fprintf(stderr, "[TRACE-WCC] after_remove m=%lld\n",
            (long long)igraph_ecount(&graph));

    auto components = GetConnectedComponents(&graph);
    fprintf(stderr, "[TRACE-WCC] initial_components=%zu\n", components.size());

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
        uint64_t thr_bits;
        std::memcpy(&thr_bits, &r.threshold, 8);
        std::cout << "    {\"n\":" << r.n << ",\"cut\":" << r.cut
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
