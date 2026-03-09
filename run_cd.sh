#!/bin/bash

# ==============================================================================
# Community Detection Evaluation Pipeline (run_cd.sh)
# ==============================================================================
# Computes base clusterings, selects best SBM models, refines via constrained
# clustering, and evaluates statistics (and accuracy for synthetic networks).
#
# USAGE:
#   Real:  ./run_cd.sh --algo <algo> --network <id> --real [OPTIONS]
#   Synth: ./run_cd.sh --algo <algo> --network <id> --generator <gen> --gt-clustering <gt> [OPTIONS]
#
# OPTIONS:
#   --run-id <id>      : (Synthetic) Identifier for the run (default: 0).
#   --criterion <name> : Connectedness criterion for WCC and CM (e.g., 'sqrt' or 'log').
#                        If provided, outputs are suffixed (e.g., +wcc(sqrt)).
#
# PATH LEGEND:
#   [INP_EDGE]
#       Real  -> data/empirical_networks/netzschleuder/<network_id>/<network_id>.csv
#       Synth -> data/synthetic_networks/networks/<generator>/<gt_clustering>/<network_id>/<run_id>/edge.csv
#   [GT_COM]
#       Synth -> data/reference_clusterings/clusterings/<gt_clustering>/<network_id>/com.csv
#   [OUT_ROOT]
#       Real  -> data/reference_clusterings
#       Synth -> data/estimated_clusterings/<generator>/<gt_clustering>
#   [SUB_PATH]
#       Real  -> <network_id>
#       Synth -> <network_id>/<run_id>
#
# ------------------------------------------------------------------------------
# STEP 1: Base Clustering
# ------------------------------------------------------------------------------
# Computes the initial community structure using the specified algorithm.
#   - Scripts: src/comm-det/{leiden,infomap,ikc,sbm}/run_*.py
#
# [Inputs]
#   - Network Edge List : [INP_EDGE]
# [Outputs]
#   - Base Clustering   : [OUT_ROOT]/clusterings/<algo>/[SUB_PATH]/com.csv
#
# ------------------------------------------------------------------------------
# STEP 2: SBM Best Model Selection (Only if algo is sbm-flat-best or sbm-nested-best)
# ------------------------------------------------------------------------------
# Evaluates entropy across pre-computed SBM variants (dc, ndc, pp) to pick the best.
#   - Script: src/comm-det/sbm/choose_best_sbm.py
#
# [Inputs]
#   - SBM Variant Coms  : [OUT_ROOT]/clusterings/sbm-<variant>/[SUB_PATH]/com.csv
#   - SBM Entropies     : [OUT_ROOT]/clusterings/sbm-<variant>/[SUB_PATH]/entropy.txt
# [Outputs]
#   - Selection Log     : [OUT_ROOT]/clusterings/<algo>/[SUB_PATH]/best_model.txt
#   - Symlinks          : Creates symlinks to the winning model's com.csv, stats, 
#                         and acc directories to bypass redundant computations.
#
# ------------------------------------------------------------------------------
# STEP 3: Statistics Computation
# ------------------------------------------------------------------------------
# Calculates network metrics for the estimated base clustering.
#   - Script: network_evaluation/network_stats/compute_cluster_stats.py
#
# [Inputs]
#   - Network Edge List : [INP_EDGE]
#   - Base Clustering   : (Generated in Step 1)
# [Outputs]
#   - Stats Directory   : [OUT_ROOT]/stats/<algo>/[SUB_PATH]/
#
# ------------------------------------------------------------------------------
# STEP 4: Accuracy Evaluation (Synthetic Networks Only)
# ------------------------------------------------------------------------------
# Compares the estimated base clustering against the ground truth.
#   - Script: network_evaluation/commdet_acc/compute_cd_accuracy.py
#
# [Inputs]
#   - Network Edge List : [INP_EDGE]
#   - Base Clustering   : (Generated in Step 1)
#   - Ground Truth Coms : [GT_COM]
# [Outputs]
#   - Acc Directory     : [OUT_ROOT]/acc/<algo>/[SUB_PATH]/
#
# ------------------------------------------------------------------------------
# STEP 5: Post-Processing (CC, WCC, CM)
# ------------------------------------------------------------------------------
# Refines the base clustering via Constrained Clustering variants.
#   - Binary: ./constrained-clustering/constrained_clustering
#
# [Inputs]
#   - Network Edge List : [INP_EDGE]
#   - Base Clustering   : (Generated in Step 1)
# [Outputs]
#   - Refined Clustering: [OUT_ROOT]/clusterings/<algo>+<pp>[criterion]/[SUB_PATH]/com.csv
#
# ------------------------------------------------------------------------------
# STEP 6: Post-Processing Evaluation
# ------------------------------------------------------------------------------
# Re-runs the Statistics and Accuracy evaluations on the refined clustering outputs.
#   - Scripts: Uses the same Python scripts from Steps 3 and 4.
#
# [Inputs]
#   - Network Edge List : [INP_EDGE]
#   - Refined Clustering: (Generated in Step 5)
#   - Ground Truth Coms : [GT_COM] (Synth only)
# [Outputs]
#   - Stats Directory   : [OUT_ROOT]/stats/<algo>+<pp>[criterion]/[SUB_PATH]/
#   - Acc Directory     : [OUT_ROOT]/acc/<algo>+<pp>[criterion]/[SUB_PATH]/ (Synth only)
# ==============================================================================

# Constants
TIMEOUT="3d"

# ==========================================
# Helper Functions
# ==========================================
# Custom log function to prepend timestamps to output
log() {
    builtin echo "[$(date +'%Y-%m-%d %H:%M:%S')] $*"
}

# ==========================================
# Argument Parsing
# ==========================================
algo=""
network_id=""
is_real=0
generator=""
gt_clustering=""
run_id="0"
criterion=""

while [[ "$#" -gt 0 ]]; do
    case $1 in
        --algo) algo="$2"; shift 2 ;;
        --network) network_id="$2"; shift 2 ;;
        --real) is_real=1; shift 1 ;;
        --generator) generator="$2"; shift 2 ;;
        --gt-clustering) gt_clustering="$2"; shift 2 ;;
        --run-id) run_id="$2"; shift 2 ;;
        --criterion) criterion="$2"; shift 2 ;;
        -*) log "Unknown parameter passed: $1"; exit 1 ;;
        *) log "Unexpected argument: $1"; exit 1 ;;
    esac
done

if [ -z "${algo}" ] || [ -z "${network_id}" ]; then
    log "Error: --algo and --network are required parameters."
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
        WCC_CRIT="0.2n^0.5"
        CM_CRIT="0.2n^0.5"
    elif [[ "${criterion}" == "log" ]]; then
        WCC_CRIT="1log_10(n)"
        CM_CRIT="1log_10(n)"
    else
        # Fallback if a custom formula is provided directly
        WCC_CRIT="${criterion}"
        CM_CRIT="${criterion}"
    fi
fi

# Flags
IS_RUN_CC=1
IS_RUN_WCC=1
IS_RUN_CM=1

case ${algo} in
    ikc*) IS_RUN_CC=0; IS_RUN_WCC=0; IS_RUN_CM=0 ;;
    leiden*) IS_RUN_CC=0; IS_RUN_WCC=0 ;;
    sbm*) IS_RUN_CM=0 ;;
    infomap) IS_RUN_WCC=0; IS_RUN_CM=0 ;;
esac

# ==========================================
# Input/Output Path Routing (Real vs Synthetic)
# ==========================================
if [ "${is_real}" -eq 1 ]; then
    dataset_type="real"
    inp_edge="data/empirical_networks/netzschleuder/${network_id}/${network_id}.csv"
    COMMDET_BASE="data/reference_clusterings"
    out_subpath="${network_id}"
    gt_file="" # Not applicable for real networks
else
    if [ -z "${generator}" ] || [ -z "${gt_clustering}" ]; then
        log "Error: For synthetic networks, --generator and --gt-clustering must be provided."
        exit 1
    fi

    dataset_type="${generator}/${gt_clustering} (run: ${run_id})"
    inp_edge="data/synthetic_networks/networks/${generator}/${gt_clustering}/${network_id}/${run_id}/edge.csv"
    COMMDET_BASE="data/estimated_clusterings/${generator}/${gt_clustering}"
    out_subpath="${network_id}/${run_id}"
    gt_file="data/reference_clusterings/clusterings/${gt_clustering}/${network_id}/com.csv"
fi

if [ ! -f "${inp_edge}" ]; then
    log "Input file ${inp_edge} does not exist. Skipping."
    exit 1
fi

base_root_clusterings="${COMMDET_BASE}/clusterings"
base_root_stats="${COMMDET_BASE}/stats"
base_root_acc="${COMMDET_BASE}/acc"

# ==========================================
# Functions: Stats and Accuracy
# ==========================================
run_stats() {
    local edge_file=$1
    local com_file=$2
    local stats_dir=$3

    log "Computing stats..."
    if [ ! -f "${stats_dir}/done" ]; then
        mkdir -p "${stats_dir}"
        { /usr/bin/time -v python network_evaluation/network_stats/compute_cluster_stats.py \
            --network "${edge_file}" \
            --community "${com_file}" \
            --outdir "${stats_dir}"; } 2> "${stats_dir}/error.log"
    else
        log "Stats already done."
    fi
}

run_accuracy() {
    local edge_file=$1
    local gt_f=$2
    local est_file=$3
    local acc_d=$4

    log "Computing accuracy..."
    if [ ! -f "${acc_d}/done" ]; then
        mkdir -p "${acc_d}"
        { /usr/bin/time -v python network_evaluation/commdet_acc/compute_cd_accuracy.py \
            --input-network "${edge_file}" \
            --gt-clustering "${gt_f}" \
            --est-clustering "${est_file}" \
            --output-prefix "${acc_d}/result"; } 2> "${acc_d}/error.log"
    else
        log "Accuracy already done."
    fi
}

log "============================"
log "${algo} ${network_id} | Dataset: ${dataset_type}"

leiden_model=""
leiden_res=""
ikc_k=""
sbm_model=""

if [[ ${algo} == leiden* ]]; then
    leiden_model=$(echo ${algo} | cut -d'-' -f2)
    if [[ ${leiden_model} == cpm ]]; then
        leiden_res=$(echo ${algo} | cut -d'-' -f3)
    fi
elif [[ ${algo} == ikc* ]]; then
    ikc_k=$(echo ${algo} | cut -d'-' -f2)
elif [[ ${algo} == sbm* ]]; then
    sbm_model=$(echo ${algo} | cut -d'-' -f2-)
fi

# ==========================================
# 1. Run Base Clustering
# ==========================================
suffix="${algo}"
out_dir="${base_root_clusterings}/${suffix}/${out_subpath}"
stats_dir="${base_root_stats}/${suffix}/${out_subpath}"
acc_dir="${base_root_acc}/${suffix}/${out_subpath}"

base_com="${out_dir}/com.csv"
base_dens="${out_dir}/density.csv"
base_done="${out_dir}/done"

log "Running clustering..."
if [ ! -f "${base_done}" ]; then
    if [[ ${algo} == leiden* ]]; then
        if [[ ${leiden_model} == cpm ]]; then
            mkdir -p "${out_dir}"
            { timeout "${TIMEOUT}" /usr/bin/time -v python src/comm-det/leiden/run_leiden.py \
                --edgelist "${inp_edge}" \
                --output-directory "${out_dir}" \
                --model cpm \
                --resolution "${leiden_res}"; } 2> "${out_dir}/error.log"
        elif [[ ${leiden_model} == mod ]]; then
            mkdir -p "${out_dir}"
            { timeout "${TIMEOUT}" /usr/bin/time -v python src/comm-det/leiden/run_leiden.py \
                --edgelist "${inp_edge}" \
                --output-directory "${out_dir}" \
                --model mod; } 2> "${out_dir}/error.log"
        else
            log "Unknown leiden_model: ${leiden_model}"; exit 1
        fi
    elif [[ ${algo} == infomap ]]; then
        mkdir -p "${out_dir}"
        { timeout "${TIMEOUT}" /usr/bin/time -v python src/comm-det/infomap/run_infomap.py \
            --edgelist "${inp_edge}" \
            --output-directory "${out_dir}"; } 1> "${out_dir}/output.log" 2> "${out_dir}/error.log"
    elif [[ ${algo} == ikc* ]]; then
        mkdir -p "${out_dir}"
        { timeout "${TIMEOUT}" /usr/bin/time -v python src/comm-det/ikc/run_ikc.py \
            --edgelist "${inp_edge}" \
            --output-directory "${out_dir}" \
            --kvalue "${ikc_k}"; } 1> "${out_dir}/output.log" 2> "${out_dir}/error.log"
    elif [[ ${algo} == sbm* ]]; then
        if [[ ${sbm_model} =~ ^(flat-dc|flat-ndc|flat-pp|nested-dc|nested-ndc)$ ]]; then
            log "Running ${sbm_model}..."
            mkdir -p "${out_dir}"
            { timeout "${TIMEOUT}" /usr/bin/time -v python src/comm-det/sbm/run_sbm.py \
                --edgelist "${inp_edge}" \
                --output-directory "${out_dir}" \
                --method "${sbm_model}"; } 2> "${out_dir}/error.log"
        elif [[ ${sbm_model} == "flat-best" ]]; then
            log "Running flat-best (selecting best of dc, ndc, pp)..."
            sbm_flat_dc_root="${base_root_clusterings}/sbm-flat-dc/${out_subpath}"
            sbm_flat_ndc_root="${base_root_clusterings}/sbm-flat-ndc/${out_subpath}"
            sbm_flat_pp_root="${base_root_clusterings}/sbm-flat-pp/${out_subpath}"
            
            mkdir -p "${out_dir}"
            { timeout "${TIMEOUT}" /usr/bin/time -v python src/comm-det/sbm/choose_best_sbm.py \
                --entropy_files "${sbm_flat_dc_root}/entropy.txt" "${sbm_flat_ndc_root}/entropy.txt" "${sbm_flat_pp_root}/entropy.txt" \
                --com_files "${sbm_flat_dc_root}/com.csv" "${sbm_flat_ndc_root}/com.csv" "${sbm_flat_pp_root}/com.csv" \
                --out_dir "${out_dir}"; } 1> "${out_dir}/out.log" 2> "${out_dir}/error.log"

            if [ -f "${out_dir}/best_model.txt" ]; then
                best_model=$(cat "${out_dir}/best_model.txt")
                log "Best SBM model selected: ${best_model}"
            else
                log "Error: Best model selection failed. best_model.txt not found."
            fi
        elif [[ ${sbm_model} == "nested-best" ]]; then
            log "Running nested-best (selecting best of dc, ndc)..."
            sbm_nested_dc_root="${base_root_clusterings}/sbm-nested-dc/${out_subpath}"
            sbm_nested_ndc_root="${base_root_clusterings}/sbm-nested-ndc/${out_subpath}"
            
            mkdir -p "${out_dir}"
            { timeout "${TIMEOUT}" /usr/bin/time -v python src/comm-det/sbm/choose_best_sbm.py \
                --entropy_files "${sbm_nested_dc_root}/entropy.txt" "${sbm_nested_ndc_root}/entropy.txt" \
                --com_files "${sbm_nested_dc_root}/com.csv" "${sbm_nested_ndc_root}/com.csv" \
                --out_dir "${out_dir}"; } 1> "${out_dir}/out.log" 2> "${out_dir}/error.log"

            if [ -f "${out_dir}/best_model.txt" ]; then
                best_model=$(cat "${out_dir}/best_model.txt")
                log "Best SBM model selected: ${best_model}"
            else
                log "Error: Best model selection failed. best_model.txt not found."
            fi
        else
            log "Unknown sbm_model: ${sbm_model}"; exit 1
        fi
    else
        log "Unknown method: ${algo}"; exit 1
    fi

    if [ -f "${base_com}" ]; then 
        touch "${base_done}"
    fi
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
    if [ ! -f "${best_model_file}" ]; then
        log "Error: ${best_model_file} not found. Could not link downstream tasks."
        exit 1
    fi
    
    best_algo=$(cat "${best_model_file}")

    if [ ! -L "${base_com}" ] || [ ! -e "${base_com}" ]; then
        log "CRITICAL: Symlink ${base_com} does not exist or is broken."
        exit 1
    fi

    log "Best model successfully read as: ${best_algo}. Symlinking downstream folders..."

    # 1. Symlink base stats folder
    target_stats="${PWD}/${base_root_stats}/${best_algo}/${out_subpath}"
    if [ -e "${target_stats}" ]; then
        mkdir -p "$(dirname "${stats_dir}")"
        ln -sfn "${target_stats}" "${stats_dir}"
    else
        log "Warning: Base stats folder ${target_stats} not found. Skipping symlink."
    fi

    # 1b. Symlink base acc folder (for synthetic)
    if [ "${is_real}" -eq 0 ]; then
        target_acc="${PWD}/${base_root_acc}/${best_algo}/${out_subpath}"
        if [ -e "${target_acc}" ]; then
            mkdir -p "$(dirname "${acc_dir}")"
            ln -sfn "${target_acc}" "${acc_dir}"
        fi
    fi

    # 2. Symlink post-processing & their respective stats/acc folders
    for base_pp in cc wcc cm; do
        # Determine the correct tag and skip conditionally
        if [ "${base_pp}" == "cc" ]; then
            if [ "${IS_RUN_CC}" -ne 1 ]; then continue; fi
            pp_tag="cc"
        elif [ "${base_pp}" == "wcc" ]; then
            if [ "${IS_RUN_WCC}" -ne 1 ]; then continue; fi
            pp_tag="wcc${CRIT_SUFFIX}"
        elif [ "${base_pp}" == "cm" ]; then
            if [ "${IS_RUN_CM}" -ne 1 ]; then continue; fi
            pp_tag="cm${CRIT_SUFFIX}"
        fi

        target_pp_clust="${PWD}/${base_root_clusterings}/${best_algo}+${pp_tag}/${out_subpath}"
        target_pp_stats="${PWD}/${base_root_stats}/${best_algo}+${pp_tag}/${out_subpath}"
        
        # Check and link post-processing clustering
        if [ -e "${target_pp_clust}" ]; then
            mkdir -p "$(dirname "${base_root_clusterings}/${algo}+${pp_tag}/${out_subpath}")"
            ln -sfn "${target_pp_clust}" "${base_root_clusterings}/${algo}+${pp_tag}/${out_subpath}"
        else
            log "Warning: Post-processing cluster folder ${target_pp_clust} not found. Skipping symlink."
        fi

        # Check and link post-processing stats
        if [ -e "${target_pp_stats}" ]; then
            mkdir -p "$(dirname "${base_root_stats}/${algo}+${pp_tag}/${out_subpath}")"
            ln -sfn "${target_pp_stats}" "${base_root_stats}/${algo}+${pp_tag}/${out_subpath}"
        else
            log "Warning: Post-processing stats folder ${target_pp_stats} not found. Skipping symlink."
        fi
        
        # Check and link post-processing acc (for synthetic)
        if [ "${is_real}" -eq 0 ]; then
            target_pp_acc="${PWD}/${base_root_acc}/${best_algo}+${pp_tag}/${out_subpath}"
            if [ -e "${target_pp_acc}" ]; then
                mkdir -p "$(dirname "${base_root_acc}/${algo}+${pp_tag}/${out_subpath}")"
                ln -sfn "${target_pp_acc}" "${base_root_acc}/${algo}+${pp_tag}/${out_subpath}"
            else
                log "Warning: Post-processing acc folder ${target_pp_acc} not found. Skipping symlink."
            fi
        fi
    done

    log "[cd-done] ${algo} ${network_id} ${dataset_type}" >> complete.log
    exit 0
fi

run_stats "${inp_edge}" "${base_com}" "${stats_dir}"
if [ "${is_real}" -eq 0 ]; then
    run_accuracy "${inp_edge}" "${gt_file}" "${base_com}" "${acc_dir}"
fi

# ==========================================
# 2. Run CC
# ==========================================
if [ "${IS_RUN_CC}" -eq 1 ]; then
    suffix="${algo}+cc"
    out_cc_dir="${base_root_clusterings}/${suffix}/${out_subpath}"
    stats_cc_dir="${base_root_stats}/${suffix}/${out_subpath}"
    acc_cc_dir="${base_root_acc}/${suffix}/${out_subpath}"
    cc_com="${out_cc_dir}/com.csv"
    cc_done="${out_cc_dir}/done"
    
    if [ ! -f "${cc_done}" ]; then
        log "Running CC..."
        mkdir -p "${out_cc_dir}"
        { timeout "${TIMEOUT}" /usr/bin/time -v ./constrained-clustering/constrained_clustering \
            MincutOnly \
            --edgelist "${inp_edge}" \
            --existing-clustering "${base_com}" \
            --num-processors 1 \
            --output-file "${cc_com}" \
            --log-file "${out_cc_dir}/cc.log" \
            --log-level 1 \
            --connectedness-criterion 0; } 2> "${out_cc_dir}/error.log"
        [ -f "${cc_com}" ] && touch "${cc_done}"
    else
        log "CC already done."
    fi
    
    if [ -f "${cc_com}" ]; then
        run_stats "${inp_edge}" "${cc_com}" "${stats_cc_dir}"
        if [ "${is_real}" -eq 0 ]; then
            run_accuracy "${inp_edge}" "${gt_file}" "${cc_com}" "${acc_cc_dir}"
        fi
    fi
fi

# ==========================================
# 3. Run WCC 
# ==========================================
if [ "${IS_RUN_WCC}" -eq 1 ]; then
    suffix="${algo}+wcc${CRIT_SUFFIX}"
    out_wcc_dir="${base_root_clusterings}/${suffix}/${out_subpath}"
    stats_wcc_dir="${base_root_stats}/${suffix}/${out_subpath}"
    acc_wcc_dir="${base_root_acc}/${suffix}/${out_subpath}"
    wcc_com="${out_wcc_dir}/com.csv"
    wcc_done="${out_wcc_dir}/done"
    
    if [ ! -f "${wcc_done}" ]; then
        log "Running WCC (${WCC_CRIT})..."
        mkdir -p "${out_wcc_dir}"
        { timeout "${TIMEOUT}" /usr/bin/time -v ./constrained-clustering/constrained_clustering \
            MincutOnly \
            --connectedness-criterion "${WCC_CRIT}" \
            --edgelist "${inp_edge}" \
            --existing-clustering "${base_com}" \
            --num-processors 1 \
            --output-file "${wcc_com}" \
            --log-file "${out_wcc_dir}/wcc.log" \
            --log-level 1; } 2> "${out_wcc_dir}/error.log"
        [ -f "${wcc_com}" ] && touch "${wcc_done}"
    else
        log "WCC already done."
    fi

    if [ -f "${wcc_com}" ]; then
        run_stats "${inp_edge}" "${wcc_com}" "${stats_wcc_dir}"
        if [ "${is_real}" -eq 0 ]; then
            run_accuracy "${inp_edge}" "${gt_file}" "${wcc_com}" "${acc_wcc_dir}"
        fi
    fi
fi

# ==========================================
# 4. Run CM
# ==========================================
if [ "${IS_RUN_CM}" -eq 1 ]; then
    suffix="${algo}+cm${CRIT_SUFFIX}"
    out_cm_dir="${base_root_clusterings}/${suffix}/${out_subpath}"
    stats_cm_dir="${base_root_stats}/${suffix}/${out_subpath}"
    acc_cm_dir="${base_root_acc}/${suffix}/${out_subpath}"
    cm_com="${out_cm_dir}/com.csv"
    cm_done="${out_cm_dir}/done"
    
    if [ ! -f "${cm_done}" ]; then
        log "Running CM (${CM_CRIT})..."
        if [[ ${algo} == leiden* ]]; then
            if [[ ${leiden_model} == cpm ]]; then
                mkdir -p "${out_cm_dir}"
                { timeout "${TIMEOUT}" /usr/bin/time -v ./constrained-clustering/constrained_clustering \
                    CM \
                    --mincut-type "cactus" \
                    --connectedness-criterion "${CM_CRIT}" \
                    --edgelist "${inp_edge}" \
                    --existing-clustering "${base_com}" \
                    --algorithm "leiden-cpm" \
                    --clustering-parameter "${leiden_res}" \
                    --num-processors 1 \
                    --output-file "${cm_com}" \
                    --history-file "${out_cm_dir}/history.log" \
                    --log-file "${out_cm_dir}/cm.log" \
                    --log-level 1; } 2> "${out_cm_dir}/error.log"
            elif [[ ${leiden_model} == mod ]]; then
                mkdir -p "${out_cm_dir}"
                { timeout "${TIMEOUT}" /usr/bin/time -v ./constrained-clustering/constrained_clustering \
                    CM \
                    --mincut-type "cactus" \
                    --connectedness-criterion "${CM_CRIT}" \
                    --edgelist "${inp_edge}" \
                    --existing-clustering "${base_com}" \
                    --algorithm "leiden-mod" \
                    --num-processors 1 \
                    --output-file "${cm_com}" \
                    --history-file "${out_cm_dir}/history.log" \
                    --log-file "${out_cm_dir}/cm.log" \
                    --log-level 1; } 2> "${out_cm_dir}/error.log"
            fi
        else
            log "CM not implemented for ${algo}"
        fi
        [ -f "${cm_com}" ] && touch "${cm_done}"
    else
        log "CM already done."
    fi

    if [ -f "${cm_com}" ]; then
        run_stats "${inp_edge}" "${cm_com}" "${stats_cm_dir}"
        if [ "${is_real}" -eq 0 ]; then
            run_accuracy "${inp_edge}" "${gt_file}" "${cm_com}" "${acc_cm_dir}"
        fi
    fi
fi

log "[cd-done] ${algo} ${network_id} ${dataset_type}" >> complete.log