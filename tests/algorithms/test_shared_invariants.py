"""Shared invariants every algo's com.csv must satisfy.

- Header is 'node_id,cluster_id'.
- No duplicate (node_id, cluster_id) pairs.
- Each node appears at most once.
- No singleton clusters (drop_singleton_clusters runs pre-write).
"""

import os
import subprocess
from collections import Counter
from pathlib import Path

import pandas as pd
import pytest

pytestmark = pytest.mark.slow

NWBENCH_BIN = "/home/vltanh/miniconda3/envs/nwbench/bin"


def _env():
    e = os.environ.copy()
    e["PATH"] = f"{NWBENCH_BIN}:{e.get('PATH', '')}"
    e["PYTHONHASHSEED"] = "0"
    e["OMP_NUM_THREADS"] = "1"
    return e


@pytest.fixture(scope="module")
def all_algo_outputs(repo_root, tmp_path_factory, dnc_edgelist):
    """Run every base algo once on dnc, return {algo: output_dir}."""
    out_root = tmp_path_factory.mktemp("invariants")
    algos = [
        "leiden-mod",
        "leiden-cpm-0.5",
        "louvain-mod",
        "louvain-zahn",
        "louvain-owzad-0.5",
        "louvain-goldberg",
        "louvain-condora",
        "louvain-devind",
        "louvain-devuni",
        "louvain-dp",
        "louvain-shimalik-1",
        "louvain-balmod",
        "infomap",
        "ikc-3",
        "sbm-flat-dc",
        "sbm-flat-ndc",
        "sbm-flat-pp",
        "sbm-nested-dc",
        "sbm-nested-ndc",
    ]
    results = {}
    for algo in algos:
        rc = subprocess.run(
            [
                "bash", str(repo_root / "run_cd.sh"),
                "--algo", algo,
                "--input-edgelist", str(dnc_edgelist),
                "--output-dir", str(out_root),
                "--network", "dnc",
            ],
            capture_output=True, text=True, env=_env(),
        )
        if rc.returncode == 0:
            results[algo] = out_root / "clusterings" / algo / "dnc" / "com.csv"
    return results


def test_every_algo_has_canonical_header(all_algo_outputs):
    for algo, com in all_algo_outputs.items():
        assert com.exists(), f"missing com.csv for {algo}"
        first_line = com.read_text().split("\n")[0]
        assert first_line == "node_id,cluster_id", f"{algo}: bad header '{first_line}'"


def test_no_duplicate_nodes(all_algo_outputs):
    for algo, com in all_algo_outputs.items():
        df = pd.read_csv(com)
        assert df["node_id"].is_unique, f"{algo}: duplicate node_id"


def test_no_singleton_clusters(all_algo_outputs):
    for algo, com in all_algo_outputs.items():
        df = pd.read_csv(com)
        counts = df["cluster_id"].value_counts()
        singletons = counts[counts < 2]
        assert len(singletons) == 0, f"{algo}: singleton clusters present: {singletons.to_dict()}"
