"""Direct tests of the single_stage_pipeline.sh driver."""

import os
import subprocess
import textwrap
from pathlib import Path

import pytest


def make_wrapper(driver, tmp_path, cd_inputs="", cd_outputs=None, cd_params=None):
    """Build a tiny wrapper that exercises the driver with a stub CD_CMD."""
    out = tmp_path / "out"
    out.mkdir(exist_ok=True)
    if cd_outputs is None:
        cd_outputs = f"{out}/com.csv"
    cd_params_lines = ""
    if cd_params:
        joined = " ".join(f'"{p}"' for p in cd_params)
        cd_params_lines = f"CD_PARAMS=({joined})"

    algo = tmp_path / "algo.sh"
    algo.write_text(textwrap.dedent(f"""\
        #!/bin/bash
        echo "node_id,cluster_id" > "$1/com.csv"
        echo "0,1" >> "$1/com.csv"
        echo "1,1" >> "$1/com.csv"
    """))
    algo.chmod(0o755)

    wrap = tmp_path / "wrap.sh"
    wrap.write_text(textwrap.dedent(f"""\
        #!/bin/bash
        set -u
        OUTPUT_DIR="{out}"
        CD_STAGE_NAME="toy"
        CD_CMD=("{algo}" "{out}")
        CD_INPUTS="{cd_inputs or '/dev/null'}"
        CD_OUTPUTS="{cd_outputs}"
        {cd_params_lines}
        TIMEOUT=30s
        KEEP_STATE=1
        source {driver}
    """))
    wrap.chmod(0o755)
    return wrap, out


def test_driver_runs_and_marks_done(repo_root, tmp_path):
    driver = repo_root / "src" / "_common" / "single_stage_pipeline.sh"
    wrap, out = make_wrapper(driver, tmp_path)
    rc = subprocess.run(["bash", str(wrap)], capture_output=True, text=True)
    assert rc.returncode == 0, rc.stderr
    assert (out / "done").exists()
    assert (out / "com.csv").exists()
    assert (out / "params.txt").exists()
    assert (out / "run.log").exists()


def test_driver_cache_hit_on_rerun(repo_root, tmp_path):
    driver = repo_root / "src" / "_common" / "single_stage_pipeline.sh"
    wrap, out = make_wrapper(driver, tmp_path)
    subprocess.run(["bash", str(wrap)], capture_output=True, text=True).check_returncode()
    rc2 = subprocess.run(["bash", str(wrap)], capture_output=True, text=True)
    assert rc2.returncode == 0
    assert "cache hit" in rc2.stdout


def test_driver_recomputes_on_output_tamper(repo_root, tmp_path):
    driver = repo_root / "src" / "_common" / "single_stage_pipeline.sh"
    wrap, out = make_wrapper(driver, tmp_path)
    subprocess.run(["bash", str(wrap)], capture_output=True, text=True).check_returncode()
    (out / "com.csv").write_text("tampered\n")
    rc = subprocess.run(["bash", str(wrap)], capture_output=True, text=True)
    assert rc.returncode == 0
    assert "Running toy" in rc.stdout
    assert "node_id" in (out / "com.csv").read_text()


def test_driver_recomputes_on_params_tamper(repo_root, tmp_path):
    driver = repo_root / "src" / "_common" / "single_stage_pipeline.sh"
    wrap, out = make_wrapper(driver, tmp_path, cd_params=["seed=1"])
    subprocess.run(["bash", str(wrap)], capture_output=True, text=True).check_returncode()
    # Re-create wrapper with different params.
    wrap2, _ = make_wrapper(driver, tmp_path, cd_params=["seed=2"])
    # Same output dir; params change should bust cache.
    rc = subprocess.run(["bash", str(wrap2)], capture_output=True, text=True)
    assert rc.returncode == 0
    assert "Running toy" in rc.stdout, "params tamper should re-run"


def test_driver_rejects_missing_required_vars(repo_root, tmp_path):
    driver = repo_root / "src" / "_common" / "single_stage_pipeline.sh"
    wrap = tmp_path / "wrap.sh"
    wrap.write_text(f"#!/bin/bash\nsource {driver}\n")
    wrap.chmod(0o755)
    rc = subprocess.run(["bash", str(wrap)], capture_output=True, text=True)
    assert rc.returncode == 2
