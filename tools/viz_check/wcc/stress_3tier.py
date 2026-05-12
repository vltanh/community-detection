"""WCC L4 bumped 3-tier empirical-network stress driver.

Chains a community-detection algorithm (Leiden-Mod, Leiden-CPM, or
SBM-Flat-PP) to produce the input partition, then runs the WCC tracer +
JS self-RNG check on that partition. Per (fixture, seed, criterion,
partitioner) cell:

  Partitioner = leiden-mod:
    leiden_kernel_check <edge.csv> <out.csv> mod 1.0 <seed> 1
      → partition CSV (node_id,cluster_id) at out.csv
  Partitioner = sbm-flat-pp:
    sbm_flat_kernel_check --mode=pp --edge=<edge.csv> --init=<init.csv>
                          --seed=<seed> --sweeps=<S>
      → JSON to stdout containing renum_to_orig + final_membership
      → post-processed into a (node_id,cluster_id) CSV
    Init partition is a K-block random assignment over orig node ids
    via stdlib Random(seed).randint(0, K-1); same scheme as
    tools/viz_check/sbm/diagnostic/stress_3tier.py.

  Then for both partitioners:
    wcc_kernel_check_swapped <edge.csv> <com.csv> <out.csv>
                             <criterion> cactus <seed>
      → trace JSON on stdout
    node self_rng_check.mjs <trace.json> <edge.csv> <com.csv>
                            <criterion> <seed>
      → bit-equal lockstep check; exit 0 = PASS

Panel = bumped 3-tier (every network in
`data/empirical_networks/networks/` with `n ≤ 30000` per
`_common/empirical_panel.py`; 161 fixtures, 50 seeds = 8050 cells per
partitioner per criterion). Seeds skip 0 per igraph quirk inherited via
VieCut chain. Criterion sweep: {1log_10(n), 2log_10(n), 1log_2(n)} —
same as legacy synth-partition stress matrix at
tools/viz_check/wcc/stress_matrix.sh.

PASS = 0 mismatches per cell across:
  - row-E sub-term probes (pre_log_bits, log_n_bits) lockstep
  - threshold uint64 reinterpret (thr_bit_mm)
  - per-pop cluster size, cut value, well-connected verdict
  - in/out partition labels
  - survivors set
  - pop count parity

Usage:
    python stress_3tier.py [--tiers T1,T2,T3] [--seeds s1,...]
                            [--criteria c1,c2,c3] [--workers N]
                            [--partitioners leiden-mod,leiden-cpm,sbm-flat-pp]
                            [--leiden-cpm-resolution 0.5]
                            [--sbm-K 8] [--sbm-sweeps 10,5,2]
                            [--quick] [--timeout 1200]
"""
from __future__ import annotations

import argparse
import json
import os
import random
import subprocess
import sys
import time
from concurrent.futures import ProcessPoolExecutor, as_completed
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "_common"))
import driver as D  # noqa: E402
from empirical_panel import discover_panel, SEEDS_50  # noqa: E402

REPO = D.repo_root_from_here(HERE)
LEIDEN_TRACER = Path("/tmp/leiden_kernel_check")
SBM_TRACER = Path("/tmp/sbm_flat_kernel_check")
WCC_TRACER = Path("/tmp/wcc_kernel_check_swapped")
CHECK = HERE / "self_rng_check.mjs"

# Tier ordinal used to pick the per-tier sweep count for sbm-flat-pp.
TIER_ORDER = ["T1", "T2", "T3"]

DEFAULT_CRITERIA = ["1log_10(n)", "2log_10(n)", "1log_2(n)"]
DEFAULT_PARTITIONERS = ["leiden-mod", "leiden-cpm", "sbm-flat-pp"]
# Leiden-Mod: param=1.0 (no gamma).
LEIDEN_MOD_PARAM = "1.0"
# Leiden-CPM: gamma default 0.5 (matches CC/CM/VieCut symmetric panel).
DEFAULT_LEIDEN_CPM_RESOLUTION = 0.5
# SBM-Flat-PP defaults; same K=8 + per-tier sweeps as the upstream
# tools/viz_check/sbm/diagnostic/stress_3tier.py driver.
SBM_K = 8
SBM_SWEEPS_PER_TIER = {"T1": 10, "T2": 5, "T3": 2}


def _collect_orig_nodes(edge_path: Path) -> list[str]:
    """First-encounter orig-id list — matches the order both Leiden's
    igraph load + SBM tracer's `idToOrig` use to assign new ids."""
    seen: set[str] = set()
    nodes: list[str] = []
    with edge_path.open() as f:
        header = f.readline()  # discard
        for line in f:
            line = line.strip()
            if not line:
                continue
            parts = line.replace("\t", ",").split(",")
            if len(parts) < 2:
                continue
            s, t = parts[0].strip(), parts[1].strip()
            if s not in seen:
                seen.add(s); nodes.append(s)
            if t not in seen:
                seen.add(t); nodes.append(t)
    return nodes


def _synth_sbm_init(edge_path: Path, init_path: Path, seed: int, K: int) -> None:
    """Write /tmp/.../init.csv: K-block random init keyed on orig node
    ids found in edge.csv. Mirrors tools/viz_check/sbm/diagnostic/
    stress_3tier.py:synth_init (sorted-orig walk, Random(seed))."""
    nodes_sorted = sorted(_collect_orig_nodes(edge_path))
    rng = random.Random(seed)
    with init_path.open("w") as f:
        f.write("node_id,cluster_id\n")
        for n in nodes_sorted:
            f.write(f"{n},{rng.randint(0, K - 1)}\n")


def _sbm_final_to_com(trace_json: Path, com_path: Path) -> bool:
    """Read SBM tracer JSON; write (orig_id, cluster_id) com.csv keyed
    by renum_to_orig × final_membership. Returns False if either field
    is missing/malformed."""
    try:
        data = json.loads(trace_json.read_text())
    except (OSError, json.JSONDecodeError):
        return False
    renum = data.get("renum_to_orig")
    final = data.get("final_membership")
    if not isinstance(renum, list) or not isinstance(final, list):
        return False
    if len(renum) != len(final):
        return False
    with com_path.open("w") as f:
        f.write("node_id,cluster_id\n")
        for orig, lab in zip(renum, final):
            f.write(f"{orig},{lab}\n")
    return True


def _leiden_partition(edge_path: Path, com_path: Path, seed: int,
                      err_path: Path, timeout: int,
                      quality: str = "mod",
                      param: str = LEIDEN_MOD_PARAM
                      ) -> tuple[bool, str]:
    """Run Leiden tracer (iters=1) at the requested quality/param. On
    success, com_path contains node_id,cluster_id. Defaults match
    Leiden-Mod (quality=mod, param=1.0)."""
    try:
        rc = subprocess.run(
            [str(LEIDEN_TRACER), str(edge_path), str(com_path),
             quality, str(param), str(seed), "1"],
            capture_output=True, timeout=timeout,
        )
    except subprocess.TimeoutExpired:
        return False, "leiden tracer timeout"
    if rc.returncode != 0:
        tail = rc.stderr[-400:].decode("utf-8", "replace")
        return False, f"leiden rc={rc.returncode}: {tail}"
    err_path.write_bytes(rc.stderr)
    if not com_path.is_file():
        return False, "leiden produced no com.csv"
    return True, ""


def _sbm_partition(edge_path: Path, com_path: Path, seed: int, sweeps: int,
                   K: int, scratch_dir: Path, tag: str,
                   timeout: int) -> tuple[bool, str]:
    """Run SBM-flat-PP: synth K-block init → MH sweeps → extract
    final_membership and write to com_path with orig-id keys."""
    init_path = scratch_dir / f"{tag}.sbminit"
    trace_path = scratch_dir / f"{tag}.sbmtrace"
    try:
        _synth_sbm_init(edge_path, init_path, seed, K)
    except OSError as e:
        return False, f"sbm init synth failed: {e}"
    try:
        rc = subprocess.run(
            [str(SBM_TRACER), "--mode=pp", f"--edge={edge_path}",
             f"--init={init_path}", f"--seed={seed}",
             f"--sweeps={sweeps}"],
            capture_output=True, timeout=timeout,
        )
    except subprocess.TimeoutExpired:
        return False, "sbm tracer timeout"
    if rc.returncode != 0:
        tail = rc.stderr[-400:].decode("utf-8", "replace")
        return False, f"sbm rc={rc.returncode}: {tail}"
    trace_path.write_bytes(rc.stdout)
    if not _sbm_final_to_com(trace_path, com_path):
        return False, "sbm trace missing renum_to_orig/final_membership"
    return True, ""


def run_cell(name: str, edge: str, tier: str, seed: int, criterion: str,
             partitioner: str, sbm_K: int, sbm_sweeps_per_tier: dict[str, int],
             leiden_cpm_resolution: float, timeout: int, scratch: str
             ) -> tuple[str, str, str, int, str, bool, int, int, int, str]:
    """One (partitioner, fixture, seed, criterion) cell.

    Returns (partitioner, name, tier, seed, criterion, ok, pops, survivors,
             records, log_tail).
    """
    edge_path = Path(edge)
    scratch_dir = Path(scratch)
    scratch_dir.mkdir(parents=True, exist_ok=True)
    crit_tag = criterion.replace("(", "_").replace(")", "_").replace("/", "_")
    tag = f"{partitioner}_{name}_s{seed}_{crit_tag}"
    com = scratch_dir / f"{tag}.com"
    wcc_out = scratch_dir / f"{tag}.wccout"
    trace = scratch_dir / f"{tag}.json"
    err = scratch_dir / f"{tag}.partitioner.err"
    wcc_err = scratch_dir / f"{tag}.wcc.err"

    # 1. Partitioner: Leiden-Mod, Leiden-CPM, or SBM-Flat-PP.
    if partitioner == "leiden-mod":
        ok_p, msg = _leiden_partition(edge_path, com, seed, err, timeout,
                                      quality="mod", param=LEIDEN_MOD_PARAM)
    elif partitioner == "leiden-cpm":
        ok_p, msg = _leiden_partition(edge_path, com, seed, err, timeout,
                                      quality="cpm",
                                      param=str(leiden_cpm_resolution))
    elif partitioner == "sbm-flat-pp":
        sweeps = sbm_sweeps_per_tier.get(tier, 5)
        ok_p, msg = _sbm_partition(edge_path, com, seed, sweeps, sbm_K,
                                   scratch_dir, tag, timeout)
    else:
        return partitioner, name, tier, seed, criterion, False, 0, 0, 0, \
               f"unknown partitioner: {partitioner}"
    if not ok_p:
        return partitioner, name, tier, seed, criterion, False, 0, 0, 0, msg

    # 2. WCC tracer (swapped): writes trace JSON on stdout.
    try:
        rc = subprocess.run(
            [str(WCC_TRACER), str(edge_path), str(com), str(wcc_out),
             criterion, "cactus", str(seed)],
            capture_output=True, timeout=timeout,
        )
    except subprocess.TimeoutExpired:
        return partitioner, name, tier, seed, criterion, False, 0, 0, 0, "wcc tracer timeout"
    if rc.returncode != 0:
        tail = rc.stderr[-400:].decode("utf-8", "replace")
        return partitioner, name, tier, seed, criterion, False, 0, 0, 0, \
               f"wcc rc={rc.returncode}: {tail}"
    trace.write_bytes(rc.stdout)
    wcc_err.write_bytes(rc.stderr)

    # 3. JS self-RNG check.
    try:
        rc = subprocess.run(
            ["node", str(CHECK), str(trace), str(edge_path),
             str(com), criterion, str(seed)],
            capture_output=True, text=True, timeout=timeout,
        )
    except subprocess.TimeoutExpired:
        return partitioner, name, tier, seed, criterion, False, 0, 0, 0, "js check timeout"
    log = (rc.stdout + rc.stderr).strip()

    pops = 0
    survivors = 0
    for line in log.splitlines():
        if line.startswith("canonical:"):
            for p in line.split():
                if p.startswith("pops="):
                    try:
                        pops = int(p.split("=", 1)[1])
                    except ValueError:
                        pass
                elif p.startswith("survivors="):
                    try:
                        survivors = int(p.split("=", 1)[1])
                    except ValueError:
                        pass
    records = pops * 50
    ok = rc.returncode == 0

    if ok:
        # Cleanup on PASS; keep on FAIL for diag.
        candidates = [com, wcc_out, trace, err, wcc_err,
                      scratch_dir / f"{tag}.sbminit",
                      scratch_dir / f"{tag}.sbmtrace"]
        for f in candidates:
            try: f.unlink()
            except OSError: pass
    last = log.splitlines()[-1] if log.splitlines() else "(no output)"
    return partitioner, name, tier, seed, criterion, ok, pops, survivors, records, last


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--tiers", default="T1,T2,T3")
    ap.add_argument("--seeds", default=",".join(str(s) for s in SEEDS_50))
    ap.add_argument("--criteria", default=",".join(DEFAULT_CRITERIA))
    ap.add_argument("--partitioners", default="leiden-mod,sbm-flat-pp",
                    help="comma-separated subset of "
                         "{leiden-mod,leiden-cpm,sbm-flat-pp}")
    ap.add_argument("--leiden-cpm-resolution", type=float,
                    default=DEFAULT_LEIDEN_CPM_RESOLUTION,
                    help="gamma for the leiden-cpm partitioner (ignored "
                         "otherwise). Default 0.5 matches the symmetric panel.")
    ap.add_argument("--sbm-K", type=int, default=SBM_K,
                    help="initial block count for sbm-flat-pp synth init")
    ap.add_argument("--sbm-sweeps", default="10,5,2",
                    help="per-tier sweeps for sbm-flat-pp (T1,T2,T3)")
    ap.add_argument("--workers", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--quick", action="store_true",
                    help="3 seeds × 1 fixture per tier × 1 criterion (smoke).")
    ap.add_argument("--timeout", type=int, default=1200,
                    help="per-cell timeout in seconds (T3 cells need >=1200s)")
    ap.add_argument("--scratch", default="/tmp/wcc_stress_3tier_chain")
    ap.add_argument("--max-per-tier", type=int, default=0,
                    help="cap fixture count per tier (0 = unlimited)")
    args = ap.parse_args()

    tiers = args.tiers.split(",")
    partitioners = [p.strip() for p in args.partitioners.split(",") if p.strip()]
    for p in partitioners:
        if p not in DEFAULT_PARTITIONERS:
            sys.exit(f"unknown partitioner: {p} "
                     f"(expected subset of {DEFAULT_PARTITIONERS})")
    panel = discover_panel(REPO)
    if args.quick:
        seeds = SEEDS_50[:3]
        per_tier_cap = 1
        criteria = DEFAULT_CRITERIA[:1]
    else:
        seeds = [int(s) for s in args.seeds.split(",")]
        per_tier_cap = args.max_per_tier or None
        criteria = [c.strip() for c in args.criteria.split(",") if c.strip()]

    sbm_sweeps_list = [int(s) for s in args.sbm_sweeps.split(",")]
    sbm_sweeps_per_tier: dict[str, int] = {}
    for i, t in enumerate(TIER_ORDER):
        sbm_sweeps_per_tier[t] = sbm_sweeps_list[min(i, len(sbm_sweeps_list) - 1)]

    needed_bins: list[Path] = [WCC_TRACER]
    if "leiden-mod" in partitioners or "leiden-cpm" in partitioners:
        needed_bins.append(LEIDEN_TRACER)
    if "sbm-flat-pp" in partitioners:
        needed_bins.append(SBM_TRACER)
    for binp in needed_bins:
        if not binp.is_file():
            sys.exit(f"tracer binary missing: {binp}")

    # Build cell list, grouped per tier.
    # Cell tuple: (name, edge, tier, seed, criterion, partitioner)
    tier_cells: dict[str, list[tuple[str, str, str, int, str, str]]] = {}
    for tier in tiers:
        if tier not in panel:
            sys.exit(f"unknown tier: {tier}")
        fixtures = panel[tier]
        if per_tier_cap is not None:
            fixtures = fixtures[:per_tier_cap]
        cells: list[tuple[str, str, str, int, str, str]] = []
        for fx in fixtures:
            for partitioner in partitioners:
                for seed in seeds:
                    for crit in criteria:
                        cells.append((fx["subnet"], fx["edge"], tier, seed,
                                      crit, partitioner))
        tier_cells[tier] = cells

    total_cells = sum(len(c) for c in tier_cells.values())
    print(f"WCC L4 bumped 3-tier stress: tiers={','.join(tiers)} "
          f"partitioners={','.join(partitioners)} "
          f"seeds={len(seeds)} criteria={len(criteria)} "
          f"workers={args.workers} timeout={args.timeout}s "
          f"total_cells={total_cells}")
    if "leiden-cpm" in partitioners:
        print(f"  leiden-cpm: gamma={args.leiden_cpm_resolution}")
    if "sbm-flat-pp" in partitioners:
        print(f"  sbm-flat-pp: K={args.sbm_K} sweeps_per_tier={sbm_sweeps_per_tier}")
    for tier in tiers:
        print(f"  {tier}: {len(panel[tier])} fixtures")
    print()
    sys.stdout.flush()

    grand_pass = 0
    grand_fail = 0
    grand_pops = 0
    grand_records = 0
    # Per-partitioner aggregate.
    p_agg: dict[str, dict[str, int]] = {
        p: {"pass": 0, "fail": 0, "pops": 0, "records": 0} for p in partitioners
    }
    failed_cells: list[tuple[str, str, str, int, str, str]] = []
    t0 = time.time()

    for tier in tiers:
        cells = tier_cells[tier]
        if not cells:
            continue
        print(f"=== {tier} ({len(cells)} cells) ===")
        sys.stdout.flush()
        tier_pass = tier_fail = tier_pops = tier_records = 0
        # Per-(partitioner, criterion) breakdown.
        per_pc: dict[tuple[str, str], dict[str, int]] = {
            (p, c): {"pass": 0, "fail": 0} for p in partitioners for c in criteria
        }
        results: list[tuple] = []
        with ProcessPoolExecutor(max_workers=args.workers) as ex:
            futures = [
                ex.submit(run_cell, *c, args.sbm_K, sbm_sweeps_per_tier,
                          args.leiden_cpm_resolution,
                          args.timeout, args.scratch)
                for c in cells
            ]
            for fut in as_completed(futures):
                results.append(fut.result())
        # Sort: partitioner, name, criterion, seed.
        results.sort(key=lambda r: (r[0], r[1], r[4], r[3]))
        for partitioner, name, _tier, seed, crit, ok, pops, survivors, records, tail in results:
            tier_pops += pops
            tier_records += records
            p_agg[partitioner]["pops"] += pops
            p_agg[partitioner]["records"] += records
            if ok:
                tier_pass += 1
                per_pc[(partitioner, crit)]["pass"] += 1
                p_agg[partitioner]["pass"] += 1
            else:
                tier_fail += 1
                per_pc[(partitioner, crit)]["fail"] += 1
                p_agg[partitioner]["fail"] += 1
                failed_cells.append((tier, partitioner, name, seed, crit, tail))
                print(f"  FAIL {partitioner} {name} crit={crit} seed={seed}: {tail}")
                sys.stdout.flush()
        grand_pass += tier_pass
        grand_fail += tier_fail
        grand_pops += tier_pops
        grand_records += tier_records
        print(f"  {tier}: PASS={tier_pass} FAIL={tier_fail} pops={tier_pops:,} records={tier_records:,}")
        for partitioner in partitioners:
            for c in criteria:
                row = per_pc[(partitioner, c)]
                print(f"    {partitioner} crit={c}: PASS={row['pass']} FAIL={row['fail']}")
        sys.stdout.flush()

    elapsed = time.time() - t0
    print()
    print("=== SUMMARY ===")
    print(f"Cells: PASS={grand_pass} FAIL={grand_fail} (total={grand_pass + grand_fail})")
    for partitioner in partitioners:
        row = p_agg[partitioner]
        print(f"  {partitioner}: PASS={row['pass']} FAIL={row['fail']} "
              f"pops={row['pops']:,} records={row['records']:,}")
    print(f"Cumulative pops:    {grand_pops:,}")
    print(f"Cumulative records: {grand_records:,}")
    print(f"Wall: {elapsed:.1f}s")
    if failed_cells:
        print()
        print("Failed cells:")
        for tier, partitioner, name, seed, crit, tail in failed_cells[:50]:
            print(f"  {tier}/{partitioner}/{name} crit={crit} seed={seed}: {tail}")
    if grand_fail == 0 and grand_pass > 0:
        print("OVERALL: PASS")
    elif grand_pass == 0:
        print("OVERALL: NO CELLS RUN")
    else:
        print("OVERALL: FAIL")
    return 0 if grand_fail == 0 and grand_pass > 0 else 1


if __name__ == "__main__":
    sys.exit(main())
