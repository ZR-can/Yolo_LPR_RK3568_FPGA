cmake_minimum_required(VERSION 3.10)

project(its_traffic_benchmark)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (NOT DEFINED YOLO_LPR_DEMO_ROOT)
    if (DEFINED ENV{YOLO_LPR_DEMO_ROOT})
        set(YOLO_LPR_DEMO_ROOT "$ENV{YOLO_LPR_DEMO_ROOT}")
    else()
        get_filename_component(_ITS_RKNN_DIR "${CMAKE_CURRENT_SOURCE_DIR}" ABSOLUTE)
        get_filename_component(_ITS_DIR "${_ITS_RKNN_DIR}/.." ABSOLUTE)
        get_filename_component(_ELSE_DIR "${_ITS_DIR}/.." ABSOLUTE)
        set(YOLO_LPR_DEMO_ROOT "${_ELSE_DIR}/Yolo_LPR_RK3568_FPGA/3_NPU_Yolov8_LPR_Demo")
    endif()
endif()

get_filename_component(YOLO_LPR_DEMO_ROOT "${YOLO_LPR_DEMO_ROOT}" ABSOLUTE)

if (NOT EXISTS "${YOLO_LPR_DEMO_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR
        "YOLO_LPR_DEMO_ROOT is invalid: ${YOLO_LPR_DEMO_ROOT}\n"
        "Set it with -DYOLO_LPR_DEMO_ROOT=/path/to/3_NPU_Yolov8_LPR_Demo "
        "or export YOLO_LPR_DEMO_ROOT=/path/to/3_NPU_Yolov8_LPR_Demo")
endif()

message(STATUS "YOLO_LPR_DEMO_ROOT=${YOLO_LPR_DEMO_ROOT}")

if (NOT TARGET_SOC)
    set(TARGET_SOC rk356x)
endif()

if (TARGET_SOC STREQUAL "rk3568" OR TARGET_SOC STREQUAL "rk3566" OR TARGET_SOC STREQUAL "rk3562")
    set(TARGET_SOC rk356x)
endif()

add_subdirectory("${YOLO_LPR_DEMO_ROOT}/3rdparty" "${CMAKE_BINARY_DIR}/yolo_lpr_3rdparty.out")
add_subdirectory("${YOLO_LPR_DEMO_ROOT}/utils" "${CMAKE_BINARY_DIR}/yolo_lpr_utils.out")

if (CMAKE_SYSTEM_NAME STREQUAL "Android")
    set(OpenCV_DIR "${YOLO_LPR_DEMO_ROOT}/3rdparty/opencv/opencv-android-sdk-build/sdk/native/jni/abi-${CMAKE_ANDROID_ARCH_ABI}")
else()
    if (CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(OpenCV_DIR "${YOLO_LPR_DEMO_ROOT}/3rdparty/opencv/opencv-linux-aarch64/share/OpenCV")
    else()
        set(OpenCV_DIR "${YOLO_LPR_DEMO_ROOT}/3rdparty/opencv/opencv-linux-armhf/share/OpenCV")
    endif()
endif()
find_package(OpenCV REQUIRED)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

set(TARGET_TRAFFIC_BENCH yolov8_traffic_benchmark)
set(TARGET_PERSON_LIGHT_VIDEO yolov8_person_light_video)
set(TARGET_REDLIGHT_VIOLATION_FRAME yolov8_redlight_violation_frame)

add_executable(${TARGET_TRAFFIC_BENCH}
    traffic_benchmark_main.cc
    traffic_scene.cc
    yolov8_fp_fallback.cc
    "${YOLO_LPR_DEMO_ROOT}/src/postprocess.cc"
    "${YOLO_LPR_DEMO_ROOT}/src/yolov8_zero_copy.cc"
)

target_compile_definitions(${TARGET_TRAFFIC_BENCH} PRIVATE ZERO_COPY ITS_TRAFFIC_CLASS_ONLY)

target_include_directories(${TARGET_TRAFFIC_BENCH} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    "${YOLO_LPR_DEMO_ROOT}"
    "${YOLO_LPR_DEMO_ROOT}/include"
    "${YOLO_LPR_DEMO_ROOT}/utils"
    "${YOLO_LPR_DEMO_ROOT}/3rdparty/allocator/drm"
    ${LIBRKNNRT_INCLUDES}
    ${LIBRGA_INCLUDES}
)

target_link_libraries(${TARGET_TRAFFIC_BENCH}
    imageutils
    fileutils
    imagedrawing
    ${LIBRKNNRT}
    ${OpenCV_LIBS}
    dl
    Threads::Threads
)

install(TARGETS ${TARGET_TRAFFIC_BENCH} DESTINATION ${TARGET_TRAFFIC_BENCH})
install(FILES coco_80_labels_list.txt DESTINATION ${TARGET_TRAFFIC_BENCH}/model RENAME labels_list.txt)
install(FILES traffic_roi.json DESTINATION ${TARGET_TRAFFIC_BENCH}/model)

add_executable(${TARGET_PERSON_LIGHT_VIDEO}
    person_light_video_main.cc
    yolov8_fp_fallback.cc
    "${YOLO_LPR_DEMO_ROOT}/src/postprocess.cc"
    "${YOLO_LPR_DEMO_ROOT}/src/yolov8_zero_copy.cc"
)

target_compile_definitions(${TARGET_PERSON_LIGHT_VIDEO} PRIVATE ZERO_COPY ITS_TRAFFIC_CLASS_ONLY)

target_include_directories(${TARGET_PERSON_LIGHT_VIDEO} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    "${YOLO_LPR_DEMO_ROOT}"
    "${YOLO_LPR_DEMO_ROOT}/include"
    "${YOLO_LPR_DEMO_ROOT}/utils"
    "${YOLO_LPR_DEMO_ROOT}/3rdparty/allocator/drm"
    ${LIBRKNNRT_INCLUDES}
    ${LIBRGA_INCLUDES}
)

target_link_libraries(${TARGET_PERSON_LIGHT_VIDEO}
    imageutils
    fileutils
    imagedrawing
    ${LIBRKNNRT}
    ${OpenCV_LIBS}
    dl
    Threads::Threads
)

install(TARGETS ${TARGET_PERSON_LIGHT_VIDEO} DESTINATION ${TARGET_TRAFFIC_BENCH})

add_executable(${TARGET_REDLIGHT_VIOLATION_FRAME}
    redlight_violation_frame_main.cc
    yolov8_fp_fallback.cc
    "${YOLO_LPR_DEMO_ROOT}/src/postprocess.cc"
    "${YOLO_LPR_DEMO_ROOT}/src/yolov8_zero_copy.cc"
)

target_compile_definitions(${TARGET_REDLIGHT_VIOLATION_FRAME} PRIVATE ZERO_COPY ITS_TRAFFIC_CLASS_ONLY)

target_include_directories(${TARGET_REDLIGHT_VIOLATION_FRAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    "${YOLO_LPR_DEMO_ROOT}"
    "${YOLO_LPR_DEMO_ROOT}/include"
    "${YOLO_LPR_DEMO_ROOT}/utils"
    "${YOLO_LPR_DEMO_ROOT}/3rdparty/allocator/drm"
    ${LIBRKNNRT_INCLUDES}
    ${LIBRGA_INCLUDES}
)

target_link_libraries(${TARGET_REDLIGHT_VIOLATION_FRAME}
    imageutils
    fileutils
    imagedrawing
    ${LIBRKNNRT}
    ${OpenCV_LIBS}
    dl
    Threads::Threads
)

install(TARGETS ${TARGET_REDLIGHT_VIOLATION_FRAME} DESTINATION ${TARGET_TRAFFIC_BENCH})
