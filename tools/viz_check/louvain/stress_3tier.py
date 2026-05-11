"""Louvain L4 bumped 3-tier empirical-network stress driver.

Runs the Louvain L4 byte-equal kernel cross-check (kernel_check_l4.py's
`run_cell`) on the bumped 3-tier panel from
`_common/empirical_panel.py` — every network in
`data/empirical_networks/networks/` with `n ≤ 30000` (161 fixtures
total), sweeping the standard 50-seed set per fixture.

Subsumes the heritage `kernel_check_l4_empirical.py` driver (now
dropped). 161 × 50 = **8050 cells per panel**.

Per (fixture, seed): cpp tracer (/tmp/louvain_l4_tracer) emits per-visit
trace JSON; kernel_check_l4.mjs replays the JS standalone and bit-
compares (level, pass, visit, v, fromComm, toComm, moved, dSbits,
dGainBits) + fineMembership + Q_final. PASS = 0 mismatches per cell.

Defaults to --working-tree (uncommitted L4 rework lives across the
community-detection cpp + mjs and the vltanh.github.io louvain.js;
HEAD-pin would gate against a stale JS).

Usage:
    python stress_3tier.py [--tiers T1,T2,T3] [--seeds s1,s2,...]
                           [--workers N] [--no-working-tree] [--quick]
"""
from __future__ import annotations

import argparse
import os
import sys
import time
from concurrent.futures import ProcessPoolExecutor, as_completed
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "_common"))
import driver as D  # noqa: E402
from empirical_panel import discover_panel, SEEDS_50  # noqa: E402

# Reuse the per-cell runner from kernel_check_l4.
sys.path.insert(0, str(HERE))
import kernel_check_l4 as L4  # noqa: E402

REPO = D.repo_root_from_here(HERE)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--tiers", default="T1,T2,T3")
    ap.add_argument("--seeds", default=",".join(str(s) for s in SEEDS_50))
    ap.add_argument("--workers", type=int, default=os.cpu_count() or 4,
                    help="parallel worker processes")
    ap.add_argument("--working-tree", dest="working_tree",
                    action="store_true", default=True,
                    help="(default ON) replay against working-tree "
                    "louvain.js. The L4 rework is uncommitted across CD "
                    "+ vltanh.github.io and is internally consistent.")
    ap.add_argument("--no-working-tree", dest="working_tree",
                    action="store_false",
                    help="replay against the HEAD-pinned louvain.js.")
    ap.add_argument("--quick", action="store_true",
                    help="3 seeds × 1 fixture per tier (smoke).")
    ap.add_argument("--max-per-tier", type=int, default=0,
                    help="cap fixture count per tier (0 = unlimited)")
    args = ap.parse_args()

    tiers = args.tiers.split(",")
    panel = discover_panel(REPO)
    if args.quick:
        seeds = SEEDS_50[:3]
        per_tier_cap = 1
    else:
        seeds = [int(s) for s in args.seeds.split(",")]
        per_tier_cap = args.max_per_tier or None

    # Build cpp tracer (no-op if up to date) + resolve louvain.js, set env
    # once before forking workers.
    D.build_tracer(L4.HERE, L4.TRACER, script_name="build_l4.sh")
    if args.working_tree:
        web = L4.find_web_repo()
        louvain_js = web / "comdet" / "js" / "louvain" / "louvain.js"
        if not louvain_js.is_file():
            sys.exit(f"working-tree louvain.js not found: {louvain_js}")
        common_js = web / "comdet" / "js" / "common" / "common.js"
        if not common_js.is_file():
            sys.exit(f"working-tree common.js not found: {common_js}")
        print(f"[working-tree] using {louvain_js}")
    else:
        louvain_js = L4.ensure_louvain_head_js()
        common_js = Path(L4.COMMON_JS_HEAD)
        print(f"[HEAD-pinned] using {louvain_js}")
    os.environ["LOUVAIN_JS"] = str(louvain_js)
    os.environ["COMMON_JS"] = str(common_js)

    # Build the cell list per tier (so per-tier counters work).
    tier_cells: dict[str, list[tuple[str, str, int]]] = {}
    for tier in tiers:
        if tier not in panel:
            sys.exit(f"unknown tier: {tier}")
        fixtures = panel[tier]
        if per_tier_cap is not None:
            fixtures = fixtures[:per_tier_cap]
        cells: list[tuple[str, str, int]] = []
        for fx in fixtures:
            for seed in seeds:
                cells.append((fx["subnet"], fx["edge"], seed))
        tier_cells[tier] = cells

    total_cells = sum(len(c) for c in tier_cells.values())
    print(f"L4 bumped 3-tier stress: tiers={','.join(tiers)} seeds={len(seeds)} "
          f"workers={args.workers} total_cells={total_cells}")
    for tier in tiers:
        print(f"  {tier}: {len(panel[tier])} fixtures")
    print()
    sys.stdout.flush()

    grand_pass = 0
    grand_fail = 0
    grand_visits = 0
    failed_cells: list[tuple[str, str, int, str]] = []
    t0 = time.time()

    for tier in tiers:
        cells = tier_cells[tier]
        if not cells:
            continue
        print(f"=== {tier} ({len(cells)} cells) ===")
        sys.stdout.flush()
        tier_pass = tier_fail = tier_visits = 0
        results: list[tuple[str, int, bool, int, str]] = []
        with ProcessPoolExecutor(max_workers=args.workers) as ex:
            futures = [ex.submit(L4.run_cell, *c) for c in cells]
            for fut in as_completed(futures):
                results.append(fut.result())
        results.sort(key=lambda r: (r[0], r[1]))
        for name, seed, ok, visits, log in results:
            tier_visits += visits
            if ok:
                tier_pass += 1
            else:
                tier_fail += 1
                last = log.strip().splitlines()[-1] if log.strip() else "(no output)"
                failed_cells.append((tier, name, seed, last))
                print(f"  FAIL {name} seed={seed}: {last}")
                sys.stdout.flush()
        grand_pass += tier_pass
        grand_fail += tier_fail
        grand_visits += tier_visits
        print(f"  {tier}: PASS={tier_pass} FAIL={tier_fail} visits={tier_visits:,}")
        sys.stdout.flush()

    elapsed = time.time() - t0
    print()
    print(f"=== SUMMARY ===")
    print(f"Cells: PASS={grand_pass} FAIL={grand_fail} (total={grand_pass + grand_fail})")
    print(f"Cumulative visits: {grand_visits:,}")
    print(f"Wall: {elapsed:.1f}s")
    if failed_cells:
        print()
        print("Failed cells:")
        for tier, name, seed, last in failed_cells[:50]:
            print(f"  {tier}/{name} seed={seed}: {last}")
    if grand_fail == 0 and grand_pass > 0:
        print("OVERALL: PASS")
    elif grand_pass == 0:
        print("OVERALL: NO CELLS RUN")
    else:
        print("OVERALL: FAIL")
    return 0 if grand_fail == 0 and grand_pass > 0 else 1


if __name__ == "__main__":
    sys.exit(main())
