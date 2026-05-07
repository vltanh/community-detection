"""Batch Infomap byte-equal verification across empirical networks.

For each network in data/empirical_networks/networks/:
  1. Run canonical pypi Infomap (canonical_run.py) -> canon.csv + L.
  2. Run forked tracer (/tmp/infomap_kernel_check) -> tracer.csv + trace.json.
  3. diff canon.csv vs tracer.csv (byte-equal partition).
  4. Run kernel_check.mjs JS replay -> verify max |ΔL| == 0
     (deterministic-core bit-identical via deltasOracle).

Filters networks by size: --max-nodes (default 2000), --max-edges
(default 50000). Skips networks where any of the three legs fails;
emits a per-network status row.

Run:
    conda run -n nwbench python tools/viz_check/infomap/batch_empirical.py [--seed N] [--max-nodes M] [--max-edges E]
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "_common"))
import driver as D

REPO = D.repo_root_from_here(HERE)
DATA = REPO.parent / "data" / "empirical_networks"
TRACER = Path("/tmp/infomap_kernel_check")
CANON_RUNNER = HERE / "canonical_run.py"
JS_REPLAY = HERE / "kernel_check.mjs"

CONDA_ENV = os.environ.get("INFOMAP_CONDA_ENV", "nwbench")
PY = ["conda", "run", "-n", CONDA_ENV, "python"]


def list_networks(max_nodes: int, max_edges: int):
    cases = []
    nets_dir = DATA / "networks"
    stats_dir = DATA / "stats"
    for name in sorted(os.listdir(nets_dir)):
        edge = nets_dir / name / f"{name}.csv"
        if not edge.is_file():
            continue
        try:
            n = int((stats_dir / name / "n_nodes.txt").read_text().strip())
            m = int((stats_dir / name / "n_edges.txt").read_text().strip())
        except Exception:
            continue
        if n > max_nodes or m > max_edges:
            continue
        cases.append((n, m, name, edge))
    cases.sort()
    return cases


def run_one(name: str, edge: Path, seed: int, work: Path,
            verbose: bool) -> tuple[str, str]:
    """Returns (status, detail). status in {PASS, FAIL_canon, FAIL_tracer,
    FAIL_diff, FAIL_replay, FAIL_skip}."""
    canon_csv = work / f"{name}_canon.csv"
    canon_stats = work / f"{name}_canon_stats.json"
    tracer_csv = work / f"{name}_tracer.csv"
    tracer_json = work / f"{name}_tracer.json"
    tracer_log = work / f"{name}_tracer.log"

    # Leg A: canonical pypi.
    rc = subprocess.run(PY + [str(CANON_RUNNER), str(edge), str(seed),
                              str(canon_csv)],
                        capture_output=True, text=True)
    if rc.returncode != 0:
        return ("FAIL_canon", rc.stderr.strip()[:200])
    canon_stats.write_text(rc.stdout)
    canon_csv_text = canon_csv.read_text()

    # Leg B: tracer.
    with open(tracer_json, "w") as out, open(tracer_log, "w") as err:
        rc = subprocess.run([str(TRACER), str(edge), str(seed),
                             str(tracer_csv)], stdout=out, stderr=err)
    if rc.returncode != 0:
        return ("FAIL_tracer", tracer_log.read_text().strip()[:200])
    tracer_csv_text = tracer_csv.read_text()

    # Diff canon vs tracer.
    if canon_csv_text != tracer_csv_text:
        return ("FAIL_diff", "canon != tracer com.csv")

    # Leg C: JS replay.
    rc = subprocess.run(["node", str(JS_REPLAY), str(tracer_json),
                         str(canon_csv)], capture_output=True, text=True)
    out = rc.stdout
    if rc.returncode != 0:
        return ("FAIL_replay", (out + rc.stderr).strip().splitlines()[-1][:200])
    last_line = out.strip().splitlines()[-1] if out.strip() else "(empty)"
    # Pull max |ΔL| from JS replay output.
    max_dl = "?"
    for line in out.splitlines():
        if "max |ΔL|" in line:
            max_dl = line.strip().split("=")[-1].strip()
            break
    return ("PASS", f"max|ΔL|={max_dl}; {last_line[:80]}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--max-nodes", type=int, default=2000)
    ap.add_argument("--max-edges", type=int, default=50000)
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    if not TRACER.exists():
        D.build_tracer(HERE, TRACER)

    cases = list_networks(args.max_nodes, args.max_edges)
    print(f"Found {len(cases)} networks (n<={args.max_nodes}, m<={args.max_edges})")
    print()
    print(f"{'n':>6} {'m':>7}  {'name':<28}  {'status':<10}  detail")
    print("-" * 100)

    work = D.workdir(REPO) / "infomap_empirical"
    work.mkdir(exist_ok=True)
    counts = {"PASS": 0, "FAIL_canon": 0, "FAIL_tracer": 0,
              "FAIL_diff": 0, "FAIL_replay": 0, "FAIL_skip": 0}
    failures = []
    for n, m, name, edge in cases:
        status, detail = run_one(name, edge, args.seed, work, args.verbose)
        counts[status] = counts.get(status, 0) + 1
        marker = "✓" if status == "PASS" else "✗"
        print(f"{n:6d} {m:7d}  {name:<28}  {marker} {status:<8}  {detail}")
        if status != "PASS":
            failures.append((name, status, detail))

    print()
    print(f"Summary: {counts.get('PASS', 0)}/{len(cases)} PASS")
    for k, v in counts.items():
        if k != "PASS" and v > 0:
            print(f"  {k}: {v}")
    if failures:
        sys.exit(1)


if __name__ == "__main__":
    main()
