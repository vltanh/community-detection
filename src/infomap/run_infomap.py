import sys
import time
import logging
import argparse
from pathlib import Path

import pandas as pd
from infomap import Infomap


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
    return parser.parse_args()


args = parse_args()
edgelist_fn = args.edgelist
output_dir = Path(args.output_directory)

output_dir.mkdir(parents=True, exist_ok=True)

logging.basicConfig(
    filename=output_dir / "run.log",
    filemode="w",
    level=logging.INFO,
    format="%(asctime)s - %(levelname)s - %(message)s",
)
logging.getLogger().addHandler(logging.StreamHandler(sys.stdout))

start = time.perf_counter()

im = Infomap()

logging.info(f"Reading edgelist from {edgelist_fn}...")

df = pd.read_csv(edgelist_fn)

nodes = pd.unique(df[["source", "target"]].values.ravel("K"))
node_map = {node: i for i, node in enumerate(nodes)}
inv_node_map = {i: node for node, i in node_map.items()}

for row in df.itertuples(index=False):
    im.add_link(node_map[row.source], node_map[row.target])

elapsed = time.perf_counter() - start
logging.info(f"[TIME] Loading network: {elapsed}")

start = time.perf_counter()

im.run()

elapsed = time.perf_counter() - start
logging.info(f"[TIME] Running Infomap algorithm: {elapsed}")

start = time.perf_counter()

results = []
for node in im.tree:
    if node.is_leaf:
        results.append({"node_int": node.node_id, "cluster_id": node.module_id})

df_results = pd.DataFrame(results)

df_results["node_id"] = df_results["node_int"].map(inv_node_map)

cluster_counts = df_results["cluster_id"].value_counts()
valid_clusters = cluster_counts[cluster_counts > 1].index
df_filtered = df_results[df_results["cluster_id"].isin(valid_clusters)].copy()

unique_ids = sorted(df_filtered["cluster_id"].unique())
id_map = {old_id: new_id for new_id, old_id in enumerate(unique_ids)}
df_filtered["cluster_id"] = df_filtered["cluster_id"].map(id_map)

logging.info(f"Removed {len(df_results) - len(df_filtered)} singleton nodes.")

output_file = output_dir / "com.csv"
df_filtered[["node_id", "cluster_id"]].to_csv(
    output_file, index=False, sep=",", header=["node_id", "cluster_id"]
)

elapsed = time.perf_counter() - start
logging.info(f"[TIME] Saving results: {elapsed}")
