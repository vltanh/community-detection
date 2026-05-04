"""CC kernel cross-check driver.

Three legs per fixture:

1. **Canonical leg** — `constrained_clustering MincutOnly --connectedness-criterion 0`.
2. **Instrumented C++ leg** — /tmp/cc_kernel_check; verbatim copy of the CC pipeline
   (mincut_only.cpp Simple branch + GetConnectedComponents) with stderr trace at
   every key state. Asserts byte-equal CSV output vs canonical.
3. **JS replay leg** — tools/viz_check/cc/kernel_check.mjs; consumes the C++ tracer's
   stdout JSON, runs comdet/js/cc/cc.js on the same input, asserts byte-equal
   per-cluster node lists.

CC has no RNG. The three legs must produce identical output.

Run:
    python tools/viz_check/cc/kernel_check.py
    python tools/viz_check/cc/kernel_check.py --verbose
"""
from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent.parent  # community-detection/
BIN = REPO / "constrained-clustering" / "build" / "bin" / "constrained_clustering"
TRACER = Path("/tmp/cc_kernel_check")
JS_REPLAY = HERE / "kernel_check.mjs"


def build_tracer():
    rc = subprocess.run(["bash", str(HERE / "instrumented" / "build.sh")],
                        capture_output=True, text=True)
    if rc.returncode != 0:
        sys.stderr.write(rc.stdout + rc.stderr)
        sys.exit(2)


def emit_fixture_inputs(out_dir: Path):
    """Emit edge.csv + com_gt.csv from the comdet 32-node fixture."""
    fixture_emitter = REPO / "tests" / "cd_verify" / "emit_fixture.js"
    rc = subprocess.run(["node", str(fixture_emitter), str(out_dir)],
                        capture_output=True, text=True)
    if rc.returncode != 0:
        sys.stderr.write(rc.stdout + rc.stderr)
        sys.exit(2)


def run_canonical(edge: Path, com: Path, out: Path) -> str:
    log = out.with_suffix(".log")
    rc = subprocess.run([str(BIN), "MincutOnly",
                         "--edgelist", str(edge),
                         "--existing-clustering", str(com),
                         "--output-file", str(out),
                         "--log-file", str(log),
                         "--connectedness-criterion", "0",
                         "--num-processors", "1"],
                        capture_output=True, text=True)
    if rc.returncode != 0:
        raise RuntimeError(f"binary failed: {rc.stderr}")
    return out.read_text()


def run_tracer(edge: Path, com: Path, out: Path) -> tuple[str, str, str]:
    rc = subprocess.run([str(TRACER), str(edge), str(com), str(out)],
                        capture_output=True, text=True)
    if rc.returncode != 0:
        raise RuntimeError(f"tracer failed: {rc.stderr}")
    return out.read_text(), rc.stdout, rc.stderr


def run_js_replay(trace_json: Path, edge: Path, com: Path) -> tuple[bool, str]:
    rc = subprocess.run(["node", str(JS_REPLAY), str(trace_json),
                         str(edge), str(com)],
                        capture_output=True, text=True)
    return rc.returncode == 0, rc.stdout + rc.stderr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    if not BIN.exists():
        sys.exit(f"missing binary: {BIN}; build first")

    build_tracer()

    fixtures = []
    # 1. 32-node comdet fixture.
    work = REPO / "tests" / "cd_verify"
    work.mkdir(parents=True, exist_ok=True)
    emit_fixture_inputs(work)
    fixtures.append(("fixture32", work / "edge.csv", work / "com_gt.csv"))
    # 2. dnc with leiden-mod input clustering (already staged in cd_verify/).
    if (work / "dnc_edge.csv").exists() and (work / "dnc_com.csv").exists():
        fixtures.append(("dnc", work / "dnc_edge.csv", work / "dnc_com.csv"))

    failures = 0
    for name, edge, com in fixtures:
        print(f"\n=== {name} ===")
        canon_out = work / f"{name}_canon.csv"
        tracer_out = work / f"{name}_tracer.csv"
        tracer_json = work / f"{name}_tracer.json"
        canon_csv = run_canonical(edge, com, canon_out)
        tracer_csv, tracer_stdout, tracer_stderr = run_tracer(edge, com, tracer_out)
        tracer_json.write_text(tracer_stdout)
        if args.verbose:
            print("[tracer trace]")
            print(tracer_stderr)

        # Leg 1 vs Leg 2: byte-equal CSV.
        if canon_csv == tracer_csv:
            print(f"  canonical == tracer: PASS (byte-equal CSV, {len(canon_csv.splitlines())} lines)")
        else:
            print(f"  canonical != tracer: FAIL")
            failures += 1

        # Leg 2 vs Leg 3: JS replay.
        ok, replay_log = run_js_replay(tracer_json, edge, com)
        last = replay_log.strip().splitlines()[-1] if replay_log.strip() else "(no output)"
        if ok:
            print(f"  tracer == js_replay: PASS ({last})")
        else:
            print(f"  tracer != js_replay: FAIL")
            print(replay_log)
            failures += 1

    print()
    if failures:
        print(f"OVERALL: FAIL ({failures} failures)")
        sys.exit(1)
    print("OVERALL: PASS")


if __name__ == "__main__":
    main()
