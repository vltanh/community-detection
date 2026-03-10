# Community Detection Pipeline

This script computes base clusterings on a network (including selects the best SBM models if applicable), refines the structure by post-processing with CC/WCC/CM, and evaluates both statistics and ground-truth accuracy.

## 1. Custom Mode (Standard Usage)

Use this mode to provide explicit file paths for your own datasets.

**Usage:**

```bash
./run_cd.sh --algo <algo> --input-edgelist <path> --output-dir <dir> [OPTIONS]
```

### Required Arguments

| Argument | Description |
| --- | --- |
| `--algo <algo>` | Algorithm to run. |
| `--input-edgelist <p>` | Path to the input edge list CSV. |
| `--output-dir <dir>` | Base directory for outputs. |

### Optional Arguments & Flags

| Argument | Description |
| --- | --- |
| `--criterion <name>` | Connectedness criterion for WCC and CM (e.g., `sqrt` or `log`). |
| `--network <id>` | Optional identifier to group outputs in the directory structure. |
| `--generator <gen>` | Optional identifier to group outputs in the directory structure. |
| `--gt-clustering-id <id>` | Optional identifier to group outputs in the directory structure. |
| `--run-id <id>` | Optional numerical run identifier. |
| `--run-stats` | Enables network statistics computation. |
| `--input-gt-clustering <p>` | Path to ground-truth `com.csv` (triggers accuracy evaluation if `--run-acc` is enabled). |
| `--run-acc` | Enables accuracy evaluation against ground truth. |
| `--run-cc` / `-wcc` / `-cm` | Enables post-processing refinement algorithms. |

### Intended Custom Structure

Outputs are dynamically nested based on the pathing identifiers provided.

**Inputs (Manually Provided):**

* Input edgelist: `<input-edgelist>`
* Ground truth clustering (optional): `<input-gt-clustering>`

**Outputs (Dynamically Routed):**
*(Note: `<out-root>` represents `<output-dir>[/<generator>][/<gt-clustering-id>]`, `<algo-pp>` represents `<algo>[+<pp>(<criterion>)]`, and `<sub-path>` represents `[/<network>][/<run-id>]`)*

* Base/Refined Clusterings: `<out-root>/clusterings/<algo-pp><sub-path>/com.csv`
* Stats (`--run-stats`): `<out-root>/stats/<algo-pp><sub-path>/`
* Accuracy (`--run-acc`): `<out-root>/acc/<algo-pp><sub-path>/`

## 2. Macro Mode (Internal / Pre-configured Usage)

Use this mode to automatically map inputs and outputs to the standard `data/` directory structure for either Empirical (`--real`) or Synthetic (`--synthetic`) networks.

**Usage (Real Networks):**

```bash
./run_cd.sh --real --algo <algo> --network <id> [OPTIONS]
```

**Usage (Synthetic Networks):**

```bash
./run_cd.sh --synthetic --algo <algo> --network <id> --generator <gen> --gt-clustering-id <id> [OPTIONS]
```

### Required Arguments

| Argument | Description |
| --- | --- |
| `--real` OR `--synthetic` | Flag to trigger the internal pathing engine for the respective dataset type. |
| `--algo <algo>` | Algorithm to run. |
| `--network <id>` | Network identifier used to locate empirical/synthetic data. |
| `--generator <gen>` | Generator used (Required for `--synthetic`). |
| `--gt-clustering-id <id>` | Ground-truth identifier (Required for `--synthetic`). |

### Optional Arguments & Flags

| Argument | Description |
| --- | --- |
| `--criterion <name>` | Connectedness criterion for WCC and CM. |
| `--run-id <id>` | Numerical run identifier (Defaults to `0` for `--synthetic`). |
| `--run-stats` | Enables network statistics computation. |
| `--run-acc` | Enables accuracy evaluation against ground truth. |
| `--run-cc` / `-wcc` / `-cm` | Enables post-processing refinement algorithms. |

### Intended Macro Structure

When using macros, the script strictly adheres to the following architecture based on the provided IDs.

**Inputs (Auto-Resolved):**

* Input edgelist:
    * Real: `data/empirical_networks/networks/<network>/<network>.csv`
    * Synthetic: `data/synthetic_networks/networks/<generator>/<gt-clustering-id>/<network>/<run-id>/edge.csv`
* Ground truth:
    * Synthetic: `data/reference_clusterings/clusterings/<gt-clustering-id>/<network>/com.csv`

**Outputs (Auto-Routed):**

Base Dir (`<out-root>`): 
* Real: `data/reference_clusterings`
* Synthetic: `data/estimated_clusterings/<generator>/<gt-clustering-id>`

Inside the respective `<out-root>`, outputs follow this structure (with `<algo-pp>` representing `<algo>[+<pp>(<criterion>)]`):

* Base/Refined Clusterings: `<out-root>/clusterings/<algo-pp>/<network>[/<run-id>]/com.csv`
* Stats (`--run-stats`): `<out-root>/stats/<algo-pp>/<network>[/<run-id>]/`
* Accuracy (`--run-acc`): `<out-root>/acc/<algo-pp>/<network>[/<run-id>]/`

## Pipeline Execution Steps

Regardless of the mode used, the script executes the following stages (skipping evaluation and post-processing steps unless their flags are provided):

### Step 1: Base Clustering

Computes the initial community structure using the specified algorithm (`leiden`, `infomap`, `ikc`, `sbm`, etc.).

* **Outputs:** `[OUT_ROOT]/clusterings/<algo><SUB_PATH>/com.csv`

### Step 2: SBM Best Model Selection

*(Executes only if `<algo>` is `sbm-flat-best` or `sbm-nested-best`)*. Evaluates entropy across pre-computed SBM variants (`dc`, `ndc`, `pp`) to select the winning model and creates symlinks to its outputs.

* **Outputs:** `best_model.txt` and generated symlinks.

### Step 3: Statistics Computation (`--run-stats`)

Calculates network metrics for the estimated base clustering.

* **Outputs:** `[OUT_ROOT]/stats/<algo><SUB_PATH>/`

### Step 4: Accuracy Evaluation (`--run-acc`)

Compares the estimated base clustering against the provided ground truth.

* **Outputs:** `[OUT_ROOT]/acc/<algo><SUB_PATH>/`

### Step 5: Post-Processing (`--run-cc`, `--run-wcc`, `--run-cm`)

Refines the base clustering via Constrained Clustering variants.

* **Outputs:** `[OUT_ROOT]/clusterings/<algo>+<pp>[criterion]<SUB_PATH>/com.csv`

### Step 6: Post-Processing Evaluation

Re-runs the Statistics (Step 3) and Accuracy (Step 4) evaluations on the newly refined clustering outputs, subject to the same `--run-stats` and `--run-acc` flags.

* **Outputs:** `[OUT_ROOT]/stats/<algo>+<pp>[criterion]<SUB_PATH>/` and `[OUT_ROOT]/acc/<algo>+<pp>[criterion]<SUB_PATH>/`

## Examples

In each block below, both commands execute the same clustering logic and output to the same structure, but Macro mode auto-routes while Custom mode allows precise path control.

```bash
# Custom mode
./run_cd.sh \
    --algo sbm-flat-dc \
    --criterion log \
    --input-edgelist test/input/dnc/dnc.csv \
    --output-dir test/output/reference_clusterings \
    --network dnc \
    --run-stats \
    --run-cc --run-wcc --run-cm

# Macro mode
./run_cd.sh \
    --algo sbm-flat-dc \
    --criterion log \
    --real \
    --network dnc \
    --run-stats \
    --run-cc --run-wcc --run-cm
```

```bash
# Custom mode
./run_cd.sh \
    --algo sbm-flat-best \
    --criterion sqrt \
    --input-edgelist "test/output/synthetic_networks/networks/ec-sbm-v2/sbm-flat-best+wcc(log)/dnc/0/edge.csv" \
    --input-gt-clustering "test/output/reference_clusterings/clusterings/sbm-flat-best+wcc(log)/dnc/com.csv" \
    --output-dir test/output/estimated_clusterings \
    --network dnc --generator ec-sbm-v2 --gt-clustering-id "sbm-flat-best+wcc(log)" --run-id 0 \
    --run-stats --run-acc \
    --run-cc --run-wcc --run-cm

# Macro mode
./run_cd.sh \
    --algo sbm-flat-best \
    --criterion sqrt \
    --synthetic \
    --network dnc --generator ec-sbm-v2 --gt-clustering-id "sbm-flat-best+wcc(log)" --run-id 0 \
    --run-cc --run-wcc --run-cm --run-stats --run-acc
```