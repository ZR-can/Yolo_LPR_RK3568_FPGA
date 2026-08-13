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
BUILD_DIR="/tmp/qtu1280_build"
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

EXECUTABLE="${INSTALL_DIR}/yolov8_ppocr_pcie_qt_ui_1280x800/yolov8_ppocr_pcie_qt_ui_1280x800"
if [ ! -x "${EXECUTABLE}" ]; then
    echo "Build finished but executable is missing: ${EXECUTABLE}"
    exit 1
fi
FPGA_CTRL_TOOL="${INSTALL_DIR}/yolov8_ppocr_pcie_qt_ui_1280x800/fpga_bar0_ctrl_test"
FPGA_CTRL_SCRIPT="${INSTALL_DIR}/yolov8_ppocr_pcie_qt_ui_1280x800/fpga_preproc_ctrl.sh"
for REQUIRED_CTRL_FILE in \
    "${FPGA_CTRL_TOOL}" \
    "${FPGA_CTRL_SCRIPT}"; do
    if [ ! -x "${REQUIRED_CTRL_FILE}" ]; then
        echo "Build finished but FPGA control entry point is missing or not executable: ${REQUIRED_CTRL_FILE}"
        exit 1
    fi
done
TRAFFIC_MODEL="${INSTALL_DIR}/yolov8_ppocr_pcie_qt_ui_1280x800/model/traffic/yolov8_traffic_i8.rknn"
TRAFFIC_LABELS="${INSTALL_DIR}/yolov8_ppocr_pcie_qt_ui_1280x800/model/traffic/labels_list.txt"
IMAGE_PPOCR_MODEL="${INSTALL_DIR}/yolov8_ppocr_pcie_qt_ui_1280x800/model/ppocrv4_rec14_fold_affine_1x1_rk3568_fp16.rknn"
for REQUIRED_MODE_FILE in \
    "${TRAFFIC_MODEL}" \
    "${TRAFFIC_LABELS}" \
    "${IMAGE_PPOCR_MODEL}"; do
    if [ ! -f "${REQUIRED_MODE_FILE}" ]; then
        echo "Build finished but mode resource is missing: ${REQUIRED_MODE_FILE}"
        exit 1
    fi
done

echo "Installed to: ${INSTALL_DIR}"
