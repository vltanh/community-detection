import argparse
import sys
from pathlib import Path
import pandas as pd


def parse_args():
    parser = argparse.ArgumentParser(
        description="Choose the best SBM model based on entropy."
    )
    parser.add_argument(
        "--entropy_files", nargs="+", required=True, help="List of entropy.txt files"
    )
    parser.add_argument(
        "--com_files", nargs="+", required=True, help="List of com.csv files"
    )
    parser.add_argument(
        "--model_names",
        nargs="+",
        required=True,
        help="List of model names corresponding to the files",
    )
    parser.add_argument(
        "--out_dir", default=".", help="Output directory (default: current directory)"
    )
    args = parser.parse_args()
    return args


def choose_best_sbm(entropy_files, com_files, model_names, out_dir):
    """Choose the best SBM model based on entropy and output the result."""
    if not (len(entropy_files) == len(com_files) == len(model_names)):
        print(
            "Error: The number of entropy files, com files, and model names must be identical.",
            file=sys.stderr,
        )
        sys.exit(1)

    data = []
    for e_file, c_file, m_name in zip(entropy_files, com_files, model_names):
        e_path = Path(e_file)
        c_path = Path(c_file)

        # Check that both files exist before attempting to read them
        if not e_path.exists() or not c_path.exists():
            print(
                f"Error: Missing required files -> {e_path} or {c_path}",
                file=sys.stderr,
            )
            sys.exit(1)

        try:
            entropy = float(e_path.read_text().strip())
        except ValueError:
            print(f"Error: Could not read a valid float from {e_path}", file=sys.stderr)
            sys.exit(1)

        data.append({"model": m_name, "entropy": entropy, "com_file": c_file})

    df = pd.DataFrame(data)

    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    # Write models file
    models_file = out_dir / "models.txt"
    df[["model", "entropy"]].to_csv(models_file, index=False)

    # Find the model with the minimum entropy
    best_model = df.loc[df["entropy"].idxmin()]
    best_com_file = Path(best_model["com_file"])
    best_model_name = best_model["model"]

    # Explicitly write the best model's name to a text file for Bash to read
    best_model_file = out_dir / "best_model.txt"
    best_model_file.write_text(best_model_name + "\n")

    # Create a symlink to the best com.csv in the output directory
    base_com = out_dir / "com.csv"
    if base_com.exists() or base_com.is_symlink():
        base_com.unlink()  # Remove existing file or symlink if it exists
    base_com.symlink_to(best_com_file.resolve())

    # Print for standard output logging
    print(f"Best model selected: {best_model_name}")


def main():
    args = parse_args()
    choose_best_sbm(args.entropy_files, args.com_files, args.model_names, args.out_dir)


if __name__ == "__main__":
    main()
