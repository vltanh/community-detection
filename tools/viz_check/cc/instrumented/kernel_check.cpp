// CC kernel cross-check, instrumented C++ leg.
//
// Verbatim copy of the CC pipeline in constrained-clustering:
//   src/mincut_only.cpp:4-46          (MincutOnly::main, Simple branch)
//   includes/constrained.h:393-419    (GetConnectedComponents) - shared
//   includes/constrained.h:122-151    (RemoveInterClusterEdges) - shared
//   src/constrained.cpp:32-104        (Get/LoadEdges, InvertMap) - shared
//   includes/constrained.h:71-97      (ReadCommunities) - shared
//   src/constrained.cpp:135-152       (WriteClusterQueue<vector<int>>)
//
// Shared helpers live in ../../_common/tracer_io.h. Per-algo logic
// (the mincut_only.cpp Simple-branch loop) lives in main() below.
//
// Build: ./build.sh -> /tmp/cc_kernel_check
// Run:   /tmp/cc_kernel_check <edge.csv> <com.csv> <out.csv>
// stdout: WriteClusterQueue body + JSON trace.
// stderr: [TRACE-CC ...] structured log.
#include <cstdio>
#include <fstream>
#include <iostream>
#include <queue>
#include <string>
#include <vector>
#include "tracer_io.h"

using namespace viz_check;

// [UPSTREAM constrained.cpp:135-152] WriteClusterQueue (CC + WCC variant)
static void WriteClusterQueue(std::queue<std::vector<int>>& q,
                              const std::map<int, std::string>& new_to_orig,
                              const std::string& output_file) {
    std::ofstream out(output_file);
    int current_cluster_id = 0;
    out << "node_id,cluster_id\n";
    while (!q.empty()) {
        auto cur = q.front(); q.pop();
        for (int new_id : cur) {
            out << new_to_orig.at(new_id) << "," << current_cluster_id << "\n";
        }
        current_cluster_id++;
    }
    fprintf(stderr, "[TRACE-CC] WriteClusterQueue out_clusters=%d\n", current_cluster_id);
}

// [UPSTREAM mincut_only.cpp:4-46] MincutOnly::main, Simple branch
int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr, "usage: %s <edge.csv> <com.csv> <out.csv>\n", argv[0]);
        return 2;
    }
    std::string edge_csv = argv[1];
    std::string com_csv = argv[2];
    std::string out_csv = argv[3];

    fprintf(stderr, "[TRACE-CC] PIPELINE_START edge=%s com=%s\n",
            edge_csv.c_str(), com_csv.c_str());

    // mincut_only.cpp:18-22
    auto orig_to_new = GetOriginalToNewIdMap(edge_csv);
    auto new_to_orig = InvertMap(orig_to_new);
    igraph_t graph;
    igraph_empty(&graph, 0, IGRAPH_UNDIRECTED);
    LoadEdgesFromFile(&graph, edge_csv, orig_to_new);
    fprintf(stderr, "[TRACE-CC] loaded n=%zu m=%lld\n",
            orig_to_new.size(), (long long)igraph_ecount(&graph));

    // mincut_only.cpp:29
    auto partition = ReadCommunities(orig_to_new, com_csv);
    fprintf(stderr, "[TRACE-CC] ReadCommunities assigned=%zu / map=%zu\n",
            partition.size(), orig_to_new.size());

    // mincut_only.cpp:35
    int m_before = igraph_ecount(&graph);
    RemoveInterClusterEdges(&graph, partition);
    fprintf(stderr, "[TRACE-CC] RemoveInterClusterEdges kept=%lld removed=%d\n",
            (long long)igraph_ecount(&graph),
            m_before - (int)igraph_ecount(&graph));

    // mincut_only.cpp:40
    auto components = GetConnectedComponents(&graph);
    fprintf(stderr, "[TRACE-CC] GetConnectedComponents num_components=%zu\n",
            components.size());
    for (size_t i = 0; i < components.size(); i++) {
        fprintf(stderr, "[TRACE-CC]   comp[%zu] size=%zu first=%d\n",
                i, components[i].size(), components[i][0]);
    }

    // mincut_only.cpp:42-45 (Simple branch)
    std::queue<std::vector<int>> done_being_mincut_clusters;
    for (auto& c : components) done_being_mincut_clusters.push(c);
    fprintf(stderr, "[TRACE-CC] DONE_QUEUE size=%zu\n", done_being_mincut_clusters.size());

    WriteClusterQueue(done_being_mincut_clusters, new_to_orig, out_csv);

    // Emit JSON trace to stdout for the JS replay leg.
    std::cout << "{\n";
    std::cout << "  \"n_nodes\": " << orig_to_new.size() << ",\n";
    std::cout << "  \"node_map\": [";
    {
        bool first = true;
        for (auto const& [nid, orig] : new_to_orig) {
            if (!first) std::cout << ",";
            first = false;
            std::cout << "{\"new\":" << nid << ",\"orig\":\"" << orig << "\"}";
        }
    }
    std::cout << "],\n  \"components\": [";
    for (size_t i = 0; i < components.size(); i++) {
        if (i) std::cout << ",";
        emit_int_array(std::cout, components[i]);
    }
    std::cout << "]\n}\n";

    igraph_destroy(&graph);
    fprintf(stderr, "[TRACE-CC] PIPELINE_END\n");
    return 0;
}
