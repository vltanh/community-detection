# Source: network-generation/src/pipeline_common.py. Keep in sync via tools/check_common_sync.sh.
# Subset for community-detection: setup_logging, timed, standard_setup,
# drop_singleton_clusters. NG-only helpers (simplify_edges, write_edge_tuples_csv,
# load_probs_matrix) skipped because CD does not write edge.csv.

import logging
import time
from contextlib import contextmanager
from pathlib import Path

import pandas as pd


def setup_logging(log_filepath: Path, level=logging.INFO, extra_handlers=None):
    """Route root logger to `log_filepath` with timestamps; no console output.

    `level` and `extra_handlers` are reserved for future callers (per plan
    decision 12: future-proof API surface, additive-only growth). Today's CD
    pipeline.sh wrappers pass neither; they default to INFO + file-only.
    """
    log_filepath = Path(log_filepath)
    log_filepath.parent.mkdir(parents=True, exist_ok=True)

    logger = logging.getLogger()
    logger.setLevel(level)

    for handler in logger.handlers[:]:
        logger.removeHandler(handler)

    file_handler = logging.FileHandler(log_filepath, mode="w")
    file_handler.setLevel(level)

    formatter = logging.Formatter("%(asctime)s - %(levelname)s - %(message)s")
    file_handler.setFormatter(formatter)

    logger.addHandler(file_handler)

    if extra_handlers:
        for handler in extra_handlers:
            logger.addHandler(handler)


def standard_setup(output_dir):
    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    setup_logging(output_dir / "run.log")
    return output_dir


@contextmanager
def timed(label):
    start = time.perf_counter()
    yield
    logging.info(f"{label} elapsed: {time.perf_counter() - start:.4f} seconds")


def drop_singleton_clusters(com_df):
    """Shipping guard for com.csv: drop clusters with <= 1 member."""
    counts = com_df["cluster_id"].value_counts()
    kept = counts[counts > 1].index
    n_dropped = len(counts) - len(kept)
    if n_dropped:
        logging.info(f"Dropping {n_dropped} singleton cluster(s) from com.csv")
    return com_df[com_df["cluster_id"].isin(kept)]
