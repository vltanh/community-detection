/******************************************************************************
 * recursive_cactus.h
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
#include <map>
#include <memory>
#include <queue>
#include <string>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>

#include "tools/container_swap.h"
#include "algorithms/flow/push_relabel.h"
#include "algorithms/global_mincut/cactus/all_cut_local_red.h"
#include "algorithms/global_mincut/cactus/graph_modification.h"
#include "algorithms/global_mincut/cactus/heavy_edges.h"
#include "algorithms/global_mincut/noi_minimum_cut.h"
#include "algorithms/misc/graph_algorithms.h"
#include "algorithms/misc/strongly_connected_components.h"
#include "algorithms/multicut/multicut_problem.h"
#include "common/configuration.h"
#include "common/definitions.h"
#include "data_structure/mutable_graph.h"
#include "data_structure/priority_queues/node_bucket_pq.h"
#include "tlx/logger.hpp"
#include "tools/random_functions.h"
#include "tools/string.h"

#ifdef PARALLEL
#include "parallel/coarsening/contract_graph.h"
#include "parallel/data_structure/union_find.h"
#else
#include "coarsening/contract_graph.h"
#include "data_structure/union_find.h"
#endif

template <class GraphPtr>
class recursive_cactus {
 public:
    recursive_cactus() { }
    explicit recursive_cactus(EdgeWeight mincut)
        : mincut(mincut), problem_id(random_functions::next()) { }
    ~recursive_cactus() { }

    static constexpr bool debug = false;
    bool timing = configuration::getConfig()->verbose;

    void setMincut(EdgeWeight mc) {
        mincut = mc;
    }

    mutableGraphPtr flowMincut(
        const std::vector<GraphPtr>& graphs) {
        // Note: Min added because of stack uninitialized value errors in cactus mincut
        this->problem_id = random_functions::next();

        std::vector<mutableGraphPtr> flow_graphs;

        mutableGraphPtr in_graph;
        if constexpr (std::is_same<GraphPtr, mutableGraphPtr>::value) {
            in_graph = std::make_shared<mutable_graph>(*(graphs.back()));
        } else {
            in_graph = mutable_graph::from_graph_access(graphs.back());
        }

        auto out_graph = recursiveCactus(in_graph, 0);
        VIECUT_ASSERT_TRUE(graph_modification::isCNCR(out_graph, mincut));
        LOGC(timing) << "t " << t.elapsed() << " cactus n "
                     << out_graph->n() << " m " << out_graph->m();
        return out_graph;
    }

    /*
    * This is called in the case that the minimum cut is decreased by deleting
    * an edge between vertices s and t. Thus we know that all minimum cuts
    * separate s and t and we can just run one run of findSTCactus(s, t) which
    * is significantly faster than building the cactus completely
    */
    mutableGraphPtr decrementalRebuild(mutableGraphPtr graph,
                                       NodeID s, EdgeWeight mincut,
                                       size_t fpid) {
        setMincut(mincut);
        strongly_connected_components scc;
        auto [v, num_comp, blocksizes] = scc.strong_components(graph, fpid);
        auto STCactus = findSTCactus(v, graph, s, num_comp);
        return STCactus;
    }

 private:
    mutableGraphPtr recursiveCactus(
        mutableGraphPtr G, size_t depth) {
        // [TRACE-RC-W] wrapper entry+exit (recursive_cactus.h:102-111).
        // Distinguishes wrapper frame from internal frame; emits G state
        // BEFORE+AFTER heavy_edges.removeHeavyEdges + contractCycleEdges +
        // reInsertCycles + reInsertVertices.
        std::fprintf(stderr,
            "[TRACE-RC-W] enter depth:%zu n_in:%u m_in:%lu mincut:%lu\n",
            depth, G->n(), (unsigned long)G->m(), (unsigned long)mincut);
        heavy_edges he(mincut);
        auto cactusEdges = he.removeHeavyEdges(G);
        std::fprintf(stderr,
            "[TRACE-RC-W] after_removeHeavy depth:%zu n:%u m:%lu "
            "cactusEdges_n:%zu\n",
            depth, G->n(), (unsigned long)G->m(), cactusEdges.size());
        auto cycleEdges = he.contractCycleEdges(G);
        std::fprintf(stderr,
            "[TRACE-RC-W] after_contractCycle depth:%zu n:%u m:%lu "
            "cycleEdges_n:%zu\n",
            depth, G->n(), (unsigned long)G->m(), cycleEdges.size());
        G = internalRecursiveCactus(G, depth);
        std::fprintf(stderr,
            "[TRACE-RC-W] after_internal depth:%zu n:%u m:%lu\n",
            depth, G->n(), (unsigned long)G->m());
        he.reInsertCycles(G, cycleEdges);
        std::fprintf(stderr,
            "[TRACE-RC-W] after_reInsertCycles depth:%zu n:%u m:%lu\n",
            depth, G->n(), (unsigned long)G->m());
        for (NodeID gn : G->nodes()) {
            std::fprintf(stderr,
                "[TRACE-RC-W]  reInsCyc_contained[%u]:", gn);
            for (NodeID cv : G->containedVertices(gn))
                std::fprintf(stderr, "%u,", cv);
            std::fprintf(stderr, "\n");
        }
        he.reInsertVertices(G, cactusEdges);
        std::fprintf(stderr,
            "[TRACE-RC-W] after_reInsertVerts depth:%zu n:%u m:%lu\n",
            depth, G->n(), (unsigned long)G->m());
        for (NodeID gn : G->nodes()) {
            std::fprintf(stderr,
                "[TRACE-RC-W]  reInsVtx_contained[%u]:", gn);
            for (NodeID cv : G->containedVertices(gn))
                std::fprintf(stderr, "%u,", cv);
            std::fprintf(stderr, "\n");
        }
        std::fprintf(stderr,
            "[TRACE-RC-W] exit depth:%zu n_out:%u m_out:%lu\n",
            depth, G->n(), (unsigned long)G->m());
        return G;
    }

    mutableGraphPtr internalRecursiveCactus(
        mutableGraphPtr G, size_t depth) {
        std::fprintf(stderr,
            "[TRACE-RC] internalRecursiveCactus depth:%zu n:%u m:%lu "
            "problem_id:%zu\n",
            depth, G->n(), (unsigned long)G->m(), problem_id);
        if (depth % 100 == 0) {
            LOGC(configuration::getConfig()->verbose) << "depth " << depth
                                                      << " G n " << G->n()
                                                      << " m " << G->m();
        }

        if (depth % 10 == 0) {
            size_t previous = UNDEFINED_NODE;
            int reduce_iter = 0;
            while (previous > G->n()) {
                previous = G->n();
                noi_minimum_cut<mutableGraphPtr> noi;
                auto uf = noi.modified_capforest(G, mincut + 1);
                std::fprintf(stderr,
                    "[TRACE-RC] reduce iter:%d step:capforest "
                    "before_n:%u uf_n:%u\n",
                    reduce_iter, G->n(), uf.n());
                G = contraction::fromUnionFind(G, &uf);
                std::fprintf(stderr,
                    "[TRACE-RC] reduce iter:%d step:capforest after_n:%u\n",
                    reduce_iter, G->n());
                for (NodeID gn : G->nodes()) {
                    std::fprintf(stderr,
                        "[TRACE-RC]  capforest_after_contained[%u]:", gn);
                    for (NodeID cv : G->containedVertices(gn))
                        std::fprintf(stderr, "%u,", cv);
                    std::fprintf(stderr, "\n");
                }
                auto uf12 = all_cut_local_red::allCutsPrTests12(G, mincut);
                G = contraction::fromUnionFind(G, &uf12);
                std::fprintf(stderr,
                    "[TRACE-RC] reduce iter:%d step:pr12 after_n:%u\n",
                    reduce_iter, G->n());
                auto uf34 = all_cut_local_red::allCutsPrTests34(G, mincut);
                G = contraction::fromUnionFind(G, &uf34);
                std::fprintf(stderr,
                    "[TRACE-RC] reduce iter:%d step:pr34 after_n:%u\n",
                    reduce_iter, G->n());
                reduce_iter++;
            }
        }

        if (G->number_of_nodes() == 1 || G->number_of_edges() == 0) {
            return G;
        }
        VIECUT_ASSERT_TRUE(graph_modification::isCNCR(G, mincut));
        FlowType max_flow;
        NodeID s = 0;
        NodeID tgt = 0;
        EdgeID e = 0;

        std::string es = configuration::getConfig()->edge_selection;
        std::fprintf(stderr,
            "[TRACE-RC] edge_selection depth:%zu es:%s\n",
            depth, es.c_str());

        if (es == "heavy")
            std::tie(s, e, tgt) = maximumFlowEdge(G);
        if (es == "heavy_weighted" || es == "" || es == "heavy_vertex")
            std::tie(s, e, tgt) = maximumWeightedFlowEdge(G);
        if (es == "central")
            std::tie(s, e, tgt) = centralFlowEdge(G);
        if (es == "random")
            std::tie(s, e, tgt) = findFlowEdge(G);

        std::fprintf(stderr,
            "[TRACE-RC] flow_edge depth:%zu s:%u e:%lu tgt:%u\n",
            depth, s, (unsigned long)e, tgt);

        {
            std::vector<NodeID> vtcs = { s, tgt };
            push_relabel pr;
            size_t pid_before = problem_id;
            problem_id++;
            std::fprintf(stderr,
                "[TRACE-RC] problem_id depth:%zu pid_before:%zu "
                "pid_after:%zu\n",
                depth, pid_before, problem_id);
            max_flow = pr.solve_max_flow_min_cut(
                G, vtcs, 0, false, false, problem_id).first;
        }
        std::fprintf(stderr,
            "[TRACE-RC] max_flow depth:%zu val:%ld mincut:%lu\n",
            depth, (long)max_flow, (unsigned long)mincut);

        bool mf_gt_mc = (max_flow > (FlowType)mincut);
        std::fprintf(stderr,
            "[TRACE-RC] mf_branch depth:%zu mf:%ld mincut:%lu gt:%d\n",
            depth, (long)max_flow, (unsigned long)mincut, (int)mf_gt_mc);
        if (mf_gt_mc) {
            LOG << "max flow is larger " << max_flow;
            VIECUT_ASSERT_EQ(G->getEdgeTarget(s, e), tgt);
            G->contractEdge(s, e);
            std::fprintf(stderr,
                "[TRACE-RC] contractEdge depth:%zu n_after:%u m_after:%lu\n",
                depth, G->n(), (unsigned long)G->m());
            G = recursiveCactus(G, depth + 1);
            return G;
        } else {
            bool eq_2 = (G->number_of_nodes() == 2);
            std::fprintf(stderr,
                "[TRACE-RC] n2_branch depth:%zu n:%u eq2:%d\n",
                depth, G->n(), (int)eq_2);
            if (eq_2) {
                return G;
            }
            strongly_connected_components scc;
            auto [v, num_comp, blocksizes] =
                scc.strong_components(G, problem_id);
            EdgeWeight deg_s = G->getWeightedNodeDegree(s);
            EdgeWeight deg_t = G->getWeightedNodeDegree(tgt);
            bool nc_eq_2 = (num_comp == 2);
            bool deg_s_eq_mc = (deg_s == mincut);
            bool deg_t_eq_mc = (deg_t == mincut);
            bool sc_branch = nc_eq_2 && (deg_s_eq_mc || deg_t_eq_mc);
            std::fprintf(stderr,
                "[TRACE-RC] singleton_branch depth:%zu num_comp:%zu "
                "nc_eq_2:%d deg_s:%lu deg_t:%lu mc:%lu deg_s_eq_mc:%d "
                "deg_t_eq_mc:%d take:%d\n",
                depth, num_comp, (int)nc_eq_2,
                (unsigned long)deg_s, (unsigned long)deg_t,
                (unsigned long)mincut, (int)deg_s_eq_mc,
                (int)deg_t_eq_mc, (int)sc_branch);
            if (sc_branch) {
                std::vector<int> empty;
                v.swap(empty);
                NodeID ctr = deg_s_eq_mc ? s : tgt;
                NodeID other = (ctr == s) ? tgt : s;
                VIECUT_ASSERT_EQ(G->getWeightedNodeDegree(ctr), mincut);
                auto elementsInCtr = G->containedVertices(ctr);
                auto elementsInOther = G->containedVertices(other);
                std::fprintf(stderr,
                    "[TRACE-RC] singleton ctr:%u other:%u "
                    "elemCtr_n:%zu elemOther_n:%zu elemCtr:[",
                    ctr, other, elementsInCtr.size(), elementsInOther.size());
                for (size_t k = 0; k < elementsInCtr.size(); ++k) {
                    if (k) std::fprintf(stderr, ",");
                    std::fprintf(stderr, "%u", elementsInCtr[k]);
                }
                std::fprintf(stderr, "] elemOther:[");
                for (size_t k = 0; k < elementsInOther.size(); ++k) {
                    if (k) std::fprintf(stderr, ",");
                    std::fprintf(stderr, "%u", elementsInOther[k]);
                }
                std::fprintf(stderr, "]\n");
                G->contractEdge(s, e);
                std::fprintf(stderr,
                    "[TRACE-RC] singleton after_contract n:%u m:%lu\n",
                    G->n(), (unsigned long)G->m());
                NodeID contracted_v = G->getCurrentPosition(elementsInCtr[0]);
                G->setContainedVertices(contracted_v, elementsInOther);
                std::fprintf(stderr,
                    "[TRACE-RC] singleton contracted_v:%u setContained_n:%zu\n",
                    contracted_v, elementsInOther.size());
                size_t set_pos_idx = 0;
                for (NodeID n : elementsInOther) {
                    G->setCurrentPosition(n, contracted_v);
                    std::fprintf(stderr,
                        "[TRACE-RC] singleton setCurrPos idx:%zu n:%u pos:%u\n",
                        set_pos_idx++, n, contracted_v);
                }

                VIECUT_ASSERT_TRUE(graph_modification::isCNCR(G, mincut));
                auto ret = recursiveCactus(G, depth + 1);
                NodeID other_now = ret->getCurrentPosition(elementsInOther[0]);
                NodeID new_node = ret->new_empty_node();
                std::fprintf(stderr,
                    "[TRACE-RC] singleton other_now:%u new_node:%u "
                    "ret_n:%u ret_m:%lu\n",
                    other_now, new_node, ret->n(),
                    (unsigned long)ret->m());
                ret->new_edge(other_now, new_node, mincut);
                ret->setContainedVertices(new_node, elementsInCtr);
                size_t set_pos2_idx = 0;
                for (NodeID n : elementsInCtr) {
                    ret->setCurrentPosition(n, new_node);
                    std::fprintf(stderr,
                        "[TRACE-RC] singleton ctr_setCurrPos idx:%zu "
                        "n:%u pos:%u\n",
                        set_pos2_idx++, n, new_node);
                }
                return ret;
            }

            auto STCactus = findSTCactus(v, G, s, num_comp);

            double g_n = static_cast<double>(G->n());
            std::fprintf(stderr,
                "[TRACE-RC] block_iter_init depth:%zu num_comp:%zu g_n:%f "
                "g_n_half:%f\n",
                depth, num_comp, g_n, g_n / 2.0);
            // first the small blocks, last the big one. then we don't need to
            // copy graphs as the small ones are newly generated
            for (int c = 0; c < static_cast<int>(num_comp); ++c) {
                double bs_d = static_cast<double>(blocksizes[c]);
                bool small = (bs_d <= (g_n / 2.0));
                std::fprintf(stderr,
                    "[TRACE-RC] block_iter_small depth:%zu c:%d "
                    "blocksize:%zu bs_d:%f le_half:%d\n",
                    depth, c, blocksizes[c], bs_d, (int)small);
                if (small) {
                    STCactus = mergeCactusWithComponent(
                        STCactus, G, depth, c, v, blocksizes[c]);
                }
            }
            for (int c = 0; c < static_cast<int>(num_comp); ++c) {
                double bs_d = static_cast<double>(blocksizes[c]);
                bool big = (bs_d > (g_n / 2.0));
                std::fprintf(stderr,
                    "[TRACE-RC] block_iter_big depth:%zu c:%d "
                    "blocksize:%zu bs_d:%f gt_half:%d\n",
                    depth, c, blocksizes[c], bs_d, (int)big);
                if (big) {
                    STCactus = mergeCactusWithComponent(
                        STCactus, G, depth, c, v, blocksizes[c]);
                }
            }
            VIECUT_ASSERT_TRUE(graph_modification::isCNCR(STCactus, mincut));
            return STCactus;
        }
    }

    mutableGraphPtr mergeCactusWithComponent(
        mutableGraphPtr STCactus,
        mutableGraphPtr G,
        size_t depth, int component,
        const std::vector<int>& scc_result, size_t blocksize) {
        // [TRACE-RC-MC] entry — emit STCactus + G state + component + blocksize
        // BEFORE any of the two arms execute (recursive_cactus.h:228-356).
        std::fprintf(stderr,
            "[TRACE-RC-MC] enter depth:%zu component:%d blocksize:%zu "
            "STCactus_n:%u G_n:%u g_n_half:%f bs_le_half:%d\n",
            depth, component, blocksize, STCactus->n(), G->n(),
            static_cast<double>(G->n()) / 2.0,
            (int)(static_cast<double>(blocksize)
                  <= static_cast<double>(G->n()) / 2.0));
        NodeID uncontracted_base_vertex = UNDEFINED_NODE;
        NodeID contracted_base_vertex = UNDEFINED_NODE;
        mutableGraphPtr graph;
        if (static_cast<double>(blocksize) <=
            (static_cast<double>(G->n()) / 2.0)) {
            std::fprintf(stderr,
                "[TRACE-RC-MC] arm:small depth:%zu blocksize:%zu G_n:%u\n",
                depth, blocksize, G->n());
            graph = std::make_shared<mutable_graph>();
            graph->start_construction(blocksize + 1);
            graph->setOriginalNodes(G->getOriginalNodes());
            graph->new_empty_node();
            for (NodeID n : graph->nodes()) {
                graph->setContainedVertices(n, { });
            }

            std::vector<NodeID> contained;
            NodeID vtx = 0;

            for (NodeID n : G->nodes()) {
                if (scc_result[n] == component) {
                    graph->new_empty_node();
                    contained.emplace_back(vtx++);
                    bool first_un = (uncontracted_base_vertex == UNDEFINED_NODE
                                     && !G->containedVertices(n).empty());
                    if (first_un) {
                        uncontracted_base_vertex = G->containedVertices(n)[0];
                    }
                    std::fprintf(stderr,
                        "[TRACE-RC-MC-N] n:%u arm:eq_comp slot:%u "
                        "first_un:%d ubv:%u\n",
                        n, contained.back(), (int)first_un,
                        uncontracted_base_vertex);
                    for (NodeID con : G->containedVertices(n)) {
                        graph->addContainedVertex(contained.back(), con);
                        graph->setCurrentPosition(con, contained.back());
                    }
                } else {
                    contained.emplace_back(blocksize);
                    bool first_ct = (contracted_base_vertex == UNDEFINED_NODE
                                     && !G->containedVertices(n).empty());
                    if (first_ct) {
                        contracted_base_vertex = G->containedVertices(n)[0];
                    }
                    std::fprintf(stderr,
                        "[TRACE-RC-MC-N] n:%u arm:neq_comp slot:%zu "
                        "first_ct:%d cbv:%u\n",
                        n, blocksize, (int)first_ct,
                        contracted_base_vertex);
                    for (NodeID con : G->containedVertices(n)) {
                        graph->addContainedVertex(blocksize, con);
                        graph->setCurrentPosition(con, blocksize);
                    }
                }
            }

            // Per-edge accumulator BEFORE+AFTER (row M).
            for (NodeID n : G->nodes()) {
                if (contained[n] != blocksize) {
                    EdgeWeight to_contracted = 0;
                    for (EdgeID e : G->edges_of(n)) {
                        NodeID t = G->getEdgeTarget(n, e);
                        EdgeWeight wgt = G->getEdgeWeight(n, e);
                        EdgeWeight tc_before = to_contracted;
                        bool t_is_blk = (contained[t] == blocksize);
                        bool n_lt_t = (contained[n] < contained[t]);
                        if (t_is_blk) {
                            to_contracted += wgt;
                            std::fprintf(stderr,
                                "[TRACE-RC-MC-E] n:%u e:%lu t:%u wgt:%lu "
                                "branch:to_contracted cn:%u ct:%u "
                                "tc_before:%lu tc_after:%lu\n",
                                n, (unsigned long)e, t,
                                (unsigned long)wgt,
                                contained[n], contained[t],
                                (unsigned long)tc_before,
                                (unsigned long)to_contracted);
                        } else if (n_lt_t) {
                            graph->new_edge(contained[n], contained[t], wgt);
                            std::fprintf(stderr,
                                "[TRACE-RC-MC-E] n:%u e:%lu t:%u wgt:%lu "
                                "branch:new_edge cn:%u ct:%u tc:%lu\n",
                                n, (unsigned long)e, t,
                                (unsigned long)wgt,
                                contained[n], contained[t],
                                (unsigned long)to_contracted);
                        } else {
                            std::fprintf(stderr,
                                "[TRACE-RC-MC-E] n:%u e:%lu t:%u wgt:%lu "
                                "branch:skip cn:%u ct:%u\n",
                                n, (unsigned long)e, t,
                                (unsigned long)wgt,
                                contained[n], contained[t]);
                        }
                    }

                    bool tc_gt_0 = (to_contracted > 0);
                    if (tc_gt_0) {
                        graph->new_edge(contained[n], blocksize, to_contracted);
                    }
                    std::fprintf(stderr,
                        "[TRACE-RC-MC-N-DONE] n:%u tc:%lu emit_to_blk:%d\n",
                        n, (unsigned long)to_contracted, (int)tc_gt_0);
                }
            }
            graph->finish_construction();
            std::fprintf(stderr,
                "[TRACE-RC-MC] small_after_build n:%u m:%lu\n",
                graph->n(), (unsigned long)graph->m());
        } else {
            std::fprintf(stderr,
                "[TRACE-RC-MC] arm:big depth:%zu blocksize:%zu G_n:%u\n",
                depth, blocksize, G->n());
            // find a node in G that is contracted
            // and one that is not contracted,
            // use their location in contracted graphs
            // to re-find nodes as IDs swap around
            TracerSet<NodeID> all_ctr;
            for (size_t i = 0; i < scc_result.size(); ++i) {
                if (scc_result[i] != component) {
                    all_ctr.insert(i);
                    bool first_ct = (contracted_base_vertex == UNDEFINED_NODE
                                     && !G->containedVertices(i).empty());
                    if (first_ct) {
                        contracted_base_vertex = G->containedVertices(i)[0];
                    }
                    std::fprintf(stderr,
                        "[TRACE-RC-MC-I] i:%zu arm:neq_comp insert "
                        "all_ctr_size:%zu first_ct:%d cbv:%u\n",
                        i, all_ctr.size(), (int)first_ct,
                        contracted_base_vertex);
                } else {
                    bool first_un = (uncontracted_base_vertex == UNDEFINED_NODE
                                     && !G->containedVertices(i).empty());
                    if (first_un) {
                        uncontracted_base_vertex = G->containedVertices(i)[0];
                    }
                    std::fprintf(stderr,
                        "[TRACE-RC-MC-I] i:%zu arm:eq_comp skip "
                        "first_un:%d ubv:%u\n",
                        i, (int)first_un, uncontracted_base_vertex);
                }
            }
            std::fprintf(stderr,
                "[TRACE-RC-MC] all_ctr_dump size:%zu members:[",
                all_ctr.size());
            {
                bool first = true;
                for (const auto& v : all_ctr) {
                    if (!first) std::fprintf(stderr, ",");
                    first = false;
                    std::fprintf(stderr, "%u", v);
                }
            }
            std::fprintf(stderr,
                "] G_n_before:%u G_m_before:%lu\n",
                G->n(), (unsigned long)G->m());
            graph = G;
            graph->contractVertexSet(all_ctr);
            std::fprintf(stderr,
                "[TRACE-RC-MC] after_contractVertexSet n:%u m:%lu\n",
                graph->n(), (unsigned long)graph->m());
        }
        std::fprintf(stderr,
            "[TRACE-RC-MC] before_recurse depth:%zu graph_n:%u m:%lu "
            "ubv:%u cbv:%u\n",
            depth, graph->n(), (unsigned long)graph->m(),
            uncontracted_base_vertex, contracted_base_vertex);
        auto n_i = recursiveCactus(graph, depth + 1);
        NodeID merge_vtx_in_cactus = STCactus->getCurrentPosition(
            uncontracted_base_vertex);
        NodeID nibar = n_i->getCurrentPosition(contracted_base_vertex);
        std::fprintf(stderr,
            "[TRACE-RC-MC] merge_args depth:%zu STCactus_n:%u n_i_n:%u "
            "merge_vtx:%u nibar:%u mincut:%lu\n",
            depth, STCactus->n(), n_i->n(),
            merge_vtx_in_cactus, nibar, (unsigned long)mincut);
        STCactus = graph_modification::mergeGraphs(
            STCactus, merge_vtx_in_cactus, n_i, nibar, mincut);
        std::fprintf(stderr,
            "[TRACE-RC-MC] after_merge STCactus_n:%u STCactus_m:%lu\n",
            STCactus->n(), (unsigned long)STCactus->m());
        VIECUT_ASSERT_TRUE(graph_modification::isCNCR(STCactus, mincut));
        return STCactus;
    }

    mutableGraphPtr findSTCactus(
        const std::vector<int>& v, mutableGraphPtr G,
        NodeID s, int num_comp) {
        std::fprintf(stderr,
            "[TRACE-RC] findSTCactus_entry num_comp:%d s:%u G_n:%u\n",
            num_comp, s, G->n());
        for (NodeID gn : G->nodes()) {
            std::fprintf(stderr, "[TRACE-RC]  G_contained[%u]:", gn);
            for (NodeID cv : G->containedVertices(gn))
                std::fprintf(stderr, "%u,", cv);
            std::fprintf(stderr, "\n");
        }
        auto contract = std::make_shared<mutable_graph>();
        contract->start_construction(num_comp);
        NodeID contained = G->containedVertices(s)[0];
        contract->setOriginalNodes(G->getOriginalNodes());
        for (NodeID n : contract->nodes()) {
            contract->setContainedVertices(n, { });
        }
        for (NodeID n = 0; n < G->getOriginalNodes(); ++n) {
            NodeID pos = G->getCurrentPosition(n);
            if (pos < G->n()) {
                NodeID n_in_contract = v[G->getCurrentPosition(n)];
                contract->addContainedVertex(n_in_contract, n);
                contract->setCurrentPosition(n, n_in_contract);
            }
        }
        for (NodeID n : contract->nodes()) {
            for (NodeID m : contract->nodes()) {
                if (n < m) {
                    contract->new_edge(n, m, 0);
                }
            }
        }
        // [TRACE-STC-Q] quotient-edge accumulator (recursive_cactus.h:392-403)
        // Composite arithmetic chain: e_ctr index computed via integer
        // comparison-as-int, wgt_ctr accumulator over edges between SCC
        // components. Single-ulp / integer-truncation drift here flips
        // cactus weights. Emit ALL intermediates BEFORE+AFTER per
        // byte-equal-tracer "Extensive printout" discipline.
        for (NodeID n : G->nodes()) {
            for (EdgeID e : G->edges_of(n)) {
                NodeID t = G->getEdgeTarget(n, e);
                EdgeWeight wgt = G->getEdgeWeight(n, e);
                int ctr = v[t];
                int v_n = v[n];
                bool gate = (v_n > ctr);
                if (gate) {
                    int ctr_gt_vn = (ctr > v_n) ? 1 : 0;
                    EdgeID e_ctr = ctr - ctr_gt_vn;
                    EdgeWeight wgt_ctr_before =
                        contract->getEdgeWeight(v_n, e_ctr);
                    EdgeWeight wgt_ctr = wgt + wgt_ctr_before;
                    contract->setEdgeWeight(v_n, e_ctr, wgt_ctr);
                    std::fprintf(stderr,
                        "[TRACE-STC-Q] n:%u e:%lu t:%u wgt:%lu v_n:%d "
                        "ctr:%d gate:1 ctr_gt_vn:%d e_ctr:%lu "
                        "wgt_ctr_before:%lu wgt_ctr_after:%lu\n",
                        n, (unsigned long)e, t, (unsigned long)wgt,
                        v_n, ctr, ctr_gt_vn, (unsigned long)e_ctr,
                        (unsigned long)wgt_ctr_before,
                        (unsigned long)wgt_ctr);
                } else {
                    std::fprintf(stderr,
                        "[TRACE-STC-Q] n:%u e:%lu t:%u wgt:%lu v_n:%d "
                        "ctr:%d gate:0\n",
                        n, (unsigned long)e, t, (unsigned long)wgt,
                        v_n, ctr);
                }
            }
        }
        contract->finish_construction();
        auto stcactus = std::make_shared<mutable_graph>();
        NodeID num_vertices = contract->n();
        stcactus->start_construction(num_vertices);
        stcactus->resizePositions(contract->getOriginalNodes());
        s = contract->getCurrentPosition(contained);
        node_bucket_pq pq(num_vertices, mincut + 1);
        for (NodeID n = 0; n < num_vertices; ++n) {
            stcactus->new_node();
            if (n != s)
                pq.insert(n, 0);
        }
        for (EdgeID e : contract->edges_of(s)) {
            NodeID tgt = contract->getEdgeTarget(s, e);
            pq.increaseKey(tgt, contract->getEdgeWeight(s, e));
        }
        std::vector<NodeID> node_mapping(contract->number_of_nodes());
        std::vector<NodeID> rev_node_mapping(contract->number_of_nodes());
        stcactus->setContainedVertices(0, contract->containedVertices(s));
        node_mapping[s] = 0;
        for (NodeID v : stcactus->containedVertices(0)) {
            stcactus->setCurrentPosition(v, 0);
        }
        for (NodeID n = 1; n < num_vertices; ++n) {
            NodeID next = pq.deleteMax();
            std::fprintf(stderr,
                "[TRACE-RC] findSTCactus_pop slot:%u contract_node:%u "
                "key:%lu\n", n, next, (unsigned long)pq.getKey(next));
            for (EdgeID e : contract->edges_of(next)) {
                NodeID tgt = contract->getEdgeTarget(next, e);
                EdgeWeight wgt = pq.getKey(tgt);
                if (pq.contains(tgt)) {
                    EdgeWeight new_wgt = wgt + contract->getEdgeWeight(next, e);
                    pq.increaseKey(tgt, std::min(new_wgt, mincut));
                }
            }
            node_mapping[next] = n;
            rev_node_mapping[n] = next;
            stcactus->setContainedVertices(
                n, contract->containedVertices(next));
            for (NodeID v : stcactus->containedVertices(n)) {
                stcactus->setCurrentPosition(v, n);
            }
        }
        stcactus->finish_construction();
        std::vector<std::vector<NodeID> > A;
        std::vector<NodeID> B;
        std::vector<bool> order;
        size_t i = 1;
        B.emplace_back(0);
        order.emplace_back(false);
        // [TRACE-STC-SEG] cycle segmentation loop
        // (recursive_cactus.h:455-477). T10 boundary cycle_degree == 0 ||
        // == mincut; tie at exact-mincut flips a cycle into a tree segment.
        // Per-iteration: emit cycle_degree BEFORE+AFTER each edge (the
        // accumulator delta), branch decision, curr_cycle state.
        while (i < (contract->number_of_nodes() - 1)) {
            EdgeWeight cycle_degree = 0;
            // membership-only test (.count(tgt)); iteration-order independent;
            // left as std::unordered_set per audit row H site #9.
            std::unordered_set<NodeID> curr_cycle;
            NodeID n = rev_node_mapping[i];
            size_t outer_step = 0;
            while ((cycle_degree == 0 || cycle_degree == mincut)
                   && (i + 1 < contract->number_of_nodes())) {
                n = rev_node_mapping[i];
                std::fprintf(stderr,
                    "[TRACE-STC-SEG] outer_i:%zu outer_step:%zu n:%u "
                    "cycle_degree_before:%lu curr_cycle_size:%zu\n",
                    i, outer_step, n, (unsigned long)cycle_degree,
                    curr_cycle.size());
                for (EdgeID e : contract->edges_of(n)) {
                    NodeID tgt = contract->getEdgeTarget(n, e);
                    EdgeWeight wgt = contract->getEdgeWeight(n, e);
                    bool in_cycle = curr_cycle.count(tgt) > 0;
                    EdgeWeight cd_before = cycle_degree;
                    if (in_cycle) {
                        cycle_degree -= wgt;
                    } else {
                        cycle_degree += wgt;
                    }
                    std::fprintf(stderr,
                        "[TRACE-STC-SEG-E] outer_i:%zu n:%u e:%lu tgt:%u "
                        "wgt:%lu in_cycle:%d cd_before:%lu cd_after:%lu\n",
                        i, n, (unsigned long)e, tgt, (unsigned long)wgt,
                        (int)in_cycle,
                        (unsigned long)cd_before,
                        (unsigned long)cycle_degree);
                }
                bool eq_mc = (cycle_degree == mincut);
                if (eq_mc) {
                    i++;
                    curr_cycle.insert(n);
                }
                std::fprintf(stderr,
                    "[TRACE-STC-SEG] outer_step:%zu n:%u cd_after_loop:%lu "
                    "eq_mc:%d i_after:%zu curr_cycle_size_after:%zu\n",
                    outer_step, n, (unsigned long)cycle_degree,
                    (int)eq_mc, i, curr_cycle.size());
                outer_step++;
            }
            if (curr_cycle.size() > 0) {
                A.emplace_back();
                order.emplace_back(true);
                std::fprintf(stderr,
                    "[TRACE-STC-SEG] commit_cycle A_idx:%zu B_idx:%zu "
                    "size:%zu vstart:%zu vend:%zu\n",
                    A.size() - 1, B.size(), curr_cycle.size(),
                    (size_t)(i - curr_cycle.size()), i);
                for (size_t v = i - curr_cycle.size(); v < i; ++v) {
                    A.back().emplace_back(v);
                }
            } else {
                i++;
                B.emplace_back(node_mapping[n]);
                order.emplace_back(false);
                std::fprintf(stderr,
                    "[TRACE-STC-SEG] commit_tree A_idx:%zu B_idx:%zu "
                    "node_mapping:%u i_after:%zu\n",
                    A.size(), B.size() - 1, node_mapping[n], i);
            }
        }
        order.emplace_back(false);
        B.emplace_back(num_vertices - 1);
        NodeID previous = 0;
        size_t a_index = 0, b_index = 0;
        VIECUT_ASSERT_EQ(order.size(), A.size() + B.size());
        // [TRACE-STC-OUT] output cactus edge emission
        // (recursive_cactus.h:495-530). Output cactus structure determined
        // entirely here. Emit per-iteration order[i] branch, j-loop step,
        // previous chain BEFORE+AFTER, new_edge_order args.
        for (size_t i = 0; i < (A.size() + B.size() - 1); ++i) {
            bool order_i = order[i];
            bool order_next = (i + 1 < order.size()) ? (bool)order[i + 1]
                                                     : false;
            std::fprintf(stderr,
                "[TRACE-STC-OUT] i:%zu order_i:%d order_next:%d "
                "previous:%u a_index:%zu b_index:%zu A_size:%zu B_size:%zu\n",
                i, (int)order_i, (int)order_next, previous,
                a_index, b_index, A.size(), B.size());
            if (order_i) {
                // make cycle
                size_t A_n = A[a_index].size();
                for (size_t j = 0; j < A_n; ++j) {
                    if (j > 0) {
                        std::fprintf(stderr,
                            "[TRACE-STC-OUT-E] i:%zu j:%zu kind:within "
                            "u:%u v:%u w:%lu\n",
                            i, j, A[a_index][j - 1], A[a_index][j],
                            (unsigned long)(mincut / 2));
                        stcactus->new_edge_order(A[a_index][j - 1],
                                                 A[a_index][j], mincut / 2);
                    } else {
                        std::fprintf(stderr,
                            "[TRACE-STC-OUT-E] i:%zu j:%zu kind:from_prev "
                            "u:%u v:%u w:%lu\n",
                            i, j, previous, A[a_index][0],
                            (unsigned long)(mincut / 2));
                        stcactus->new_edge_order(previous,
                                                 A[a_index][0], mincut / 2);
                    }
                    if (j == A_n - 1) {
                        // last vertex, connect with next cycle or ordered vtx
                        NodeID next;
                        bool next_is_cycle = (order_next == true);
                        if (next_is_cycle) {
                            next = stcactus->new_empty_node();
                        } else {
                            next = B[b_index];
                        }
                        std::fprintf(stderr,
                            "[TRACE-STC-OUT-E] i:%zu j:%zu kind:to_next "
                            "u:%u v:%u w:%lu next_is_cycle:%d previous_before:%u\n",
                            i, j, A[a_index][j], next,
                            (unsigned long)(mincut / 2),
                            (int)next_is_cycle, previous);
                        stcactus->new_edge_order(A[a_index][j], next,
                                                 mincut / 2);
                        std::fprintf(stderr,
                            "[TRACE-STC-OUT-E] i:%zu j:%zu kind:prev_to_next "
                            "u:%u v:%u w:%lu\n",
                            i, j, previous, next,
                            (unsigned long)(mincut / 2));
                        stcactus->new_edge_order(previous, next, mincut / 2);
                        previous = next;
                    }
                }
                a_index++;
                std::fprintf(stderr,
                    "[TRACE-STC-OUT] i:%zu kind:cycle_done a_index_after:%zu "
                    "previous_after:%u\n",
                    i, a_index, previous);
            } else {
                // make ordered vtx
                if (!order_next) {
                    std::fprintf(stderr,
                        "[TRACE-STC-OUT-E] i:%zu kind:tree u:%u v:%u "
                        "w:%lu\n",
                        i, B[b_index], B[b_index + 1],
                        (unsigned long)mincut);
                    stcactus->new_edge_order(B[b_index],
                                             B[b_index + 1], mincut);
                }
                previous = B[b_index];
                b_index++;
                std::fprintf(stderr,
                    "[TRACE-STC-OUT] i:%zu kind:tree_done b_index_after:%zu "
                    "previous_after:%u\n",
                    i, b_index, previous);
            }
        }
        stcactus->finish_construction();
        return stcactus;
    }

    std::tuple<NodeID, EdgeID, NodeID> centralFlowEdge(
        mutableGraphPtr G) {
        NodeID random_vtx = random_functions::nextInt(0, G->n() - 1);
        NodeID v1 = std::get<2>(graph_algorithms::bfsDistances(G, random_vtx));
        auto [parent, distance, v2] = graph_algorithms::bfsDistances(G, v1);
        uint32_t max_distance = distance[v2];
        for (uint32_t d = max_distance; d > (max_distance + 1) / 2; --d) {
            if (distance[v2] != d) {
                LOG1 << distance[v2] << " of " << v2 << " is not " << d;
                exit(1);
            }
            v2 = parent[v2];
        }

        for (EdgeID e : G->edges_of(v2)) {
            if (G->getEdgeTarget(v2, e) == parent[v2]) {
                return std::make_tuple(v2, e, parent[v2]);
            }
        }

        LOG1 << "Central flow edge didn't find an edge!";
        exit(1);
    }

    // [TRACE-RC-FE] maximumFlowEdge (recursive_cactus.h:513-545). T11/T12.
    // Strict `>` ASC iteration; first-encountered max wins.
    std::tuple<NodeID, EdgeID, NodeID> maximumFlowEdge(
        mutableGraphPtr G) {
        NodeWeight max_degree = 0;
        NodeID s = UNDEFINED_NODE;

        for (NodeID n : G->nodes()) {
            NodeWeight d = G->getUnweightedNodeDegree(n);
            bool empty = G->isEmpty(n);
            bool gt = (d > max_degree && !empty);
            if (gt) {
                max_degree = d;
                s = n;
            }
            std::fprintf(stderr,
                "[TRACE-RC-FE] maxFE n:%u deg:%lu max_before:%lu empty:%d "
                "gt:%d s:%u\n",
                n, (unsigned long)d, (unsigned long)max_degree,
                (int)empty, (int)gt, s);
        }

        NodeID t = UNDEFINED_NODE;
        EdgeID e = UNDEFINED_EDGE;
        NodeWeight max_ngbr = 0;
        for (EdgeID edge : G->edges_of(s)) {
            NodeID ngbr = G->getEdgeTarget(s, edge);
            NodeWeight d = G->getUnweightedNodeDegree(ngbr);
            bool empty = G->isEmpty(ngbr);
            bool gt = (d > max_ngbr && !empty);
            if (gt) {
                max_ngbr = d;
                t = ngbr;
                e = edge;
            }
            std::fprintf(stderr,
                "[TRACE-RC-FE] maxFE_ngbr s:%u edge:%lu ngbr:%u deg:%lu "
                "max_before:%lu empty:%d gt:%d t:%u e:%lu\n",
                s, (unsigned long)edge, ngbr, (unsigned long)d,
                (unsigned long)max_ngbr, (int)empty, (int)gt,
                t, (unsigned long)e);
        }

        if (t == UNDEFINED_NODE) {
            LOG1 << "Heaviest vertex has only empty neighbours!";
            return findFlowEdge(G);
        } else {
            return std::make_tuple(s, e, t);
        }
    }

    // [TRACE-RC-WFE] maximumWeightedFlowEdge (recursive_cactus.h:547-579).
    // T13/T14.
    std::tuple<NodeID, EdgeID, NodeID> maximumWeightedFlowEdge(
        mutableGraphPtr G) {
        NodeWeight max_degree = 0;
        NodeID s = UNDEFINED_NODE;

        for (NodeID n : G->nodes()) {
            NodeWeight d = G->getWeightedNodeDegree(n);
            bool empty = G->isEmpty(n);
            bool gt = (d > max_degree && !empty);
            if (gt) {
                max_degree = d;
                s = n;
            }
            std::fprintf(stderr,
                "[TRACE-RC-WFE] maxWFE n:%u wdeg:%lu max_before:%lu "
                "empty:%d gt:%d s:%u\n",
                n, (unsigned long)d, (unsigned long)max_degree,
                (int)empty, (int)gt, s);
        }

        NodeID t = UNDEFINED_NODE;
        EdgeID e = UNDEFINED_EDGE;
        NodeWeight max_ngbr = 0;
        for (EdgeID edge : G->edges_of(s)) {
            NodeID ngbr = G->getEdgeTarget(s, edge);
            NodeWeight d = G->getWeightedNodeDegree(ngbr);
            bool empty = G->isEmpty(ngbr);
            bool gt = (d > max_ngbr && !empty);
            if (gt) {
                max_ngbr = d;
                t = ngbr;
                e = edge;
            }
            std::fprintf(stderr,
                "[TRACE-RC-WFE] maxWFE_ngbr s:%u edge:%lu ngbr:%u "
                "wdeg:%lu max_before:%lu empty:%d gt:%d t:%u e:%lu\n",
                s, (unsigned long)edge, ngbr, (unsigned long)d,
                (unsigned long)max_ngbr, (int)empty, (int)gt,
                t, (unsigned long)e);
        }

        if (t == UNDEFINED_NODE) {
            LOG1 << "Heaviest vertex has only empty neighbours!";
            return findFlowEdge(G);
        } else {
            return std::make_tuple(s, e, t);
        }
    }

    // [TRACE-RC-FFE] findFlowEdge (recursive_cactus.h:583-614). Per-step
    // edge-search decision; cpp RNG draws emitted via [TRACE-RNG] in
    // random_functions::nextInt.
    std::tuple<NodeID, EdgeID, NodeID> findFlowEdge(
        mutableGraphPtr G) {
        NodeID s = random_functions::nextInt(0, G->n() - 1);
        NodeID tgt = 0;
        NodeID max_edge = G->get_first_invalid_edge(s) - 1;
        EdgeID e = random_functions::nextInt(0, max_edge);
        std::fprintf(stderr,
            "[TRACE-RC-FFE] entry s:%u max_edge:%u e:%lu G_n:%u\n",
            s, max_edge, (unsigned long)e, G->n());
        bool edge_found = false;
        size_t step = 0;
        while (!edge_found) {
            while (G->isEmpty(s)) {
                std::fprintf(stderr,
                    "[TRACE-RC-FFE] step:%zu skip_empty_s s:%u\n",
                    step, s);
                if (s + 1 >= G->n()) {
                    s = 0;
                } else {
                    s++;
                }
            }
            e = 0;
            while (e < G->get_first_invalid_edge(s)
                   && (G->isEmpty(G->getEdgeTarget(s, e)))) {
                e++;
            }

            bool fie_ok = (e < G->get_first_invalid_edge(s));
            std::fprintf(stderr,
                "[TRACE-RC-FFE] step:%zu s:%u e:%lu fie:%lu found:%d\n",
                step, s, (unsigned long)e,
                (unsigned long)G->get_first_invalid_edge(s),
                (int)fie_ok);
            if (fie_ok) {
                edge_found = true;
            } else {
                if (s + 1 >= G->n()) {
                    s = 0;
                } else {
                    s++;
                }
            }
            step++;
        }

        tgt = G->getEdgeTarget(s, e);
        std::fprintf(stderr,
            "[TRACE-RC-FFE] exit s:%u e:%lu tgt:%u\n",
            s, (unsigned long)e, tgt);
        return std::make_tuple(s, e, tgt);
    }

    timer t;
    EdgeWeight mincut;
    size_t problem_id;
};
