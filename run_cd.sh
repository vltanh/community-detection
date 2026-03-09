#!/bin/bash

# ==============================================================================
# Community Detection Evaluation Pipeline (run_cd.sh)
# ==============================================================================
# Computes base clusterings, selects best SBM models, refines via constrained
# clustering, and evaluates statistics (and accuracy for synthetic networks).
#
# USAGE:
#   Real:   ./run_cd.sh --algo <algo> --network <id> --real [OPTIONS]
#   Synth:  ./run_cd.sh --algo <algo> --network <id> --generator <gen> --gt-clustering <gt> [OPTIONS]
#   Custom: ./run_cd.sh --algo <algo> --input <path> --out-clustering <dir> [OPTIONS]
#
# OPTIONS:
#   --run-id <id>      : (Synthetic) Identifier for the run (default: 0).
#   --criterion <name> : Connectedness criterion for WCC and CM (e.g., 'sqrt' or 'log').
#                        If provided, outputs are suffixed (e.g., +wcc(sqrt)).
#   --input <path>     : Custom path to the input edge list CSV.
#   --out-clustering <d>: Custom output directory for clusterings.
#   --out-stats <dir>  : Custom output directory for stats (required unless --skip-stats).
#   --out-acc <dir>    : Custom output directory for accuracy (required if --gt is passed and not --skip-acc).
#   --gt <path>        : Custom path to the ground-truth clustering CSV (triggers acc eval).
#   --skip-stats       : Bypasses network statistics computation.
#   --skip-acc         : Bypasses accuracy evaluation against ground truth.
#
# PATH LEGEND:
#   [INP_EDGE]
#       Real   -> data/empirical_networks/netzschleuder/<network_id>/<network_id>.csv
#       Synth  -> data/synthetic_networks/networks/<generator>/<gt_clustering>/<network_id>/<run_id>/edge.csv
#       Custom -> <input_path>
#   [GT_COM]
#       Synth  -> data/reference_clusterings/clusterings/<gt_clustering>/<network_id>/com.csv
#       Custom -> <gt_path>
#   [OUT_ROOT]
#       Real/Synth -> Standardized data/ hierarchy with <network_id>/<run_id> subpaths.
#       Custom     -> Explicitly mapped to --out-clustering, --out-stats, --out-acc.
#   [SUB_PATH]
#       Real   -> <network_id>
#       Synth  -> <network_id>/<run_id>
#       Custom -> N/A (direct paths used)
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
# STEP 3: Statistics Computation (Skipped if --skip-stats is set)
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
# STEP 4: Accuracy Evaluation (Skipped if --skip-acc is set, or no GT available)
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
log() {
    builtin echo "[$(date +'%Y-%m-%d %H:%M:%S')] $*"
}

get_clust_dir() {
    if [ "${is_custom}" -eq 1 ]; then echo "${custom_out_clust}/$1"
    else echo "${base_root_clusterings}/$1/${out_subpath}"; fi
}

get_stats_dir() {
    if [ "${is_custom}" -eq 1 ]; then echo "${custom_out_stats}/$1"
    else echo "${base_root_stats}/$1/${out_subpath}"; fi
}

get_acc_dir() {
    if [ "${is_custom}" -eq 1 ]; then echo "${custom_out_acc}/$1"
    else echo "${base_root_acc}/$1/${out_subpath}"; fi
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

custom_input=""
custom_out_clust=""
custom_out_stats=""
custom_out_acc=""
custom_gt=""

skip_stats=0
skip_acc=0

while [[ "$#" -gt 0 ]]; do
    case $1 in
        --algo) algo="$2"; shift 2 ;;
        --network) network_id="$2"; shift 2 ;;
        --real) is_real=1; shift 1 ;;
        --generator) generator="$2"; shift 2 ;;
        --gt-clustering) gt_clustering="$2"; shift 2 ;;
        --run-id) run_id="$2"; shift 2 ;;
        --criterion) criterion="$2"; shift 2 ;;
        --input) custom_input="$2"; shift 2 ;;
        --out-clustering) custom_out_clust="$2"; shift 2 ;;
        --out-stats) custom_out_stats="$2"; shift 2 ;;
        --out-acc) custom_out_acc="$2"; shift 2 ;;
        --gt) custom_gt="$2"; shift 2 ;;
        --skip-stats) skip_stats=1; shift 1 ;;
        --skip-acc) skip_acc=1; shift 1 ;;
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

# Flags
IS_RUN_CC=1; IS_RUN_WCC=1; IS_RUN_CM=1

case ${algo} in
    ikc*) IS_RUN_CC=0; IS_RUN_WCC=0; IS_RUN_CM=0 ;;
    leiden*) IS_RUN_CC=0; IS_RUN_WCC=0 ;;
    sbm*) IS_RUN_CM=0 ;;
    infomap) IS_RUN_WCC=0; IS_RUN_CM=0 ;;
esac

# ==========================================
# Input/Output Path Routing (Real vs Synth vs Custom)
# ==========================================
is_custom=0
has_gt=0

if [ -n "${custom_input}" ]; then
    is_custom=1
    if [ -z "${custom_out_clust}" ]; then
        log "Error: --out-clustering must be provided if --input is used."
        exit 1
    fi
    if [ "${skip_stats}" -eq 0 ] && [ -z "${custom_out_stats}" ]; then
        log "Error: --out-stats must be provided if --input is used, unless --skip-stats is set."
        exit 1
    fi
    dataset_type="custom"
    inp_edge="${custom_input}"
    
    if [ -n "${custom_gt}" ]; then
        if [ "${skip_acc}" -eq 0 ] && [ -z "${custom_out_acc}" ]; then
            log "Error: --out-acc must be provided if --gt is supplied, unless --skip-acc is set."
            exit 1
        fi
        gt_file="${custom_gt}"
        has_gt=1
    fi

elif [ "${is_real}" -eq 1 ]; then
    if [ -z "${network_id}" ]; then log "Error: --network required for real datasets."; exit 1; fi
    dataset_type="real"
    inp_edge="data/empirical_networks/netzschleuder/${network_id}/${network_id}.csv"
    base_root_clusterings="data/reference_clusterings/clusterings"
    base_root_stats="data/reference_clusterings/stats"
    out_subpath="${network_id}"

else
    if [ -z "${network_id}" ]; then log "Error: --network required for synthetic datasets."; exit 1; fi
    if [ -z "${generator}" ] || [ -z "${gt_clustering}" ]; then
        log "Error: For synthetic networks, --generator and --gt-clustering must be provided."
        exit 1
    fi

    dataset_type="${generator}/${gt_clustering} (run: ${run_id})"
    inp_edge="data/synthetic_networks/networks/${generator}/${gt_clustering}/${network_id}/${run_id}/edge.csv"
    COMMDET_BASE="data/estimated_clusterings/${generator}/${gt_clustering}"
    base_root_clusterings="${COMMDET_BASE}/clusterings"
    base_root_stats="${COMMDET_BASE}/stats"
    base_root_acc="${COMMDET_BASE}/acc"
    out_subpath="${network_id}/${run_id}"
    gt_file="data/reference_clusterings/clusterings/${gt_clustering}/${network_id}/com.csv"
    has_gt=1
fi

if [ ! -f "${inp_edge}" ]; then
    log "Input file ${inp_edge} does not exist. Skipping."
    exit 1
fi

# ==========================================
# Functions: Stats and Accuracy
# ==========================================
run_stats() {
    if [ "${skip_stats}" -eq 1 ]; then return; fi
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
    if [ "${skip_acc}" -eq 1 ]; then return; fi
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
out_dir=$(get_clust_dir "${suffix}")
stats_dir=$(get_stats_dir "${suffix}")
acc_dir=$(get_acc_dir "${suffix}")

base_com="${out_dir}/com.csv"
base_done="${out_dir}/done"

log "Running clustering..."
if [ ! -f "${base_done}" ]; then
    if [[ ${algo} == leiden* ]]; then
        if [[ ${leiden_model} == cpm ]]; then
            mkdir -p "${out_dir}"
            { timeout "${TIMEOUT}" /usr/bin/time -v python src/leiden/run_leiden.py \
                --edgelist "${inp_edge}" --output-directory "${out_dir}" \
                --model cpm --resolution "${leiden_res}"; } 2> "${out_dir}/error.log"
        elif [[ ${leiden_model} == mod ]]; then
            mkdir -p "${out_dir}"
            { timeout "${TIMEOUT}" /usr/bin/time -v python src/leiden/run_leiden.py \
                --edgelist "${inp_edge}" --output-directory "${out_dir}" --model mod; } 2> "${out_dir}/error.log"
        else
            log "Unknown leiden_model: ${leiden_model}"; exit 1
        fi
    elif [[ ${algo} == infomap ]]; then
        mkdir -p "${out_dir}"
        { timeout "${TIMEOUT}" /usr/bin/time -v python src/infomap/run_infomap.py \
            --edgelist "${inp_edge}" --output-directory "${out_dir}"; } 1> "${out_dir}/output.log" 2> "${out_dir}/error.log"
    elif [[ ${algo} == ikc* ]]; then
        mkdir -p "${out_dir}"
        { timeout "${TIMEOUT}" /usr/bin/time -v python src/ikc/run_ikc.py \
            --edgelist "${inp_edge}" --output-directory "${out_dir}" --kvalue "${ikc_k}"; } 1> "${out_dir}/output.log" 2> "${out_dir}/error.log"
    elif [[ ${algo} == sbm* ]]; then
        if [[ ${sbm_model} =~ ^(flat-dc|flat-ndc|flat-pp|nested-dc|nested-ndc)$ ]]; then
            log "Running ${sbm_model}..."
            mkdir -p "${out_dir}"
            { timeout "${TIMEOUT}" /usr/bin/time -v python src/sbm/run_sbm.py \
                --edgelist "${inp_edge}" --output-directory "${out_dir}" --method "${sbm_model}"; } 2> "${out_dir}/error.log"
        elif [[ ${sbm_model} == "flat-best" ]]; then
            log "Running flat-best..."
            sbm_flat_dc_root=$(get_clust_dir "sbm-flat-dc")
            sbm_flat_ndc_root=$(get_clust_dir "sbm-flat-ndc")
            sbm_flat_pp_root=$(get_clust_dir "sbm-flat-pp")
            
            mkdir -p "${out_dir}"
            { timeout "${TIMEOUT}" /usr/bin/time -v python src/sbm/choose_best_sbm.py \
                --entropy_files "${sbm_flat_dc_root}/entropy.txt" "${sbm_flat_ndc_root}/entropy.txt" "${sbm_flat_pp_root}/entropy.txt" \
                --com_files "${sbm_flat_dc_root}/com.csv" "${sbm_flat_ndc_root}/com.csv" "${sbm_flat_pp_root}/com.csv" \
                --out_dir "${out_dir}"; } 1> "${out_dir}/out.log" 2> "${out_dir}/error.log"
            
            [ -f "${out_dir}/best_model.txt" ] && log "Best SBM selected: $(cat "${out_dir}/best_model.txt")" || log "Error: best_model.txt missing."
        elif [[ ${sbm_model} == "nested-best" ]]; then
            log "Running nested-best..."
            sbm_nested_dc_root=$(get_clust_dir "sbm-nested-dc")
            sbm_nested_ndc_root=$(get_clust_dir "sbm-nested-ndc")
            
            mkdir -p "${out_dir}"
            { timeout "${TIMEOUT}" /usr/bin/time -v python src/sbm/choose_best_sbm.py \
                --entropy_files "${sbm_nested_dc_root}/entropy.txt" "${sbm_nested_ndc_root}/entropy.txt" \
                --com_files "${sbm_nested_dc_root}/com.csv" "${sbm_nested_ndc_root}/com.csv" \
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
    if [ "${skip_stats}" -eq 0 ]; then
        target_stats=$(get_stats_dir "${best_algo}")
        link_stats=$(get_stats_dir "${algo}")
        if [ -e "${target_stats}" ]; then
            mkdir -p "$(dirname "${link_stats}")"
            ln -sfn "$(realpath "${target_stats}")" "${link_stats}"
        fi
    fi

    # 1b. Symlink base acc folder
    if [ "${has_gt}" -eq 1 ] && [ "${skip_acc}" -eq 0 ]; then
        target_acc=$(get_acc_dir "${best_algo}")
        link_acc=$(get_acc_dir "${algo}")
        if [ -e "${target_acc}" ]; then
            mkdir -p "$(dirname "${link_acc}")"
            ln -sfn "$(realpath "${target_acc}")" "${link_acc}"
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

        target_pp_clust=$(get_clust_dir "${best_algo}+${pp_tag}")
        link_pp_clust=$(get_clust_dir "${algo}+${pp_tag}")
        if [ -e "${target_pp_clust}" ]; then
            mkdir -p "$(dirname "${link_pp_clust}")"
            ln -sfn "$(realpath "${target_pp_clust}")" "${link_pp_clust}"
        fi

        if [ "${skip_stats}" -eq 0 ]; then
            target_pp_stats=$(get_stats_dir "${best_algo}+${pp_tag}")
            link_pp_stats=$(get_stats_dir "${algo}+${pp_tag}")
            if [ -e "${target_pp_stats}" ]; then
                mkdir -p "$(dirname "${link_pp_stats}")"
                ln -sfn "$(realpath "${target_pp_stats}")" "${link_pp_stats}"
            fi
        fi
        
        if [ "${has_gt}" -eq 1 ] && [ "${skip_acc}" -eq 0 ]; then
            target_pp_acc=$(get_acc_dir "${best_algo}+${pp_tag}")
            link_pp_acc=$(get_acc_dir "${algo}+${pp_tag}")
            if [ -e "${target_pp_acc}" ]; then
                mkdir -p "$(dirname "${link_pp_acc}")"
                ln -sfn "$(realpath "${target_pp_acc}")" "${link_pp_acc}"
            fi
        fi
    done

    log "[cd-done] ${algo} ${network_id:-[Custom]} ${dataset_type}" >> complete.log
    exit 0
fi

run_stats "${inp_edge}" "${base_com}" "${stats_dir}"
if [ "${has_gt}" -eq 1 ]; then run_accuracy "${inp_edge}" "${gt_file}" "${base_com}" "${acc_dir}"; fi

# ==========================================
# 2. Run CC
# ==========================================
if [ "${IS_RUN_CC}" -eq 1 ]; then
    suffix="${algo}+cc"
    out_cc_dir=$(get_clust_dir "${suffix}")
    stats_cc_dir=$(get_stats_dir "${suffix}")
    acc_cc_dir=$(get_acc_dir "${suffix}")
    cc_com="${out_cc_dir}/com.csv"
    cc_done="${out_cc_dir}/done"
    
    if [ ! -f "${cc_done}" ]; then
        log "Running CC..."
        mkdir -p "${out_cc_dir}"
        { timeout "${TIMEOUT}" /usr/bin/time -v ./constrained-clustering/constrained_clustering \
            MincutOnly --edgelist "${inp_edge}" --existing-clustering "${base_com}" \
            --num-processors 1 --output-file "${cc_com}" --log-file "${out_cc_dir}/cc.log" \
            --log-level 1 --connectedness-criterion 0; } 2> "${out_cc_dir}/error.log"
        [ -f "${cc_com}" ] && touch "${cc_done}"
    else log "CC already done."; fi
    
    if [ -f "${cc_com}" ]; then
        run_stats "${inp_edge}" "${cc_com}" "${stats_cc_dir}"
        [ "${has_gt}" -eq 1 ] && run_accuracy "${inp_edge}" "${gt_file}" "${cc_com}" "${acc_cc_dir}"
    fi
fi

# ==========================================
# 3. Run WCC 
# ==========================================
if [ "${IS_RUN_WCC}" -eq 1 ]; then
    suffix="${algo}+wcc${CRIT_SUFFIX}"
    out_wcc_dir=$(get_clust_dir "${suffix}")
    stats_wcc_dir=$(get_stats_dir "${suffix}")
    acc_wcc_dir=$(get_acc_dir "${suffix}")
    wcc_com="${out_wcc_dir}/com.csv"
    wcc_done="${out_wcc_dir}/done"
    
    if [ ! -f "${wcc_done}" ]; then
        log "Running WCC (${WCC_CRIT})..."
        mkdir -p "${out_wcc_dir}"
        { timeout "${TIMEOUT}" /usr/bin/time -v ./constrained-clustering/constrained_clustering \
            MincutOnly --connectedness-criterion "${WCC_CRIT}" --edgelist "${inp_edge}" \
            --existing-clustering "${base_com}" --num-processors 1 --output-file "${wcc_com}" \
            --log-file "${out_wcc_dir}/wcc.log" --log-level 1; } 2> "${out_wcc_dir}/error.log"
        [ -f "${wcc_com}" ] && touch "${wcc_done}"
    else log "WCC already done."; fi

    if [ -f "${wcc_com}" ]; then
        run_stats "${inp_edge}" "${wcc_com}" "${stats_wcc_dir}"
        [ "${has_gt}" -eq 1 ] && run_accuracy "${inp_edge}" "${gt_file}" "${wcc_com}" "${acc_wcc_dir}"
    fi
fi

# ==========================================
# 4. Run CM
# ==========================================
if [ "${IS_RUN_CM}" -eq 1 ]; then
    suffix="${algo}+cm${CRIT_SUFFIX}"
    out_cm_dir=$(get_clust_dir "${suffix}")
    stats_cm_dir=$(get_stats_dir "${suffix}")
    acc_cm_dir=$(get_acc_dir "${suffix}")
    cm_com="${out_cm_dir}/com.csv"
    cm_done="${out_cm_dir}/done"
    
    if [ ! -f "${cm_done}" ]; then
        log "Running CM (${CM_CRIT})..."
        if [[ ${algo} == leiden* ]]; then
            if [[ ${leiden_model} == cpm ]]; then
                mkdir -p "${out_cm_dir}"
                { timeout "${TIMEOUT}" /usr/bin/time -v ./constrained-clustering/constrained_clustering \
                    CM --mincut-type "cactus" --connectedness-criterion "${CM_CRIT}" \
                    --edgelist "${inp_edge}" --existing-clustering "${base_com}" \
                    --algorithm "leiden-cpm" --clustering-parameter "${leiden_res}" \
                    --num-processors 1 --output-file "${cm_com}" --history-file "${out_cm_dir}/history.log" \
                    --log-file "${out_cm_dir}/cm.log" --log-level 1; } 2> "${out_cm_dir}/error.log"
            elif [[ ${leiden_model} == mod ]]; then
                mkdir -p "${out_cm_dir}"
                { timeout "${TIMEOUT}" /usr/bin/time -v ./constrained-clustering/constrained_clustering \
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
        [ "${has_gt}" -eq 1 ] && run_accuracy "${inp_edge}" "${gt_file}" "${cm_com}" "${acc_cm_dir}"
    fi
fi

log "[cd-done] ${algo} ${network_id:-[Custom]} ${dataset_type}" >> complete.log