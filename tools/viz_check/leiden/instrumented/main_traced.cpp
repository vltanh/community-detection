// Leiden tracer main entry. Builds CPMVertexPartition (or Modularity)
// over a graph, runs the FORKED Optimiser::optimise_partition (which
// calls our traced move_nodes) once for 2 iterations, dumps JSON
// trace including every shuffle's random_order + every per-visit
// move record.
//
// Build: ./build.sh -> /tmp/leiden_kernel_check
// Run:   /tmp/leiden_kernel_check <edge.csv> <out.csv> <cpm|mod> <param> <seed>

#include "mincut_custom.h"  // pull VieCut deps; same link surface as cm/wcc.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <set>
#include <string>
#include <vector>
#include <unistd.h>
#include <igraph.h>
#include <libleidenalg/GraphHelper.h>
#include <libleidenalg/CPMVertexPartition.h>
#include <libleidenalg/ModularityVertexPartition.h>
#include <libleidenalg/Optimiser.h>
#include "tracer_io.h"

using namespace viz_check;

// Forwards from optimiser_traced.cpp.
struct LeidenTraceMove {
    size_t pass; size_t visit_idx;
    size_t v; size_t from_comm; size_t to_comm;
    double dQ; bool moved;
};
struct LeidenTracePass {
    size_t pass; size_t n_nodes_in_queue;
    int phase;                                 // 0 = move_nodes, 1 = merge_nodes_constrained
    size_t level;                              // graph vcount at this pass
    std::vector<size_t> shuffled_nodes;
    std::vector<size_t> pre_membership;
    std::vector<LeidenTraceMove> moves;
    double total_improv;
    size_t nb_moves;
    std::vector<size_t> post_membership;
};
struct LeidenTrace {
    std::vector<LeidenTracePass> passes;
    double Q_init; double Q_final;
    size_t n_communities_final;
};
extern const LeidenTrace& leiden_trace_get();
extern void leiden_trace_reset();

int main(int argc, char** argv) {
    if (argc < 6) {
        std::fprintf(stderr,
                     "usage: %s <edge.csv> <out.csv> <cpm|mod> <param> <seed> [iters=2]\n",
                     argv[0]);
        return 2;
    }
    std::string edge_csv = argv[1];
    std::string out_csv = argv[2];
    std::string quality = argv[3];
    double param = std::stod(argv[4]);
    int seed = std::atoi(argv[5]);
    int iters = (argc > 6) ? std::atoi(argv[6]) : 2;

    fprintf(stderr, "[TRACE-LD] PIPELINE_START edge=%s quality=%s param=%.6f seed=%d\n",
            edge_csv.c_str(), quality.c_str(), param, seed);

    auto orig_to_new = GetOriginalToNewIdMap(edge_csv);
    auto new_to_orig = InvertMap(orig_to_new);
    igraph_t graph;
    igraph_empty(&graph, 0, IGRAPH_UNDIRECTED);
    LoadEdgesFromFile(&graph, edge_csv, orig_to_new);
    fprintf(stderr, "[TRACE-LD] loaded n=%lld m=%lld\n",
            (long long)igraph_vcount(&graph), (long long)igraph_ecount(&graph));

    Graph leiden_graph(&graph);
    MutableVertexPartition* partition = nullptr;
    if (quality == "cpm")      partition = new CPMVertexPartition(&leiden_graph, param);
    else if (quality == "mod") partition = new ModularityVertexPartition(&leiden_graph);
    else { fprintf(stderr, "unknown quality '%s'\n", quality.c_str()); return 2; }

    leiden_trace_reset();
    Optimiser o;
    o.set_rng_seed(seed);
    double Q_init = partition->quality();
    for (int i = 0; i < iters; i++) o.optimise_partition(partition);
    double Q_final = partition->quality();

    fprintf(stderr, "[TRACE-LD] PIPELINE_END Q %.6f -> %.6f comms=%zu passes=%zu\n",
            Q_init, Q_final, partition->n_communities(),
            leiden_trace_get().passes.size());

    // Write CSV partition.
    {
        std::ofstream out(out_csv);
        out << "node_id,cluster_id\n";
        for (size_t v = 0; v < (size_t)igraph_vcount(&graph); v++) {
            out << new_to_orig.at(v) << "," << partition->membership(v) << "\n";
        }
    }

    // Emit JSON trace.
    const auto& trace = leiden_trace_get();
    std::cout << std::setprecision(17) << "{\n";
    std::cout << "  \"Q_init\": " << Q_init << ",\n";
    std::cout << "  \"Q_final\": " << Q_final << ",\n";
    std::cout << "  \"n_communities\": " << partition->n_communities() << ",\n";
    std::cout << "  \"membership\": [";
    for (size_t v = 0; v < (size_t)igraph_vcount(&graph); v++) {
        if (v) std::cout << ",";
        std::cout << partition->membership(v);
    }
    std::cout << "],\n";
    std::cout << "  \"renum_to_orig\": [";
    for (size_t v = 0; v < (size_t)igraph_vcount(&graph); v++) {
        if (v) std::cout << ",";
        std::cout << "\"" << new_to_orig.at(v) << "\"";
    }
    std::cout << "],\n";
    std::cout << "  \"passes\": [\n";
    for (size_t pi = 0; pi < trace.passes.size(); pi++) {
        if (pi) std::cout << ",\n";
        const auto& p = trace.passes[pi];
        std::cout << "    {\"pass\":" << p.pass
                  << ",\"phase\":\"" << (p.phase == 0 ? "move" : "refine") << "\""
                  << ",\"level\":" << p.level
                  << ",\"queue\":" << p.n_nodes_in_queue
                  << ",\"nb_moves\":" << p.nb_moves
                  << ",\"total_improv\":" << p.total_improv
                  << ",\"shuffled_nodes\":[";
        for (size_t k = 0; k < p.shuffled_nodes.size(); k++) {
            if (k) std::cout << ",";
            std::cout << p.shuffled_nodes[k];
        }
        std::cout << "],\"pre_membership\":[";
        for (size_t k = 0; k < p.pre_membership.size(); k++) {
            if (k) std::cout << ",";
            std::cout << p.pre_membership[k];
        }
        std::cout << "],\"moves\":[";
        for (size_t k = 0; k < p.moves.size(); k++) {
            if (k) std::cout << ",";
            const auto& m = p.moves[k];
            std::cout << "{\"v\":" << m.v
                      << ",\"from\":" << m.from_comm
                      << ",\"to\":" << m.to_comm
                      << ",\"dQ\":" << m.dQ
                      << ",\"moved\":" << (m.moved ? "true" : "false") << "}";
        }
        std::cout << "],\"post_membership\":[";
        for (size_t k = 0; k < p.post_membership.size(); k++) {
            if (k) std::cout << ",";
            std::cout << p.post_membership[k];
        }
        std::cout << "]}";
    }
    std::cout << "\n  ]\n}\n";
    std::cout.flush();
    fflush(stdout);

    delete partition;
    igraph_destroy(&graph);
    _exit(0);
}
