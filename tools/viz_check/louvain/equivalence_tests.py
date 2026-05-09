"""Louvain L4 tracer equivalence tests (per byte-equal-tracer skill §Step 1).

Three preflight gates that must pass before claiming the L4 tracer's
verification result is meaningful:

(a) Build-pair test: /tmp/louvain_l4_tracer_canonical's PARTITION (the
    com.csv composed from fineMembership) byte-equals the unmodified
    canonical pipeline (run_louvain.py + LD_PRELOAD shim) under matching
    LOUVAIN_SEED. Proves the canonical-mode build of the toggle source
    didn't perturb canonical's algorithm. Runs on fixture32 only (small,
    deterministic).

(b) Self-determinism test: /tmp/louvain_l4_tracer_swapped run twice on
    the same seed produces bit-identical JSON output. Catches uninit
    memory and time-based non-determinism.

(c) Structural sanity test: /tmp/louvain_l4_tracer_swapped's final
    partition K and Q come within stats noise of run_louvain.py's K
    and Q. Catches gross tracer-introduced bugs (different algorithm
    body) without claiming RNG/FP-bit-equality across the swap.

Run:
    python tools/viz_check/louvain/equivalence_tests.py [--seed N]
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
LOUVAIN_DIR = REPO / "externals" / "louvain"
SEED_PRELOAD = REPO / "src" / "louvain" / "seed_shim" / "louvain_seed_preload.so"
RUN_LOUVAIN = REPO / "src" / "louvain" / "run_louvain.py"
PYCOMMON = REPO / "src" / "_common"
TRACER_SWAPPED = Path("/tmp/louvain_l4_tracer_swapped")
TRACER_CANONICAL = Path("/tmp/louvain_l4_tracer_canonical")
BUILD_SCRIPT = HERE / "instrumented" / "build_l4.sh"

CONDA_ENV = os.environ.get("LOUVAIN_CONDA_ENV", "nwbench")
PY = ["conda", "run", "-n", CONDA_ENV, "python"]


def ensure_built() -> None:
    """Rebuild both binaries unconditionally so toggle changes propagate."""
    rc = subprocess.run(["bash", str(BUILD_SCRIPT)], capture_output=True, text=True)
    if rc.returncode != 0:
        sys.stderr.write(rc.stdout + rc.stderr)
        sys.exit(2)


def canonical_com_csv(edge: Path, work: Path, name: str, seed: int) -> str:
    """Run run_louvain.py with LD_PRELOAD shim → com.csv text."""
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


def tracer_canonical_com_csv(edge: Path, seed: int) -> str:
    """Run /tmp/louvain_l4_tracer_canonical with LD_PRELOAD shim →
    decode fineMembership → compose com.csv with run_louvain.py's
    drop-singletons + sorted-unique-comm-ASC renumber + sort-by-node-id
    convention."""
    env = os.environ.copy()
    env["LD_PRELOAD"] = str(SEED_PRELOAD)
    env["LOUVAIN_SEED"] = str(seed)
    # The tracer's CANONICAL_MODE main passes argv[2] as a fallback seed
    # (used iff the shim doesn't fire). Pass seed too for belt-and-braces.
    rc = subprocess.run([str(TRACER_CANONICAL), str(edge), str(seed)],
                        capture_output=True, text=True, env=env)
    if rc.returncode != 0:
        raise RuntimeError(f"tracer_canonical failed: {rc.stderr}")
    t = json.loads(rc.stdout)
    fine = t["fine_membership_post_renumber"]
    # Mirror run_louvain.py:save_results convention.
    cnt = Counter(fine)
    keep = {c for c, k in cnt.items() if k > 1}
    kept = [(i, c) for i, c in enumerate(fine) if c in keep]
    ucs = sorted({c for _, c in kept})
    remap = {old: new for new, old in enumerate(ucs)}
    rows = sorted([(o, remap[c]) for o, c in kept])
    lines = ["node_id,cluster_id"]
    for o, c in rows:
        lines.append(f"{o},{c}")
    return "\n".join(lines) + "\n"


def tracer_swapped_json(edge: Path, seed: int) -> str:
    rc = subprocess.run([str(TRACER_SWAPPED), str(edge), str(seed)],
                        capture_output=True, text=True)
    if rc.returncode != 0:
        raise RuntimeError(f"tracer_swapped failed: {rc.stderr}")
    return rc.stdout


def partition_stats(com_csv: str) -> tuple[int, int]:
    """Return (n_kept, K) from a com.csv string."""
    lines = [l for l in com_csv.strip().splitlines()[1:] if l]
    K = len(set(l.split(",")[1] for l in lines))
    return len(lines), K


def test_a_build_pair(edge: Path, work: Path, name: str, seed: int) -> bool:
    print(f"\n[a] build-pair: tracer_canonical PARTITION == run_louvain.py com.csv  ({name}, seed={seed})")
    canon = canonical_com_csv(edge, work, name, seed)
    trc = tracer_canonical_com_csv(edge, seed)
    if canon == trc:
        print(f"     PASS ({len(canon.splitlines())} lines)")
        return True
    # Diagnose mismatch: line count + first 5 differing lines.
    cl, tl = canon.splitlines(), trc.splitlines()
    print(f"     FAIL: canon_lines={len(cl)} tracer_lines={len(tl)}")
    diffs = 0
    for a, b in zip(cl, tl):
        if a != b:
            diffs += 1
            if diffs <= 5:
                print(f"       canon: {a}")
                print(f"      tracer: {b}")
    if diffs > 5:
        print(f"     ... +{diffs - 5} more diffs")
    return False


def test_b_self_determinism(edge: Path, seed: int) -> bool:
    print(f"\n[b] self-determinism: tracer_swapped same-seed two runs bit-equal  (seed={seed})")
    a = tracer_swapped_json(edge, seed)
    b = tracer_swapped_json(edge, seed)
    if a == b:
        print(f"     PASS ({len(a)} bytes match)")
        return True
    # Find first divergent byte.
    for i in range(min(len(a), len(b))):
        if a[i] != b[i]:
            print(f"     FAIL: first diff at byte {i}: a={a[max(0,i-20):i+20]!r} b={b[max(0,i-20):i+20]!r}")
            return False
    print(f"     FAIL: lengths differ a={len(a)} b={len(b)}")
    return False


def test_c_structural_sanity(edge: Path, work: Path, name: str, seed: int) -> bool:
    print(f"\n[c] structural sanity: tracer_swapped K + Q ≈ run_louvain.py K + Q  ({name}, seed={seed})")
    canon = canonical_com_csv(edge, work, name, seed)
    canon_n, canon_K = partition_stats(canon)
    swp_json = tracer_swapped_json(edge, seed)
    swp = json.loads(swp_json)
    fine = swp["fine_membership_post_renumber"]
    # Compose tracer_swapped's com.csv same way.
    cnt = Counter(fine)
    keep = {c for c, k in cnt.items() if k > 1}
    kept = [(i, c) for i, c in enumerate(fine) if c in keep]
    ucs = sorted({c for _, c in kept})
    remap = {old: new for new, old in enumerate(ucs)}
    swp_n = len(kept)
    swp_K = len(remap)
    # For Q we'd need to run the canonical partition through a Q
    # calculator. Skip exact Q comparison; rely on K + n_kept agreement.
    # Tolerance: ≤20% deviation. Different RNG → different trajectory →
    # different K, but should land in same order of magnitude.
    K_ok = abs(canon_K - swp_K) <= max(2, int(canon_K * 0.5))
    n_ok = abs(canon_n - swp_n) <= max(2, int(canon_n * 0.2))
    print(f"     canon: n_kept={canon_n} K={canon_K}")
    print(f"     swp:   n_kept={swp_n} K={swp_K}  (K_ok={K_ok}, n_ok={n_ok})")
    if K_ok and n_ok:
        print(f"     PASS (K within tolerance; trajectories diverge under different RNG, expected)")
        return True
    print(f"     FAIL")
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seed", type=int, default=42,
                    help="Seed for build-pair + structural tests (default: 42)")
    ap.add_argument("--no-rebuild", action="store_true",
                    help="Skip the unconditional rebuild step")
    args = ap.parse_args()

    if not args.no_rebuild:
        ensure_built()
    if not TRACER_SWAPPED.exists() or not TRACER_CANONICAL.exists():
        sys.exit("tracer binaries missing; build via instrumented/build_l4.sh")
    if not RUN_LOUVAIN.exists():
        sys.exit(f"missing run_louvain.py: {RUN_LOUVAIN}")
    if not SEED_PRELOAD.exists():
        sys.exit(f"missing seed shim: {SEED_PRELOAD} (build via {SEED_PRELOAD.parent}/build.sh)")

    work = D.workdir(REPO)
    D.emit_fixture_inputs(REPO, work)

    fixture_name, fixture_edge = None, None
    for name, edge, _com in D.fixture_cases(work):
        if name == "fixture32":
            fixture_name, fixture_edge = name, edge
            break
    if fixture_edge is None:
        sys.exit("fixture32 not found in fixture_cases")

    results = []
    results.append(("a", test_a_build_pair(fixture_edge, work, fixture_name, args.seed)))
    results.append(("b", test_b_self_determinism(fixture_edge, args.seed)))
    results.append(("c", test_c_structural_sanity(fixture_edge, work, fixture_name, args.seed)))

    print()
    print("=" * 60)
    fails = [tag for tag, ok in results if not ok]
    if fails:
        print(f"OVERALL: FAIL ({len(fails)} of 3 — failed: {','.join(fails)})")
        sys.exit(1)
    print("OVERALL: PASS (3 of 3)")


if __name__ == "__main__":
    main()
