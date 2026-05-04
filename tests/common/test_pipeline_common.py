"""Unit tests for src/_common/pipeline_common.py (CD subset)."""

import logging
import sys
from pathlib import Path

import pandas as pd
import pytest


@pytest.fixture(scope="session")
def pipeline_common(shared_dir):
    sys.path.insert(0, str(shared_dir))
    import pipeline_common  # noqa: E402
    return pipeline_common


def test_setup_logging_writes_to_file(pipeline_common, tmp_path):
    log = tmp_path / "run.log"
    pipeline_common.setup_logging(log)
    logging.info("hello")
    assert "hello" in log.read_text()


def test_setup_logging_replaces_handlers(pipeline_common, tmp_path):
    """Re-calling setup_logging clears prior handlers (avoid double-write)."""
    log1 = tmp_path / "first.log"
    log2 = tmp_path / "second.log"
    pipeline_common.setup_logging(log1)
    logging.info("a")
    pipeline_common.setup_logging(log2)
    logging.info("b")
    assert "a" in log1.read_text()
    assert "b" not in log1.read_text()
    assert "b" in log2.read_text()


def test_standard_setup_creates_dir_and_returns_path(pipeline_common, tmp_path):
    out = pipeline_common.standard_setup(tmp_path / "a" / "b")
    assert out.exists()
    assert (out / "run.log").exists()
    assert isinstance(out, Path)


def test_timed_logs_elapsed(pipeline_common, tmp_path):
    log = tmp_path / "run.log"
    pipeline_common.setup_logging(log)
    with pipeline_common.timed("phase_x"):
        pass
    text = log.read_text()
    assert "phase_x elapsed:" in text


def test_drop_singleton_clusters_drops_size_1(pipeline_common):
    df = pd.DataFrame(
        {"node_id": [1, 2, 3, 4, 5, 6], "cluster_id": [10, 10, 20, 30, 30, 40]}
    )
    kept = pipeline_common.drop_singleton_clusters(df)
    assert sorted(kept["cluster_id"].unique().tolist()) == [10, 30]


def test_drop_singleton_clusters_keeps_size_2_plus(pipeline_common):
    df = pd.DataFrame({"node_id": [1, 2, 3, 4, 5], "cluster_id": [1, 1, 2, 2, 2]})
    kept = pipeline_common.drop_singleton_clusters(df)
    assert sorted(kept["cluster_id"].unique().tolist()) == [1, 2]
    assert len(kept) == 5
