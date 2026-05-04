"""state.sh + pipeline_common.py must stay in sync with NG (drift detector pass)."""

import shutil
import subprocess
from pathlib import Path

import pytest


@pytest.fixture(scope="session")
def umbrella_root(repo_root):
    return repo_root.parent


@pytest.fixture(scope="session")
def detector(umbrella_root):
    return umbrella_root / "tools" / "check_common_sync.sh"


def test_detector_exists(detector):
    assert detector.exists(), f"drift detector missing at {detector}"


def test_drift_detector_clean(detector):
    """Detector must report all shared _common/ files in sync."""
    result = subprocess.run(["bash", str(detector)], capture_output=True, text=True)
    assert result.returncode == 0, (
        f"drift detector failed (rc={result.returncode}):\n"
        f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
    )
    assert "All shared _common/ files are in sync" in result.stdout
