// Louvain instrumented kernel cross-check, main entry.
//
// Runs the Louvain modularity loop on a binary graph file (output of
// gen-louvain's `convert` utility) and emits a JSON trace of every
// per-level random_order + per-visit move to stdout.
//
// Build: ../instrumented/build.sh -> /tmp/louvain_kernel_check
// Run:   /tmp/louvain_kernel_check <graph.bin> <seed> <relabel.txt>
//
// stdout: { levels: [{level, n, random_order, moves: [...], Q_before,
//                     Q_after, passes}, ...],
//           final_membership: [...],   // per (relabeled) node id
//           Q_final: ... }
// stderr: [TRACE-LV ...] log lines.

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <unistd.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
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
};
extern const std::vector<LouvainTraceLevel>& louvain_trace_get();

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

    // Scope: trace ONLY the level-0 sweep. Multi-level aggregation is
    // a deterministic chain of partition2graph_binary + recurse; level
    // 0 is where the RNG-driven random_order is consumed and the JS
    // replay needs that exact ordering. After level 0 the partition is
    // captured + emitted. Multi-level reproduction is a follow-up.
    Graph g(graph_bin, NULL, UNWEIGHTED);
    Modularity m_q(g);
    Louvain c(-1, 1e-6L, &m_q);

    int n_orig = g.nb_nodes;
    long double q0 = m_q.quality();

    c.one_level();
    long double new_qual = m_q.quality();

    std::vector<int> level0_n2c(c.qual->size);
    for (int i = 0; i < c.qual->size; i++) level0_n2c[i] = c.qual->n2c[i];


    fprintf(stderr, "[TRACE-LV] PIPELINE_END levels=1 Q0=%.6Lf Q1=%.6Lf\n", q0, new_qual);

    // Read relabel map (renumbered_id -> original_string_id) so JSON
    // can include the original node ids.
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
        std::cout << "]}";
    }
    std::cout << "\n  ],\n";
    std::cout << "  \"final_membership\": [";
    for (size_t i = 0; i < level0_n2c.size(); i++) {
        if (i) std::cout << ",";
        std::cout << level0_n2c[i];
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

    // NB: skipping `delete q` and other cleanup to avoid double-free
    // chasing through Graph reference juggling. This is a one-shot
    // tool; OS reclaims on exit.
    _exit(0);
}
