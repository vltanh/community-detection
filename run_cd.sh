#!/bin/bash
#
# Community-detection dispatcher. Parses CLI, routes paths, gates per-algo
# post-procs, recurses for *-best dependencies, then invokes per-algo +
# per-post-proc pipeline.sh wrappers (which source single_stage_pipeline.sh
# under src/_common/ for state-guarded execution).

# Determinism env (mirrors network-generation; pins set/dict iteration order
# and OpenMP thread count for byte-reproducible runs).
export PYTHONHASHSEED=0
export OMP_NUM_THREADS=1

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" &> /dev/null && pwd)"
if [[ "${SCRIPT_DIR}" == *"/slurmd/job"* ]]; then
    SCRIPT_DIR="${SLURM_SUBMIT_DIR}"
fi

CC_BINARY="${SCRIPT_DIR}/constrained-clustering/constrained_clustering"

log() {
    builtin echo "[$(date +'%Y-%m-%d %H:%M:%S')] $*"
}

# ==========================================
# Argument Parsing
# ==========================================
algo=""
network_id=""
is_real=0
is_synthetic=0
generator=""
gt_clustering=""
run_id=""
criterion=""

custom_input=""
custom_out_dir=""
custom_gt=""

run_stats_flag=0
run_acc_flag=0
run_cc_flag=0
run_wcc_flag=0
run_cm_flag=0
keep_state_flag=0

TIMEOUT="7d"
N_THREADS="1"
SEED="1"

while [[ "$#" -gt 0 ]]; do
    case $1 in
        --algo) algo="$2"; shift 2 ;;
        --network) network_id="$2"; shift 2 ;;
        --real) is_real=1; shift 1 ;;
        --synthetic) is_synthetic=1; shift 1 ;;
        --generator) generator="$2"; shift 2 ;;
        --gt-clustering-id) gt_clustering="$2"; shift 2 ;;
        --run-id) run_id="$2"; shift 2 ;;
        --criterion) criterion="$2"; shift 2 ;;
        --input-edgelist) custom_input="$2"; shift 2 ;;
        --output-dir) custom_out_dir="$2"; shift 2 ;;
        --input-gt-clustering) custom_gt="$2"; shift 2 ;;
        --run-stats) run_stats_flag=1; shift 1 ;;
        --run-acc) run_acc_flag=1; shift 1 ;;
        --run-cc) run_cc_flag=1; shift 1 ;;
        --run-wcc) run_wcc_flag=1; shift 1 ;;
        --run-cm) run_cm_flag=1; shift 1 ;;
        --keep-state) keep_state_flag=1; shift 1 ;;
        --timeout) TIMEOUT="$2"; shift 2 ;;
        --n-threads) N_THREADS="$2"; shift 2 ;;
        --seed) SEED="$2"; shift 2 ;;
        -*) log "Unknown parameter: $1"; exit 1 ;;
        *) log "Unexpected argument: $1"; exit 1 ;;
    esac
done

if [ -z "${algo}" ]; then
    log "Error: --algo is a required parameter."
    exit 1
fi

# ==========================================
# Connectedness Criterion Configuration
# ==========================================
CRIT_SUFFIX=""
WCC_CRIT="1log_10(n)"
CM_CRIT="0.2n^0.5"

if [[ -n "${criterion}" ]]; then
    CRIT_SUFFIX="(${criterion})"
    case "${criterion}" in
        sqrt) WCC_CRIT="0.2n^0.5"; CM_CRIT="0.2n^0.5" ;;
        log)  WCC_CRIT="1log_10(n)"; CM_CRIT="1log_10(n)" ;;
        *)    WCC_CRIT="${criterion}"; CM_CRIT="${criterion}" ;;
    esac
fi

# ==========================================
# Post-Processing Gate Matrix (per-algo)
# ==========================================
IS_RUN_CC=$run_cc_flag
IS_RUN_WCC=$run_wcc_flag
IS_RUN_CM=$run_cm_flag

case ${algo} in
    ikc*)
        [ "$IS_RUN_CC" -eq 1 ] && log "Warning: CC is not necessary for IKC. Disabling."
        ([ "$IS_RUN_WCC" -eq 1 ] || [ "$IS_RUN_CM" -eq 1 ]) && log "Warning: WCC, and CM are not supported for IKC. Disabling."
        IS_RUN_CC=0; IS_RUN_WCC=0; IS_RUN_CM=0
        ;;
    leiden*)
        [ "$IS_RUN_CC" -eq 1 ] && log "Warning: CC is not necessary for Leiden. Disabling."
        [ "$IS_RUN_WCC" -eq 1 ] && log "Warning: WCC is not supported for Leiden. Disabling."
        IS_RUN_CC=0; IS_RUN_WCC=0
        ;;
    sbm*)
        [ "$IS_RUN_CM" -eq 1 ] && log "Warning: CM is not supported for SBM. Disabling."
        IS_RUN_CM=0
        ;;
    infomap)
        ([ "$IS_RUN_WCC" -eq 1 ] || [ "$IS_RUN_CM" -eq 1 ]) && log "Warning: WCC and CM are not supported for Infomap. Disabling."
        IS_RUN_WCC=0; IS_RUN_CM=0
        ;;
esac

# ==========================================
# Input/Output Path Routing
# ==========================================
has_gt=0
gt_file=""
dataset_type=""

if [ "${is_real}" -eq 1 ]; then
    [ -z "${network_id}" ] && { log "Error: --network required for --real."; exit 1; }
    custom_input="${SCRIPT_DIR}/data/empirical_networks/networks/${network_id}/${network_id}.csv"
    custom_out_dir="${SCRIPT_DIR}/data/reference_clusterings"
    generator=""
    gt_clustering=""
    run_id=""
    dataset_type="real"
fi

if [ "${is_synthetic}" -eq 1 ]; then
    if [ -z "${network_id}" ] || [ -z "${generator}" ] || [ -z "${gt_clustering}" ]; then
        log "Error: --network, --generator, and --gt-clustering-id required for --synthetic."
        exit 1
    fi
    run_id="${run_id:-0}"
    custom_input="${SCRIPT_DIR}/data/synthetic_networks/networks/${generator}/${gt_clustering}/${network_id}/${run_id}/edge.csv"
    custom_out_dir="${SCRIPT_DIR}/data/estimated_clusterings"
    custom_gt="${SCRIPT_DIR}/data/reference_clusterings/clusterings/${gt_clustering}/${network_id}/com.csv"
    dataset_type="${generator}/${gt_clustering} (run: ${run_id})"
fi

if [ -n "${custom_input}" ]; then
    [ -z "${custom_out_dir}" ] && { log "Error: --output-dir must be provided."; exit 1; }
    inp_edge="${custom_input}"
    COMMDET_BASE="${custom_out_dir}${generator:+/${generator}}${gt_clustering:+/${gt_clustering}}"
    opt_subpath="${network_id:+/${network_id}}${run_id:+/${run_id}}"
    if [ -z "${dataset_type}" ]; then
        dataset_type="[Custom]"
        [ -n "${network_id}" ] && dataset_type="${dataset_type} ${network_id}"
        [ -n "${generator}" ] && dataset_type="${dataset_type} (Gen: ${generator})"
        [ -n "${gt_clustering}" ] && dataset_type="${dataset_type} (GT: ${gt_clustering})"
        [ -n "${run_id}" ] && dataset_type="${dataset_type} (Run: ${run_id})"
    fi
    [ -n "${custom_gt}" ] && { gt_file="${custom_gt}"; has_gt=1; }
else
    log "Error: You must specify --real, --synthetic, or provide --input-edgelist + --output-dir."
    exit 1
fi

[ ! -f "${inp_edge}" ] && { log "Input file ${inp_edge} does not exist. Skipping."; exit 1; }

base_root_clusterings="${COMMDET_BASE}/clusterings"
base_root_stats="${COMMDET_BASE}/stats"
base_root_acc="${COMMDET_BASE}/acc"

# ==========================================
# Helpers
# ==========================================
keep_state_arg=()
[ "${keep_state_flag}" -eq 1 ] && keep_state_arg=(--keep-state)

run_stats() {
    [ "${run_stats_flag}" -eq 0 ] && return
    local edge_file=$1; local com_file=$2; local stats_dir=$3

    log "Evaluating stats state via Python StateTracker..."
    mkdir -p "${stats_dir}"

    { /usr/bin/time -v python "${SCRIPT_DIR}/network_evaluation/network_stats/compute_cluster_stats.py" \
        --network "${edge_file}" \
        --community "${com_file}" \
        --outdir "${stats_dir}"; } 2> "${stats_dir}/error.log"

    if [ ${?} -ne 0 ]; then
        log "ERROR: Stats computation failed for ${stats_dir}."
    else
        log "Stats evaluation complete for ${stats_dir}."
    fi
}

run_accuracy() {
    [ "${run_acc_flag}" -eq 0 ] && return
    if [ "${has_gt}" -eq 0 ]; then
        log "Warning: Accuracy evaluation requested but no ground-truth provided. Skipping."
        return
    fi
    local edge_file=$1; local gt_f=$2; local est_file=$3; local acc_d=$4

    log "Evaluating accuracy state via Python StateTracker..."
    mkdir -p "${acc_d}"

    { /usr/bin/time -v python "${SCRIPT_DIR}/network_evaluation/commdet_acc/compute_cd_accuracy.py" \
        --input-network "${edge_file}" \
        --gt-clustering "${gt_f}" \
        --est-clustering "${est_file}" \
        --output-prefix "${acc_d}/result"; } 2> "${acc_d}/error.log"

    if [ ${?} -ne 0 ]; then
        log "ERROR: Accuracy computation failed for ${acc_d}."
    else
        log "Accuracy evaluation complete for ${acc_d}."
    fi
}

run_dependency() {
    local target_algo="$1"
    log "--> Triggering dependency evaluation for: ${target_algo}"

    local cmd=("bash" "${BASH_SOURCE[0]}" "--algo" "${target_algo}")
    [[ ${is_real} -eq 1 ]] && cmd+=("--real")
    [[ ${is_synthetic} -eq 1 ]] && cmd+=("--synthetic")
    [[ -n "${network_id}" ]] && cmd+=("--network" "${network_id}")
    [[ -n "${generator}" ]] && cmd+=("--generator" "${generator}")
    [[ -n "${gt_clustering}" ]] && cmd+=("--gt-clustering-id" "${gt_clustering}")
    [[ -n "${run_id}" ]] && cmd+=("--run-id" "${run_id}")
    [[ -n "${criterion}" ]] && cmd+=("--criterion" "${criterion}")
    [[ -n "${custom_input}" ]] && cmd+=("--input-edgelist" "${custom_input}")
    [[ -n "${custom_out_dir}" ]] && cmd+=("--output-dir" "${custom_out_dir}")
    [[ -n "${custom_gt}" ]] && cmd+=("--input-gt-clustering" "${custom_gt}")
    [[ -n "${TIMEOUT}" ]] && cmd+=("--timeout" "${TIMEOUT}")
    [[ -n "${N_THREADS}" ]] && cmd+=("--n-threads" "${N_THREADS}")
    [[ -n "${SEED}" ]] && cmd+=("--seed" "${SEED}")
    [[ ${run_stats_flag} -eq 1 ]] && cmd+=("--run-stats")
    [[ ${run_acc_flag} -eq 1 ]] && cmd+=("--run-acc")
    [[ ${run_cc_flag} -eq 1 ]] && cmd+=("--run-cc")
    [[ ${run_wcc_flag} -eq 1 ]] && cmd+=("--run-wcc")
    [[ ${run_cm_flag} -eq 1 ]] && cmd+=("--run-cm")
    [[ ${keep_state_flag} -eq 1 ]] && cmd+=("--keep-state")

    "${cmd[@]}" || { log "CRITICAL: Dependency ${target_algo} failed."; exit 1; }
}

# ==========================================
# Resolve Meta-Model Dependencies First
# ==========================================
case "${algo}" in
    sbm-flat-best)
        log "Resolving downstream states for flat-best dependencies..."
        for dep in sbm-flat-dc sbm-flat-ndc sbm-flat-pp; do run_dependency "${dep}"; done
        ;;
    sbm-nested-best)
        log "Resolving downstream states for nested-best dependencies..."
        for dep in sbm-nested-dc sbm-nested-ndc; do run_dependency "${dep}"; done
        ;;
esac

log "============================"
log "${algo} ${network_id:-[Custom]} | Dataset: ${dataset_type}"

# ==========================================
# Algorithm decode
# ==========================================
leiden_model=""
leiden_res=""
ikc_k=""
sbm_model=""

if [[ ${algo} == leiden* ]]; then
    leiden_model=$(echo "${algo}" | cut -d'-' -f2)
    [[ ${leiden_model} == cpm ]] && leiden_res=$(echo "${algo}" | cut -d'-' -f3)
elif [[ ${algo} == ikc* ]]; then
    ikc_k=$(echo "${algo}" | cut -d'-' -f2)
elif [[ ${algo} == sbm* ]]; then
    sbm_model=$(echo "${algo}" | cut -d'-' -f2-)
fi

base_dir="${base_root_clusterings}/${algo}${opt_subpath}"
stats_dir="${base_root_stats}/${algo}${opt_subpath}"
acc_dir="${base_root_acc}/${algo}${opt_subpath}"
base_com="${base_dir}/com.csv"

# ==========================================
# Base Stage: invoke per-algo wrapper
# ==========================================
case "${algo}" in
    leiden-cpm-*)
        bash "${SCRIPT_DIR}/src/leiden/pipeline.sh" \
            --input-edgelist "${inp_edge}" \
            --output-dir "${base_dir}" \
            --model cpm --resolution "${leiden_res}" \
            --seed "${SEED}" --timeout "${TIMEOUT}" --n-threads "${N_THREADS}" \
            "${keep_state_arg[@]}"
        ;;
    leiden-mod)
        bash "${SCRIPT_DIR}/src/leiden/pipeline.sh" \
            --input-edgelist "${inp_edge}" \
            --output-dir "${base_dir}" \
            --model mod \
            --seed "${SEED}" --timeout "${TIMEOUT}" --n-threads "${N_THREADS}" \
            "${keep_state_arg[@]}"
        ;;
    infomap)
        bash "${SCRIPT_DIR}/src/infomap/pipeline.sh" \
            --input-edgelist "${inp_edge}" \
            --output-dir "${base_dir}" \
            --seed "${SEED}" --timeout "${TIMEOUT}" --n-threads "${N_THREADS}" \
            "${keep_state_arg[@]}"
        ;;
    ikc-*)
        bash "${SCRIPT_DIR}/src/ikc/pipeline.sh" \
            --input-edgelist "${inp_edge}" \
            --output-dir "${base_dir}" \
            --k "${ikc_k}" \
            --seed "${SEED}" --timeout "${TIMEOUT}" --n-threads "${N_THREADS}" \
            "${keep_state_arg[@]}"
        ;;
    sbm-flat-dc|sbm-flat-ndc|sbm-flat-pp|sbm-nested-dc|sbm-nested-ndc)
        bash "${SCRIPT_DIR}/src/sbm/pipeline.sh" \
            --input-edgelist "${inp_edge}" \
            --output-dir "${base_dir}" \
            --method "${sbm_model}" \
            --seed "${SEED}" --timeout "${TIMEOUT}" --n-threads "${N_THREADS}" \
            "${keep_state_arg[@]}"
        ;;
    sbm-flat-best)
        bash "${SCRIPT_DIR}/src/sbm/flat_best_pipeline.sh" \
            --output-dir "${base_dir}" \
            --dc-dir  "${base_root_clusterings}/sbm-flat-dc${opt_subpath}" \
            --ndc-dir "${base_root_clusterings}/sbm-flat-ndc${opt_subpath}" \
            --pp-dir  "${base_root_clusterings}/sbm-flat-pp${opt_subpath}" \
            --seed "${SEED}" --timeout "${TIMEOUT}" \
            "${keep_state_arg[@]}"
        ;;
    sbm-nested-best)
        bash "${SCRIPT_DIR}/src/sbm/nested_best_pipeline.sh" \
            --output-dir "${base_dir}" \
            --dc-dir  "${base_root_clusterings}/sbm-nested-dc${opt_subpath}" \
            --ndc-dir "${base_root_clusterings}/sbm-nested-ndc${opt_subpath}" \
            --seed "${SEED}" --timeout "${TIMEOUT}" \
            "${keep_state_arg[@]}"
        ;;
    *)
        log "Unknown algorithm: ${algo}"; exit 1
        ;;
esac

[ ! -f "${base_com}" ] && { log "CRITICAL: Base clustering ${base_com} missing."; exit 1; }

# ==========================================
# Symlink Shortcut for *-best Meta-Models
# ==========================================
if [[ "${algo}" == "sbm-flat-best" || "${algo}" == "sbm-nested-best" ]]; then
    best_model_file="${base_dir}/best_model.txt"
    [ ! -f "${best_model_file}" ] && { log "Error: ${best_model_file} not found."; exit 1; }
    best_algo=$(cat "${best_model_file}")
    [ "${best_algo#sbm-}" = "${best_algo}" ] && best_algo="sbm-${best_algo}"

    log "Symlinking downstream folders for: ${best_algo}..."

    link_target_dir() {
        local subroot="$1"
        local target="${subroot}/${best_algo}${opt_subpath}"
        local link="${subroot}/${algo}${opt_subpath}"
        [ -L "${link}" ] && rm "${link}"
        if [ -e "${target}" ]; then
            mkdir -p "$(dirname "${link}")"
            ln -sfn "$(realpath "${target}")" "${link}"
        else
            log "Warning: ${target} not found; skipping symlink."
        fi
    }

    if [ "${run_stats_flag}" -eq 1 ]; then link_target_dir "${base_root_stats}"; fi
    if [ "${run_acc_flag}" -eq 1 ] && [ "${has_gt}" -eq 1 ]; then link_target_dir "${base_root_acc}"; fi

    for base_pp in cc wcc cm; do
        case "${base_pp}" in
            cc)  [ "${IS_RUN_CC}"  -eq 1 ] || continue; pp_tag="cc" ;;
            wcc) [ "${IS_RUN_WCC}" -eq 1 ] || continue; pp_tag="wcc${CRIT_SUFFIX}" ;;
            cm)  [ "${IS_RUN_CM}"  -eq 1 ] || continue; pp_tag="cm${CRIT_SUFFIX}" ;;
        esac
        # The clustering symlinks use +pp_tag suffix on both target and link.
        link_pp_suffix() {
            local subroot="$1"
            local target="${subroot}/${best_algo}+${pp_tag}${opt_subpath}"
            local link="${subroot}/${algo}+${pp_tag}${opt_subpath}"
            [ -L "${link}" ] && rm "${link}"
            if [ -e "${target}" ]; then
                mkdir -p "$(dirname "${link}")"
                ln -sfn "$(realpath "${target}")" "${link}"
            else
                log "Warning: ${target} not found; skipping symlink."
            fi
        }
        link_pp_suffix "${base_root_clusterings}"
        [ "${run_stats_flag}" -eq 1 ] && link_pp_suffix "${base_root_stats}"
        [ "${run_acc_flag}" -eq 1 ] && [ "${has_gt}" -eq 1 ] && link_pp_suffix "${base_root_acc}"
    done

    log "[cd] ${algo} ${network_id:-[Custom]} ${dataset_type} ${criterion}" >> "${SCRIPT_DIR}/complete.log"
    exit 0
fi

run_stats "${inp_edge}" "${base_com}" "${stats_dir}"
run_accuracy "${inp_edge}" "${gt_file}" "${base_com}" "${acc_dir}"

# ==========================================
# Post-Proc Stages: invoke wrappers
# ==========================================
run_postproc() {
    local stage="$1"; local crit="$2"; local out_dir="$3"; local stats_d="$4"; local acc_d="$5"
    local pp_com="${out_dir}/com.csv"

    case "${stage}" in
        cc)
            bash "${SCRIPT_DIR}/src/cc/pipeline.sh" \
                --input-edgelist "${inp_edge}" --base-com "${base_com}" \
                --output-dir "${out_dir}" --binary "${CC_BINARY}" \
                --seed "${SEED}" --timeout "${TIMEOUT}" --n-threads "${N_THREADS}" \
                "${keep_state_arg[@]}"
            ;;
        wcc)
            bash "${SCRIPT_DIR}/src/wcc/pipeline.sh" \
                --input-edgelist "${inp_edge}" --base-com "${base_com}" \
                --output-dir "${out_dir}" --binary "${CC_BINARY}" \
                --criterion "${crit}" \
                --seed "${SEED}" --timeout "${TIMEOUT}" --n-threads "${N_THREADS}" \
                "${keep_state_arg[@]}"
            ;;
        cm)
            local cm_args=(--input-edgelist "${inp_edge}" --base-com "${base_com}"
                           --output-dir "${out_dir}" --binary "${CC_BINARY}"
                           --criterion "${crit}"
                           --seed "${SEED}" --timeout "${TIMEOUT}" --n-threads "${N_THREADS}")
            if [[ ${algo} == leiden-cpm-* ]]; then
                cm_args+=(--base-algo leiden-cpm --base-resolution "${leiden_res}")
            elif [[ ${algo} == leiden-mod ]]; then
                cm_args+=(--base-algo leiden-mod)
            else
                log "CM not implemented for ${algo}; skipping."
                return
            fi
            bash "${SCRIPT_DIR}/src/cm/pipeline.sh" "${cm_args[@]}" "${keep_state_arg[@]}"
            ;;
    esac

    if [ -f "${pp_com}" ]; then
        run_stats "${inp_edge}" "${pp_com}" "${stats_d}"
        run_accuracy "${inp_edge}" "${gt_file}" "${pp_com}" "${acc_d}"
    fi
}

[ "${IS_RUN_CC}" -eq 1 ] && run_postproc cc "0" \
    "${base_root_clusterings}/${algo}+cc${opt_subpath}" \
    "${base_root_stats}/${algo}+cc${opt_subpath}" \
    "${base_root_acc}/${algo}+cc${opt_subpath}"

[ "${IS_RUN_WCC}" -eq 1 ] && run_postproc wcc "${WCC_CRIT}" \
    "${base_root_clusterings}/${algo}+wcc${CRIT_SUFFIX}${opt_subpath}" \
    "${base_root_stats}/${algo}+wcc${CRIT_SUFFIX}${opt_subpath}" \
    "${base_root_acc}/${algo}+wcc${CRIT_SUFFIX}${opt_subpath}"

[ "${IS_RUN_CM}" -eq 1 ] && run_postproc cm "${CM_CRIT}" \
    "${base_root_clusterings}/${algo}+cm${CRIT_SUFFIX}${opt_subpath}" \
    "${base_root_stats}/${algo}+cm${CRIT_SUFFIX}${opt_subpath}" \
    "${base_root_acc}/${algo}+cm${CRIT_SUFFIX}${opt_subpath}"

log "[cd] ${algo} ${network_id:-[Custom]} ${dataset_type} ${criterion}" >> "${SCRIPT_DIR}/complete.log"
