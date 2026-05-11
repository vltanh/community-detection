#!/usr/bin/env python3
"""SBM bumped 3-tier stress driver.

Runs the SBM byte-equal kernel cross-check on the bumped 3-tier panel
(every network in `data/empirical_networks/networks/` with `n ≤ 30000`
per `_common/empirical_panel.py`; 161 fixtures, 50 seeds = 8050 cells
per mode), sweeping seeds × modes per tier.

Two mode families:

- **Flat** (--modes dc,ndc,pp): cpp tracer `flat_traced --mode=<m>` +
  JS `self_rng_check.mjs <trace> <edge> <mode>`. Synth init uses a
  fixed K-block partition (--K, default 8).

- **Nested** (--nested-modes dc,ndc): cpp tracer
  `flat_traced --nested --auto-levels=<L> --mode=<m>` + JS
  `self_rng_check.mjs <trace> <edge> <mode> --nested`. Synth init
  uses B0 = ceil(sqrt(N)) per fixture to keep the level-0 block count
  well below the dense-array B=N capacity (avoids the OOB latent
  documented in `audit.md` "Known latent limit").

Per (fixture, seed, mode): synthesizes init membership via stdlib
Random(seed), writes /tmp/sbm_stress_<basename>_<seed>_com.csv,
invokes cpp tracer (flat or nested), then JS self-RNG check.

Outputs PASS/FAIL tally + cumulative visits per tier per family.

Usage:
    python stress_3tier.py [--tiers T1,T2,T3] [--seeds s1,...]
                           [--sweeps N,N,N]
                           [--modes dc,ndc,pp] [--nested-modes dc,ndc]
                           [--nested-sweeps N,N,N] [--auto-levels 4]
                           [--K 8] [--quick]
"""
from __future__ import annotations

import argparse
import math
import random
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
# Path layout: community-detection/tools/viz_check/sbm/diagnostic/stress_3tier.py
# 4 parents up = community-detection/
CD = HERE.parents[3]
sys.path.insert(0, str(CD / "tools" / "viz_check" / "_common"))
from empirical_panel import discover_panel, SEEDS_50  # noqa: E402

REPO = CD                                            # community-detection/
TRACER = "/tmp/sbm_flat_kernel_check"
SELF_RNG_CHECK = CD / "tools" / "viz_check" / "sbm" / "self_rng_check.mjs"


def collect_nodes(edge_path: Path) -> list[str]:
    """Return sorted list of unique node ids found in edge.csv."""
    nodes: set[str] = set()
    with edge_path.open() as f:
        f.readline()  # discard header
        for line in f:
            line = line.strip()
            if not line: continue
            parts = line.replace("\t", ",").split(",")
            if len(parts) < 2: continue
            nodes.add(parts[0].strip())
            nodes.add(parts[1].strip())
    return sorted(nodes)


def synth_init(edge_path: Path, seed: int, K: int) -> tuple[Path, int]:
    """Generate /tmp/sbm_stress_<base>_<seed>_K<K>_com.csv with random
    K-block init membership over node ids found in edge.csv. Each unique
    node id gets one of K labels via Random(seed).randint(0, K-1).
    Returns (path, N) where N = node count."""
    base = edge_path.stem
    nodes = collect_nodes(edge_path)
    N = len(nodes)
    out = Path(f"/tmp/sbm_stress_{base}_{seed}_K{K}_com.csv")
    rng = random.Random(seed)
    with out.open("w") as f:
        f.write("node_id,cluster_id\n")
        for n in nodes:
            f.write(f"{n},{rng.randint(0, K - 1)}\n")
    return out, N


def run_cell(name: str, edge: Path, com: Path, mode: str, seed: int,
             sweeps: int, *, nested: bool = False, auto_levels: int = 4,
             verbose: bool = False) -> tuple[bool, int, str]:
    """Two legs:
      1. cpp tracer emits trace JSON to /tmp/sbm_stress_trace.json.
         Adds --nested --auto-levels=<L> when nested=True.
      2. self_rng_check.mjs compares JS standalone vs cpp via raw bit
         equality (BigUint64Array reinterpret) — TRUE byte-equal. Adds
         --nested when nested=True.
    Returns (pass, visits, last_line_of_output).
    """
    family = "nested" if nested else "flat"
    trace_path = f"/tmp/sbm_stress_{name}_{seed}_{family}_{mode}_trace.json"
    cmd = [TRACER, f"--mode={mode}", f"--edge={edge}", f"--init={com}",
           f"--seed={seed}", f"--sweeps={sweeps}"]
    if nested:
        cmd.append("--nested")
        cmd.append(f"--auto-levels={auto_levels}")
    with open(trace_path, "w") as fout:
        rc1 = subprocess.run(
            cmd, stdout=fout, stderr=subprocess.PIPE, text=True, timeout=900,
        )
    if rc1.returncode != 0:
        return False, 0, f"cpp tracer failed: {rc1.stderr.strip()[:200]}"
    js_cmd = ["node", str(SELF_RNG_CHECK), trace_path, str(edge), mode]
    if nested:
        js_cmd.append("--nested")
    rc2 = subprocess.run(
        js_cmd, capture_output=True, text=True, timeout=900,
    )
    out = rc2.stdout + rc2.stderr
    passed = (rc2.returncode == 0)
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


def run_panel(panel: dict, tiers: list[str], seeds: list[int],
              modes: list[str], sweeps: list[int], *, nested: bool,
              auto_levels: int, K_flat: int, per_tier_cap: int | None,
              verbose: bool, family_label: str) -> tuple[int, int, int]:
    """Run the bumped 3-tier panel for one mode family (flat or nested).
    Returns (passes, fails, total_visits)."""
    if not modes:
        return 0, 0, 0
    print(f"\n#### {family_label} family ####")
    sys.stdout.flush()
    grand_pass = grand_fail = grand_visits = 0
    failed_cells: list[str] = []
    for ti, tier in enumerate(tiers):
        sweep = sweeps[ti] if ti < len(sweeps) else sweeps[-1]
        fixtures = panel[tier]
        if per_tier_cap is not None:
            fixtures = fixtures[:per_tier_cap]
        tier_pass = tier_fail = tier_visits = 0
        print(f"\n=== {family_label} {tier} (sweeps={sweep}, "
              f"{len(fixtures)} fixtures, {len(seeds)} seeds, "
              f"{len(modes)} modes = "
              f"{len(fixtures) * len(seeds) * len(modes)} cells) ===")
        sys.stdout.flush()
        for fx in fixtures:
            base = fx["subnet"]
            edge = Path(fx["edge"])
            # Pick K per-fixture: nested → ceil(sqrt(N)); flat → K_flat.
            if nested:
                _, N = synth_init(edge, seeds[0], 2)  # probe N
                K = max(2, math.ceil(math.sqrt(N)))
            else:
                K = K_flat
            for seed in seeds:
                com, _ = synth_init(edge, seed, K)
                for mode in modes:
                    passed, visits, last = run_cell(
                        base, edge, com, mode, seed, sweep,
                        nested=nested, auto_levels=auto_levels,
                        verbose=verbose,
                    )
                    grand_visits += visits
                    tier_visits += visits
                    if passed:
                        grand_pass += 1
                        tier_pass += 1
                    else:
                        grand_fail += 1
                        tier_fail += 1
                        failed_cells.append(
                            f"{family_label}/{tier}/{base}/seed={seed}/"
                            f"mode={mode}: {last}")
                        print(f"  FAIL {base} seed={seed} mode={mode}: {last}")
                        sys.stdout.flush()
        print(f"  {family_label} {tier}: PASS={tier_pass} FAIL={tier_fail} "
              f"visits={tier_visits:,}")
        sys.stdout.flush()
    if failed_cells:
        print(f"\n{family_label} failed cells:")
        for f in failed_cells[:50]:
            print(f"  {f}")
    return grand_pass, grand_fail, grand_visits


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tiers", default="T1,T2,T3")
    ap.add_argument("--seeds", default="50",
                    help="50 = the standard 50-seed sweep, or comma list of ints")
    ap.add_argument("--sweeps", default="5,3,2",
                    help="per-tier flat sweep counts (comma list)")
    ap.add_argument("--modes", default="dc,ndc,pp",
                    help="flat modes (comma list); '' to skip flat")
    ap.add_argument("--nested-modes", default="",
                    help="nested modes (comma list, e.g. dc,ndc); "
                         "empty disables nested family")
    ap.add_argument("--nested-sweeps", default="3,2,1",
                    help="per-tier nested sweep counts; bounded to keep "
                         "B<N capacity within sweep budget")
    ap.add_argument("--auto-levels", type=int, default=4,
                    help="nested hierarchy depth cap (passed to cpp tracer)")
    ap.add_argument("--K", type=int, default=8,
                    help="flat-family initial block count")
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--quick", action="store_true",
                    help="3 seeds, 2 sweeps, 1 fixture per tier (smoke)")
    ap.add_argument("--max-per-tier", type=int, default=0,
                    help="cap fixture count per tier (0 = unlimited)")
    args = ap.parse_args()

    panel = discover_panel(REPO)
    if args.quick:
        seeds = SEEDS_50[:3]
        flat_sweeps = [2, 2, 2]
        nested_sweeps = [2, 2, 2]
        tiers = ["T1", "T2", "T3"]
        per_tier_cap = 1
    else:
        if args.seeds == "50":
            seeds = list(SEEDS_50)
        else:
            seeds = [int(s) for s in args.seeds.split(",")]
        flat_sweeps = [int(s) for s in args.sweeps.split(",")]
        nested_sweeps = [int(s) for s in args.nested_sweeps.split(",")]
        tiers = args.tiers.split(",")
        per_tier_cap = args.max_per_tier or None

    flat_modes = [m for m in args.modes.split(",") if m]
    nested_modes = [m for m in args.nested_modes.split(",") if m]

    t0 = time.time()

    flat_pass, flat_fail, flat_visits = run_panel(
        panel, tiers, seeds, flat_modes, flat_sweeps,
        nested=False, auto_levels=args.auto_levels, K_flat=args.K,
        per_tier_cap=per_tier_cap, verbose=args.verbose,
        family_label="flat",
    )
    nested_pass, nested_fail, nested_visits = run_panel(
        panel, tiers, seeds, nested_modes, nested_sweeps,
        nested=True, auto_levels=args.auto_levels, K_flat=args.K,
        per_tier_cap=per_tier_cap, verbose=args.verbose,
        family_label="nested",
    )

    elapsed = time.time() - t0
    grand_pass = flat_pass + nested_pass
    grand_fail = flat_fail + nested_fail
    grand_visits = flat_visits + nested_visits

    print(f"\n=== SUMMARY ===")
    if flat_modes:
        print(f"  flat:   PASS={flat_pass} FAIL={flat_fail} "
              f"visits={flat_visits:,}")
    if nested_modes:
        print(f"  nested: PASS={nested_pass} FAIL={nested_fail} "
              f"visits={nested_visits:,}")
    print(f"  TOTAL:  PASS={grand_pass} FAIL={grand_fail} "
          f"visits={grand_visits:,}  Wall: {elapsed:.1f}s")
    sys.exit(0 if grand_fail == 0 else 1)


if __name__ == "__main__":
    main()
