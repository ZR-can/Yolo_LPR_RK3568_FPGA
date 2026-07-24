cmake_minimum_required(VERSION 3.15)

project(yolov8_ppocr_pcie_qt_ui LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_INSTALL_RPATH "$ORIGIN/../lib")

set(PPOCR_DEMO_ROOT
    "${CMAKE_CURRENT_LIST_DIR}/../3_NPU_Yolov8_PPOCR_Demo"
    CACHE PATH "Path to the stage-3 YOLOv8 + PP-OCR backend")
set(TRAFFIC_DEMO_ROOT
    "${CMAKE_CURRENT_LIST_DIR}/../4_NPU_Yolov8_Traffic_Demo"
    CACHE PATH "Path to the stage-4 YOLOv8 traffic-violation backend")

if (NOT EXISTS "${PPOCR_DEMO_ROOT}/src/main_ppocr.cc")
    message(FATAL_ERROR "Stage-3 PP-OCR backend not found: ${PPOCR_DEMO_ROOT}")
endif()
if (NOT EXISTS "${TRAFFIC_DEMO_ROOT}/src/main_pcie_traffic.cc")
    message(FATAL_ERROR "Stage-4 traffic backend not found: ${TRAFFIC_DEMO_ROOT}")
endif()

add_subdirectory("${PPOCR_DEMO_ROOT}/3rdparty" 3rdparty.out)
add_subdirectory("${PPOCR_DEMO_ROOT}/utils" utils.out)

if (CMAKE_SYSTEM_NAME STREQUAL "Android")
    set(OpenCV_DIR
        "${PPOCR_DEMO_ROOT}/3rdparty/opencv/opencv-android-sdk-build/sdk/native/jni/abi-${CMAKE_ANDROID_ARCH_ABI}")
elseif (CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(OpenCV_DIR
        "${PPOCR_DEMO_ROOT}/3rdparty/opencv/opencv-linux-aarch64/share/OpenCV")
else()
    set(OpenCV_DIR
        "${PPOCR_DEMO_ROOT}/3rdparty/opencv/opencv-linux-armhf/share/OpenCV")
endif()

find_package(OpenCV REQUIRED)
find_package(Qt5 REQUIRED COMPONENTS Widgets)
find_package(Threads REQUIRED)

set(PPOCR_BACKEND_SRCS
    "${PPOCR_DEMO_ROOT}/src/main_ppocr.cc"
    "${PPOCR_DEMO_ROOT}/include/pcie_demo_bridge.h"
    "${PPOCR_DEMO_ROOT}/src/pcie_frame_source.cc"
    "${PPOCR_DEMO_ROOT}/src/postprocess.cc"
    "${PPOCR_DEMO_ROOT}/src/yolov8_zero_copy.cc"
    "${PPOCR_DEMO_ROOT}/src/ppocr_rec.cc"
    "${PPOCR_DEMO_ROOT}/src/ppocr_retry_policy.cc"
    "${PPOCR_DEMO_ROOT}/src/yolo_ppocr_pipeline.cc"
    "${PPOCR_DEMO_ROOT}/src/simple_tracker.cc"
    "${PPOCR_DEMO_ROOT}/src/plate_rule.cc"
    "${PPOCR_DEMO_ROOT}/src/drm_display.cc"
    "${PPOCR_DEMO_ROOT}/src/rga_overlay_renderer.cc"
)

set(QT_UI_SRCS
    src/main_pcie_qt.cc
    src/pcie_qt_ui_helpers.cc
    include/pcie_qt_ui_helpers.h
    src/mainwindow.ui
)

set(TRAFFIC_BACKEND_SRCS
    "${TRAFFIC_DEMO_ROOT}/src/main_pcie_traffic.cc"
    "${TRAFFIC_DEMO_ROOT}/include/traffic_pcie_bridge.h"
    "${TRAFFIC_DEMO_ROOT}/src/traffic_violation.cc"
    "${TRAFFIC_DEMO_ROOT}/src/traffic_temporal_tracker.cc"
    "${TRAFFIC_DEMO_ROOT}/src/traffic_overlay_renderer.cc"
    "${TRAFFIC_DEMO_ROOT}/src/postprocess.cc"
    "${TRAFFIC_DEMO_ROOT}/src/yolov8_zero_copy.cc"
)

add_library(traffic_qt_backend STATIC ${TRAFFIC_BACKEND_SRCS})
target_compile_definitions(traffic_qt_backend PRIVATE
    ZERO_COPY
    TRAFFIC_QT_UI_BUILD
)
target_include_directories(traffic_qt_backend PRIVATE
    "${TRAFFIC_DEMO_ROOT}/include"
    "${TRAFFIC_DEMO_ROOT}/utils"
    "${PPOCR_DEMO_ROOT}/include"
    "${PPOCR_DEMO_ROOT}/utils"
    "${PPOCR_DEMO_ROOT}/3rdparty/allocator/drm"
    ${LIBRKNNRT_INCLUDES}
    ${LIBRGA_INCLUDES}
)

set(TARGET_NAME yolov8_ppocr_pcie_qt_ui)
add_executable(${TARGET_NAME} ${QT_UI_SRCS} ${PPOCR_BACKEND_SRCS})
set_target_properties(${TARGET_NAME} PROPERTIES
    AUTOMOC ON
    AUTOUIC ON
    AUTOUIC_SEARCH_PATHS "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_compile_definitions(${TARGET_NAME} PRIVATE ZERO_COPY PCIE_QT_UI_BUILD)
target_include_directories(${TARGET_NAME} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${PPOCR_DEMO_ROOT}"
    "${PPOCR_DEMO_ROOT}/src"
    "${PPOCR_DEMO_ROOT}/include"
    "${PPOCR_DEMO_ROOT}/utils"
    "${TRAFFIC_DEMO_ROOT}/include"
    "${PPOCR_DEMO_ROOT}/3rdparty/allocator/drm"
    ${LIBRKNNRT_INCLUDES}
    ${LIBRGA_INCLUDES}
)
target_link_libraries(${TARGET_NAME}
    Qt5::Widgets
    traffic_qt_backend
    imageutils
    fileutils
    imagedrawing
    ${LIBRKNNRT}
    ${OpenCV_LIBS}
    m
    dl
    Threads::Threads
)

set(PCIE_DRIVER_MODULE "${PPOCR_DEMO_ROOT}/pango_pci_driver.ko")
set(YOLO_MODEL_FILE "${PPOCR_DEMO_ROOT}/model/yolov8.rknn")
set(LABEL_FILE "${PPOCR_DEMO_ROOT}/model/labels_list.txt")
set(PPOCR_MODEL_FILE
    "${PPOCR_DEMO_ROOT}/model/ppocrv4_rec14_fold_affine_1x1_rk3568_hybrid_mmse_h2_add27_hsw4.rknn")
set(PPOCR_IMAGE_MODEL_FILE
    "${PPOCR_DEMO_ROOT}/model/ppocrv4_rec14_fold_affine_1x1_rk3568_fp16.rknn")
set(PPOCR_DICT_FILE "${PPOCR_DEMO_ROOT}/model/cblprd_plate_dict.txt")
set(TRAFFIC_MODEL_FILE "${TRAFFIC_DEMO_ROOT}/model/yolov8_traffic_i8.rknn")
set(TRAFFIC_LABEL_FILE "${TRAFFIC_DEMO_ROOT}/model/labels_list.txt")
set(CHINESE_FONT_FILE
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/simhei.ttf")

foreach(REQUIRED_FILE
        "${PCIE_DRIVER_MODULE}"
        "${YOLO_MODEL_FILE}"
        "${LABEL_FILE}"
        "${PPOCR_MODEL_FILE}"
        "${PPOCR_IMAGE_MODEL_FILE}"
        "${PPOCR_DICT_FILE}"
        "${TRAFFIC_MODEL_FILE}"
        "${TRAFFIC_LABEL_FILE}"
        "${CHINESE_FONT_FILE}")
    if (NOT EXISTS "${REQUIRED_FILE}")
        message(FATAL_ERROR "Required deployment file is missing: ${REQUIRED_FILE}")
    endif()
endforeach()

install(TARGETS ${TARGET_NAME} DESTINATION ${TARGET_NAME})
install(FILES
    "${YOLO_MODEL_FILE}"
    "${LABEL_FILE}"
    "${PPOCR_MODEL_FILE}"
    "${PPOCR_IMAGE_MODEL_FILE}"
    "${PPOCR_DICT_FILE}"
    DESTINATION ${TARGET_NAME}/model
)
install(FILES "${CHINESE_FONT_FILE}"
        DESTINATION ${TARGET_NAME}/assets/fonts)
install(FILES
    "${TRAFFIC_MODEL_FILE}"
    "${TRAFFIC_LABEL_FILE}"
    DESTINATION ${TARGET_NAME}/model/traffic
)
install(FILES "${PCIE_DRIVER_MODULE}" DESTINATION ${TARGET_NAME})
