#!/bin/bash

# Constants
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" &> /dev/null && pwd)"

# ==========================================
# Helper Functions: Logging & State Tracking
# ==========================================
log() {
    builtin echo "[$(date +'%Y-%m-%d %H:%M:%S')] $*"
}

is_step_done() {
    local done_file="$1"
    if [ ! -f "${done_file}" ]; then return 1; fi
    if ! sha256sum --status -c "${done_file}" 2>/dev/null; then
        log "State change detected. Recomputing..."
        return 1
    fi
    return 0
}

mark_done() {
    local done_file="$1"
    local stage_name="$2"
    read -r -a inputs <<< "$3"
    local out_dir="$4"

    local tmp_done="${done_file}.tmp.$$"

    sha256sum "${inputs[@]}" > "${tmp_done}"
    find "${out_dir}" -maxdepth 1 -type f ! -name "$(basename "${done_file}")" ! -name "$(basename "${tmp_done}")" -exec sha256sum {} + >> "${tmp_done}"

    mv "${tmp_done}" "${done_file}"
    log "Success [${stage_name}]: I/O hashes recorded atomically."
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

TIMEOUT="7d"

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
        --timeout) TIMEOUT="$2"; shift 2 ;;
        -*) log "Unknown parameter passed: $1"; exit 1 ;;
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
WCC_CRIT="1log_10(n)" # Default
CM_CRIT="0.2n^0.5"    # Default

if [[ -n "${criterion}" ]]; then
    CRIT_SUFFIX="(${criterion})"
    if [[ "${criterion}" == "sqrt" ]]; then
        WCC_CRIT="0.2n^0.5"; CM_CRIT="0.2n^0.5"
    elif [[ "${criterion}" == "log" ]]; then
        WCC_CRIT="1log_10(n)"; CM_CRIT="1log_10(n)"
    else
        WCC_CRIT="${criterion}"; CM_CRIT="${criterion}"
    fi
fi

# ==========================================
# Post-Processing Flags & Constraints
# ==========================================
IS_RUN_CC=$run_cc_flag
IS_RUN_WCC=$run_wcc_flag
IS_RUN_CM=$run_cm_flag

case ${algo} in
    ikc*)
        if [ "$IS_RUN_CC" -eq 1 ]; then log "Warning: CC is not necessary for IKC. Disabling."; fi
        if [ "$IS_RUN_WCC" -eq 1 ] || [ "$IS_RUN_CM" -eq 1 ]; then log "Warning: WCC, and CM are not supported for IKC. Disabling."; fi
        IS_RUN_CC=0; IS_RUN_WCC=0; IS_RUN_CM=0
        ;;
    leiden*)
        if [ "$IS_RUN_CC" -eq 1 ]; then log "Warning: CC is not necessary for Leiden. Disabling."; fi
        if [ "$IS_RUN_WCC" -eq 1 ]; then log "Warning: WCC is not supported for Leiden. Disabling."; fi
        IS_RUN_CC=0; IS_RUN_WCC=0
        ;;
    sbm*)
        if [ "$IS_RUN_CM" -eq 1 ]; then log "Warning: CM is not supported for SBM. Disabling."; fi
        IS_RUN_CM=0
        ;;
    infomap)
        if [ "$IS_RUN_WCC" -eq 1 ] || [ "$IS_RUN_CM" -eq 1 ]; then log "Warning: WCC and CM are not supported for Infomap. Disabling."; fi
        IS_RUN_WCC=0; IS_RUN_CM=0
        ;;
esac

# ==========================================
# Input/Output Path Routing (Unified)
# ==========================================
has_gt=0

if [ "${is_real}" -eq 1 ]; then
    if [ -z "${network_id}" ]; then log "Error: --network required for --real."; exit 1; fi
    custom_input="data/empirical_networks/networks/${network_id}/${network_id}.csv"
    custom_out_dir="data/reference_clusterings"
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
    custom_input="data/synthetic_networks/networks/${generator}/${gt_clustering}/${network_id}/${run_id}/edge.csv"
    custom_out_dir="data/estimated_clusterings"
    custom_gt="data/reference_clusterings/clusterings/${gt_clustering}/${network_id}/com.csv"
    dataset_type="${generator}/${gt_clustering} (run: ${run_id})"
fi

if [ -n "${custom_input}" ]; then
    if [ -z "${custom_out_dir}" ]; then log "Error: --output-dir must be provided."; exit 1; fi
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
    if [ -n "${custom_gt}" ]; then gt_file="${custom_gt}"; has_gt=1; fi
else
    log "Error: You must specify --real, --synthetic, or provide --input-edgelist and --output-dir."
    exit 1
fi

if [ ! -f "${inp_edge}" ]; then log "Input file ${inp_edge} does not exist. Skipping."; exit 1; fi

base_root_clusterings="${COMMDET_BASE}/clusterings"
base_root_stats="${COMMDET_BASE}/stats"
base_root_acc="${COMMDET_BASE}/acc"

# ==========================================
# Dependency Trigger
# ==========================================
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

    # Cascade the evaluation flags so the winning symlink naturally references valid stats/acc directories
    [[ ${run_stats_flag} -eq 1 ]] && cmd+=("--run-stats")
    [[ ${run_acc_flag} -eq 1 ]] && cmd+=("--run-acc")
    [[ ${run_cc_flag} -eq 1 ]] && cmd+=("--run-cc")
    [[ ${run_wcc_flag} -eq 1 ]] && cmd+=("--run-wcc")
    [[ ${run_cm_flag} -eq 1 ]] && cmd+=("--run-cm")

    "${cmd[@]}" || { log "CRITICAL: Dependency ${target_algo} failed."; exit 1; }
}

# ==========================================
# Functions: Stats and Accuracy
# ==========================================
run_stats() {
    if [ "${run_stats_flag}" -eq 0 ]; then return; fi
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
    if [ "${run_acc_flag}" -eq 0 ]; then return; fi
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

log "============================"
log "${algo} ${network_id:-[Custom]} | Dataset: ${dataset_type}"

leiden_model=""
leiden_res=""
ikc_k=""
sbm_model=""

if [[ ${algo} == leiden* ]]; then
    leiden_model=$(echo ${algo} | cut -d'-' -f2)
    [[ ${leiden_model} == cpm ]] && leiden_res=$(echo ${algo} | cut -d'-' -f3)
elif [[ ${algo} == ikc* ]]; then
    ikc_k=$(echo ${algo} | cut -d'-' -f2)
elif [[ ${algo} == sbm* ]]; then
    sbm_model=$(echo ${algo} | cut -d'-' -f2-)
fi

# ==========================================
# Resolve Meta-Model Dependencies
# ==========================================
if [[ ${algo} == "sbm-flat-best" ]]; then
    log "Resolving downstream states for flat-best dependencies..."
    for dep in sbm-flat-dc sbm-flat-ndc sbm-flat-pp; do
        run_dependency "${dep}"
    done
elif [[ ${algo} == "sbm-nested-best" ]]; then
    log "Resolving downstream states for nested-best dependencies..."
    for dep in sbm-nested-dc sbm-nested-ndc; do
        run_dependency "${dep}"
    done
fi

# ==========================================
# 1. Run Base Clustering
# ==========================================
suffix="${algo}"
out_dir="${base_root_clusterings}/${suffix}${opt_subpath}"
stats_dir="${base_root_stats}/${suffix}${opt_subpath}"
acc_dir="${base_root_acc}/${suffix}${opt_subpath}"

base_com="${out_dir}/com.csv"
base_done="${out_dir}/done"

log "Evaluating base clustering state..."
if ! is_step_done "${base_done}"; then
    log "Running clustering..."
    if [[ ${algo} == leiden* ]]; then
        if [[ ${leiden_model} == cpm ]]; then
            mkdir -p "${out_dir}"
            { timeout "${TIMEOUT}" /usr/bin/time -v python "${SCRIPT_DIR}/src/leiden/run_leiden.py" \
                --edgelist "${inp_edge}" --output-directory "${out_dir}" \
                --model cpm --resolution "${leiden_res}"; } 2> "${out_dir}/error.log"
        elif [[ ${leiden_model} == mod ]]; then
            mkdir -p "${out_dir}"
            { timeout "${TIMEOUT}" /usr/bin/time -v python "${SCRIPT_DIR}/src/leiden/run_leiden.py" \
                --edgelist "${inp_edge}" --output-directory "${out_dir}" --model mod; } 2> "${out_dir}/error.log"
        else
            log "Unknown leiden_model: ${leiden_model}"; exit 1
        fi
    elif [[ ${algo} == infomap ]]; then
        mkdir -p "${out_dir}"
        { timeout "${TIMEOUT}" /usr/bin/time -v python "${SCRIPT_DIR}/src/infomap/run_infomap.py" \
            --edgelist "${inp_edge}" --output-directory "${out_dir}"; } 1> "${out_dir}/output.log" 2> "${out_dir}/error.log"
    elif [[ ${algo} == ikc* ]]; then
        mkdir -p "${out_dir}"
        { timeout "${TIMEOUT}" /usr/bin/time -v python "${SCRIPT_DIR}/src/ikc/run_ikc.py" \
            --edgelist "${inp_edge}" --output-directory "${out_dir}" --kvalue "${ikc_k}"; } 1> "${out_dir}/output.log" 2> "${out_dir}/error.log"
    elif [[ ${algo} == sbm* ]]; then
        if [[ ${sbm_model} =~ ^(flat-dc|flat-ndc|flat-pp|nested-dc|nested-ndc)$ ]]; then
            log "Running ${sbm_model}..."
            mkdir -p "${out_dir}"
            { timeout "${TIMEOUT}" /usr/bin/time -v python "${SCRIPT_DIR}/src/sbm/run_sbm.py" \
                --edgelist "${inp_edge}" --output-directory "${out_dir}" --method "${sbm_model}"; } 2> "${out_dir}/error.log"
        elif [[ ${sbm_model} == "flat-best" ]]; then
            log "Selecting best flat-sbm..."
            sbm_flat_dc_root="${base_root_clusterings}/sbm-flat-dc${opt_subpath}"
            sbm_flat_ndc_root="${base_root_clusterings}/sbm-flat-ndc${opt_subpath}"
            sbm_flat_pp_root="${base_root_clusterings}/sbm-flat-pp${opt_subpath}"

            mkdir -p "${out_dir}"
            { timeout "${TIMEOUT}" /usr/bin/time -v python "${SCRIPT_DIR}/src/sbm/choose_best_sbm.py" \
                --entropy_files "${sbm_flat_dc_root}/entropy.txt" "${sbm_flat_ndc_root}/entropy.txt" "${sbm_flat_pp_root}/entropy.txt" \
                --com_files "${sbm_flat_dc_root}/com.csv" "${sbm_flat_ndc_root}/com.csv" "${sbm_flat_pp_root}/com.csv" \
                --model_names "sbm-flat-dc" "sbm-flat-ndc" "sbm-flat-pp" \
                --out_dir "${out_dir}"; } 1> "${out_dir}/out.log" 2> "${out_dir}/error.log"

            [ -f "${out_dir}/best_model.txt" ] && log "Best SBM selected: $(cat "${out_dir}/best_model.txt")" || log "Error: best_model.txt missing."
        elif [[ ${sbm_model} == "nested-best" ]]; then
            log "Selecting best nested-sbm..."
            sbm_nested_dc_root="${base_root_clusterings}/sbm-nested-dc${opt_subpath}"
            sbm_nested_ndc_root="${base_root_clusterings}/sbm-nested-ndc${opt_subpath}"

            mkdir -p "${out_dir}"
            { timeout "${TIMEOUT}" /usr/bin/time -v python "${SCRIPT_DIR}/src/sbm/choose_best_sbm.py" \
                --entropy_files "${sbm_nested_dc_root}/entropy.txt" "${sbm_nested_ndc_root}/entropy.txt" \
                --com_files "${sbm_nested_dc_root}/com.csv" "${sbm_nested_ndc_root}/com.csv" \
                --model_names "sbm-nested-dc" "sbm-nested-ndc" \
                --out_dir "${out_dir}"; } 1> "${out_dir}/out.log" 2> "${out_dir}/error.log"

            [ -f "${out_dir}/best_model.txt" ] && log "Best SBM selected: $(cat "${out_dir}/best_model.txt")" || log "Error: best_model.txt missing."
        else
            log "Unknown sbm_model: ${sbm_model}"; exit 1
        fi
    else
        log "Unknown method: ${algo}"; exit 1
    fi

    # Determine cryptographic inputs for Base Clustering
    base_inputs="${inp_edge}"
    if [[ ${sbm_model} == "flat-best" ]]; then
        base_inputs="${inp_edge} ${base_root_clusterings}/sbm-flat-dc${opt_subpath}/com.csv ${base_root_clusterings}/sbm-flat-dc${opt_subpath}/entropy.txt ${base_root_clusterings}/sbm-flat-ndc${opt_subpath}/com.csv ${base_root_clusterings}/sbm-flat-ndc${opt_subpath}/entropy.txt ${base_root_clusterings}/sbm-flat-pp${opt_subpath}/com.csv ${base_root_clusterings}/sbm-flat-pp${opt_subpath}/entropy.txt"
    elif [[ ${sbm_model} == "nested-best" ]]; then
        base_inputs="${inp_edge} ${base_root_clusterings}/sbm-nested-dc${opt_subpath}/com.csv ${base_root_clusterings}/sbm-nested-dc${opt_subpath}/entropy.txt ${base_root_clusterings}/sbm-nested-ndc${opt_subpath}/com.csv ${base_root_clusterings}/sbm-nested-ndc${opt_subpath}/entropy.txt"
    fi

    if [ -f "${base_com}" ] || [ -f "${out_dir}/best_model.txt" ]; then
        mark_done "${base_done}" "Base Clustering (${algo})" "${base_inputs}" "${out_dir}"
    fi
else
    log "Base clustering already up-to-date."
fi

if [ ! -f "${base_com}" ]; then
    log "CRITICAL: Base clustering failed or timed out."
    exit 1
fi

# ==========================================
# Symlink Shortcut for "Best" Meta-Models
# ==========================================
if [[ ${sbm_model} == "flat-best" || ${sbm_model} == "nested-best" ]]; then
    best_model_file="${out_dir}/best_model.txt"
    if [ ! -f "${best_model_file}" ]; then log "Error: ${best_model_file} not found."; exit 1; fi
    best_algo=$(cat "${best_model_file}")

    if [ ! -L "${base_com}" ] && [ ! -e "${base_com}" ]; then log "CRITICAL: Symlink broken."; exit 1; fi

    log "Symlinking downstream folders for: ${best_algo}..."

    if [ "${run_stats_flag}" -eq 1 ]; then
        target_stats="${base_root_stats}/${best_algo}${opt_subpath}"
        link_stats="${base_root_stats}/${algo}${opt_subpath}"
        [ -L "${link_stats}" ] && rm "${link_stats}"
        if [ -e "${target_stats}" ]; then
            mkdir -p "$(dirname "${link_stats}")"
            ln -sfn "$(realpath "${target_stats}")" "${link_stats}"
        else
            log "Warning: Expected base stats ${target_stats} not found. Symlink not created."
        fi
    fi

    if [ "${run_acc_flag}" -eq 1 ] && [ "${has_gt}" -eq 1 ]; then
        target_acc="${base_root_acc}/${best_algo}${opt_subpath}"
        link_acc="${base_root_acc}/${algo}${opt_subpath}"
        [ -L "${link_acc}" ] && rm "${link_acc}"
        if [ -e "${target_acc}" ]; then
            mkdir -p "$(dirname "${link_acc}")"
            ln -sfn "$(realpath "${target_acc}")" "${link_acc}"
        else
            log "Warning: Expected base accuracy ${target_acc} not found. Symlink not created."
        fi
    fi

    for base_pp in cc wcc cm; do
        if [ "${base_pp}" == "cc" ]; then
            if [ "${IS_RUN_CC}" -ne 1 ]; then continue; fi; pp_tag="cc"
        elif [ "${base_pp}" == "wcc" ]; then
            if [ "${IS_RUN_WCC}" -ne 1 ]; then continue; fi; pp_tag="wcc${CRIT_SUFFIX}"
        elif [ "${base_pp}" == "cm" ]; then
            if [ "${IS_RUN_CM}" -ne 1 ]; then continue; fi; pp_tag="cm${CRIT_SUFFIX}"
        fi

        target_pp_clust="${base_root_clusterings}/${best_algo}+${pp_tag}${opt_subpath}"
        link_pp_clust="${base_root_clusterings}/${algo}+${pp_tag}${opt_subpath}"
        [ -L "${link_pp_clust}" ] && rm "${link_pp_clust}"
        if [ -e "${target_pp_clust}" ]; then
            mkdir -p "$(dirname "${link_pp_clust}")"
            ln -sfn "$(realpath "${target_pp_clust}")" "${link_pp_clust}"
        else
            log "Warning: Expected post-processing clustering ${target_pp_clust} not found. Symlink not created."
        fi

        if [ "${run_stats_flag}" -eq 1 ]; then
            target_pp_stats="${base_root_stats}/${best_algo}+${pp_tag}${opt_subpath}"
            link_pp_stats="${base_root_stats}/${algo}+${pp_tag}${opt_subpath}"
            [ -L "${link_pp_stats}" ] && rm "${link_pp_stats}"
            if [ -e "${target_pp_stats}" ]; then
                mkdir -p "$(dirname "${link_pp_stats}")"
                ln -sfn "$(realpath "${target_pp_stats}")" "${link_pp_stats}"
            else
                log "Warning: Expected post-processing stats ${target_pp_stats} not found. Symlink not created."
            fi
        fi

        if [ "${run_acc_flag}" -eq 1 ] && [ "${has_gt}" -eq 1 ]; then
            target_pp_acc="${base_root_acc}/${best_algo}+${pp_tag}${opt_subpath}"
            link_pp_acc="${base_root_acc}/${algo}+${pp_tag}${opt_subpath}"
            [ -L "${link_pp_acc}" ] && rm "${link_pp_acc}"
            if [ -e "${target_pp_acc}" ]; then
                mkdir -p "$(dirname "${link_pp_acc}")"
                ln -sfn "$(realpath "${target_pp_acc}")" "${link_pp_acc}"
            else
                log "Warning: Expected post-processing accuracy ${target_pp_acc} not found. Symlink not created."
            fi
        fi
    done

    log "[cd] ${algo} ${network_id:-[Custom]} ${dataset_type} ${criterion}" >> complete.log
    exit 0
fi

run_stats "${inp_edge}" "${base_com}" "${stats_dir}"
run_accuracy "${inp_edge}" "${gt_file}" "${base_com}" "${acc_dir}"

# ==========================================
# 2. Run CC
# ==========================================
if [ "${IS_RUN_CC}" -eq 1 ]; then
    suffix="${algo}+cc"
    out_cc_dir="${base_root_clusterings}/${suffix}${opt_subpath}"
    stats_cc_dir="${base_root_stats}/${suffix}${opt_subpath}"
    acc_cc_dir="${base_root_acc}/${suffix}${opt_subpath}"
    cc_com="${out_cc_dir}/com.csv"
    cc_done="${out_cc_dir}/done"

    log "Evaluating CC state..."
    if ! is_step_done "${cc_done}"; then
        log "Running CC..."
        mkdir -p "${out_cc_dir}"
        { timeout "${TIMEOUT}" /usr/bin/time -v "${SCRIPT_DIR}/constrained-clustering/constrained_clustering" \
            MincutOnly --edgelist "${inp_edge}" --existing-clustering "${base_com}" \
            --num-processors 1 --output-file "${cc_com}" --log-file "${out_cc_dir}/cc.log" \
            --log-level 1 --connectedness-criterion 0; } 2> "${out_cc_dir}/error.log"
        if [ -f "${cc_com}" ]; then
            mark_done "${cc_done}" "CC" "${inp_edge} ${base_com}" "${out_cc_dir}"
        fi
    else log "CC already up-to-date."; fi

    if [ -f "${cc_com}" ]; then
        run_stats "${inp_edge}" "${cc_com}" "${stats_cc_dir}"
        run_accuracy "${inp_edge}" "${gt_file}" "${cc_com}" "${acc_cc_dir}"
    fi
fi

# ==========================================
# 3. Run WCC
# ==========================================
if [ "${IS_RUN_WCC}" -eq 1 ]; then
    suffix="${algo}+wcc${CRIT_SUFFIX}"
    out_wcc_dir="${base_root_clusterings}/${suffix}${opt_subpath}"
    stats_wcc_dir="${base_root_stats}/${suffix}${opt_subpath}"
    acc_wcc_dir="${base_root_acc}/${suffix}${opt_subpath}"
    wcc_com="${out_wcc_dir}/com.csv"
    wcc_done="${out_wcc_dir}/done"

    log "Evaluating WCC state..."
    if ! is_step_done "${wcc_done}"; then
        log "Running WCC (${WCC_CRIT})..."
        mkdir -p "${out_wcc_dir}"
        { timeout "${TIMEOUT}" /usr/bin/time -v "${SCRIPT_DIR}/constrained-clustering/constrained_clustering" \
            MincutOnly --connectedness-criterion "${WCC_CRIT}" --edgelist "${inp_edge}" \
            --existing-clustering "${base_com}" --num-processors 1 --output-file "${wcc_com}" \
            --log-file "${out_wcc_dir}/wcc.log" --log-level 1; } 2> "${out_wcc_dir}/error.log"
        if [ -f "${wcc_com}" ]; then
            mark_done "${wcc_done}" "WCC" "${inp_edge} ${base_com}" "${out_wcc_dir}"
        fi
    else log "WCC already up-to-date."; fi

    if [ -f "${wcc_com}" ]; then
        run_stats "${inp_edge}" "${wcc_com}" "${stats_wcc_dir}"
        run_accuracy "${inp_edge}" "${gt_file}" "${wcc_com}" "${acc_wcc_dir}"
    fi
fi

# ==========================================
# 4. Run CM
# ==========================================
if [ "${IS_RUN_CM}" -eq 1 ]; then
    suffix="${algo}+cm${CRIT_SUFFIX}"
    out_cm_dir="${base_root_clusterings}/${suffix}${opt_subpath}"
    stats_cm_dir="${base_root_stats}/${suffix}${opt_subpath}"
    acc_cm_dir="${base_root_acc}/${suffix}${opt_subpath}"
    cm_com="${out_cm_dir}/com.csv"
    cm_done="${out_cm_dir}/done"

    log "Evaluating CM state..."
    if ! is_step_done "${cm_done}"; then
        log "Running CM (${CM_CRIT})..."
        if [[ ${algo} == leiden* ]]; then
            if [[ ${leiden_model} == cpm ]]; then
                mkdir -p "${out_cm_dir}"
                { timeout "${TIMEOUT}" /usr/bin/time -v "${SCRIPT_DIR}/constrained-clustering/constrained_clustering" \
                    CM --mincut-type "cactus" --connectedness-criterion "${CM_CRIT}" \
                    --edgelist "${inp_edge}" --existing-clustering "${base_com}" \
                    --algorithm "leiden-cpm" --clustering-parameter "${leiden_res}" \
                    --num-processors 1 --output-file "${cm_com}" --history-file "${out_cm_dir}/history.log" \
                    --log-file "${out_cm_dir}/cm.log" --log-level 1; } 2> "${out_cm_dir}/error.log"
            elif [[ ${leiden_model} == mod ]]; then
                mkdir -p "${out_cm_dir}"
                { timeout "${TIMEOUT}" /usr/bin/time -v "${SCRIPT_DIR}/constrained-clustering/constrained_clustering" \
                    CM --mincut-type "cactus" --connectedness-criterion "${CM_CRIT}" \
                    --edgelist "${inp_edge}" --existing-clustering "${base_com}" \
                    --algorithm "leiden-mod" --num-processors 1 --output-file "${cm_com}" \
                    --history-file "${out_cm_dir}/history.log" --log-file "${out_cm_dir}/cm.log" \
                    --log-level 1; } 2> "${out_cm_dir}/error.log"
            fi
        else log "CM not implemented for ${algo}"; fi
        if [ -f "${cm_com}" ]; then
            mark_done "${cm_done}" "CM" "${inp_edge} ${base_com}" "${out_cm_dir}"
        fi
    else log "CM already up-to-date."; fi

    if [ -f "${cm_com}" ]; then
        run_stats "${inp_edge}" "${cm_com}" "${stats_cm_dir}"
        run_accuracy "${inp_edge}" "${gt_file}" "${cm_com}" "${acc_cm_dir}"
    fi
fi

log "[cd] ${algo} ${network_id:-[Custom]} ${dataset_type} ${criterion}" >> complete.log
