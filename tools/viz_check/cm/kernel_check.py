"""CM kernel cross-check driver.

Run:
    python tools/viz_check/cm/kernel_check.py [--verbose]
"""
from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "_common"))
import driver as D

REPO = D.repo_root_from_here(HERE)
BIN = REPO / "constrained-clustering" / "build" / "bin" / "constrained_clustering"
TRACER = Path("/tmp/cm_kernel_check")
JS_REPLAY = HERE / "kernel_check.mjs"
CRITERION = "1log_10(n)"
RESOLUTION = 0.0001


def canonical_fn(edge: Path, com: Path, work: Path, name: str):
    out = work / f"{name}_cm_canon.csv"
    rc = subprocess.run([str(BIN), "CM",
                         "--edgelist", str(edge),
                         "--existing-clustering", str(com),
                         "--output-file", str(out),
                         "--log-file", str(out.with_suffix(".log")),
                         "--history-file", str(out.with_suffix(".hist")),
                         "--algorithm", "leiden-cpm",
                         "--clustering-parameter", str(RESOLUTION),
                         "--connectedness-criterion", CRITERION,
                         "--num-processors", "1"],
                        capture_output=True, text=True)
    if rc.returncode != 0:
        raise RuntimeError(f"binary failed: {rc.stderr}")
    return out.read_text(), rc.stdout + rc.stderr


def tracer_fn(edge: Path, com: Path, work: Path, name: str):
    out = work / f"{name}_cm_tracer.csv"
    json_path = work / f"{name}_cm_tracer.json"
    rc = subprocess.run([str(TRACER), str(edge), str(com), str(out),
                         CRITERION, str(RESOLUTION), "0"],
                        capture_output=True, text=True)
    if rc.returncode != 0:
        raise RuntimeError(f"tracer failed: {rc.stderr}")
    json_path.write_text(rc.stdout)
    return out.read_text(), json_path, rc.stderr


def replay_fn(json_path: Path, edge: Path, com: Path):
    rc = subprocess.run(["node", str(JS_REPLAY), str(json_path),
                         str(edge), str(com), CRITERION],
                        capture_output=True, text=True)
    return rc.returncode == 0, rc.stdout + rc.stderr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    if not BIN.exists():
        sys.exit(f"missing binary: {BIN}")
    D.build_tracer(HERE, TRACER)
    work = D.workdir(REPO)
    D.emit_fixture_inputs(REPO, work)

    failures = 0
    for name, edge, com in D.fixture_cases(work):
        print(f"  (criterion={CRITERION}, resolution={RESOLUTION})")
        failures += D.run_three_legs(
            name, edge, com, work,
            canonical_fn=canonical_fn, tracer_fn=tracer_fn, replay_fn=replay_fn,
            verbose=args.verbose,
        )

    print()
    if failures:
        print(f"OVERALL: FAIL ({failures} failures)")
        sys.exit(1)
    print("OVERALL: PASS")


if __name__ == "__main__":
    main()
