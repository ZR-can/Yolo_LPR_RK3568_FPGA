#!/bin/bash

set -euo pipefail

TARGET_SOC=rk3568
TARGET_ARCH=aarch64
BUILD_TYPE=Release
JOBS=4

while getopts ":t:a:b:j:" opt; do
    case "${opt}" in
        t) TARGET_SOC="${OPTARG}" ;;
        a) TARGET_ARCH="${OPTARG}" ;;
        b) BUILD_TYPE="${OPTARG}" ;;
        j) JOBS="${OPTARG}" ;;
        :) echo "Option -${OPTARG} requires an argument"; exit 1 ;;
        ?) echo "Unknown option: -${OPTARG}"; exit 1 ;;
    esac
done

case "${TARGET_SOC}" in
    rk3562|rk3566|rk3568|rk356x) CMAKE_TARGET_SOC=rk356x ;;
    *) echo "Unsupported target: ${TARGET_SOC}; this UI is validated for RK3568/rk356x"; exit 1 ;;
esac

GCC_COMPILER="${GCC_COMPILER:-/usr/bin/aarch64-linux-gnu}"
QT_ARM64_PREFIX="${QT_ARM64_PREFIX:-$HOME/Qt-5.12.9-arm64}"
CC="${GCC_COMPILER}-gcc"
CXX="${GCC_COMPILER}-g++"

if ! command -v "${CC}" >/dev/null 2>&1; then
    echo "Cross compiler is missing: ${CC}"
    exit 1
fi
if [ ! -x "${QT_ARM64_PREFIX}/bin/qmake" ]; then
    echo "Qt ARM64 qmake is missing: ${QT_ARM64_PREFIX}/bin/qmake"
    exit 1
fi

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
PLATFORM="${CMAKE_TARGET_SOC}_linux_${TARGET_ARCH}"
BUILD_DIR="${SCRIPT_DIR}/build/build_yolov8_ppocr_qt_${PLATFORM}_${BUILD_TYPE}"
INSTALL_DIR="${SCRIPT_DIR}/install/${PLATFORM}/rknn_yolov8_ppocr_qt_ui_demo"

cmake -S "${SCRIPT_DIR}" -B "${BUILD_DIR}" \
    -DTARGET_SOC="${CMAKE_TARGET_SOC}" \
    -DCMAKE_SYSTEM_NAME=Linux \
    -DCMAKE_SYSTEM_PROCESSOR="${TARGET_ARCH}" \
    -DCMAKE_C_COMPILER="${CC}" \
    -DCMAKE_CXX_COMPILER="${CXX}" \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DCMAKE_PREFIX_PATH="${QT_ARM64_PREFIX}" \
    -DQt5_DIR="${QT_ARM64_PREFIX}/lib/cmake/Qt5" \
    -DCMAKE_INSTALL_PREFIX="${INSTALL_DIR}"

cmake --build "${BUILD_DIR}" --parallel "${JOBS}"
cmake --install "${BUILD_DIR}"

LEGACY_FINETUNE_DIR="${INSTALL_DIR}/yolov8_ppocr_pcie_qt_ui_finetune_demo"
if [ -d "${LEGACY_FINETUNE_DIR}" ]; then
    cmake -E remove_directory "${LEGACY_FINETUNE_DIR}"
fi

EXECUTABLE="${INSTALL_DIR}/yolov8_ppocr_pcie_qt_ui/yolov8_ppocr_pcie_qt_ui"
if [ ! -x "${EXECUTABLE}" ]; then
    echo "Build finished but executable is missing: ${EXECUTABLE}"
    exit 1
fi
LAUNCHER="${INSTALL_DIR}/yolov8_ppocr_pcie_qt_ui/run-qt-demo.sh"
if [ ! -x "${LAUNCHER}" ]; then
    echo "Build finished but launcher is missing: ${LAUNCHER}"
    exit 1
fi
for FPGA_CONTROL_FILE in \
    "${INSTALL_DIR}/yolov8_ppocr_pcie_qt_ui/fpga_bar0_ctrl_test" \
    "${INSTALL_DIR}/yolov8_ppocr_pcie_qt_ui/fpga_preproc_ctrl.sh"; do
    if [ ! -x "${FPGA_CONTROL_FILE}" ]; then
        echo "Build finished but FPGA control file is missing or not executable: ${FPGA_CONTROL_FILE}"
        exit 1
    fi
done
YOLO_MODEL="${INSTALL_DIR}/yolov8_ppocr_pcie_qt_ui/model/yolov8.rknn"
YOLO_OBB_MODEL="${INSTALL_DIR}/yolov8_ppocr_pcie_qt_ui/model/yolov8_obb.rknn"
VIDEO_PPOCR_MODEL="${INSTALL_DIR}/yolov8_ppocr_pcie_qt_ui/model/ppocrv4_rec14_fold_affine_1x1_rk3568_hybrid_mmse_h2_add27_hsw4.rknn"
PPOCR_DICTIONARY="${INSTALL_DIR}/yolov8_ppocr_pcie_qt_ui/model/cblprd_plate_dict.txt"
PLATE_LABELS="${INSTALL_DIR}/yolov8_ppocr_pcie_qt_ui/model/labels_list.txt"
TRAFFIC_MODEL="${INSTALL_DIR}/yolov8_ppocr_pcie_qt_ui/model/traffic/yolov8_traffic_i8.rknn"
TRAFFIC_LABELS="${INSTALL_DIR}/yolov8_ppocr_pcie_qt_ui/model/traffic/labels_list.txt"
TRAFFIC_ROI_CONFIG="${INSTALL_DIR}/yolov8_ppocr_pcie_qt_ui/model/traffic/traffic_roi.conf"
IMAGE_PPOCR_MODEL="${INSTALL_DIR}/yolov8_ppocr_pcie_qt_ui/model/ppocrv4_rec14_fold_affine_1x1_rk3568_fp16.rknn"
for REQUIRED_MODE_FILE in \
    "${YOLO_MODEL}" \
    "${YOLO_OBB_MODEL}" \
    "${VIDEO_PPOCR_MODEL}" \
    "${PPOCR_DICTIONARY}" \
    "${PLATE_LABELS}" \
    "${TRAFFIC_MODEL}" \
    "${TRAFFIC_LABELS}" \
    "${TRAFFIC_ROI_CONFIG}" \
    "${IMAGE_PPOCR_MODEL}"; do
    if [ ! -f "${REQUIRED_MODE_FILE}" ]; then
        echo "Build finished but mode resource is missing: ${REQUIRED_MODE_FILE}"
        exit 1
    fi
done

DEPLOY_SOURCE_MODEL="${SCRIPT_DIR}/../3_NPU_Yolov8_PPOCR_Demo/model/finetune_i8.rknn"
if ! cmp -s "${DEPLOY_SOURCE_MODEL}" "${YOLO_MODEL}"; then
    echo "Installed YOLO model differs from: ${DEPLOY_SOURCE_MODEL}"
    exit 1
fi
DEPLOY_SOURCE_OBB_MODEL="${SCRIPT_DIR}/../2_Model_Conversion_PC_Simulation/yolov8_obb/model/yolov8_obb_i8.rknn"
if ! cmp -s "${DEPLOY_SOURCE_OBB_MODEL}" "${YOLO_OBB_MODEL}"; then
    echo "Installed image-mode OBB model differs from: ${DEPLOY_SOURCE_OBB_MODEL}"
    exit 1
fi

echo "Installed to: ${INSTALL_DIR}"
