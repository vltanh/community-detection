"""Infomap structural cross-check driver.

NOT 3-leg byte-equal. The JS Infomap kernel
(comdet/js/infomap/infomap.js) is a faithful undirected-unweighted
port of Rosvall + Bergstrom 2008 with greedy pair-joining + greedy
single-node tuning. The canonical pypi Infomap C++ implementation
uses different optimisation heuristics + supports many more features.
The two are different algorithmic instantiations of the same paper,
so byte-equal partition is impossible by design.

What this verifier does:
  - Run canonical pypi Infomap with --seed=N --silent --two-level on
    the fixture to produce canonical com.csv + codelength L_canon.
  - Run JS runInfomap on the same fixture with the same seed; compute
    JS partition + JS map-equation L_js.
  - Compute JS map-equation on the canonical partition (L_js_on_canon).
    This validates the JS map-equation formula matches canonical's:
    if |L_js_on_canon - L_canon| < 1e-3, the JS formula is correct.
  - Report ARI between JS and canonical partitions (informational).

Acceptance: |L_js_on_canon - L_canon| < 1e-3.

Run:
    python tools/viz_check/infomap/kernel_check.py [--verbose] [--seed N]
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
CANON_RUNNER = HERE / "canonical_run.py"
JS_REPLAY = HERE / "kernel_check.mjs"

CONDA_ENV = os.environ.get("INFOMAP_CONDA_ENV", "nwbench")
PY = ["conda", "run", "-n", CONDA_ENV, "python"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--seed", type=int, default=1)
    args = ap.parse_args()

    work = D.workdir(REPO)
    D.emit_fixture_inputs(REPO, work)

    failures = 0
    for name, edge, _com in D.fixture_cases(work):
        print(f"\n=== {name} (seed={args.seed}) ===")
        canon_csv = work / f"{name}_infomap_canon.csv"
        canon_stats = work / f"{name}_infomap_canon_stats.json"
        rc = subprocess.run(PY + [str(CANON_RUNNER), str(edge),
                                  str(args.seed), str(canon_csv)],
                            capture_output=True, text=True)
        if rc.returncode != 0:
            sys.stderr.write(rc.stderr)
            failures += 1
            continue
        canon_stats.write_text(rc.stdout)
        if args.verbose:
            print(rc.stderr)

        ok, log, err = D.run_capture(["node", str(JS_REPLAY),
                                      str(canon_csv), str(canon_stats),
                                      str(edge), str(args.seed)])
        print(log + err, end="")
        if not ok:
            failures += 1

    print()
    if failures:
        print(f"OVERALL: FAIL ({failures} failures)")
        sys.exit(1)
    print("OVERALL: PASS")


if __name__ == "__main__":
    main()
