"""IKC kernel cross-check driver.

Three legs:
  - canonical: src/ikc/run_ikc.py (Python; networkit + pandas).
  - tracer: tools/viz_check/ikc/instrumented/kernel_check.py (Python
    fork of the canonical with [TRACE-IKC] log lines + JSON emit).
  - JS replay: kernel_check.mjs (loads comdet/js/ikc/ikc.js via Node).

IKC is deterministic (no RNG), so JS replay matches canonical via
per-iteration max_k + component-size + kept-component-size tuples
plus member-set bijection at the partition level. CSV byte-equal vs
canonical is achieved by emitting accepted clusters in tracer
(canonical) order.

Run:
    python tools/viz_check/ikc/kernel_check.py [--verbose]
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
CANON = REPO / "src" / "ikc" / "run_ikc.py"
TRACER = HERE / "instrumented" / "kernel_check.py"
JS_REPLAY = HERE / "kernel_check.mjs"
PYCOMMON = REPO / "src" / "_common"

# Default k_floor per fixture. dnc uses 3 (matches examples/regen.sh
# ikc-3 row); fixture32 uses 2 (smaller graph; demo-friendly).
KFLOOR = {"fixture32": 2, "dnc": 3}

# Conda env name (canonical IKC needs networkit).
CONDA_ENV = os.environ.get("IKC_CONDA_ENV", "nwbench")
PY = ["conda", "run", "-n", CONDA_ENV, "python"]


def normalize_edge_for_canon(edge: Path, work: Path, name: str) -> Path:
    """Canonical run_ikc.py requires `source,target` headers. fixture32
    edge.csv uses `node_id1,node_id2`; rewrite to a sibling tmp file
    when needed. dnc_edge.csv already uses source,target."""
    head = edge.read_text().split("\n", 1)[0].strip()
    if head == "source,target":
        return edge
    out = work / f"{name}_st_edge.csv"
    body = edge.read_text().split("\n", 1)[1]
    out.write_text("source,target\n" + body)
    return out


def canonical_fn(edge: Path, com: Path, work: Path, name: str):
    edge_st = normalize_edge_for_canon(edge, work, name)
    out_dir = work / f"{name}_ikc_canon"
    out_dir.mkdir(exist_ok=True)
    env = os.environ.copy()
    env["PYTHONPATH"] = str(PYCOMMON)
    rc = subprocess.run(PY + [str(CANON), "-e", str(edge_st),
                              "-o", str(out_dir),
                              "-k", str(KFLOOR[name]), "-q"],
                        capture_output=True, text=True, env=env)
    if rc.returncode != 0:
        raise RuntimeError(f"canonical failed: {rc.stderr}")
    return (out_dir / "com.csv").read_text(), rc.stdout + rc.stderr


def tracer_fn(edge: Path, com: Path, work: Path, name: str):
    out = work / f"{name}_ikc_tracer.csv"
    json_path = work / f"{name}_ikc_tracer.json"
    log_path = work / f"{name}_ikc_tracer.log"
    rc = subprocess.run(PY + [str(TRACER), str(edge),
                              str(KFLOOR[name]), str(out)],
                        capture_output=True, text=True)
    if rc.returncode != 0:
        log_path.write_text(rc.stderr)
        raise RuntimeError(f"tracer failed: {rc.stderr}")
    json_path.write_text(rc.stdout)
    log_path.write_text(rc.stderr)
    return out.read_text(), json_path, rc.stderr


def replay_fn(json_path: Path, edge: Path, com: Path):
    out = edge.parent / (json_path.stem.replace("_ikc_tracer", "_ikc_js") + ".csv")
    rc = subprocess.run(["node", str(JS_REPLAY), str(json_path),
                         str(edge), str(out)],
                        capture_output=True, text=True)
    return rc.returncode == 0, rc.stdout + rc.stderr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    if not CANON.exists():
        sys.exit(f"missing canonical: {CANON}")
    work = D.workdir(REPO)
    D.emit_fixture_inputs(REPO, work)

    failures = 0
    for name, edge, com in D.fixture_cases(work):
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
