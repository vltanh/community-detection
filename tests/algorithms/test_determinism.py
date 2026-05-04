"""Determinism tests: byte-identity across PYTHONHASHSEED + reruns."""

import os
import subprocess
from pathlib import Path

import pytest

pytestmark = pytest.mark.slow

NWBENCH_BIN = "/home/vltanh/miniconda3/envs/nwbench/bin"


def _run_algo(repo_root, algo, fixture, output, hash_seed="0"):
    e = os.environ.copy()
    e["PATH"] = f"{NWBENCH_BIN}:{e.get('PATH', '')}"
    e["PYTHONHASHSEED"] = hash_seed
    e["OMP_NUM_THREADS"] = "1"
    return subprocess.run(
        [
            "bash", str(repo_root / "run_cd.sh"),
            "--algo", algo,
            "--input-edgelist", str(fixture / "edge.csv"),
            "--output-dir", str(output),
            "--network", "cd_fixture",
            "--seed", "1",
        ],
        capture_output=True, text=True, env=e,
    )


@pytest.mark.parametrize("algo", ["leiden-mod", "infomap", "ikc-3", "sbm-flat-dc"])
def test_byte_identity_across_pythonhashseed(repo_root, tmp_path, cd_fixture_dir, algo):
    """Run twice with PYTHONHASHSEED=0 vs PYTHONHASHSEED=7; output must be byte-identical."""
    out0 = tmp_path / "h0"
    out7 = tmp_path / "h7"
    rc0 = _run_algo(repo_root, algo, cd_fixture_dir, out0, hash_seed="0")
    rc7 = _run_algo(repo_root, algo, cd_fixture_dir, out7, hash_seed="7")
    assert rc0.returncode == 0 and rc7.returncode == 0
    com0 = (out0 / "clusterings" / algo / "cd_fixture" / "com.csv").read_bytes()
    com7 = (out7 / "clusterings" / algo / "cd_fixture" / "com.csv").read_bytes()
    assert com0 == com7, f"{algo}: PYTHONHASHSEED-dependent output (drift between hash 0 and 7)"


@pytest.mark.parametrize("algo", ["leiden-mod", "ikc-3", "sbm-flat-dc"])
def test_byte_identity_across_two_runs_same_seed(repo_root, tmp_path, cd_fixture_dir, algo):
    out1 = tmp_path / "r1"
    out2 = tmp_path / "r2"
    rc1 = _run_algo(repo_root, algo, cd_fixture_dir, out1)
    rc2 = _run_algo(repo_root, algo, cd_fixture_dir, out2)
    assert rc1.returncode == 0 and rc2.returncode == 0
    com1 = (out1 / "clusterings" / algo / "cd_fixture" / "com.csv").read_bytes()
    com2 = (out2 / "clusterings" / algo / "cd_fixture" / "com.csv").read_bytes()
    assert com1 == com2, f"{algo}: reruns differ"
