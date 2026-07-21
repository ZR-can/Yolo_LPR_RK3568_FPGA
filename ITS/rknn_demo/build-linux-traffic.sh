#!/bin/bash

set -e

echo "$0 $@"

while getopts ":t:a:b:r" opt; do
  case $opt in
    t)
      TARGET_SOC=$OPTARG
      ;;
    a)
      TARGET_ARCH=$OPTARG
      ;;
    b)
      BUILD_TYPE=$OPTARG
      ;;
    r)
      DISABLE_RGA=ON
      ;;
    :)
      echo "Option -$OPTARG requires an argument."
      exit 1
      ;;
    ?)
      echo "Invalid option: -$OPTARG index:$OPTIND"
      exit 1
      ;;
  esac
done

if [[ -z ${TARGET_SOC} ]]; then
  TARGET_SOC=rk3568
fi

if [[ -z ${TARGET_ARCH} ]]; then
  TARGET_ARCH=aarch64
fi

if [[ -z ${BUILD_TYPE} ]]; then
  BUILD_TYPE=Release
fi

if [[ -z ${DISABLE_RGA} ]]; then
  DISABLE_RGA=OFF
fi

case ${TARGET_SOC} in
  rk3568|rk3566|rk3562|rk356x)
    TARGET_SOC_CMAKE=rk356x
    TARGET_PLATFORM=rk356x_linux_${TARGET_ARCH}
    ;;
  rk3588|rk3576)
    TARGET_SOC_CMAKE=${TARGET_SOC}
    TARGET_PLATFORM=${TARGET_SOC}_linux_${TARGET_ARCH}
    ;;
  *)
    echo "Invalid target: ${TARGET_SOC}"
    echo "Valid target: rk3568,rk3566,rk3562,rk356x,rk3588,rk3576"
    exit 1
    ;;
esac

if [[ -z ${GCC_COMPILER} ]]; then
  GCC_COMPILER=aarch64-linux-gnu
fi

export CC=${GCC_COMPILER}-gcc
export CXX=${GCC_COMPILER}-g++

if ! command -v ${CC} >/dev/null 2>&1; then
  echo "${CC} is not available"
  echo "Please set GCC_COMPILER, for example:"
  echo "export GCC_COMPILER=\$HOME/gcc-linaro-6.3.1-2017.05-x86_64_aarch64-linux-gnu/bin/aarch64-linux-gnu"
  exit 1
fi

ROOT_PWD=$( cd "$( dirname "$0" )" && pwd )

if [[ -z ${YOLO_LPR_DEMO_ROOT} ]]; then
  CANDIDATE_ROOT="${ROOT_PWD}/../../Yolo_LPR_RK3568_FPGA/3_NPU_Yolov8_LPR_Demo"
  if [[ -f "${CANDIDATE_ROOT}/CMakeLists.txt" ]]; then
    YOLO_LPR_DEMO_ROOT=$(cd "${CANDIDATE_ROOT}" && pwd)
  else
    echo "YOLO_LPR_DEMO_ROOT is not set and default path was not found:"
    echo "${CANDIDATE_ROOT}"
    echo "Please export YOLO_LPR_DEMO_ROOT=/path/to/3_NPU_Yolov8_LPR_Demo"
    exit 1
  fi
fi

INSTALL_DIR=${ROOT_PWD}/install/${TARGET_PLATFORM}/its_traffic_benchmark
BUILD_DIR=${ROOT_PWD}/build/build_its_traffic_benchmark_${TARGET_PLATFORM}_${BUILD_TYPE}

echo "==================================="
echo "TARGET_SOC=${TARGET_SOC_CMAKE}"
echo "TARGET_ARCH=${TARGET_ARCH}"
echo "BUILD_TYPE=${BUILD_TYPE}"
echo "DISABLE_RGA=${DISABLE_RGA}"
echo "YOLO_LPR_DEMO_ROOT=${YOLO_LPR_DEMO_ROOT}"
echo "INSTALL_DIR=${INSTALL_DIR}"
echo "BUILD_DIR=${BUILD_DIR}"
echo "CC=${CC}"
echo "CXX=${CXX}"
echo "==================================="

rm -rf "${INSTALL_DIR}" "${BUILD_DIR}"
mkdir -p "${BUILD_DIR}"

cd "${BUILD_DIR}"

cmake "${ROOT_PWD}" \
  -DTARGET_SOC=${TARGET_SOC_CMAKE} \
  -DCMAKE_SYSTEM_NAME=Linux \
  -DCMAKE_SYSTEM_PROCESSOR=${TARGET_ARCH} \
  -DCMAKE_C_COMPILER=${CC} \
  -DCMAKE_CXX_COMPILER=${CXX} \
  -DCMAKE_BUILD_TYPE=${BUILD_TYPE} \
  -DDISABLE_RGA=${DISABLE_RGA} \
  -DYOLO_LPR_DEMO_ROOT="${YOLO_LPR_DEMO_ROOT}" \
  -DCMAKE_INSTALL_PREFIX="${INSTALL_DIR}"

make -j4
make install

MODEL_DIR="${INSTALL_DIR}/yolov8_traffic_benchmark/model"
TEST_IMAGE_SRC="${ROOT_PWD}/test_images"
TEST_IMAGE_DST="${INSTALL_DIR}/yolov8_traffic_benchmark/test_images"
TEST_FRAME_SRC="${ROOT_PWD}/test_frames"
TEST_FRAME_DST="${INSTALL_DIR}/yolov8_traffic_benchmark/test_frames"
TEST_VIDEO_SRC="${ROOT_PWD}/test_videos"
TEST_VIDEO_DST="${INSTALL_DIR}/yolov8_traffic_benchmark/test_videos"
FP_MODEL_SRC="${ROOT_PWD}/model_convert/yolov8n_coco_fp.rknn"

if [[ -f "${FP_MODEL_SRC}" ]]; then
  mkdir -p "${MODEL_DIR}"
  cp -f "${FP_MODEL_SRC}" "${MODEL_DIR}/"
  echo "Copied FP RKNN model:"
  echo "${MODEL_DIR}/yolov8n_coco_fp.rknn"
fi

if [[ -d "${TEST_IMAGE_SRC}" ]]; then
  rm -rf "${TEST_IMAGE_DST}"
  mkdir -p "${TEST_IMAGE_DST}"
  cp -f "${TEST_IMAGE_SRC}"/* "${TEST_IMAGE_DST}/" 2>/dev/null || true
  echo "Copied test images:"
  echo "${TEST_IMAGE_DST}"
fi

if [[ -d "${TEST_FRAME_SRC}" ]]; then
  rm -rf "${TEST_FRAME_DST}"
  mkdir -p "${TEST_FRAME_DST}"
  cp -rf "${TEST_FRAME_SRC}"/* "${TEST_FRAME_DST}/" 2>/dev/null || true
  echo "Copied test frames:"
  echo "${TEST_FRAME_DST}"
fi

if [[ -d "${TEST_VIDEO_SRC}" ]]; then
  rm -rf "${TEST_VIDEO_DST}"
  mkdir -p "${TEST_VIDEO_DST}"
  cp -f "${TEST_VIDEO_SRC}"/* "${TEST_VIDEO_DST}/" 2>/dev/null || true
  echo "Copied test videos:"
  echo "${TEST_VIDEO_DST}"
fi

if [[ ! -x "${INSTALL_DIR}/yolov8_traffic_benchmark/yolov8_traffic_benchmark" ]]; then
  echo "The benchmark executable is missing from install directory."
  exit 1
fi

if [[ ! -x "${INSTALL_DIR}/yolov8_traffic_benchmark/yolov8_person_light_video" ]]; then
  echo "The person/light video executable is missing from install directory."
  exit 1
fi

if [[ ! -x "${INSTALL_DIR}/yolov8_traffic_benchmark/yolov8_redlight_violation_frame" ]]; then
  echo "The red-light violation frame executable is missing from install directory."
  exit 1
fi

echo "Build done:"
echo "${INSTALL_DIR}/yolov8_traffic_benchmark"
