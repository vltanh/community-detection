#!/bin/bash

# ==============================================================================
# Community Detection Evaluation Pipeline (run_cd.sh)
# ==============================================================================
# Computes base clusterings, selects best SBM models, refines via constrained
# clustering, and evaluates statistics (and accuracy if ground truth is provided).
#
# USAGE:
#   Real:   ./run_cd.sh --algo <algo> --real --network <id> [OPTIONS]
#   Synth:  ./run_cd.sh --algo <algo> --synthetic --network <id> --generator <gen> --gt-clustering-id <gt> [OPTIONS]
#   Custom: ./run_cd.sh --algo <algo> --input-edgelist <path> --output-dir <dir> [--network <id>] [OPTIONS]
#
# OPTIONS:
#   [General]
#     --algo <algo>          : (Required) Algorithm to run (e.g., leiden-cpm-0.1, infomap, sbm-flat-dc).
#     --criterion <name>     : Connectedness criterion for WCC and CM (e.g., 'sqrt' or 'log').
#                              If provided, outputs are suffixed (e.g., +wcc(sqrt)).
#
#   [Macros: Auto-populate input/output paths]
#     --real                    : Use standard paths for empirical networks.
#     --synthetic               : Use standard paths for synthetic networks.
#     --network <id>            : Network identifier (Required for macros, optional for custom paths to group outputs).
#     --generator <gen>         : Generator used (Required for --synthetic).
#     --gt-clustering-id <gt>   : Ground-truth identifier (Required for --synthetic).
#     --run-id <id>             : Run identifier (Default: 0).
#
#   [Custom Paths] (Overrides macros if explicitly provided)
#     --input-edgelist <p>      : Custom path to the input edge list CSV.
#     --output-dir <dir>        : Root directory for outputs.
#     --input-gt-clustering <p> : (Optional) Path to ground-truth CSV (triggers acc eval if --run-acc).
#
#   [Execution Flags] (Stats, Acc, and Post-Processing skip by default)
#     --run-stats            : Enable network statistics computation.
#     --run-acc              : Enable accuracy evaluation against ground truth.
#     --run-cc               : Enable Constrained Clustering (CC) post-processing.
#     --run-wcc              : Enable Weakly Constrained Clustering (WCC) post-processing.
#     --run-cm               : Enable Cactus Min-cut (CM) post-processing.
#
# PATH LEGEND:
#   [INP_EDGE]
#       Synth       -> data/synthetic_networks/networks/<generator>/<gt_clustering>/<network_id>/<run_id>/edge.csv
#       Real        -> data/empirical_networks/netzschleuder/<network_id>/<network_id>.csv
#       Custom      -> <input_edgelist>
#   [GT_COM]
#       Synth       -> data/reference_clusterings/clusterings/<gt_clustering>/<network_id>/com.csv
#       Real/Custom -> <input_gt_clustering>
#   [OUT_ROOT]
#       Synth       -> data/estimated_clusterings/<generator>/<gt_clustering>
#       Real        -> data/reference_clusterings
#       Custom      -> <output_dir>
#   [SUB_PATH]
#       Synth       -> <network_id>/<run_id>
#       Real        -> <network_id>
#       Custom      -> Maps directly to OUT_ROOT subdirectories unless --network <id> is passed, 
#                      in which case outputs are grouped under /<network_id>
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
#   - Symlinks          : Creates symlinks to the winning model's outputs.
#
# ------------------------------------------------------------------------------
# STEP 3: Statistics Computation (Requires --run-stats)
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
# STEP 4: Accuracy Evaluation (Requires --run-acc and a ground-truth clustering)
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
# STEP 5: Post-Processing (Requires --run-cc, --run-wcc, or --run-cm)
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
# Re-runs the Statistics and Accuracy evaluations on the refined clustering outputs 
# (subject to the same --run-stats and --run-acc flags).
#
# [Inputs]
#   - Network Edge List : [INP_EDGE]
#   - Refined Clustering: (Generated in Step 5)
#   - Ground Truth Coms : [GT_COM]
# [Outputs]
#   - Stats Directory   : [OUT_ROOT]/stats/<algo>+<pp>[criterion]/[SUB_PATH]/
#   - Acc Directory     : [OUT_ROOT]/acc/<algo>+<pp>[criterion]/[SUB_PATH]/
# ==============================================================================

# Constants
TIMEOUT="3d"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" &> /dev/null && pwd)"

# ==========================================
# Helper Functions
# ==========================================
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
run_id="0"
criterion=""

custom_input=""
custom_out_dir=""
custom_gt=""

run_stats_flag=0
run_acc_flag=0
run_cc_flag=0
run_wcc_flag=0
run_cm_flag=0

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
        if [ "$IS_RUN_CC" -eq 1 ]; then
            log "Warning: CC is not necessary for IKC. Disabling."
        fi
        if [ "$IS_RUN_WCC" -eq 1 ] || [ "$IS_RUN_CM" -eq 1 ]; then
            log "Warning: WCC, and CM are not supported for IKC. Disabling."
        fi
        IS_RUN_CC=0; IS_RUN_WCC=0; IS_RUN_CM=0
        ;;
    leiden*)
        if [ "$IS_RUN_CC" -eq 1 ]; then
            log "Warning: CC is not necessary for Leiden. Disabling."
        fi
        if [ "$IS_RUN_WCC" -eq 1 ]; then
            log "Warning: WCC is not supported for Leiden. Disabling."
        fi
        IS_RUN_CC=0; IS_RUN_WCC=0
        ;;
    sbm*)
        if [ "$IS_RUN_CM" -eq 1 ]; then
            log "Warning: CM is not supported for SBM. Disabling."
        fi
        IS_RUN_CM=0
        ;;
    infomap)
        if [ "$IS_RUN_WCC" -eq 1 ] || [ "$IS_RUN_CM" -eq 1 ]; then
            log "Warning: WCC and CM are not supported for Infomap. Disabling."
        fi
        IS_RUN_WCC=0; IS_RUN_CM=0
        ;;
esac

# ==========================================
# Input/Output Path Routing (Unified)
# ==========================================
has_gt=0
out_subpath=""

# Macro: Real Networks
if [ "${is_real}" -eq 1 ]; then
    if [ -z "${network_id}" ]; then log "Error: --network required for --real."; exit 1; fi
    custom_input="data/empirical_networks/netzschleuder/${network_id}/${network_id}.csv"
    custom_out_dir="data/reference_clusterings"
    dataset_type="real"
    out_subpath="${network_id}"
fi

# Macro: Synthetic Networks
if [ "${is_synthetic}" -eq 1 ]; then
    if [ -z "${network_id}" ] || [ -z "${generator}" ] || [ -z "${gt_clustering}" ]; then
        log "Error: --network, --generator, and --gt-clustering-id required for --synthetic."
        exit 1
    fi
    custom_input="data/synthetic_networks/networks/${generator}/${gt_clustering}/${network_id}/${run_id}/edge.csv"
    custom_out_dir="data/estimated_clusterings/${generator}/${gt_clustering}"
    custom_gt="data/reference_clusterings/clusterings/${gt_clustering}/${network_id}/com.csv"
    dataset_type="${generator}/${gt_clustering} (run: ${run_id})"
    out_subpath="${network_id}/${run_id}"
fi

# Unified Execution Engine
if [ -n "${custom_input}" ]; then
    if [ -z "${custom_out_dir}" ]; then
        log "Error: --output-dir must be provided."
        exit 1
    fi
    
    inp_edge="${custom_input}"
    COMMDET_BASE="${custom_out_dir}"
    dataset_type="${dataset_type:-custom}"
    
    # If network_id was provided explicitly in custom mode, use it. Otherwise, subpath remains empty.
    if [ "${is_real}" -eq 0 ] && [ "${is_synthetic}" -eq 0 ] && [ -n "${network_id}" ]; then
        out_subpath="${network_id}"
    fi
    
    if [ -n "${custom_gt}" ]; then
        gt_file="${custom_gt}"
        has_gt=1
    fi
else
    log "Error: You must specify --real, --synthetic, or provide --input-edgelist and --output-dir."
    exit 1
fi

if [ ! -f "${inp_edge}" ]; then
    log "Input file ${inp_edge} does not exist. Skipping."
    exit 1
fi

# Calculate the dynamic path modifier once to remove repetition
opt_subpath="${out_subpath:+/${out_subpath}}"

base_root_clusterings="${COMMDET_BASE}/clusterings"
base_root_stats="${COMMDET_BASE}/stats"
base_root_acc="${COMMDET_BASE}/acc"

# ==========================================
# Functions: Stats and Accuracy
# ==========================================
run_stats() {
    if [ "${run_stats_flag}" -eq 0 ]; then return; fi
    local edge_file=$1; local com_file=$2; local stats_dir=$3
    log "Computing stats..."
    if [ ! -f "${stats_dir}/done" ]; then
        mkdir -p "${stats_dir}"
        { /usr/bin/time -v python network_evaluation/network_stats/compute_cluster_stats.py \
            --network "${edge_file}" \
            --community "${com_file}" \
            --outdir "${stats_dir}"; } 2> "${stats_dir}/error.log"
    else log "Stats already done."; fi
}

run_accuracy() {
    if [ "${run_acc_flag}" -eq 0 ]; then return; fi
    if [ "${has_gt}" -eq 0 ]; then
        log "Warning: Accuracy evaluation requested but no ground-truth provided. Skipping."
        return
    fi
    local edge_file=$1; local gt_f=$2; local est_file=$3; local acc_d=$4
    log "Computing accuracy..."
    if [ ! -f "${acc_d}/done" ]; then
        mkdir -p "${acc_d}"
        { /usr/bin/time -v python network_evaluation/commdet_acc/compute_cd_accuracy.py \
            --input-network "${edge_file}" \
            --gt-clustering "${gt_f}" \
            --est-clustering "${est_file}" \
            --output-prefix "${acc_d}/result"; } 2> "${acc_d}/error.log"
    else log "Accuracy already done."; fi
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
# 1. Run Base Clustering
# ==========================================
suffix="${algo}"
out_dir="${base_root_clusterings}/${suffix}${opt_subpath}"
stats_dir="${base_root_stats}/${suffix}${opt_subpath}"
acc_dir="${base_root_acc}/${suffix}${opt_subpath}"

base_com="${out_dir}/com.csv"
base_done="${out_dir}/done"

log "Running clustering..."
if [ ! -f "${base_done}" ]; then
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
            log "Running flat-best..."
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
            log "Running nested-best..."
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

    [ -f "${base_com}" ] && touch "${base_done}"
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

    # 1. Symlink base stats folder
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

    # 1b. Symlink base acc folder
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

    # 2. Symlink post-processing & their respective stats/acc folders
    for base_pp in cc wcc cm; do
        if [ "${base_pp}" == "cc" ]; then
            if [ "${IS_RUN_CC}" -ne 1 ]; then continue; fi; pp_tag="cc"
        elif [ "${base_pp}" == "wcc" ]; then
            if [ "${IS_RUN_WCC}" -ne 1 ]; then continue; fi; pp_tag="wcc${CRIT_SUFFIX}"
        elif [ "${base_pp}" == "cm" ]; then
            if [ "${IS_RUN_CM}" -ne 1 ]; then continue; fi; pp_tag="cm${CRIT_SUFFIX}"
        fi

        # Cluster symlink (always created if post-processing is run)
        target_pp_clust="${base_root_clusterings}/${best_algo}+${pp_tag}${opt_subpath}"
        link_pp_clust="${base_root_clusterings}/${algo}+${pp_tag}${opt_subpath}"
        [ -L "${link_pp_clust}" ] && rm "${link_pp_clust}"
        if [ -e "${target_pp_clust}" ]; then
            mkdir -p "$(dirname "${link_pp_clust}")"
            ln -sfn "$(realpath "${target_pp_clust}")" "${link_pp_clust}"
        else
            log "Warning: Expected post-processing clustering ${target_pp_clust} not found. Symlink not created."
        fi

        # Stats symlink
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
        
        # Acc symlink
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
    
    if [ ! -f "${cc_done}" ]; then
        log "Running CC..."
        mkdir -p "${out_cc_dir}"
        { timeout "${TIMEOUT}" /usr/bin/time -v "${SCRIPT_DIR}/constrained-clustering/constrained_clustering" \
            MincutOnly --edgelist "${inp_edge}" --existing-clustering "${base_com}" \
            --num-processors 1 --output-file "${cc_com}" --log-file "${out_cc_dir}/cc.log" \
            --log-level 1 --connectedness-criterion 0; } 2> "${out_cc_dir}/error.log"
        [ -f "${cc_com}" ] && touch "${cc_done}"
    else log "CC already done."; fi
    
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
    
    if [ ! -f "${wcc_done}" ]; then
        log "Running WCC (${WCC_CRIT})..."
        mkdir -p "${out_wcc_dir}"
        { timeout "${TIMEOUT}" /usr/bin/time -v "${SCRIPT_DIR}/constrained-clustering/constrained_clustering" \
            MincutOnly --connectedness-criterion "${WCC_CRIT}" --edgelist "${inp_edge}" \
            --existing-clustering "${base_com}" --num-processors 1 --output-file "${wcc_com}" \
            --log-file "${out_wcc_dir}/wcc.log" --log-level 1; } 2> "${out_wcc_dir}/error.log"
        [ -f "${wcc_com}" ] && touch "${wcc_done}"
    else log "WCC already done."; fi

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
    
    if [ ! -f "${cm_done}" ]; then
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
        [ -f "${cm_com}" ] && touch "${cm_done}"
    else log "CM already done."; fi

    if [ -f "${cm_com}" ]; then
        run_stats "${inp_edge}" "${cm_com}" "${stats_cm_dir}"
        run_accuracy "${inp_edge}" "${gt_file}" "${cm_com}" "${acc_cm_dir}"
    fi
fi

log "[cd] ${algo} ${network_id:-[Custom]} ${dataset_type} ${criterion}" >> complete.log