import sys
import time
import logging
import argparse
from pathlib import Path

import igraph as ig
import pandas as pd
import leidenalg as la


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--edgelist",
        type=str,
        required=True,
    )
    parser.add_argument(
        "--output-directory",
        type=str,
        required=True,
    )
    parser.add_argument(
        "--model",
        type=str,
        choices=["cpm", "mod"],
    )
    parser.add_argument(
        "--resolution",
        type=float,
        default=None,
    )
    parser.add_argument(
        "--seed",
        type=int,
        default=1234,
    )
    parser.add_argument(
        "--weighted",
        action="store_true",
    )
    parser.add_argument(
        "--n-iterations",
        type=int,
        default=2,
    )
    return parser.parse_args()


args = parse_args()
edgelist_fn = args.edgelist
output_dir = Path(args.output_directory)
model = args.model
resolution = args.resolution
seed = args.seed
n_iterations = args.n_iterations
is_weighted = args.weighted

output_dir.mkdir(parents=True, exist_ok=True)

logging.basicConfig(
    filename=output_dir / "run.log",
    filemode="w",
    level=logging.INFO,
    format="%(asctime)s - %(levelname)s - %(message)s",
)
logging.getLogger().addHandler(logging.StreamHandler(sys.stdout))

start = time.perf_counter()

df = pd.read_csv(edgelist_fn)
g = ig.Graph.TupleList(
    df.itertuples(index=False),
    directed=False,
    vertex_name_attr="name",
    weights="weight" if is_weighted else None,
)

elapsed = time.perf_counter() - start
logging.info(f"[TIME] Loading network: {elapsed}")

start = time.perf_counter()

if model == "cpm":
    partition = la.find_partition(
        g,
        la.CPMVertexPartition,
        resolution_parameter=resolution,
        seed=seed,
        n_iterations=n_iterations,
        weights="weight" if is_weighted else None,
    )
elif model == "mod":
    partition = la.find_partition(
        g,
        la.ModularityVertexPartition,
        seed=seed,
        n_iterations=n_iterations,
        weights="weight" if is_weighted else None,
    )
else:
    raise ValueError(f"Unknown model: {model}")

elapsed = time.perf_counter() - start
logging.info(f"[TIME] Running Leiden algorithm: {elapsed}")

start = time.perf_counter()

df2 = pd.DataFrame(
    {
        "node_id": g.vs["name"],
        "cluster_id": partition.membership,
    }
)

cluster_counts = df2["cluster_id"].value_counts()
valid_clusters = cluster_counts[cluster_counts > 1].index
df_filtered = df2[df2["cluster_id"].isin(valid_clusters)].copy()

unique_ids = sorted(df_filtered["cluster_id"].unique())
id_map = {old_id: new_id for new_id, old_id in enumerate(unique_ids)}
df_filtered["cluster_id"] = df_filtered["cluster_id"].map(id_map)

logging.info(f"Removed {len(df2) - len(df_filtered)} singleton nodes.")

df_filtered.to_csv(
    output_dir / "com.csv", index=False, sep=",", header=["node_id", "cluster_id"]
)

elapsed = time.perf_counter() - start
logging.info(f"[TIME] Saving results: {elapsed}")
