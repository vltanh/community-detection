"""Louvain kernel cross-check driver.

Pipeline: emit fixture -> convert edge.csv to gen-louvain binary
(via `convert` utility) -> run instrumented tracer with seeded srand
-> JS replay reads tracer's random_order + per-visit moves, asserts
byte-equal level-0 trace + Q match.

Run:
    python tools/viz_check/louvain/kernel_check.py [--verbose] [--seed N]
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
TRACER = Path("/tmp/louvain_kernel_check")
JS_REPLAY = HERE / "kernel_check.mjs"
CONVERT = REPO / "externals" / "louvain" / "convert"


def convert_to_bin(edge: Path, work: Path):
    """edge.csv -> louvain.bin + relabel.txt via gen-louvain's convert."""
    txt = work / "louvain_edge.txt"
    txt.write_text("\n".join([
        line.replace(",", " ")
        for line in edge.read_text().strip().splitlines()[1:]
    ]) + "\n")
    binp = work / "louvain.bin"
    relabel = work / "louvain_relabel.txt"
    rc = subprocess.run([str(CONVERT), "-i", str(txt), "-o", str(binp),
                         "-r", str(relabel)],
                        capture_output=True, text=True)
    if rc.returncode != 0:
        raise RuntimeError(f"convert failed: {rc.stderr}")
    return binp, relabel


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--seed", type=int, default=42)
    args = ap.parse_args()

    if not CONVERT.exists():
        sys.exit(f"missing convert: {CONVERT}")
    D.build_tracer(HERE, TRACER)
    work = D.workdir(REPO)
    D.emit_fixture_inputs(REPO, work)

    failures = 0
    for name, edge, _com in D.fixture_cases(work):
        print(f"\n=== {name} (seed={args.seed}) ===")
        graph_bin, relabel = convert_to_bin(edge, work)
        rc = subprocess.run([str(TRACER), str(graph_bin), str(args.seed), str(relabel)],
                            capture_output=True, text=True)
        if rc.returncode != 0:
            sys.stderr.write(rc.stderr)
            failures += 1
            continue
        trace_json = work / f"{name}_louvain_tracer.json"
        trace_json.write_text(rc.stdout)
        if args.verbose:
            print(rc.stderr)
        ok, log, _err = D.run_capture(["node", str(JS_REPLAY), str(trace_json), str(edge)])
        last = (log + _err).strip().splitlines()[-1] if (log + _err).strip() else "(no output)"
        if ok:
            print(f"  JS replay vs canonical level-0: PASS ({last})")
        else:
            print(f"  JS replay vs canonical level-0: FAIL")
            print(log + _err)
            failures += 1

    print()
    if failures:
        print(f"OVERALL: FAIL ({failures} failures)")
        sys.exit(1)
    print("OVERALL: PASS")
    print("\nNOTE: this verifies LEVEL 0 only (RNG-driven random_order +")
    print("per-visit moves byte-equal). Multi-level chain verification is")
    print("structural-only; see tracer source for the path to extending.")


if __name__ == "__main__":
    main()
