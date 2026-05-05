"""CM kernel cross-check driver.

Three legs:

1. **Canonical** — `constrained_clustering CM --algorithm leiden-cpm --clustering-parameter 0.0001 --connectedness-criterion 1log_10(n)`.
2. **Instrumented C++** — /tmp/cm_kernel_check; verbatim copy of the CM
   pipeline (single-threaded, --prune false, leiden-cpm) linked against
   constrained-clustering's libinternal_libs.a + libleidenalg + libigraph.
   Logs every pop's mincut bipartition AND every recluster's Leiden
   output for both sides.
3. **JS replay** — comdet/js/cm/cm.js with cutOracle + baseAlgoFn
   injection (parentNodes context). JS reads the C++ trace and feeds
   per-pop cuts + per-recluster Leiden partitions deterministically.
   Asserts byte-equal pop trace + lineage-tagged survivors.

Run:
    python tools/viz_check/cm/kernel_check.py [--verbose]
"""
from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent.parent
BIN = REPO / "constrained-clustering" / "build" / "bin" / "constrained_clustering"
TRACER = Path("/tmp/cm_kernel_check")
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


def run_canonical(edge: Path, com: Path, out: Path, criterion: str, resolution: float):
    rc = subprocess.run([str(BIN), "CM",
                         "--edgelist", str(edge),
                         "--existing-clustering", str(com),
                         "--output-file", str(out),
                         "--log-file", str(out.with_suffix(".log")),
                         "--history-file", str(out.with_suffix(".hist")),
                         "--algorithm", "leiden-cpm",
                         "--clustering-parameter", str(resolution),
                         "--connectedness-criterion", criterion,
                         "--num-processors", "1"],
                        capture_output=True, text=True)
    if rc.returncode != 0:
        raise RuntimeError(f"binary failed: {rc.stderr}")
    return out.read_text()


def run_tracer(edge: Path, com: Path, out: Path, criterion: str, resolution: float):
    rc = subprocess.run([str(TRACER), str(edge), str(com), str(out),
                         criterion, str(resolution), "0"],
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

    cases = [("fixture32", work / "edge.csv", work / "com_gt.csv", "1log_10(n)", 0.0001)]
    if (work / "dnc_edge.csv").exists():
        cases.append(("dnc", work / "dnc_edge.csv", work / "dnc_com.csv", "1log_10(n)", 0.0001))

    failures = 0
    for name, edge, com, crit, res in cases:
        print(f"\n=== {name} (criterion={crit}, resolution={res}) ===")
        canon_out = work / f"{name}_cm_canon.csv"
        tracer_out = work / f"{name}_cm_tracer.csv"
        tracer_json = work / f"{name}_cm_tracer.json"
        canon_csv = run_canonical(edge, com, canon_out, crit, res)
        tracer_csv, tracer_stdout, tracer_stderr = run_tracer(edge, com, tracer_out, crit, res)
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
