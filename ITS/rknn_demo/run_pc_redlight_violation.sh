#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ITS_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

ONNX_MODEL="${ONNX_MODEL:-${SCRIPT_DIR}/model_convert/yolov8n_coco.onnx}"
CONFIG="${CONFIG:-${SCRIPT_DIR}/redlight_violation_config.json}"
INPUT_VIDEO="${INPUT_VIDEO:-${ITS_ROOT}/video/test1(1).mp4}"
OUT_VIDEO="${OUT_VIDEO:-${SCRIPT_DIR}/pc_outputs/redlight_violation/test1_1_redlight_violation.mp4}"
LIGHT_STATE="${LIGHT_STATE:-auto}"
MAX_FRAMES="${MAX_FRAMES:-0}"
FRAME_STRIDE="${FRAME_STRIDE:-1}"
OUTPUT_FPS="${OUTPUT_FPS:-30}"
CONF="${CONF:-0.25}"
NMS="${NMS:-0.50}"
CROP="${CROP:-}"

echo "==================================="
echo "ONNX_MODEL=${ONNX_MODEL}"
echo "CONFIG=${CONFIG}"
echo "INPUT_VIDEO=${INPUT_VIDEO}"
echo "OUT_VIDEO=${OUT_VIDEO}"
echo "LIGHT_STATE=${LIGHT_STATE}"
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

mkdir -p "$(dirname "${OUT_VIDEO}")"

crop_args=()
if [[ -n "${CROP}" ]]; then
  crop_args=(--crop "${CROP}")
fi

python3 "${SCRIPT_DIR}/pc_redlight_violation.py" \
  --onnx "${ONNX_MODEL}" \
  --input "${INPUT_VIDEO}" \
  --output "${OUT_VIDEO}" \
  --config "${CONFIG}" \
  --light-state "${LIGHT_STATE}" \
  --max-frames "${MAX_FRAMES}" \
  --frame-stride "${FRAME_STRIDE}" \
  --output-fps "${OUTPUT_FPS}" \
  --conf "${CONF}" \
  --nms "${NMS}" \
  "${crop_args[@]}" \
  --dump-shapes

echo "Done."
echo "Video: ${OUT_VIDEO}"
echo "CSV: ${OUT_VIDEO%.*}.csv"
