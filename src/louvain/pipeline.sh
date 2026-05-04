#!/bin/bash
#
# Louvain (gen-louvain) wrapper. Invoked by run_cd.sh dispatcher; sources
# single_stage_pipeline.sh.
#
# Calls run_louvain.py which shells out to the upstream gen-louvain trio
# (convert + louvain + hierarchy). Reproducibility relies on the LD_PRELOAD
# shim under src/louvain/seed_shim/ overriding srand at load time; upstream
# source is never modified.

set -u

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
if [[ "${SCRIPT_DIR}" == *"/slurmd/job"* ]]; then
    SCRIPT_DIR="${SLURM_SUBMIT_DIR}/src/louvain"
fi
SHARED_DIR="$( cd "${SCRIPT_DIR}/../_common" && pwd )"
REPO_ROOT="$( cd "${SCRIPT_DIR}/../.." && pwd )"

INPUT_EDGELIST=""
OUTPUT_DIR=""
LOUVAIN_QUALITY=""
LOUVAIN_ALPHA=""
LOUVAIN_KMIN=""
LOUVAIN_EPSILON=""
LOUVAIN_DIR="${REPO_ROOT}/externals/louvain"
SEED_PRELOAD="${SCRIPT_DIR}/seed_shim/louvain_seed_preload.so"
SEED="1"
TIMEOUT="3d"
N_THREADS="1"
KEEP_STATE="0"

while [ $# -gt 0 ]; do
    case "$1" in
        --input-edgelist)  INPUT_EDGELIST="$2"; shift 2 ;;
        --output-dir)      OUTPUT_DIR="$2"; shift 2 ;;
        --quality)         LOUVAIN_QUALITY="$2"; shift 2 ;;
        --alpha)           LOUVAIN_ALPHA="$2"; shift 2 ;;
        --kmin)            LOUVAIN_KMIN="$2"; shift 2 ;;
        --epsilon)         LOUVAIN_EPSILON="$2"; shift 2 ;;
        --louvain-dir)     LOUVAIN_DIR="$2"; shift 2 ;;
        --seed-preload)    SEED_PRELOAD="$2"; shift 2 ;;
        --seed)            SEED="$2"; shift 2 ;;
        --timeout)         TIMEOUT="$2"; shift 2 ;;
        --n-threads)       N_THREADS="$2"; shift 2 ;;
        --keep-state)      KEEP_STATE="1"; shift ;;
        *) echo "Error [louvain/pipeline]: unknown flag $1" >&2; exit 1 ;;
    esac
done

if [ -z "${INPUT_EDGELIST}" ] || [ -z "${OUTPUT_DIR}" ] || [ -z "${LOUVAIN_QUALITY}" ]; then
    echo "Error [louvain/pipeline]: --input-edgelist, --output-dir, --quality required." >&2
    exit 2
fi

case "${LOUVAIN_QUALITY}" in
    mod|zahn|owzad|goldberg|condora|devind|devuni|dp|shimalik|balmod) ;;
    *) echo "Error [louvain/pipeline]: --quality must be one of mod/zahn/owzad/goldberg/condora/devind/devuni/dp/shimalik/balmod (got ${LOUVAIN_QUALITY})." >&2
       exit 2 ;;
esac

if [ ! -x "${LOUVAIN_DIR}/louvain" ]; then
    echo "Error [louvain/pipeline]: louvain binary not found at ${LOUVAIN_DIR}/louvain." >&2
    echo "Build via: cd ${LOUVAIN_DIR} && make" >&2
    exit 1
fi
if [ ! -f "${SEED_PRELOAD}" ]; then
    echo "Error [louvain/pipeline]: seed shim not found at ${SEED_PRELOAD}." >&2
    echo "Build via: bash ${SCRIPT_DIR}/seed_shim/build.sh" >&2
    exit 1
fi

CD_STAGE_NAME="louvain-${LOUVAIN_QUALITY}"
[ "${LOUVAIN_QUALITY}" = "owzad" ] && [ -n "${LOUVAIN_ALPHA}" ] && CD_STAGE_NAME="${CD_STAGE_NAME}-${LOUVAIN_ALPHA}"
[ "${LOUVAIN_QUALITY}" = "shimalik" ] && [ -n "${LOUVAIN_KMIN}" ] && CD_STAGE_NAME="${CD_STAGE_NAME}-${LOUVAIN_KMIN}"

CMD=(python "${SCRIPT_DIR}/run_louvain.py"
     --edgelist "${INPUT_EDGELIST}"
     --output-directory "${OUTPUT_DIR}"
     --quality "${LOUVAIN_QUALITY}"
     --louvain-bin-dir "${LOUVAIN_DIR}"
     --seed-preload "${SEED_PRELOAD}"
     --seed "${SEED}")
[ -n "${LOUVAIN_ALPHA}" ] && CMD+=(--alpha "${LOUVAIN_ALPHA}")
[ -n "${LOUVAIN_KMIN}" ] && CMD+=(--kmin "${LOUVAIN_KMIN}")
[ -n "${LOUVAIN_EPSILON}" ] && CMD+=(--epsilon "${LOUVAIN_EPSILON}")

CD_CMD=("${CMD[@]}")
CD_INPUTS="${INPUT_EDGELIST}"
CD_OUTPUTS="${OUTPUT_DIR}/com.csv"
CD_PARAMS=("quality=${LOUVAIN_QUALITY}"
           "alpha=${LOUVAIN_ALPHA}"
           "kmin=${LOUVAIN_KMIN}"
           "epsilon=${LOUVAIN_EPSILON}"
           "seed=${SEED}")

# shellcheck disable=SC1091
source "${SHARED_DIR}/single_stage_pipeline.sh"
