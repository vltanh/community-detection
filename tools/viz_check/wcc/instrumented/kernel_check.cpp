// WCC kernel cross-check, instrumented C++ leg.
//
// Verbatim copy of the WCC pipeline from constrained-clustering:
//   src/mincut_only.cpp:4-103         (MincutOnly::main, Logarithmic branch)
//   includes/mincut_only.h:39-132     (MinCutWorker single-threaded path)
//   includes/mincut_only.h:13-37      (GetConnectedComponentsOnPartition)
//   includes/constrained.h:425-471    (IsWellConnected)
//   includes/constrained.h:393-419    (GetConnectedComponents)
//   includes/constrained.h:122-151    (RemoveInterClusterEdges)
//   src/constrained.cpp:32-104        (Get/LoadEdgesFromFile, GetOriginalToNewIdMap)
//   src/constrained.cpp:135-152       (WriteClusterQueue<vector<int>>)
//
// Link against constrained-clustering's libinternal_libs.a (which gives
// MinCutCustom from VieCut). Single-threaded path forced (num_proc = 1)
// so the queue order is deterministic.
//
// Build: ./build.sh
// Run:   /tmp/wcc_kernel_check <edge.csv> <com.csv> <out.csv>
//        --connectedness-criterion 1log_10(n) (hardcoded for now)
//
// stdout = JSON trace: {pops:[{n, cut, in[], out[], wc}, ...],
//                       survivors:[[ids],...]}
// stderr = [TRACE-WCC ...] structured log.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <igraph.h>
#include "mincut_custom.h"

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
    igraph_vector_int_t rm;
    igraph_vector_int_init(&rm, 0);
    igraph_eit_t eit;
    igraph_eit_create(g, igraph_ess_all(IGRAPH_EDGEORDER_ID), &eit);
    for (; !IGRAPH_EIT_END(eit); IGRAPH_EIT_NEXT(eit)) {
        igraph_integer_t e = IGRAPH_EIT_GET(eit);
        int from = IGRAPH_FROM(g, e), to = IGRAPH_TO(g, e);
        if (m.contains(from) && m.contains(to) && m.at(from) == m.at(to)) {
            // keep
        } else {
            igraph_vector_int_push_back(&rm, e);
        }
    }
    igraph_es_t es;
    igraph_es_vector_copy(&es, &rm);
    igraph_delete_edges(g, es);
    igraph_eit_destroy(&eit);
    igraph_es_destroy(&es);
    igraph_vector_int_destroy(&rm);
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
    igraph_vector_int_destroy(&cid);
    igraph_vector_int_destroy(&sz);
    for (auto& [c, members] : bucket) out.push_back(std::move(members));
    return out;
}

// ============================================================
// [UPSTREAM mincut_only.h:13-37] GetConnectedComponentsOnPartition
// ============================================================
static std::vector<std::vector<int>>
GetConnectedComponentsOnPartition(const igraph_t* g, std::vector<int>& partition) {
    std::vector<std::vector<int>> out;
    igraph_vector_int_t keep, idmap;
    igraph_vector_int_init(&idmap, partition.size());
    igraph_vector_int_init(&keep, partition.size());
    for (size_t i = 0; i < partition.size(); i++) VECTOR(keep)[i] = partition[i];
    igraph_t sub;
    igraph_induced_subgraph_map(g, &sub, igraph_vss_vector(&keep),
                                IGRAPH_SUBGRAPH_CREATE_FROM_SCRATCH, NULL, &idmap);
    auto comps = GetConnectedComponents(&sub);
    for (auto& c : comps) {
        std::vector<int> tr;
        for (int newid : c) tr.push_back(VECTOR(idmap)[newid]);
        out.push_back(std::move(tr));
    }
    igraph_vector_int_destroy(&keep);
    igraph_vector_int_destroy(&idmap);
    igraph_destroy(&sub);
    return out;
}

// ============================================================
// [UPSTREAM constrained.h:425-471] IsWellConnected (Logarithmic branch only)
// ============================================================
static bool IsWellConnectedLog(double pre_computed_log,
                               int in_size, int out_size, int cut) {
    double thr = pre_computed_log * std::log((double)(in_size + out_size));
    bool is_close = std::abs(thr - cut) <= 1e-9;
    return !is_close && thr < cut;
}

// ============================================================
// [UPSTREAM mincut_only.h:39-132] MinCutWorker (single-threaded inline)
// + [UPSTREAM mincut_only.cpp:51-95] outer round loop
//
// Single-threaded: num_proc = 1 always; sentinel is unnecessary, we
// drain the queue directly.
// ============================================================
struct PopRecord {
    int n;
    int cut;
    double threshold;
    bool wc;
    std::vector<int> cluster_nodes;
    std::vector<int> in_partition;   // global ids
    std::vector<int> out_partition;  // global ids
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
        // Subgraph on `cur`.
        igraph_vector_int_t keep, idmap;
        igraph_vector_int_init(&idmap, cur.size());
        igraph_vector_int_init(&keep, cur.size());
        for (size_t i = 0; i < cur.size(); i++) VECTOR(keep)[i] = cur[i];
        igraph_t sub;
        igraph_induced_subgraph_map(graph, &sub, igraph_vss_vector(&keep),
                                    IGRAPH_SUBGRAPH_CREATE_FROM_SCRATCH, NULL, &idmap);
        // Mincut.
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
        // Translate in_local + out_local to global ids.
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
            if (in_local.size() > 1) {
                auto comps = GetConnectedComponentsOnPartition(&sub, in_local);
                for (auto& comp : comps) {
                    std::vector<int> tr;
                    for (int i : comp) tr.push_back(VECTOR(idmap)[i]);
                    if (tr.size() > 1) {
                        to_be_mincut.push(tr);
                        rec.pushed.push_back(tr);
                        fprintf(stderr, "[TRACE-WCC]   PUSH in size=%zu\n", tr.size());
                    } else {
                        fprintf(stderr, "[TRACE-WCC]   DROP in size=%zu\n", tr.size());
                    }
                }
            }
            if (out_local.size() > 1) {
                auto comps = GetConnectedComponentsOnPartition(&sub, out_local);
                for (auto& comp : comps) {
                    std::vector<int> tr;
                    for (int i : comp) tr.push_back(VECTOR(idmap)[i]);
                    if (tr.size() > 1) {
                        to_be_mincut.push(tr);
                        rec.pushed.push_back(tr);
                        fprintf(stderr, "[TRACE-WCC]   PUSH out size=%zu\n", tr.size());
                    } else {
                        fprintf(stderr, "[TRACE-WCC]   DROP out size=%zu\n", tr.size());
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

static void emit_int_array(std::ostream& os, const std::vector<int>& v) {
    os << "[";
    for (size_t i = 0; i < v.size(); i++) {
        if (i) os << ",";
        os << v[i];
    }
    os << "]";
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

    fprintf(stderr, "[TRACE-WCC] survivors=%zu total_pops=%zu\n", done.size(), trace.size());

    // WriteClusterQueue on `done`.
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

    // Emit JSON trace to stdout.
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
