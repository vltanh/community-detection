#!/bin/bash
#
# Single-stage driver for community-detection. Each invocation handles one
# pipeline stage (base clustering OR one post-proc), state-guarded via the
# helpers in src/_common/state.sh (verbatim copy from network-generation).
#
# This is a CD-specific design, not a port of NG's two-stage
# simple_pipeline.sh. Per-algo run_cd.sh dispatcher loops over stages
# (base + optional cc + optional wcc + optional cm), invoking this driver
# once per stage. Each stage owns its own OUTPUT_DIR.
#
# Wrapper contract (must set):
#   CD_STAGE_NAME       short label for logs (e.g. "leiden", "cc", "wcc")
#   OUTPUT_DIR          user-facing output directory; driver writes
#                       OUTPUT_DIR/{pipeline.log, done, params.txt,
#                       time_and_err.log}; CD_CMD writes com.csv (and any
#                       algo-side run.log via setup_logging).
#   CD_CMD              bash array; the command to execute. Must write its
#                       outputs into OUTPUT_DIR.
#   CD_INPUTS           space-separated declared inputs (sha256-tracked).
#                       Anything that, when changed, must invalidate this
#                       stage's cache. Includes upstream com.csv files
#                       for post-proc stages.
#   CD_OUTPUTS          space-separated declared outputs. Typically just
#                       "${OUTPUT_DIR}/com.csv". The driver hashes these
#                       at mark_done time and verifies them at is_step_done.
#   CD_PARAMS           bash array; key=value entries for params.txt
#                       (the fingerprint that participates in CD_INPUTS).
# Optional:
#   TIMEOUT             default 3d
#   SEED                default 0
#   N_THREADS           default 1
#   KEEP_STATE          0|1, default 0. When 0, OUTPUT_DIR keeps user-facing
#                       artifacts only; when 1, also keeps internal logs.
#                       Single-stage shape means there is no .state/ subtree
#                       to wipe; KEEP_STATE controls only whether
#                       time_and_err.log + params.txt survive the run.

set -u

if [ -z "${CD_STAGE_NAME:-}" ] || [ -z "${OUTPUT_DIR:-}" ]; then
    echo "Error [single_stage_pipeline]: wrapper did not set CD_STAGE_NAME or OUTPUT_DIR." >&2
    exit 2
fi

if ! declare -p CD_CMD >/dev/null 2>&1; then
    echo "Error [single_stage_pipeline]: CD_CMD not set." >&2
    exit 2
fi

if [ -z "${CD_INPUTS:-}" ] || [ -z "${CD_OUTPUTS:-}" ]; then
    echo "Error [single_stage_pipeline]: CD_INPUTS or CD_OUTPUTS not set." >&2
    exit 2
fi

: "${TIMEOUT:=3d}"
: "${SEED:=0}"
: "${N_THREADS:=1}"
: "${KEEP_STATE:=0}"

if ! declare -p CD_PARAMS >/dev/null 2>&1; then
    CD_PARAMS=()
fi

SHARED_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
SRC_DIR="$( cd "${SHARED_DIR}/.." && pwd )"
export PYTHONPATH="${SHARED_DIR}${PYTHONPATH:+:${PYTHONPATH}}"

# shellcheck disable=SC1091
source "${SHARED_DIR}/state.sh"

mkdir -p "${OUTPUT_DIR}"

PARAMS_FILE="${OUTPUT_DIR}/params.txt"
DONE_FILE="${OUTPUT_DIR}/done"
LOG_FILE="${OUTPUT_DIR}/pipeline.log"
TIME_LOG="${OUTPUT_DIR}/time_and_err.log"

# Always write params.txt (participates in input hash) and log header.
if [ "${#CD_PARAMS[@]}" -gt 0 ]; then
    write_params_file "${PARAMS_FILE}" "${CD_PARAMS[@]}"
else
    write_params_file "${PARAMS_FILE}" "stage=${CD_STAGE_NAME}"
fi

log_invocation_header "${LOG_FILE}" "${SEED}" "${KEEP_STATE}"

# Inputs hash includes the params.txt so any wrapper-supplied knob change
# invalidates the cache.
EFFECTIVE_INPUTS="${CD_INPUTS} ${PARAMS_FILE}"

if is_step_done "${DONE_FILE}" "${CD_OUTPUTS}"; then
    note_stage_skipped "${TIME_LOG}"
    echo "Skipping ${CD_STAGE_NAME}: valid done-file found at ${DONE_FILE}."
    append_stage_log "${LOG_FILE}" "${CD_STAGE_NAME}" "${TIME_LOG}"
    if [ "${KEEP_STATE}" = "0" ]; then
        rm -f "${TIME_LOG}" "${PARAMS_FILE}"
    fi
    echo "=== ${CD_STAGE_NAME} stage completed (cache hit) ==="
    exit 0
fi

echo "=== Running ${CD_STAGE_NAME} ==="
run_stage "${TIME_LOG}" "${CD_CMD[@]}"
rc=$?
if [ "${rc}" -ne 0 ]; then
    echo "Error [${CD_STAGE_NAME}]: command exited with status ${rc}." >&2
    append_stage_log "${LOG_FILE}" "${CD_STAGE_NAME}" "${TIME_LOG}"
    exit "${rc}"
fi

mark_done "${DONE_FILE}" "${CD_STAGE_NAME}" "${EFFECTIVE_INPUTS}" "${CD_OUTPUTS}"

# Fold per-stage time_and_err.log into pipeline.log, plus the algo-side
# run.log if the algo's setup_logging wrote one (overwritten on each rerun
# so we fold immediately after run, before any later stage clobbers it).
append_stage_log "${LOG_FILE}" "${CD_STAGE_NAME}" "${TIME_LOG}"
if [ -f "${OUTPUT_DIR}/run.log" ] && [ "${OUTPUT_DIR}/run.log" != "${LOG_FILE}" ]; then
    append_stage_log "${LOG_FILE}" "${CD_STAGE_NAME} (python)" "${OUTPUT_DIR}/run.log"
fi

if [ "${KEEP_STATE}" = "0" ]; then
    rm -f "${TIME_LOG}" "${PARAMS_FILE}"
fi

echo "=== ${CD_STAGE_NAME} stage completed successfully ==="
echo "Output: ${OUTPUT_DIR}/com.csv"
