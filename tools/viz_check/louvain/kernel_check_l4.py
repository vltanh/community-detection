"""Louvain L4 self-RNG bit-equal-per-step stress driver.

Stress matrix:
  seeds  = {1, 7, 13, 42, 99, 137, 1729, 65535, 2147483646}
  inputs = {fixture32, dnc, gnm_100_p010, gnm_500_p005, gnm_2000_p005, gnm_5000_p002}
  variant = mod (Modularity) only

Per cell: run /tmp/louvain_l4_tracer; run kernel_check_l4.mjs (JS replay,
own MT19937 same seed, bit-compare per-visit). PASS = 0 mismatches per
cell. Cumulative target ≥1M visits (the full matrix exceeds 3M).

Inputs:
  fixture32 / dnc — read from tests/cd_verify/edge.csv and dnc_edge.csv
    (same fixtures used by every other CD tracer).
  gnm_*_p* — generated on the fly into the work dir using a deterministic
    graph seed (NOT the run seed; just a fixed graph seed so the matrix
    is reproducible across machines).

Run:
  python tools/viz_check/louvain/kernel_check_l4.py [--seeds 1,7,...]
"""
from __future__ import annotations

import argparse
import os
import random
import subprocess
import sys
from concurrent.futures import ProcessPoolExecutor, as_completed
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "_common"))
import driver as D

REPO = D.repo_root_from_here(HERE)
WORK = D.workdir(REPO)
TRACER = Path("/tmp/louvain_l4_tracer")
JS_REPLAY = HERE / "kernel_check_l4.mjs"

DEFAULT_SEEDS = [1, 7, 13, 42, 99, 137, 1729, 65535, 2147483646]
LOUVAIN_JS_HEAD = "/tmp/louvain_head_l4.js"

# vltanh.github.io is a separate clone (not a submodule) parked at the
# repo root or one of its parents; layout varies by checkout. Walk a
# small candidate list rather than hardcoding one.
WEB_REPO_CANDIDATES = [
    REPO.parent / "web" / "vltanh.github.io",
    REPO.parent / "vltanh.github.io",
    REPO / "vltanh.github.io",
]


def find_web_repo() -> Path:
    for c in WEB_REPO_CANDIDATES:
        if c.is_dir():
            return c
    sys.exit("cannot locate vltanh.github.io repo for HEAD JS extraction")


def ensure_louvain_head_js() -> Path:
    """Pin the HEAD revision of louvain.js to a stable temp path so L4
    verification is robust against uncommitted edits in the working tree
    (caller must keep the JS module byte-identical to the cpp tracer's
    algorithmic mirror; modifying it without re-running L4 breaks
    bit-equality)."""
    web = find_web_repo()
    rc = subprocess.run(
        ["git", "-C", str(web), "show", "HEAD:comdet/js/louvain/louvain.js"],
        capture_output=True, text=True,
    )
    if rc.returncode != 0:
        sys.stderr.write(rc.stderr)
        sys.exit(2)
    Path(LOUVAIN_JS_HEAD).write_text(rc.stdout)
    return Path(LOUVAIN_JS_HEAD)


def gen_gnm_csv(out: Path, n: int, p: float, seed: int) -> None:
    """Deterministic GNM(n, p) edge list."""
    if out.exists():
        return
    rng = random.Random(seed)
    edges = []
    for u in range(n):
        for v in range(u + 1, n):
            if rng.random() < p:
                edges.append((u, v))
    if not edges:
        edges.append((0, 1))
    text = ["source,target"]
    for u, v in edges:
        text.append(f"{u},{v}")
    out.write_text("\n".join(text) + "\n")


def run_cell(name: str, edge: str, seed: int) -> tuple[str, int, bool, int, str]:
    """Run one (input, seed) cell. Returns (name, seed, ok, visits, log).

    `edge` is passed as str so the tuple is picklable across worker
    processes. `LOUVAIN_JS` env is read from os.environ (set in main
    once before fanning out)."""
    edge_path = Path(edge)
    trace = WORK / f"{name}_l4_s{seed}.json"
    rc = subprocess.run([str(TRACER), str(edge_path), str(seed)],
                        capture_output=True, text=True)
    if rc.returncode != 0:
        return name, seed, False, 0, f"tracer failed: {rc.stderr}"
    trace.write_text(rc.stdout)
    rc = subprocess.run(["node", str(JS_REPLAY), str(trace), str(edge_path)],
                        capture_output=True, text=True, env=os.environ.copy())
    log = (rc.stdout + rc.stderr).strip()
    visits = 0
    for line in log.splitlines():
        if line.startswith("L4 visits="):
            try:
                visits = int(line.split("visits=")[1].split()[0])
            except ValueError:
                pass
    return name, seed, rc.returncode == 0, visits, log


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--seeds", default=",".join(str(s) for s in DEFAULT_SEEDS))
    ap.add_argument("--quick", action="store_true",
                    help="fixture32 + dnc only (skip gnm inputs)")
    ap.add_argument("--workers", type=int, default=os.cpu_count() or 4,
                    help="parallel worker processes")
    args = ap.parse_args()
    seeds = [int(s) for s in args.seeds.split(",")]

    D.build_tracer(HERE, TRACER, script_name="build_l4.sh")
    louvain_js = ensure_louvain_head_js()
    os.environ["LOUVAIN_JS"] = str(louvain_js)

    inputs = [
        ("fixture32", WORK / "edge.csv"),
        ("dnc",       WORK / "dnc_edge.csv"),
    ]
    if not args.quick:
        gnms = [
            ("gnm_100_p010",  WORK / "gnm_100_p010.csv",  100,  0.10,  12345),
            ("gnm_500_p005",  WORK / "gnm_500_p005.csv",  500,  0.05,  67890),
            ("gnm_2000_p005", WORK / "gnm_2000_p005.csv", 2000, 0.005, 24680),
            ("gnm_5000_p002", WORK / "gnm_5000_p002.csv", 5000, 0.002, 13579),
        ]
        for name, p, n, prob, gseed in gnms:
            gen_gnm_csv(p, n, prob, seed=gseed)
            inputs.append((name, p))

    cells = [(name, str(edge), seed)
             for name, edge in inputs if edge.exists()
             for seed in seeds]
    skipped = [(name, edge) for name, edge in inputs if not edge.exists()]
    for name, edge in skipped:
        print(f"  {name}: SKIP (missing {edge})")

    print(f"L4 stress matrix: {len(cells)} cells × {args.workers} workers\n")
    total_visits = 0
    failures = 0
    results: list[tuple[str, int, bool, int, str]] = []
    with ProcessPoolExecutor(max_workers=args.workers) as ex:
        futures = [ex.submit(run_cell, *c) for c in cells]
        for fut in as_completed(futures):
            results.append(fut.result())
    # Print in a stable order so output is reproducible.
    results.sort(key=lambda r: (r[0], r[1]))
    for name, seed, ok, visits, log in results:
        total_visits += visits
        tag = "PASS" if ok else "FAIL"
        print(f"  {name} seed={seed:>10}: {tag} visits={visits}")
        if not ok:
            failures += 1
            for line in log.splitlines()[-5:]:
                print(f"    {line}")

    print()
    print(f"Cumulative visits: {total_visits}")
    print(f"Cells failed: {failures}")
    if failures:
        print("OVERALL: FAIL")
        return 1
    if total_visits < 1_000_000:
        print(f"OVERALL: PASS structurally but < 1M visits ({total_visits}).")
        return 0
    print("OVERALL: PASS (≥1M visits, 0 mismatches)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
