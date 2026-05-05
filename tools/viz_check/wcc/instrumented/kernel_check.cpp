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
//
// Shared helpers in ../../_common/tracer_io.h. Per-algo logic
// (single-thread MinCutWorker loop) lives here.
//
// Build: ./build.sh -> /tmp/wcc_kernel_check
// Run:   /tmp/wcc_kernel_check <edge.csv> <com.csv> <out.csv> [criterion]
//        (criterion defaults to "1log_10(n)")
//
// stdout: JSON trace {pops:[{n, cut, in[], out[], wc, pushed}], survivors}.
// stderr: [TRACE-WCC ...] structured log.
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <queue>
#include <stdexcept>
#include <string>
#include <vector>
#include "tracer_io.h"
#include "mincut_custom.h"

using namespace viz_check;

// [UPSTREAM constrained.h:425-471] IsWellConnected (Logarithmic branch only)
static bool IsWellConnectedLog(double pre_computed_log,
                               int in_size, int out_size, int cut) {
    double thr = pre_computed_log * std::log((double)(in_size + out_size));
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
        MinCutCustom mcc(&sub, "cactus");
        int cut = mcc.ComputeMinCut();
        std::vector<int> in_local = mcc.GetInPartition();
        std::vector<int> out_local = mcc.GetOutPartition();
        bool wc = IsWellConnectedLog(pre_computed_log,
                                     (int)in_local.size(),
                                     (int)out_local.size(), cut);

        PopRecord rec;
        rec.n = (int)cur.size();
        rec.cut = cut;
        rec.threshold = pre_computed_log * std::log((double)cur.size());
        rec.wc = wc;
        rec.cluster_nodes = cur;
        for (int i : in_local)  rec.in_partition.push_back(VECTOR(idmap)[i]);
        for (int i : out_local) rec.out_partition.push_back(VECTOR(idmap)[i]);

        fprintf(stderr, "[TRACE-WCC] POP idx=%d n=%d cut=%d thr=%.6f wc=%s "
                        "in=%zu out=%zu\n",
                pop_idx, rec.n, rec.cut, rec.threshold, wc ? "true" : "false",
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
        std::fprintf(stderr, "usage: %s <edge.csv> <com.csv> <out.csv> [criterion]\n", argv[0]);
        return 2;
    }
    std::string edge_csv = argv[1];
    std::string com_csv  = argv[2];
    std::string out_csv  = argv[3];
    std::string criterion = (argc >= 5) ? argv[4] : "1log_10(n)";

    // Parse Clog_x(n).
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
    double pre_computed_log = C_param / std::log(x_param);
    fprintf(stderr, "[TRACE-WCC] criterion='%s' C=%.6f x=%.6f pre_log=%.6f\n",
            criterion.c_str(), C_param, x_param, pre_computed_log);

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
    RunWCC(&graph, pre_computed_log, to_be_mincut, done, trace);

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
    std::cout << "{\n  \"node_map\": [";
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
    std::cout << std::setprecision(17);
    for (size_t i = 0; i < trace.size(); i++) {
        if (i) std::cout << ",\n";
        const auto& r = trace[i];
        std::cout << "    {\"n\":" << r.n << ",\"cut\":" << r.cut
                  << ",\"thr\":" << r.threshold << ",\"wc\":"
                  << (r.wc ? "true" : "false")
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
