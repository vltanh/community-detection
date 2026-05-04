"""--keep-state plumbing through every wrapper + the driver.

Verifies: --keep-state preserves time_and_err.log + params.txt across runs;
absence of --keep-state cleans them up. Run as slow because it invokes the
real algos via the wrappers.
"""

import os
import subprocess
from pathlib import Path

import pytest

pytestmark = pytest.mark.slow

NWBENCH_BIN = "/home/vltanh/miniconda3/envs/nwbench/bin"


def _env():
    e = os.environ.copy()
    e["PATH"] = f"{NWBENCH_BIN}:{e.get('PATH', '')}"
    e["PYTHONHASHSEED"] = "0"
    e["OMP_NUM_THREADS"] = "1"
    return e


def _run_leiden(repo_root, output_dir, keep_state, dnc_edgelist):
    args = [
        "bash", str(repo_root / "src" / "leiden" / "pipeline.sh"),
        "--input-edgelist", str(dnc_edgelist),
        "--output-dir", str(output_dir),
        "--model", "mod",
    ]
    if keep_state:
        args.append("--keep-state")
    return subprocess.run(args, capture_output=True, text=True, env=_env())


def test_keep_state_preserves_intermediates(repo_root, tmp_path, dnc_edgelist):
    if not dnc_edgelist.exists():
        pytest.skip(f"dnc fixture not present")
    out = tmp_path / "leiden-mod"
    rc = _run_leiden(repo_root, out, keep_state=True, dnc_edgelist=dnc_edgelist)
    assert rc.returncode == 0, rc.stderr
    assert (out / "com.csv").exists()
    assert (out / "params.txt").exists()
    assert (out / "time_and_err.log").exists()
    assert (out / "pipeline.log").exists()


def test_no_keep_state_cleans_intermediates(repo_root, tmp_path, dnc_edgelist):
    if not dnc_edgelist.exists():
        pytest.skip(f"dnc fixture not present")
    out = tmp_path / "leiden-mod"
    rc = _run_leiden(repo_root, out, keep_state=False, dnc_edgelist=dnc_edgelist)
    assert rc.returncode == 0, rc.stderr
    assert (out / "com.csv").exists()
    assert not (out / "params.txt").exists(), "params.txt should be removed without --keep-state"
    assert not (out / "time_and_err.log").exists(), "time_and_err.log should be removed without --keep-state"


def test_keep_state_3run_roundtrip(repo_root, tmp_path, dnc_edgelist):
    """Three runs with --keep-state: first cache miss, next two cache hit, output stable."""
    if not dnc_edgelist.exists():
        pytest.skip(f"dnc fixture not present")
    out = tmp_path / "leiden-mod"

    rc1 = _run_leiden(repo_root, out, keep_state=True, dnc_edgelist=dnc_edgelist)
    assert rc1.returncode == 0
    com1 = (out / "com.csv").read_bytes()

    rc2 = _run_leiden(repo_root, out, keep_state=True, dnc_edgelist=dnc_edgelist)
    assert rc2.returncode == 0
    assert "cache hit" in rc2.stdout, "second run should be cache hit"
    com2 = (out / "com.csv").read_bytes()

    rc3 = _run_leiden(repo_root, out, keep_state=True, dnc_edgelist=dnc_edgelist)
    assert rc3.returncode == 0
    assert "cache hit" in rc3.stdout, "third run should be cache hit"
    com3 = (out / "com.csv").read_bytes()

    assert com1 == com2 == com3, "com.csv must be byte-stable across 3 runs"


def test_output_tamper_invalidates_cache(repo_root, tmp_path, dnc_edgelist):
    if not dnc_edgelist.exists():
        pytest.skip(f"dnc fixture not present")
    out = tmp_path / "leiden-mod"
    rc1 = _run_leiden(repo_root, out, keep_state=True, dnc_edgelist=dnc_edgelist)
    assert rc1.returncode == 0
    # Tamper.
    (out / "com.csv").write_text("tampered\n")
    rc2 = _run_leiden(repo_root, out, keep_state=True, dnc_edgelist=dnc_edgelist)
    assert rc2.returncode == 0
    assert "cache hit" not in rc2.stdout, "tampered output should trigger recompute"
    # Output restored.
    com_after = (out / "com.csv").read_text()
    assert "node_id,cluster_id" in com_after.split("\n")[0]


def test_params_tamper_invalidates_cache(repo_root, tmp_path, dnc_edgelist):
    """Re-invocation with different --seed (changes CD_PARAMS) must invalidate."""
    if not dnc_edgelist.exists():
        pytest.skip(f"dnc fixture not present")
    out = tmp_path / "leiden-mod"

    def run(seed):
        return subprocess.run(
            [
                "bash", str(repo_root / "src" / "leiden" / "pipeline.sh"),
                "--input-edgelist", str(dnc_edgelist),
                "--output-dir", str(out),
                "--model", "mod",
                "--seed", str(seed),
                "--keep-state",
            ],
            capture_output=True, text=True, env=_env(),
        )

    rc1 = run(1)
    assert rc1.returncode == 0

    rc2 = run(1)
    assert rc2.returncode == 0
    assert "cache hit" in rc2.stdout

    rc3 = run(42)
    assert rc3.returncode == 0
    assert "cache hit" not in rc3.stdout, "seed change must invalidate"
