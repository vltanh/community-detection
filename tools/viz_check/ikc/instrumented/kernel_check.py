"""IKC canonical-tracer (Python instrumented fork of run_ikc.py).

Verbatim fork of community-detection/src/ikc/run_ikc.py:
  iterative_k_core_decomposition_MCS_ES + kc + k_valid + modular + format_graph,
with [TRACE-IKC] stderr prints + per-step JSON trace emission to stdout.

Output schema = community-detection/tools/viz_check/ikc/TRACE_SCHEMA.md.

TRACER_MODE env flag (kept for byte-equal-tracer playbook compliance):
  - TRACER_MODE=0 (default): silent. CSV is written; JSON trace is suppressed
    (stdout = empty). Used for build-pair test (a) where canonical
    run_ikc.py CSV must byte-equal tracer CSV without print interference.
  - TRACER_MODE=1: verbose. CSV is written *unchanged* (same path as
    TRACER_MODE=0); JSON trace is emitted to stdout; full [TRACE-IKC]
    log lines hit stderr. Used for the diff harness leg.

  Both modes execute the *same* algorithmic path. IKC has zero RNG calls
  and the modular() FP gate is dead (run_ikc.py:280 returns the constant
  POSITIVE_VALUE = 1 unconditionally; the paper formula on lines 284-296
  is unreachable). So there is no RNG-byte swap and no FP-primitive swap
  to gate on TRACER_MODE — the flag exists purely to mirror the
  byte-equal-tracer playbook discipline (every other tracer in this tree
  has the same flag) and to silence stdout for build-pair byte-equality.

CLI:
  python kernel_check.py <edge.csv> <kfloor> <out.csv>

  - edge.csv: header is read positionally (first two columns); accepts
    fixture32 (`node_id1,node_id2`) or canonical (`source,target`).
  - kfloor: integer minimum k for valid clusters (run_ikc.py -k).
  - out.csv: where the com.csv equivalent is written; byte-equal to
    canonical run_ikc.py com.csv.
"""

from __future__ import annotations

import csv
import json
import os
import sys
from collections import deque

import networkit as nk
import pandas as pd


# -- env / mode -------------------------------------------------------------

TRACER_MODE = os.environ.get("TRACER_MODE", "0") == "1"


def trace_log(*parts):
    """[TRACE-IKC] stderr prints. Only emitted under TRACER_MODE=1."""
    if TRACER_MODE:
        print("[TRACE-IKC]", *parts, file=sys.stderr)


# -- lex-smallest BFS canonicalization --------------------------------------
#
# TRACE_SCHEMA.md (contract notes) specifies `members_iter_order` as the
# lex-smallest BFS visit list: BFS from min(members), expanding each node's
# neighbours in ASC order. Both Python tracer + JS kernel must agree on this
# canonical order; NetworKit's `cc.getComponents()` natural order is
# implementation-defined (forEdges-insertion order on the recompacted
# weighted-directed graph) and diverges from lex-smallest BFS on larger
# graphs (T2/T3 stress-tier). We re-run BFS here against the kcore subgraph
# to lock both legs to the same canonical list.


def neighbors_asc(graph, node):
    """Sorted ASC list of neighbours of `node` in `graph`. Uses both
    directions when the graph is directed; deduplicated via set."""
    nbrs = set()
    for u in graph.iterNeighbors(node):
        nbrs.add(u)
    if graph.isDirected():
        for u in graph.iterInNeighbors(node):
            nbrs.add(u)
    return sorted(nbrs)


def lex_smallest_bfs(graph, members_set):
    """BFS from min(members_set) on `graph`, expanding neighbours in ASC
    order, only visiting nodes in members_set. Returns the visit order."""
    seen = set()
    order = []
    root = min(members_set)
    queue = deque([root])
    seen.add(root)
    while queue:
        v = queue.popleft()
        order.append(v)
        for u in neighbors_asc(graph, v):
            if u in seen:
                continue
            if u not in members_set:
                continue
            seen.add(u)
            queue.append(u)
    return order


# -- helpers (verbatim fork of run_ikc.py) ----------------------------------


def kc(graph):
    """[UPSTREAM run_ikc.py:224-263] kc(graph, k=None) variant.

    With k=None the canonical sets k = max_k, so the early-exit
    (max_k < k) never fires; we always populate kcore_members.
    """
    kc_obj = nk.centrality.CoreDecomposition(graph, storeNodeOrder=True)
    kc_obj.run()
    partition = kc_obj.getPartition()
    max_k = kc_obj.maxCoreNumber()
    k = max_k  # k=None branch in canonical
    save_k = k
    if max_k < k:
        return None, max_k, kc_obj
    kcore_members = []
    while k <= max_k:
        kcore_members.extend(partition.getMembers(k))
        k += 1
    trace_log(f"  kc: save_k={save_k} max_k={max_k} kcore_members_count={len(kcore_members)}")
    return nk.graphtools.subgraphFromNodes(graph, kcore_members), max_k, kc_obj


def k_valid(component, subgraph, k):
    """[UPSTREAM run_ikc.py:266-277].

    Iterates subgraph.iterNodes() (whole kcore subgraph) and only
    inspects nodes that are in the component set; bails on first
    intra-subgraph degree < k. Returns (ok, failing_node, loop_scope)
    where failing_node is None on success and loop_scope is the
    full per-iteration trace (G2 probe: every subgraph node visited
    in iteration order with in_component + degIn + degOut + sum +
    check_result, regardless of short-circuit).
    """
    component_nodes = set(component)
    loop_scope = []  # G2: every subgraph.iterNodes() visit
    ok = True
    failing = None
    for node in subgraph.iterNodes():
        in_comp = node in component_nodes
        din = subgraph.degreeIn(node)
        dout = subgraph.degreeOut(node)
        s = din + dout
        if not in_comp:
            check_result = "skipped"
        elif ok:
            if s < k:
                check_result = "fail"
                ok = False
                failing = node
            else:
                check_result = "ok"
        else:
            # Past the short-circuit: canonical's `break` (run_ikc.py:276)
            # stops iteration entirely; we still record the node + degrees
            # but tag as "post_break_unvisited" so the trace exposes the
            # full subgraph.iterNodes() scope (G2 audit row F) without
            # asserting canonical visited it.
            check_result = "post_break_unvisited"
        loop_scope.append({
            "subgraph_id": int(node),
            "in_component": bool(in_comp),
            "degIn": int(din),
            "degOut": int(dout),
            "sum": int(s),
            "check_result": check_result,
        })
    return ok, failing, loop_scope


def modular(component, orig_graph, inverted_orig):
    """[UPSTREAM run_ikc.py:280-282] dead gate, returns 1 unconditionally."""
    POSITIVE_VALUE = 1
    return POSITIVE_VALUE


def orig_id_component(component, inverted_orig):
    """[UPSTREAM run_ikc.py:299-300]"""
    return [inverted_orig[node] for node in component]


def format_graph(graph1):
    """[UPSTREAM run_ikc.py:303-330].

    Re-compacts node IDs, promotes unweighted-directed -> weighted-directed
    (edge weight = degreeIn(u); never read by the kernel), strips
    self-loops. Returns (graph, inverted_orig_node_id_dict).
    """
    origNodeIdDict = nk.graphtools.getContinuousNodeIds(graph1)
    invertedOrigNodeIdDict = dict(map(reversed, origNodeIdDict.items()))
    graph1 = nk.graphtools.getCompactedGraph(graph1, origNodeIdDict)

    if graph1.isWeighted() is False:
        weighted = nk.Graph(n=0, weighted=True, directed=True)
        for _node in graph1.iterNodes():
            weighted.addNode()
        for u, v in graph1.iterEdges():
            if weighted.hasNode(u) is False:
                weighted.addNode(u)
            if weighted.hasNode(v) is False:
                weighted.addNode(v)
            weighted.addEdge(u, v, graph1.degreeIn(u))
        graph = weighted
    else:
        graph = graph1

    graph.removeSelfLoops()
    return graph, invertedOrigNodeIdDict


# -- traced kernel ----------------------------------------------------------


def ikc_traced(graph, k_floor, inverted_orig, node_id_map_records):
    """[UPSTREAM run_ikc.py:83-221].

    Per-iter shape:
      iter, residual_n_before, residual_m_before,
      residual_compact_ids_sorted,
      max_k, core_numbers_sorted,
      kcore_n, kcore_m, kcore_compact_ids_sorted,
      components: [<comp_record>, ...],
      nodes_to_remove_sorted, kept_clusters_this_iter, terminated.

    `accepted_ledger` records, at acceptance time, each cluster's
    (max_k_at_acceptance, members_compact_in_orig_id) — compact ids
    mapped back to the original (string) names via inverted_orig.
    The diff harness uses orig-name space for cross-impl partition
    comparison since compact-id space drifts under recompaction.
    """
    orig_graph = nk.graphtools.subgraphFromNodes(graph, graph.iterNodes())
    L = orig_graph.numberOfEdges()
    singletons = []
    final_clusters = []

    nbr_failed_modularity = 0
    nbr_failed_k_valid = 0

    iters = []
    accepted_ledger = []
    iter_idx = 0

    while graph.numberOfNodes() > 0:
        residual_n_before = graph.numberOfNodes()
        residual_m_before = graph.numberOfEdges()
        residual_compact_ids_sorted = sorted(graph.iterNodes())
        trace_log(
            f"iter={iter_idx} residual_n={residual_n_before} "
            f"residual_m={residual_m_before}"
        )
        trace_log(f"  residual_compact_ids_sorted={residual_compact_ids_sorted}")

        sub_kc, max_k, kcore_obj = kc(graph)
        # core numbers per residual compact id (ascending).
        partition = kcore_obj.getPartition()
        core_numbers_sorted = []
        for nid in residual_compact_ids_sorted:
            core_numbers_sorted.append([nid, int(partition.subsetOf(nid))])
        trace_log(f"  max_k={max_k} core_numbers_sorted={core_numbers_sorted}")

        if sub_kc is None:
            trace_log("  TERMINATE: subgraph_none")
            iters.append({
                "iter": iter_idx,
                "residual_n_before": residual_n_before,
                "residual_m_before": residual_m_before,
                "residual_compact_ids_sorted": residual_compact_ids_sorted,
                "max_k": int(max_k),
                "core_numbers_sorted": core_numbers_sorted,
                "kcore_n": 0,
                "kcore_m": 0,
                "kcore_compact_ids_sorted": [],
                "components": [],
                "nodes_to_remove_sorted": [],
                "kept_clusters_this_iter": 0,
                "bail_L": None,
                "bail_per_node_modularity": [],
                "terminated": "subgraph_none",
            })
            break

        if max_k < k_floor:
            trace_log(f"  TERMINATE: max_k_below_floor (max_k={max_k} < k_floor={k_floor})")
            # G8: bail-iter (d/(2L))² FP primitive (run_ikc.py:118-120).
            # Live FP on this path. Trace per-node: original-id, degree
            # (orig_graph.degree = NetworKit directed out-degree), L, the
            # ratio d/(2L), its square, and the negated value appended to
            # final_clusters. Iteration order = graph.iterNodes() (ASC by
            # current compact id).
            bail_per_node_modularity = []
            two_L = 2 * L
            for node in graph.iterNodes():
                orig_node = inverted_orig[node]
                d = orig_graph.degree(orig_node)
                ratio = d / two_L if two_L != 0 else 0.0
                ratio_sq = ratio ** 2
                modu = (-1) * ratio_sq
                final_clusters.append(([orig_node], 0, modu))
                rec = {
                    "compact_id": int(node),
                    "orig_compact_id": int(orig_node),
                    "d": int(d),
                    "two_L": int(two_L),
                    "ratio": float(ratio),
                    "ratio_squared": float(ratio_sq),
                    "neg_ratio_squared": float(modu),
                }
                bail_per_node_modularity.append(rec)
                trace_log(
                    f"    [TRACE-IKC-BAIL] compact={node} orig={orig_node} "
                    f"d={d} 2L={two_L} ratio={ratio!r} sq={ratio_sq!r} "
                    f"neg_sq={modu!r}"
                )
            for node in singletons:
                final_clusters.append(([node], 0, 0))
            iters.append({
                "iter": iter_idx,
                "residual_n_before": residual_n_before,
                "residual_m_before": residual_m_before,
                "residual_compact_ids_sorted": residual_compact_ids_sorted,
                "max_k": int(max_k),
                "core_numbers_sorted": core_numbers_sorted,
                "kcore_n": 0,
                "kcore_m": 0,
                "kcore_compact_ids_sorted": [],
                "components": [],
                "nodes_to_remove_sorted": [],
                "kept_clusters_this_iter": 0,
                "bail_L": int(L),
                "bail_per_node_modularity": bail_per_node_modularity,
                "terminated": "max_k_below_floor",
            })
            break

        kcore_n = sub_kc.numberOfNodes()
        kcore_m = sub_kc.numberOfEdges()
        kcore_compact_ids_sorted = sorted(sub_kc.iterNodes())
        trace_log(
            f"  kcore_n={kcore_n} kcore_m={kcore_m} "
            f"kcore_compact_ids_sorted={kcore_compact_ids_sorted}"
        )

        cc = nk.components.WeaklyConnectedComponents(sub_kc)
        cc.run()
        components = cc.getComponents()
        trace_log(f"  components_count={len(components)}")

        nodes_to_remove = set()
        comp_records = []

        for comp_idx, component in enumerate(components):
            comp_n = len(component)
            members_set = set(int(n) for n in component)
            # `members_iter_order` is the canonical lex-smallest BFS visit
            # list (TRACE_SCHEMA.md contract). NetworKit's `component` order
            # is impl-defined and not used for tracing; it remains the
            # source for cluster_orig / nodes_to_remove / singletons /
            # accepted_ledger.members_compact_in_orig_id below, all of
            # which feed CSV writing (where intra-cluster row order must
            # match canonical run_ikc.py byte-for-byte).
            members_iter_order = lex_smallest_bfs(sub_kc, members_set)
            members_sorted = sorted(members_iter_order)
            trace_log(
                f"  comp[{comp_idx}] n={comp_n} "
                f"members_iter_order={members_iter_order}"
            )

            # G2: k-validity loop scope (run_ikc.py:266-277). Canonical
            # iterates subgraph.iterNodes() (every kcore-subgraph node in
            # compact-ASC order) and inspects only those in the component;
            # short-circuits on first failing component-node. The per-node
            # records below expose the loop's true scope + iteration order
            # + skip/check/fail decisions, separately from members_iter_order
            # (which is lex-BFS over component nodes only).
            ok, failing, k_valid_loop_scope = k_valid(component, sub_kc, k_floor)
            for rec in k_valid_loop_scope:
                trace_log(
                    f"    [TRACE-IKC-KV] subgraph_id={rec['subgraph_id']} "
                    f"in_comp={rec['in_component']} "
                    f"degIn={rec['degIn']} degOut={rec['degOut']} "
                    f"sum={rec['sum']} vs_kfloor={k_floor} "
                    f"check={rec['check_result']}"
                )

            modu_value = 1.0  # dead-gate constant; mirror run_ikc.py:280
            modu_pos = (modu_value > 0)
            kept = False
            fate_reason = ""
            if ok:
                # canonical calls modular(), which returns POSITIVE_VALUE = 1.
                modu = modular(component, orig_graph, inverted_orig)
                trace_log(f"    k_valid=True modular={modu} (dead-gate constant)")
                if modu > 0:
                    kept = True
                    fate_reason = "accepted"
                    cluster_orig = [inverted_orig[n] for n in component]
                    final_clusters.append((cluster_orig, max_k, modu))
                    # Map compact ids -> orig (string) names via inverted_orig.
                    # Schema TRACE_SCHEMA.md: members_compact_in_orig_id is the
                    # orig-name list (string-coerced to round-trip with the
                    # JS side, which reads CSV cells as strings).
                    # Use lex-BFS order (members_iter_order) so the field
                    # matches JS's accepted-cluster member order (JS captures
                    # cluster members from compLocal which iterates lex-BFS).
                    # CSV writing still uses NetworKit-natural `component`
                    # order via cluster_orig so byte-equal-CSV holds.
                    accepted_ledger.append({
                        "max_k_at_acceptance": int(max_k),
                        "members_compact_in_orig_id": [
                            node_id_map_records[inverted_orig[n]]["orig"]
                            for n in members_iter_order
                        ],
                    })
                    nodes_to_remove.update(component)
                    trace_log(f"    ACCEPT cluster_orig={cluster_orig}")
                else:
                    fate_reason = "failed modularity"
                    nbr_failed_modularity += 1
                    nodes_to_remove.update(component)
                    singletons.extend(orig_id_component(component, inverted_orig))
                    trace_log("    FAIL: modularity")
            else:
                fate_reason = "failed k-valid"
                nbr_failed_k_valid += 1
                nodes_to_remove.update(component)
                singletons.extend(orig_id_component(component, inverted_orig))
                trace_log(
                    f"    FAIL: k-valid (first failing compact_id={failing})"
                )

            # G9: nodes_to_remove accumulator state after this component's
            # `update(component)` (run_ikc.py:131, 151, 160, 165). The per-
            # component mutation order is invisible at iter_end; this field
            # captures the accumulator's sorted membership after each
            # component closes its branch (every branch path updates the
            # set — including the accepted path's :165 update at canonical).
            nodes_to_remove_after_update_sorted = sorted(int(n) for n in nodes_to_remove)
            trace_log(
                f"    [TRACE-IKC-NTR] comp_idx={comp_idx} fate={fate_reason} "
                f"ntr_size_after={len(nodes_to_remove_after_update_sorted)} "
                f"component_compact_ids={sorted(int(n) for n in component)}"
            )

            comp_records.append({
                "comp_idx": comp_idx,
                "comp_n": comp_n,
                "members_iter_order": members_iter_order,
                "members_sorted": members_sorted,
                "k_valid": bool(ok),
                "k_valid_failing_compact_id": (None if ok else int(failing)),
                "k_valid_loop_scope": k_valid_loop_scope,
                "modular_value": float(modu_value),
                "modular_pos": bool(modu_pos),
                "kept": bool(kept),
                "fate_reason": fate_reason,
                "nodes_to_remove_after_update_sorted":
                    nodes_to_remove_after_update_sorted,
            })

        nodes_to_remove_sorted = sorted(int(n) for n in nodes_to_remove)
        kept_clusters_this_iter = sum(1 for c in comp_records if c["kept"])
        trace_log(
            f"  nodes_to_remove_sorted={nodes_to_remove_sorted} "
            f"kept_clusters_this_iter={kept_clusters_this_iter}"
        )

        # G9: capture the actual set-iteration order at the removal site
        # (run_ikc.py:193). Python set iteration order is hash-order,
        # implementation-defined; NetworKit's getContinuousNodeIds
        # reassigns by old-compact-id-ASC so the removal call order does
        # NOT affect downstream compaction. Emit as STDERR-only trace
        # (not JSON) since Python's set hash-order and JS's lex-BFS
        # array order are fundamentally divergent by design — the
        # invariant being audited is that the resulting multiset is
        # equal (already covered by nodes_to_remove_sorted) AND that
        # the downstream recompaction is order-independent. The probe
        # exposes the order so a future regression in the recompaction
        # invariant would be diagnosable.
        nodes_to_remove_removal_iter_order = [int(n) for n in nodes_to_remove]
        trace_log(
            f"  [TRACE-IKC-NTR] removal_iter_order="
            f"{nodes_to_remove_removal_iter_order}"
        )

        iters.append({
            "iter": iter_idx,
            "residual_n_before": residual_n_before,
            "residual_m_before": residual_m_before,
            "residual_compact_ids_sorted": residual_compact_ids_sorted,
            "max_k": int(max_k),
            "core_numbers_sorted": core_numbers_sorted,
            "kcore_n": kcore_n,
            "kcore_m": kcore_m,
            "kcore_compact_ids_sorted": kcore_compact_ids_sorted,
            "components": comp_records,
            "nodes_to_remove_sorted": nodes_to_remove_sorted,
            "kept_clusters_this_iter": kept_clusters_this_iter,
            "bail_L": None,
            "bail_per_node_modularity": [],
            "terminated": None,
        })

        # Apply removals (run_ikc.py:193-194; set iteration order matches canonical).
        for node in nodes_to_remove:
            graph.removeNode(node)

        # Recompact (run_ikc.py:199-206). New compact ids are 0..n-1
        # in old-compact-id-ascending order.
        node_id_dict = nk.graphtools.getContinuousNodeIds(graph)
        inverted_node_id_dict = dict(map(reversed, node_id_dict.items()))
        temp_dict = dict()
        for new_id, old_id in inverted_node_id_dict.items():
            orig_id = inverted_orig[old_id]
            temp_dict[new_id] = orig_id
        inverted_orig = temp_dict
        graph = nk.graphtools.getCompactedGraph(graph, node_id_dict)

        trace_log(f"  recompact: nodes_left={graph.numberOfNodes()}")
        iter_idx += 1

    trace_log(
        f"PIPELINE_END iters={len(iters)} clusters={len(final_clusters)} "
        f"failed_kvalid={nbr_failed_k_valid} failed_mod={nbr_failed_modularity}"
    )
    return final_clusters, iters, accepted_ledger


# -- print_clusters (verbatim of run_ikc.py:45-80) --------------------------


def print_clusters(clusters, out_csv, inverted_node_id_map):
    """[UPSTREAM run_ikc.py:45-80].

    - Header: node_id,cluster_id (comma; lineterminator '\\n').
    - Skip clusters with len(cluster) <= 1.
    - cluster_id increments 0..K-1 in canonical iteration order.
    """
    cluster_id_counter = 0
    csv_lines = 0  # post-filter row count (excluding header)

    with open(out_csv, "w") as output:
        csvwriter = csv.writer(output, delimiter=",", lineterminator="\n")
        csvwriter.writerow(["node_id", "cluster_id"])

        for cluster_info in clusters:
            (cluster, _k, _modularity_score) = cluster_info
            if len(cluster) <= 1:
                continue
            for node in cluster:
                csvwriter.writerow([inverted_node_id_map[node], cluster_id_counter])
                csv_lines += 1
            cluster_id_counter += 1

    return cluster_id_counter, csv_lines


# -- main -------------------------------------------------------------------


def main():
    if len(sys.argv) < 4:
        print(
            "usage: kernel_check.py <edge.csv> <kfloor> <out.csv>",
            file=sys.stderr,
        )
        sys.exit(2)

    edge_csv = sys.argv[1]
    k_floor = int(sys.argv[2])
    out_csv = sys.argv[3]

    trace_log(
        f"PIPELINE_START edge={edge_csv} k_floor={k_floor} out={out_csv} "
        f"TRACER_MODE={int(TRACER_MODE)}"
    )

    # ---- read fixture -----------------------------------------------------
    df = pd.read_csv(edge_csv)
    src_col, tgt_col = df.columns[0], df.columns[1]
    # Canonical (run_ikc.py:28) does pd.unique on stacked source+target.
    unique_nodes = pd.unique(df[[src_col, tgt_col]].values.ravel("K"))
    node_map = {name: i for i, name in enumerate(unique_nodes)}
    inverted_node_id_map = {i: name for name, i in node_map.items()}
    n_orig = len(unique_nodes)
    trace_log(f"  read_csv: rows={len(df)} unique_nodes={n_orig}")

    # node_id_map records, encounter order = pd.unique order (= compact id 0..n-1).
    node_id_map_records = [
        {"orig": str(name), "compact": int(i)}
        for i, name in enumerate(unique_nodes)
    ]

    # ---- build graph ------------------------------------------------------
    graph1 = nk.Graph(n=n_orig, weighted=False, directed=True)
    for row in df.itertuples(index=False):
        u = getattr(row, src_col) if hasattr(row, src_col) else row[0]
        v = getattr(row, tgt_col) if hasattr(row, tgt_col) else row[1]
        graph1.addEdge(node_map[u], node_map[v])

    graph, inverted_orig = format_graph(graph1)
    m_orig = graph.numberOfEdges()
    trace_log(f"  format_graph: n={graph.numberOfNodes()} m={m_orig}")

    # ---- run kernel -------------------------------------------------------
    final_clusters, iters, accepted_ledger = ikc_traced(
        graph, k_floor, inverted_orig, node_id_map_records
    )

    # ---- write CSV (always; identical in both modes) ----------------------
    cluster_id_counter, csv_lines = print_clusters(
        final_clusters, out_csv, inverted_node_id_map
    )
    trace_log(f"  print_clusters: kept={cluster_id_counter} csv_lines={csv_lines}")

    # ---- emit JSON trace (TRACER_MODE=1 only) -----------------------------
    if not TRACER_MODE:
        return

    # Singleton-pre-filter count = total final_clusters (incl size <= 1).
    n_pre = len(final_clusters)
    n_post = cluster_id_counter

    # Fixture basename for the trace top-level field.
    fixture_basename = os.path.basename(edge_csv)
    if fixture_basename.endswith(".csv"):
        fixture_basename = fixture_basename[:-4]

    out_json = {
        "fixture": fixture_basename,
        "k_floor": int(k_floor),
        "n_orig": int(n_orig),
        "m_orig": int(m_orig),
        "node_id_map": node_id_map_records,
        "iters": iters,
        "final": {
            "n_final_clusters_pre_singleton_filter": int(n_pre),
            "n_final_clusters_post_singleton_filter": int(n_post),
            "csv_lines": int(csv_lines),
            "accepted_canonical_order": accepted_ledger,
        },
    }

    # sort_keys=True for self-determinism (no dict-order leakage).
    sys.stdout.write(json.dumps(out_json, sort_keys=True))
    sys.stdout.write("\n")
    sys.stdout.flush()


if __name__ == "__main__":
    main()
