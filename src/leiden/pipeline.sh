#!/bin/bash
#
# Leiden CPM + Modularity wrapper.
# Invoked by run_cd.sh dispatcher; sources single_stage_pipeline.sh.

set -u

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
if [[ "${SCRIPT_DIR}" == *"/slurmd/job"* ]]; then
    SCRIPT_DIR="${SLURM_SUBMIT_DIR}/src/leiden"
fi
SHARED_DIR="$( cd "${SCRIPT_DIR}/../_common" && pwd )"

INPUT_EDGELIST=""
OUTPUT_DIR=""
LEIDEN_MODEL=""
LEIDEN_RESOLUTION=""
LEIDEN_N_ITERATIONS="2"
LEIDEN_WEIGHTED="false"
SEED="1"
TIMEOUT="3d"
N_THREADS="1"
KEEP_STATE="0"

while [ $# -gt 0 ]; do
    case "$1" in
        --input-edgelist)    INPUT_EDGELIST="$2"; shift 2 ;;
        --output-dir)        OUTPUT_DIR="$2"; shift 2 ;;
        --model)             LEIDEN_MODEL="$2"; shift 2 ;;
        --resolution)        LEIDEN_RESOLUTION="$2"; shift 2 ;;
        --n-iterations)      LEIDEN_N_ITERATIONS="$2"; shift 2 ;;
        --weighted)          LEIDEN_WEIGHTED="true"; shift ;;
        --seed)              SEED="$2"; shift 2 ;;
        --timeout)           TIMEOUT="$2"; shift 2 ;;
        --n-threads)         N_THREADS="$2"; shift 2 ;;
        --keep-state)        KEEP_STATE="1"; shift ;;
        *) echo "Error [leiden/pipeline]: unknown flag $1" >&2; exit 1 ;;
    esac
done

if [ -z "${INPUT_EDGELIST}" ] || [ -z "${OUTPUT_DIR}" ] || [ -z "${LEIDEN_MODEL}" ]; then
    echo "Error [leiden/pipeline]: --input-edgelist, --output-dir, --model required." >&2
    exit 2
fi

if [ "${LEIDEN_MODEL}" != "cpm" ] && [ "${LEIDEN_MODEL}" != "mod" ]; then
    echo "Error [leiden/pipeline]: --model must be cpm or mod (got ${LEIDEN_MODEL})." >&2
    exit 2
fi

if [ "${LEIDEN_MODEL}" = "cpm" ] && [ -z "${LEIDEN_RESOLUTION}" ]; then
    echo "Error [leiden/pipeline]: --model cpm requires --resolution." >&2
    exit 2
fi

CD_STAGE_NAME="leiden-${LEIDEN_MODEL}${LEIDEN_RESOLUTION:+-${LEIDEN_RESOLUTION}}"

CMD=(python "${SCRIPT_DIR}/run_leiden.py"
     --edgelist "${INPUT_EDGELIST}"
     --output-directory "${OUTPUT_DIR}"
     --model "${LEIDEN_MODEL}"
     --seed "${SEED}"
     --n-iterations "${LEIDEN_N_ITERATIONS}")
[ -n "${LEIDEN_RESOLUTION}" ] && CMD+=(--resolution "${LEIDEN_RESOLUTION}")
[ "${LEIDEN_WEIGHTED}" = "true" ] && CMD+=(--weighted)

CD_CMD=("${CMD[@]}")
CD_INPUTS="${INPUT_EDGELIST}"
CD_OUTPUTS="${OUTPUT_DIR}/com.csv"
CD_PARAMS=("model=${LEIDEN_MODEL}"
           "resolution=${LEIDEN_RESOLUTION}"
           "n_iterations=${LEIDEN_N_ITERATIONS}"
           "weighted=${LEIDEN_WEIGHTED}"
           "seed=${SEED}")

# shellcheck disable=SC1091
source "${SHARED_DIR}/single_stage_pipeline.sh"
