"""IKC byte-equal stress matrix runner.

Iterates a fixture panel x k_floor sweep, invokes diff.mjs per cell, tallies
PASS/FAIL + records. IKC has no RNG, so the matrix is fixtures x k_floor
only (no seed sweep).

Default panel: cd_stress_tiers T1 (11) + T2 (4) + T3 (2) = 17 fixtures.
Default k_floor sweep: {2, 3, 5, 8, 10}. Cell = 17 * 5 = 85.

Run:
    python tools/viz_check/ikc/stress.py [--tiers T1,T2,T3] [--ks 2,3,5,8,10]
        [--timeout 600] [--fixtures-root <path>] [--report <out.tsv>]

Exit code: 0 if all cells PASS, 1 otherwise. Per-cell stdout PASS/FAIL.
Final summary printed to stdout. Optional TSV report via --report.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent.parent  # community-detection/
DIFF = HERE / "diff.mjs"

# Per cd_stress_tiers reference. (basename, subdir, n, m, tier).
PANEL = [
    # T1
    ("copenhagen_fb_friends",        "copenhagen",              800,    6418, "T1"),
    ("product_space_HS",             "product_space",           866,    2532, "T1"),
    ("dnc",                          "dnc",                     906,   10429, "T1"),
    ("euroroad",                     "euroroad",               1174,    1417, "T1"),
    ("facebook_organizations_M1",    "facebook_organizations", 1429,   19357, "T1"),
    ("netscience",                   "netscience",             1461,    2742, "T1"),
    ("new_zealand_collab",           "new_zealand_collab",     1511,    4273, "T1"),
    ("collins_yeast",                "collins_yeast",          1622,    9070, "T1"),
    ("bible_nouns",                  "bible_nouns",            1773,    9131, "T1"),
    ("interactome_yeast",            "interactome_yeast",      1846,    2203, "T1"),
    ("drosophila_flybi",             "drosophila_flybi",       2906,    8569, "T1"),
    # T2
    ("arxiv_authors_HepTh",          "arxiv_authors",          9875,   25973, "T2"),
    ("sp_infectious",                "sp_infectious",         10972,   44517, "T2"),
    ("arxiv_authors_HepPh",          "arxiv_authors",         12006,  118489, "T2"),
    ("physics_collab_arXiv",         "physics_collab",        14065,   35175, "T2"),
    # T3
    ("internet_as",                  "internet_as",           22963,   48436, "T3"),
    ("arxiv_authors_CondMat",        "arxiv_authors",         23133,   93439, "T3"),
]


def parse_args() -> argparse.Namespace:
    ap = argparse.ArgumentParser()
    ap.add_argument("--tiers", default="T1,T2,T3",
                    help="comma-list of T1/T2/T3 to include (default: all)")
    ap.add_argument("--ks", default="2,3,5,8,10",
                    help="comma-list of k_floor values (default: 2,3,5,8,10)")
    ap.add_argument("--timeout", type=int, default=600,
                    help="per-cell timeout seconds (default: 600)")
    ap.add_argument("--fixtures-root", default=str(REPO.parent / "data" / "empirical_networks" / "networks"),
                    help="root of empirical-networks fixture tree")
    ap.add_argument("--report", default=None,
                    help="optional TSV report path")
    ap.add_argument("--bail", action="store_true",
                    help="stop at first failing cell")
    return ap.parse_args()


def run_cell(edge_csv: Path, k_floor: int, fixture_name: str,
             timeout: int) -> tuple[str, int, float, str]:
    """Run diff.mjs for one cell. Returns (status, records, elapsed, last_line).
    status in {"PASS", "FAIL", "TIMEOUT", "ERROR"}.
    """
    t0 = time.monotonic()
    try:
        rc = subprocess.run(
            ["node", str(DIFF), str(edge_csv), str(k_floor),
             "--fixture-name", fixture_name],
            capture_output=True, text=True, timeout=timeout,
        )
    except subprocess.TimeoutExpired:
        return ("TIMEOUT", 0, time.monotonic() - t0, "timeout")
    elapsed = time.monotonic() - t0
    out = (rc.stdout or "").strip()
    err = (rc.stderr or "").strip()
    last = out.splitlines()[-1] if out else (err.splitlines()[-1] if err else "")
    if rc.returncode != 0:
        # diff harness prints FAIL line on stdout, divergence dump on stderr
        m = re.search(r"records \(py\): (\d+)", out + "\n" + err)
        records = int(m.group(1)) if m else 0
        return ("FAIL", records, elapsed, last)
    m = re.search(r"records=(\d+)", out)
    records = int(m.group(1)) if m else 0
    return ("PASS", records, elapsed, last)


def main() -> int:
    args = parse_args()
    tiers = set(args.tiers.split(","))
    ks = [int(x) for x in args.ks.split(",")]
    fixtures_root = Path(args.fixtures_root)

    panel = [p for p in PANEL if p[4] in tiers]
    if not panel:
        sys.exit(f"no fixtures matched tiers={tiers}")

    print(f"# stress matrix: {len(panel)} fixtures x {len(ks)} k_floor = {len(panel) * len(ks)} cells")
    print(f"# tiers={','.join(sorted(tiers))} ks={ks} timeout={args.timeout}s")
    print()

    rows = []
    pass_n = fail_n = timeout_n = 0
    total_records = 0
    t_start = time.monotonic()

    for (basename, subdir, n, m, tier) in panel:
        edge = fixtures_root / subdir / f"{basename}.csv"
        if not edge.exists():
            print(f"[{tier}] {basename}: SKIP (missing {edge})")
            continue
        for k in ks:
            status, records, elapsed, last = run_cell(edge, k, basename, args.timeout)
            print(f"[{tier}] {basename} k={k} n={n} m={m}: {status} "
                  f"records={records} {elapsed:.1f}s | {last[:80]}")
            rows.append({
                "tier": tier, "fixture": basename, "n": n, "m": m,
                "k_floor": k, "status": status, "records": records,
                "elapsed_s": round(elapsed, 2), "summary": last,
            })
            if status == "PASS":
                pass_n += 1
                total_records += records
            elif status == "TIMEOUT":
                timeout_n += 1
            else:
                fail_n += 1
                if args.bail:
                    break
        if args.bail and fail_n:
            break

    t_elapsed = time.monotonic() - t_start
    print()
    print(f"=== summary ===")
    print(f"cells: {len(rows)}  PASS: {pass_n}  FAIL: {fail_n}  TIMEOUT: {timeout_n}")
    print(f"cumulative records: {total_records:,}")
    print(f"wall: {t_elapsed:.1f}s")

    # tier breakdown
    by_tier = {}
    for r in rows:
        by_tier.setdefault(r["tier"], {"PASS": 0, "FAIL": 0, "TIMEOUT": 0, "records": 0})
        by_tier[r["tier"]][r["status"]] = by_tier[r["tier"]].get(r["status"], 0) + 1
        if r["status"] == "PASS":
            by_tier[r["tier"]]["records"] += r["records"]
    for tier in sorted(by_tier.keys()):
        b = by_tier[tier]
        print(f"  {tier}: PASS={b['PASS']} FAIL={b['FAIL']} TIMEOUT={b['TIMEOUT']} "
              f"records={b['records']:,}")

    if args.report:
        with open(args.report, "w") as f:
            f.write("tier\tfixture\tn\tm\tk_floor\tstatus\trecords\telapsed_s\tsummary\n")
            for r in rows:
                f.write(f"{r['tier']}\t{r['fixture']}\t{r['n']}\t{r['m']}\t"
                        f"{r['k_floor']}\t{r['status']}\t{r['records']}\t"
                        f"{r['elapsed_s']}\t{r['summary']}\n")
        print(f"  report: {args.report}")

    return 0 if (fail_n == 0 and timeout_n == 0) else 1


if __name__ == "__main__":
    sys.exit(main())
