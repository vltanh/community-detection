"""Louvain L4 empirical-network stress driver.

Reuses kernel_check_l4.py's per-cell runner. Walks data/empirical_networks
(reference_empirical_networks_dataset.md), filters to n <= NODE_LIMIT,
runs SEED_COUNT seeds per network. Per cell: bit-equal verify (level,
pass, visit, v, fromComm, toComm, moved, dSbits, dGainBits) +
fineMembership + Q_final.

Run:
    python tools/viz_check/louvain/kernel_check_l4_empirical.py \
        [--node-limit 30000] [--seed-count 50] [--workers N] \
        [--working-tree] [--max-networks N]
"""
from __future__ import annotations

import argparse
import csv
import os
import sys
import time
from concurrent.futures import ProcessPoolExecutor, as_completed
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "_common"))
import driver as D

# Reuse the per-cell runner from kernel_check_l4.
sys.path.insert(0, str(HERE))
import kernel_check_l4 as L4

REPO = D.repo_root_from_here(HERE)
EMP_ROOT = REPO.parent / "data" / "empirical_networks" / "networks"
EMP_STATS = EMP_ROOT / "processed_networks_stats.csv"


def discover_networks(node_limit: int) -> list[tuple[str, Path, int, int]]:
    """Return [(name, edge_csv_path, n, m), ...] with n <= node_limit."""
    out: list[tuple[str, Path, int, int]] = []
    if not EMP_STATS.is_file():
        sys.exit(f"missing stats: {EMP_STATS}")
    with EMP_STATS.open() as f:
        rdr = csv.DictReader(f)
        for row in rdr:
            n = int(row["nodes"])
            m = int(row["edges"])
            if n > node_limit:
                continue
            name = row["net_subnet"]
            edge_path = EMP_ROOT / row["net"] / f"{name}.csv"
            if not edge_path.is_file():
                # Some net_subnets don't match a file; try net.csv at top.
                alt = EMP_ROOT / row["net"] / f"{row['net']}.csv"
                if alt.is_file():
                    edge_path = alt
                else:
                    continue
            out.append((name, edge_path, n, m))
    out.sort(key=lambda x: x[2])  # ASC by node count
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--node-limit", type=int, default=30000)
    ap.add_argument("--seed-count", type=int, default=50)
    ap.add_argument("--workers", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--working-tree", action="store_true")
    ap.add_argument("--max-networks", type=int, default=0,
                    help="cap network count (0 = unlimited)")
    ap.add_argument("--seed-base", type=int, default=1,
                    help="seeds run from seed_base..seed_base+seed_count-1")
    ap.add_argument("--stop-on-fail", action="store_true",
                    help="abort after first failure (for fast-fail diagnosis)")
    args = ap.parse_args()

    D.build_tracer(L4.HERE, L4.TRACER, script_name="build_l4.sh")
    if args.working_tree:
        web = L4.find_web_repo()
        louvain_js = web / "comdet" / "js" / "louvain" / "louvain.js"
        if not louvain_js.is_file():
            sys.exit(f"working-tree louvain.js not found: {louvain_js}")
        print(f"[working-tree] using {louvain_js}\n")
    else:
        louvain_js = L4.ensure_louvain_head_js()
    os.environ["LOUVAIN_JS"] = str(louvain_js)

    networks = discover_networks(args.node_limit)
    if args.max_networks > 0:
        networks = networks[:args.max_networks]
    seeds = list(range(args.seed_base, args.seed_base + args.seed_count))

    total_cells = len(networks) * len(seeds)
    print(f"empirical L4 stress: {len(networks)} networks × {len(seeds)} seeds = {total_cells} cells")
    print(f"  node_limit={args.node_limit}  workers={args.workers}")
    print()

    cells = []
    for name, edge, n, m in networks:
        for seed in seeds:
            cells.append((name, str(edge), seed, n, m))

    failures: list[tuple[str, int, str]] = []
    cumulative_visits = 0
    start = time.time()

    with ProcessPoolExecutor(max_workers=args.workers) as ex:
        fut_to_cell = {
            ex.submit(L4.run_cell, name, edge, seed): (name, seed, n, m)
            for name, edge, seed, n, m in cells
        }
        done = 0
        for fut in as_completed(fut_to_cell):
            name, seed, n, m = fut_to_cell[fut]
            res_name, res_seed, ok, visits, log = fut.result()
            done += 1
            cumulative_visits += visits
            if not ok:
                failures.append((name, seed, log))
                last = log.strip().splitlines()[-1] if log.strip() else "(no output)"
                print(f"  [{done}/{total_cells}] {name}(n={n},m={m}) seed={seed:>10}: FAIL visits={visits}  {last}")
                if args.stop_on_fail:
                    for f in fut_to_cell:
                        f.cancel()
                    break
            elif done % 50 == 0 or done == total_cells:
                elapsed = time.time() - start
                rate = done / elapsed if elapsed > 0 else 0
                print(f"  [{done}/{total_cells}] cumulative_visits={cumulative_visits} elapsed={elapsed:.0f}s rate={rate:.1f}cells/s")

    print()
    print(f"cumulative_visits={cumulative_visits}")
    print(f"failures={len(failures)}/{total_cells}")
    if failures:
        print("first 10 failures:")
        for name, seed, log in failures[:10]:
            last = log.strip().splitlines()[-1] if log.strip() else "(no output)"
            print(f"  {name} seed={seed}: {last}")
        return 1
    print("OVERALL: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
