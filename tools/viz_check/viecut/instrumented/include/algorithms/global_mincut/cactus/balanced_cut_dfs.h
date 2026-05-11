/******************************************************************************
 * balanced_cut_dfs.h
 *
 * Source of VieCut
 *
 ******************************************************************************
 * Copyright (C) 2019 Alexander Noe <alexander.noe@univie.ac.at>
 *
 * Published under the MIT license in the LICENSE file.
 *****************************************************************************/

#pragma once

#include <algorithm>
#include <memory>
#include <tuple>
#include <vector>

#include "common/configuration.h"
#include "common/definitions.h"
#include "data_structure/mutable_graph.h"
#include "tools/random_functions.h"

template <class GraphPtr>
class balanced_cut_dfs {
 public:
    static constexpr bool debug = false;

    balanced_cut_dfs() = delete;
    balanced_cut_dfs(GraphPtr original_graph,
                     mutableGraphPtr G, EdgeWeight mincut)
        : original_graph(original_graph),
          G(G),
          mincut(mincut),
          status(G->n(), UNDISCOVERED),
          subtree_weight(G->n(), UNDEFINED_EDGE),
          parent(G->n(), UNDEFINED_NODE),
          outgoing_cycles(G->n()) { }

    std::tuple<NodeID, EdgeID, NodeID, EdgeID, bool> runDFS() {
        start_vertex = random_functions::nextInt(0, G->n() - 1);
        std::fprintf(stderr, "[TRACE-BCD] runDFS_start cactus_n:%u start_vertex:%u\n",
                     (unsigned)G->n(), (unsigned)start_vertex);
        optimize_conductance =
            configuration::getConfig()->find_lowest_conductance;
        if (optimize_conductance) {
            sum_of_weights = original_graph->sumOfEdgeWeights();
        }
        processVertex(start_vertex);
        std::fprintf(stderr, "[TRACE-BCD] runDFS_end best_n:%u best_e:%u best_n2:%u best_e2:%u in_cycle:%d best_w:%lld\n",
                     (unsigned)best_n, (unsigned)best_e,
                     (unsigned)best_n2, (unsigned)best_e2,
                     (int)best_in_cycle, (long long)best_weight);

        // Note: Min commented it out
        /* LOG1 << "Most balanced cut has weight " */
        /*      << best_weight << " on the lighter side and " */
        /*      << totalWeight() - best_weight << " on the heavier side"; */

        // Return cut edge(s) for most balanced mincut.
        // If most balanced mincut is on a cycle, we return both edges.
        // If it is a tree edge, we return it twice (simpler handling outside)
        LOG << "n " << best_n << " with edge " << best_e
            << " to " << G->getEdgeTarget(best_n, best_e);
        if (best_in_cycle) {
            LOG << "n " << best_n2 << " with edge " << best_e2
                << " to " << G->getEdgeTarget(best_n2, best_e2);
            NodeID best_n2_r = G->getEdgeTarget(best_n2, best_e2);
            EdgeID best_e2_r = G->getReverseEdge(best_n2, best_e2);
            return std::make_tuple(best_n, best_e, best_n2_r, best_e2_r, true);
        } else {
            return std::make_tuple(best_n, best_e, best_n, best_e, false);
        }
    }

 private:
    void processVertex(NodeID n) {
        status[n] = ACTIVE;

        EdgeID fie = G->get_first_invalid_edge(n);
        bool leaf = (fie == 1 && n != start_vertex);
        EdgeWeight vw_leaf = leaf ? findVertexWeight(n) : 0;
        std::fprintf(stderr,
            "[TRACE-BCD-P] enter n:%u fie:%lu start:%u leaf:%d "
            "vw_leaf:%lu\n",
            n, (unsigned long)fie, start_vertex,
            (int)leaf, (unsigned long)vw_leaf);
        if (leaf) {
            // leaf
            subtree_weight[n] = vw_leaf;
            status[n] = FINISHED;
            std::fprintf(stderr,
                "[TRACE-BCD-P] leaf_exit n:%u subtree_weight:%lu\n",
                n, (unsigned long)subtree_weight[n]);
            return;
        }

        EdgeWeight children_weight = 0;
        for (EdgeID e : G->edges_of(n)) {
            NodeID t = G->getEdgeTarget(n, e);
            std::fprintf(stderr,
                "[TRACE-BCD-P] edge n:%u e:%lu t:%u status_t:%d\n",
                n, (unsigned long)e, t, (int)status[t]);
            switch (status[t]) {
            case UNDISCOVERED: {
                parent[t] = n;
                processVertex(t);
                EdgeWeight lb = lighterBlock(subtree_weight[t]);
                EdgeWeight w = G->getEdgeWeight(n, e);
                bool t20 = (lb > best_weight && w == mincut);
                std::fprintf(stderr,
                    "[TRACE-BCD-P] und_post n:%u e:%lu t:%u "
                    "subtree_t:%lu lb:%lu best_w:%lu w:%lu mc:%lu "
                    "t20_update:%d\n",
                    n, (unsigned long)e, t, (unsigned long)subtree_weight[t],
                    (unsigned long)lb, (unsigned long)best_weight,
                    (unsigned long)w, (unsigned long)mincut,
                    (int)t20);
                if (t20) {
                    best_in_cycle = false;
                    best_n = n;
                    best_e = e;
                    best_weight = lb;
                }
                EdgeWeight cw_before = children_weight;
                children_weight += subtree_weight[t];
                std::fprintf(stderr,
                    "[TRACE-BCD-P] cw_acc n:%u t:%u cw_before:%lu "
                    "subtree_t:%lu cw_after:%lu\n",
                    n, t, (unsigned long)cw_before,
                    (unsigned long)subtree_weight[t],
                    (unsigned long)children_weight);
                break;
            }

            case ACTIVE:
            case CYCLE:
                if (parent[n] != t) {
                    // found a cycle, mark it, process it when we are back at t
                    status[t] = CYCLE;
                    outgoing_cycles[t].emplace_back(n);
                    std::fprintf(stderr,
                        "[TRACE-BCD-P] cycle_mark n:%u t:%u "
                        "oc_size:%zu\n",
                        n, t, outgoing_cycles[t].size());
                }
                break;
            default:
                break;
                // do nothing
            }
        }

        if (status[n] == CYCLE) {
            checkForCycles(n);
        }

        EdgeWeight vw = findVertexWeight(n);
        subtree_weight[n] = children_weight + vw;
        std::fprintf(stderr,
            "[TRACE-BCD-P] exit n:%u cw:%lu vw:%lu subtree_weight:%lu\n",
            n, (unsigned long)children_weight, (unsigned long)vw,
            (unsigned long)subtree_weight[n]);

        status[n] = FINISHED;
    }

    EdgeWeight findVertexWeight(NodeID n) {
        EdgeWeight result;
        if (optimize_conductance) {
            EdgeWeight vertex_weight = 0;
            for (const auto& v : G->containedVertices(n)) {
                vertex_weight += original_graph->getWeightedNodeDegree(v);
            }
            result = vertex_weight;
        } else {
            result = G->containedVertices(n).size();
        }
        std::fprintf(stderr,
            "[TRACE-BCD-VW] n:%u oc:%d w:%lu\n",
            n, (int)optimize_conductance, (unsigned long)result);
        return result;
    }

    EdgeWeight totalWeight() {
        if (optimize_conductance) {
            return sum_of_weights;
        } else {
            return G->getOriginalNodes();
        }
    }

    EdgeWeight lighterBlock(EdgeWeight w) {
        EdgeWeight tw = totalWeight();
        EdgeWeight result = std::min(w, tw - w);
        std::fprintf(stderr,
            "[TRACE-BCD-LB] w:%lu tw:%lu tw_minus_w:%ld lighter:%lu\n",
            (unsigned long)w, (unsigned long)tw, (long)(tw - w),
            (unsigned long)result);
        return result;
    }

    void investigateCycle(NodeID t, NodeID start) {
        std::fprintf(stderr,
            "[TRACE-BCD-IC] enter t:%u start:%u\n", t, start);
        std::vector<NodeID> cycle_vertices;
        std::vector<EdgeWeight> cycle_weight;
        cycle_vertices.emplace_back(t);
        cycle_weight.emplace_back(subtree_weight[t]);

        while (cycle_vertices.back() != start) {
            NodeID par = parent[cycle_vertices.back()];
            EdgeWeight delta = subtree_weight[par]
                               - subtree_weight[cycle_vertices.back()];
            std::fprintf(stderr,
                "[TRACE-BCD-IC] walk par:%u cv_back:%u "
                "subtree_par:%lu subtree_back:%lu delta:%ld\n",
                par, cycle_vertices.back(),
                (unsigned long)subtree_weight[par],
                (unsigned long)subtree_weight[cycle_vertices.back()],
                (long)delta);
            cycle_weight.emplace_back(delta);
            cycle_vertices.emplace_back(par);
        }

        EdgeWeight cw_final = totalWeight()
                              - subtree_weight[cycle_vertices[
                                                   cycle_vertices.size() - 2]];
        cycle_weight.back() = cw_final;
        std::fprintf(stderr,
            "[TRACE-BCD-IC] cycle_built length:%zu cw_back_final:%ld\n",
            cycle_weight.size(), (long)cw_final);

        NodeID length = cycle_weight.size();

        NodeID back = length;
        NodeID front = 1;
        EdgeWeight in_weight = cycle_weight[0];

        size_t sw_step = 0;
        while (back <= 2 * length) {
            EdgeWeight lb = lighterBlock(in_weight);
            bool t21 = (lb >= best_weight);
            std::fprintf(stderr,
                "[TRACE-BCD-IC] sw step:%zu back:%u front:%u "
                "in_w:%lu lb:%lu best_w:%lu t21:%d\n",
                sw_step, back, front, (unsigned long)in_weight,
                (unsigned long)lb, (unsigned long)best_weight,
                (int)t21);
            if (t21) {
                best_weight = lb;
                best_in_cycle = true;

                // front edge
                best_n = cycle_vertices[(front - 1) % length];
                for (EdgeID e : G->edges_of(best_n)) {
                    if (G->getEdgeTarget(best_n, e)
                        == cycle_vertices[front % length]) {
                        best_e = e;
                        break;
                    }
                }

                // back edge
                best_n2 = cycle_vertices[(back - 1) % length];
                for (EdgeID e : G->edges_of(best_n2)) {
                    if (G->getEdgeTarget(best_n2, e)
                        == cycle_vertices[back % length]) {
                        best_e2 = e;
                        break;
                    }
                }
                std::fprintf(stderr,
                    "[TRACE-BCD-IC] best_update best_n:%u best_e:%u "
                    "best_n2:%u best_e2:%u best_w:%lu\n",
                    best_n, best_e, best_n2, best_e2,
                    (unsigned long)best_weight);
            }

            EdgeWeight tw = totalWeight();
            EdgeWeight inw2 = in_weight * 2;
            bool t22 = (inw2 >= tw);
            EdgeWeight in_before = in_weight;
            if (t22) {
                in_weight -= cycle_weight[back % length];
                back++;
            } else {
                in_weight += cycle_weight[front % length];
                front++;
            }
            std::fprintf(stderr,
                "[TRACE-BCD-IC] sw_dir step:%zu inw2:%ld tw:%lu t22:%d "
                "in_before:%lu in_after:%lu back:%u front:%u\n",
                sw_step, (long)inw2, (unsigned long)tw, (int)t22,
                (unsigned long)in_before, (unsigned long)in_weight,
                back, front);
            sw_step++;
        }
        std::fprintf(stderr,
            "[TRACE-BCD-IC] exit best_n:%u best_e:%u best_n2:%u "
            "best_e2:%u best_w:%lu\n",
            best_n, best_e, best_n2, best_e2,
            (unsigned long)best_weight);
    }

    void checkForCycles(NodeID n) {
        for (NodeID t : outgoing_cycles[n]) {
            investigateCycle(t, n);
        }
    }

    GraphPtr original_graph;
    mutableGraphPtr G;
    EdgeWeight mincut;
    NodeID start_vertex;
    std::vector<DFSVertexStatus> status;
    std::vector<EdgeWeight> subtree_weight;
    std::vector<NodeID> parent;
    std::vector<std::vector<NodeID> > outgoing_cycles;
    EdgeWeight sum_of_weights = 0;

    bool best_in_cycle = false;
    bool optimize_conductance = false;
    NodeID best_n = 0;
    EdgeID best_e = 0;

    NodeID best_n2 = 0;
    EdgeID best_e2 = 0;

    EdgeWeight best_weight = 0;
};
