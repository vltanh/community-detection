// Shared C++ helpers for the CD viz_check tracers.
//
// Verbatim copies of constrained-clustering's edge-list / clustering /
// connected-components helpers, with [UPSTREAM <file>:<line>] markers
// preserved. Each tracer (cc/wcc/cm) used to ship its own copy; this
// header is the single source-of-truth.
//
// Header-only (static inline) so each tracer.cpp can include and link
// without an extra .o.
#ifndef VIZ_CHECK_TRACER_IO_H
#define VIZ_CHECK_TRACER_IO_H

#include <fstream>
#include <map>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <igraph.h>

namespace viz_check {

// [UPSTREAM constrained.h:57-69]
static inline char get_delimiter(const std::string& filepath) {
    std::ifstream f(filepath);
    std::string line;
    std::getline(f, line);
    if (line.find(',') != std::string::npos) return ',';
    if (line.find('\t') != std::string::npos) return '\t';
    if (line.find(' ') != std::string::npos) return ' ';
    throw std::invalid_argument("Could not detect filetype for " + filepath);
}

// [UPSTREAM constrained.cpp:32-64]
static inline std::map<std::string, int>
GetOriginalToNewIdMap(const std::string& edgelist) {
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

static inline std::map<int, std::string>
InvertMap(const std::map<std::string, int>& m) {
    std::map<int, std::string> inv;
    for (auto const& [orig, nid] : m) inv[nid] = orig;
    return inv;
}

// [UPSTREAM constrained.cpp:66-104]
static inline void
LoadEdgesFromFile(igraph_t* g, const std::string& edgelist,
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

// [UPSTREAM constrained.h:71-97]
static inline std::map<int,int>
ReadCommunities(const std::map<std::string,int>& m, const std::string& path) {
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

// [UPSTREAM constrained.h:122-151] keep intra-cluster edges, drop inter.
static inline void
RemoveInterClusterEdges(igraph_t* g, const std::map<int,int>& m) {
    igraph_vector_int_t rm; igraph_vector_int_init(&rm, 0);
    igraph_eit_t eit; igraph_eit_create(g, igraph_ess_all(IGRAPH_EDGEORDER_ID), &eit);
    for (; !IGRAPH_EIT_END(eit); IGRAPH_EIT_NEXT(eit)) {
        igraph_integer_t e = IGRAPH_EIT_GET(eit);
        int from = IGRAPH_FROM(g, e), to = IGRAPH_TO(g, e);
        // Mirror upstream: empty `if` body when both endpoints share a cluster.
        if (m.contains(from) && m.contains(to) && m.at(from) == m.at(to)) {
            (void)0;
        } else {
            igraph_vector_int_push_back(&rm, e);
        }
    }
    igraph_es_t es; igraph_es_vector_copy(&es, &rm);
    igraph_delete_edges(g, es);
    igraph_eit_destroy(&eit); igraph_es_destroy(&es); igraph_vector_int_destroy(&rm);
}

// [UPSTREAM constrained.h:393-419] iterate node_id 0..vcount-1; bucket
// by component_id (std::map keeps ascending key order); skip singletons.
static inline std::vector<std::vector<int>>
GetConnectedComponents(igraph_t* g) {
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

// [UPSTREAM mincut_only.h:13-37]
static inline std::vector<std::vector<int>>
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

// JSON emit helper used by every tracer to serialise integer arrays.
template <class S>
static inline void emit_int_array(S& os, const std::vector<int>& v) {
    os << "[";
    for (size_t i = 0; i < v.size(); i++) {
        if (i) os << ",";
        os << v[i];
    }
    os << "]";
}

}  // namespace viz_check

#endif  // VIZ_CHECK_TRACER_IO_H
