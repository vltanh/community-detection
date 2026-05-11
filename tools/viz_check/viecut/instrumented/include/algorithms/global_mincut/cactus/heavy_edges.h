/******************************************************************************
 * heavy_edges.h
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
#include <cstdio>
#include <memory>
#include <tuple>
#include <utility>
#include <vector>

#include "tools/container_swap.h"
#include "common/definitions.h"
#include "data_structure/mutable_graph.h"

class heavy_edges {
 public:
    explicit heavy_edges(EdgeWeight mincut) : mincut(mincut) { }

    std::vector<std::tuple<NodeID, std::vector<NodeID> > > removeHeavyEdges(
        mutableGraphPtr G) {
        // [TRACE-HE] removeHeavyEdges entry — graph state BEFORE in-place
        // mutation. Called at every recursiveCactus depth; tracer's prior
        // [TRACE-RC] probes only see G AFTER heavy_edges mutated it.
        std::fprintf(stderr,
            "[TRACE-HE] removeHeavyEdges entry n_before:%u m_before:%lu "
            "mincut:%lu\n",
            G->n(), (unsigned long)G->m(), (unsigned long)mincut);
        std::vector<std::tuple<NodeID, std::vector<NodeID> > > cactusEdge;
        TracerMap<NodeID, std::vector<NodeID> > contract;
        std::vector<NodeID> markForCactus;
        for (NodeID n : G->nodes()) {
            if (G->isEmpty(n))
                continue;

            for (EdgeID e : G->edges_of(n)) {
                EdgeWeight wgt = G->getEdgeWeight(n, e);
                NodeID target = G->getEdgeTarget(n, e);

                if (G->isEmpty(target))
                    continue;

                // T23: wgt > mincut strict; T24: wgt == mincut.
                bool t23_gt = (wgt > mincut);
                bool t24_eq = (wgt == mincut);
                if (t23_gt) {
                    NodeID v1 = G->containedVertices(n)[0];
                    NodeID v2 = G->containedVertices(target)[0];
                    NodeID min = std::min(v1, v2);
                    NodeID max = std::max(v1, v2);

                    bool insertion = (contract.find(min) == contract.end());
                    if (insertion) {
                        contract[min] = { max };
                    } else {
                        contract[min].emplace_back(max);
                    }
                    std::fprintf(stderr,
                        "[TRACE-HE-EDGE] n:%u e:%lu wgt:%lu target:%u "
                        "branch:gt v1:%u v2:%u min:%u max:%u insertion:%d "
                        "contract_size:%zu\n",
                        n, (unsigned long)e, (unsigned long)wgt, target,
                        v1, v2, min, max, (int)insertion, contract.size());
                }

                if (t24_eq) {
                    EdgeID first_inv = G->get_first_invalid_edge(n);
                    bool deg1 = (first_inv == 1);
                    if (deg1) {
                        // each edge is seen from both adjacent nodes
                        // so we get all edges
                        NodeID v0 = G->containedVertices(n)[0];
                        markForCactus.emplace_back(v0);
                        std::fprintf(stderr,
                            "[TRACE-HE-EDGE] n:%u e:%lu wgt:%lu target:%u "
                            "branch:eq deg1:1 first_inv:%lu v0:%u "
                            "mark_size:%zu\n",
                            n, (unsigned long)e, (unsigned long)wgt, target,
                            (unsigned long)first_inv, v0, markForCactus.size());
                    } else {
                        std::fprintf(stderr,
                            "[TRACE-HE-EDGE] n:%u e:%lu wgt:%lu target:%u "
                            "branch:eq deg1:0 first_inv:%lu\n",
                            n, (unsigned long)e, (unsigned long)wgt, target,
                            (unsigned long)first_inv);
                    }
                }
            }
        }

        // contract-map iteration BEFORE contractVertexSet calls — captures
        // TracerMap id-ASC iteration order (audit row H site #7).
        std::fprintf(stderr,
            "[TRACE-HE] removeHeavyEdges contract_map_size:%zu "
            "mark_size:%zu\n",
            contract.size(), markForCactus.size());
        size_t group_idx = 0;
        for (const auto& [lowest, others] : contract) {
            TracerSet<NodeID> vtxset;
            vtxset.insert(G->getCurrentPosition(lowest));
            for (const auto& v : others) {
                vtxset.insert(G->getCurrentPosition(v));
            }
            // vtxset iteration order is TracerSet (id-ASC under TRACER_MODE).
            std::fprintf(stderr,
                "[TRACE-HE-GROUP] idx:%zu lowest:%u others_n:%zu "
                "vtxset_size:%zu vtxset:[",
                group_idx, lowest, others.size(), vtxset.size());
            bool first = true;
            for (const auto& v : vtxset) {
                if (!first) std::fprintf(stderr, ",");
                first = false;
                std::fprintf(stderr, "%u", v);
            }
            std::fprintf(stderr, "]\n");
            if (vtxset.size() > 1) {
                G->contractVertexSet(vtxset);
            }
            group_idx++;
        }

        for (const NodeID& e : markForCactus) {
            if (G->n() > 2) {
                NodeID n = G->getCurrentPosition(e);
                VIECUT_ASSERT_EQ(G->get_first_invalid_edge(n), 1);
                NodeID t = G->getEdgeTarget(n, 0);
                if (G->isEmpty(t)) {
                    continue;
                }
                NodeID vtx_in_t = G->containedVertices(t)[0];
                std::fprintf(stderr,
                    "[TRACE-HE-MARK] orig:%u curr_n:%u t:%u vtx_in_t:%u "
                    "n_before_del:%u\n",
                    e, n, t, vtx_in_t, G->n());
                cactusEdge.emplace_back(vtx_in_t, G->containedVertices(n));
                G->deleteVertex(n);
            }
        }
        std::fprintf(stderr,
            "[TRACE-HE] removeHeavyEdges exit n_after:%u m_after:%lu "
            "cactusEdge_size:%zu\n",
            G->n(), (unsigned long)G->m(), cactusEdge.size());
        return cactusEdge;
    }

    std::vector<std::tuple<std::pair<NodeID, NodeID>, std::vector<NodeID> > >
    contractCycleEdges(mutableGraphPtr G) {
        std::fprintf(stderr,
            "[TRACE-HE] contractCycleEdges entry n_before:%u m_before:%lu "
            "mincut:%lu\n",
            G->n(), (unsigned long)G->m(), (unsigned long)mincut);
        std::vector<std::tuple<std::pair<NodeID, NodeID>,
                               std::vector<NodeID> > > cycleEdges;
        // as we contract edges, we use basic for loop so G->n() can update
        for (NodeID n = 0; n < G->n(); ++n) {
            EdgeID first_inv = G->get_first_invalid_edge(n);
            EdgeWeight wdeg = (first_inv == 2)
                ? G->getWeightedNodeDegree(n) : (EdgeWeight)-1;
            // primary branch: deg==2 and weighted-degree==mincut.
            bool primary = (first_inv == 2 && wdeg == mincut);
            if (primary) {
                NodeID n0 = G->getEdgeTarget(n, 0);
                NodeID n1 = G->getEdgeTarget(n, 1);
                bool n0_empty = G->isEmpty(n0);
                bool n1_empty = G->isEmpty(n1);
                if (n0_empty || n1_empty) {
                    std::fprintf(stderr,
                        "[TRACE-HE-CYC] n:%u branch:primary skip_empty "
                        "n0:%u n1:%u n0e:%d n1e:%d\n",
                        n, n0, n1, (int)n0_empty, (int)n1_empty);
                    LOG1 << "TODO(anoe): This case actually happens!";
                    continue;
                }

                // if the weights are different, n will definitely be connected
                // to the heavier of n0 and n1. for readability we will set
                // n0 and n1 to this heavier vertex, when reinserting p0 will
                // definitely be in the same block as p1 (as they are equal)
                // so we don't create the additional edge to the other neighbor
                EdgeWeight w0 = G->getEdgeWeight(n, 0);
                EdgeWeight w1 = G->getEdgeWeight(n, 1);
                EdgeID rev = 0;
                bool t25_w1_gt = (w1 > w0);
                bool t26_w1_lt = (w1 < w0);
                NodeID orig_n0 = n0;
                NodeID orig_n1 = n1;
                if (t25_w1_gt) {
                    rev = 1;
                    n0 = n1;
                }
                if (t26_w1_lt) {
                    n1 = n0;
                }

                NodeID p0 = G->containedVertices(n0)[0];
                NodeID p1 = G->containedVertices(n1)[0];
                auto contained = G->containedVertices(n);
                G->setContainedVertices(n, { });
                for (const auto& c : contained) {
                    G->setCurrentPosition(c, UNDEFINED_NODE);
                }
                std::fprintf(stderr,
                    "[TRACE-HE-CYC] n:%u branch:primary first_inv:%lu "
                    "wdeg:%lu orig_n0:%u orig_n1:%u w0:%lu w1:%lu "
                    "t25_gt:%d t26_lt:%d rev:%lu n0:%u n1:%u p0:%u p1:%u "
                    "contained_n:%zu\n",
                    n, (unsigned long)first_inv, (unsigned long)wdeg,
                    orig_n0, orig_n1,
                    (unsigned long)w0, (unsigned long)w1,
                    (int)t25_w1_gt, (int)t26_w1_lt, (unsigned long)rev,
                    n0, n1, p0, p1, contained.size());
                G->contractEdgeSparseTarget(n0, G->getReverseEdge(n, rev));
                cycleEdges.emplace_back(std::make_pair(p0, p1), contained);
            } else if (first_inv == 2 &&
                       G->getEdgeWeight(n, 0) != G->getEdgeWeight(n, 1)) {
                // edge 0 heavier, thus can't be in mincut and be contracted
                EdgeWeight w0 = G->getEdgeWeight(n, 0);
                EdgeWeight w1 = G->getEdgeWeight(n, 1);
                bool h1 = (w0 < w1);
                EdgeID heavier = h1 ? 1 : 0;
                NodeID ngbr = G->getEdgeTarget(n, heavier);
                EdgeID rev = G->getReverseEdge(n, heavier);
                std::fprintf(stderr,
                    "[TRACE-HE-CYC] n:%u branch:alt first_inv:%lu w0:%lu "
                    "w1:%lu h1:%d heavier:%lu ngbr:%u rev:%lu\n",
                    n, (unsigned long)first_inv,
                    (unsigned long)w0, (unsigned long)w1,
                    (int)h1, (unsigned long)heavier, ngbr,
                    (unsigned long)rev);
                G->contractEdgeSparseTarget(ngbr, rev);
            } else {
                // No-op branch — only emit when first_inv == 2 to keep trace
                // size manageable (most graphs have many higher-degree nodes).
                if (first_inv == 2) {
                    std::fprintf(stderr,
                        "[TRACE-HE-CYC] n:%u branch:none first_inv:%lu "
                        "wdeg:%lu mincut:%lu\n",
                        n, (unsigned long)first_inv, (unsigned long)wdeg,
                        (unsigned long)mincut);
                }
            }
        }

        std::fprintf(stderr,
            "[TRACE-HE] contractCycleEdges exit n_after:%u m_after:%lu "
            "cycleEdges_size:%zu\n",
            G->n(), (unsigned long)G->m(), cycleEdges.size());
        return cycleEdges;
    }

    void reInsertCycles(
        mutableGraphPtr G,
        std::vector<std::tuple<std::pair<NodeID, NodeID>,
                               std::vector<NodeID> > > toInsert) {
        for (size_t i = toInsert.size(); i-- > 0; ) {
            const auto& [p, cont] = toInsert[i];
            NodeID n0 = G->getCurrentPosition(p.first);
            NodeID n1 = G->getCurrentPosition(p.second);
            NodeID reIns = G->new_empty_node();
            if (n0 == n1) {
                G->new_edge_order(n0, reIns, mincut);
                G->setContainedVertices(reIns, cont);
                for (NodeID v : cont) {
                    G->setCurrentPosition(v, reIns);
                }
            } else {
                EdgeID e = UNDEFINED_EDGE;
                for (EdgeID arc : G->edges_of(n0)) {
                    if (G->getEdgeTarget(n0, arc) == n1) {
                        e = arc;
                        break;
                    }
                }

                if (e == UNDEFINED_EDGE) {
                    LOG1 << "Vertices aren't neighbours! Something is wrong!";
                    exit(1);
                }

                G->new_edge_order(n0, reIns, mincut / 2);
                G->new_edge_order(n1, reIns, mincut / 2);
                EdgeWeight w01 = G->getEdgeWeight(n0, e);
                if (w01 == (mincut / 2)) {
                    G->deleteEdge(n0, e);
                } else {
                    G->setEdgeWeight(n0, e, w01 - (mincut / 2));
                }
            }
            G->setContainedVertices(reIns, cont);
            for (NodeID v : cont) {
                G->setCurrentPosition(v, reIns);
            }
        }
    }

    void reInsertVertices(
        mutableGraphPtr G,
        std::vector<std::tuple<NodeID, std::vector<NodeID> > > toInsert) {
        for (size_t i = toInsert.size(); i-- > 0; ) {
            const auto& [t, cont] = toInsert[i];
            NodeID curr = G->getCurrentPosition(t);
            NodeID vtx = G->new_empty_node();
            G->new_edge_order(curr, vtx, mincut);
            G->setContainedVertices(vtx, cont);
            for (const auto& e : G->containedVertices(vtx)) {
                G->setCurrentPosition(e, vtx);
            }
        }
    }

 private:
    EdgeWeight mincut;
};
