"""Louvain L4 self-RNG bit-equal-per-step stress driver.

Stress matrix:
  seeds  = {1, 7, 13, 42, 99, 137, 1729, 65535, 2147483646}
  inputs = {fixture32, dnc, gnm_100_p010, gnm_500_p005}
  variant = mod (Modularity) only

Per cell: run /tmp/louvain_l4_tracer; run kernel_check_l4.mjs (JS replay,
own MT19937 same seed, bit-compare per-visit). PASS = 0 mismatches per
cell. Cumulative ≥1M visits.

Inputs:
  fixture32 / dnc — read from tests/cd_verify/edge.csv and dnc_edge.csv
    (same fixtures used by every other CD tracer).
  gnm_100_p010, gnm_500_p005 — generated on the fly into the work dir
    using a deterministic seed (NOT the run seed; just a fixed graph
    seed so the matrix is reproducible across machines).

Run:
  python tools/viz_check/louvain/kernel_check_l4.py [--seeds 1,7,...]
"""
from __future__ import annotations

import argparse
import os
import random
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent.parent
TRACER = Path("/tmp/louvain_l4_tracer")
JS_REPLAY = HERE / "kernel_check_l4.mjs"
WORK = REPO / "tests" / "cd_verify"

DEFAULT_SEEDS = [1, 7, 13, 42, 99, 137, 1729, 65535, 2147483646]
LOUVAIN_JS_HEAD = "/tmp/louvain_head_l4.js"


def ensure_louvain_head_js() -> Path:
    """Extract the HEAD revision of louvain.js to a stable temp path so
    L4 verification is robust against uncommitted edits in the working
    tree (caller must keep the JS module byte-identical to the cpp
    tracer's algorithmic mirror — modifying it without re-running L4
    breaks bit-equality)."""
    web_repo = REPO.parent / "web" / "vltanh.github.io"
    if not web_repo.is_dir():
        # Repo layout fallback: the user keeps vltanh.github.io as a
        # sibling of community-detection rather than under the parent
        # web/ dir. Walk up until we find it.
        candidates = [
            REPO.parent / "vltanh.github.io",
            REPO / "vltanh.github.io",
        ]
        for c in candidates:
            if c.is_dir():
                web_repo = c
                break
    if not web_repo.is_dir():
        sys.exit("cannot locate vltanh.github.io repo for HEAD JS extraction")
    rc = subprocess.run(
        ["git", "-C", str(web_repo), "show", "HEAD:comdet/js/louvain/louvain.js"],
        capture_output=True, text=True,
    )
    if rc.returncode != 0:
        sys.stderr.write(rc.stderr)
        sys.exit(2)
    Path(LOUVAIN_JS_HEAD).write_text(rc.stdout)
    return Path(LOUVAIN_JS_HEAD)


def gen_gnm_csv(out: Path, n: int, p: float, seed: int) -> None:
    """Deterministic GNM(n, p) edge list. Used to generate stress-matrix
    inputs that aren't already in tests/cd_verify."""
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


def run_cell(name: str, edge: Path, seed: int, louvain_js: Path) -> tuple[bool, int, str]:
    """Run one (input, seed) cell. Returns (ok, visits, log)."""
    trace = WORK / f"{name}_l4_s{seed}.json"
    rc = subprocess.run([str(TRACER), str(edge), str(seed)],
                        capture_output=True, text=True)
    if rc.returncode != 0:
        return False, 0, f"tracer failed: {rc.stderr}"
    trace.write_text(rc.stdout)
    env = os.environ.copy()
    env["LOUVAIN_JS"] = str(louvain_js)
    rc = subprocess.run(["node", str(JS_REPLAY), str(trace), str(edge)],
                        capture_output=True, text=True, env=env)
    log = (rc.stdout + rc.stderr).strip()
    visits = 0
    for line in log.splitlines():
        if line.startswith("L4 visits="):
            try:
                visits = int(line.split("visits=")[1].split()[0])
            except Exception:
                pass
    return rc.returncode == 0, visits, log


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--seeds", default=",".join(str(s) for s in DEFAULT_SEEDS))
    ap.add_argument("--quick", action="store_true",
                    help="fixture32 + dnc only (skip gnm inputs)")
    args = ap.parse_args()
    seeds = [int(s) for s in args.seeds.split(",")]

    if not TRACER.exists():
        # Auto-build.
        rc = subprocess.run(["bash", str(HERE / "instrumented" / "build_l4.sh")],
                            capture_output=True, text=True)
        if rc.returncode != 0:
            sys.stderr.write(rc.stderr)
            return 2

    louvain_js = ensure_louvain_head_js()

    # Inputs.
    inputs = [
        ("fixture32", WORK / "edge.csv"),
        ("dnc",       WORK / "dnc_edge.csv"),
    ]
    if not args.quick:
        gnm_a = WORK / "gnm_100_p010.csv";   gen_gnm_csv(gnm_a, 100,   0.10,    seed=12345)
        gnm_b = WORK / "gnm_500_p005.csv";   gen_gnm_csv(gnm_b, 500,   0.05,    seed=67890)
        gnm_c = WORK / "gnm_2000_p005.csv";  gen_gnm_csv(gnm_c, 2000,  0.005,   seed=24680)
        gnm_d = WORK / "gnm_5000_p002.csv";  gen_gnm_csv(gnm_d, 5000,  0.002,   seed=13579)
        inputs.extend([
            ("gnm_100_p010",  gnm_a),
            ("gnm_500_p005",  gnm_b),
            ("gnm_2000_p005", gnm_c),
            ("gnm_5000_p002", gnm_d),
        ])

    total_visits = 0
    failures = 0
    print(f"L4 stress matrix: {len(inputs)} inputs × {len(seeds)} seeds = {len(inputs) * len(seeds)} cells\n")
    for name, edge in inputs:
        if not edge.exists():
            print(f"  {name}: SKIP (missing {edge})")
            continue
        for seed in seeds:
            ok, visits, log = run_cell(name, edge, seed, louvain_js)
            total_visits += visits
            tag = "PASS" if ok else "FAIL"
            print(f"  {name} seed={seed:>10}: {tag} visits={visits}")
            if not ok:
                failures += 1
                # Print last 5 lines of log for diagnostic.
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
