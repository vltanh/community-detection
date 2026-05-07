#!/usr/bin/env python3
"""Convert source,target CSV to METIS file (1-indexed adjacency-list).

Mirrors the per-cluster routine in kernel_check.py:cluster_to_metis but
applied to a whole-graph CSV. Self-loops + parallel edges deduplicated.
"""
from __future__ import annotations
import sys
from pathlib import Path


def csv_to_metis(csv_path: Path, metis_path: Path) -> tuple[int, int]:
    nodes: dict[str, int] = {}
    edges: list[tuple[int, int]] = []
    with csv_path.open() as f:
        header = next(f).strip().lower()
        if not (header.startswith("source") or header.startswith("node_id1")):
            raise SystemExit(f"unexpected header: {header}")
        for line in f:
            parts = line.strip().split(",")
            if len(parts) < 2:
                continue
            s, t = parts[0], parts[1]
            if s == t:
                continue
            if s not in nodes:
                nodes[s] = len(nodes)
            if t not in nodes:
                nodes[t] = len(nodes)
            edges.append((nodes[s], nodes[t]))

    n = len(nodes)
    seen: set[tuple[int, int]] = set()
    adj: list[list[int]] = [[] for _ in range(n)]
    for u, v in edges:
        a, b = (u, v) if u < v else (v, u)
        if (a, b) in seen:
            continue
        seen.add((a, b))
        adj[a].append(b + 1)
        adj[b].append(a + 1)

    m = sum(len(a) for a in adj) // 2
    with metis_path.open("w") as f:
        f.write(f"{n} {m}\n")
        for i in range(n):
            f.write(" ".join(str(x) for x in sorted(adj[i])) + "\n")
    return n, m


def main():
    if len(sys.argv) != 3:
        sys.exit(f"usage: {sys.argv[0]} <edge.csv> <out.metis>")
    n, m = csv_to_metis(Path(sys.argv[1]), Path(sys.argv[2]))
    print(f"n={n} m={m} -> {sys.argv[2]}")


if __name__ == "__main__":
    main()
