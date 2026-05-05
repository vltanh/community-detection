// CM kernel cross-check, instrumented C++ leg.
//
// Verbatim copy of the CM pipeline from constrained-clustering:
//   src/cm.cpp:3-105                  (CM::main, num_proc=1 path)
//   includes/cm.h:12-39               (RunClusterOnPartition)
//   includes/cm.h:41-155              (MinCutOrClusterWorker, --prune false)
//   includes/constrained.h:301-324    (RunLeidenAndUpdatePartition)
//   includes/constrained.h:335-389    (GetCommunities)
//   includes/constrained.h:393-419    (GetConnectedComponents)
//   includes/constrained.h:122-151    (RemoveInterClusterEdges)
//   includes/constrained.h:425-471    (IsWellConnected, log branch)
//   src/constrained.cpp:32-104        (LoadEdgesFromFile, GetOriginalToNewIdMap)
//   src/constrained.cpp:116-133       (WriteClusterQueue<pair> for lineage)
//
// Single-thread (num_proc = 1), --prune false, --algorithm leiden-cpm,
// hardcoded 1log_10(n) criterion (configurable via argv).
//
// stdout = JSON trace: {init: [...], rounds: [{pops: [...], reclusters: [...]}]}
// stderr = [TRACE-CM ...] structured log.
//
// Linked against constrained-clustering's libinternal_libs.a (gives
// MinCutCustom + libigraph + libleidenalg).
// Per constrained.h:1-4, mincut_custom.h MUST be included first.
#include "mincut_custom.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <queue>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <igraph.h>
#include <libleidenalg/GraphHelper.h>
#include <libleidenalg/CPMVertexPartition.h>
#include <libleidenalg/Optimiser.h>

// ============================================================
// [UPSTREAM constrained.h:57-69] get_delimiter
// ============================================================
static char get_delimiter(const std::string& filepath) {
    std::ifstream f(filepath);
    std::string line;
    std::getline(f, line);
    if (line.find(',') != std::string::npos) return ',';
    if (line.find('\t') != std::string::npos) return '\t';
    if (line.find(' ') != std::string::npos) return ' ';
    throw std::invalid_argument("Could not detect filetype for " + filepath);
}

// ============================================================
// [UPSTREAM constrained.cpp:32-64] GetOriginalToNewIdMap
// ============================================================
static std::map<std::string, int> GetOriginalToNewIdMap(const std::string& edgelist) {
    std::map<std::string, int> m;
    char d = get_delimiter(edgelist);
    std::ifstream f(edgelist);
    std::string line;
    int line_no = 0; int next = 0;
    while (std::getline(f, line)) {
        std::stringstream ss(line);
        std::string val;
        std::vector<std::string> cols;
        while (std::getline(ss, val, d)) cols.push_back(val);
        if (line_no == 0) { line_no++; continue; }
        if (!m.contains(cols[0])) m[cols[0]] = next++;
        if (!m.contains(cols[1])) m[cols[1]] = next++;
        line_no++;
    }
    return m;
}

static std::map<int, std::string> InvertMap(const std::map<std::string, int>& m) {
    std::map<int, std::string> inv;
    for (auto const& [orig, nid] : m) inv[nid] = orig;
    return inv;
}

// ============================================================
// [UPSTREAM constrained.cpp:66-104] LoadEdgesFromFile
// ============================================================
static void LoadEdgesFromFile(igraph_t* g, const std::string& edgelist,
                              const std::map<std::string,int>& m) {
    igraph_add_vertices(g, m.size(), NULL);
    char d = get_delimiter(edgelist);
    std::ifstream f(edgelist);
    std::string line;
    int line_no = 0;
    std::vector<std::pair<int,int>> raw;
    while (std::getline(f, line)) {
        if (line_no == 0) { line_no++; continue; }
        std::stringstream ss(line);
        std::string s, t;
        std::getline(ss, s, d);
        std::getline(ss, t, d);
        raw.emplace_back(m.at(s), m.at(t));
        line_no++;
    }
    igraph_vector_int_t edges;
    igraph_vector_int_init(&edges, raw.size() * 2);
    for (size_t i = 0; i < raw.size(); i++) {
        VECTOR(edges)[2*i] = raw[i].first;
        VECTOR(edges)[2*i+1] = raw[i].second;
    }
    igraph_add_edges(g, &edges, NULL);
    igraph_vector_int_destroy(&edges);
}

// ============================================================
// [UPSTREAM constrained.h:71-97] ReadCommunities
// ============================================================
static std::map<int,int> ReadCommunities(const std::map<std::string,int>& m,
                                         const std::string& path) {
    std::map<int,int> p;
    char d = get_delimiter(path);
    std::ifstream f(path);
    std::string line;
    int line_no = 0;
    while (std::getline(f, line)) {
        std::stringstream ss(line);
        std::string val; std::vector<std::string> cols;
        while (std::getline(ss, val, d)) cols.push_back(val);
        if (line_no == 0) { line_no++; continue; }
        if (m.contains(cols[0])) p[m.at(cols[0])] = std::atoi(cols[1].c_str());
        line_no++;
    }
    return p;
}

// ============================================================
// [UPSTREAM constrained.h:122-151] RemoveInterClusterEdges
// ============================================================
static void RemoveInterClusterEdges(igraph_t* g, const std::map<int,int>& m) {
    igraph_vector_int_t rm; igraph_vector_int_init(&rm, 0);
    igraph_eit_t eit; igraph_eit_create(g, igraph_ess_all(IGRAPH_EDGEORDER_ID), &eit);
    for (; !IGRAPH_EIT_END(eit); IGRAPH_EIT_NEXT(eit)) {
        igraph_integer_t e = IGRAPH_EIT_GET(eit);
        int from = IGRAPH_FROM(g, e), to = IGRAPH_TO(g, e);
        if (m.contains(from) && m.contains(to) && m.at(from) == m.at(to)) {
        } else igraph_vector_int_push_back(&rm, e);
    }
    igraph_es_t es; igraph_es_vector_copy(&es, &rm);
    igraph_delete_edges(g, es);
    igraph_eit_destroy(&eit); igraph_es_destroy(&es); igraph_vector_int_destroy(&rm);
}

// ============================================================
// [UPSTREAM constrained.h:393-419] GetConnectedComponents
// ============================================================
static std::vector<std::vector<int>> GetConnectedComponents(igraph_t* g) {
    std::vector<std::vector<int>> out;
    std::map<int, std::vector<int>> bucket;
    igraph_vector_int_t cid; igraph_vector_int_init(&cid, 0);
    igraph_vector_int_t sz;  igraph_vector_int_init(&sz, 0);
    igraph_integer_t nc;
    igraph_connected_components(g, &cid, &sz, &nc, IGRAPH_WEAK);
    for (int n = 0; n < igraph_vcount(g); n++) {
        int c = VECTOR(cid)[n];
        if (VECTOR(sz)[c] > 1) bucket[c].push_back(n);
    }
    igraph_vector_int_destroy(&cid); igraph_vector_int_destroy(&sz);
    for (auto& [c, members] : bucket) out.push_back(std::move(members));
    return out;
}

// ============================================================
// [UPSTREAM constrained.h:425-471] IsWellConnected (log branch)
// ============================================================
static bool IsWellConnectedLog(double pre_computed_log,
                               int in_size, int out_size, int cut) {
    double thr = pre_computed_log * std::log((double)(in_size + out_size));
    bool is_close = std::abs(thr - cut) <= 1e-9;
    return !is_close && thr < cut;
}

// ============================================================
// [UPSTREAM cm.h:12-39] RunClusterOnPartition (leiden-cpm only)
// + [UPSTREAM constrained.h:301-324] RunLeidenAndUpdatePartition
// ============================================================
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
    // Run Leiden CPM on the induced subgraph.
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
    int cluster_id;       // CM lineage id (input)
    int n;
    int cut;
    double threshold;
    bool wc;
    std::vector<int> cluster_nodes;
    std::vector<int> in_partition;
    std::vector<int> out_partition;
    std::vector<int> in_leiden_membership;   // size = in_partition.size()
    std::vector<int> out_leiden_membership;  // size = out_partition.size()
    std::vector<std::pair<std::vector<int>, int>> children;  // (nodes, parent_cluster_id)
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
    double pre_log = C / std::log(x);
    fprintf(stderr, "[TRACE-CM] criterion='%s' pre_log=%.6f resolution=%.6f seed=%d\n",
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

    // [UPSTREAM cm.cpp:25-57] initial bucket -> first-component-keeps-id,
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
    int previous_done_size = 0;

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
            std::map<int,int> new_to_old, old_to_new;
            for (size_t i = 0; i < cur.size(); i++) {
                new_to_old[i] = VECTOR(idmap)[i];
                old_to_new[VECTOR(idmap)[i]] = i;
            }

            // No --prune: skip the prune loop. Run mincut once.
            MinCutCustom mcc(&sub, "cactus");
            int cut = mcc.ComputeMinCut();
            std::vector<int> in_local = mcc.GetInPartition();
            std::vector<int> out_local = mcc.GetOutPartition();
            bool wc = IsWellConnectedLog(pre_log, in_local.size(), out_local.size(), cut);

            PopRecord rec;
            rec.round = round;
            rec.pop_idx = pop_idx++;
            rec.cluster_id = current_cluster_id;
            rec.n = cur.size();
            rec.cut = cut;
            rec.threshold = pre_log * std::log((double)cur.size());
            rec.wc = wc;
            rec.cluster_nodes = cur;
            for (int i : in_local)  rec.in_partition.push_back(VECTOR(idmap)[i]);
            for (int i : out_local) rec.out_partition.push_back(VECTOR(idmap)[i]);

            fprintf(stderr, "[TRACE-CM]   POP r=%d idx=%d cid=%d n=%d cut=%d thr=%.6f wc=%s\n",
                    rec.round, rec.pop_idx, rec.cluster_id, rec.n, rec.cut, rec.threshold,
                    wc ? "true" : "false");

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
        previous_done_size = done_being_clustered.size();
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

    // Emit JSON trace.
    std::cout << std::setprecision(17) << "{\n  \"node_map\": [";
    {
        bool first = true;
        for (auto& [nid, orig] : new_to_orig) {
            if (!first) std::cout << ",";
            first = false;
            std::cout << "{\"new\":" << nid << ",\"orig\":\"" << orig << "\"}";
        }
    }
    std::cout << "],\n  \"pops\": [\n";
    auto emit_arr = [](const std::vector<int>& v) {
        std::cout << "[";
        for (size_t i = 0; i < v.size(); i++) { if (i) std::cout << ","; std::cout << v[i]; }
        std::cout << "]";
    };
    for (size_t i = 0; i < trace.size(); i++) {
        if (i) std::cout << ",\n";
        const auto& r = trace[i];
        std::cout << "    {\"round\":" << r.round << ",\"pop_idx\":" << r.pop_idx
                  << ",\"cluster_id\":" << r.cluster_id
                  << ",\"n\":" << r.n << ",\"cut\":" << r.cut
                  << ",\"thr\":" << r.threshold
                  << ",\"wc\":" << (r.wc ? "true" : "false")
                  << ",\"cluster\":";
        emit_arr(r.cluster_nodes);
        std::cout << ",\"in\":"; emit_arr(r.in_partition);
        std::cout << ",\"out\":"; emit_arr(r.out_partition);
        std::cout << ",\"in_leiden\":"; emit_arr(r.in_leiden_membership);
        std::cout << ",\"out_leiden\":"; emit_arr(r.out_leiden_membership);
        std::cout << ",\"children\":[";
        for (size_t k = 0; k < r.children.size(); k++) {
            if (k) std::cout << ",";
            std::cout << "{\"parent\":" << r.children[k].second << ",\"nodes\":";
            emit_arr(r.children[k].first);
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
            emit_arr(q.front().first);
            std::cout << "}";
            q.pop();
        }
    }
    std::cout << "]\n}\n";

    igraph_destroy(&graph);
    fprintf(stderr, "[TRACE-CM] PIPELINE_END pops=%zu\n", trace.size());
    return 0;
}
