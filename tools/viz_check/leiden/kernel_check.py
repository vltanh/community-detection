"""Leiden kernel cross-check driver (libleidenalg byte-equal trace).

Two-leg verification:
  1. Forked libleidenalg Optimiser.cpp (with [TRACE-LD] capture in
     move_nodes) at /tmp/leiden_kernel_check produces byte-equal trace
     of every pass's random_order + per-visit move sequence.
  2. JS replay applies the canonical move sequence to comdet/js/leiden's
     Partition. Currently FAILS: JS Leiden CPM uses a different weight
     convention than libleidenalg (2x scaling + intra-edge double-count).

Run:
    python tools/viz_check/leiden/kernel_check.py [--verbose] [--seed N]
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
TRACER = Path("/tmp/leiden_kernel_check")
JS_REPLAY = HERE / "kernel_check.mjs"
QUALITY = "cpm"
PARAM = 0.0001


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--seed", type=int, default=42)
    args = ap.parse_args()

    D.build_tracer(HERE, TRACER)
    work = D.workdir(REPO)
    D.emit_fixture_inputs(REPO, work)

    failures = 0
    for name, edge, _com in D.fixture_cases(work):
        print(f"\n=== {name} (quality={QUALITY}, param={PARAM}, seed={args.seed}) ===")
        out = work / f"{name}_leiden_tracer.csv"
        rc = subprocess.run([str(TRACER), str(edge), str(out),
                             QUALITY, str(PARAM), str(args.seed)],
                            capture_output=True, text=True)
        if rc.returncode != 0:
            sys.stderr.write(rc.stderr)
            failures += 1
            continue
        trace_json = work / f"{name}_leiden_tracer.json"
        trace_json.write_text(rc.stdout)
        if args.verbose:
            print(rc.stderr)
        ok, log, err = D.run_capture(["node", str(JS_REPLAY), str(trace_json),
                                      str(edge), QUALITY, str(PARAM)])
        last = (log + err).strip().splitlines()[-1] if (log + err).strip() else "(no output)"
        if ok:
            print(f"  JS replay vs canonical: PASS ({last})")
        else:
            print(f"  JS replay vs canonical: FAIL")
            print(log + err)
            failures += 1

    print()
    if failures:
        print(f"OVERALL: FAIL ({failures} failures)")
        print()
        print("The CANONICAL byte-equal trace is captured (see *_leiden_tracer.json):")
        print("every pass's shuffled_nodes + every visit's (v, fromComm, toComm,")
        print("dQ, moved) is recorded from the forked libleidenalg Optimiser.")
        print("JS replay fails because comdet/js/leiden CPM convention diverges")
        print("from libleidenalg's. See leiden_tracer_verify.md for fix scope.")
        sys.exit(1)
    print("OVERALL: PASS")


if __name__ == "__main__":
    main()
