/******************************************************************************
 * viecut.h (instrumented sibling)
 *
 * Source of VieCut — sibling fork of
 * VieCut/lib/algorithms/global_mincut/viecut.h with [TRACE-VC] stderr
 * probes at the LP-loop entry/exit + per-iteration sub-phase boundaries
 * (viecut.h:54-120). The LP-loop fires only on n>10000; per byte-equal-
 * tracer "Extensive printout" discipline, every per-iter scalar is
 * emitted so cross-side bisection localizes drift to a single sub-phase.
 *
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
#include <vector>

#include "algorithms/flow/excess_scaling.h"
#include "algorithms/global_mincut/minimum_cut.h"
#include "algorithms/global_mincut/minimum_cut_helpers.h"
#include "algorithms/global_mincut/noi_minimum_cut.h"
#include "algorithms/misc/strongly_connected_components.h"
#include "common/definitions.h"
#include "data_structure/flow_graph.h"
#include "data_structure/graph_access.h"
#include "tlx/logger.hpp"
#include "tools/graph_extractor.h"
#include "tools/timer.h"

#ifdef PARALLEL
#include "parallel/coarsening/contract_graph.h"
#include "parallel/coarsening/contraction_tests.h"
#include "parallel/coarsening/label_propagation.h"
#else
#include "coarsening/contract_graph.h"
#include "coarsening/contraction_tests.h"
#include "coarsening/label_propagation.h"
#endif

template <class GraphPtr>
class viecut : public minimum_cut {
 public:
    typedef GraphPtr GraphPtrType;
    static constexpr bool debug = false;
    bool timing = configuration::getConfig()->verbose;
    viecut() { }

    virtual ~viecut() { }

    EdgeWeight perform_minimum_cut(GraphPtr G) {
        return perform_minimum_cut(G, false);
    }

    EdgeWeight perform_minimum_cut(GraphPtr G,
                                   bool indirect) {
        if (!G) {
            return -1;
        }

        EdgeWeight cut = G->getMinDegree();
        std::vector<GraphPtr> graphs;
        graphs.push_back(G);

        minimum_cut_helpers<GraphPtr>::setInitialCutValues(graphs);
        std::fprintf(stderr,
            "[TRACE-VC] lp_loop_enter n:%u min_deg:%lu\n",
            graphs.back()->number_of_nodes(), (unsigned long)cut);

        int lp_iter = 0;
        while (graphs.back()->number_of_nodes() > 10000 &&
               (graphs.size() == 1 ||
                (graphs.back()->number_of_nodes() <
                 graphs[graphs.size() - 2]->number_of_nodes()))) {
            std::fprintf(stderr,
                "[TRACE-VC] lp_iter_start iter:%d n:%u m:%lu cut:%lu\n",
                lp_iter,
                graphs.back()->number_of_nodes(),
                (unsigned long)graphs.back()->number_of_edges(),
                (unsigned long)cut);
            timer t;
            G = graphs.back();
            label_propagation<GraphPtr> lp;
            std::vector<NodeID> cluster_mapping = lp.propagate_labels(G);
            std::fprintf(stderr,
                "[TRACE-VC] lp_after_propagate iter:%d cluster_map_n:%zu\n",
                lp_iter, cluster_mapping.size());
            auto [mapping, reverse_mapping] =
                minimum_cut_helpers<GraphPtr>::remap_cluster(
                    G, cluster_mapping);
            std::fprintf(stderr,
                "[TRACE-VC] lp_remap iter:%d mapping_n:%zu rev_n:%zu\n",
                lp_iter, mapping.size(), reverse_mapping.size());

            EdgeWeight cut_before_tc = cut;
            contraction::findTrivialCuts(G, &mapping, &reverse_mapping, cut);
            std::fprintf(stderr,
                "[TRACE-VC] lp_trivial iter:%d cut_before:%lu cut_after:%lu\n",
                lp_iter, (unsigned long)cut_before_tc, (unsigned long)cut);

            auto H = contraction::contractGraph(G, mapping, reverse_mapping);
            graphs.push_back(H);
            EdgeWeight cut_before_uc = cut;
            cut = minimum_cut_helpers<GraphPtr>::updateCut(graphs, cut);
            std::fprintf(stderr,
                "[TRACE-VC] lp_contracted iter:%d n_after:%u cut_before:%lu "
                "cut:%lu\n",
                lp_iter,
                graphs.back()->number_of_nodes(),
                (unsigned long)cut_before_uc, (unsigned long)cut);

            union_find uf = tests::prTests12(graphs.back(), cut);
            std::fprintf(stderr,
                "[TRACE-VC] lp_pr12 iter:%d uf_n:%u\n",
                lp_iter, uf.n());
            graphs.push_back(
                contraction::fromUnionFind(graphs.back(), &uf, true));
            cut = minimum_cut_helpers<GraphPtr>::updateCut(graphs, cut);
            union_find uf2 = tests::prTests34(graphs.back(), cut);
            std::fprintf(stderr,
                "[TRACE-VC] lp_pr34 iter:%d uf_n:%u\n",
                lp_iter, uf2.n());
            graphs.push_back(
                contraction::fromUnionFind(graphs.back(), &uf2, true));
            cut = minimum_cut_helpers<GraphPtr>::updateCut(graphs, cut);
            std::fprintf(stderr,
                "[TRACE-VC] lp_iter_end iter:%d n_after:%u cut:%lu\n",
                lp_iter, graphs.back()->number_of_nodes(),
                (unsigned long)cut);
            lp_iter++;
        }

        std::fprintf(stderr,
            "[TRACE-VC] lp_loop_exit iters:%d n_final:%u cut:%lu\n",
            lp_iter, graphs.back()->number_of_nodes(),
            (unsigned long)cut);

        if (graphs.back()->number_of_nodes() > 1) {
            timer t;
            noi_minimum_cut<GraphPtr> noi;
            cut = std::min(cut, noi.perform_minimum_cut(graphs.back(), true));
            std::fprintf(stderr,
                "[TRACE-VC] noi_final cut:%lu\n", (unsigned long)cut);
        }

        if (!indirect && configuration::getConfig()->save_cut)
            minimum_cut_helpers<GraphPtr>::retrieveMinimumCut(graphs);

        return cut;
    }
};
