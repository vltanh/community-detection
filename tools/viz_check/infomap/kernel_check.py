"""Infomap kernel cross-check driver (3-leg byte-equal).

Three legs:
  - canonical: pypi Infomap with `--seed N --silent --two-level` driven
    via canonical_run.py. Emits com.csv (drop-singletons + ASC-renumber +
    sort-by-node_id) + codelength L_canon + module count.
  - tracer: forked Infomap (replaces InfomapBase.cpp with
    infomap_base_traced.cpp + per-stage [TRACE-IM] hooks inside
    InfomapBase::partition). Same canonical algorithm, byte-equal
    output, plus a JSON trace of stage-by-stage (label, L, leaf_to_top).
  - JS replay: kernel_check.mjs reads the tracer JSON, builds the same
    JS Infomap-style graph, applies the FINAL leaf_to_top, and verifies
    JS map-equation L matches L_canon within 1e-9 (formula byte-equal
    at machine epsilon) + post-processed final partition matches
    canonical com.csv exactly.

The 3-leg byte-equal: canonical com.csv == tracer com.csv (verified by
diff) AND L_canon == L_js_on_canonical_partition AND the JS-canonicalized
final stage partition == canonical com.csv.

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
TRACER = Path("/tmp/infomap_kernel_check")
CANON_RUNNER = HERE / "canonical_run.py"
JS_REPLAY = HERE / "kernel_check.mjs"

CONDA_ENV = os.environ.get("INFOMAP_CONDA_ENV", "nwbench")
PY = ["conda", "run", "-n", CONDA_ENV, "python"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--seed", type=int, default=1)
    args = ap.parse_args()

    D.build_tracer(HERE, TRACER)
    work = D.workdir(REPO)
    D.emit_fixture_inputs(REPO, work)

    failures = 0
    for name, edge, _com in D.fixture_cases(work):
        print(f"\n=== {name} (seed={args.seed}) ===")

        # Leg A: canonical pypi com.csv.
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
        canon_csv_text = canon_csv.read_text()

        # Leg B: tracer (forked InfomapBase.cpp + main_traced.cpp).
        tracer_csv = work / f"{name}_infomap_tracer.csv"
        tracer_json = work / f"{name}_infomap_tracer.json"
        tracer_log = work / f"{name}_infomap_tracer.log"
        with open(tracer_json, "w") as out, open(tracer_log, "w") as err:
            rc = subprocess.run([str(TRACER), str(edge), str(args.seed),
                                 str(tracer_csv)],
                                stdout=out, stderr=err)
        if rc.returncode != 0:
            sys.stderr.write(tracer_log.read_text())
            failures += 1
            continue
        if args.verbose:
            print(tracer_log.read_text())
        tracer_csv_text = tracer_csv.read_text()

        # canon_csv vs tracer_csv (byte-equal; both apply identical
        # drop-singletons + ASC-renumber + sort-by-node_id after the
        # algorithm runs).
        if canon_csv_text == tracer_csv_text:
            print(f"  canonical == tracer: PASS ({len(canon_csv_text.splitlines())} lines)")
        else:
            print(f"  canonical != tracer: FAIL")
            failures += 1

        # Leg C: JS replay.
        ok, log, err = D.run_capture(["node", str(JS_REPLAY),
                                      str(tracer_json), str(canon_csv),
                                      str(edge)])
        last = (log + err).strip().splitlines()[-1] if (log + err).strip() else "(no output)"
        if ok:
            # Print the full JS replay log for transparency.
            for line in log.strip().splitlines():
                print(f"  {line}")
            print(f"  tracer == js_replay: PASS ({last})")
        else:
            print(f"  tracer != js_replay: FAIL")
            print(log + err)
            failures += 1

    print()
    if failures:
        print(f"OVERALL: FAIL ({failures} failures)")
        sys.exit(1)
    print("OVERALL: PASS")


if __name__ == "__main__":
    main()
