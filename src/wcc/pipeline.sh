#!/bin/bash
#
# WCC (well-connected components) post-proc wrapper. Invokes the
# constrained_clustering MincutOnly subcommand with a threshold criterion
# (default 1log_10(n); sqrt is 0.2n^0.5).

set -u

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
if [[ "${SCRIPT_DIR}" == *"/slurmd/job"* ]]; then
    SCRIPT_DIR="${SLURM_SUBMIT_DIR}/src/wcc"
fi
SHARED_DIR="$( cd "${SCRIPT_DIR}/../_common" && pwd )"
DEFAULT_BINARY="$( cd "${SCRIPT_DIR}/../.." && pwd )/constrained-clustering/constrained_clustering"

INPUT_EDGELIST=""
BASE_COM=""
OUTPUT_DIR=""
BINARY="${DEFAULT_BINARY}"
WCC_CRIT="1log_10(n)"
TIMEOUT="3d"
SEED="1"
N_THREADS="1"
KEEP_STATE="0"

while [ $# -gt 0 ]; do
    case "$1" in
        --input-edgelist)  INPUT_EDGELIST="$2"; shift 2 ;;
        --base-com)        BASE_COM="$2"; shift 2 ;;
        --output-dir)      OUTPUT_DIR="$2"; shift 2 ;;
        --binary)          BINARY="$2"; shift 2 ;;
        --criterion)       WCC_CRIT="$2"; shift 2 ;;
        --seed)            SEED="$2"; shift 2 ;;
        --timeout)         TIMEOUT="$2"; shift 2 ;;
        --n-threads)       N_THREADS="$2"; shift 2 ;;
        --keep-state)      KEEP_STATE="1"; shift ;;
        *) echo "Error [wcc/pipeline]: unknown flag $1" >&2; exit 1 ;;
    esac
done

if [ -z "${INPUT_EDGELIST}" ] || [ -z "${BASE_COM}" ] || [ -z "${OUTPUT_DIR}" ]; then
    echo "Error [wcc/pipeline]: --input-edgelist, --base-com, --output-dir required." >&2
    exit 2
fi

if [ ! -x "${BINARY}" ]; then
    echo "Error [wcc/pipeline]: constrained_clustering binary not found at ${BINARY}." >&2
    echo "Build via: cd constrained-clustering && bash setup.sh && bash easy_build_and_compile.sh" >&2
    exit 1
fi

CD_STAGE_NAME="wcc"
mkdir -p "${OUTPUT_DIR}/.state"
CD_CMD=("${BINARY}" MincutOnly
        --connectedness-criterion "${WCC_CRIT}"
        --edgelist "${INPUT_EDGELIST}"
        --existing-clustering "${BASE_COM}"
        --num-processors "${N_THREADS}"
        --output-file "${OUTPUT_DIR}/com.csv"
        --log-file "${OUTPUT_DIR}/.state/wcc.log"
        --log-level 1)
CD_INPUTS="${INPUT_EDGELIST} ${BASE_COM}"
CD_OUTPUTS="${OUTPUT_DIR}/com.csv"
CD_PARAMS=("criterion=${WCC_CRIT}" "n_threads=${N_THREADS}" "seed=${SEED}")
CD_EXTRA_LOGS=("${OUTPUT_DIR}/.state/wcc.log")

# shellcheck disable=SC1091
source "${SHARED_DIR}/single_stage_pipeline.sh"
