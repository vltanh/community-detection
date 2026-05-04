import argparse
import logging
import sys
from pathlib import Path

import igraph as ig
import leidenalg as la
import pandas as pd

from pipeline_common import drop_singleton_clusters, standard_setup, timed


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("--edgelist", type=str, required=True)
    parser.add_argument("--output-directory", type=str, required=True)
    parser.add_argument("--model", type=str, choices=["cpm", "mod"])
    parser.add_argument("--resolution", type=float, default=None)
    parser.add_argument("--seed", type=int, default=1234)
    parser.add_argument("--weighted", action="store_true")
    parser.add_argument("--n-iterations", type=int, default=2)
    return parser.parse_args()


def main():
    args = parse_args()
    output_dir = standard_setup(args.output_directory)
    logging.getLogger().addHandler(logging.StreamHandler(sys.stdout))

    with timed("load_network"):
        df = pd.read_csv(args.edgelist)
        g = ig.Graph.TupleList(
            df.itertuples(index=False),
            directed=False,
            vertex_name_attr="name",
            weights="weight" if args.weighted else None,
        )

    with timed("leiden_run"):
        if args.model == "cpm":
            partition = la.find_partition(
                g,
                la.CPMVertexPartition,
                resolution_parameter=args.resolution,
                seed=args.seed,
                n_iterations=args.n_iterations,
                weights="weight" if args.weighted else None,
            )
        elif args.model == "mod":
            partition = la.find_partition(
                g,
                la.ModularityVertexPartition,
                seed=args.seed,
                n_iterations=args.n_iterations,
                weights="weight" if args.weighted else None,
            )
        else:
            raise ValueError(f"Unknown model: {args.model}")

    with timed("save_results"):
        df_full = pd.DataFrame(
            {"node_id": g.vs["name"], "cluster_id": partition.membership}
        )
        df_filtered = drop_singleton_clusters(df_full)
        unique_ids = sorted(df_filtered["cluster_id"].unique())
        id_map = {old_id: new_id for new_id, old_id in enumerate(unique_ids)}
        df_filtered = df_filtered.assign(cluster_id=df_filtered["cluster_id"].map(id_map))
        df_filtered.to_csv(
            output_dir / "com.csv", index=False, sep=",", header=["node_id", "cluster_id"]
        )


if __name__ == "__main__":
    main()
