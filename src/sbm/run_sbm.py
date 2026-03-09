import sys
import time
import logging
import argparse
from pathlib import Path

import graph_tool.all as gt
import pandas as pd


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
    parser.add_argument(
        "--edgelist",
        type=str,
        required=True,
        help="Path to the edgelist file",
    )
    parser.add_argument(
        "--output-directory",
        type=str,
        required=True,
        help="Directory to save the output files",
    )
    parser.add_argument(
        "--method",
        type=str,
        choices=["flat-dc", "flat-ndc", "flat-pp", "nested-dc", "nested-ndc"],
        required=True,
        help="Method to use for model selection",
    )
    return parser.parse_args()


args = parse_args()
edgelist_fn = args.edgelist
output_dir = Path(args.output_directory)
method = args.method
is_nested = method.startswith("nested")

# ===========

output_dir.mkdir(parents=True, exist_ok=True)

logging.basicConfig(
    filename=output_dir / "run.log",
    filemode="w",
    level=logging.INFO,
    format="%(asctime)s - %(levelname)s - %(message)s",
)
logging.getLogger().addHandler(logging.StreamHandler(sys.stdout))

# ===========

start = time.perf_counter()

g = load_network(edgelist_fn)

elapsed = time.perf_counter() - start
logging.info(f"[TIME] Loading network: {elapsed}")

# ===========

start = time.perf_counter()

if method in ["flat-dc", "flat-ndc"]:
    state = gt.minimize_blockmodel_dl(
        g,
        state=gt.BlockState,
        state_args=dict(deg_corr=method == "flat-dc"),
    )
elif method == "flat-pp":
    state = gt.minimize_blockmodel_dl(
        g,
        state=gt.PPBlockState,
    )
elif method in ["nested-dc", "nested-ndc"]:
    state = gt.minimize_nested_blockmodel_dl(
        g,
        state_args=dict(deg_corr=method == "nested-dc"),
    )

elapsed = time.perf_counter() - start
logging.info(f"[TIME] Initializing state: {elapsed}")

# ===========

# start = time.perf_counter()

# gt.mcmc_anneal(
#     state,
#     beta_range=(1, 10),
#     niter=1000,
#     mcmc_equilibrate_args=dict(
#         force_niter=10,
#     ),
# )

# delta_S = 1.0
# while delta_S > 0:
#     entropy_before = state.entropy()
#     state.multiflip_mcmc_sweep(beta=float("inf"), niter=10)
#     delta_S = entropy_before - state.entropy()

# elapsed = time.perf_counter() - start
# logging.info(f"[TIME] Refining state and quenching to local minimum: {elapsed}")

# ===========

start = time.perf_counter()

entropy = state.entropy()
with open(output_dir / "entropy.txt", "w") as f:
    f.write(f"{entropy}\n")

elapsed = time.perf_counter() - start
logging.info(f"[TIME] Calculating entropy: {elapsed}")

# ===========

start = time.perf_counter()

if not is_nested:
    refined_partition = state.get_blocks()
else:
    refined_partition = state.get_levels()[0].get_blocks()

data = []
for v in g.vertices():
    data.append(
        {
            "node_id": g.vp.name[v],
            "cluster_id": refined_partition[v],
        }
    )
df = pd.DataFrame(data)

cluster_counts = df["cluster_id"].value_counts()
valid_clusters = cluster_counts[cluster_counts > 1].index
df_filtered = df[df["cluster_id"].isin(valid_clusters)].copy()

unique_ids = sorted(df_filtered["cluster_id"].unique())
id_map = {old_id: new_id for new_id, old_id in enumerate(unique_ids)}
df_filtered["cluster_id"] = df_filtered["cluster_id"].map(id_map)

logging.info(f"Removed {len(df) - len(df_filtered)} singleton nodes.")

output_file = output_dir / "com.csv"
df_filtered.to_csv(output_file, index=False, sep=",", header=["node_id", "cluster_id"])

elapsed = time.perf_counter() - start
logging.info(f"[TIME] Storing refined partition: {elapsed}")
