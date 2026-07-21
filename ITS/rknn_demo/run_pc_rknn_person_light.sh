#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ITS_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

RKNN_MODEL="${RKNN_MODEL:-${SCRIPT_DIR}/model_convert/yolov8n_coco_fp.rknn}"
ONNX_MODEL="${ONNX_MODEL:-${SCRIPT_DIR}/model_convert/yolov8n_coco.onnx}"
VIDEO1="${VIDEO1:-${ITS_ROOT}/video/test1.mp4}"
VIDEO2="${VIDEO2:-${ITS_ROOT}/video/test2.mp4}"
OUT_DIR="${OUT_DIR:-${SCRIPT_DIR}/pc_outputs/person_light}"
MAX_FRAMES="${MAX_FRAMES:-300}"
FRAME_STRIDE="${FRAME_STRIDE:-1}"
OUTPUT_FPS="${OUTPUT_FPS:-0}"
CONF="${CONF:-0.35}"
NMS="${NMS:-0.50}"
CROP="${CROP:-}"

echo "==================================="
echo "RKNN_MODEL=${RKNN_MODEL}"
echo "ONNX_MODEL=${ONNX_MODEL}"
echo "VIDEO1=${VIDEO1}"
echo "VIDEO2=${VIDEO2}"
echo "OUT_DIR=${OUT_DIR}"
echo "MAX_FRAMES=${MAX_FRAMES}"
echo "FRAME_STRIDE=${FRAME_STRIDE}"
echo "OUTPUT_FPS=${OUTPUT_FPS}"
echo "CONF=${CONF}"
echo "NMS=${NMS}"
echo "CROP=${CROP}"
echo "==================================="

python3 - <<'PY'
import importlib.util
missing = []
for name in ("cv2", "numpy", "rknn"):
    if importlib.util.find_spec(name) is None:
        missing.append(name)
if missing:
    raise SystemExit("Missing Python packages: " + ", ".join(missing))
PY

mkdir -p "${OUT_DIR}"

run_one() {
  local input_video="$1"
  local stem="$2"
  if [[ ! -f "${input_video}" ]]; then
    echo "ERROR: input video not found: ${input_video}" >&2
    exit 2
  fi

  local crop_args=()
  if [[ -n "${CROP}" ]]; then
    crop_args=(--crop "${CROP}")
  fi

  python3 "${SCRIPT_DIR}/pc_rknn_person_light.py" \
    --rknn "${RKNN_MODEL}" \
    --onnx "${ONNX_MODEL}" \
    --source onnx \
    --input "${input_video}" \
    --output "${OUT_DIR}/${stem}_person_light.mp4" \
    --mode video \
    --max-frames "${MAX_FRAMES}" \
    --frame-stride "${FRAME_STRIDE}" \
    --output-fps "${OUTPUT_FPS}" \
    --conf "${CONF}" \
    --nms "${NMS}" \
    "${crop_args[@]}" \
    --dump-shapes
}

run_one "${VIDEO1}" "test1"
run_one "${VIDEO2}" "test2"

echo "Done."
echo "Outputs:"
echo "  ${OUT_DIR}/test1_person_light.mp4"
echo "  ${OUT_DIR}/test1_person_light.csv"
echo "  ${OUT_DIR}/test2_person_light.mp4"
echo "  ${OUT_DIR}/test2_person_light.csv"
