import argparse
import logging
import sys
from pathlib import Path

import pandas as pd

from pipeline_common import setup_logging, timed


def parse_args():
    parser = argparse.ArgumentParser(description="Choose the best SBM model based on entropy.")
    parser.add_argument("--entropy_files", nargs="+", required=True)
    parser.add_argument("--com_files", nargs="+", required=True)
    parser.add_argument("--model_names", nargs="+", required=True)
    parser.add_argument("--out_dir", default=".")
    parser.add_argument("--log-file", type=str, default=None)
    return parser.parse_args()


def choose_best_sbm(entropy_files, com_files, model_names, out_dir):
    if not (len(entropy_files) == len(com_files) == len(model_names)):
        logging.error("Number of entropy files, com files, and model names must match.")
        sys.exit(1)

    rows = []
    for e_file, c_file, m_name in zip(entropy_files, com_files, model_names):
        e_path, c_path = Path(e_file), Path(c_file)
        if not e_path.exists() or not c_path.exists():
            logging.error(f"Missing required file: {e_path} or {c_path}")
            sys.exit(1)
        try:
            entropy = float(e_path.read_text().strip())
        except ValueError:
            logging.error(f"Could not read float from {e_path}")
            sys.exit(1)
        rows.append({"model": m_name, "entropy": entropy, "com_file": c_file})

    df = pd.DataFrame(rows)
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    df[["model", "entropy"]].to_csv(out_dir / "models.txt", index=False)

    best = df.loc[df["entropy"].idxmin()]
    (out_dir / "best_model.txt").write_text(best["model"] + "\n")

    base_com = out_dir / "com.csv"
    if base_com.exists() or base_com.is_symlink():
        base_com.unlink()
    base_com.symlink_to(Path(best["com_file"]).resolve())

    logging.info(f"Best model selected: {best['model']}")


def main():
    args = parse_args()
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    setup_logging(Path(args.log_file) if args.log_file else out_dir / "run.log")
    logging.getLogger().addHandler(logging.StreamHandler(sys.stdout))
    with timed("choose_best_sbm"):
        choose_best_sbm(args.entropy_files, args.com_files, args.model_names, args.out_dir)


if __name__ == "__main__":
    main()
