/******************************************************************************
 * noi_minimum_cut.h (instrumented)
 *
 * Sibling reimpl of VieCut/lib/algorithms/global_mincut/noi_minimum_cut.h with
 * stderr [TRACE-CAP] emission throughout modified_capforest. Capforest is the
 * tie-break-densest phase in the cactus pipeline (audit rows F, I, J, M);
 * end-state-only lockstep cannot localize divergences inside the pop loop,
 * so per-pop + per-edge visibility is required for first-divergent-step
 * localization per byte-equal-tracer "Extensive printout" discipline.
 *
 * Trace prefixes emitted (audit-row tags in [] brackets):
 *   [TRACE-CAP] entry n:%u mincut:%lu                          [N]
 *   [TRACE-CAP] start raw_next:0x%08x mod:%u start:%u          [B,C]
 *   [TRACE-CAP] pop slot:%zu node:%u visited_after:1 r_v:%lu   [M,L]
 *   [TRACE-CAP-EDGE] node:%u e:%lu tgt:%u w:%lu                [M]
 *                    rv_before:%lu rv_lt_mc:%d mc_eq_0:%d      [F, T1]
 *                    increase:%d rv_plus_w:%lu rv_plus_w_ge_mc:%d   [F, T2, M]
 *                    union:%d rv_after:%lu new_rv:%lu          [M, I]
 *                    seen_before:%d pq_op:%s                   [F]
 *   [TRACE-CAP] exit uf_n:%u
 *
 * Probes accumulate monotonically per "Tracer prints stay" discipline.
 * Behaviour-bit-equal to upstream when probes elide.
 *****************************************************************************/

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "algorithms/global_mincut/minimum_cut.h"
#include "algorithms/global_mincut/minimum_cut_helpers.h"
#include "algorithms/multicut/multicut_problem.h"
#include "common/configuration.h"
#include "common/definitions.h"
#include "data_structure/graph_access.h"
#include "data_structure/mutable_graph.h"
#include "data_structure/priority_queues/bucket_pq.h"
#include "data_structure/priority_queues/fifo_node_bucket_pq.h"
#include "data_structure/priority_queues/maxNodeHeap.h"
#include "data_structure/priority_queues/node_bucket_pq.h"
#include "data_structure/priority_queues/vecMaxNodeHeap.h"
#include "tools/random_functions.h"
#include "tools/timer.h"

#ifdef PARALLEL
#include "parallel/coarsening/contract_graph.h"
#include "parallel/coarsening/contraction_tests.h"
#include "parallel/data_structure/union_find.h"
#else
#include "coarsening/contract_graph.h"
#include "coarsening/contraction_tests.h"
#include "data_structure/union_find.h"
#endif

template <class GraphPtr>
class noi_minimum_cut : public minimum_cut {
 public:
    typedef GraphPtr GraphPtrType;
    noi_minimum_cut() { }
    ~noi_minimum_cut() { }
    static constexpr bool debug = false;
    static constexpr bool timing = true;

    EdgeWeight perform_minimum_cut(GraphPtr G) {
        return perform_minimum_cut(G, false);
    }

    EdgeWeight perform_minimum_cut(GraphPtr G, bool indirect) {
        if (!G) {
            return -1;
        }

        std::vector<GraphPtr> graphs;
        timer t;
        EdgeWeight mincut = G->getMinDegree();
        graphs.push_back(G);
        minimum_cut_helpers<GraphPtr>::setInitialCutValues(graphs);

        // [TRACE-NOI] outer perform_minimum_cut entry + per-iter
        // (noi_minimum_cut.h:61-83). Outer-while runs capforest +
        // fromUnionFind until n<=2 or mincut==0.
        std::fprintf(stderr,
            "[TRACE-NOI] outer_entry G_n:%u indirect:%d initial_mincut:%lu\n",
            G->number_of_nodes(), (int)indirect, (unsigned long)mincut);
        int noi_iter = 0;
        while (graphs.back()->number_of_nodes() > 2 && mincut > 0) {
            std::fprintf(stderr,
                "[TRACE-NOI] outer_iter:%d n_before:%u mincut_before:%lu\n",
                noi_iter, graphs.back()->number_of_nodes(),
                (unsigned long)mincut);
            auto uf = modified_capforest(graphs.back(), mincut);
            graphs.emplace_back(
                contraction::fromUnionFind(graphs.back(), &uf, true));
            EdgeWeight mc_pre = mincut;
            mincut = minimum_cut_helpers<GraphPtr>::updateCut(graphs, mincut);
            std::fprintf(stderr,
                "[TRACE-NOI] outer_iter:%d n_after:%u uf_n:%u mincut:%lu "
                "mincut_before:%lu\n",
                noi_iter, graphs.back()->n(), uf.n(),
                (unsigned long)mincut, (unsigned long)mc_pre);
            noi_iter++;
        }
        std::fprintf(stderr,
            "[TRACE-NOI] outer_exit n_final:%u mincut:%lu iters:%d\n",
            graphs.back()->number_of_nodes(),
            (unsigned long)mincut, noi_iter);

        if (!indirect && configuration::getConfig()->save_cut)
            minimum_cut_helpers<GraphPtr>::retrieveMinimumCut(graphs);

        return mincut;
    }

    static priority_queue_interface * selectPq(GraphPtr G,
                                               EdgeWeight mincut) {
        priority_queue_interface* pq;

        size_t max_deg = G->getMaxDegree();

        if (configuration::getConfig()->disable_limiting) {
            mincut = max_deg;
        }

        if (configuration::getConfig()->pq == "default") {
            if (mincut > 10000 && mincut > G->number_of_nodes()) {
                pq = new vecMaxNodeHeap(G->number_of_nodes());
            } else {
                pq = new fifo_node_bucket_pq(G->number_of_nodes(), mincut);
            }
        } else {
            if (configuration::getConfig()->pq == "bqueue") {
                pq = new fifo_node_bucket_pq(G->number_of_nodes(), mincut);
            } else {
                if (configuration::getConfig()->pq == "heap") {
                    pq = new vecMaxNodeHeap(G->number_of_nodes());
                } else {
                    if (configuration::getConfig()->pq == "bstack") {
                        pq = new node_bucket_pq(G->number_of_nodes(), mincut);
                    } else {
                        std::cerr << "unknown pq type "
                                  << configuration::getConfig()->pq
                                  << std::endl;
                        exit(1);
                        pq = new maxNodeHeap();
                    }
                }
            }
        }
        return pq;
    }

    union_find modified_capforest(GraphPtr G,
                                  EdgeWeight mincut) {
        std::fprintf(stderr,
            "[TRACE-CAP] entry n:%u mincut:%lu\n",
            G->number_of_nodes(), (unsigned long)mincut);
        union_find uf(G->number_of_nodes());

        priority_queue_interface* pq = selectPq(G, mincut);

        std::vector<bool> visited(G->number_of_nodes(), false);
        std::vector<bool> seen(G->number_of_nodes(), false);
        std::vector<EdgeWeight> r_v(G->number_of_nodes(), 0);

        // [TRACE-CAP] starting_node draw — emit raw next() + post-image
        // modulus result. Raw next() is also emitted by random_functions
        // via [TRACE-RNG]; this duplicate explicit emission ties the raw
        // draw to its post-modulus node selection.
        uint32_t starting_raw = random_functions::next();
        NodeID starting_node = starting_raw % G->number_of_nodes();
        std::fprintf(stderr,
            "[TRACE-CAP] start mod_n:%u start:%u\n",
            G->number_of_nodes(), starting_node);
        std::fprintf(stderr,
            "[TRACE-CAP] start_raw raw:0x%08x mod_n:%u start:%u\n",
            starting_raw, G->number_of_nodes(), starting_node);

        NodeID current_node = starting_node;

        pq->insert(current_node, 0);

        size_t pop_slot = 0;
        while (!pq->empty()) {
            current_node = pq->deleteMax();
            visited[current_node] = true;
            std::fprintf(stderr,
                "[TRACE-CAP] pop slot:%zu node:%u r_v:%lu seen:%d\n",
                pop_slot, current_node, (unsigned long)r_v[current_node],
                (int)seen[current_node]);
            pop_slot++;
            if (!configuration::getConfig()->disable_limiting) {
                for (EdgeID e : G->edges_of(current_node)) {
                    NodeID tgt = G->getEdgeTarget(current_node, e);
                    if (!visited[tgt]) {
                        bool increase = false;
                        EdgeWeight w = G->getEdgeWeight(current_node, e);
                        EdgeWeight rv_before = r_v[tgt];
                        // T1 boundary: r_v[tgt] < mincut || mincut == 0
                        bool t1_rv_lt_mc = (rv_before < mincut);
                        bool t1_mc_eq_0 = (mincut == 0);
                        bool union_taken = false;
                        EdgeWeight rv_plus_w = rv_before + w;
                        bool t2_ge_mc = (rv_plus_w >= mincut);

                        if (t1_rv_lt_mc || t1_mc_eq_0) {
                            increase = true;
                            if (t2_ge_mc) {
                                uf.Union(current_node, tgt);
                                union_taken = true;
                            }
                        }

                        r_v[tgt] += w;
                        EdgeWeight rv_after = r_v[tgt];

                        size_t new_rv = std::min((EdgeWeight)r_v[tgt],
                                                 mincut);

                        bool seen_before = seen[tgt];
                        const char* pq_op = "none";
                        if (seen[tgt]) {
                            if (increase && !visited[tgt]) {
                                pq->increaseKey(tgt, new_rv);
                                pq_op = "increaseKey";
                            }
                        } else {
                            seen[tgt] = true;
                            pq->insert(tgt, new_rv);
                            pq_op = "insert";
                        }

                        std::fprintf(stderr,
                            "[TRACE-CAP-EDGE] node:%u e:%lu tgt:%u w:%lu "
                            "rv_before:%lu t1_lt:%d t1_mc0:%d increase:%d "
                            "rv_plus_w:%lu t2_ge:%d union:%d "
                            "rv_after:%lu new_rv:%lu seen_before:%d "
                            "pq_op:%s\n",
                            current_node, (unsigned long)e, tgt,
                            (unsigned long)w, (unsigned long)rv_before,
                            (int)t1_rv_lt_mc, (int)t1_mc_eq_0, (int)increase,
                            (unsigned long)rv_plus_w, (int)t2_ge_mc,
                            (int)union_taken, (unsigned long)rv_after,
                            (unsigned long)new_rv, (int)seen_before, pq_op);
                    }
                }
            } else {
                for (EdgeID e : G->edges_of(current_node)) {
                    NodeID tgt = G->getEdgeTarget(current_node, e);

                    if (!visited[tgt]) {
                        EdgeWeight w = G->getEdgeWeight(current_node, e);
                        EdgeWeight rv_before = r_v[tgt];
                        bool t1_rv_lt_mc = (rv_before < mincut);
                        bool union_taken = false;
                        EdgeWeight rv_plus_w = rv_before + w;
                        bool t2_ge_mc = (rv_plus_w >= mincut);

                        if (t1_rv_lt_mc) {
                            if (t2_ge_mc) {
                                uf.Union(current_node, tgt);
                                union_taken = true;
                            }
                        }

                        r_v[tgt] += w;
                        EdgeWeight rv_after = r_v[tgt];

                        bool seen_before = seen[tgt];
                        const char* pq_op = "none";
                        if (seen[tgt]) {
                            if (!visited[tgt]) {
                                pq->increaseKey(tgt, r_v[tgt]);
                                pq_op = "increaseKey";
                            }
                        } else {
                            seen[tgt] = true;
                            pq->insert(tgt, r_v[tgt]);
                            pq_op = "insert";
                        }

                        std::fprintf(stderr,
                            "[TRACE-CAP-EDGE] node:%u e:%lu tgt:%u w:%lu "
                            "rv_before:%lu t1_lt:%d t1_mc0:0 increase:%d "
                            "rv_plus_w:%lu t2_ge:%d union:%d "
                            "rv_after:%lu new_rv:%lu seen_before:%d "
                            "pq_op:%s\n",
                            current_node, (unsigned long)e, tgt,
                            (unsigned long)w, (unsigned long)rv_before,
                            (int)t1_rv_lt_mc, (int)t1_rv_lt_mc,
                            (unsigned long)rv_plus_w, (int)t2_ge_mc,
                            (int)union_taken, (unsigned long)rv_after,
                            (unsigned long)rv_after, (int)seen_before, pq_op);
                    }
                }
            }
        }
        delete pq;
        std::fprintf(stderr,
            "[TRACE-CAP] exit uf_n:%u\n", uf.n());
        return uf;
    }
};
