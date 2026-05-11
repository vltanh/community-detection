/******************************************************************************
 * strongly_connected_components.h (instrumented)
 *
 * Sibling reimpl of VieCut/lib/.../strongly_connected_components.h.
 * Emits [TRACE-SCC] stderr lines:
 *   - one line per assigned comp_num (node:N comp:C)
 *   - one summary line at end (count + blocksizes)
 *****************************************************************************/

#pragma once

#include <cstdio>
#include <memory>
#include <stack>
#include <tuple>
#include <utility>
#include <vector>

#include "common/definitions.h"
#include "data_structure/graph_access.h"
#include "data_structure/mutable_graph.h"
#include "tools/graph_extractor.h"

class strongly_connected_components {
 public:
    static const bool debug = false;

    strongly_connected_components() { }
    virtual ~strongly_connected_components() { }

    std::tuple<std::vector<int>, size_t, std::vector<size_t> >
    strong_components(mutableGraphPtr G, size_t fpid = UNDEFINED_NODE) {
        m_dfsnum.resize(G->number_of_nodes());
        m_comp_num.resize(G->number_of_nodes());
        m_dfscount = 0;
        m_comp_count = 0;
        m_fpid = fpid;
        m_blocksizes.clear();

        for (NodeID node : G->nodes()) {
            m_comp_num[node] = -1;
            m_dfsnum[node] = -1;
        }

        std::fprintf(stderr,
            "[TRACE-SCC] start n:%u fpid:%lu\n",
            G->number_of_nodes(), (unsigned long)fpid);

        for (NodeID node : G->nodes()) {
            if (m_dfsnum[node] == -1) {
                explicit_scc_dfs(node, G);
            }
        }

        std::fprintf(stderr, "[TRACE-SCC] done count:%zu", m_comp_count);
        for (size_t i = 0; i < m_blocksizes.size(); i++) {
            std::fprintf(stderr, " bs[%zu]:%zu", i, m_blocksizes[i]);
        }
        std::fprintf(stderr, "\n");
        for (NodeID n = 0; n < m_comp_num.size(); n++) {
            std::fprintf(stderr, "[TRACE-SCC] comp_num[%u]:%d\n",
                         n, m_comp_num[n]);
        }

        return std::make_tuple(m_comp_num, m_comp_count, m_blocksizes);
    }

    size_t strong_components(graphAccessPtr G,
                             std::vector<int>* cn) {
        std::vector<int>& comp_num = *cn;
        m_dfsnum.resize(G->number_of_nodes());
        m_comp_num.resize(G->number_of_nodes());
        m_dfscount = 0;
        m_comp_count = 0;

        for (NodeID node : G->nodes()) {
            m_comp_num[node] = -1;
            m_dfsnum[node] = -1;
        }

        for (NodeID node : G->nodes()) {
            if (m_dfsnum[node] == -1) {
                explicit_scc_dfs(node, G);
            }
        }

        for (NodeID node : G->nodes()) {
            comp_num[node] = m_comp_num[node];
        }

        return m_comp_count;
    }

    void explicit_scc_dfs(NodeID node, mutableGraphPtr G) {
        iteration_stack.push(
            std::pair<NodeID, EdgeID>(node, G->get_first_edge(node)));

        m_dfsnum[node] = m_dfscount++;
        m_unfinished.push(node);
        m_roots.push(node);
        std::fprintf(stderr,
            "[TRACE-SCC-S] root_push node:%u dfsnum:%d unfinished_n:%zu "
            "roots_n:%zu\n",
            node, m_dfsnum[node], m_unfinished.size(), m_roots.size());

        while (!iteration_stack.empty()) {
            NodeID current_node = iteration_stack.top().first;
            EdgeID current_edge = iteration_stack.top().second;
            iteration_stack.pop();
            std::fprintf(stderr,
                "[TRACE-SCC-S] frame current:%u current_edge:%lu "
                "stack_n:%zu\n",
                current_node, (unsigned long)current_edge,
                iteration_stack.size());

            bool recursed = false;
            for (EdgeID e : G->edges_of_starting_at(current_node,
                                                    current_edge)) {
                FlowType ef;
                FlowType ew = static_cast<FlowType>(
                    G->getEdgeWeight(current_node, e));
                if (m_fpid == UNDEFINED_NODE) {
                    ef = G->getEdgeFlow(current_node, e);
                } else {
                    ef = G->getEdgeFlow(current_node, e, m_fpid);
                }
                bool full = (ef == ew);
                if (full) {
                    std::fprintf(stderr,
                        "[TRACE-SCC-S] edge current:%u e:%lu skip:full "
                        "ef:%ld ew:%ld\n",
                        current_node, (unsigned long)e,
                        (long)ef, (long)ew);
                    continue;
                }
                NodeID target = G->getEdgeTarget(current_node, e);
                int dfsnum_t = m_dfsnum[target];
                int comp_t = m_comp_num[target];
                std::fprintf(stderr,
                    "[TRACE-SCC-S] edge current:%u e:%lu target:%u "
                    "dfsnum_t:%d comp_t:%d\n",
                    current_node, (unsigned long)e, target,
                    dfsnum_t, comp_t);
                if (dfsnum_t == -1) {
                    iteration_stack.push(std::pair<NodeID, EdgeID>(
                                             current_node, e));
                    iteration_stack.push(std::pair<NodeID, EdgeID>(target, 0));
                    m_dfsnum[target] = m_dfscount++;
                    m_unfinished.push(target);
                    m_roots.push(target);
                    std::fprintf(stderr,
                        "[TRACE-SCC-S] descend target:%u new_dfsnum:%d "
                        "stack_n:%zu unfinished_n:%zu roots_n:%zu\n",
                        target, m_dfsnum[target],
                        iteration_stack.size(), m_unfinished.size(),
                        m_roots.size());
                    recursed = true;
                    break;
                } else if (comp_t == -1) {
                    size_t pops = 0;
                    while (m_dfsnum[m_roots.top()] > m_dfsnum[target]) {
                        m_roots.pop();
                        pops++;
                    }
                    std::fprintf(stderr,
                        "[TRACE-SCC-S] back_edge target:%u pops:%zu "
                        "roots_top_after:%u\n",
                        target, pops,
                        m_roots.empty() ? 0xffffffffu : m_roots.top());
                }
            }
            (void)recursed;

            bool root_match = (current_node == m_roots.top());
            std::fprintf(stderr,
                "[TRACE-SCC-S] root_check current:%u roots_top:%u match:%d\n",
                current_node, m_roots.top(), (int)root_match);
            if (root_match) {
                NodeID w = 0;
                m_blocksizes.emplace_back();
                do {
                    w = m_unfinished.top();
                    m_unfinished.pop();
                    m_comp_num[w] = m_comp_count;
                    m_blocksizes[m_comp_count]++;
                    std::fprintf(stderr,
                        "[TRACE-SCC-S] commit w:%u comp:%zu blocksize:%zu\n",
                        w, m_comp_count, m_blocksizes[m_comp_count]);
                } while (w != current_node);
                m_comp_count++;
                m_roots.pop();
                std::fprintf(stderr,
                    "[TRACE-SCC-S] comp_done count:%zu roots_n:%zu "
                    "unfinished_n:%zu\n",
                    m_comp_count, m_roots.size(), m_unfinished.size());
            }
        }
    }

    void explicit_scc_dfs(NodeID node, graphAccessPtr G) {
        iteration_stack.push(std::pair<NodeID, EdgeID>(
                                 node, G->get_first_edge(node)));
        m_dfsnum[node] = m_dfscount++;
        m_unfinished.push(node);
        m_roots.push(node);

        while (!iteration_stack.empty()) {
            NodeID current_node = iteration_stack.top().first;
            EdgeID current_edge = iteration_stack.top().second;
            iteration_stack.pop();

            for (EdgeID e : G->edges_of_starting_at(current_node,
                                                    current_edge)) {
                NodeID target = G->getEdgeTarget(e);
                if (m_dfsnum[target] == -1) {
                    iteration_stack.push(std::pair<NodeID, EdgeID>(
                                             current_node, e));
                    iteration_stack.push(std::pair<NodeID, EdgeID>(
                                             target, G->get_first_edge(
                                                 target)));
                    m_dfsnum[target] = m_dfscount++;
                    m_unfinished.push(target);
                    m_roots.push(target);
                    break;
                } else if (m_comp_num[target] == -1) {
                    while (m_dfsnum[m_roots.top()] > m_dfsnum[target])
                        m_roots.pop();
                }
            }

            if (current_node == m_roots.top()) {
                NodeID w = 0;
                do {
                    w = m_unfinished.top();
                    m_unfinished.pop();
                    m_comp_num[w] = m_comp_count;
                } while (w != current_node);
                m_comp_count++;
                m_roots.pop();
            }
        }
    }

    graphAccessPtr largest_scc(graphAccessPtr G) {
        std::vector<int32_t> components(G->number_of_nodes());
        auto ct = strong_components(G, &components);

        std::vector<uint64_t> compsizes(static_cast<uint64_t>(ct));
        for (int32_t component : components) {
            ++compsizes[component];
        }

        auto max_size = std::max_element(compsizes.begin(), compsizes.end());
        int max_comp = static_cast<int>(max_size - compsizes.begin());

        for (NodeID n : G->nodes()) {
            if (components[n] == max_comp) {
                G->setPartitionIndex(n, 0);
            } else {
                G->setPartitionIndex(n, 1);
            }
        }

        graph_extractor ge;
        return ge.extract_block(G, 0).first;
    }

 private:
    int32_t m_dfscount;
    size_t m_comp_count;
    size_t m_fpid;

    std::vector<int> m_dfsnum;
    std::vector<int> m_comp_num;
    std::vector<size_t> m_blocksizes;
    std::stack<NodeID> m_unfinished;
    std::stack<NodeID> m_roots;
    std::stack<std::pair<NodeID, EdgeID> > iteration_stack;
};
