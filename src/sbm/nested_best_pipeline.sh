#!/bin/bash
#
# SBM nested-best meta-model wrapper. Picks the lowest-entropy variant from
# {nested-dc, nested-ndc} via choose_best_sbm.py + symlinks the winner's
# com.csv. Dispatcher pre-runs the two nested variants and passes paths.

set -u

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
if [[ "${SCRIPT_DIR}" == *"/slurmd/job"* ]]; then
    SCRIPT_DIR="${SLURM_SUBMIT_DIR}/src/sbm"
fi
SHARED_DIR="$( cd "${SCRIPT_DIR}/../_common" && pwd )"

OUTPUT_DIR=""
DC_DIR=""
NDC_DIR=""
SEED="1"
TIMEOUT="3d"
KEEP_STATE="0"

while [ $# -gt 0 ]; do
    case "$1" in
        --output-dir)      OUTPUT_DIR="$2"; shift 2 ;;
        --dc-dir)          DC_DIR="$2"; shift 2 ;;
        --ndc-dir)         NDC_DIR="$2"; shift 2 ;;
        --seed)            SEED="$2"; shift 2 ;;
        --timeout)         TIMEOUT="$2"; shift 2 ;;
        --keep-state)      KEEP_STATE="1"; shift ;;
        *) echo "Error [sbm/nested_best_pipeline]: unknown flag $1" >&2; exit 1 ;;
    esac
done

if [ -z "${OUTPUT_DIR}" ] || [ -z "${DC_DIR}" ] || [ -z "${NDC_DIR}" ]; then
    echo "Error [sbm/nested_best_pipeline]: --output-dir, --dc-dir, --ndc-dir required." >&2
    exit 2
fi

for d in "${DC_DIR}" "${NDC_DIR}"; do
    if [ ! -f "${d}/com.csv" ] || [ ! -f "${d}/entropy.txt" ]; then
        echo "Error [sbm/nested_best_pipeline]: ${d}/com.csv or entropy.txt missing." >&2
        exit 1
    fi
done

CD_STAGE_NAME="sbm-nested-best"
CD_CMD=(python "${SCRIPT_DIR}/choose_best_sbm.py"
        --entropy_files "${DC_DIR}/entropy.txt" "${NDC_DIR}/entropy.txt"
        --com_files     "${DC_DIR}/com.csv"     "${NDC_DIR}/com.csv"
        --model_names   "nested-dc" "nested-ndc"
        --out_dir       "${OUTPUT_DIR}")
CD_INPUTS="${DC_DIR}/entropy.txt ${NDC_DIR}/entropy.txt ${DC_DIR}/com.csv ${NDC_DIR}/com.csv"
CD_OUTPUTS="${OUTPUT_DIR}/com.csv ${OUTPUT_DIR}/best_model.txt ${OUTPUT_DIR}/models.txt"
CD_PARAMS=("variants=nested-dc,nested-ndc" "seed=${SEED}")

# shellcheck disable=SC1091
source "${SHARED_DIR}/single_stage_pipeline.sh"
