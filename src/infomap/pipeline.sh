#!/bin/bash
#
# Infomap wrapper. Invoked by run_cd.sh dispatcher; sources
# single_stage_pipeline.sh.

set -u

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
if [[ "${SCRIPT_DIR}" == *"/slurmd/job"* ]]; then
    SCRIPT_DIR="${SLURM_SUBMIT_DIR}/src/infomap"
fi
SHARED_DIR="$( cd "${SCRIPT_DIR}/../_common" && pwd )"

INPUT_EDGELIST=""
OUTPUT_DIR=""
SEED="1"
TIMEOUT="3d"
N_THREADS="1"
KEEP_STATE="0"

while [ $# -gt 0 ]; do
    case "$1" in
        --input-edgelist)  INPUT_EDGELIST="$2"; shift 2 ;;
        --output-dir)      OUTPUT_DIR="$2"; shift 2 ;;
        --seed)            SEED="$2"; shift 2 ;;
        --timeout)         TIMEOUT="$2"; shift 2 ;;
        --n-threads)       N_THREADS="$2"; shift 2 ;;
        --keep-state)      KEEP_STATE="1"; shift ;;
        *) echo "Error [infomap/pipeline]: unknown flag $1" >&2; exit 1 ;;
    esac
done

if [ -z "${INPUT_EDGELIST}" ] || [ -z "${OUTPUT_DIR}" ]; then
    echo "Error [infomap/pipeline]: --input-edgelist, --output-dir required." >&2
    exit 2
fi

CD_STAGE_NAME="infomap"
CD_CMD=(python "${SCRIPT_DIR}/run_infomap.py"
        --edgelist "${INPUT_EDGELIST}"
        --output-directory "${OUTPUT_DIR}")
CD_INPUTS="${INPUT_EDGELIST}"
CD_OUTPUTS="${OUTPUT_DIR}/com.csv"
CD_PARAMS=("seed=${SEED}")

# shellcheck disable=SC1091
source "${SHARED_DIR}/single_stage_pipeline.sh"
