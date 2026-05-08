/******************************************************************************
 * most_balanced_minimum_cut.h
 *
 * Source of VieCut
 *
 ******************************************************************************
 * Copyright (C) 2019 Alexander Noe <alexander.noe@univie.ac.at>
 *
 * Published under the MIT license in the LICENSE file.
 *****************************************************************************/

#pragma once

#include <cstdio>
#include <memory>
#include <queue>
#include <unordered_set>
#include <utility>
#include <vector>

#include "algorithms/global_mincut/cactus/balanced_cut_dfs.h"
#include "common/configuration.h"
#include "data_structure/mutable_graph.h"
#include "io/graph_io.h"

template <class GraphPtr>
class most_balanced_minimum_cut {
 public:
    std::vector<std::pair<NodeID, EdgeID> > findCutFromCactus(
        mutableGraphPtr G, EdgeWeight mincut,
        GraphPtr original_graph) {
        if (!configuration::getConfig()->save_cut) {
            LOG1 << "Error: can't find most balanced minimum cut "
                 << "when save_cut is not set";
            exit(1);
        }

        if (configuration::getConfig()->output_path != "")
            configuration::getConfig()->set_node_in_cut = true;

        std::vector<std::pair<NodeID, EdgeID> > originalBestcutEdges;

        if (mincut == 0) {
            LOG1 << "G has multiple connected components and mincut is 0.";
            return originalBestcutEdges;
        }

        // Dump cactus adjacency before DFS (for tie-break bisection).
        std::fprintf(stderr, "[TRACE-MB] cactus_dump n:%u\n", (unsigned)G->n());
        for (NodeID n = 0; n < G->n(); n++) {
            std::fprintf(stderr, "[TRACE-MB]   node:%u contained:", (unsigned)n);
            for (NodeID v : G->containedVertices(n))
                std::fprintf(stderr, "%u,", (unsigned)v);
            std::fprintf(stderr, " adj:");
            for (EdgeID e = 0; e < G->get_first_invalid_edge(n); e++) {
                NodeID t = G->getEdgeTarget(n, e);
                EdgeWeight w = G->getEdgeWeight(n, e);
                std::fprintf(stderr, "(t=%u,w=%lld),", (unsigned)t, (long long)w);
            }
            std::fprintf(stderr, "\n");
        }

        balanced_cut_dfs dfs(original_graph, G, mincut);
        auto [n1, e1, n2, e2, bestcutInCycle] = dfs.runDFS();
        std::fprintf(stderr, "[TRACE-MB] dfs_out n1:%u e1:%u n2:%u e2:%u in_cycle:%d\n",
                     (unsigned)n1, (unsigned)e1, (unsigned)n2, (unsigned)e2,
                     (int)bestcutInCycle);

        if (!configuration::getConfig()->set_node_in_cut) {
            return std::vector<std::pair<NodeID, EdgeID> > { };
        }

        NodeID rev_n1 = G->getEdgeTarget(n1, e1);
        NodeID rev_n2 = G->getEdgeTarget(n2, e2);
        std::fprintf(stderr, "[TRACE-MB] revs rev_n1:%u rev_n2:%u\n",
                     (unsigned)rev_n1, (unsigned)rev_n2);

        for (NodeID n : original_graph->nodes()) {
            original_graph->setNodeInCut(n, false);
        }

        std::queue<NodeID> q;
        std::vector<bool> checked(G->n(), false);
        q.push(n1);
        checked[n1] = true;
        if (n1 != n2) {
            q.push(n2);
            checked[n2] = true;
        }
        // we want to set one side of most balanced cut to inCut
        // setting the targets of the edges to true makes sure the bfs doesn't
        // go into other side of most balanced cut
        checked[rev_n1] = true;
        checked[rev_n2] = true;

        while (!q.empty()) {
            NodeID top = q.front();
            q.pop();
            std::fprintf(stderr, "[TRACE-MB] bfs_visit cactus_node:%u contained:",
                         (unsigned)top);
            for (NodeID v : G->containedVertices(top)) {
                original_graph->setNodeInCut(v, true);
                std::fprintf(stderr, "%u,", (unsigned)v);
            }
            std::fprintf(stderr, "\n");
            for (EdgeID e : G->edges_of(top)) {
                NodeID t = G->getEdgeTarget(top, e);
                if (!checked[t]) {
                    q.push(t);
                    checked[t] = true;
                }
            }
        }

        for (NodeID on : original_graph->nodes()) {
            for (EdgeID oe : original_graph->edges_of(on)) {
                NodeID ot = original_graph->getEdgeTarget(on, oe);
                if (original_graph->getNodeInCut(on)
                    != original_graph->getNodeInCut(ot)) {
                    originalBestcutEdges.emplace_back(on, oe);
                }
            }
        }

        if (bestcutInCycle)
            LOG0 << "bestcut is in cycle";

        if (configuration::getConfig()->output_path != "") {
            LOG1 << "Printing output to file "
                 << configuration::getConfig()->output_path;

            graph_io::writeCut(original_graph,
                               configuration::getConfig()->output_path);
        }

        return originalBestcutEdges;
    }
};
