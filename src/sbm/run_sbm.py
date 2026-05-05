import argparse
import logging
import sys
from pathlib import Path

import graph_tool.all as gt
import numpy as np
import pandas as pd

from pipeline_common import drop_singleton_clusters, setup_logging, timed


def load_network(edgelist_fn):
    g = gt.load_graph_from_csv(
        edgelist_fn,
        skip_first=True,
        csv_options={"delimiter": ","},
    )
    gt.remove_parallel_edges(g)
    gt.remove_self_loops(g)
    return g


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("--edgelist", type=str, required=True)
    parser.add_argument("--output-directory", type=str, required=True)
    parser.add_argument("--log-file", type=str, default=None)
    parser.add_argument(
        "--method",
        type=str,
        choices=["flat-dc", "flat-ndc", "flat-pp", "nested-dc", "nested-ndc"],
        required=True,
    )
    parser.add_argument("--seed", type=int, default=1)
    return parser.parse_args()


def main():
    args = parse_args()
    output_dir = Path(args.output_directory)
    output_dir.mkdir(parents=True, exist_ok=True)
    setup_logging(Path(args.log_file) if args.log_file else output_dir / "run.log")
    logging.getLogger().addHandler(logging.StreamHandler(sys.stdout))

    is_nested = args.method.startswith("nested")

    np.random.seed(args.seed)
    gt.seed_rng(args.seed)

    with timed("load_network"):
        g = load_network(args.edgelist)

    with timed("sbm_init_state"):
        if args.method in ("flat-dc", "flat-ndc"):
            state = gt.minimize_blockmodel_dl(
                g,
                state=gt.BlockState,
                state_args=dict(deg_corr=args.method == "flat-dc"),
            )
        elif args.method == "flat-pp":
            state = gt.minimize_blockmodel_dl(g, state=gt.PPBlockState)
        else:
            state = gt.minimize_nested_blockmodel_dl(
                g, state_args=dict(deg_corr=args.method == "nested-dc")
            )

    with timed("entropy"):
        entropy = state.entropy()
        (output_dir / "entropy.txt").write_text(f"{entropy}\n")

    with timed("save_results"):
        if not is_nested:
            partition = state.get_blocks()
        else:
            partition = state.get_levels()[0].get_blocks()

        df = pd.DataFrame(
            {
                "node_id": [g.vp.name[v] for v in g.vertices()],
                "cluster_id": [partition[v] for v in g.vertices()],
            }
        )

        df_filtered = drop_singleton_clusters(df)
        unique_ids = sorted(df_filtered["cluster_id"].unique())
        id_map = {old_id: new_id for new_id, old_id in enumerate(unique_ids)}
        df_filtered = df_filtered.assign(cluster_id=df_filtered["cluster_id"].map(id_map))
        df_filtered.to_csv(
            output_dir / "com.csv", index=False, sep=",", header=["node_id", "cluster_id"]
        )


if __name__ == "__main__":
    main()
