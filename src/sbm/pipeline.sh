#!/bin/bash
#
# SBM wrapper (flat-dc / flat-ndc / flat-pp / nested-dc / nested-ndc).
# Invoked by run_cd.sh dispatcher; sources single_stage_pipeline.sh.
# *-best variants live in flat_best_pipeline.sh + nested_best_pipeline.sh.

set -u

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
if [[ "${SCRIPT_DIR}" == *"/slurmd/job"* ]]; then
    SCRIPT_DIR="${SLURM_SUBMIT_DIR}/src/sbm"
fi
SHARED_DIR="$( cd "${SCRIPT_DIR}/../_common" && pwd )"

INPUT_EDGELIST=""
OUTPUT_DIR=""
SBM_METHOD=""
SEED="1"
TIMEOUT="3d"
N_THREADS="1"
KEEP_STATE="0"

while [ $# -gt 0 ]; do
    case "$1" in
        --input-edgelist)  INPUT_EDGELIST="$2"; shift 2 ;;
        --output-dir)      OUTPUT_DIR="$2"; shift 2 ;;
        --method)          SBM_METHOD="$2"; shift 2 ;;
        --seed)            SEED="$2"; shift 2 ;;
        --timeout)         TIMEOUT="$2"; shift 2 ;;
        --n-threads)       N_THREADS="$2"; shift 2 ;;
        --keep-state)      KEEP_STATE="1"; shift ;;
        *) echo "Error [sbm/pipeline]: unknown flag $1" >&2; exit 1 ;;
    esac
done

if [ -z "${INPUT_EDGELIST}" ] || [ -z "${OUTPUT_DIR}" ] || [ -z "${SBM_METHOD}" ]; then
    echo "Error [sbm/pipeline]: --input-edgelist, --output-dir, --method required." >&2
    exit 2
fi

case "${SBM_METHOD}" in
    flat-dc|flat-ndc|flat-pp|nested-dc|nested-ndc) ;;
    *) echo "Error [sbm/pipeline]: --method must be one of flat-dc/flat-ndc/flat-pp/nested-dc/nested-ndc (got ${SBM_METHOD})." >&2
       exit 2 ;;
esac

# graph-tool reads OMP_NUM_THREADS; pinned to N_THREADS for deterministic runs.
export OMP_NUM_THREADS="${N_THREADS}"

CD_STAGE_NAME="sbm-${SBM_METHOD}"
CD_CMD=(python "${SCRIPT_DIR}/run_sbm.py"
        --edgelist "${INPUT_EDGELIST}"
        --output-directory "${OUTPUT_DIR}"
        --method "${SBM_METHOD}")
CD_INPUTS="${INPUT_EDGELIST}"
CD_OUTPUTS="${OUTPUT_DIR}/com.csv ${OUTPUT_DIR}/entropy.txt"
CD_PARAMS=("method=${SBM_METHOD}" "seed=${SEED}" "n_threads=${N_THREADS}")

# shellcheck disable=SC1091
source "${SHARED_DIR}/single_stage_pipeline.sh"
