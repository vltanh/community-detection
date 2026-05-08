/******************************************************************************
 * cactus_mincut.h (instrumented)
 *
 * Sibling reimpl that wraps VieCut's cactus_mincut with [TRACE-CM] /
 * [TRACE-PR-FLOW] / [TRACE-SCC] / [TRACE-PQ-STC] / [TRACE-SET-*] stderr
 * emission at every nondeterminism source identified in the byte-equal
 * audit:
 *   - SCC comp_num assignment (Tarjan order).
 *   - findSTCactus PQ pop sequence + cactus-id assignment.
 *   - unordered_set iteration order at: contractGraphSparse edge_positions,
 *     mergeCactusWithComponent all_ctr, mutable_graph contractEdge map,
 *     mutable_graph contractVertexSet map.
 *
 * This header takes precedence over VieCut/lib/.../cactus_mincut.h via
 * -I instrumented/include in build.sh. Other sibling headers (contract_graph,
 * recursive_cactus, mutable_graph, scc) live under the same prefix and
 * each emit additional [TRACE-*] lines at their own use sites.
 *****************************************************************************/

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <memory>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "algorithms/global_mincut/cactus/most_balanced_minimum_cut.h"
#include "algorithms/global_mincut/cactus/recursive_cactus.h"
#include "algorithms/global_mincut/minimum_cut.h"
#include "algorithms/global_mincut/noi_minimum_cut.h"
#include "algorithms/global_mincut/viecut.h"
#include "common/definitions.h"
#include "data_structure/graph_access.h"
#include "data_structure/mutable_graph.h"
#include "data_structure/priority_queues/maxNodeHeap.h"
#include "io/graph_io.h"
#include "tools/string.h"
#include "tools/timer.h"

#ifdef PARALLEL
#include "parallel/coarsening/contract_graph.h"
#else
#include "coarsening/contract_graph.h"
#endif

template <class GraphPtr>
class cactus_mincut : public minimum_cut {
 public:
    typedef GraphPtr GraphPtrType;
    cactus_mincut() { }
    virtual ~cactus_mincut() { }
    static constexpr bool debug = false;

    bool timing = configuration::getConfig()->verbose;

    EdgeWeight perform_minimum_cut(GraphPtr G) {
        return std::get<0>(findAllMincuts(G));
    }

    std::tuple<EdgeWeight, mutableGraphPtr,
               std::vector<std::pair<NodeID, EdgeID> > >
    findAllMincuts(GraphPtr G, EdgeWeight known_mincut = UNDEFINED_NODE) {
        std::vector<GraphPtr> v = { G };
        return findAllMincuts(v, known_mincut);
    }

    std::tuple<EdgeWeight, mutableGraphPtr,
               std::vector<std::pair<NodeID, EdgeID> > >
    findAllMincuts(std::vector<GraphPtr> graphs,
                   EdgeWeight known_mincut = UNDEFINED_NODE) {
        if (graphs.size() == 0 || !graphs.back()) {
            mutableGraphPtr empty;
            return std::make_tuple(
                -1, empty, std::vector<std::pair<NodeID, EdgeID> > { });
        }
        recursive_cactus<GraphPtr> rc;
        EdgeWeight mincut = graphs.back()->getMinDegree();
        timer t;
        if (known_mincut == UNDEFINED_NODE) {
            viecut<GraphPtr> vc;
            mincut = vc.perform_minimum_cut(graphs.back());
        } else {
            mincut = known_mincut;
        }
        std::fprintf(stderr, "[TRACE-CM] viecut_mincut_bound mincut:%lu\n",
                     (unsigned long)mincut);
        noi_minimum_cut<GraphPtr> noi;
        std::vector<std::vector<std::pair<NodeID, NodeID> > > guaranteed_edges;
        std::vector<size_t> ge_ids;

        minimum_cut_helpers<GraphPtr>::setInitialCutValues(graphs);

        NodeID previous_size = UNDEFINED_NODE;
        int loop_iter = 0;
        while (graphs.back()->number_of_nodes() * 1.01 < previous_size) {
            previous_size = graphs.back()->number_of_nodes();
            auto current_graph = graphs.back();
            EdgeWeight current_mincut = mincut;
            ge_ids.emplace_back(graphs.size() - 1);
            guaranteed_edges.emplace_back();
            std::fprintf(stderr,
                "[TRACE-CM] loop_iter:%d n:%u m:%lu mincut:%lu\n",
                loop_iter, current_graph->number_of_nodes(),
                (unsigned long)current_graph->number_of_edges(),
                (unsigned long)mincut);

            std::vector<std::pair<NodeID, NodeID> > contractable;

            timer time;
            auto uf = noi.modified_capforest(current_graph, mincut + 1);

            for (NodeID n : current_graph->nodes()) {
                EdgeID e = current_graph->get_first_edge(n);
                if (current_graph->get_first_invalid_edge(n) - e == 1) {
                    if ((current_graph->getEdgeWeight(n, e) == mincut)
                        && uf.n() > 1) {
                        NodeID t = current_graph->getEdgeTarget(n, e);
                        uf.Union(n, t);
                        guaranteed_edges.back().emplace_back(n, t);
                    }
                }
            }

            std::fprintf(stderr,
                "[TRACE-CM] capforest iter:%d before_n:%u uf_n:%u\n",
                loop_iter, current_graph->number_of_nodes(), uf.n());
            if (uf.n() < current_graph->number_of_nodes()) {
                auto newg =
                    contraction::fromUnionFind(current_graph, &uf, true);
                graphs.emplace_back(newg);
                std::fprintf(stderr,
                    "[TRACE-CM] capforest_after iter:%d after_n:%u\n",
                    loop_iter, graphs.back()->n());
                for (NodeID gn : graphs.back()->nodes()) {
                    std::fprintf(stderr, "[TRACE-CM]  cap_contained[%u]:",
                                 gn);
                    for (NodeID cv : graphs.back()->containedVertices(gn))
                        std::fprintf(stderr, "%u,", cv);
                    std::fprintf(stderr, "\n");
                }
                mincut = minimum_cut_helpers<GraphPtr>::updateCut(
                    graphs, mincut);
            }

            union_find uf12 = tests::prTests12(
                graphs.back(), mincut + 1, true);
            std::fprintf(stderr,
                "[TRACE-CM] pr12 iter:%d before_n:%u uf_n:%u\n",
                loop_iter, graphs.back()->n(), uf12.n());
            if (uf12.n() < graphs.back()->number_of_nodes()) {
                auto g12 = contraction::fromUnionFind(
                    graphs.back(), &uf12, true);
                graphs.push_back(g12);
                std::fprintf(stderr,
                    "[TRACE-CM] pr12_after iter:%d after_n:%u\n",
                    loop_iter, graphs.back()->n());
                for (NodeID gn : graphs.back()->nodes()) {
                    std::fprintf(stderr, "[TRACE-CM]  pr12_contained[%u]:", gn);
                    for (NodeID cv : graphs.back()->containedVertices(gn))
                        std::fprintf(stderr, "%u,", cv);
                    std::fprintf(stderr, "\n");
                }
                mincut = minimum_cut_helpers<GraphPtr>::updateCut(
                    graphs, mincut);
            }

            // PROBE: dump n=12 graph adjacency before pr34 iter:3 (and others).
            {
                auto Gp = graphs.back();
                std::fprintf(stderr, "[TRACE-CM] pr34_input iter:%d n:%u m:%lld\n",
                    loop_iter, Gp->n(), (long long)Gp->m());
                for (NodeID v : Gp->nodes()) {
                    std::fprintf(stderr, "[TRACE-CM]  pr34_adj[%u] deg:%lu wdeg:%lu",
                        v, (unsigned long)Gp->getUnweightedNodeDegree(v),
                        (unsigned long)Gp->getWeightedNodeDegree(v));
                    for (EdgeID e : Gp->edges_of(v)) {
                        auto [t, w] = Gp->getEdge(v, e);
                        std::fprintf(stderr, " (%u,%lu)", t, (unsigned long)w);
                    }
                    std::fprintf(stderr, "\n");
                }
            }
            union_find uf34 = tests::prTests34(
                graphs.back(), mincut + 1, true);
            std::fprintf(stderr,
                "[TRACE-CM] pr34 iter:%d before_n:%u uf_n:%u\n",
                loop_iter, graphs.back()->n(), uf34.n());
            for (NodeID i = 0; i < uf34.n(); i++) {}
            {
                std::fprintf(stderr, "[TRACE-CM] pr34_uf iter:%d members", loop_iter);
                for (NodeID v = 0; v < graphs.back()->n(); v++) {
                    std::fprintf(stderr, " %u->%u", v, uf34.Find(v));
                }
                std::fprintf(stderr, "\n");
            }
            if (uf34.n() < graphs.back()->number_of_nodes()) {
                auto g34 = contraction::fromUnionFind(
                    graphs.back(), &uf34, true);
                graphs.push_back(g34);
                std::fprintf(stderr,
                    "[TRACE-CM] pr34_after iter:%d after_n:%u\n",
                    loop_iter, graphs.back()->n());
                for (NodeID gn : graphs.back()->nodes()) {
                    std::fprintf(stderr, "[TRACE-CM]  pr34_contained[%u]:", gn);
                    for (NodeID cv : graphs.back()->containedVertices(gn))
                        std::fprintf(stderr, "%u,", cv);
                    std::fprintf(stderr, "\n");
                }
                mincut = minimum_cut_helpers<GraphPtr>::updateCut(
                    graphs, mincut);
            }

            if (current_mincut > mincut) {
                guaranteed_edges.clear();
                ge_ids.clear();
            }
            loop_iter++;
        }

        if (graphs.back()->number_of_nodes() > 1)
            mincut = noi.perform_minimum_cut(graphs.back());

        rc.setMincut(mincut);
        std::fprintf(stderr, "[TRACE-CM] before_flowmincut n:%u m:%lu mincut:%lu\n",
                     graphs.back()->number_of_nodes(),
                     (unsigned long)graphs.back()->number_of_edges(),
                     (unsigned long)mincut);
        auto out_graph = rc.flowMincut(graphs);

        // Dump cactus tree post-flowMincut, pre-setVertexLocations.
        std::fprintf(stderr,
            "[TRACE-CM] flowmincut_done cactus_n:%u cactus_m:%lu\n",
            out_graph->n(), (unsigned long)out_graph->m());

        minimum_cut_helpers<GraphPtr>::setVertexLocations(
            out_graph, graphs, ge_ids, guaranteed_edges, mincut);

        std::vector<std::pair<NodeID, EdgeID> > mb_edges;
        if (configuration::getConfig()->find_most_balanced_cut) {
            most_balanced_minimum_cut<GraphPtr> mbmc;
            mb_edges = mbmc.findCutFromCactus(out_graph, mincut, graphs[0]);
        }

        if (configuration::getConfig()->cactus_filename != "") {
            // omitted: GraphML emission (not used by tracer)
        }

        return std::make_tuple(mincut, out_graph, mb_edges);
    }
};
