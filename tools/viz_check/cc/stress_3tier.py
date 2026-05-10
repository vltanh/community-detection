"""CC L4 3-tier empirical-network stress driver.

Runs the CC byte-equal cross-check on the canonical 3-tier panel from
reference_cd_stress_tiers.md (17 fixtures across T1/T2/T3, n=800..23,133)
under one of several input-partition sources. The partition source does
not bear on CC's byte-equal contract (CC is deterministic BFS over an
input partition; rows A-M in cc_audit.md are unchanged across input
shapes) but a richer input panel exercises more (graph, partition)
combinations of the kernel's only state.

Input-partition variants (--partition):
  - sbm-flat-pp: SBM Flat-PP MCMC final_membership (Zhang+Peixoto 2020),
    seeded by K0=sqrt(N) Random(seed) init then advanced T1=5 / T2=3 /
    T3=2 MCMC sweeps. Matches sbm/diagnostic/stress_3tier.py schedule.
  - leiden-mod: Leiden-Mod (quality=mod, param=1.0, iters=1) final
    membership under MT19937(seed). Matches CM's leiden-mod base method.
  - leiden-cpm: Leiden-CPM final membership; param = --resolution
    (default 0.5, typed exactly as `0.5`). Matches CM's leiden-cpm base
    method when --resolution is set to the matching gamma.

Per (fixture, seed, partition):
  1. Run the partition tracer (SBM-flat-pp or Leiden); emit a
     CC-format com.csv (`node_id,cluster_id`, header + one row per
     original input node).
  2. cpp CC canonical tracer (/tmp/cc_kernel_check_canonical) and
     swapped tracer (/tmp/cc_kernel_check_swapped) both run on
     (edge.csv, com.csv); their outputs must be byte-equal (vacuous
     for CC since it has zero RNG/FP).
  3. JS CC replay (kernel_check.mjs) reads canonical's JSON trace,
     runs comdet/js/cc/cc.js on the same input, byte-compares per-
     component node-id-ASC contents.

PASS = canon-vs-swap byte-equal AND tracer-vs-JS byte-equal.

Seeds avoid 0 (igraph MT19937 quirk maps 0 -> 4357 per rng_mt19937.c).

Usage:
    python stress_3tier.py [--tiers T1,T2,T3] [--seeds s1,...]
                            [--partition sbm-flat-pp|leiden-mod|leiden-cpm]
                            [--resolution 0.5]
                            [--workers N] [--quick] [--timeout 900]
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

REPO = D.repo_root_from_here(HERE)
NETS = REPO.parent / "data" / "empirical_networks" / "networks"
SBM_TRACER = Path("/tmp/sbm_flat_kernel_check")
LEIDEN_TRACER = Path("/tmp/leiden_kernel_check")
CC_CANON = Path("/tmp/cc_kernel_check_canonical")
CC_SWAP = Path("/tmp/cc_kernel_check_swapped")
JS_REPLAY = HERE / "kernel_check.mjs"
SBM_BUILD_DIR = REPO / "tools" / "viz_check" / "sbm"
LEIDEN_BUILD_DIR = REPO / "tools" / "viz_check" / "leiden"

TIERS = {
    "T1": [
        ("copenhagen", "copenhagen_fb_friends", 800),
        ("product_space", "product_space_HS", 866),
        ("dnc", "dnc", 906),
        ("euroroad", "euroroad", 1174),
        ("facebook_organizations", "facebook_organizations_M1", 1429),
        ("netscience", "netscience", 1461),
        ("new_zealand_collab", "new_zealand_collab", 1511),
        ("collins_yeast", "collins_yeast", 1622),
        ("bible_nouns", "bible_nouns", 1773),
        ("interactome_yeast", "interactome_yeast", 1846),
        ("drosophila_flybi", "drosophila_flybi", 2906),
    ],
    "T2": [
        ("arxiv_authors", "arxiv_authors_HepTh", 9875),
        ("sp_infectious", "sp_infectious", 10972),
        ("arxiv_authors", "arxiv_authors_HepPh", 12006),
        ("physics_collab", "physics_collab_arXiv", 14065),
    ],
    "T3": [
        ("internet_as", "internet_as", 22963),
        ("arxiv_authors", "arxiv_authors_CondMat", 23133),
    ],
}

DEFAULT_SEEDS = [1, 7, 13, 42, 99, 137, 1729, 65535, 2147483646]
# Sweep schedule mirrors sbm/diagnostic/stress_3tier.py (T1=5, T2=3, T3=2).
# Sweep depth affects which partition lands in CC but not CC's byte-equal
# contract — CC is deterministic and partition-input agnostic.
TIER_SWEEPS = {"T1": 5, "T2": 3, "T3": 2}
SBM_MODE = "pp"


def collect_nodes(edge_path: Path) -> list[str]:
    """Read edge.csv (header + source,target rows), return ordered list
    of unique node-id strings in CSV encounter order (= canonical's
    GetOriginalToNewIdMap insertion order, which is what the tracer
    assumes for the (orig, new) id map)."""
    nodes: list[str] = []
    seen: set[str] = set()
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


def synth_init(edge_path: Path, seed: int, scratch_dir: Path,
               tag: str) -> tuple[Path, int]:
    """Synthesize an init partition with K = max(2, isqrt(N)) labels.

    Same convention as sbm/audit.md's nested 3-tier driver (B0 = sqrt(N)
    bound). Returns (init_csv, n)."""
    nodes = collect_nodes(edge_path)
    n = len(nodes)
    K = max(2, int(n ** 0.5))
    rng = random.Random(seed)
    out = scratch_dir / f"{tag}_init.com"
    with out.open("w") as f:
        f.write("node_id,cluster_id\n")
        for nid in nodes:
            f.write(f"{nid},{rng.randint(0, K - 1)}\n")
    return out, n


def parse_sbm_trace(trace_path: Path) -> tuple[list[str], list[int]]:
    """Read trace JSON; return (renum_to_orig, final_membership)."""
    with trace_path.open() as f:
        tr = json.load(f)
    renum = tr["renum_to_orig"]
    mem = tr["final_membership"]
    if len(renum) != len(mem):
        raise RuntimeError(
            f"renum_to_orig len={len(renum)} != final_membership len={len(mem)}"
        )
    return renum, mem


def write_cc_com(renum: list[str], mem: list[int], out: Path) -> None:
    """Materialize SBM final partition as a CC-readable com.csv.

    Format matches the cpp ReadCommunities(orig_to_new, com_csv) loader:
    header + `<orig_node_id>,<cluster_id>` per row."""
    with out.open("w") as f:
        f.write("node_id,cluster_id\n")
        for orig, cid in zip(renum, mem):
            f.write(f"{orig},{cid}\n")


def make_partition_sbm_flat_pp(edge_path: Path, seed: int, sweeps: int,
                               timeout: int, scratch_dir: Path, tag: str,
                               cc_com: Path
                               ) -> tuple[bool, int, int, str]:
    """Produce CC com.csv from an SBM Flat-PP MCMC run.

    Returns (ok, n, K_final, msg). On failure msg carries the cause; on
    success n is the input graph's node count and K_final = number of
    distinct cluster ids in the final membership."""
    init_com = scratch_dir / f"{tag}_init.com"
    sbm_trace = scratch_dir / f"{tag}_sbm.json"
    try:
        init_com, n = synth_init(edge_path, seed, scratch_dir, tag)
    except Exception as e:
        return False, 0, 0, f"init synth fail: {e}"
    try:
        rc = subprocess.run(
            [str(SBM_TRACER), f"--mode={SBM_MODE}", f"--edge={edge_path}",
             f"--init={init_com}", f"--seed={seed}", f"--sweeps={sweeps}"],
            capture_output=True, timeout=timeout,
        )
    except subprocess.TimeoutExpired:
        return False, n, 0, "sbm tracer timeout"
    if rc.returncode != 0:
        tail = rc.stderr[-400:].decode("utf-8", "replace")
        return False, n, 0, f"sbm rc={rc.returncode}: {tail}"
    sbm_trace.write_bytes(rc.stdout)
    try:
        renum, mem = parse_sbm_trace(sbm_trace)
    except Exception as e:
        return False, n, 0, f"sbm trace parse fail: {e}"
    write_cc_com(renum, mem, cc_com)
    K_final = len(set(mem))
    # Cleanup intermediates (kept on failure path above).
    for p in (init_com, sbm_trace):
        try: p.unlink()
        except OSError: pass
    return True, n, K_final, ""


def make_partition_leiden(edge_path: Path, seed: int, quality: str,
                          param: float, timeout: int, scratch_dir: Path,
                          tag: str, cc_com: Path
                          ) -> tuple[bool, int, int, str]:
    """Produce CC com.csv by running the Leiden tracer (iters=1) at the
    requested quality. Output format from /tmp/leiden_kernel_check is
    already the CC com.csv shape (header + `<orig_id>,<cluster_id>`)."""
    try:
        rc = subprocess.run(
            [str(LEIDEN_TRACER), str(edge_path), str(cc_com), quality,
             str(param), str(seed), "1"],
            capture_output=True, timeout=timeout,
        )
    except subprocess.TimeoutExpired:
        return False, 0, 0, "leiden tracer timeout"
    if rc.returncode != 0:
        tail = rc.stderr[-400:].decode("utf-8", "replace")
        return False, 0, 0, f"leiden rc={rc.returncode}: {tail}"
    # Derive n + K_final from the produced com.csv.
    n = 0
    cids: set[int] = set()
    try:
        with cc_com.open() as f:
            next(f)  # header
            for line in f:
                parts = line.strip().split(",")
                if len(parts) < 2:
                    continue
                n += 1
                try: cids.add(int(parts[1]))
                except ValueError: pass
    except OSError as e:
        return False, 0, 0, f"leiden com read: {e}"
    return True, n, len(cids), ""


def run_cell(name: str, edge: str, seed: int, sweeps: int, partition: str,
             resolution: float, timeout: int, scratch: str
             ) -> tuple[str, int, bool, int, int, int, str]:
    """One (fixture, seed) cell.

    Returns (name, seed, ok, n, K_final, records, log_tail)."""
    edge_path = Path(edge)
    scratch_dir = Path(scratch)
    scratch_dir.mkdir(parents=True, exist_ok=True)
    tag = f"{name}_{partition}_s{seed}"
    cc_com = scratch_dir / f"{tag}_cc.com"
    out_canon = scratch_dir / f"{tag}_cc_canon.csv"
    out_swap = scratch_dir / f"{tag}_cc_swap.csv"
    json_canon = scratch_dir / f"{tag}_cc_canon.json"
    json_swap = scratch_dir / f"{tag}_cc_swap.json"

    # Step 1-3: produce input partition (dispatch on --partition).
    if partition == "sbm-flat-pp":
        ok, n, K_final, msg = make_partition_sbm_flat_pp(
            edge_path, seed, sweeps, timeout, scratch_dir, tag, cc_com,
        )
    elif partition == "leiden-mod":
        ok, n, K_final, msg = make_partition_leiden(
            edge_path, seed, "mod", 1.0, timeout, scratch_dir, tag, cc_com,
        )
    elif partition == "leiden-cpm":
        ok, n, K_final, msg = make_partition_leiden(
            edge_path, seed, "cpm", resolution, timeout, scratch_dir, tag,
            cc_com,
        )
    else:
        return name, seed, False, 0, 0, 0, f"unknown partition: {partition}"
    if not ok:
        return name, seed, False, n, K_final, 0, msg

    # Step 4: CC canonical tracer.
    try:
        rc = subprocess.run(
            [str(CC_CANON), str(edge_path), str(cc_com), str(out_canon)],
            capture_output=True, timeout=timeout,
        )
    except subprocess.TimeoutExpired:
        return name, seed, False, n, K_final, 0, "cc canon timeout"
    if rc.returncode != 0:
        tail = rc.stderr[-400:].decode("utf-8", "replace")
        return name, seed, False, n, K_final, 0, f"cc canon rc={rc.returncode}: {tail}"
    json_canon.write_bytes(rc.stdout)

    # Step 5: CC swapped tracer (vacuous swap for CC; identical output).
    try:
        rc = subprocess.run(
            [str(CC_SWAP), str(edge_path), str(cc_com), str(out_swap)],
            capture_output=True, timeout=timeout,
        )
    except subprocess.TimeoutExpired:
        return name, seed, False, n, K_final, 0, "cc swap timeout"
    if rc.returncode != 0:
        tail = rc.stderr[-400:].decode("utf-8", "replace")
        return name, seed, False, n, K_final, 0, f"cc swap rc={rc.returncode}: {tail}"
    json_swap.write_bytes(rc.stdout)

    # Step 6: byte-equal canon-vs-swap (vacuous for CC).
    if out_canon.read_bytes() != out_swap.read_bytes():
        return name, seed, False, n, K_final, 0, "cc canon != cc swap (out csv)"
    if json_canon.read_bytes() != json_swap.read_bytes():
        return name, seed, False, n, K_final, 0, "cc canon != cc swap (json)"

    # Step 7: JS replay.
    try:
        rc = subprocess.run(
            ["node", str(JS_REPLAY), str(json_canon),
             str(edge_path), str(cc_com)],
            capture_output=True, text=True, timeout=timeout,
        )
    except subprocess.TimeoutExpired:
        return name, seed, False, n, K_final, 0, "js replay timeout"
    log = (rc.stdout + rc.stderr).strip()
    ok = rc.returncode == 0

    # Count CC output records (post-singleton-filter rows, excluding header).
    try:
        with out_canon.open("r") as f:
            records = sum(1 for _ in f) - 1
    except OSError:
        records = 0

    # Cleanup on PASS; keep on FAIL for diag.
    if ok:
        for f in (cc_com, out_canon, out_swap, json_canon, json_swap):
            try: f.unlink()
            except OSError: pass

    last = log.splitlines()[-1] if log.splitlines() else "(no output)"
    return name, seed, ok, n, K_final, records, last


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--tiers", default="T1,T2,T3")
    ap.add_argument("--seeds", default=",".join(str(s) for s in DEFAULT_SEEDS))
    ap.add_argument("--workers", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--quick", action="store_true",
                    help="3 seeds × 1 fixture per tier (smoke).")
    ap.add_argument("--timeout", type=int, default=900,
                    help="per-cell timeout in seconds (T3 cells need >=600s)")
    ap.add_argument("--sweeps-t1", type=int, default=TIER_SWEEPS["T1"])
    ap.add_argument("--sweeps-t2", type=int, default=TIER_SWEEPS["T2"])
    ap.add_argument("--sweeps-t3", type=int, default=TIER_SWEEPS["T3"])
    ap.add_argument("--partition", default="sbm-flat-pp",
                    choices=("sbm-flat-pp", "leiden-mod", "leiden-cpm"),
                    help="Source of the CC input partition.")
    ap.add_argument("--resolution", type=float, default=0.5,
                    help="Resolution gamma for leiden-cpm (ignored otherwise). "
                         "Default 0.5 matches the canonical symmetric-coverage panel.")
    ap.add_argument("--scratch", default="/tmp/cc_stress_3tier")
    args = ap.parse_args()

    tier_sweeps = {"T1": args.sweeps_t1, "T2": args.sweeps_t2, "T3": args.sweeps_t3}
    tiers = args.tiers.split(",")
    if args.quick:
        seeds = DEFAULT_SEEDS[:3]
        fixtures_per_tier = 1
    else:
        seeds = [int(s) for s in args.seeds.split(",")]
        fixtures_per_tier = None

    # Build CC tracers (no-op if up to date).
    D.build_tracer(HERE, CC_CANON)
    if not CC_CANON.is_file():
        sys.exit(f"cc canonical tracer missing: {CC_CANON}")
    if not CC_SWAP.is_file():
        sys.exit(f"cc swapped tracer missing: {CC_SWAP}")

    # Build the partition-source tracer required by --partition.
    if args.partition == "sbm-flat-pp":
        D.build_tracer(SBM_BUILD_DIR, SBM_TRACER)
        if not SBM_TRACER.is_file():
            sys.exit(f"sbm tracer missing: {SBM_TRACER}")
    elif args.partition in ("leiden-mod", "leiden-cpm"):
        D.build_tracer(LEIDEN_BUILD_DIR, LEIDEN_TRACER)
        if not LEIDEN_TRACER.is_file():
            sys.exit(f"leiden tracer missing: {LEIDEN_TRACER}\n"
                     f"build via: bash {LEIDEN_BUILD_DIR}/instrumented/build.sh")

    # Build cell list, grouped per tier.
    tier_cells: dict[str, list[tuple[str, str, int, int]]] = {}
    skipped: list[str] = []
    for tier in tiers:
        if tier not in TIERS:
            sys.exit(f"unknown tier: {tier}")
        fixtures = TIERS[tier]
        if fixtures_per_tier is not None:
            fixtures = fixtures[:fixtures_per_tier]
        cells: list[tuple[str, str, int, int]] = []
        sweeps = tier_sweeps[tier]
        for subdir, base, _n in fixtures:
            edge = NETS / subdir / f"{base}.csv"
            if not edge.is_file():
                skipped.append(f"{tier}/{base} (missing {edge})")
                continue
            for seed in seeds:
                cells.append((base, str(edge), seed, sweeps))
        tier_cells[tier] = cells

    total_cells = sum(len(c) for c in tier_cells.values())
    if args.partition == "leiden-cpm":
        partition_tag = f"leiden-cpm(gamma={args.resolution})"
    else:
        partition_tag = args.partition
    print(f"CC L4 3-tier stress ({partition_tag} input): tiers={','.join(tiers)} "
          f"seeds={len(seeds)} workers={args.workers} timeout={args.timeout}s "
          f"sweeps={tier_sweeps} total_cells={total_cells}")
    for s in skipped:
        print(f"  SKIP {s}")
    print()
    sys.stdout.flush()

    grand_pass = 0
    grand_fail = 0
    grand_records = 0
    failed_cells: list[tuple[str, str, int, str]] = []
    t0 = time.time()

    for tier in tiers:
        cells = tier_cells[tier]
        if not cells:
            continue
        print(f"=== {tier} ({len(cells)} cells, sweeps={tier_sweeps[tier]}) ===")
        sys.stdout.flush()
        tier_pass = tier_fail = tier_records = 0
        results: list[tuple] = []
        with ProcessPoolExecutor(max_workers=args.workers) as ex:
            futures = [
                ex.submit(run_cell, *c, args.partition, args.resolution,
                          args.timeout, args.scratch)
                for c in cells
            ]
            for fut in as_completed(futures):
                results.append(fut.result())
        results.sort(key=lambda r: (r[0], r[1]))
        for name, seed, ok, n, K_final, records, tail in results:
            tier_records += records
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
