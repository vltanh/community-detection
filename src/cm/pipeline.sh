#!/bin/bash
#
# CM (Connectivity Modifier) post-proc wrapper. Invokes the
# constrained_clustering CM subcommand which iteratively mincuts +
# re-clusters each disconnected piece until well-connected.
# Supported base algos: leiden-cpm, leiden-mod, louvain (modularity).

set -u

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
if [[ "${SCRIPT_DIR}" == *"/slurmd/job"* ]]; then
    SCRIPT_DIR="${SLURM_SUBMIT_DIR}/src/cm"
fi
SHARED_DIR="$( cd "${SCRIPT_DIR}/../_common" && pwd )"
DEFAULT_BINARY="$( cd "${SCRIPT_DIR}/../.." && pwd )/constrained-clustering/constrained_clustering"

INPUT_EDGELIST=""
BASE_COM=""
OUTPUT_DIR=""
BINARY="${DEFAULT_BINARY}"
CM_CRIT="0.2n^0.5"
BASE_ALGO=""
BASE_RESOLUTION=""
MINCUT_TYPE="cactus"
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
        --criterion)       CM_CRIT="$2"; shift 2 ;;
        --base-algo)       BASE_ALGO="$2"; shift 2 ;;
        --base-resolution) BASE_RESOLUTION="$2"; shift 2 ;;
        --mincut-type)     MINCUT_TYPE="$2"; shift 2 ;;
        --seed)            SEED="$2"; shift 2 ;;
        --timeout)         TIMEOUT="$2"; shift 2 ;;
        --n-threads)       N_THREADS="$2"; shift 2 ;;
        --keep-state)      KEEP_STATE="1"; shift ;;
        *) echo "Error [cm/pipeline]: unknown flag $1" >&2; exit 1 ;;
    esac
done

if [ -z "${INPUT_EDGELIST}" ] || [ -z "${BASE_COM}" ] || [ -z "${OUTPUT_DIR}" ] || [ -z "${BASE_ALGO}" ]; then
    echo "Error [cm/pipeline]: --input-edgelist, --base-com, --output-dir, --base-algo required." >&2
    exit 2
fi

case "${BASE_ALGO}" in
    leiden-cpm|leiden-mod|louvain) ;;
    *) echo "Error [cm/pipeline]: --base-algo must be leiden-cpm, leiden-mod, or louvain (got ${BASE_ALGO})." >&2
       exit 2 ;;
esac

if [ "${BASE_ALGO}" = "leiden-cpm" ] && [ -z "${BASE_RESOLUTION}" ]; then
    echo "Error [cm/pipeline]: --base-algo leiden-cpm requires --base-resolution." >&2
    exit 2
fi

if [ ! -x "${BINARY}" ]; then
    echo "Error [cm/pipeline]: constrained_clustering binary not found at ${BINARY}." >&2
    echo "Build via: cd constrained-clustering && bash setup.sh && bash easy_build_and_compile.sh" >&2
    exit 1
fi

CD_STAGE_NAME="cm"
mkdir -p "${OUTPUT_DIR}/.state"
CMD=("${BINARY}" CM
     --mincut-type "${MINCUT_TYPE}"
     --connectedness-criterion "${CM_CRIT}"
     --edgelist "${INPUT_EDGELIST}"
     --existing-clustering "${BASE_COM}"
     --algorithm "${BASE_ALGO}"
     --num-processors "${N_THREADS}"
     --output-file "${OUTPUT_DIR}/com.csv"
     --history-file "${OUTPUT_DIR}/.state/history.log"
     --log-file "${OUTPUT_DIR}/.state/cm.log"
     --log-level 1)
[ -n "${BASE_RESOLUTION}" ] && CMD+=(--clustering-parameter "${BASE_RESOLUTION}")

CD_CMD=("${CMD[@]}")
CD_INPUTS="${INPUT_EDGELIST} ${BASE_COM}"
CD_OUTPUTS="${OUTPUT_DIR}/com.csv"
CD_PARAMS=("criterion=${CM_CRIT}"
           "base_algo=${BASE_ALGO}"
           "base_resolution=${BASE_RESOLUTION}"
           "mincut_type=${MINCUT_TYPE}"
           "n_threads=${N_THREADS}"
           "seed=${SEED}")
CD_EXTRA_LOGS=("${OUTPUT_DIR}/.state/cm.log" "${OUTPUT_DIR}/.state/history.log")

# shellcheck disable=SC1091
source "${SHARED_DIR}/single_stage_pipeline.sh"
