"""VieCut cactus mincut + most-balanced cut kernel cross-check driver.

Per-cluster: for each cluster in com.csv, induce its subgraph from
edge.csv, run VieCut cactus mincut on the subgraph, parse the cactus
GraphML + bipartition cut.txt, and assert that the JS replay (port of
balanced_cut_dfs + most_balanced) reproduces the canonical bipartition
for SOME start_vertex in 0..|cactus|-1.

Three legs per cluster:
  1. canonical: VieCut/build/mincut <metis> cactus -b -s -o <cut> -t
                <cactus> -r 0
  2. tracer:    same invocation; the cactus.graphml is parsed into JSON
                alongside the bipartition.
  3. replay:    Node JS over the cactus, sweep start_vertex.

Run:
    python tools/viz_check/viecut/kernel_check.py [--verbose]
"""
from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "_common"))
import driver as D

REPO = D.repo_root_from_here(HERE)
BIN = REPO / "VieCut" / "build" / "mincut"
JS_REPLAY = HERE / "kernel_check.mjs"


def read_edges(edge_csv: Path) -> tuple[list[tuple[int, int]], int]:
    edges: list[tuple[int, int]] = []
    max_id = -1
    with edge_csv.open() as f:
        header = next(f).strip().lower()
        if not (header.startswith("node_id1") or header.startswith("source")):
            raise RuntimeError(f"unexpected header: {header}")
        for line in f:
            parts = line.strip().split(",")
            if len(parts) < 2:
                continue
            u, v = int(parts[0]), int(parts[1])
            if u == v:
                continue
            edges.append((u, v))
            max_id = max(max_id, u, v)
    return edges, max_id + 1


def read_clusters(com_csv: Path) -> dict[int, list[int]]:
    """Return {cluster_id: [node_id, ...]}, node lists sorted ascending."""
    clusters: dict[int, list[int]] = {}
    with com_csv.open() as f:
        next(f)  # header
        for line in f:
            parts = line.strip().split(",")
            if len(parts) < 2:
                continue
            n, c = int(parts[0]), int(parts[1])
            clusters.setdefault(c, []).append(n)
    for c in clusters:
        clusters[c].sort()
    return clusters


def cluster_to_metis(global_edges, cluster_nodes, metis_path: Path
                     ) -> tuple[list[int], int, int]:
    """Induce subgraph on cluster_nodes; renumber 1..k for METIS.

    Returns (cluster_nodes, n, m). cluster_nodes is the canonical
    ordering of original ids = subgraph 0-indexed positions.
    """
    cluster_set = set(cluster_nodes)
    pos = {nid: i for i, nid in enumerate(cluster_nodes)}
    seen: set[tuple[int, int]] = set()
    edges_local: list[tuple[int, int]] = []
    for u, v in global_edges:
        if u in cluster_set and v in cluster_set:
            a, b = (pos[u], pos[v]) if pos[u] < pos[v] else (pos[v], pos[u])
            if (a, b) in seen:
                continue
            seen.add((a, b))
            edges_local.append((a, b))
    n = len(cluster_nodes)
    adj: list[list[int]] = [[] for _ in range(n)]
    for u, v in edges_local:
        adj[u].append(v + 1)
        adj[v].append(u + 1)
    m = sum(len(a) for a in adj) // 2
    with metis_path.open("w") as f:
        f.write(f"{n} {m}\n")
        for i in range(n):
            f.write(" ".join(str(x) for x in sorted(adj[i])) + "\n")
    return cluster_nodes, n, m


def parse_cactus_graphml(text: str) -> dict:
    nodes_raw = re.findall(
        r'<node id="(\d+)">\s*<data key="containedVertices">([^<]*)</data>',
        text, flags=re.DOTALL)
    nodes = []
    for nid, cv in nodes_raw:
        contained = []
        cv = cv.strip()
        if cv:
            for tok in cv.split(","):
                tok = tok.strip()
                if tok:
                    contained.append(int(tok))
        nodes.append({"id": int(nid), "contained": contained})
    nodes.sort(key=lambda d: d["id"])

    edges_raw = re.findall(
        r'<edge id="[^"]*" source="(\d+)" target="(\d+)">\s*'
        r'<data key="weight">([^<]+)</data>',
        text, flags=re.DOTALL)
    edges = [{"u": int(s), "v": int(t), "weight": float(w)}
             for s, t, w in edges_raw]

    n = len(nodes)
    adj: list[list[dict]] = [[] for _ in range(n)]
    for e in edges:
        u, v, w = e["u"], e["v"], e["weight"]
        u_idx = len(adj[u])
        v_idx = len(adj[v])
        adj[u].append({"target": v, "weight": w, "rev": v_idx})
        adj[v].append({"target": u, "weight": w, "rev": u_idx})
    return {"n": n, "nodes": nodes, "edges": edges, "adj": adj}


def parse_bipartition(text: str, n_nodes: int) -> list[int]:
    vals = []
    for line in text.strip().splitlines():
        line = line.strip()
        if line:
            vals.append(int(line))
    if len(vals) != n_nodes:
        raise RuntimeError(
            f"cut.txt length {len(vals)} != n_nodes {n_nodes}")
    return vals


def run_mincut(metis: Path, cut_file: Path, cactus_file: Path,
               log_file: Path) -> tuple[bool, str, int]:
    """Run mincut binary. Returns (cut_found, log_text, mincut_value).

    cut_found = False when mincut==0 (graph disconnected; binary skips
    cactus + does not write cut.txt).
    """
    rc = subprocess.run(
        [str(BIN), str(metis), "cactus",
         "-b", "-s",
         "-o", str(cut_file),
         "-t", str(cactus_file),
         "-r", "0"],
        capture_output=True, text=True)
    log = rc.stdout + rc.stderr
    log_file.write_text(log)
    if rc.returncode != 0:
        raise RuntimeError(f"mincut binary failed: {rc.stderr}")
    mincut_val = None
    for line in log.splitlines():
        m = re.search(r"\bcut=(-?\d+)\b", line)
        if m:
            mincut_val = int(m.group(1))
            break
    if mincut_val is None:
        raise RuntimeError("could not parse 'cut=' from binary stdout")
    cut_found = mincut_val > 0 and cut_file.exists()
    return cut_found, log, mincut_val


def per_cluster_3leg(name: str, edge: Path, com: Path, work: Path,
                     verbose: bool) -> tuple[int, int, int]:
    """Run all clusters of one fixture; return (clusters_run, fails, skipped)."""
    global_edges, n_global = read_edges(edge)
    clusters = read_clusters(com)
    fails = 0
    skipped = 0
    runs = 0

    aggregate_canon: list[str] = []
    aggregate_tracer: list[str] = []
    aggregate_payload = {"name": name, "clusters": []}

    for cid in sorted(clusters):
        nodes = clusters[cid]
        if len(nodes) < 2:
            skipped += 1
            continue
        tag = f"{name}_c{cid}"
        metis = work / f"{tag}_viecut.metis"
        cluster_to_metis(global_edges, nodes, metis)

        cut_canon = work / f"{tag}_viecut_canon_cut.txt"
        cactus_canon = work / f"{tag}_viecut_canon_cactus.graphml"
        log_canon = work / f"{tag}_viecut_canon.log"
        ok, _, mincut_val = run_mincut(metis, cut_canon, cactus_canon, log_canon)
        if not ok:
            skipped += 1
            continue

        cut_tracer = work / f"{tag}_viecut_tracer_cut.txt"
        cactus_tracer = work / f"{tag}_viecut_tracer_cactus.graphml"
        log_tracer = work / f"{tag}_viecut_tracer.log"
        ok2, _, mincut_val2 = run_mincut(metis, cut_tracer, cactus_tracer, log_tracer)
        if not ok2 or mincut_val2 != mincut_val:
            print(f"  [{tag}] FAIL: tracer reproduce")
            fails += 1
            continue

        # canonical-vs-tracer byte-equal check
        canon_text = cut_canon.read_text()
        tracer_text = cut_tracer.read_text()
        aggregate_canon.append(f"{tag} {canon_text.replace(chr(10), '|')}")
        aggregate_tracer.append(f"{tag} {tracer_text.replace(chr(10), '|')}")

        # build replay payload
        cactus = parse_cactus_graphml(cactus_tracer.read_text())
        bipartition = parse_bipartition(tracer_text, len(nodes))
        cluster_payload = {
            "tag": tag,
            "cluster_id": cid,
            "n_local": len(nodes),
            "mincut": mincut_val,
            "cactus": cactus,
            "bipartition": bipartition,
        }
        aggregate_payload["clusters"].append(cluster_payload)
        runs += 1

    # aggregate canonical vs tracer
    if aggregate_canon != aggregate_tracer:
        print(f"  [{name}] canonical != tracer (per-cluster aggregate)")
        fails += 1
    else:
        print(f"  [{name}] canonical == tracer: PASS ({runs} clusters, "
              f"{skipped} skipped/no-cut)")

    # JS replay one shot for whole fixture
    json_path = work / f"{name}_viecut_tracer.json"
    json_path.write_text(json.dumps(aggregate_payload))
    rc = subprocess.run(["node", str(JS_REPLAY), str(json_path)],
                        capture_output=True, text=True)
    if rc.returncode == 0:
        print(f"  [{name}] tracer == js_replay: PASS ({runs} clusters)")
        if verbose:
            print(rc.stdout)
    else:
        print(f"  [{name}] tracer != js_replay: FAIL")
        print(rc.stdout)
        print(rc.stderr)
        fails += 1
    return runs, fails, skipped


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    if not BIN.exists():
        sys.exit(
            f"missing binary: {BIN}\n"
            f"build with: cd {BIN.parent.parent} && bash compile.sh "
            f"-DCMAKE_POLICY_VERSION_MINIMUM=3.5")
    work = D.workdir(REPO)
    D.emit_fixture_inputs(REPO, work)

    total_fails = 0
    total_runs = 0
    for name, edge, com in D.fixture_cases(work):
        print(f"\n=== {name} ===")
        runs, fails, skipped = per_cluster_3leg(
            name, edge, com, work, args.verbose)
        total_fails += fails
        total_runs += runs

    print()
    if total_fails:
        print(f"OVERALL: FAIL ({total_fails} failures, {total_runs} cluster-runs)")
        sys.exit(1)
    print(f"OVERALL: PASS ({total_runs} cluster-runs)")


if __name__ == "__main__":
    main()
