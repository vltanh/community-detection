"""End-to-end smoke: every base algo + every applicable post-proc on cd_fixture.

Each algo must produce a non-empty com.csv with the canonical
'node_id,cluster_id' header.
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


def _dispatch(repo_root, algo, fixture, output, gates=()):
    args = [
        "bash", str(repo_root / "run_cd.sh"),
        "--algo", algo,
        "--input-edgelist", str(fixture / "edge.csv"),
        "--output-dir", str(output),
        "--network", "cd_fixture",
        *gates,
    ]
    return subprocess.run(args, capture_output=True, text=True, env=_env())


@pytest.mark.parametrize(
    "algo",
    [
        "leiden-mod",
        "leiden-cpm-0.5",
        "infomap",
        "ikc-3",
        "sbm-flat-dc",
        "sbm-flat-ndc",
        "sbm-flat-pp",
        "sbm-nested-dc",
        "sbm-nested-ndc",
    ],
)
def test_base_algo_smoke(repo_root, tmp_path, cd_fixture_dir, algo):
    rc = _dispatch(repo_root, algo, cd_fixture_dir, tmp_path)
    assert rc.returncode == 0, f"{algo} failed: {rc.stderr}"
    com = tmp_path / "clusterings" / algo / "cd_fixture" / "com.csv"
    assert com.exists(), f"missing com.csv for {algo}"
    text = com.read_text()
    assert text.startswith("node_id,cluster_id"), f"bad header in {algo}/com.csv"
    lines = text.strip().split("\n")
    assert len(lines) >= 2, f"{algo} produced empty clustering"


def test_sbm_flat_best_picks_winner(repo_root, tmp_path, cd_fixture_dir):
    rc = _dispatch(repo_root, "sbm-flat-best", cd_fixture_dir, tmp_path)
    assert rc.returncode == 0, rc.stderr
    base = tmp_path / "clusterings" / "sbm-flat-best" / "cd_fixture"
    assert (base / "com.csv").is_symlink() or (base / "com.csv").exists()
    best_model = (base / "best_model.txt").read_text().strip()
    assert best_model in ("flat-dc", "flat-ndc", "flat-pp"), f"unexpected best: {best_model}"


def test_sbm_nested_best_picks_winner(repo_root, tmp_path, cd_fixture_dir):
    rc = _dispatch(repo_root, "sbm-nested-best", cd_fixture_dir, tmp_path)
    assert rc.returncode == 0, rc.stderr
    base = tmp_path / "clusterings" / "sbm-nested-best" / "cd_fixture"
    best_model = (base / "best_model.txt").read_text().strip()
    assert best_model in ("nested-dc", "nested-ndc"), f"unexpected best: {best_model}"


@pytest.mark.parametrize(
    "algo,gates,suffix",
    [
        ("leiden-mod", ["--run-cm"], "+cm"),
        ("sbm-flat-dc", ["--run-cc"], "+cc"),
        ("sbm-flat-dc", ["--run-wcc"], "+wcc"),
    ],
)
def test_postproc_smoke(repo_root, tmp_path, cd_fixture_dir, algo, gates, suffix):
    binary = repo_root / "constrained-clustering" / "constrained_clustering"
    if not binary.exists():
        pytest.skip(f"constrained_clustering binary not built (Phase 0e #20)")
    rc = _dispatch(repo_root, algo, cd_fixture_dir, tmp_path, gates=gates)
    assert rc.returncode == 0, rc.stderr
    pp_com = tmp_path / "clusterings" / f"{algo}{suffix}" / "cd_fixture" / "com.csv"
    assert pp_com.exists(), f"missing post-proc com.csv at {pp_com}"
    assert pp_com.read_text().startswith("node_id,cluster_id")
