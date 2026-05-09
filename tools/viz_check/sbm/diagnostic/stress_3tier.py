#!/usr/bin/env python3
"""SBM 3-tier stress driver.

Runs the SBM byte-equal kernel cross-check (kernel_check.py) on the
17-fixture undirected-non-bipartite panel from
reference_cd_stress_tiers.md, sweeping seeds × modes per tier.

Per (fixture, seed): synthesizes a K-block init membership via stdlib
RandomState(seed), writes /tmp/sbm_stress_<basename>_<seed>_com.csv,
then invokes kernel_check.py via --graph for each mode.

Outputs PASS/FAIL tally + cumulative visits per tier.

Skips nested variants: the cpp tracer's B=N capacity has a documented
latent OOB on small nested graphs (`audit.md` "Known latent limit").
Flat-only.

Usage:
    python stress_3tier.py [--tiers T1,T2,T3] [--seeds N] [--sweeps N,N,N]
                           [--modes dc,ndc,pp] [--K 8] [--quick]
"""
from __future__ import annotations

import argparse
import json
import random
import subprocess
import sys
import time
from pathlib import Path

CD = Path(__file__).resolve().parents[4]            # community-detection/
REPO = CD.parent                                     # netsci-research/
NETS = REPO / "data" / "empirical_networks" / "networks"
KERNEL_CHECK = CD / "tools" / "viz_check" / "sbm" / "kernel_check.py"

TIERS = {
    "T1": [
        ("copenhagen", "copenhagen_fb_friends"),
        ("product_space", "product_space_HS"),
        ("dnc", "dnc"),
        ("euroroad", "euroroad"),
        ("facebook_organizations", "facebook_organizations_M1"),
        ("netscience", "netscience"),
        ("new_zealand_collab", "new_zealand_collab"),
        ("collins_yeast", "collins_yeast"),
        ("bible_nouns", "bible_nouns"),
        ("interactome_yeast", "interactome_yeast"),
        ("drosophila_flybi", "drosophila_flybi"),
    ],
    "T2": [
        ("arxiv_authors", "arxiv_authors_HepTh"),
        ("sp_infectious", "sp_infectious"),
        ("arxiv_authors", "arxiv_authors_HepPh"),
        ("physics_collab", "physics_collab_arXiv"),
    ],
    "T3": [
        ("internet_as", "internet_as"),
        ("arxiv_authors", "arxiv_authors_CondMat"),
    ],
}

DEFAULT_SEEDS_9 = [1, 7, 13, 42, 99, 137, 1729, 65535, 2147483646]


def synth_init(edge_path: Path, seed: int, K: int) -> Path:
    """Generate /tmp/sbm_stress_<base>_<seed>_com.csv with random K-block
    init membership over node ids found in edge.csv. Each unique node id
    gets one of K labels via Random(seed).randint(0, K-1)."""
    base = edge_path.stem
    out = Path(f"/tmp/sbm_stress_{base}_{seed}_com.csv")
    nodes = set()
    with edge_path.open() as f:
        header = f.readline()  # discard
        for line in f:
            line = line.strip()
            if not line: continue
            parts = line.replace("\t", ",").split(",")
            if len(parts) < 2: continue
            nodes.add(parts[0].strip())
            nodes.add(parts[1].strip())
    rng = random.Random(seed)
    with out.open("w") as f:
        f.write("node_id,cluster_id\n")
        for n in sorted(nodes):
            f.write(f"{n},{rng.randint(0, K - 1)}\n")
    return out


def run_cell(name: str, edge: Path, com: Path, mode: str, seed: int,
             sweeps: int, verbose: bool = False) -> tuple[bool, int, str]:
    """Returns (pass, visits, last_line_of_output)."""
    cmd = [
        sys.executable, str(KERNEL_CHECK),
        "--mode", mode,
        "--seed", str(seed),
        "--sweeps", str(sweeps),
        "--no-canonical-check",
        "--graph", f"{name}:{edge}:{com}",
    ]
    rc = subprocess.run(cmd, capture_output=True, text=True, timeout=900)
    out = rc.stdout + rc.stderr
    passed = "OVERALL: PASS" in out
    # Parse visits from "n=N sweeps=K visits=V" line in JS replay output.
    visits = 0
    for line in out.splitlines():
        if "visits=" in line:
            try:
                visits = max(visits, int(line.split("visits=")[1].split()[0]))
            except (ValueError, IndexError):
                pass
    last = out.strip().splitlines()[-1] if out.strip() else "(no output)"
    if verbose and not passed:
        sys.stderr.write(out + "\n")
    return passed, visits, last


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tiers", default="T1,T2,T3")
    ap.add_argument("--seeds", default="9", help="9 = the standard 9-seed minimum")
    ap.add_argument("--sweeps", default="10,5,2", help="per-tier comma list")
    ap.add_argument("--modes", default="dc,ndc,pp")
    ap.add_argument("--K", type=int, default=8, help="initial block count")
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--quick", action="store_true",
                    help="3 seeds, 2 sweeps, 1 fixture per tier (smoke)")
    args = ap.parse_args()

    if args.quick:
        seeds = DEFAULT_SEEDS_9[:3]
        sweeps = [2, 2, 2]
        tiers = ["T1", "T2", "T3"]
        fixtures_per_tier = 1
    else:
        seeds = DEFAULT_SEEDS_9 if args.seeds == "9" else [int(s) for s in args.seeds.split(",")]
        sweeps = [int(s) for s in args.sweeps.split(",")]
        tiers = args.tiers.split(",")
        fixtures_per_tier = None  # all

    modes = args.modes.split(",")

    t0 = time.time()
    grand_pass = 0
    grand_fail = 0
    grand_visits = 0
    failed_cells: list[str] = []

    for ti, tier in enumerate(tiers):
        sweep = sweeps[ti] if ti < len(sweeps) else sweeps[-1]
        fixtures = TIERS[tier]
        if fixtures_per_tier is not None:
            fixtures = fixtures[:fixtures_per_tier]
        tier_pass = tier_fail = tier_visits = 0
        print(f"\n=== {tier} (sweeps={sweep}, {len(fixtures)} fixtures, "
              f"{len(seeds)} seeds, {len(modes)} modes = "
              f"{len(fixtures) * len(seeds) * len(modes)} cells) ===")
        sys.stdout.flush()
        for subdir, base in fixtures:
            edge = NETS / subdir / f"{base}.csv"
            if not edge.exists():
                print(f"  SKIP {base}: edge file missing at {edge}")
                continue
            for seed in seeds:
                com = synth_init(edge, seed, args.K)
                for mode in modes:
                    passed, visits, last = run_cell(base, edge, com, mode,
                                                    seed, sweep, args.verbose)
                    grand_visits += visits
                    tier_visits += visits
                    if passed:
                        grand_pass += 1
                        tier_pass += 1
                    else:
                        grand_fail += 1
                        tier_fail += 1
                        failed_cells.append(f"{tier}/{base}/seed={seed}/mode={mode}: {last}")
                        print(f"  FAIL {base} seed={seed} mode={mode}: {last}")
                        sys.stdout.flush()
        print(f"  {tier}: PASS={tier_pass} FAIL={tier_fail} visits={tier_visits:,}")
        sys.stdout.flush()

    elapsed = time.time() - t0
    print(f"\n=== SUMMARY ===")
    print(f"Cells: PASS={grand_pass} FAIL={grand_fail}  Visits: {grand_visits:,}  "
          f"Wall: {elapsed:.1f}s")
    if failed_cells:
        print("\nFailed cells:")
        for f in failed_cells[:50]:
            print(f"  {f}")
    sys.exit(0 if grand_fail == 0 else 1)


if __name__ == "__main__":
    main()
