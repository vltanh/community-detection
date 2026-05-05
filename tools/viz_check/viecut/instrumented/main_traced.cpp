/******************************************************************************
 * main_traced.cpp
 *
 * Instrumented entrypoint for byte-equal verification of the JS port. Reads
 * a METIS graph + seed, runs cactus_mincut::findAllMincuts with save_cut +
 * find_most_balanced (matching the canonical -b -s -o -t flag set), and
 * emits to stderr:
 *   [TRACE-RNG] one line per random_functions call (via the instrumented
 *               include/tools/random_functions.h that takes precedence in
 *               the include search path).
 *   [TRACE-PQ-NOI]    per-pop trace of modified_capforest's bucket queue.
 *   [TRACE-PQ-STC]    per-pop trace of recursive_cactus::findSTCactus's PQ.
 *   [TRACE-PQ-PR]     per-pop trace of push_relabel's m_Q.
 *   [TRACE-SET-AC]    iteration order of all_ctr unordered_set in
 *                     mergeCactusWithComponent.
 *   [TRACE-SET-EP]    iteration order of edge_positions in
 *                     contractGraphSparse.
 *   [TRACE-SET-CE]    iteration order of contractEdge map.
 * (The latter three are TODO; first cut emits RNG only.)
 *
 * stdout: JSON with cactus tree + bipartition + final mincut value.
 *
 * Run: ./mincut_traced <metis_path> <seed>
 *****************************************************************************/

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "algorithms/global_mincut/cactus/cactus_mincut.h"
#include "algorithms/global_mincut/minimum_cut.h"
#include "common/configuration.h"
#include "common/definitions.h"
#include "data_structure/graph_access.h"
#include "data_structure/mutable_graph.h"
#include "io/graph_io.h"
#include "tools/random_functions.h"
#include "tools/timer.h"

typedef mutable_graph graph_type;
typedef std::shared_ptr<graph_type> GraphPtr;

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <metis_path> <seed>\n", argv[0]);
        return 1;
    }
    auto cfg = configuration::getConfig();
    cfg->graph_filename = argv[1];
    cfg->seed = static_cast<size_t>(std::atoi(argv[2]));
    cfg->save_cut = true;
    cfg->find_most_balanced_cut = true;
    cfg->set_node_in_cut = true;
    cfg->find_lowest_conductance = false;

    random_functions::setSeed(cfg->seed);
    GraphPtr G = graph_io::readGraphWeighted<graph_type>(cfg->graph_filename);

    cactus_mincut<GraphPtr> mc;
    auto [mincut, cactus_graph, mb_edges] = mc.findAllMincuts(G);

    std::ostringstream out;
    out << "{\n";
    out << "  \"mincut\": " << mincut << ",\n";

    out << "  \"cactus\": {\n";
    out << "    \"n\": " << cactus_graph->number_of_nodes() << ",\n";
    out << "    \"nodes\": [";
    for (NodeID n : cactus_graph->nodes()) {
        if (n > 0) out << ", ";
        out << "{\"id\":" << n << ",\"contained\":[";
        auto cv = cactus_graph->containedVertices(n);
        for (size_t i = 0; i < cv.size(); i++) {
            if (i > 0) out << ",";
            out << cv[i];
        }
        out << "]}";
    }
    out << "],\n";
    out << "    \"edges\": [";
    bool first_edge = true;
    for (NodeID n : cactus_graph->nodes()) {
        for (EdgeID e : cactus_graph->edges_of(n)) {
            NodeID t = cactus_graph->getEdgeTarget(n, e);
            if (n > t) continue;
            if (!first_edge) out << ", ";
            first_edge = false;
            out << "{\"u\":" << n << ",\"v\":" << t
                << ",\"weight\":" << cactus_graph->getEdgeWeight(n, e) << "}";
        }
    }
    out << "],\n";
    out << "    \"adj\": [";
    for (NodeID n : cactus_graph->nodes()) {
        if (n > 0) out << ", ";
        out << "[";
        bool first_adj = true;
        for (EdgeID e : cactus_graph->edges_of(n)) {
            if (!first_adj) out << ",";
            first_adj = false;
            out << "{\"target\":" << cactus_graph->getEdgeTarget(n, e)
                << ",\"weight\":" << cactus_graph->getEdgeWeight(n, e)
                << ",\"rev\":" << cactus_graph->getReverseEdge(n, e) << "}";
        }
        out << "]";
    }
    out << "]\n";
    out << "  },\n";

    // Bipartition: per-original-node 0/1 from getNodeInCut on G.
    out << "  \"bipartition\": [";
    for (NodeID n : G->nodes()) {
        if (n > 0) out << ",";
        out << (G->getNodeInCut(n) ? 1 : 0);
    }
    out << "]\n";

    out << "}\n";
    std::cout << out.str();
    return 0;
}
