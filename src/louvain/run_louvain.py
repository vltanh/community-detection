"""Wrapper for the upstream gen-louvain CLI (convert + louvain + hierarchy).

Pipeline (per gen-louvain README):
  1. Write the input edge list as space-separated `src dst` text.
  2. `convert -i <text> -o <bin> -r <relabel>`: build the binary graph and
     emit a relabel file (`<orig_id> <renumbered_id>` per line).
  3. `louvain <bin> -l -1 -q <id_qual> [-c alpha] [-k kmin] [-e eps] > <tree>`.
     Reproducibility: when `--seed` is supplied, the binary is run with
     `LD_PRELOAD=tools/louvain_seed/louvain_seed_preload.so` and
     `LOUVAIN_SEED=<seed>` so the unmodified upstream `srand(time+pid)` call is
     overridden by the shim. The upstream source is never patched.
  4. `hierarchy <tree> -n` to read the level count L; the partition we ship is
     the deepest level (best modularity per Blondel et al.).
  5. `hierarchy <tree> -l <L-1>` to print `<renumbered_id> <community_id>`.
  6. Invert the relabel map to recover original node IDs, drop singleton
     clusters, renumber cluster IDs 0..K-1, write `com.csv`.
"""
import argparse
import logging
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import pandas as pd

from pipeline_common import drop_singleton_clusters, setup_logging, timed


QUALITY_IDS = {
    "mod": 0,
    "zahn": 1,
    "owzad": 2,
    "goldberg": 3,
    "condora": 4,
    "devind": 5,
    "devuni": 6,
    "dp": 7,
    "shimalik": 8,
    "balmod": 9,
}


def parse_args():
    p = argparse.ArgumentParser()
    p.add_argument("--edgelist", required=True)
    p.add_argument("--output-directory", required=True)
    p.add_argument("--quality", required=True, choices=sorted(QUALITY_IDS))
    p.add_argument("--alpha", type=float, default=None,
                   help="Owsinski-Zadrozny alpha (q=2); default 0.5.")
    p.add_argument("--kmin", type=int, default=None,
                   help="Shi-Malik kappa_min (q=8); default 1.")
    p.add_argument("--epsilon", type=float, default=None,
                   help="Convergence threshold for one pass.")
    p.add_argument("--seed", type=int, default=None)
    p.add_argument("--louvain-bin-dir", required=True)
    p.add_argument("--seed-preload", default=None,
                   help="Path to louvain_seed_preload.so (LD_PRELOAD shim).")
    p.add_argument("--log-file", type=str, default=None)
    return p.parse_args()


def _read_relabel(path: Path) -> dict[int, int]:
    """Map renumbered_id -> original_id."""
    out: dict[int, int] = {}
    with path.open() as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            orig, renum = line.split()
            out[int(renum)] = int(orig)
    return out


def _hierarchy_levels(hierarchy_bin: Path, tree_path: Path) -> int:
    proc = subprocess.run(
        [str(hierarchy_bin), str(tree_path), "-n"],
        capture_output=True, text=True, check=True,
    )
    for line in proc.stdout.splitlines():
        if line.startswith("Number of levels:"):
            return int(line.split(":", 1)[1].strip())
    raise RuntimeError(f"hierarchy -n produced no level header: {proc.stdout!r}")


def _read_partition(hierarchy_bin: Path, tree_path: Path, level: int) -> dict[int, int]:
    """Run `hierarchy <tree> -l <level>` and parse `node comm` lines."""
    proc = subprocess.run(
        [str(hierarchy_bin), str(tree_path), "-l", str(level)],
        capture_output=True, text=True, check=True,
    )
    out: dict[int, int] = {}
    for line in proc.stdout.splitlines():
        line = line.strip()
        if not line:
            continue
        node, comm = line.split()
        out[int(node)] = int(comm)
    return out


def main():
    args = parse_args()
    output_dir = Path(args.output_directory)
    output_dir.mkdir(parents=True, exist_ok=True)
    setup_logging(Path(args.log_file) if args.log_file else output_dir / "run.log")
    logging.getLogger().addHandler(logging.StreamHandler(sys.stdout))

    bin_dir = Path(args.louvain_bin_dir).resolve()
    convert_bin = bin_dir / "convert"
    louvain_bin = bin_dir / "louvain"
    hierarchy_bin = bin_dir / "hierarchy"
    for b in (convert_bin, louvain_bin, hierarchy_bin):
        if not b.is_file() or not os.access(b, os.X_OK):
            raise FileNotFoundError(
                f"louvain binary not found / not executable: {b} "
                f"(build via `cd {bin_dir} && make`)"
            )

    qual_id = QUALITY_IDS[args.quality]
    if args.quality == "owzad" and args.alpha is None:
        args.alpha = 0.5
    if args.quality == "shimalik" and args.kmin is None:
        args.kmin = 1

    with timed("load_network"):
        df = pd.read_csv(args.edgelist)

    with tempfile.TemporaryDirectory(prefix="louvain_") as workdir:
        work = Path(workdir)
        text_path = work / "graph.txt"
        bin_path = work / "graph.bin"
        relabel_path = work / "relabel.txt"
        tree_path = work / "graph.tree"

        with timed("write_text"):
            df.to_csv(text_path, sep=" ", index=False, header=False)

        with timed("convert"):
            subprocess.run(
                [str(convert_bin), "-i", str(text_path),
                 "-o", str(bin_path), "-r", str(relabel_path)],
                check=True,
            )

        cmd = [str(louvain_bin), str(bin_path), "-l", "-1", "-q", str(qual_id)]
        if args.quality == "owzad":
            cmd += ["-c", f"{args.alpha:.10g}"]
        if args.quality == "shimalik":
            cmd += ["-k", str(args.kmin)]
        if args.epsilon is not None:
            cmd += ["-e", f"{args.epsilon:.10g}"]

        env = os.environ.copy()
        if args.seed is not None:
            if not args.seed_preload:
                raise RuntimeError(
                    "--seed requires --seed-preload pointing at "
                    "louvain_seed_preload.so (build via "
                    "src/louvain/seed_shim/build.sh)."
                )
            preload = Path(args.seed_preload).resolve()
            if not preload.is_file():
                raise FileNotFoundError(
                    f"louvain seed shim not found: {preload}"
                )
            existing = env.get("LD_PRELOAD", "")
            env["LD_PRELOAD"] = (
                f"{preload}:{existing}" if existing else str(preload)
            )
            env["LOUVAIN_SEED"] = str(args.seed)

        with timed("louvain_run"):
            with tree_path.open("w") as fout:
                subprocess.run(cmd, check=True, stdout=fout, env=env)

        with timed("read_partition"):
            n_levels = _hierarchy_levels(hierarchy_bin, tree_path)
            partition = _read_partition(hierarchy_bin, tree_path, n_levels - 1)
            renum_to_orig = _read_relabel(relabel_path)

    with timed("save_results"):
        rows = [
            (renum_to_orig[renum], comm)
            for renum, comm in partition.items()
            if renum in renum_to_orig
        ]
        df_full = pd.DataFrame(rows, columns=["node_id", "cluster_id"])
        df_filtered = drop_singleton_clusters(df_full)
        unique_ids = sorted(df_filtered["cluster_id"].unique())
        id_map = {old: new for new, old in enumerate(unique_ids)}
        df_filtered = df_filtered.assign(
            cluster_id=df_filtered["cluster_id"].map(id_map)
        )
        df_filtered = df_filtered.sort_values("node_id")
        df_filtered.to_csv(
            output_dir / "com.csv",
            index=False, sep=",", header=["node_id", "cluster_id"],
        )


if __name__ == "__main__":
    main()
