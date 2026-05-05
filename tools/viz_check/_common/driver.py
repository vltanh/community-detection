"""Shared driver helpers for the CD viz_check tracers.

Each per-algo `kernel_check.py` shells out to:
  1. its `instrumented/build.sh` (rebuilds the C++ tracer; no-ops if
     up-to-date relative to source).
  2. `tests/cd_verify/emit_fixture.js` (writes edge.csv + com_gt.csv;
     skipped if the outputs are newer than the emitter source).
  3. the canonical binary (constrained_clustering CC/MincutOnly/CM, or
     gen-louvain) on each fixture.
  4. the C++ tracer.
  5. the JS replay (kernel_check.mjs).

This module factors out the orchestration spine. Per-algo drivers
implement the leg-specific bits (canonical argv, tracer argv, replay
argv, output-file naming) and call `run_three_legs` to drive the loop.
"""
from __future__ import annotations

import subprocess
import sys
from pathlib import Path


def repo_root_from_here(here: Path) -> Path:
    """tools/viz_check/<algo>/kernel_check.py -> community-detection/"""
    return here.parent.parent.parent


def workdir(repo: Path) -> Path:
    """Shared CSV staging dir for fixtures + per-algo outputs."""
    d = repo / "tests" / "cd_verify"
    d.mkdir(parents=True, exist_ok=True)
    return d


def newest_mtime(*paths: Path) -> float:
    """Max mtime over given paths; 0 if none exist."""
    return max((p.stat().st_mtime for p in paths if p.exists()), default=0.0)


def build_tracer(here: Path, output_bin: Path | None = None) -> None:
    """Run `here/instrumented/build.sh`. Skip if output_bin is newer
    than every source file in instrumented/.
    """
    script = here / "instrumented" / "build.sh"
    if output_bin is not None and output_bin.exists():
        srcs = list((here / "instrumented").glob("*"))
        srcs = [p for p in srcs if p.is_file()]
        if output_bin.stat().st_mtime > newest_mtime(*srcs):
            return
    rc = subprocess.run(["bash", str(script)], capture_output=True, text=True)
    if rc.returncode != 0:
        sys.stderr.write(rc.stdout + rc.stderr)
        sys.exit(2)


def emit_fixture_inputs(repo: Path, work: Path) -> None:
    """Run tests/cd_verify/emit_fixture.js. Skip when outputs are
    newer than the emitter."""
    emitter = repo / "tests" / "cd_verify" / "emit_fixture.js"
    edge = work / "edge.csv"
    com = work / "com_gt.csv"
    if (edge.exists() and com.exists()
            and min(edge.stat().st_mtime, com.stat().st_mtime)
            > emitter.stat().st_mtime):
        return
    rc = subprocess.run(["node", str(emitter), str(work)],
                        capture_output=True, text=True)
    if rc.returncode != 0:
        sys.stderr.write(rc.stdout + rc.stderr)
        sys.exit(2)


def run_capture(argv: list[str]) -> tuple[bool, str, str]:
    """Run a subprocess; return (ok, stdout, stderr)."""
    rc = subprocess.run(argv, capture_output=True, text=True)
    return rc.returncode == 0, rc.stdout, rc.stderr


def run_three_legs(name: str, edge: Path, com: Path,
                   work: Path, *,
                   canonical_fn, tracer_fn, replay_fn,
                   verbose: bool = False) -> int:
    """Drive one (fixture, algo) cell.

    Each *_fn is a callable: returns (ok, log).
      canonical_fn(edge, com, work, name) -> (csv_text, log_text)
      tracer_fn(edge, com, work, name)    -> (csv_text, json_path, log_text)
      replay_fn(json_path, edge, com)     -> (ok_bool, log_text)

    Returns 0 on PASS for both byte-equal canon-vs-tracer and tracer-
    vs-replay; nonzero on any failure.
    """
    print(f"\n=== {name} ===")
    canon_csv, canon_log = canonical_fn(edge, com, work, name)
    tracer_csv, tracer_json, tracer_log = tracer_fn(edge, com, work, name)
    if verbose:
        print(tracer_log)
    failures = 0
    if canon_csv == tracer_csv:
        print(f"  canonical == tracer: PASS ({len(canon_csv.splitlines())} lines)")
    else:
        print("  canonical != tracer: FAIL")
        failures += 1
    ok, replay_log = replay_fn(tracer_json, edge, com)
    last = replay_log.strip().splitlines()[-1] if replay_log.strip() else "(no output)"
    if ok:
        print(f"  tracer == js_replay: PASS ({last})")
    else:
        print("  tracer != js_replay: FAIL")
        print(replay_log)
        failures += 1
    return failures


def fixture_cases(work: Path, *, dnc_present: bool = True) -> list[tuple[str, Path, Path]]:
    """Standard (name, edge_csv, com_csv) cases: fixture32 + dnc."""
    cases = [("fixture32", work / "edge.csv", work / "com_gt.csv")]
    if dnc_present and (work / "dnc_edge.csv").exists() and (work / "dnc_com.csv").exists():
        cases.append(("dnc", work / "dnc_edge.csv", work / "dnc_com.csv"))
    return cases
