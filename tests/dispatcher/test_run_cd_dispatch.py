"""Dispatcher routing tests: arg parsing, gate matrix, *-best cascade.

Stub the per-algo wrappers so the dispatcher's routing can be tested without
running real algorithms. Slow tests reuse the real wrappers + the dnc
edgelist fixture; non-slow tests use stubs.
"""

import os
import subprocess
import textwrap
from pathlib import Path

import pytest


def run_dispatcher(repo_root, args, *, env=None, cwd=None):
    e = os.environ.copy()
    if env:
        e.update(env)
    return subprocess.run(
        ["bash", str(repo_root / "run_cd.sh"), *args],
        capture_output=True,
        text=True,
        env=e,
        cwd=cwd,
    )


def test_dispatcher_requires_algo(repo_root):
    rc = run_dispatcher(repo_root, [])
    assert rc.returncode == 1
    assert "--algo is a required parameter" in rc.stdout or "--algo is a required parameter" in rc.stderr


def test_dispatcher_rejects_unknown_flag(repo_root):
    rc = run_dispatcher(repo_root, ["--algo", "leiden-mod", "--bogus-flag"])
    assert rc.returncode == 1
    assert "Unknown parameter" in rc.stdout


def test_dispatcher_requires_input_or_mode(repo_root):
    rc = run_dispatcher(repo_root, ["--algo", "leiden-mod"])
    assert rc.returncode == 1


def test_dispatcher_rejects_synthetic_without_required_args(repo_root):
    rc = run_dispatcher(repo_root, ["--algo", "leiden-mod", "--synthetic"])
    assert rc.returncode == 1
    assert "--network, --generator, and --gt-clustering-id required" in rc.stdout


@pytest.mark.parametrize(
    "algo,gates_in,expect_warn",
    [
        ("ikc-3", ["--run-cc"], "CC is not necessary for IKC"),
        ("ikc-3", ["--run-wcc"], "WCC, and CM are not supported for IKC"),
        ("leiden-mod", ["--run-wcc"], "WCC is not supported for Leiden"),
        ("sbm-flat-dc", ["--run-cm"], "CM is not supported for SBM"),
        ("infomap", ["--run-cm"], "WCC and CM are not supported for Infomap"),
    ],
)
def test_dispatcher_gate_matrix_warns(repo_root, tmp_path, dnc_edgelist, algo, gates_in, expect_warn):
    """Per-algo gate matrix logs the expected warning when a forbidden post-proc is requested."""
    if not dnc_edgelist.exists():
        pytest.skip(f"dnc fixture not present at {dnc_edgelist}")
    rc = run_dispatcher(
        repo_root,
        [
            "--algo", algo,
            "--input-edgelist", str(dnc_edgelist),
            "--output-dir", str(tmp_path),
            "--network", "dnc",
            *gates_in,
        ],
        env={"PATH": "/home/vltanh/miniconda3/envs/nwbench/bin:" + os.environ.get("PATH", "")},
    )
    # Don't assert rc==0; nwbench might not be present in the test env. Just
    # check the gate matrix output appeared in stdout.
    assert expect_warn in rc.stdout, (
        f"expected '{expect_warn}' in dispatcher output:\n{rc.stdout}\nstderr:\n{rc.stderr}"
    )
