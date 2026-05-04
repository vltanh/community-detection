"""WCC kernel cross-check driver.

Three legs:

1. **Canonical** — `constrained_clustering MincutOnly --connectedness-criterion 1log_10(n)`.
2. **Instrumented C++** — /tmp/wcc_kernel_check; verbatim copy of the WCC pipeline
   (single-threaded MinCutWorker loop) linked against constrained-clustering's
   libinternal_libs.a (so it uses VieCut's cactus mincut, byte-equal to canonical).
3. **JS replay** — comdet/js/wcc/wcc.js with cutOracle injection. JS reads the
   C++ tracer's per-pop bipartition and injects it instead of running its own
   Stoer-Wagner. Asserts byte-equal pop trace + survivors.

Run:
    python tools/viz_check/wcc/kernel_check.py [--verbose]
"""
from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent.parent
BIN = REPO / "constrained-clustering" / "build" / "bin" / "constrained_clustering"
TRACER = Path("/tmp/wcc_kernel_check")
JS_REPLAY = HERE / "kernel_check.mjs"


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


def run_canonical(edge: Path, com: Path, out: Path, criterion: str) -> str:
    rc = subprocess.run([str(BIN), "MincutOnly",
                         "--edgelist", str(edge),
                         "--existing-clustering", str(com),
                         "--output-file", str(out),
                         "--log-file", str(out.with_suffix(".log")),
                         "--connectedness-criterion", criterion,
                         "--num-processors", "1"],
                        capture_output=True, text=True)
    if rc.returncode != 0:
        raise RuntimeError(f"binary failed: {rc.stderr}")
    return out.read_text()


def run_tracer(edge: Path, com: Path, out: Path, criterion: str):
    rc = subprocess.run([str(TRACER), str(edge), str(com), str(out), criterion],
                        capture_output=True, text=True)
    if rc.returncode != 0:
        raise RuntimeError(f"tracer failed: {rc.stderr}")
    return out.read_text(), rc.stdout, rc.stderr


def run_js_replay(trace_json: Path, edge: Path, com: Path, criterion: str):
    rc = subprocess.run(["node", str(JS_REPLAY), str(trace_json),
                         str(edge), str(com), criterion],
                        capture_output=True, text=True)
    return rc.returncode == 0, rc.stdout + rc.stderr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    if not BIN.exists():
        sys.exit(f"missing binary: {BIN}")
    build_tracer()

    work = REPO / "tests" / "cd_verify"
    work.mkdir(parents=True, exist_ok=True)
    emit_fixture_inputs(work)

    cases = [("fixture32", work / "edge.csv", work / "com_gt.csv", "1log_10(n)")]
    if (work / "dnc_edge.csv").exists():
        cases.append(("dnc", work / "dnc_edge.csv", work / "dnc_com.csv", "1log_10(n)"))

    failures = 0
    for name, edge, com, crit in cases:
        print(f"\n=== {name} (criterion={crit}) ===")
        canon_out = work / f"{name}_wcc_canon.csv"
        tracer_out = work / f"{name}_wcc_tracer.csv"
        tracer_json = work / f"{name}_wcc_tracer.json"
        canon_csv = run_canonical(edge, com, canon_out, crit)
        tracer_csv, tracer_stdout, tracer_stderr = run_tracer(edge, com, tracer_out, crit)
        tracer_json.write_text(tracer_stdout)
        if args.verbose:
            print(tracer_stderr)
        if canon_csv == tracer_csv:
            print(f"  canonical == tracer: PASS ({len(canon_csv.splitlines())} lines)")
        else:
            print(f"  canonical != tracer: FAIL")
            failures += 1
        ok, log = run_js_replay(tracer_json, edge, com, crit)
        last = log.strip().splitlines()[-1] if log.strip() else "(no output)"
        if ok:
            print(f"  tracer == js_replay: PASS ({last})")
        else:
            print(f"  tracer != js_replay: FAIL")
            print(log)
            failures += 1

    print()
    if failures:
        print(f"OVERALL: FAIL ({failures} failures)")
        sys.exit(1)
    print("OVERALL: PASS")


if __name__ == "__main__":
    main()
