"""Louvain kernel cross-check driver (level 0 byte-equal trace).

Pipeline:

1. Build instrumented tracer at /tmp/louvain_kernel_check.
2. Use gen-louvain's `convert` utility to convert edge.csv to the
   canonical binary graph format (with relabel.txt).
3. Run /tmp/louvain_kernel_check graph.bin <seed> relabel.txt to get
   level-0 trace (random_order + per-visit moves + final Q).
4. Run JS replay (kernel_check.mjs) with the canonical's random_order
   injected; assert byte-equal per-visit node order + same per-pass
   move count + Q match within 1e-3 (long-double->double rounding).

Run:
    python tools/viz_check/louvain/kernel_check.py [--verbose] [--seed N]
"""
from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent.parent
TRACER = Path("/tmp/louvain_kernel_check")
JS_REPLAY = HERE / "kernel_check.mjs"
CONVERT = REPO / "externals" / "louvain" / "convert"


def build_tracer():
    rc = subprocess.run(["bash", str(HERE / "instrumented" / "build.sh")],
                        capture_output=True, text=True)
    if rc.returncode != 0:
        sys.stderr.write(rc.stdout + rc.stderr)
        sys.exit(2)


def emit_fixture_inputs(out_dir: Path):
    fixture_emitter = REPO / "tests" / "cd_verify" / "emit_fixture.js"
    rc = subprocess.run(["node", str(fixture_emitter), str(out_dir)],
                        capture_output=True, text=True)
    if rc.returncode != 0:
        sys.stderr.write(rc.stdout + rc.stderr)
        sys.exit(2)


def convert_to_bin(edge: Path, work: Path):
    """edge.csv -> louvain.bin + relabel.txt via gen-louvain's `convert`."""
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


def run_tracer(graph_bin: Path, seed: int, relabel: Path) -> tuple[str, str]:
    rc = subprocess.run([str(TRACER), str(graph_bin), str(seed), str(relabel)],
                        capture_output=True, text=True)
    if rc.returncode != 0:
        raise RuntimeError(f"tracer failed: {rc.stderr}")
    return rc.stdout, rc.stderr


def run_js_replay(trace_json: Path, edge: Path) -> tuple[bool, str]:
    rc = subprocess.run(["node", str(JS_REPLAY), str(trace_json), str(edge)],
                        capture_output=True, text=True)
    return rc.returncode == 0, rc.stdout + rc.stderr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--seed", type=int, default=42)
    args = ap.parse_args()

    if not CONVERT.exists():
        sys.exit(f"missing convert: {CONVERT}")
    build_tracer()

    work = REPO / "tests" / "cd_verify"
    emit_fixture_inputs(work)
    edge = work / "edge.csv"

    cases = [("fixture32", edge)]
    if (work / "dnc_edge.csv").exists():
        cases.append(("dnc", work / "dnc_edge.csv"))

    failures = 0
    for name, e in cases:
        print(f"\n=== {name} (seed={args.seed}) ===")
        graph_bin, relabel = convert_to_bin(e, work)
        stdout, stderr = run_tracer(graph_bin, args.seed, relabel)
        trace_json = work / f"{name}_louvain_tracer.json"
        trace_json.write_text(stdout)
        if args.verbose:
            print(stderr)
        ok, log = run_js_replay(trace_json, e)
        last = log.strip().splitlines()[-1] if log.strip() else "(no output)"
        if ok:
            print(f"  JS replay vs canonical level-0: PASS ({last})")
        else:
            print(f"  JS replay vs canonical level-0: FAIL")
            print(log)
            failures += 1

    print()
    if failures:
        print(f"OVERALL: FAIL ({failures} failures)")
        sys.exit(1)
    print("OVERALL: PASS")
    print("\nNOTE: this verifies LEVEL 0 only (RNG-driven random_order +")
    print("per-visit moves byte-equal). Multi-level chain verification is")
    print("structural-only at the moment; see tracer source for the")
    print("path to extending to all levels.")


if __name__ == "__main__":
    main()
