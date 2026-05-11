"""IKC bumped 3-tier byte-equal stress driver.

Iterates the bumped 3-tier panel (every network in
`data/empirical_networks/networks/` with `n ≤ 30000` per
`_common/empirical_panel.py`; 161 fixtures) x k_floor sweep, invokes
diff.mjs per cell, tallies PASS/FAIL + records. IKC has no RNG so the
matrix is fixtures × k_floor only (no seed sweep).

Default k_floor sweep: {2, 3, 5, 8, 10}. Cells = 161 × 5 = 805.

(IKC's variant axis is k_floor, not seed; total cell count differs from
the other algos which sweep 50 seeds × 1 variant = 8050.)

Run:
    python tools/viz_check/ikc/stress_3tier.py [--tiers T1,T2,T3]
        [--ks 2,3,5,8,10] [--timeout 600] [--report <out.tsv>]
"""
from __future__ import annotations

import argparse
import re
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "_common"))
import driver as D  # noqa: E402
from empirical_panel import discover_panel, tier_for  # noqa: E402

REPO = D.repo_root_from_here(HERE)
DIFF = HERE / "diff.mjs"


def parse_args() -> argparse.Namespace:
    ap = argparse.ArgumentParser()
    ap.add_argument("--tiers", default="T1,T2,T3",
                    help="comma-list of T1/T2/T3 to include (default: all)")
    ap.add_argument("--ks", default="2,3,5,8,10",
                    help="comma-list of k_floor values (default: 2,3,5,8,10)")
    ap.add_argument("--timeout", type=int, default=600,
                    help="per-cell timeout seconds (default: 600)")
    ap.add_argument("--report", default=None,
                    help="optional TSV report path")
    ap.add_argument("--bail", action="store_true",
                    help="stop at first failing cell")
    ap.add_argument("--max-per-tier", type=int, default=0,
                    help="cap fixture count per tier (0 = unlimited)")
    return ap.parse_args()


def run_cell(edge_csv: Path, k_floor: int, fixture_name: str,
             timeout: int) -> tuple[str, int, float, str]:
    """Run diff.mjs for one cell. Returns (status, records, elapsed, last_line)."""
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
    panel = discover_panel(REPO)
    panel_flat = []
    for t in ("T1", "T2", "T3"):
        if t not in tiers:
            continue
        fixtures = panel[t]
        if args.max_per_tier:
            fixtures = fixtures[:args.max_per_tier]
        for fx in fixtures:
            panel_flat.append((fx["subnet"], fx["edge"], fx["n"], fx["m"], t))

    if not panel_flat:
        sys.exit(f"no fixtures matched tiers={tiers}")

    print(f"# IKC bumped 3-tier stress: {len(panel_flat)} fixtures x {len(ks)} k_floor "
          f"= {len(panel_flat) * len(ks)} cells")
    print(f"# tiers={','.join(sorted(tiers))} ks={ks} timeout={args.timeout}s")
    print()

    rows = []
    pass_n = fail_n = timeout_n = 0
    total_records = 0
    t_start = time.monotonic()

    for (basename, edge_str, n, m, tier) in panel_flat:
        edge = Path(edge_str)
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
