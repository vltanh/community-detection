"""Louvain L4 3-tier empirical-network stress driver.

Runs the Louvain L4 byte-equal kernel cross-check (kernel_check_l4.py's
`run_cell`) on the standard 17-fixture undirected non-bipartite panel
from reference_cd_stress_tiers.md, sweeping the standard 9 seeds per
fixture.

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
import driver as D

# Reuse the per-cell runner from kernel_check_l4.
sys.path.insert(0, str(HERE))
import kernel_check_l4 as L4

REPO = D.repo_root_from_here(HERE)
NETS = REPO.parent / "data" / "empirical_networks" / "networks"

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

DEFAULT_SEEDS = [1, 7, 13, 42, 99, 137, 1729, 65535, 2147483646]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--tiers", default="T1,T2,T3")
    ap.add_argument("--seeds", default=",".join(str(s) for s in DEFAULT_SEEDS))
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
    args = ap.parse_args()

    tiers = args.tiers.split(",")
    if args.quick:
        seeds = DEFAULT_SEEDS[:3]
        fixtures_per_tier = 1
    else:
        seeds = [int(s) for s in args.seeds.split(",")]
        fixtures_per_tier = None  # all

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
    skipped: list[str] = []
    for tier in tiers:
        if tier not in TIERS:
            sys.exit(f"unknown tier: {tier}")
        fixtures = TIERS[tier]
        if fixtures_per_tier is not None:
            fixtures = fixtures[:fixtures_per_tier]
        cells: list[tuple[str, str, int]] = []
        for subdir, base in fixtures:
            edge = NETS / subdir / f"{base}.csv"
            if not edge.is_file():
                skipped.append(f"{tier}/{base} (missing {edge})")
                continue
            for seed in seeds:
                cells.append((base, str(edge), seed))
        tier_cells[tier] = cells

    total_cells = sum(len(c) for c in tier_cells.values())
    print(f"L4 3-tier stress: tiers={','.join(tiers)} seeds={len(seeds)} "
          f"workers={args.workers} total_cells={total_cells}")
    for s in skipped:
        print(f"  SKIP {s}")
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
