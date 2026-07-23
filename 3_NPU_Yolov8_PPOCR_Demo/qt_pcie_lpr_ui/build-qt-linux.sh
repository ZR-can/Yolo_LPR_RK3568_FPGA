#!/bin/bash

set -e

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
DEMO_ROOT=$(cd "${SCRIPT_DIR}/.." && pwd)

BUILD_TYPE=${BUILD_TYPE:-Release}
TARGET_SOC=${TARGET_SOC:-rk3568}
TARGET_ARCH=${TARGET_ARCH:-aarch64}

echo "This wrapper now builds the Qt UI through the main demo build script."
echo "Use ${DEMO_ROOT}/build-linux.sh -t ${TARGET_SOC} -a ${TARGET_ARCH} -d yolov8_lpr -q"

exec "${DEMO_ROOT}/build-linux.sh" \
    -t "${TARGET_SOC}" \
    -a "${TARGET_ARCH}" \
    -d yolov8_lpr \
    -b "${BUILD_TYPE}" \
    -q
