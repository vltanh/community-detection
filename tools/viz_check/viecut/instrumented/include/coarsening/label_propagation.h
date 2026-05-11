/******************************************************************************
 * label_propagation.h (instrumented sibling)
 *
 * Source of VieCut — sibling fork of
 * VieCut/lib/coarsening/label_propagation.h with [TRACE-LP] stderr probes
 * at the LP body (label_propagation.h:35-90). Per byte-equal-tracer
 * "Extensive printout" discipline: every per-node + per-edge accumulator
 * BEFORE+AFTER, every tie-break decision result.
 *
 * Replaces `random_functions::permutate_vector_local` with the existing
 * harness-traced helper `permutate_vector_local_traced` declared in the
 * instrumented random_functions.h sibling. That helper hand-replicates
 * libstdc++ std::shuffle's optimized branch but routes uint32 draws
 * through random_functions::next() so each draw emits [TRACE-RNG].
 *
 * Behaviour-bit-equal to upstream when probes elide.
 *****************************************************************************/

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <random>
#include <utility>
#include <vector>

#include "common/definitions.h"
#include "data_structure/graph_access.h"
#include "tlx/logger.hpp"
#include "tools/random_functions.h"

template <class GraphPtr>
class label_propagation {
    static constexpr bool debug = false;

 public:
    label_propagation() { }
    virtual ~label_propagation() { }

    std::vector<NodeID> propagate_labels(GraphPtr G) {
        std::fprintf(stderr,
            "[TRACE-LP] entry n:%u\n", G->number_of_nodes());
        timer t;
        std::vector<NodeID> cluster_id(G->number_of_nodes());
        std::vector<NodeID> permutation(G->number_of_nodes());
        bool timing = configuration::getConfig()->verbose;

        std::vector<std::pair<EdgeWeight, NodeID> > hash_vec(
            G->number_of_nodes(), std::make_pair(0, 0));

        for (size_t i = 0; i < cluster_id.size(); ++i) {
            cluster_id[i] = i;
        }

        // Use the traced permutate helper from the instrumented
        // random_functions sibling: routes uint32 draws through next()
        // so each draw emits [TRACE-RNG] in the same order the JS port
        // consumes them.
        random_functions::permutate_vector_local_traced(&permutation, true);
        std::fprintf(stderr,
            "[TRACE-LP] perm_done size:%zu first:%u last:%u\n",
            permutation.size(),
            permutation.empty() ? 0xffffffffu : permutation.front(),
            permutation.empty() ? 0xffffffffu : permutation.back());

        size_t iterations = 2;

        LOG << "Number of iterations: " << iterations;

        for (size_t j = 0; j < iterations; j++) {
            std::fprintf(stderr,
                "[TRACE-LP] iter_start j:%zu\n", j);
            for (NodeID node : G->nodes()) {
                NodeID n = permutation[node];
                PartitionID max_block = cluster_id[n];
                EdgeWeight max_value = 0;
                for (EdgeID e : G->edges_of(n)) {
                    NodeID target = G->getEdgeTarget(n, e);
                    PartitionID block = cluster_id[target];
                    bool stale = (hash_vec[block].second != n);
                    EdgeWeight before = hash_vec[block].first;
                    if (stale) {
                        hash_vec[block].first = 0;
                        hash_vec[block].second = n;
                    }
                    EdgeWeight before_add = hash_vec[block].first;
                    hash_vec[block].first += G->getEdgeWeight(n, e);
                    EdgeWeight after_add = hash_vec[block].first;
                    EdgeWeight mv_before = max_value;
                    PartitionID mb_before = max_block;
                    bool strict_gt = (hash_vec[block].first > max_value);
                    bool eq = (hash_vec[block].first == max_value);
                    bool tie_take = false;
                    bool consumed_next = false;
                    if (strict_gt) {
                        max_value = hash_vec[block].first;
                        max_block = block;
                    } else if (eq) {
                        uint32_t r = random_functions::next();
                        consumed_next = true;
                        tie_take = ((r % 2u) != 0);
                        if (tie_take) {
                            max_value = hash_vec[block].first;
                            max_block = block;
                        }
                        std::fprintf(stderr,
                            "[TRACE-LP-TIE] j:%zu node:%u n:%u e:%lu "
                            "tgt:%u block:%u rng:0x%08x mod2:%u "
                            "take:%d\n",
                            j, node, n, (unsigned long)e, target, block,
                            r, (unsigned)(r % 2u), (int)tie_take);
                    }
                    std::fprintf(stderr,
                        "[TRACE-LP-E] j:%zu node:%u n:%u e:%lu tgt:%u "
                        "block:%u stale:%d hv_before_reset:%lu "
                        "hv_before_add:%lu w:%lu hv_after:%lu mv_before:%lu "
                        "mb_before:%u strict_gt:%d eq:%d consumed_next:%d "
                        "tie_take:%d mv_after:%lu mb_after:%u\n",
                        j, node, n, (unsigned long)e, target, block,
                        (int)stale, (unsigned long)before,
                        (unsigned long)before_add,
                        (unsigned long)G->getEdgeWeight(n, e),
                        (unsigned long)after_add,
                        (unsigned long)mv_before, mb_before,
                        (int)strict_gt, (int)eq, (int)consumed_next,
                        (int)tie_take,
                        (unsigned long)max_value, max_block);
                }
                cluster_id[n] = max_block;
                std::fprintf(stderr,
                    "[TRACE-LP-N] j:%zu node:%u n:%u cluster_id_after:%u\n",
                    j, node, n, cluster_id[n]);
            }
            LOGC(timing) << "LP: Iteration " << j
                         << " -  Timer: " << t.elapsedToZero();
        }

        std::fprintf(stderr,
            "[TRACE-LP] exit n:%u\n", G->number_of_nodes());
        return cluster_id;
    }

    void certify_clusters(
        graphAccessPtr G,
        const std::vector<NodeID>& cluster_id,
        const std::vector<std::vector<NodeID> >& reverse_mapping) {
        for (size_t p = 0; p < reverse_mapping.size(); ++p) {
            std::vector<EdgeWeight> in_block(reverse_mapping[p].size());
            std::vector<EdgeWeight> out(reverse_mapping[p].size());

            for (size_t i = 0; i < reverse_mapping[p].size(); ++i) {
                for (EdgeID e : G->edges_of(reverse_mapping[p][i])) {
                    if (cluster_id[G->getEdgeTarget(e)] == p) {
                        in_block[i] += G->getEdgeWeight(e);
                    } else {
                        out[i] += G->getEdgeWeight(e);
                    }
                }
            }
        }
    }
};
