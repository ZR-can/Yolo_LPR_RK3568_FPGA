#!/usr/bin/env bash

set -euo pipefail

if [[ $# -lt 5 || $# -gt 7 ]]; then
  echo "Usage: $0 MODEL_PATH DICTIONARY_PATH MANIFEST_DIR DATA_ROOT OUTPUT_DIR [WARMUP_COUNT=20] [PROGRESS_STEP=500]" >&2
  exit 2
fi

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
eval_bin="${script_dir}/ppocr_rec_eval_demo"
model_path=$(realpath "$1")
dictionary_path=$(realpath "$2")
manifest_dir=$(realpath "$3")
data_root=$(realpath "$4")
output_dir=$5
warmup_count=${6:-20}
progress_step=${7:-500}

[[ -x "${eval_bin}" ]] || {
  echo "Missing evaluator executable: ${eval_bin}" >&2
  exit 1
}
[[ "${warmup_count}" =~ ^[0-9]+$ ]] || {
  echo "Invalid warmup count: ${warmup_count}" >&2
  exit 2
}
[[ "${progress_step}" =~ ^[0-9]+$ ]] || {
  echo "Invalid progress step: ${progress_step}" >&2
  exit 2
}

mkdir -p "${output_dir}"
output_dir=$(realpath "${output_dir}")
manifest_list="${output_dir}/manifest_list.tsv"
details_path="${output_dir}/details.tsv"
log_path="${output_dir}/evaluation.log"

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

: >"${manifest_list}"
for index in "${!subset_names[@]}"; do
  subset=${subset_names[$index]}
  manifest="${manifest_dir}/${manifest_names[$index]}"
  [[ -f "${manifest}" ]] || {
    echo "Missing validation manifest: ${manifest}" >&2
    exit 1
  }
  printf '%s\t%s\n' "${subset}" "${manifest}" >>"${manifest_list}"
done

export RKNN_LOG_LEVEL=0

echo "RKNN model: ${model_path}"
echo "Dictionary: ${dictionary_path}"
echo "Manifest directory: ${manifest_dir}"
echo "Data root: ${data_root}"
echo "Output directory: ${output_dir}"

"${eval_bin}" \
  "${model_path}" \
  "${dictionary_path}" \
  "${manifest_list}" \
  "${data_root}" \
  "${details_path}" \
  "${warmup_count}" \
  "${progress_step}" \
  2>&1 \
  | sed -e '/^origin size=/d' -e '/^input image:/d' \
  | tee "${log_path}"

echo "Evaluation log: ${log_path}"
echo "Per-image details: ${details_path}"
