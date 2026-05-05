"""IKC instrumented Python tracer.

Verbatim fork of community-detection/src/ikc/run_ikc.py's
iterative_k_core_decomposition_MCS_ES + kc + k_valid + modular,
with [TRACE-IKC] log lines + JSON emit at every iteration.

IKC is deterministic (no RNG); the trace is the per-iteration
state of the peel loop:
  - max_k (largest k still present)
  - per-component (size, k_valid, modular>0, kept/dropped)
  - graph-state node count after removal
  - final cluster list

Build: not needed (Python).
Run:   conda run -n nwbench python kernel_check.py <edge.csv> <kfloor> <out.csv>
       stdout = JSON trace; stderr = [TRACE-IKC] log lines.
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

import networkit as nk
import pandas as pd


def trace_log(*parts):
    print("[TRACE-IKC]", *parts, file=sys.stderr)


def kc(graph):
    """[UPSTREAM run_ikc.py:222-261] kc(graph, k=None) variant.

    Mirrors canonical: when k=None, sets k=max_k. Returns (None, max_k)
    only if max_k < k (i.e. max_k < max_k, never). So None happens only
    when storeNodeOrder yields max_k undefined? No - canonical returns None
    only when its early-exit triggers; with k=None the early-exit never
    fires. We must still loop k..max_k populating members.
    """
    kc_obj = nk.centrality.CoreDecomposition(graph, storeNodeOrder=True)
    kc_obj.run()
    partition = kc_obj.getPartition()
    max_k = kc_obj.maxCoreNumber()
    k = max_k  # k=None branch in canonical
    if max_k < k:
        return None, max_k
    kcore_members = []
    kk = k
    while kk <= max_k:
        kcore_members.extend(partition.getMembers(kk))
        kk += 1
    return nk.graphtools.subgraphFromNodes(graph, kcore_members), max_k


def k_valid(component, subgraph, k):
    """[UPSTREAM run_ikc.py:264-275]"""
    component_nodes = set(component)
    for node in subgraph.iterNodes():
        if node in component_nodes:
            if (subgraph.degreeIn(node) + subgraph.degreeOut(node)) < k:
                return False
    return True


def modular(component, orig_graph, inverted_orig):
    """[UPSTREAM run_ikc.py:278-280] - always returns POSITIVE_VALUE = 1."""
    return 1


def orig_id_component(component, inverted_orig):
    return [inverted_orig[n] for n in component]


def format_graph(graph1):
    """[UPSTREAM run_ikc.py:301-328]"""
    origNodeIdDict = nk.graphtools.getContinuousNodeIds(graph1)
    invertedOrigNodeIdDict = dict(map(reversed, origNodeIdDict.items()))
    graph1 = nk.graphtools.getCompactedGraph(graph1, origNodeIdDict)
    if graph1.isWeighted() is False:
        weighted = nk.Graph(n=0, weighted=True, directed=True)
        for _ in graph1.iterNodes():
            weighted.addNode()
        for u, v in graph1.iterEdges():
            if not weighted.hasNode(u):
                weighted.addNode(u)
            if not weighted.hasNode(v):
                weighted.addNode(v)
            weighted.addEdge(u, v, graph1.degreeIn(u))
        graph = weighted
    else:
        graph = graph1
    graph.removeSelfLoops()
    return graph, invertedOrigNodeIdDict


def ikc_traced(graph, k_floor, inverted_orig):
    """[UPSTREAM run_ikc.py:81-219] iterative_k_core_decomposition_MCS_ES,
    with [TRACE-IKC] capture per iteration.
    """
    orig_graph = nk.graphtools.subgraphFromNodes(graph, graph.iterNodes())
    L = orig_graph.numberOfEdges()
    singletons = []
    final_clusters = []
    iters = []
    iter_idx = 0

    while graph.numberOfNodes() > 0:
        sub_kc, max_k = kc(graph)
        trace_log(f"iter={iter_idx} graph_n={graph.numberOfNodes()} max_k={max_k}")
        if sub_kc is None:
            iters.append({"iter": iter_idx, "max_k": max_k, "terminated": "subgraph_none"})
            break
        if max_k < k_floor:
            # Add remaining nodes as singletons.
            for node in graph.iterNodes():
                modu = (-1) * (orig_graph.degree(inverted_orig[node]) / (2 * L)) ** 2
                final_clusters.append(([inverted_orig[node]], 0, modu))
            for node in singletons:
                final_clusters.append(([node], 0, 0))
            iters.append({"iter": iter_idx, "max_k": max_k, "terminated": "max_k_below_floor"})
            break

        cc = nk.components.WeaklyConnectedComponents(sub_kc)
        cc.run()
        components = cc.getComponents()
        comp_records = []
        nodes_to_remove = set()

        for component in components:
            comp_rec = {"size": len(component), "kept": False,
                        "modular_pos": False, "k_valid": False}
            if k_valid(component, sub_kc, k_floor):
                comp_rec["k_valid"] = True
                modu = modular(component, orig_graph, inverted_orig)
                if modu > 0:
                    comp_rec["modular_pos"] = True
                    comp_rec["kept"] = True
                    cluster_orig = [inverted_orig[n] for n in component]
                    final_clusters.append((cluster_orig, max_k, modu))
                    nodes_to_remove.update(component)
                else:
                    nodes_to_remove.update(component)
                    singletons.extend(orig_id_component(component, inverted_orig))
            else:
                nodes_to_remove.update(component)
                singletons.extend(orig_id_component(component, inverted_orig))
            comp_records.append(comp_rec)

        iters.append({
            "iter": iter_idx,
            "max_k": max_k,
            "components": comp_records,
            "kept_clusters_this_iter": sum(1 for c in comp_records if c["kept"]),
        })
        trace_log(f"  components={len(components)} kept={iters[-1]['kept_clusters_this_iter']}")

        for node in nodes_to_remove:
            graph.removeNode(node)
        node_id_dict = nk.graphtools.getContinuousNodeIds(graph)
        inv_dict = {v: k for k, v in node_id_dict.items()}
        new_inv_orig = {}
        for new_id, old_id in inv_dict.items():
            new_inv_orig[new_id] = inverted_orig[old_id]
        inverted_orig = new_inv_orig
        graph = nk.graphtools.getCompactedGraph(graph, node_id_dict)
        iter_idx += 1

    return final_clusters, iters


def main():
    if len(sys.argv) < 4:
        print("usage: kernel_check.py <edge.csv> <kfloor> <out.csv>", file=sys.stderr)
        sys.exit(2)
    edge_csv = sys.argv[1]
    k_floor = int(sys.argv[2])
    out_csv = sys.argv[3]

    trace_log(f"PIPELINE_START edge={edge_csv} k_floor={k_floor}")

    df = pd.read_csv(edge_csv)
    # Read first two columns positionally — fixture32 uses node_id1/node_id2,
    # dnc uses source/target. Canonical run_ikc requires source/target headers.
    src_col, tgt_col = df.columns[0], df.columns[1]
    unique_nodes = pd.unique(df[[src_col, tgt_col]].values.ravel("K"))
    node_map = {name: i for i, name in enumerate(unique_nodes)}
    inv_map = {i: name for name, i in node_map.items()}

    g1 = nk.Graph(n=len(unique_nodes), weighted=False, directed=True)
    for row in df.itertuples(index=False):
        g1.addEdge(node_map[getattr(row, src_col)], node_map[getattr(row, tgt_col)])

    graph, inverted_orig = format_graph(g1)
    final_clusters, iters = ikc_traced(graph, k_floor, inverted_orig)
    trace_log(f"PIPELINE_END iters={len(iters)} clusters={len(final_clusters)}")

    # Write CSV (matches run_ikc.print_clusters: skip singleton clusters,
    # consecutive cluster_ids 0..K-1, orig-name node IDs via inv_map).
    with open(out_csv, "w") as f:
        f.write("node_id,cluster_id\n")
        cid = 0
        for cluster, _max_k, _modu in final_clusters:
            if len(cluster) <= 1:
                continue
            for node in cluster:
                f.write(f"{inv_map[node]},{cid}\n")
            cid += 1

    # Emit JSON trace.
    out_json = {
        "n_nodes": len(unique_nodes),
        "n_edges": len(df),
        "k_floor": k_floor,
        "node_map": [{"orig": str(name), "new": int(i)} for name, i in node_map.items()],
        "iters": iters,
        "final_clusters": [
            {"size": len(c[0]), "max_k": int(c[1]), "modular": float(c[2]),
             "nodes": [int(n) if isinstance(n, (int,)) else int(n) for n in c[0]]}
            for c in final_clusters
        ],
    }
    print(json.dumps(out_json))


if __name__ == "__main__":
    main()
