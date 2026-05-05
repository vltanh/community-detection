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
#                       OUTPUT_DIR/{run.log, done, params.txt}.
#                       run.log is the only log artifact (invocation header
#                       + /usr/bin/time -v stderr trace + python stdout
#                       trace, all concatenated). CD_CMD writes com.csv
#                       (+ any aux outputs) directly to OUTPUT_DIR.
#   CD_CMD              bash array; the command to execute. Must write its
#                       outputs into OUTPUT_DIR. CMD should accept
#                       --log-file <path> and route python logging there;
#                       wrapper passes the per-stage path automatically.
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
#   CD_LOG_FILE_FLAG    flag name to pass python log path through, default
#                       "--log-file". Set empty to skip auto-injection (the
#                       wrapper's CD_CMD already names the path itself).
#   CD_EXTRA_LOGS       array of extra log files the wrapper's command
#                       writes (e.g. constrained_clustering's
#                       OUTPUT_DIR/cc.log). Driver folds each into
#                       run.log then removes it after the stage finishes.
#   TIMEOUT             default 3d
#   SEED                default 0
#   N_THREADS           default 1
#   KEEP_STATE          0|1, default 0. Reserved; the single-stage CD design
#                       has no intermediate state to gate.
#
# Per-leaf layout:
#   OUTPUT_DIR/
#     com.csv                            # algo output
#     done                               # hash manifest
#     params.txt                         # fingerprint
#     run.log                            # only log: invocation header +
#                                        # /usr/bin/time -v stderr trace +
#                                        # python stdout trace
#
# Per-stage shell + python logs are written to a temp dir and concatenated
# into run.log; the temp dir is removed on exit. CD is single-stage; there
# is no .state/<stage>/ subtree in the user-facing output.

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
: "${CD_LOG_FILE_FLAG:=--log-file}"

if ! declare -p CD_PARAMS >/dev/null 2>&1; then
    CD_PARAMS=()
fi

if ! declare -p CD_EXTRA_LOGS >/dev/null 2>&1; then
    CD_EXTRA_LOGS=()
fi

SHARED_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
SRC_DIR="$( cd "${SHARED_DIR}/.." && pwd )"
export PYTHONPATH="${SHARED_DIR}${PYTHONPATH:+:${PYTHONPATH}}"

# shellcheck disable=SC1091
source "${SHARED_DIR}/state.sh"

mkdir -p "${OUTPUT_DIR}"

PARAMS_FILE="${OUTPUT_DIR}/params.txt"
DONE_FILE="${OUTPUT_DIR}/done"
LOG_FILE="${OUTPUT_DIR}/run.log"

# Transient per-stage traces; folded into LOG_FILE then dropped.
STAGE_TMP_DIR=$(mktemp -d "${TMPDIR:-/tmp}/cd-stage.XXXXXX")
trap 'rm -rf "${STAGE_TMP_DIR}"' EXIT
STAGE_TIME_LOG="${STAGE_TMP_DIR}/time_and_err.log"
STAGE_RUN_LOG="${STAGE_TMP_DIR}/run.log"

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
    note_stage_skipped "${STAGE_TIME_LOG}"
    echo "Skipping ${CD_STAGE_NAME}: valid done-file found at ${DONE_FILE}."
    append_stage_log "${LOG_FILE}" "${CD_STAGE_NAME}" "${STAGE_TIME_LOG}"
    echo "=== ${CD_STAGE_NAME} stage completed (cache hit) ==="
    exit 0
fi

# Inject --log-file <STAGE_RUN_LOG> into CD_CMD unless the wrapper opted out
# (CD_LOG_FILE_FLAG empty) or already named one itself.
if [ -n "${CD_LOG_FILE_FLAG}" ]; then
    has_log_flag=0
    for tok in "${CD_CMD[@]}"; do
        if [ "${tok}" = "${CD_LOG_FILE_FLAG}" ]; then
            has_log_flag=1
            break
        fi
    done
    if [ "${has_log_flag}" -eq 0 ]; then
        CD_CMD+=("${CD_LOG_FILE_FLAG}" "${STAGE_RUN_LOG}")
    fi
fi

echo "=== Running ${CD_STAGE_NAME} ==="
run_stage "${STAGE_TIME_LOG}" "${CD_CMD[@]}"
rc=$?
if [ "${rc}" -ne 0 ]; then
    echo "Error [${CD_STAGE_NAME}]: command exited with status ${rc}." >&2
    append_stage_log "${LOG_FILE}" "${CD_STAGE_NAME}" "${STAGE_TIME_LOG}"
    exit "${rc}"
fi

mark_done "${DONE_FILE}" "${CD_STAGE_NAME}" "${EFFECTIVE_INPUTS}" "${CD_OUTPUTS}"

# Consolidate per-stage logs into the top-level run.log. Shell-wrapper trace
# first, then python trace (when present), then any wrapper-declared extras
# (e.g. constrained_clustering's cc.log / cm.log). Temp dir is wiped via the
# EXIT trap; extra log files are deleted after fold-in. Leaf ends up with
# only run.log + com.csv + done + params.txt.
append_stage_log "${LOG_FILE}" "${CD_STAGE_NAME}" "${STAGE_TIME_LOG}"
if [ -f "${STAGE_RUN_LOG}" ]; then
    append_stage_log "${LOG_FILE}" "${CD_STAGE_NAME} (python)" "${STAGE_RUN_LOG}"
fi
for extra_log in "${CD_EXTRA_LOGS[@]}"; do
    [ -f "${extra_log}" ] || continue
    extra_label="$(basename "${extra_log}" .log)"
    append_stage_log "${LOG_FILE}" "${CD_STAGE_NAME} (${extra_label})" "${extra_log}"
    rm -f "${extra_log}"
done

echo "=== ${CD_STAGE_NAME} stage completed successfully ==="
echo "Output: ${OUTPUT_DIR}/com.csv"
