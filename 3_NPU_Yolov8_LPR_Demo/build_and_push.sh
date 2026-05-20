#!/bin/bash

set -euo pipefail

TARGET_SOC="rk3568"
TARGET_ARCH="aarch64"
BUILD_DEMO_NAME="yolov8_lpr"
BUILD_TYPE=""
ENABLE_ASAN=0
DISABLE_RGA=0
SKIP_PUSH=0
ADB_BIN="${ADB_BIN:-adb}"
BOARD_ROOT="/userdata/rknn_yolov8_lpr_demo"

show_usage() {
    cat <<EOF
Usage: $0 [-t target_soc] [-a target_arch] [-d demo_name] [-b build_type] [-m] [-r] [-n]

Default:
  -t rk3568
  -a aarch64
  -d yolov8_lpr

Options:
  -t  target SOC
  -a  target arch
  -d  demo name
  -b  build type (Debug/Release)
  -m  enable ASAN
  -r  disable RGA
  -n  build only, skip adb push
EOF
}

while getopts ":t:a:d:b:mrn" opt; do
    case "$opt" in
        t) TARGET_SOC="$OPTARG" ;;
        a) TARGET_ARCH="$OPTARG" ;;
        d) BUILD_DEMO_NAME="$OPTARG" ;;
        b) BUILD_TYPE="$OPTARG" ;;
        m) ENABLE_ASAN=1 ;;
        r) DISABLE_RGA=1 ;;
        n) SKIP_PUSH=1 ;;
        :) echo "Option -$OPTARG requires an argument." ; exit 1 ;;
        \?) show_usage ; exit 1 ;;
    esac
done

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
BUILD_SCRIPT="${SCRIPT_DIR}/build-linux.sh"

if ! command -v "$ADB_BIN" >/dev/null 2>&1; then
    echo "adb is not available: ${ADB_BIN}"
    exit 1
fi

if [ ! -x "$BUILD_SCRIPT" ]; then
    echo "Build script not found or not executable: $BUILD_SCRIPT"
    exit 1
fi

case "$TARGET_SOC" in
    rk3568|rk3566|rk3562) TARGET_SOC_DIR="rk356x" ;;
    rv1103) TARGET_SOC_DIR="rv1106" ;;
    rv1126) TARGET_SOC_DIR="rv1126" ;;
    *) TARGET_SOC_DIR="$TARGET_SOC" ;;
esac

TARGET_PLATFORM="${TARGET_SOC_DIR}_linux"
if [ -n "$TARGET_ARCH" ]; then
    TARGET_PLATFORM="${TARGET_PLATFORM}_${TARGET_ARCH}"
fi

TARGET_SDK="rknn_${BUILD_DEMO_NAME}_demo"
INSTALL_DIR="${SCRIPT_DIR}/install/${TARGET_PLATFORM}/${TARGET_SDK}"
BUILD_DIR="${SCRIPT_DIR}/build/build_${TARGET_SDK}_${TARGET_PLATFORM}_${BUILD_TYPE:-Release}"

if command -v git >/dev/null 2>&1; then
    CURRENT_BRANCH=$(git -C "$SCRIPT_DIR" branch --show-current 2>/dev/null || true)
    CURRENT_COMMIT=$(git -C "$SCRIPT_DIR" rev-parse --short HEAD 2>/dev/null || true)
    echo "git branch: ${CURRENT_BRANCH:-detached}"
    echo "git commit: ${CURRENT_COMMIT:-unknown}"
    if [ -z "${CURRENT_BRANCH:-}" ]; then
        echo "Refusing to build from detached HEAD."
        exit 1
    fi
fi

echo "Cleaning build directory: $BUILD_DIR"
rm -rf "$BUILD_DIR"

BUILD_ARGS=(-t "$TARGET_SOC" -a "$TARGET_ARCH" -d "$BUILD_DEMO_NAME")
if [ -n "$BUILD_TYPE" ]; then
    BUILD_ARGS+=(-b "$BUILD_TYPE")
fi
if [ "$ENABLE_ASAN" -eq 1 ]; then
    BUILD_ARGS+=(-m)
fi
if [ "$DISABLE_RGA" -eq 1 ]; then
    BUILD_ARGS+=(-r)
fi

"$BUILD_SCRIPT" "${BUILD_ARGS[@]}"

if [ "$SKIP_PUSH" -eq 1 ]; then
    echo "Build completed. Push skipped by -n."
    exit 0
fi

if [ ! -d "$INSTALL_DIR" ]; then
    echo "Install directory not found: $INSTALL_DIR"
    exit 1
fi

echo "Removing old demo on device: $BOARD_ROOT"
"$ADB_BIN" shell rm -rf "$BOARD_ROOT"

echo "Pushing demo tree to device"
"$ADB_BIN" push "$INSTALL_DIR" /userdata/

echo "Restoring execute bits on device"
"$ADB_BIN" shell "chmod +x ${BOARD_ROOT}/yolov8_lpr_picture_demo/yolov8_lpr_picture_demo"
"$ADB_BIN" shell "chmod +x ${BOARD_ROOT}/yolov8_lpr_video_demo/yolov8_lpr_video_demo"

echo "Done. Manual runtime verification is still required."
