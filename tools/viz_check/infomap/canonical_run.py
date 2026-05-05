"""Canonical Infomap (pypi) runner. Emits com.csv + L + module count.

Run:
    conda run -n nwbench python canonical_run.py <edge.csv> <seed> <out.csv>
        > <stats.json>
"""
from __future__ import annotations

import json
import sys
from collections import Counter

import pandas as pd
from infomap import Infomap


def main():
    if len(sys.argv) < 4:
        print("usage: canonical_run.py <edge.csv> <seed> <out.csv>", file=sys.stderr)
        sys.exit(2)
    edge_csv, seed, out_csv = sys.argv[1], sys.argv[2], sys.argv[3]

    im = Infomap(f"--seed {seed} --silent --two-level")

    df = pd.read_csv(edge_csv)
    src_col, tgt_col = df.columns[0], df.columns[1]
    nodes = pd.unique(df[[src_col, tgt_col]].values.ravel("K"))
    node_map = {n: i for i, n in enumerate(nodes)}
    inv = {i: n for n, i in node_map.items()}

    for r in df.itertuples(index=False):
        im.add_link(node_map[getattr(r, src_col)], node_map[getattr(r, tgt_col)])
    im.run()

    rows = [(int(inv[node.node_id]), int(node.module_id))
            for node in im.tree if node.is_leaf]

    # Drop singletons + renumber 0..K-1, sort by node_id (mirrors run_infomap.py).
    cnt = Counter(c for _, c in rows)
    rows = [(n, c) for n, c in rows if cnt[c] > 1]
    ucs = sorted({c for _, c in rows})
    remap = {old: new for new, old in enumerate(ucs)}
    rows = sorted([(n, remap[c]) for n, c in rows])
    with open(out_csv, "w") as f:
        f.write("node_id,cluster_id\n")
        for n, c in rows:
            f.write(f"{n},{c}\n")

    stats = {
        "codelength": float(im.codelength),
        "num_top_modules": int(im.num_top_modules),
        "n_nodes": int(len(nodes)),
        "n_edges": int(len(df)),
        "n_kept_clusters": len(ucs),
        "node_map": [{"orig": str(name), "new": int(i)}
                     for name, i in node_map.items()],
    }
    print(json.dumps(stats))


if __name__ == "__main__":
    main()
