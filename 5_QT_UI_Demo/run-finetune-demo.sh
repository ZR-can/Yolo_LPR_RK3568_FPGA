#!/bin/bash

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)

export LPR_QT_DEMO_DIR=/userdata/rknn_yolov8_ppocr_qt_ui_demo/yolov8_ppocr_pcie_qt_ui_finetune_demo
export LPR_QT_DEMO_BINARY=yolov8_ppocr_pcie_qt_ui
export LPR_QT_LAUNCHER_NAME="$0"

exec /bin/bash "${SCRIPT_DIR}/run-qt-demo.sh" "$@"
