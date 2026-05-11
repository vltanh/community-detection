// CC kernel cross-check, instrumented C++ leg.
//
// Verbatim copy of the CC pipeline in constrained-clustering:
//   src/mincut_only.cpp:4-46          (MincutOnly::main, Simple branch)
//   includes/constrained.h:393-419    (GetConnectedComponents) - shared
//   includes/constrained.h:122-151    (RemoveInterClusterEdges) - shared
//   src/constrained.cpp:32-104        (Get/LoadEdges, InvertMap) - shared
//   includes/constrained.h:71-97      (ReadCommunities) - shared
//   src/constrained.cpp:135-152       (WriteClusterQueue<vector<int>>)
//   external_libs/igraph/src/connectivity/components.c:95-201
//                                     (igraph_i_connected_components_weak
//                                      verbatim port, instrumented locally
//                                      so per-BFS-step probes are visible;
//                                      upstream igraph stays untouched.)
//
// Shared helpers live in ../../_common/tracer_io.h. CC-specific
// instrumentation (the BFS root open + expand + neighbor + membership
// + singleton-filter + bucket-fill + cluster-emit probes) lives in
// this TU so neither the upstream igraph nor sibling tracers (WCC/CM)
// see the [TRACE-CC ...] emissions.
//
// Build: ./build.sh -> /tmp/cc_kernel_check_{canonical,swapped}
// Run:   /tmp/cc_kernel_check_canonical <edge.csv> <com.csv> <out.csv>
// stdout: WriteClusterQueue body + JSON trace.
// stderr: [TRACE-CC ...] structured log. Probe tags:
//   PIPELINE_START / PIPELINE_END     - run bookends.
//   loaded n=... m=...                - post-LoadEdgesFromFile summary.
//   ReadCommunities ...               - post-ReadCommunities summary.
//   RIE_EDGE from=.. to=.. ca=.. cb=.. kept=..  - per-edge keep/drop
//                                       in RemoveInterClusterEdges.
//   RIE_SUM kept=... removed=...      - post-loop counters (back-compat).
//   CCW_ROOT first=... comp_id=... size=1
//                                     - per BFS root open
//                                       (components.c:144-157).
//   CCW_NEIGHS act=... count=... ids=[..]
//                                     - per popped-node neighbor list
//                                       (components.c:161-163).
//   CCW_VISIT act=... pop_pos=... nei=... already=... comp_id=...
//                                     - per neighbor (components.c:167).
//   CCW_MEMBER node=... comp_id=... source=root|expand
//                                     - per membership write
//                                       (components.c:155 / :174).
//   CCW_COMP_DONE comp_id=... size=... first=... last=...
//                                     - per FINALIZE_COMP
//                                       (components.c:179-182).
//   BUCKET node_id=... comp_id=... cs=... kept=...
//                                     - per node in the bucket-fill loop
//                                       (constrained.h:403-411).
//   COMP_FLUSH comp_id=... size=... first=... last=...
//                                     - per std::map iteration
//                                       (constrained.h:415-417).
//   DONE_QUEUE size=...               - post-Simple-branch enqueue count.
//   WCQ_POP cluster_id=... size=... first=... last=...
//                                     - per WriteClusterQueue FIFO pop
//                                       (constrained.cpp:135-152).
//   WriteClusterQueue out_clusters=.. - post-loop counter (back-compat).
//
// Skill-conformant compile-time toggle (per playbook §1):
//   -DCANONICAL_MODE -> tracer_canonical: original RNG/FP (vacuous for CC).
//   -DTRACER_MODE    -> tracer_swapped:   bridged RNG/FP  (vacuous for CC).
// CC has no RNG/FP on Simple branch, so both binaries are byte-identical.
// Toggle is here for skill conformance + future extension symmetry with
// VieCut/WCC/CM.
#if !defined(CANONICAL_MODE) && !defined(TRACER_MODE)
#error "must define -DCANONICAL_MODE or -DTRACER_MODE"
#endif
#if defined(CANONICAL_MODE) && defined(TRACER_MODE)
#error "exactly one of CANONICAL_MODE / TRACER_MODE must be defined"
#endif

#include <cstdio>
#include <fstream>
#include <iostream>
#include <queue>
#include <sstream>
#include <string>
#include <vector>
#include "tracer_io.h"

using namespace viz_check;

#ifdef TRACER_MODE
static constexpr const char* k_build_mode = "TRACER_MODE";
#else
static constexpr const char* k_build_mode = "CANONICAL_MODE";
#endif

// [UPSTREAM constrained.h:122-151] RemoveInterClusterEdges, per-edge probed.
// Mirrors the canonical loop verbatim but emits one [TRACE-CC] RIE_EDGE
// record per iteration of the igraph_ess_all(IGRAPH_EDGEORDER_ID) walk so
// the keep/drop decision is auditable for every edge in CSV order.
static void RemoveInterClusterEdgesProbed(
    igraph_t* g, const std::map<int,int>& m)
{
    igraph_vector_int_t rm;
    igraph_vector_int_init(&rm, 0);
    igraph_eit_t eit;
    igraph_eit_create(g, igraph_ess_all(IGRAPH_EDGEORDER_ID), &eit);
    for (; !IGRAPH_EIT_END(eit); IGRAPH_EIT_NEXT(eit)) {
        igraph_integer_t e = IGRAPH_EIT_GET(eit);
        int from = IGRAPH_FROM(g, e);
        int to   = IGRAPH_TO(g, e);
        bool from_has = m.contains(from);
        bool to_has   = m.contains(to);
        int ca = from_has ? m.at(from) : -2147483648;
        int cb = to_has   ? m.at(to)   : -2147483648;
        bool kept = (from_has && to_has && ca == cb);
        if (!kept) {
            igraph_vector_int_push_back(&rm, e);
        }
        // [UPSTREAM constrained.h:128-144] per-edge keep/drop record.
        // Closes P0 gap #8 (per-edge from/to/ca/cb/kept_flag).
        fprintf(stderr,
                "[TRACE-CC] RIE_EDGE from=%d to=%d ca=%d cb=%d kept=%d\n",
                from, to, ca, cb, kept ? 1 : 0);
    }
    igraph_es_t es;
    igraph_es_vector_copy(&es, &rm);
    igraph_delete_edges(g, es);
    igraph_eit_destroy(&eit);
    igraph_es_destroy(&es);
    igraph_vector_int_destroy(&rm);
}

// [UPSTREAM external_libs/igraph/src/connectivity/components.c:95-201]
// Verbatim port of igraph_i_connected_components_weak with per-BFS-step
// probes. Calls public igraph APIs (vcount, neighbors, dqueue, bitset);
// no upstream patch.
//
// Probe set:
//   CCW_ROOT     - components.c:144-157 outer for; per first_node open.
//   CCW_NEIGHS   - components.c:161-163 igraph_neighbors call.
//   CCW_VISIT    - components.c:165-167 per-neighbor scan.
//   CCW_MEMBER   - components.c:155 (root) + :174 (expand) membership writes.
//   CCW_COMP_DONE- components.c:179-182 FINALIZE_COMP.
static void ConnectedComponentsWeakProbed(
    igraph_t* g, igraph_vector_int_t* membership,
    igraph_vector_int_t* csize, igraph_int_t* no)
{
    igraph_int_t no_of_nodes = igraph_vcount(g);
    igraph_int_t no_of_components;
    igraph_bitset_t already_added;
    igraph_dqueue_int_t q;
    igraph_vector_int_t neis;

    igraph_vector_int_resize(membership, no_of_nodes);
    igraph_vector_int_clear(csize);

    igraph_bitset_init(&already_added, no_of_nodes);
    igraph_dqueue_int_init(&q, no_of_nodes > 100000 ? 10000 : no_of_nodes / 10);
    igraph_vector_int_init(&neis, 0);

    no_of_components = 0;
    for (igraph_int_t first_node = 0; first_node < no_of_nodes; ++first_node) {
        if (IGRAPH_BIT_TEST(already_added, first_node)) {
            continue;
        }
        IGRAPH_BIT_SET(already_added, first_node);
        igraph_int_t act_component_size = 1;
        VECTOR(*membership)[first_node] = no_of_components;
        // [UPSTREAM components.c:144-157] root open.
        // Closes P0 gap #1 + #4 (root-side membership write).
        fprintf(stderr,
                "[TRACE-CC] CCW_ROOT first=%lld comp_id=%lld size=1\n",
                (long long)first_node, (long long)no_of_components);
        fprintf(stderr,
                "[TRACE-CC] CCW_MEMBER node=%lld comp_id=%lld source=root\n",
                (long long)first_node, (long long)no_of_components);
        igraph_dqueue_int_push(&q, first_node);

        // Track pop position within this component so per-VISIT probes
        // expose the FIFO walk order (components.c:159-177).
        igraph_int_t pop_pos = 0;
        igraph_int_t first_in_comp = first_node;
        igraph_int_t last_in_comp  = first_node;

        while (!igraph_dqueue_int_empty(&q)) {
            igraph_int_t act_node = igraph_dqueue_int_pop(&q);
            igraph_neighbors(g, &neis, act_node,
                             IGRAPH_ALL, IGRAPH_NO_LOOPS, IGRAPH_MULTIPLE);
            igraph_int_t nei_count = igraph_vector_int_size(&neis);

            // [UPSTREAM components.c:161-163] neighbor list dump.
            // Closes P0 gap #3 (igraph_neighbors ordering).
            {
                std::ostringstream ss;
                ss << "[";
                for (igraph_int_t i = 0; i < nei_count; i++) {
                    if (i) ss << ",";
                    ss << (long long)VECTOR(neis)[i];
                }
                ss << "]";
                fprintf(stderr,
                        "[TRACE-CC] CCW_NEIGHS act=%lld count=%lld ids=%s\n",
                        (long long)act_node, (long long)nei_count,
                        ss.str().c_str());
            }

            for (igraph_int_t i = 0; i < nei_count; i++) {
                igraph_int_t neighbor = VECTOR(neis)[i];
                bool already = IGRAPH_BIT_TEST(already_added, neighbor);
                // [UPSTREAM components.c:165-167] per-neighbor probe.
                // Closes P0 gap #2 (per-neighbor scan visibility).
                fprintf(stderr,
                        "[TRACE-CC] CCW_VISIT act=%lld pop_pos=%lld nei=%lld "
                        "already=%d comp_id=%lld\n",
                        (long long)act_node, (long long)pop_pos,
                        (long long)neighbor, already ? 1 : 0,
                        (long long)no_of_components);
                if (already) {
                    continue;
                }
                igraph_dqueue_int_push(&q, neighbor);
                IGRAPH_BIT_SET(already_added, neighbor);
                act_component_size++;
                VECTOR(*membership)[neighbor] = no_of_components;
                // [UPSTREAM components.c:173-175] expand-side membership.
                // Closes P0 gap #4 (per-node membership write).
                fprintf(stderr,
                        "[TRACE-CC] CCW_MEMBER node=%lld comp_id=%lld "
                        "source=expand\n",
                        (long long)neighbor, (long long)no_of_components);
                last_in_comp = neighbor;
            }
            pop_pos++;
        }

        // [UPSTREAM components.c:179-182] finalize component.
        fprintf(stderr,
                "[TRACE-CC] CCW_COMP_DONE comp_id=%lld size=%lld first=%lld "
                "last=%lld\n",
                (long long)no_of_components, (long long)act_component_size,
                (long long)first_in_comp, (long long)last_in_comp);
        no_of_components++;
        igraph_vector_int_push_back(csize, act_component_size);
    }

    *no = no_of_components;
    igraph_bitset_destroy(&already_added);
    igraph_dqueue_int_destroy(&q);
    igraph_vector_int_destroy(&neis);
}

// [UPSTREAM constrained.h:393-419] GetConnectedComponents, instrumented.
// Mirrors the canonical bucket-fill verbatim but emits one [TRACE-CC]
// BUCKET record per node and one COMP_FLUSH per std::map iterator step.
static std::vector<std::vector<int>>
GetConnectedComponentsProbed(igraph_t* g)
{
    std::vector<std::vector<int>> out;
    std::map<int, std::vector<int>> bucket;
    igraph_vector_int_t cid; igraph_vector_int_init(&cid, 0);
    igraph_vector_int_t sz;  igraph_vector_int_init(&sz, 0);
    igraph_int_t nc;
    ConnectedComponentsWeakProbed(g, &cid, &sz, &nc);

    // [UPSTREAM constrained.h:403-411] bucket-fill loop.
    // Closes P0 gap #6 (per-node bucket-push) and #5 (per-cid singleton
    // keep/drop visible via `kept` flag).
    for (int n = 0; n < igraph_vcount(g); n++) {
        int c = VECTOR(cid)[n];
        int cs = VECTOR(sz)[c];
        bool kept = (cs > 1);
        if (kept) {
            bucket[c].push_back(n);
        }
        fprintf(stderr,
                "[TRACE-CC] BUCKET node_id=%d comp_id=%d cs=%d kept=%d\n",
                n, c, cs, kept ? 1 : 0);
    }
    igraph_vector_int_destroy(&cid);
    igraph_vector_int_destroy(&sz);

    // [UPSTREAM constrained.h:415-417] std::map iteration.
    for (auto& [c, members] : bucket) {
        // Closes P0 gap #6 follow-on: post-bucket per-component flush record.
        fprintf(stderr,
                "[TRACE-CC] COMP_FLUSH comp_id=%d size=%zu first=%d last=%d\n",
                c, members.size(),
                members.empty() ? -1 : members.front(),
                members.empty() ? -1 : members.back());
        out.push_back(std::move(members));
    }
    return out;
}

// [UPSTREAM constrained.cpp:135-152] WriteClusterQueue (CC + WCC variant).
// Instrumented: one [TRACE-CC] WCQ_POP record per FIFO pop showing the
// cluster_id assignment and the contents window (first/last node ids).
// Closes P0 gap #7 (per-pop cluster_id + contents).
static void WriteClusterQueueProbed(std::queue<std::vector<int>>& q,
                                    const std::map<int, std::string>& new_to_orig,
                                    const std::string& output_file) {
    std::ofstream out(output_file);
    int current_cluster_id = 0;
    out << "node_id,cluster_id\n";
    while (!q.empty()) {
        auto cur = q.front(); q.pop();
        for (int new_id : cur) {
            out << new_to_orig.at(new_id) << "," << current_cluster_id << "\n";
        }
        fprintf(stderr,
                "[TRACE-CC] WCQ_POP cluster_id=%d size=%zu first=%d last=%d\n",
                current_cluster_id, cur.size(),
                cur.empty() ? -1 : cur.front(),
                cur.empty() ? -1 : cur.back());
        current_cluster_id++;
    }
    fprintf(stderr,
            "[TRACE-CC] WriteClusterQueue out_clusters=%d\n",
            current_cluster_id);
}

// [UPSTREAM mincut_only.cpp:4-46] MincutOnly::main, Simple branch
int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr, "usage: %s <edge.csv> <com.csv> <out.csv>\n", argv[0]);
        return 2;
    }
    std::string edge_csv = argv[1];
    std::string com_csv = argv[2];
    std::string out_csv = argv[3];

    fprintf(stderr, "[TRACE-CC] PIPELINE_START edge=%s com=%s build=%s\n",
            edge_csv.c_str(), com_csv.c_str(), k_build_mode);

    // mincut_only.cpp:18-22
    auto orig_to_new = GetOriginalToNewIdMap(edge_csv);
    auto new_to_orig = InvertMap(orig_to_new);
    igraph_t graph;
    igraph_empty(&graph, 0, IGRAPH_UNDIRECTED);
    LoadEdgesFromFile(&graph, edge_csv, orig_to_new);
    fprintf(stderr, "[TRACE-CC] loaded n=%zu m=%lld\n",
            orig_to_new.size(), (long long)igraph_ecount(&graph));

    // mincut_only.cpp:29
    auto partition = ReadCommunities(orig_to_new, com_csv);
    fprintf(stderr, "[TRACE-CC] ReadCommunities assigned=%zu / map=%zu\n",
            partition.size(), orig_to_new.size());

    // mincut_only.cpp:35
    int m_before = igraph_ecount(&graph);
    RemoveInterClusterEdgesProbed(&graph, partition);
    fprintf(stderr, "[TRACE-CC] RIE_SUM kept=%lld removed=%d\n",
            (long long)igraph_ecount(&graph),
            m_before - (int)igraph_ecount(&graph));

    // mincut_only.cpp:40
    auto components = GetConnectedComponentsProbed(&graph);
    fprintf(stderr, "[TRACE-CC] GetConnectedComponents num_components=%zu\n",
            components.size());
    for (size_t i = 0; i < components.size(); i++) {
        fprintf(stderr, "[TRACE-CC]   comp[%zu] size=%zu first=%d\n",
                i, components[i].size(), components[i][0]);
    }

    // mincut_only.cpp:42-45 (Simple branch)
    std::queue<std::vector<int>> done_being_mincut_clusters;
    for (auto& c : components) done_being_mincut_clusters.push(c);
    fprintf(stderr, "[TRACE-CC] DONE_QUEUE size=%zu\n", done_being_mincut_clusters.size());

    WriteClusterQueueProbed(done_being_mincut_clusters, new_to_orig, out_csv);

    // Emit JSON trace to stdout for the JS replay leg.
    std::cout << "{\n";
    std::cout << "  \"n_nodes\": " << orig_to_new.size() << ",\n";
    std::cout << "  \"node_map\": [";
    {
        bool first = true;
        for (auto const& [nid, orig] : new_to_orig) {
            if (!first) std::cout << ",";
            first = false;
            std::cout << "{\"new\":" << nid << ",\"orig\":\"" << orig << "\"}";
        }
    }
    std::cout << "],\n  \"components\": [";
    for (size_t i = 0; i < components.size(); i++) {
        if (i) std::cout << ",";
        emit_int_array(std::cout, components[i]);
    }
    std::cout << "]\n}\n";

    igraph_destroy(&graph);
    fprintf(stderr, "[TRACE-CC] PIPELINE_END\n");
    return 0;
}
