from pathlib import Path

import pytest


REPO_ROOT = Path(__file__).resolve().parent.parent


def pytest_configure(config):
    config.addinivalue_line(
        "markers",
        "slow: end-to-end pipeline runs; opt in with `pytest -m slow`.",
    )


def pytest_collection_modifyitems(config, items):
    marker_expr = config.getoption("-m")
    if marker_expr and "slow" in marker_expr:
        return
    skip_slow = pytest.mark.skip(reason="needs `-m slow` to run")
    for item in items:
        if "slow" in item.keywords:
            item.add_marker(skip_slow)


@pytest.fixture(scope="session")
def repo_root():
    return REPO_ROOT


@pytest.fixture(scope="session")
def shared_dir(repo_root):
    return repo_root / "src" / "_common"


@pytest.fixture(scope="session")
def state_sh(shared_dir):
    return shared_dir / "state.sh"


@pytest.fixture(scope="session")
def dnc_edgelist(repo_root):
    """Path to the dnc.csv copy under the in-repo examples tree."""
    return (
        repo_root
        / "examples"
        / "input"
        / "empirical_networks"
        / "networks"
        / "dnc"
        / "dnc.csv"
    )
