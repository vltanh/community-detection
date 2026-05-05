// Louvain instrumented kernel cross-check, main entry (full multi-level).
//
// Mirrors externals/louvain/src/main_louvain.cpp's level loop:
//   srand(seed);
//   Graph g(file); init_quality(&g, 0); Louvain c(-1, prec, q);
//   do { c.one_level(); g = c.partition2graph_binary();
//        init_quality(&g, ++); c = Louvain(-1, prec, q); }
//   while(improvement);
//
// Captures every level's random_order + per-visit moves + n2c via the
// hooks in louvain_traced.cpp's tracing global. JSON shape:
//   {
//     "levels":[ {level, n, random_order, moves[], Q_before, Q_after,
//                 passes, n2c[]}, ... ],
//     "fine_membership":[...],   // per-original-node final membership
//     "renum_to_orig":[...],
//     "Q_final": ...
//   }
//
// Build: instrumented/build.sh -> /tmp/louvain_kernel_check
// Run:   /tmp/louvain_kernel_check <graph.bin> <seed> <relabel.txt>

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <unistd.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "graph_binary.h"
#include "louvain.h"
#include "modularity.h"

using namespace std;

// Forwards from louvain_traced.cpp.
struct LouvainTraceMove {
    int level; int pass; int visit; int node; int from_comm; int to_comm;
    long double dQ; bool moved;
};
struct LouvainTraceLevel {
    int level; int n_nodes;
    std::vector<int> random_order;
    std::vector<LouvainTraceMove> moves;
    long double quality_before; long double quality_after; int passes;
    std::vector<int> n2c;
};
extern const std::vector<LouvainTraceLevel>& louvain_trace_get();
extern void louvain_trace_set_level_n2c(int level, const int* n2c, int n);

int main(int argc, char** argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s <graph.bin> <seed> <relabel.txt>\n", argv[0]);
        return 2;
    }
    char* graph_bin = argv[1];
    int seed = std::atoi(argv[2]);
    char* relabel_path = argv[3];

    srand(seed);
    fprintf(stderr, "[TRACE-LV] PIPELINE_START graph=%s seed=%d\n", graph_bin, seed);

    long double precision = 1e-6L;

    // Outer Graph holds the current-level binary graph. Mirrors canonical's
    // outer `Graph g`. `q` is a Modularity quality function over `g`.
    Graph g(graph_bin, NULL, UNWEIGHTED);
    int n_orig = g.nb_nodes;
    Modularity* q = new Modularity(g);

    Louvain c(-1, precision, q);
    long double quality = q->quality();
    long double new_qual = quality;

    // Track fine-grained membership: original-node -> final community,
    // composed across levels. Initially identity.
    std::vector<int> fine(n_orig);
    for (int i = 0; i < n_orig; i++) fine[i] = i;

    bool improvement = true;
    int level = 0;
    while (improvement) {
        int n_before = q->size;
        long double q_before = q->quality();
        improvement = c.one_level();
        long double q_after = q->quality();

        // Save level n2c BEFORE renumber (partition2graph_binary will
        // renumber communities consecutively in-place via local copy,
        // but qual->n2c is unchanged).
        std::vector<int> n2c_lvl(q->size);
        for (int i = 0; i < q->size; i++) n2c_lvl[i] = q->n2c[i];

        // Compose into fine membership.
        // partition2graph_binary computes:
        //   renumber[c] = consecutive_idx(c)  (only for non-empty comms)
        // n2c_lvl[v] gives v's pre-renumber comm; renumber[n2c_lvl[v]]
        // gives v's new comm in next level. So we need that renumber map.
        std::vector<int> renumber(q->size, -1);
        for (int v = 0; v < q->size; v++) renumber[q->n2c[v]]++;
        int last = 0;
        for (int i = 0; i < q->size; i++)
            if (renumber[i] != -1) renumber[i] = last++;

        // For each original node, follow fine[v] (current level vertex it
        // collapsed to) -> q->n2c[fine[v]] -> renumber[...] = new vertex
        // id at next level.
        std::vector<int> new_fine(n_orig);
        for (int v = 0; v < n_orig; v++) {
            int curr = fine[v];
            int comm = q->n2c[curr];
            new_fine[v] = renumber[comm];
        }

        // Stash n2c into the trace level record (latest one).
        louvain_trace_set_level_n2c(level, n2c_lvl.data(), (int)n2c_lvl.size());

        new_qual = q_after;
        fine = new_fine;

        fprintf(stderr, "[TRACE-LV] LEVEL_END level=%d n=%d Q=%.6Lf->%.6Lf imp=%d\n",
                level, n_before, q_before, q_after, improvement ? 1 : 0);

        // Aggregate to next level. Canonical:
        //   g = c.partition2graph_binary();
        //   init_quality(&g, nb_calls);
        //   c = Louvain(-1, precision, q);
        // partition2graph_binary returns Graph by value -> copies into g.
        Graph g_next = c.partition2graph_binary();
        // Replace outer g + q + c.
        g = g_next;
        delete q;
        q = new Modularity(g);
        c = Louvain(-1, precision, q);
        quality = new_qual;
        level += 1;
        if (level > 100) break;
    }

    fprintf(stderr, "[TRACE-LV] PIPELINE_END levels=%d Q_final=%.6Lf\n", level, new_qual);

    // Read relabel map (renumbered_id -> original_string_id).
    std::vector<std::string> renum_to_orig(n_orig);
    {
        std::ifstream rf(relabel_path);
        std::string line;
        while (std::getline(rf, line)) {
            std::stringstream ss(line);
            std::string orig; int renum;
            ss >> orig >> renum;
            if (renum >= 0 && renum < n_orig) renum_to_orig[renum] = orig;
        }
    }

    // Emit JSON.
    auto& trace = louvain_trace_get();
    std::cout << std::setprecision(17) << "{\n";
    std::cout << "  \"levels\": [\n";
    for (size_t li = 0; li < trace.size(); li++) {
        const auto& lv = trace[li];
        if (li) std::cout << ",\n";
        std::cout << "    {\"level\":" << lv.level
                  << ",\"n\":" << lv.n_nodes
                  << ",\"Q_before\":" << (double)lv.quality_before
                  << ",\"Q_after\":" << (double)lv.quality_after
                  << ",\"passes\":" << lv.passes
                  << ",\"random_order\":[";
        for (size_t i = 0; i < lv.random_order.size(); i++) {
            if (i) std::cout << ",";
            std::cout << lv.random_order[i];
        }
        std::cout << "],\"moves\":[";
        for (size_t i = 0; i < lv.moves.size(); i++) {
            if (i) std::cout << ",";
            const auto& m = lv.moves[i];
            std::cout << "{\"pass\":" << m.pass
                      << ",\"visit\":" << m.visit
                      << ",\"node\":" << m.node
                      << ",\"from\":" << m.from_comm
                      << ",\"to\":" << m.to_comm
                      << ",\"dQ\":" << (double)m.dQ
                      << ",\"moved\":" << (m.moved ? "true" : "false") << "}";
        }
        std::cout << "],\"n2c\":[";
        for (size_t i = 0; i < lv.n2c.size(); i++) {
            if (i) std::cout << ",";
            std::cout << lv.n2c[i];
        }
        std::cout << "]}";
    }
    std::cout << "\n  ],\n";
    std::cout << "  \"fine_membership\": [";
    for (size_t i = 0; i < fine.size(); i++) {
        if (i) std::cout << ",";
        std::cout << fine[i];
    }
    std::cout << "],\n";
    std::cout << "  \"renum_to_orig\": [";
    for (size_t i = 0; i < renum_to_orig.size(); i++) {
        if (i) std::cout << ",";
        std::cout << "\"" << renum_to_orig[i] << "\"";
    }
    std::cout << "],\n";
    std::cout << "  \"Q_final\": " << (double)new_qual << "\n";
    std::cout << "}\n";
    std::cout.flush();
    fflush(stdout);

    // Skip cleanup; OS reclaims.
    _exit(0);
}
