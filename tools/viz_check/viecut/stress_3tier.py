"""VieCut bumped 3-tier byte-equal stress driver — 5 variants.

Runs the VieCut self-RNG byte-equal check on the bumped 3-tier panel
(every network in `data/empirical_networks/networks/` with `n ≤ 30000`
per `_common/empirical_panel.py`; 161 fixtures, 50 seeds = 8050 cells
per variant).

Variants (--variant):
  - legacy-whole-graph: run VieCut on the whole graph (no upstream
    partition); diff_harness.mjs compares JS observer-RNG against
    [TRACE-RNG] from /tmp/viecut_traced_swapped.
  - chained-from-leiden-mod: induce per-cluster subgraph from Leiden-Mod
    (param=1.0) output, run VieCut mincut on each subgraph.
  - chained-from-leiden-cpm: same, partition = Leiden-CPM(0.5).
  - chained-from-infomap: same, partition = Infomap two-level.
  - chained-from-sbm-flat-pp: same, partition = SBM-Flat-PP MCMC final.

Mirrors the WCC/CM/CC chain wiring pattern. For chained variants:
  1. produce partition via upstream tracer
  2. for each cluster with >= 2 nodes: induce subgraph → METIS, run
     viecut_traced_swapped + diff_harness on the cluster.
  3. PASS = every cluster diff_harness 0-divergent.

Usage:
    python stress_3tier.py [--tiers T1,T2,T3] [--seeds s1,...]
                          [--variant legacy-whole-graph|chained-from-*]
                          [--workers N] [--timeout 1200] [--quick]
"""
from __future__ import annotations

import argparse
import json
import os
import random
import re
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
VIECUT_BIN = REPO / "VieCut" / "build" / "mincut"
TRACED_SWAP = Path("/tmp/viecut_traced_swapped")
DIFF_HARNESS = HERE / "diff_harness.mjs"
LEIDEN_TRACER = Path("/tmp/leiden_kernel_check")
SBM_TRACER = Path("/tmp/sbm_flat_kernel_check")
INFOMAP_TRACER = Path("/tmp/infomap_kernel_check_swapped")

# SBM Flat-PP per-tier sweep schedule (same as CC's chain driver).
TIER_SWEEPS = {"T1": 5, "T2": 3, "T3": 2}


# ---- partition production (mirror WCC/CM/CC chain templates) ---------------

def read_edges(edge_csv: Path) -> list[tuple[int, int]]:
    edges: list[tuple[int, int]] = []
    with edge_csv.open() as f:
        next(f)
        for line in f:
            parts = line.strip().split(",")
            if len(parts) < 2:
                continue
            try:
                u, v = int(parts[0]), int(parts[1])
            except ValueError:
                continue
            if u == v:
                continue
            edges.append((u, v))
    return edges


def read_partition(com_csv: Path) -> dict[int, list[int]]:
    """Return {cid: [node_id, ...]} sorted ascending."""
    clusters: dict[int, list[int]] = {}
    with com_csv.open() as f:
        next(f)
        for line in f:
            parts = line.strip().split(",")
            if len(parts) < 2:
                continue
            try:
                n, c = int(parts[0]), int(parts[1])
            except ValueError:
                continue
            clusters.setdefault(c, []).append(n)
    for c in clusters:
        clusters[c].sort()
    return clusters


def collect_nodes_str(edge_path: Path) -> list[str]:
    nodes: set[str] = set()
    with edge_path.open() as f:
        next(f)
        for line in f:
            line = line.strip()
            if not line: continue
            parts = line.replace("\t", ",").split(",")
            if len(parts) < 2: continue
            nodes.add(parts[0].strip())
            nodes.add(parts[1].strip())
    return sorted(nodes)


def synth_sbm_init(edge: Path, seed: int, scratch: Path, tag: str) -> Path:
    nodes = collect_nodes_str(edge)
    n = len(nodes)
    K = max(2, int(n ** 0.5))
    out = scratch / f"{tag}_init.com"
    rng = random.Random(seed)
    with out.open("w") as f:
        f.write("node_id,cluster_id\n")
        for nid in nodes:
            f.write(f"{nid},{rng.randint(0, K - 1)}\n")
    return out


def produce_partition(variant: str, edge: Path, seed: int, sweeps: int,
                      timeout: int, scratch: Path, tag: str
                      ) -> tuple[bool, Path | None, str]:
    com_csv = scratch / f"{tag}_com.csv"
    if variant == "chained-from-leiden-mod":
        try:
            rc = subprocess.run(
                [str(LEIDEN_TRACER), str(edge), str(com_csv), "mod", "1.0",
                 str(seed), "1"],
                capture_output=True, timeout=timeout,
            )
        except subprocess.TimeoutExpired:
            return False, None, "leiden tracer timeout"
        if rc.returncode != 0:
            return False, None, f"leiden rc={rc.returncode}: {rc.stderr[-200:]!r}"
        return True, com_csv, ""
    if variant == "chained-from-leiden-cpm":
        try:
            rc = subprocess.run(
                [str(LEIDEN_TRACER), str(edge), str(com_csv), "cpm", "0.5",
                 str(seed), "1"],
                capture_output=True, timeout=timeout,
            )
        except subprocess.TimeoutExpired:
            return False, None, "leiden tracer timeout"
        if rc.returncode != 0:
            return False, None, f"leiden rc={rc.returncode}: {rc.stderr[-200:]!r}"
        return True, com_csv, ""
    if variant == "chained-from-infomap":
        try:
            rc = subprocess.run(
                [str(INFOMAP_TRACER), str(edge), str(seed), str(com_csv)],
                capture_output=True, timeout=timeout,
            )
        except subprocess.TimeoutExpired:
            return False, None, "infomap tracer timeout"
        if rc.returncode != 0:
            return False, None, f"infomap rc={rc.returncode}: {rc.stderr[-200:]!r}"
        return True, com_csv, ""
    if variant == "chained-from-sbm-flat-pp":
        init = synth_sbm_init(edge, seed, scratch, tag)
        try:
            rc = subprocess.run(
                [str(SBM_TRACER), "--mode=pp", f"--edge={edge}",
                 f"--init={init}", f"--seed={seed}", f"--sweeps={sweeps}"],
                capture_output=True, timeout=timeout,
            )
        except subprocess.TimeoutExpired:
            return False, None, "sbm tracer timeout"
        if rc.returncode != 0:
            return False, None, f"sbm rc={rc.returncode}: {rc.stderr[-200:]!r}"
        try:
            tr = json.loads(rc.stdout)
            renum = tr["renum_to_orig"]
            mem = tr["final_membership"]
        except Exception as e:
            return False, None, f"sbm trace parse fail: {e}"
        with com_csv.open("w") as f:
            f.write("node_id,cluster_id\n")
            for orig, cid in zip(renum, mem):
                f.write(f"{orig},{cid}\n")
        try: init.unlink()
        except OSError: pass
        return True, com_csv, ""
    return False, None, f"unknown variant: {variant}"


# ---- whole-graph VieCut + per-cluster VieCut --------------------------------

def cluster_to_metis(global_edges: list[tuple[int, int]],
                     cluster_nodes: list[int], metis_path: Path) -> int:
    """Induce subgraph + emit METIS. Return n_local."""
    pos = {nid: i for i, nid in enumerate(cluster_nodes)}
    seen: set[tuple[int, int]] = set()
    n = len(cluster_nodes)
    adj: list[list[int]] = [[] for _ in range(n)]
    for u, v in global_edges:
        if u not in pos or v not in pos:
            continue
        a, b = (pos[u], pos[v]) if pos[u] < pos[v] else (pos[v], pos[u])
        if (a, b) in seen:
            continue
        seen.add((a, b))
        adj[a].append(b + 1)
        adj[b].append(a + 1)
    m = sum(len(a) for a in adj) // 2
    with metis_path.open("w") as f:
        f.write(f"{n} {m}\n")
        for i in range(n):
            f.write(" ".join(str(x) for x in sorted(adj[i])) + "\n")
    return n


def whole_graph_metis(edges: list[tuple[int, int]], metis_path: Path) -> int:
    nodes = sorted({u for u, _ in edges} | {v for _, v in edges})
    return cluster_to_metis(edges, nodes, metis_path)


def viecut_one_subgraph(metis: Path, seed: int, scratch: Path, tag: str,
                        timeout: int) -> tuple[bool, int, str]:
    """Run viecut_traced_swapped on a single METIS subgraph + diff_harness.
    Returns (ok, records, msg). records = RNG step count diff_harness
    walked. mincut<=0 (disconnected) => trivially ok with records=1
    (mincut value compare)."""
    json_out = scratch / f"{tag}.json"
    diff_log = scratch / f"{tag}.diff.log"
    # Tracer JSON.
    try:
        rc = subprocess.run(
            [str(TRACED_SWAP), str(metis), str(seed)],
            capture_output=True, text=True, timeout=timeout,
        )
    except subprocess.TimeoutExpired:
        return False, 0, "viecut tracer timeout"
    if rc.returncode != 0:
        return False, 0, f"viecut tracer rc={rc.returncode}: {rc.stderr[-200:]!r}"
    json_out.write_text(rc.stdout)
    try:
        tj = json.loads(rc.stdout)
        mincut = tj.get("mincut", -1)
    except Exception:
        mincut = -1
    if mincut <= 0:
        try: json_out.unlink()
        except OSError: pass
        return True, 1, "mincut<=0 (disconnected)"
    # diff_harness for the RNG lockstep.
    try:
        rc2 = subprocess.run(
            ["node", str(DIFF_HARNESS), str(metis), str(seed)],
            capture_output=True, text=True, timeout=timeout,
        )
    except subprocess.TimeoutExpired:
        return False, 0, "diff_harness timeout"
    log = rc2.stdout + rc2.stderr
    diff_log.write_text(log)
    if rc2.returncode != 0:
        last = log.strip().splitlines()[-1] if log.strip() else "(no output)"
        return False, 0, f"diff_harness rc={rc2.returncode}: {last}"
    records = 0
    m = re.search(r"total_records=(\d+)", log)
    if m:
        records = int(m.group(1))
    try: json_out.unlink()
    except OSError: pass
    try: diff_log.unlink()
    except OSError: pass
    return True, records + 1, ""


def run_cell(name: str, edge_str: str, seed: int, variant: str,
             sweeps: int, timeout: int, scratch_root: str
             ) -> tuple[str, int, bool, int, int, int, str]:
    """One (fixture, seed, variant) cell.
    Returns (name, seed, ok, n_clusters, n_active, records, log_tail)."""
    edge = Path(edge_str)
    scratch = Path(scratch_root)
    scratch.mkdir(parents=True, exist_ok=True)
    tag = f"{name}_{variant}_s{seed}"

    if variant == "legacy-whole-graph":
        global_edges = read_edges(edge)
        if not global_edges:
            return name, seed, False, 0, 0, 0, "no edges"
        metis = scratch / f"{tag}_whole.metis"
        whole_graph_metis(global_edges, metis)
        ok, recs, msg = viecut_one_subgraph(metis, seed, scratch, tag, timeout)
        try: metis.unlink()
        except OSError: pass
        return name, seed, ok, 1, 1 if ok else 0, recs, msg or "PASS"

    # chained variants
    ok, com_csv, msg = produce_partition(variant, edge, seed, sweeps, timeout,
                                         scratch, tag)
    if not ok:
        return name, seed, False, 0, 0, 0, msg

    global_edges = read_edges(edge)
    clusters = read_partition(com_csv)
    total_clusters = len(clusters)
    active = 0
    records = 0
    fail_msg = ""
    for cid in sorted(clusters):
        nodes = clusters[cid]
        if len(nodes) < 2:
            continue
        cmetis = scratch / f"{tag}_c{cid}.metis"
        cluster_to_metis(global_edges, nodes, cmetis)
        ok_cl, recs, m = viecut_one_subgraph(cmetis, seed, scratch,
                                             f"{tag}_c{cid}", timeout)
        try: cmetis.unlink()
        except OSError: pass
        if not ok_cl:
            fail_msg = f"cluster {cid} (n={len(nodes)}): {m}"
            return name, seed, False, total_clusters, active, records, fail_msg
        records += recs
        active += 1
    try: com_csv.unlink()
    except OSError: pass
    return name, seed, True, total_clusters, active, records, "PASS"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--tiers", default="T1,T2,T3")
    ap.add_argument("--seeds", default=",".join(str(s) for s in SEEDS_50))
    ap.add_argument("--variant", default="legacy-whole-graph",
                    choices=("legacy-whole-graph",
                             "chained-from-leiden-mod",
                             "chained-from-leiden-cpm",
                             "chained-from-infomap",
                             "chained-from-sbm-flat-pp"))
    ap.add_argument("--workers", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--quick", action="store_true",
                    help="3 seeds × 1 fixture per tier (smoke).")
    ap.add_argument("--timeout", type=int, default=1200,
                    help="per-cell timeout in seconds (T3 cells need >=600s)")
    ap.add_argument("--scratch", default="/tmp/viecut_stress_3tier")
    ap.add_argument("--sweeps-t1", type=int, default=TIER_SWEEPS["T1"])
    ap.add_argument("--sweeps-t2", type=int, default=TIER_SWEEPS["T2"])
    ap.add_argument("--sweeps-t3", type=int, default=TIER_SWEEPS["T3"])
    ap.add_argument("--max-per-tier", type=int, default=0,
                    help="cap fixture count per tier (0 = unlimited)")
    args = ap.parse_args()

    if not TRACED_SWAP.is_file():
        sys.exit(f"viecut tracer missing: {TRACED_SWAP}\n"
                 f"build via: bash {HERE}/instrumented/build.sh")
    if not DIFF_HARNESS.is_file():
        sys.exit(f"diff_harness.mjs missing: {DIFF_HARNESS}")

    # Upstream tracer presence checks for chained variants.
    if args.variant == "chained-from-leiden-mod":
        if not LEIDEN_TRACER.is_file():
            sys.exit(f"leiden tracer missing: {LEIDEN_TRACER}")
    elif args.variant == "chained-from-leiden-cpm":
        if not LEIDEN_TRACER.is_file():
            sys.exit(f"leiden tracer missing: {LEIDEN_TRACER}")
    elif args.variant == "chained-from-infomap":
        if not INFOMAP_TRACER.is_file():
            sys.exit(f"infomap tracer missing: {INFOMAP_TRACER}")
    elif args.variant == "chained-from-sbm-flat-pp":
        if not SBM_TRACER.is_file():
            sys.exit(f"sbm tracer missing: {SBM_TRACER}")

    tier_sweeps = {"T1": args.sweeps_t1, "T2": args.sweeps_t2, "T3": args.sweeps_t3}
    tiers = args.tiers.split(",")
    panel = discover_panel(REPO)
    if args.quick:
        seeds = SEEDS_50[:3]
        per_tier_cap = 1
    else:
        seeds = [int(s) for s in args.seeds.split(",")]
        per_tier_cap = args.max_per_tier or None

    tier_cells: dict[str, list[tuple[str, str, int, int]]] = {}
    for tier in tiers:
        if tier not in panel:
            sys.exit(f"unknown tier: {tier}")
        fixtures = panel[tier]
        if per_tier_cap is not None:
            fixtures = fixtures[:per_tier_cap]
        sweeps = tier_sweeps[tier]
        cells: list[tuple[str, str, int, int]] = []
        for fx in fixtures:
            for seed in seeds:
                cells.append((fx["subnet"], fx["edge"], seed, sweeps))
        tier_cells[tier] = cells

    total = sum(len(c) for c in tier_cells.values())
    print(f"VieCut bumped 3-tier stress (variant={args.variant}): "
          f"tiers={','.join(tiers)} seeds={len(seeds)} workers={args.workers} "
          f"timeout={args.timeout}s total_cells={total}")
    for tier in tiers:
        print(f"  {tier}: {len(panel[tier])} fixtures")
    print()
    sys.stdout.flush()

    grand_pass = grand_fail = grand_records = 0
    failed_cells: list[tuple[str, str, int, str]] = []
    t0 = time.time()

    for tier in tiers:
        cells = tier_cells[tier]
        if not cells:
            continue
        print(f"=== {tier} ({len(cells)} cells) ===")
        sys.stdout.flush()
        tier_pass = tier_fail = tier_records = 0
        results: list[tuple] = []
        with ProcessPoolExecutor(max_workers=args.workers) as ex:
            futures = [
                ex.submit(run_cell, *c, args.variant, args.timeout, args.scratch)
                for c in cells
            ]
            for fut in as_completed(futures):
                results.append(fut.result())
        results.sort(key=lambda r: (r[0], r[1]))
        for name, seed, ok, ncl, nact, recs, tail in results:
            tier_records += recs
            if ok:
                tier_pass += 1
            else:
                tier_fail += 1
                failed_cells.append((tier, name, seed, tail))
                print(f"  FAIL {name} seed={seed}: {tail}")
                sys.stdout.flush()
        grand_pass += tier_pass
        grand_fail += tier_fail
        grand_records += tier_records
        print(f"  {tier}: PASS={tier_pass} FAIL={tier_fail} records={tier_records:,}")
        sys.stdout.flush()

    elapsed = time.time() - t0
    print()
    print("=== SUMMARY ===")
    print(f"Cells: PASS={grand_pass} FAIL={grand_fail} (total={grand_pass + grand_fail})")
    print(f"Cumulative records: {grand_records:,}")
    print(f"Wall: {elapsed:.1f}s")
    if failed_cells:
        print()
        print("Failed cells:")
        for tier, name, seed, tail in failed_cells[:50]:
            print(f"  {tier}/{name} seed={seed}: {tail}")
    if grand_fail == 0 and grand_pass > 0:
        print("OVERALL: PASS")
    elif grand_pass == 0:
        print("OVERALL: NO CELLS RUN")
    else:
        print("OVERALL: FAIL")
    return 0 if grand_fail == 0 and grand_pass > 0 else 1


if __name__ == "__main__":
    sys.exit(main())
