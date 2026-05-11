"""Bumped 3-tier empirical-network panel.

The 3-tier panel was bumped 2026-05-10 to subsume the heritage Louvain
empirical sweep: every network in `data/empirical_networks/networks/`
with `n <= 30000`, regardless of bipartite/directed, bucketed by node
count. Total = 161 networks. Standard sweep = 50 seeds.

Tier buckets (size only, no bipartite/directed filter — extends the
heritage rule from `reference_cd_stress_tiers.md` to all 161 nets):

  T1: n < 3000
  T2: 3000 <= n < 15000
  T3: 15000 <= n <= 30000

Per `reference_empirical_networks_dataset.md`, fixtures live at
`data/empirical_networks/networks/<net>/<net_subnet>.csv`; the legacy
`<net>/<net>.csv` fallback is preserved.

Standard 50-seed set is shared with `reference_cd_stress_tiers.md`.
"""
from __future__ import annotations

import csv
from pathlib import Path


SEEDS_50 = [
    1, 2, 3, 5, 7, 11, 13, 17, 19, 23,
    29, 31, 37, 41, 42, 43, 47, 53, 59, 61,
    67, 71, 73, 79, 83, 89, 97, 99, 101, 103,
    109, 113, 127, 137, 149, 163, 179, 199, 223, 251,
    279, 311, 349, 397, 449, 503, 569, 641, 1729, 65535,
]


def _stats_path(repo: Path) -> Path:
    return repo.parent / "data" / "empirical_networks" / "networks" / "processed_networks_stats.csv"


def _nets_root(repo: Path) -> Path:
    return repo.parent / "data" / "empirical_networks" / "networks"


def discover_panel(repo: Path, node_limit: int = 30000) -> dict[str, list[dict]]:
    """Return {'T1': [...], 'T2': [...], 'T3': [...]} where each entry is
    {'net','subnet','n','m','edge'}.

    Filter: n <= node_limit AND edge CSV exists. Sorted ASC by n,subnet.
    Bucket: T1 n<3000, T2 3000<=n<15000, T3 n>=15000.
    """
    stats = _stats_path(repo)
    root = _nets_root(repo)
    if not stats.is_file():
        raise FileNotFoundError(f"missing stats: {stats}")
    out: list[dict] = []
    with stats.open() as f:
        for row in csv.DictReader(f):
            n = int(row["nodes"])
            if n > node_limit:
                continue
            subnet = row["net_subnet"]
            edge = root / row["net"] / f"{subnet}.csv"
            if not edge.is_file():
                alt = root / row["net"] / f"{row['net']}.csv"
                if alt.is_file():
                    edge = alt
                else:
                    continue
            out.append({
                "net": row["net"], "subnet": subnet,
                "n": n, "m": int(row["edges"]),
                "edge": str(edge),
            })
    out.sort(key=lambda r: (r["n"], r["subnet"]))
    T1 = [r for r in out if r["n"] < 3000]
    T2 = [r for r in out if 3000 <= r["n"] < 15000]
    T3 = [r for r in out if r["n"] >= 15000]
    return {"T1": T1, "T2": T2, "T3": T3}


def tier_for(n: int) -> str:
    if n < 3000: return "T1"
    if n < 15000: return "T2"
    return "T3"
