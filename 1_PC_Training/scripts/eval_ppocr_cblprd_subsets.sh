#!/usr/bin/env bash

set -euo pipefail

if [[ $# -lt 2 || $# -gt 4 ]]; then
  echo "Usage: $0 CONFIG_PATH CHECKPOINT_PREFIX [DATASET_ROOT] [METRIC_NAME]" >&2
  exit 2
fi

config_path=$(realpath "$1")
checkpoint_prefix=${2%.pdparams}
dataset_root=${3:-/root/autodl-tmp/CBLPRD}
metric_name=${4:-RecMetric}

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
paddleocr_root=$(realpath "${script_dir}/../PaddleOCR")
temporary_dir=$(mktemp -d)
trap 'rm -rf -- "${temporary_dir}"' EXIT

subset_names=(basic hard 使 学 港 澳 警 领)
manifest_names=(
  val_basic.txt
  val_hard.txt
  val_special_使.txt
  val_special_学.txt
  val_special_港.txt
  val_special_澳.txt
  val_special_警.txt
  val_special_领.txt
)

if [[ "${metric_name}" == "PlateRuleRecMetric" ]]; then
  printf "%-12s %10s %14s %14s %8s %8s\n" \
    "subset" "samples" "raw_accuracy" "rule_accuracy" "fixed" "harmed"
  printf "%-12s %10s %14s %14s %8s %8s\n" \
    "------------" "----------" "--------------" "--------------" "--------" "--------"
else
  printf "%-12s %10s %12s %12s\n" "subset" "samples" "correct" "accuracy"
  printf "%-12s %10s %12s %12s\n" "------------" "----------" "------------" "------------"
fi

cd "${paddleocr_root}"
for index in "${!subset_names[@]}"; do
  subset=${subset_names[$index]}
  manifest="${dataset_root}/${manifest_names[$index]}"
  log_path="${temporary_dir}/${index}.log"

  if [[ ! -f "${manifest}" ]]; then
    echo "Missing validation manifest: ${manifest}" >&2
    exit 1
  fi

  if ! python tools/eval.py \
    -c "${config_path}" \
    -o "Global.pretrained_model=null" \
       "Global.checkpoints=${checkpoint_prefix}" \
       "Eval.dataset.label_file_list=['${manifest}']" \
       "Eval.dataset.ratio_list=[1.0]" \
       "Metric.name=${metric_name}" \
       >"${log_path}" 2>&1; then
    cat "${log_path}" >&2
    exit 1
  fi

  accuracy=$(sed -n 's/.*INFO: acc:\([0-9.eE+-]*\).*/\1/p' "${log_path}" | tail -n 1)
  if [[ -z "${accuracy}" ]]; then
    cat "${log_path}" >&2
    echo "Failed to parse accuracy for subset ${subset}" >&2
    exit 1
  fi

  samples=$(wc -l <"${manifest}")
  percentage=$(awk -v accuracy="${accuracy}" \
    'BEGIN { printf "%.4f%%", accuracy * 100.0 }')

  if [[ "${metric_name}" == "PlateRuleRecMetric" ]]; then
    raw_accuracy=$(sed -n 's/.*INFO: raw_acc:\([0-9.eE+-]*\).*/\1/p' "${log_path}" | tail -n 1)
    fixed=$(sed -n 's/.*INFO: rule_fixed_num:\([0-9.eE+-]*\).*/\1/p' "${log_path}" | tail -n 1)
    harmed=$(sed -n 's/.*INFO: rule_harmed_num:\([0-9.eE+-]*\).*/\1/p' "${log_path}" | tail -n 1)
    if [[ -z "${raw_accuracy}" || -z "${fixed}" || -z "${harmed}" ]]; then
      cat "${log_path}" >&2
      echo "Failed to parse rule metrics for subset ${subset}" >&2
      exit 1
    fi
    raw_percentage=$(awk -v accuracy="${raw_accuracy}" \
      'BEGIN { printf "%.4f%%", accuracy * 100.0 }')
    printf "%-12s %10d %14s %14s %8d %8d\n" \
      "${subset}" "${samples}" "${raw_percentage}" "${percentage}" \
      "${fixed}" "${harmed}"
  else
    correct=$(awk -v accuracy="${accuracy}" -v samples="${samples}" \
      'BEGIN { printf "%d", int(accuracy * samples + 0.5) }')
    printf "%-12s %10d %12d %12s\n" \
      "${subset}" "${samples}" "${correct}" "${percentage}"
  fi
done
