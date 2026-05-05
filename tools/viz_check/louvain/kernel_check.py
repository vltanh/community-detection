"""Louvain kernel cross-check driver (multi-level).

Three legs:
  - canonical: src/louvain/run_louvain.py (convert + louvain + hierarchy)
    with LD_PRELOAD seed shim, --quality mod --seed=<seed>. Emits com.csv.
  - tracer: instrumented gen-louvain (forked Optimiser.cpp + main_traced.cpp)
    captures every level's random_order + per-visit moves + n2c +
    composed fine_membership + Q_final.
  - JS replay: kernel_check.mjs replays canonical move sequence onto a
    JS Partition + collapses per level via canonical-style renumber-by-
    original-comm-id-ASC. Verifies: per-level membership byte-equal vs
    cpp.n2c; composed fine_membership byte-equal vs cpp.fine_membership;
    JS Q.quality on (G_original, cpp.fine_membership) matches cpp.Q_final
    within 1e-9.

The 3-leg byte-equal: canonical pipeline com.csv == tracer-composed
com.csv (singletons stripped, comm-ids renumbered 0..K-1, sorted by
node_id) AND tracer trace == JS replay.

Run:
    python tools/viz_check/louvain/kernel_check.py [--verbose] [--seed N]
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from collections import Counter
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "_common"))
import driver as D

REPO = D.repo_root_from_here(HERE)
TRACER = Path("/tmp/louvain_kernel_check")
JS_REPLAY = HERE / "kernel_check.mjs"
LOUVAIN_DIR = REPO / "externals" / "louvain"
CONVERT = LOUVAIN_DIR / "convert"
SEED_PRELOAD = REPO / "src" / "louvain" / "seed_shim" / "louvain_seed_preload.so"
RUN_LOUVAIN = REPO / "src" / "louvain" / "run_louvain.py"
PYCOMMON = REPO / "src" / "_common"

CONDA_ENV = os.environ.get("LOUVAIN_CONDA_ENV", "nwbench")
PY = ["conda", "run", "-n", CONDA_ENV, "python"]


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


def canonical_com_csv(edge: Path, work: Path, name: str, seed: int) -> str:
    """Run canonical pipeline (run_louvain.py with LD_PRELOAD seed shim).
    Returns the com.csv text."""
    out_dir = work / f"{name}_louvain_canon"
    out_dir.mkdir(exist_ok=True)
    env = os.environ.copy()
    env["PYTHONPATH"] = str(PYCOMMON)
    rc = subprocess.run(PY + [str(RUN_LOUVAIN),
                              "--edgelist", str(edge),
                              "--output-directory", str(out_dir),
                              "--quality", "mod",
                              "--seed", str(seed),
                              "--louvain-bin-dir", str(LOUVAIN_DIR),
                              "--seed-preload", str(SEED_PRELOAD)],
                        capture_output=True, text=True, env=env)
    if rc.returncode != 0:
        raise RuntimeError(f"canonical pipeline failed: {rc.stderr}")
    return (out_dir / "com.csv").read_text()


def compose_tracer_com_csv(trace_json: Path) -> str:
    """Mirror run_louvain.py:save_results: drop singletons, renumber
    cluster_ids 0..K-1 by sorted-unique-comm-ASC, sort rows by node_id."""
    t = json.loads(trace_json.read_text())
    fine = t["fine_membership"]
    ren = t["renum_to_orig"]
    cnt = Counter(fine)
    keep = {c for c, k in cnt.items() if k > 1}
    kept = [(int(ren[i]), c) for i, c in enumerate(fine) if c in keep]
    ucs = sorted({c for _, c in kept})
    remap = {old: new for new, old in enumerate(ucs)}
    rows = sorted([(o, remap[c]) for o, c in kept])
    lines = ["node_id,cluster_id"]
    for o, c in rows:
        lines.append(f"{o},{c}")
    return "\n".join(lines) + "\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--seed", type=int, default=1)
    args = ap.parse_args()

    if not CONVERT.exists():
        sys.exit(f"missing convert: {CONVERT}")
    if not RUN_LOUVAIN.exists():
        sys.exit(f"missing run_louvain.py: {RUN_LOUVAIN}")
    if not SEED_PRELOAD.exists():
        sys.exit(f"missing seed shim: {SEED_PRELOAD} (build via {SEED_PRELOAD.parent}/build.sh)")
    D.build_tracer(HERE, TRACER)
    work = D.workdir(REPO)
    D.emit_fixture_inputs(REPO, work)

    failures = 0
    for name, edge, _com in D.fixture_cases(work):
        print(f"\n=== {name} (seed={args.seed}) ===")

        # Leg A: canonical pipeline com.csv.
        canon_csv = canonical_com_csv(edge, work, name, args.seed)

        # Leg B: tracer (multi-level capture).
        graph_bin, relabel = convert_to_bin(edge, work)
        rc = subprocess.run([str(TRACER), str(graph_bin), str(args.seed),
                             str(relabel)], capture_output=True, text=True)
        if rc.returncode != 0:
            sys.stderr.write(rc.stderr)
            failures += 1
            continue
        trace_json = work / f"{name}_louvain_tracer.json"
        trace_json.write_text(rc.stdout)
        if args.verbose:
            print(rc.stderr)

        # canon_csv vs tracer-composed com.csv.
        tracer_csv = compose_tracer_com_csv(trace_json)
        if canon_csv == tracer_csv:
            print(f"  canonical == tracer: PASS ({len(canon_csv.splitlines())} lines)")
        else:
            print(f"  canonical != tracer: FAIL")
            failures += 1

        # Leg C: JS replay.
        ok, log, err = D.run_capture(["node", str(JS_REPLAY), str(trace_json),
                                       str(edge)])
        last = (log + err).strip().splitlines()[-1] if (log + err).strip() else "(no output)"
        if ok:
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
