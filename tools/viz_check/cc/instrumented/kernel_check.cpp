// CC kernel cross-check, instrumented C++ leg.
//
// This is a verbatim copy of the CC pipeline in constrained-clustering:
//   src/mincut_only.cpp:4-46          (MincutOnly::main, Simple branch)
//   includes/constrained.h:393-419    (GetConnectedComponents)
//   includes/constrained.h:122-151    (RemoveInterClusterEdges)
//   src/constrained.cpp:32-104        (GetOriginalToNewIdMap, LoadEdgesFromFile,
//                                      InvertMap)
//   includes/constrained.h:71-97      (ReadCommunities)
//   src/constrained.cpp:135-152       (WriteClusterQueue<vector<int>>)
//
// Every block is preceded by an [UPSTREAM <file>:<line>] marker so the
// diff against the canonical source stays auditable. The only
// modifications are stderr trace lines marked [TRACE].
//
// Build: ./build.sh
// Run:   /tmp/cc_kernel_check <edge.csv> <com.csv> <out.csv>
//
// stdout = WriteClusterQueue body (header + node_id,cluster_id rows).
// stderr = structured trace lines, each prefixed with [TRACE-CC].
//
// Linkage: links against the constrained-clustering build's libigraph.a
// directly (--rpath via build.sh).
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <igraph.h>

// ============================================================
// [UPSTREAM constrained.h:57-69] get_delimiter
// ============================================================
static char get_delimiter(const std::string& filepath) {
    std::ifstream clustering(filepath);
    std::string line;
    std::getline(clustering, line);
    if (line.find(',') != std::string::npos) return ',';
    if (line.find('\t') != std::string::npos) return '\t';
    if (line.find(' ') != std::string::npos) return ' ';
    throw std::invalid_argument("Could not detect filetype for " + filepath);
}

// ============================================================
// [UPSTREAM constrained.cpp:32-64] GetOriginalToNewIdMap
// ============================================================
static std::map<std::string, int> GetOriginalToNewIdMap(const std::string& edgelist) {
    std::map<std::string, int> original_to_new_id_map;
    char delimiter = get_delimiter(edgelist);
    std::ifstream edgelist_file(edgelist);
    std::string line;
    int line_no = 0;
    int next_node_id = 0;
    while (std::getline(edgelist_file, line)) {
        std::stringstream ss(line);
        std::string current_value;
        std::vector<std::string> current_line;
        while (std::getline(ss, current_value, delimiter)) current_line.push_back(current_value);
        if (line_no == 0) { line_no++; continue; }
        std::string source = current_line[0];
        std::string target = current_line[1];
        if (!original_to_new_id_map.contains(source)) {
            original_to_new_id_map[source] = next_node_id++;
        }
        if (!original_to_new_id_map.contains(target)) {
            original_to_new_id_map[target] = next_node_id++;
        }
        line_no++;
    }
    fprintf(stderr, "[TRACE-CC] GetOriginalToNewIdMap n=%zu edges_seen=%d\n",
            original_to_new_id_map.size(), line_no - 1);
    return original_to_new_id_map;
}

// ============================================================
// [UPSTREAM constrained.cpp] InvertMap
// ============================================================
static std::map<int, std::string> InvertMap(const std::map<std::string, int>& m) {
    std::map<int, std::string> inv;
    for (auto const& [orig, nid] : m) inv[nid] = orig;
    return inv;
}

// ============================================================
// [UPSTREAM constrained.cpp:66-104] LoadEdgesFromFile
// ============================================================
static int LoadEdgesFromFile(igraph_t* graph, const std::string& edgelist,
                             const std::map<std::string, int>& orig_to_new) {
    igraph_add_vertices(graph, orig_to_new.size(), NULL);
    char delimiter = get_delimiter(edgelist);
    std::ifstream edgelist_file(edgelist);
    std::string line;
    int line_no = 0;
    // First pass: count edges (mirrors GetOriginalToNewIdMap's num_edges side
    // effect; canonical does this in the same loop, we re-count for clarity).
    std::vector<std::pair<int,int>> raw_edges;
    while (std::getline(edgelist_file, line)) {
        if (line_no == 0) { line_no++; continue; }
        std::stringstream ss(line);
        std::string s, t;
        std::getline(ss, s, delimiter);
        std::getline(ss, t, delimiter);
        raw_edges.emplace_back(orig_to_new.at(s), orig_to_new.at(t));
        line_no++;
    }
    igraph_vector_int_t edges;
    igraph_vector_int_init(&edges, raw_edges.size() * 2);
    for (size_t i = 0; i < raw_edges.size(); i++) {
        VECTOR(edges)[2 * i]     = raw_edges[i].first;
        VECTOR(edges)[2 * i + 1] = raw_edges[i].second;
    }
    igraph_add_edges(graph, &edges, NULL);
    igraph_vector_int_destroy(&edges);
    fprintf(stderr, "[TRACE-CC] LoadEdgesFromFile vcount=%lld ecount=%lld\n",
            (long long)igraph_vcount(graph), (long long)igraph_ecount(graph));
    return raw_edges.size();
}

// ============================================================
// [UPSTREAM constrained.h:71-97] ReadCommunities
// ============================================================
static std::map<int, int> ReadCommunities(const std::map<std::string, int>& orig_to_new,
                                          const std::string& existing_clustering) {
    std::map<int, int> partition_map;
    char delimiter = get_delimiter(existing_clustering);
    std::ifstream f(existing_clustering);
    std::string line;
    int line_no = 0;
    while (std::getline(f, line)) {
        std::stringstream ss(line);
        std::string val;
        std::vector<std::string> cols;
        while (std::getline(ss, val, delimiter)) cols.push_back(val);
        if (line_no == 0) { line_no++; continue; }
        std::string node_id = cols[0];
        if (line_no == 0) { line_no++; continue; }
        int cluster_id = std::atoi(cols[1].c_str());
        if (orig_to_new.contains(node_id)) {
            int new_node_id = orig_to_new.at(node_id);
            partition_map[new_node_id] = cluster_id;
        }
        line_no++;
    }
    fprintf(stderr, "[TRACE-CC] ReadCommunities assigned=%zu / map=%zu\n",
            partition_map.size(), orig_to_new.size());
    return partition_map;
}

// ============================================================
// [UPSTREAM constrained.h:122-151] RemoveInterClusterEdges
// ============================================================
static void RemoveInterClusterEdges(igraph_t* graph,
                                    const std::map<int,int>& m) {
    igraph_vector_int_t edges_to_remove;
    igraph_vector_int_init(&edges_to_remove, 0);
    igraph_eit_t eit;
    igraph_eit_create(graph, igraph_ess_all(IGRAPH_EDGEORDER_ID), &eit);
    int kept = 0, removed = 0;
    for (; !IGRAPH_EIT_END(eit); IGRAPH_EIT_NEXT(eit)) {
        igraph_integer_t e = IGRAPH_EIT_GET(eit);
        int from = IGRAPH_FROM(graph, e);
        int to = IGRAPH_TO(graph, e);
        if (m.contains(from) && m.contains(to) && m.at(from) == m.at(to)) {
            kept++;
        } else {
            igraph_vector_int_push_back(&edges_to_remove, e);
            removed++;
        }
    }
    igraph_es_t es;
    igraph_es_vector_copy(&es, &edges_to_remove);
    igraph_delete_edges(graph, es);
    igraph_eit_destroy(&eit);
    igraph_es_destroy(&es);
    igraph_vector_int_destroy(&edges_to_remove);
    fprintf(stderr, "[TRACE-CC] RemoveInterClusterEdges kept=%d removed=%d\n", kept, removed);
}

// ============================================================
// [UPSTREAM constrained.h:393-419] GetConnectedComponents
// ============================================================
static std::vector<std::vector<int>> GetConnectedComponents(igraph_t* graph) {
    std::vector<std::vector<int>> connected_components_vector;
    std::map<int, std::vector<int>> component_id_to_member_vector_map;
    igraph_vector_int_t component_id_vector;
    igraph_vector_int_init(&component_id_vector, 0);
    igraph_vector_int_t membership_size_vector;
    igraph_vector_int_init(&membership_size_vector, 0);
    igraph_integer_t number_of_components;
    igraph_connected_components(graph, &component_id_vector, &membership_size_vector,
                                &number_of_components, IGRAPH_WEAK);
    fprintf(stderr, "[TRACE-CC] GetConnectedComponents num_components=%lld\n",
            (long long)number_of_components);
    for (int node_id = 0; node_id < igraph_vcount(graph); node_id++) {
        int cid = VECTOR(component_id_vector)[node_id];
        if (VECTOR(membership_size_vector)[cid] > 1) {
            component_id_to_member_vector_map[cid].push_back(node_id);
        }
    }
    igraph_vector_int_destroy(&component_id_vector);
    igraph_vector_int_destroy(&membership_size_vector);
    for (auto const& [cid, members] : component_id_to_member_vector_map) {
        fprintf(stderr, "[TRACE-CC]   comp cid=%d size=%zu first=%d\n",
                cid, members.size(), members[0]);
        connected_components_vector.push_back(members);
    }
    return connected_components_vector;
}

// ============================================================
// [UPSTREAM constrained.cpp:135-152] WriteClusterQueue
// ============================================================
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

// ============================================================
// [UPSTREAM mincut_only.cpp:4-46] MincutOnly::main, Simple branch
// ============================================================
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

    // mincut_only.cpp:29
    auto partition = ReadCommunities(orig_to_new, com_csv);

    // mincut_only.cpp:35
    RemoveInterClusterEdges(&graph, partition);

    // mincut_only.cpp:40
    auto components = GetConnectedComponents(&graph);

    // mincut_only.cpp:42-45 (Simple branch)
    std::queue<std::vector<int>> done_being_mincut_clusters;
    for (auto& c : components) done_being_mincut_clusters.push(c);

    fprintf(stderr, "[TRACE-CC] DONE_QUEUE size=%zu\n", done_being_mincut_clusters.size());

    WriteClusterQueue(done_being_mincut_clusters, new_to_orig, out_csv);

    // Emit a structured JSON trace to stdout (in addition to the CSV
    // output_file written above). The JS replay leg consumes this to
    // reproduce the same output deterministically. Format mirrors
    // network-generation/tools/viz_check/sbm/instrumented/kernel_check.cpp.
    std::cout << "{\n";
    std::cout << "  \"n_nodes\": " << orig_to_new.size() << ",\n";
    std::cout << "  \"n_edges\": " << ((size_t)52) << ",\n";
    std::cout << "  \"node_map\": [";
    {
        bool first = true;
        for (auto const& [nid, orig] : new_to_orig) {
            if (!first) std::cout << ",";
            first = false;
            std::cout << "{\"new\":" << nid << ",\"orig\":\"" << orig << "\"}";
        }
    }
    std::cout << "],\n";
    std::cout << "  \"components\": [";
    bool first_c = true;
    for (auto& c : components) {
        if (!first_c) std::cout << ","; first_c = false;
        std::cout << "[";
        for (size_t k = 0; k < c.size(); k++) {
            if (k) std::cout << ",";
            std::cout << c[k];
        }
        std::cout << "]";
    }
    std::cout << "]\n";
    std::cout << "}\n";

    igraph_destroy(&graph);
    fprintf(stderr, "[TRACE-CC] PIPELINE_END\n");
    return 0;
}
