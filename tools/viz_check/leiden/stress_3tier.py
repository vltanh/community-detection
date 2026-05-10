"""Leiden L4 3-tier empirical-network stress driver.

Runs the Leiden L4 self-RNG byte-equal check on the standard 17-fixture
undirected non-bipartite panel from reference_cd_stress_tiers.md, sweeping
the standard 9 seeds × {mod 1.0, cpm 0.05} per fixture. (The third
quality variant `rb` documented in the playbook has no JS port — see
codebase_map.md "Variant scope for this audit": only CPM + Modularity
are in-scope for the JS visualizer; Significance / Surprise / RBC / RBER
have no JS port.)

Per (fixture, seed, quality): cpp tracer (/tmp/leiden_kernel_check) emits
per-pass + per-visit trace JSON; self_rng_check.mjs replays the JS
standalone and bit-compares (per-visit v/from/to/moved/dQ + per-pass
nb_moves/total_improv) under matching MT19937 seed. PASS = 0 mismatches
per cell across both per-visit and per-pass scalar fields.

Usage:
    python stress_3tier.py [--tiers T1,T2,T3] [--seeds s1,...]
                            [--qualities mod:1.0,cpm:0.05] [--workers N]
                            [--quick] [--timeout 600]
"""
from __future__ import annotations

import argparse
import os
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
TRACER = Path("/tmp/leiden_kernel_check")
CHECK = HERE / "self_rng_check.mjs"

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
DEFAULT_QUALITIES = [("mod", 1.0), ("cpm", 0.05)]


def parse_qualities(spec: str) -> list[tuple[str, float]]:
    out: list[tuple[str, float]] = []
    for tok in spec.split(","):
        tok = tok.strip()
        if not tok:
            continue
        if ":" not in tok:
            raise ValueError(f"bad quality spec '{tok}', expected name:param")
        name, param = tok.split(":", 1)
        out.append((name.strip(), float(param.strip())))
    return out


def run_cell(name: str, edge: str, seed: int, quality: str, param: float,
             timeout: int, scratch: str) -> tuple[str, int, str, float, bool, int, int, str]:
    """One (fixture, seed, quality) cell.

    Returns (name, seed, quality, param, ok, visits, moves, log_tail)."""
    edge_path = Path(edge)
    scratch_dir = Path(scratch)
    scratch_dir.mkdir(parents=True, exist_ok=True)
    tag = f"{name}_{quality}_{param}_{seed}"
    trace = scratch_dir / f"{tag}.json"
    com = scratch_dir / f"{tag}.com"
    err = scratch_dir / f"{tag}.err"

    # Run cpp tracer with iters=1.
    try:
        rc = subprocess.run(
            [str(TRACER), str(edge_path), str(com), quality, str(param),
             str(seed), "1"],
            capture_output=True, timeout=timeout,
        )
    except subprocess.TimeoutExpired:
        return name, seed, quality, param, False, 0, 0, "tracer timeout"
    if rc.returncode != 0:
        tail = rc.stderr[-400:].decode("utf-8", "replace")
        return name, seed, quality, param, False, 0, 0, f"tracer rc={rc.returncode}: {tail}"
    trace.write_bytes(rc.stdout)
    err.write_bytes(rc.stderr)

    # Run JS self-RNG check.
    try:
        rc = subprocess.run(
            ["node", str(CHECK), str(trace), str(edge_path),
             quality, str(param), str(seed)],
            capture_output=True, text=True, timeout=timeout,
        )
    except subprocess.TimeoutExpired:
        return name, seed, quality, param, False, 0, 0, "js check timeout"
    log = (rc.stdout + rc.stderr).strip()

    visits = 0
    moves = 0
    for line in log.splitlines():
        if line.startswith("canonical:"):
            # canonical: top passes=N total visits=V
            parts = line.split()
            for p in parts:
                if p.startswith("visits="):
                    try:
                        visits = int(p.split("=", 1)[1])
                    except ValueError:
                        pass
        elif line.startswith("per-visit:"):
            # per-visit: ... dQ_bit_mm=X/M
            for p in line.split():
                if p.startswith("dQ_bit_mm="):
                    val = p.split("=", 1)[1]
                    try:
                        moves = int(val.split("/")[1])
                    except (IndexError, ValueError):
                        pass
    ok = rc.returncode == 0
    # Cleanup on PASS to save disk; keep on FAIL for diag.
    if ok:
        for f in (trace, com, err):
            try: f.unlink()
            except OSError: pass
    last = log.splitlines()[-1] if log.splitlines() else "(no output)"
    return name, seed, quality, param, ok, visits, moves, last


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--tiers", default="T1,T2,T3")
    ap.add_argument("--seeds", default=",".join(str(s) for s in DEFAULT_SEEDS))
    ap.add_argument("--qualities", default="mod:1.0,cpm:0.05")
    ap.add_argument("--workers", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--quick", action="store_true",
                    help="3 seeds × 1 fixture per tier × 1 quality (smoke).")
    ap.add_argument("--timeout", type=int, default=600,
                    help="per-cell timeout in seconds (T3 cells need >=300s)")
    ap.add_argument("--scratch", default="/tmp/leiden_stress_3tier")
    args = ap.parse_args()

    tiers = args.tiers.split(",")
    if args.quick:
        seeds = DEFAULT_SEEDS[:3]
        fixtures_per_tier = 1
        qualities = DEFAULT_QUALITIES[:1]
    else:
        seeds = [int(s) for s in args.seeds.split(",")]
        fixtures_per_tier = None
        qualities = parse_qualities(args.qualities)

    # Build cpp tracer (no-op if up to date).
    D.build_tracer(HERE, TRACER)
    if not TRACER.is_file():
        sys.exit(f"tracer binary missing: {TRACER}")

    # Build cell list, grouped per tier.
    tier_cells: dict[str, list[tuple[str, str, int, str, float]]] = {}
    skipped: list[str] = []
    for tier in tiers:
        if tier not in TIERS:
            sys.exit(f"unknown tier: {tier}")
        fixtures = TIERS[tier]
        if fixtures_per_tier is not None:
            fixtures = fixtures[:fixtures_per_tier]
        cells: list[tuple[str, str, int, str, float]] = []
        for subdir, base, _n in fixtures:
            edge = NETS / subdir / f"{base}.csv"
            if not edge.is_file():
                skipped.append(f"{tier}/{base} (missing {edge})")
                continue
            for seed in seeds:
                for q, p in qualities:
                    cells.append((base, str(edge), seed, q, p))
        tier_cells[tier] = cells

    total_cells = sum(len(c) for c in tier_cells.values())
    print(f"L4 3-tier stress: tiers={','.join(tiers)} seeds={len(seeds)} "
          f"qualities={len(qualities)} workers={args.workers} timeout={args.timeout}s "
          f"total_cells={total_cells}")
    for s in skipped:
        print(f"  SKIP {s}")
    print()
    sys.stdout.flush()

    grand_pass = 0
    grand_fail = 0
    grand_visits = 0
    grand_moves = 0
    failed_cells: list[tuple[str, str, int, str, float, str]] = []
    t0 = time.time()

    for tier in tiers:
        cells = tier_cells[tier]
        if not cells:
            continue
        print(f"=== {tier} ({len(cells)} cells) ===")
        sys.stdout.flush()
        tier_pass = tier_fail = tier_visits = tier_moves = 0
        results: list[tuple] = []
        with ProcessPoolExecutor(max_workers=args.workers) as ex:
            futures = [
                ex.submit(run_cell, *c, args.timeout, args.scratch)
                for c in cells
            ]
            for fut in as_completed(futures):
                results.append(fut.result())
        results.sort(key=lambda r: (r[0], r[2], r[1]))
        for name, seed, q, p, ok, visits, moves, tail in results:
            tier_visits += visits
            tier_moves += moves
            if ok:
                tier_pass += 1
            else:
                tier_fail += 1
                failed_cells.append((tier, name, seed, q, p, tail))
                print(f"  FAIL {name} {q}@{p} seed={seed}: {tail}")
                sys.stdout.flush()
        grand_pass += tier_pass
        grand_fail += tier_fail
        grand_visits += tier_visits
        grand_moves += tier_moves
        print(f"  {tier}: PASS={tier_pass} FAIL={tier_fail} visits={tier_visits:,} moves={tier_moves:,}")
        sys.stdout.flush()

    elapsed = time.time() - t0
    print()
    print("=== SUMMARY ===")
    print(f"Cells: PASS={grand_pass} FAIL={grand_fail} (total={grand_pass + grand_fail})")
    print(f"Cumulative visits: {grand_visits:,}")
    print(f"Cumulative moves:  {grand_moves:,}")
    print(f"Wall: {elapsed:.1f}s")
    if failed_cells:
        print()
        print("Failed cells:")
        for tier, name, seed, q, p, tail in failed_cells[:50]:
            print(f"  {tier}/{name} {q}@{p} seed={seed}: {tail}")
    if grand_fail == 0 and grand_pass > 0:
        print("OVERALL: PASS")
    elif grand_pass == 0:
        print("OVERALL: NO CELLS RUN")
    else:
        print("OVERALL: FAIL")
    return 0 if grand_fail == 0 and grand_pass > 0 else 1


if __name__ == "__main__":
    sys.exit(main())
