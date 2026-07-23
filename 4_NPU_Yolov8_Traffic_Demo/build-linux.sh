#!/bin/bash

set -euo pipefail

TARGET_SOC="rk3568"
TARGET_ARCH="aarch64"
BUILD_TYPE="Release"
DISABLE_RGA="OFF"

while getopts ":t:a:b:r" opt; do
    case "$opt" in
        t) TARGET_SOC="$OPTARG" ;;
        a) TARGET_ARCH="$OPTARG" ;;
        b) BUILD_TYPE="$OPTARG" ;;
        r) DISABLE_RGA="ON" ;;
        :) echo "Option -$OPTARG requires an argument" >&2; exit 1 ;;
        *) echo "Usage: $0 [-t rk3568] [-a aarch64] [-b Release|Debug] [-r]" >&2; exit 1 ;;
    esac
done

case "$TARGET_SOC" in
    rk3562|rk3566|rk3568|rk356x) TARGET_SOC="rk356x" ;;
    *) echo "This demo currently targets RK356x; unsupported target: $TARGET_SOC" >&2; exit 1 ;;
esac

if [[ "$TARGET_ARCH" != "aarch64" ]]; then
    echo "RK3568 deployment requires TARGET_ARCH=aarch64" >&2
    exit 1
fi

GCC_PREFIX="${GCC_COMPILER:-aarch64-linux-gnu}"
CC_BIN="${GCC_PREFIX}-gcc"
CXX_BIN="${GCC_PREFIX}-g++"
if ! command -v "$CC_BIN" >/dev/null 2>&1 || ! command -v "$CXX_BIN" >/dev/null 2>&1; then
    echo "Cross compiler not found: $CC_BIN / $CXX_BIN" >&2
    echo "Set GCC_COMPILER to the toolchain prefix, for example aarch64-linux-gnu" >&2
    exit 1
fi

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
PLATFORM="${TARGET_SOC}_linux_${TARGET_ARCH}"
BUILD_DIR="${ROOT_DIR}/build/build_yolov8_traffic_${PLATFORM}_${BUILD_TYPE}"
INSTALL_DIR="${ROOT_DIR}/install/${PLATFORM}/rknn_yolov8_traffic_demo"
THIRDPARTY_ROOT="${TRAFFIC_3RDPARTY_ROOT:-${ROOT_DIR}/../3_NPU_Yolov8_PPOCR_Demo/3rdparty}"

cmake -S "$ROOT_DIR" -B "$BUILD_DIR" \
    -DTARGET_SOC="$TARGET_SOC" \
    -DCMAKE_SYSTEM_NAME=Linux \
    -DCMAKE_SYSTEM_PROCESSOR="$TARGET_ARCH" \
    -DCMAKE_C_COMPILER="$CC_BIN" \
    -DCMAKE_CXX_COMPILER="$CXX_BIN" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DDISABLE_RGA="$DISABLE_RGA" \
    -DTRAFFIC_3RDPARTY_ROOT="$THIRDPARTY_ROOT" \
    -DCMAKE_INSTALL_PREFIX="$INSTALL_DIR"

cmake --build "$BUILD_DIR" --parallel 4
cmake --install "$BUILD_DIR"

BENCHMARK_DIR="${INSTALL_DIR}/yolov8_traffic_benchmark"
PCIE_DIR="${INSTALL_DIR}/yolov8_traffic_pcie_demo"
test -x "${BENCHMARK_DIR}/yolov8_traffic_benchmark"
test -f "${BENCHMARK_DIR}/model/yolov8_traffic_i8.rknn"
test -f "${BENCHMARK_DIR}/model/labels_list.txt"
test -x "${PCIE_DIR}/yolov8_traffic_pcie_demo"
test -f "${PCIE_DIR}/model/yolov8_traffic_i8.rknn"
test -f "${PCIE_DIR}/model/labels_list.txt"
test -f "${PCIE_DIR}/pango_pci_driver.ko"

echo "Build complete: ${BENCHMARK_DIR}"
echo "Build complete: ${PCIE_DIR}"
