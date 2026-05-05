import argparse
import logging
import sys
from pathlib import Path

import pandas as pd
from infomap import Infomap

from pipeline_common import drop_singleton_clusters, setup_logging, timed


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("--edgelist", type=str, required=True)
    parser.add_argument("--output-directory", type=str, required=True)
    parser.add_argument("--log-file", type=str, default=None)
    return parser.parse_args()


def main():
    args = parse_args()
    output_dir = Path(args.output_directory)
    output_dir.mkdir(parents=True, exist_ok=True)
    setup_logging(Path(args.log_file) if args.log_file else output_dir / "run.log")
    logging.getLogger().addHandler(logging.StreamHandler(sys.stdout))

    logging.info(f"Reading edgelist from {args.edgelist}...")

    with timed("load_network"):
        df = pd.read_csv(args.edgelist)
        nodes = pd.unique(df[["source", "target"]].values.ravel("K"))
        node_map = {node: i for i, node in enumerate(nodes)}
        inv_node_map = {i: node for node, i in node_map.items()}

        im = Infomap()
        for row in df.itertuples(index=False):
            im.add_link(node_map[row.source], node_map[row.target])

    with timed("infomap_run"):
        im.run()

    with timed("save_results"):
        results = [
            {"node_int": node.node_id, "cluster_id": node.module_id}
            for node in im.tree
            if node.is_leaf
        ]
        df_results = pd.DataFrame(results)
        df_results["node_id"] = df_results["node_int"].map(inv_node_map)

        df_filtered = drop_singleton_clusters(df_results)
        unique_ids = sorted(df_filtered["cluster_id"].unique())
        id_map = {old_id: new_id for new_id, old_id in enumerate(unique_ids)}
        df_filtered = df_filtered.assign(cluster_id=df_filtered["cluster_id"].map(id_map))
        df_filtered[["node_id", "cluster_id"]].to_csv(
            output_dir / "com.csv", index=False, sep=",", header=["node_id", "cluster_id"]
        )


if __name__ == "__main__":
    main()
